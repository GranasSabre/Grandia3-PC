// Audio output backend: SPSC ring buffer + WASAPI shared-mode event-driven render thread.
//
// The emulation thread produces exactly 48000 frames per emulated second (emulated time tracks host
// time), the device consumes at its own clock. The consumer keeps the ring fill level around a target
// latency with a slow, small (+-0.5%) resampling-ratio correction; large excursions (stalls, fast
// forward) are handled by dropping (overfull) or waiting for a refill (underrun).
#include "audio.h"
#include "platform.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <atomic>
#include <cmath>

namespace audio {

static constexpr u32 SRC_RATE = 48000;
static constexpr u32 RING_FRAMES = 1u << 15;  // 682 ms
static constexpr u32 RING_MASK = RING_FRAMES - 1;
static constexpr u32 TARGET_FRAMES = SRC_RATE * 50 / 1000;  // 50 ms in the ring (+ device buffer)
static constexpr u32 MAX_FRAMES = TARGET_FRAMES * 3;         // above this: drop back to the target

static s16 g_ring[RING_FRAMES * 2];
static std::atomic<u32> g_w{0}, g_r{0};
static std::atomic<u32> g_dropped{0};

// ---------------------------------------------------------------------------
// WAV dump (producer side)
// ---------------------------------------------------------------------------
static std::string g_wav_path;
static FILE* g_wav = nullptr;
static u64 g_wav_frames = 0, g_wav_hdr_frames = 0;

static void wav_header(FILE* f, u64 frames) {
    u32 data = (u32)std::min<u64>(frames * 4, 0xFFFFFFF0ull);
    u8 h[44];
    auto w32 = [&](int o, u32 v) { memcpy(h + o, &v, 4); };
    auto w16 = [&](int o, u16 v) { memcpy(h + o, &v, 2); };
    memcpy(h, "RIFF", 4);
    w32(4, 36 + data);
    memcpy(h + 8, "WAVEfmt ", 8);
    w32(16, 16);
    w16(20, 1);
    w16(22, 2);
    w32(24, SRC_RATE);
    w32(28, SRC_RATE * 4);
    w16(32, 4);
    w16(34, 16);
    memcpy(h + 36, "data", 4);
    w32(40, data);
    fseek(f, 0, SEEK_SET);
    fwrite(h, 1, 44, f);
    fseek(f, 0, SEEK_END);
}

void set_wav_path(const std::string& path) { g_wav_path = path; }

// ---------------------------------------------------------------------------
// Producer
// ---------------------------------------------------------------------------
void push(const s16* frames, int n) {
    if (!g_wav_path.empty() && !g_wav) {
        g_wav = fopen(g_wav_path.c_str(), "wb");
        if (g_wav) wav_header(g_wav, 0);
        else {
            log_write(LOG_SPU, "audio: cannot open WAV output '%s'", g_wav_path.c_str());
            g_wav_path.clear();
        }
    }
    if (g_wav) {
        fwrite(frames, 4, (size_t)n, g_wav);
        g_wav_frames += (u64)n;
        if (g_wav_frames - g_wav_hdr_frames >= SRC_RATE / 2) {  // keep the header valid if we get killed
            wav_header(g_wav, g_wav_frames);
            fflush(g_wav);
            g_wav_hdr_frames = g_wav_frames;
        }
    }
    u32 w = g_w.load(std::memory_order_relaxed);
    u32 r = g_r.load(std::memory_order_acquire);
    u32 space = RING_FRAMES - (w - r);
    if ((u32)n > space) {
        g_dropped.fetch_add((u32)n - space, std::memory_order_relaxed);
        n = (int)space;
    }
    for (int i = 0; i < n; i++) {
        u32 idx = (w + (u32)i) & RING_MASK;
        g_ring[idx * 2] = frames[i * 2];
        g_ring[idx * 2 + 1] = frames[i * 2 + 1];
    }
    g_w.store(w + (u32)n, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Consumer (device thread)
// ---------------------------------------------------------------------------
struct Consumer {
    bool playing = false;
    double frac = 0;
    double fill_avg = TARGET_FRAMES;
    float last_l = 0, last_r = 0;
    u32 underruns = 0, resyncs = 0;
};
static Consumer g_cons;

// Produce n output frames (stereo float) at device rate dev_rate.
static void consume(float* out, u32 n, u32 dev_rate, float volume) {
    Consumer& c = g_cons;
    u32 r = g_r.load(std::memory_order_relaxed);
    u32 w = g_w.load(std::memory_order_acquire);
    u32 avail = w - r;
    if (!c.playing) {
        if (avail < TARGET_FRAMES) {
            memset(out, 0, n * 2 * sizeof(float));
            return;
        }
        c.playing = true;
        c.fill_avg = avail;
        c.frac = 0;
    }
    if (avail > MAX_FRAMES) {  // too much latency (stall catch-up or fast forward): skip ahead
        r += avail - TARGET_FRAMES;
        avail = TARGET_FRAMES;
        c.fill_avg = avail;
        c.resyncs++;
    }
    c.fill_avg += (avail - c.fill_avg) * 0.02;
    double err = (c.fill_avg - TARGET_FRAMES) / TARGET_FRAMES;
    double adj = std::max(-0.005, std::min(0.005, err * 0.01));
    double ratio = (double)SRC_RATE / dev_rate * (1.0 + adj);
    const float scale = volume / 32768.0f;
    u32 i = 0;
    for (; i < n; i++) {
        if (avail < 2) break;
        u32 a = r & RING_MASK, b = (r + 1) & RING_MASK;
        float t = (float)c.frac;
        float l = g_ring[a * 2] + (g_ring[b * 2] - g_ring[a * 2]) * t;
        float rr = g_ring[a * 2 + 1] + (g_ring[b * 2 + 1] - g_ring[a * 2 + 1]) * t;
        out[i * 2] = l * scale;
        out[i * 2 + 1] = rr * scale;
        c.frac += ratio;
        while (c.frac >= 1.0) {
            c.frac -= 1.0;
            r++;
            avail--;
        }
    }
    if (i < n) {
        // underrun: fade out quickly from the last sample, then wait until the ring is refilled
        float l = i ? out[(i - 1) * 2] : c.last_l, rr = i ? out[(i - 1) * 2 + 1] : c.last_r;
        for (; i < n; i++) {
            l *= 0.95f;
            rr *= 0.95f;
            out[i * 2] = l;
            out[i * 2 + 1] = rr;
        }
        c.playing = false;
        c.underruns++;
    }
    c.last_l = out[(n - 1) * 2];
    c.last_r = out[(n - 1) * 2 + 1];
    g_r.store(r, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// WASAPI
// ---------------------------------------------------------------------------
static const GUID kCLSID_MMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID kIID_IMMDeviceEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID kIID_IAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
static const GUID kIID_IAudioRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
static const GUID kSUBTYPE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID kSUBTYPE_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

static const GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID kIID_IMMNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};

// Follows changes of the Windows default output device (reopen on the new default).
static std::atomic<bool> g_default_changed{false};
class DeviceNotify : public IMMNotificationClient {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, kIID_IUnknown) || IsEqualIID(riid, kIID_IMMNotificationClient)) {
            *ppv = this;
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) g_default_changed = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
};
static DeviceNotify g_notify;

template <typename T>
static void release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

enum SampleFmt { FMT_F32, FMT_S16, FMT_S32 };

struct Device {
    IMMDeviceEnumerator* enumr = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* render = nullptr;
    HANDLE event = nullptr;
    u32 rate = 0, channels = 0, buffer_frames = 0;
    SampleFmt fmt = FMT_F32;

    bool notify_registered = false;

    void close() {
        if (client) client->Stop();
        if (notify_registered && enumr) enumr->UnregisterEndpointNotificationCallback(&g_notify);
        notify_registered = false;
        release(render);
        release(client);
        release(dev);
        release(enumr);
        if (event) CloseHandle(event);
        event = nullptr;
    }

    bool open(bool log_errors) {
        HRESULT hr = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, kIID_IMMDeviceEnumerator, (void**)&enumr);
        if (FAILED(hr)) {
            if (log_errors) log_write(LOG_SPU, "audio: MMDeviceEnumerator failed (%08lx)", (unsigned long)hr);
            return false;
        }
        g_default_changed = false;
        notify_registered = SUCCEEDED(enumr->RegisterEndpointNotificationCallback(&g_notify));
        hr = enumr->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (FAILED(hr)) {
            if (log_errors) log_write(LOG_SPU, "audio: no default output device (%08lx)", (unsigned long)hr);
            return false;
        }
        hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&client);
        if (FAILED(hr)) {
            if (log_errors) log_write(LOG_SPU, "audio: IAudioClient activation failed (%08lx)", (unsigned long)hr);
            return false;
        }
        const REFERENCE_TIME dur = 300000;  // 30 ms device buffer
        // Preferred: 48 kHz float stereo, let the audio engine convert if the mix format differs.
        WAVEFORMATEXTENSIBLE wf = {};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = 2;
        wf.Format.nSamplesPerSec = SRC_RATE;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = 8;
        wf.Format.nAvgBytesPerSec = SRC_RATE * 8;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        wf.SubFormat = kSUBTYPE_FLOAT;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                dur, 0, (WAVEFORMATEX*)&wf, nullptr);
        if (SUCCEEDED(hr)) {
            rate = SRC_RATE;
            channels = 2;
            fmt = FMT_F32;
        } else {
            // Fallback: the engine's mix format.
            release(client);
            hr = dev->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void**)&client);
            if (FAILED(hr)) return false;
            WAVEFORMATEX* mix = nullptr;
            hr = client->GetMixFormat(&mix);
            if (FAILED(hr) || !mix) return false;
            rate = mix->nSamplesPerSec;
            channels = mix->nChannels;
            bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            bool is_pcm = mix->wFormatTag == WAVE_FORMAT_PCM;
            if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)mix;
                is_float = memcmp(&x->SubFormat, &kSUBTYPE_FLOAT, sizeof(GUID)) == 0;
                is_pcm = memcmp(&x->SubFormat, &kSUBTYPE_PCM, sizeof(GUID)) == 0;
            }
            bool ok = true;
            if (is_float && mix->wBitsPerSample == 32) fmt = FMT_F32;
            else if (is_pcm && mix->wBitsPerSample == 16) fmt = FMT_S16;
            else if (is_pcm && mix->wBitsPerSample == 32) fmt = FMT_S32;
            else ok = false;
            if (ok && channels >= 1)
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, mix, nullptr);
            CoTaskMemFree(mix);
            if (!ok || channels < 1 || FAILED(hr)) {
                if (log_errors) log_write(LOG_SPU, "audio: IAudioClient::Initialize failed (%08lx)", (unsigned long)hr);
                return false;
            }
        }
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(client->SetEventHandle(event))) return false;
        UINT32 bf = 0;
        if (FAILED(client->GetBufferSize(&bf))) return false;
        buffer_frames = bf;
        if (FAILED(client->GetService(kIID_IAudioRenderClient, (void**)&render))) return false;
        BYTE* data = nullptr;
        if (SUCCEEDED(render->GetBuffer(bf, &data))) render->ReleaseBuffer(bf, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(client->Start())) return false;
        log_write(LOG_MAIN, "audio: WASAPI output %u Hz, %u ch, %s, buffer %u frames", rate, channels,
                  fmt == FMT_F32 ? "float32" : (fmt == FMT_S16 ? "s16" : "s32"), buffer_frames);
        return true;
    }

    // Returns false when the device is lost.
    bool pump(std::vector<float>& tmp, float volume) {
        UINT32 pad = 0;
        HRESULT hr = client->GetCurrentPadding(&pad);
        if (FAILED(hr)) return false;
        UINT32 n = buffer_frames - pad;
        if (n == 0) return true;
        BYTE* data = nullptr;
        hr = render->GetBuffer(n, &data);
        if (FAILED(hr)) return false;
        tmp.resize(n * 2);
        consume(tmp.data(), n, rate, volume);
        for (UINT32 i = 0; i < n; i++) {
            float l = tmp[i * 2], r = tmp[i * 2 + 1];
            l = l < -1.f ? -1.f : (l > 1.f ? 1.f : l);
            r = r < -1.f ? -1.f : (r > 1.f ? 1.f : r);
            for (u32 ch = 0; ch < channels; ch++) {
                float v = channels == 1 ? (l + r) * 0.5f : (ch == 0 ? l : (ch == 1 ? r : 0.f));
                size_t k = (size_t)i * channels + ch;
                if (fmt == FMT_F32) ((float*)data)[k] = v;
                else if (fmt == FMT_S16) ((s16*)data)[k] = (s16)lrintf(v * 32767.f);
                else ((s32*)data)[k] = (s32)lrint((double)v * 2147483647.0);
            }
        }
        render->ReleaseBuffer(n, 0);
        return true;
    }
};

static DWORD WINAPI audio_thread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task_idx = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);
    (void)task;
    std::vector<float> tmp;
    bool first = true;
    double stat_t = host_time_sec();
    for (;;) {
        Device d;
        if (!d.open(first)) {
            if (first) log_write(LOG_MAIN, "audio: no output device available, running silently (will retry)");
            first = false;
            d.close();
            Sleep(2000);
            continue;
        }
        first = false;
        g_cons.playing = false;
        for (;;) {
            WaitForSingleObject(d.event, 100);
            int vol = platform::config().volume;
            vol = vol < 0 ? 0 : (vol > 100 ? 100 : vol);
            if (!d.pump(tmp, vol / 100.0f)) break;
            if (g_default_changed) {
                log_write(LOG_MAIN, "audio: default output device changed");
                break;
            }
            double now = host_time_sec();
            if (now - stat_t >= 10.0) {
                stat_t = now;
                LOG(LOG_SPU, "audio: fill %.1f ms, underruns %u, resyncs %u, dropped %u", g_cons.fill_avg * 1000.0 / SRC_RATE,
                    g_cons.underruns, g_cons.resyncs, g_dropped.load());
            }
        }
        if (!g_default_changed) log_write(LOG_MAIN, "audio: device lost, reopening");
        d.close();
        Sleep(g_default_changed ? 50 : 200);
    }
    return 0;
}

void init() {
    if (!platform::config().audio_enabled) {
        log_write(LOG_MAIN, "audio: output disabled in config");
        return;
    }
    CreateThread(nullptr, 256 * 1024, audio_thread, nullptr, 0, nullptr);
}

}  // namespace audio
