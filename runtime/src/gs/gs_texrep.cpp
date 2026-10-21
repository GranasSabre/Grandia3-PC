// HD texture packs. The GPU renderer computes a key for every texture it decodes from texel data
// and the palette entries it uses (gs_gpu.cpp). Dump mode writes each new texture as
// texdump\<key>.png; a file textures\<key>*.png (an integer multiple of the original size) is
// sampled instead of the original. PNG files are read and written with WIC on a worker thread.
// lang\de_tex\<key>*.png (translated labels) works the same way, wins over textures\ and stays
// active when the HD textures are switched off.
#include "gs_texrep.h"
#include "../platform/platform.h"
#include <windows.h>
#include <wincodec.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace gs {
namespace texrep {

static std::string g_rep_dir, g_dump_dir;
static bool g_dump = false;
static std::unordered_map<u64, std::wstring> g_files;  // replacement key -> file (read-only after init)

struct Job {
    bool load;
    u64 key;
    int w, h;
    std::vector<u32> px;
    std::string info;
    std::vector<u8> raw;
};
static std::mutex g_mu;
static std::condition_variable g_cv;
static std::deque<Job> g_jobs;
static std::unordered_map<u64, Image> g_ready;   // loaded replacements
static std::unordered_set<u64> g_requested;      // loads queued, running or finished
static std::unordered_set<u64> g_dumped;
static std::unordered_set<u64> g_lang;           // keys replaced from lang\de_tex

u64 hash(const void* data, size_t bytes, u64 seed) {
    const u8* p = (const u8*)data;
    u64 h = seed ^ (bytes * 0x9E3779B97F4A7C15ull);
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        u64 w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < bytes; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static bool parse_key(const wchar_t* name, u64& key) {
    u64 k = 0;
    for (int i = 0; i < 16; i++) {
        wchar_t c = name[i];
        int d = (c >= L'0' && c <= L'9') ? c - L'0' : (c >= L'a' && c <= L'f') ? c - L'a' + 10 : (c >= L'A' && c <= L'F') ? c - L'A' + 10 : -1;
        if (d < 0) return false;
        k = k << 4 | (u64)d;
    }
    key = k;
    return true;
}

static bool is_png(const wchar_t* name) {
    size_t n = wcslen(name);
    return n > 4 && _wcsicmp(name + n - 4, L".png") == 0;
}

static void scan(const std::wstring& dir, int depth, bool replacements) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        std::wstring path = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 4) scan(path, depth + 1, replacements);
            continue;
        }
        u64 key;
        if (!is_png(fd.cFileName) || !parse_key(fd.cFileName, key)) continue;
        if (replacements) g_files.emplace(key, path);
        else g_dumped.insert(key);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// alpha: GS 0..128 <-> PNG 0..255
static inline u32 a_to_png(u32 a) { return a >= 128 ? 255 : (a * 255 + 64) / 128; }
static inline u32 a_from_png(u32 a) { return (a * 128 + 127) / 255; }

static thread_local IWICImagingFactory* g_wic = nullptr;  // per worker thread
static std::mutex g_index_mu;

static bool load_png(const std::wstring& path, Image& img) {
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(g_wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0;
        conv->GetSize(&w, &h);
        if (w > 0 && h > 0 && w <= 8192 && h <= 8192) {
            img.w = (int)w;
            img.h = (int)h;
            img.px.resize((size_t)w * h);
            if (SUCCEEDED(conv->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)img.px.data()))) {
                for (u32& p : img.px) p = (p & 0xFFFFFF) | a_from_png(p >> 24) << 24;
                ok = true;
            }
        }
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    return ok;
}

static bool save_png(const std::wstring& path, int w, int h, const std::vector<u32>& px) {
    std::vector<u32> bgra(px.size());
    for (size_t i = 0; i < px.size(); i++) {
        u32 p = px[i];
        bgra[i] = (p & 0xFF) << 16 | (p & 0xFF00) | ((p >> 16) & 0xFF) | a_to_png(p >> 24) << 24;
    }
    std::wstring tmp = path + L".tmp";
    IWICStream* st = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(tmp.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(g_wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) && SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h))) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        if (SUCCEEDED(fr->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA) &&
            SUCCEEDED(fr->WritePixels(h, w * 4, w * h * 4, (BYTE*)bgra.data())) && SUCCEEDED(fr->Commit()) && SUCCEEDED(enc->Commit()))
            ok = true;
    }
    if (fr) fr->Release();
    if (enc) enc->Release();
    if (st) st->Release();
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
    else DeleteFileW(tmp.c_str());
    return ok;
}

static void worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic)))) {
        log_write(LOG_GS, "texrep: WIC unavailable, no texture dumps or replacements");
        g_wic = nullptr;
    }
    static FILE* index = nullptr;
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(g_mu);
            g_cv.wait(lk, [] { return !g_jobs.empty(); });
            j = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        if (!g_wic) continue;
        if (j.load) {
            Image img;
            auto it = g_files.find(j.key);
            if (it == g_files.end() || !load_png(it->second, img)) {
                log_write(LOG_GS, "texrep: cannot read replacement %016llx", (unsigned long long)j.key);
                img.w = img.h = 0;
            }
            std::lock_guard<std::mutex> lk(g_mu);
            g_ready[j.key] = std::move(img);
        } else {
            char name[32];
            snprintf(name, sizeof name, "\\%016llx.png", (unsigned long long)j.key);
            if (!j.raw.empty()) {
                char bn[32];
                snprintf(bn, sizeof bn, "\\%016llx.bin", (unsigned long long)j.key);
                if (FILE* f = fopen((g_dump_dir + bn).c_str(), "wb")) {
                    fwrite(j.raw.data(), 1, j.raw.size(), f);
                    fclose(f);
                }
            }
            if (save_png(widen(g_dump_dir + name), j.w, j.h, j.px)) {
                std::lock_guard<std::mutex> lk(g_index_mu);
                if (!index) index = fopen((g_dump_dir + "\\index.txt").c_str(), "a");
                if (index) {
                    fprintf(index, "%016llx %dx%d %s\n", (unsigned long long)j.key, j.w, j.h, j.info.c_str());
                    fflush(index);
                }
            }
        }
    }
}

void init(bool dump) {
    g_rep_dir = exe_dir() + "\\textures";
    g_dump_dir = exe_dir() + "\\texdump";
    g_dump = dump;
    // translated labels (lang\<code>_tex) win over the HD textures; English keeps the originals
    const std::string lang = platform::config().language;
    if (lang != "en") scan(widen(exe_dir() + "\\lang\\" + lang + "_tex"), 0, true);
    for (auto& [k, f] : g_files) g_lang.insert(k);
    scan(widen(g_rep_dir), 0, true);  // emplace keeps the translated images
    if (dump) {
        CreateDirectoryW(widen(g_dump_dir).c_str(), nullptr);
        scan(widen(g_dump_dir), 0, false);
    }
    for (auto& [k, f] : g_files) g_dumped.insert(k);
    if (!g_files.empty() || dump) {
        // PNG decoding is the slow part of a scene change: a few threads
        int n = (int)std::thread::hardware_concurrency() / 2;
        n = n < 2 ? 2 : (n > 8 ? 8 : n);
        for (int i = 0; i < n; i++) std::thread(worker).detach();
    }
    log_write(LOG_MAIN, "HD textures: %zu replacement images (%zu translated)%s", g_files.size(), g_lang.size(),
              dump ? ", dumping new textures to texdump" : "");
}

bool have_pack() { return !g_files.empty(); }
bool translated(u64 key) { return g_lang.count(key) != 0; }
bool dumping() { return g_dump; }

State lookup(u64 key, const Image** img) {
    if (g_files.find(key) == g_files.end()) return NONE;
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ready.find(key);
    if (it != g_ready.end()) {
        if (it->second.px.empty()) return NONE;  // unreadable file
        *img = &it->second;
        return READY;
    }
    if (g_requested.insert(key).second) {
        g_jobs.push_back(Job{true, key, 0, 0, {}, {}, {}});
        g_cv.notify_one();
    }
    return LOADING;
}

void release(u64 key) {
    // forget a loaded image (unreadable files stay marked): a later lookup reads the file again
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_ready.find(key);
    if (it != g_ready.end() && it->second.w != 0) {
        g_ready.erase(it);
        g_requested.erase(key);
    }
}

bool dumped(u64 key) { return g_dumped.count(key) != 0; }

void dump(u64 key, int w, int h, std::vector<u32>&& px, const std::string& info, std::vector<u8>&& raw) {
    if (!g_dump || !g_dumped.insert(key).second) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_jobs.push_back(Job{false, key, w, h, std::move(px), info, std::move(raw)});
    g_cv.notify_one();
}

}  // namespace texrep
}  // namespace gs
