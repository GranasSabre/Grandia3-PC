// PlayStation controllers (DualShock 4, DualSense, DualSense Edge) read directly over HID, by USB
// cable or Bluetooth, when no XInput controller is connected (Steam Input or DS4Windows already turn
// them into XInput controllers; those stay in charge). Reports are converted to the XInput layout, so
// the rest of the port (button mapping, menus) treats them like any controller: Cross = A,
// Circle = B, Square = X, Triangle = Y, Share/Create and the touchpad click = Back, Options = Start,
// PS button = 0x0400 (opens the settings menu).
//
// Report formats as in SDL (SDL_hidapi_ps4.c / _ps5.c) and Linux (hid-playstation.c):
// - DualShock 4: input 0x01 (USB; Bluetooth before full mode) with the state at byte 1, 0x11..0x19
//   (Bluetooth) at byte 3; reading feature report 0x02 switches Bluetooth to full reports.
//   Rumble: output 0x05 (USB, flags at 1, motors at 4/5) or 0x11 (Bluetooth: 0xC0 at 1, flags at 3,
//   motors at 6/7, CRC32).
// - DualSense: input 0x01 (USB: full state at byte 1; Bluetooth: short state like the DualShock 4) or
//   0x31 (Bluetooth full state at byte 2); feature reports 0x09/0x20 switch Bluetooth to full reports.
//   Rumble: output 0x02 (USB, flags at 1, motors at 3/4) or 0x31 (Bluetooth: sequence at 1, 0x10 at 2,
//   flags at 3, motors at 5/6, CRC32).
// Bluetooth output reports end with a CRC32 over 0xA2 and the first 74 bytes.
#include "platform.h"
#include <windows.h>
#include <xinput.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace platform {

namespace {

// hid.dll and setupapi.dll are loaded on demand (no import: the program starts without them)
struct Api {
    decltype(&HidD_GetHidGuid) GetHidGuid;
    decltype(&HidD_GetAttributes) GetAttributes;
    decltype(&HidD_GetPreparsedData) GetPreparsedData;
    decltype(&HidD_FreePreparsedData) FreePreparsedData;
    decltype(&HidD_GetFeature) GetFeature;
    decltype(&HidP_GetCaps) GetCaps;
    decltype(&SetupDiGetClassDevsW) GetClassDevs;
    decltype(&SetupDiEnumDeviceInterfaces) EnumInterfaces;
    decltype(&SetupDiGetDeviceInterfaceDetailW) GetInterfaceDetail;
    decltype(&SetupDiDestroyDeviceInfoList) DestroyList;
} api;

bool load_api() {
    HMODULE hid = LoadLibraryW(L"hid.dll"), sa = LoadLibraryW(L"setupapi.dll");
    if (!hid || !sa) return false;
    auto get = [](HMODULE m, const char* n) { return (void*)GetProcAddress(m, n); };
    api.GetHidGuid = (decltype(api.GetHidGuid))get(hid, "HidD_GetHidGuid");
    api.GetAttributes = (decltype(api.GetAttributes))get(hid, "HidD_GetAttributes");
    api.GetPreparsedData = (decltype(api.GetPreparsedData))get(hid, "HidD_GetPreparsedData");
    api.FreePreparsedData = (decltype(api.FreePreparsedData))get(hid, "HidD_FreePreparsedData");
    api.GetFeature = (decltype(api.GetFeature))get(hid, "HidD_GetFeature");
    api.GetCaps = (decltype(api.GetCaps))get(hid, "HidP_GetCaps");
    api.GetClassDevs = (decltype(api.GetClassDevs))get(sa, "SetupDiGetClassDevsW");
    api.EnumInterfaces = (decltype(api.EnumInterfaces))get(sa, "SetupDiEnumDeviceInterfaces");
    api.GetInterfaceDetail = (decltype(api.GetInterfaceDetail))get(sa, "SetupDiGetDeviceInterfaceDetailW");
    api.DestroyList = (decltype(api.DestroyList))get(sa, "SetupDiDestroyDeviceInfoList");
    return api.GetHidGuid && api.GetAttributes && api.GetPreparsedData && api.FreePreparsedData && api.GetFeature &&
           api.GetCaps && api.GetClassDevs && api.EnumInterfaces && api.GetInterfaceDetail && api.DestroyList;
}

struct Pad {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::wstring path;
    bool dualsense = false, bt = false;
    USHORT in_len = 0, out_len = 0, feat_len = 0;
    u8 seq = 0;
};

std::atomic<bool> g_started{false}, g_connected{false};
std::mutex g_mu;
XINPUT_GAMEPAD g_state{};          // last report in XInput layout
std::atomic<u32> g_rumble{0};      // small | large << 8 (0..255 each)
HANDLE g_wake = nullptr;           // rumble changed

bool known(USHORT vid, USHORT pid, bool& dualsense) {
    if (vid != 0x054C) return false;
    dualsense = pid == 0x0CE6 || pid == 0x0DF2;                     // DualSense, DualSense Edge
    return dualsense || pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0;  // DualShock 4 v1, v2, USB adapter
}

// first PlayStation controller among the present HID devices
bool open_pad(Pad& p) {
    GUID guid;
    api.GetHidGuid(&guid);
    HDEVINFO set = api.GetClassDevs(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof(ifd);
    DWORD i = 0;
    for (; !found && api.EnumInterfaces(set, nullptr, &guid, i, &ifd); i++) {
        DWORD need = 0;
        api.GetInterfaceDetail(set, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<u8> mem(need);
        auto* det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)mem.data();
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!api.GetInterfaceDetail(set, &ifd, det, need, nullptr, nullptr)) continue;
        // attributes through a handle without access rights (works for every HID device)
        HANDLE q = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (q == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES at{};
        at.Size = sizeof(at);
        bool ds = false;
        bool match = api.GetAttributes(q, &at) && known(at.VendorID, at.ProductID, ds);
        HIDP_CAPS caps{};
        PHIDP_PREPARSED_DATA pp = nullptr;
        if (match && api.GetPreparsedData(q, &pp)) {
            match = api.GetCaps(pp, &caps) == HIDP_STATUS_SUCCESS && caps.InputReportByteLength >= 10;
            api.FreePreparsedData(pp);
        } else match = false;
        CloseHandle(q);
        if (!match) continue;
        HANDLE h = CreateFileW(det->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        p = Pad{};
        p.h = h;
        p.path = det->DevicePath;
        p.dualsense = ds;
        p.in_len = caps.InputReportByteLength;
        p.out_len = caps.OutputReportByteLength;
        p.feat_len = caps.FeatureReportByteLength;
        // Bluetooth: the larger output reports (USB: 32 / 48 bytes), or the Bluetooth HID service in the path
        std::wstring lp = p.path;
        for (auto& c : lp) c = (wchar_t)towlower(c);
        p.bt = p.out_len > 64 || lp.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos;
        found = true;
    }
    api.DestroyList(set);
    static bool logged = false;
    if (!found && !logged) {
        logged = true;
        log_write(LOG_MAIN, "controller: no XInput controller, no DualShock 4 / DualSense among %lu HID devices (searching on)", i);
    }
    return found;
}

// Bluetooth controllers send short reports until a feature report is read (separate synchronous handle)
void enable_full_reports(const Pad& p) {
    if (!p.bt || !p.feat_len) return;
    HANDLE h = CreateFileW(p.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<u8> buf(p.feat_len);
    static const u8 DS[2] = {0x09, 0x20}, DS4[2] = {0x02, 0x05};
    for (int k = 0; k < 2; k++) {
        std::fill(buf.begin(), buf.end(), 0);
        buf[0] = p.dualsense ? DS[k] : DS4[k];
        if (api.GetFeature(h, buf.data(), (ULONG)buf.size())) break;
    }
    CloseHandle(h);
}

u32 crc32(u32 crc, const u8* p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

void parse(const Pad& p, const u8* b, DWORD n) {
    const u8* d;
    bool full_ds;  // DualSense layout (triggers before the buttons)
    u8 id = b[0];
    if (!p.dualsense) {
        if (id == 0x01 && n >= 10) d = b + 1;
        else if (id >= 0x11 && id <= 0x19 && n >= 12) d = b + 3;
        else return;
        full_ds = false;
    } else if (id == 0x01 && n >= 11) {
        d = b + 1;
        full_ds = !p.bt;
    } else if (id == 0x31 && n >= 12) {
        d = b + 2;
        full_ds = true;
    } else return;
    u8 b0, b1, b2, l2, r2;
    if (full_ds) { l2 = d[4]; r2 = d[5]; b0 = d[7]; b1 = d[8]; b2 = d[9]; }
    else { b0 = d[4]; b1 = d[5]; b2 = d[6]; l2 = d[7]; r2 = d[8]; }
    static const WORD HAT[9] = {XINPUT_GAMEPAD_DPAD_UP,
                                XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT,
                                XINPUT_GAMEPAD_DPAD_RIGHT,
                                XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT,
                                XINPUT_GAMEPAD_DPAD_DOWN,
                                XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT,
                                XINPUT_GAMEPAD_DPAD_LEFT,
                                XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT,
                                0};
    XINPUT_GAMEPAD g{};
    WORD w = HAT[(b0 & 15) > 8 ? 8 : (b0 & 15)];
    if (b0 & 0x10) w |= XINPUT_GAMEPAD_X;  // square
    if (b0 & 0x20) w |= XINPUT_GAMEPAD_A;  // cross
    if (b0 & 0x40) w |= XINPUT_GAMEPAD_B;  // circle
    if (b0 & 0x80) w |= XINPUT_GAMEPAD_Y;  // triangle
    if (b1 & 0x01) w |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (b1 & 0x02) w |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (b1 & 0x10) w |= XINPUT_GAMEPAD_BACK;  // Share / Create
    if (b1 & 0x20) w |= XINPUT_GAMEPAD_START;  // Options
    if (b1 & 0x40) w |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (b1 & 0x80) w |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (b2 & 0x01) w |= PAD_GUIDE;             // PS button
    if (b2 & 0x02) w |= XINPUT_GAMEPAD_BACK;  // touchpad click
    g.wButtons = w;
    g.bLeftTrigger = l2;
    g.bRightTrigger = r2;
    // 0..255 (y down) -> XInput (y up); converts back exactly in read_pad
    g.sThumbLX = (SHORT)((d[0] - 128) * 256);
    g.sThumbLY = (SHORT)((127 - d[1]) * 256);
    g.sThumbRX = (SHORT)((d[2] - 128) * 256);
    g.sThumbRY = (SHORT)((127 - d[3]) * 256);
    std::lock_guard<std::mutex> lk(g_mu);
    g_state = g;
}

bool send_rumble(Pad& p, u32 v, HANDLE ev) {
    u8 small = (u8)(v & 0xFF), large = (u8)(v >> 8);
    std::vector<u8> r(std::max<USHORT>(p.out_len, 78), 0);
    size_t len = p.out_len;
    if (!p.dualsense && !p.bt) {
        r[0] = 0x05; r[1] = 0x01; r[4] = small; r[5] = large;  // flag 1: motors only (the light bar stays)
    } else if (!p.dualsense) {
        r[0] = 0x11; r[1] = 0xC0; r[3] = 0x01; r[6] = small; r[7] = large;
    } else {
        // classic rumble emulation (all firmware versions), audio haptics off; halved like SDL to feel
        // like an Xbox controller
        size_t o = p.bt ? 3 : 1;
        if (p.bt) {
            r[0] = 0x31;
            r[1] = (u8)(p.seq << 4);
            r[2] = 0x10;
            p.seq = (p.seq + 1) & 15;
        } else r[0] = 0x02;
        r[o] = 0x03;
        r[o + 2] = small >> 1;
        r[o + 3] = large >> 1;
    }
    if (p.bt) {
        const u8 hdr = 0xA2;
        u32 c = crc32(crc32(0, &hdr, 1), r.data(), 74);
        r[74] = (u8)c; r[75] = (u8)(c >> 8); r[76] = (u8)(c >> 16); r[77] = (u8)(c >> 24);
    }
    if (len < (p.bt ? 78u : 6u)) return false;
    OVERLAPPED ov{};
    ov.hEvent = ev;
    ResetEvent(ev);
    DWORD done = 0;
    if (!WriteFile(p.h, r.data(), (DWORD)len, nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) return false;
    if (WaitForSingleObject(ev, 200) != WAIT_OBJECT_0) {
        CancelIoEx(p.h, &ov);
        GetOverlappedResult(p.h, &ov, &done, TRUE);
        return true;  // a slow link: try again with the next change
    }
    return GetOverlappedResult(p.h, &ov, &done, FALSE) != 0;
}

void worker() {
    if (!load_api()) {
        log_write(LOG_MAIN, "controller: HID not available, PlayStation controllers only through XInput");
        return;
    }
    HANDLE rev = CreateEventW(nullptr, TRUE, FALSE, nullptr), wev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (;;) {
        Pad p;
        if (!open_pad(p)) {
            Sleep(3000);
            continue;
        }
        enable_full_reports(p);
        log_write(LOG_MAIN, "controller: %s connected (%s, input %u bytes, output %u bytes)", p.dualsense ? "DualSense" : "DualShock 4",
                  p.bt ? "Bluetooth" : "USB", p.in_len, p.out_len);
        std::vector<u8> buf(p.in_len);
        OVERLAPPED ov{};
        ov.hEvent = rev;
        bool pending = false, warned = false;
        u32 sent = 0;
        int quiet = 0;  // seconds without a report
        for (;;) {
            if (!pending) {
                ResetEvent(rev);
                if (!ReadFile(p.h, buf.data(), (DWORD)buf.size(), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
                pending = true;
            }
            HANDLE evs[2] = {rev, g_wake};
            DWORD r = WaitForMultipleObjects(2, evs, FALSE, 1000);
            if (r == WAIT_OBJECT_0) {
                DWORD n = 0;
                if (!GetOverlappedResult(p.h, &ov, &n, FALSE)) break;
                pending = false;
                quiet = 0;
                if (n) {
                    parse(p, buf.data(), n);
                    g_connected = true;
                }
            } else if (r == WAIT_TIMEOUT && ++quiet >= 5) break;  // switched off / out of range
            u32 want = g_rumble.load();
            if (want != sent) {
                if (!send_rumble(p, want, wev) && !warned) {
                    warned = true;
                    log_write(LOG_MAIN, "controller: vibration not accepted (error %lu)", GetLastError());
                }
                sent = want;
            }
        }
        if (pending) {
            CancelIoEx(p.h, &ov);
            DWORD n;
            GetOverlappedResult(p.h, &ov, &n, TRUE);
        }
        CloseHandle(p.h);
        g_connected = false;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_state = XINPUT_GAMEPAD{};
        }
        log_write(LOG_MAIN, "controller: %s disconnected", p.dualsense ? "DualSense" : "DualShock 4");
        Sleep(500);
    }
}

}  // namespace

bool pad_state(XINPUT_STATE* xs) {
    if (XInputGetState(0, xs) == ERROR_SUCCESS) return true;
    // no XInput controller: a PlayStation controller over HID (searched in the background)
    if (!g_started.exchange(true)) {
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread(worker).detach();
    }
    if (!g_connected) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    *xs = XINPUT_STATE{};
    xs->Gamepad = g_state;
    return true;
}

void pad_rumble(float small, float large) {
    auto b = [](float f) { return (u32)(f <= 0 ? 0 : f >= 1 ? 255 : f * 255.0f + 0.5f); };
    u32 v = b(small) | b(large) << 8;
    if (g_rumble.exchange(v) != v && g_wake) SetEvent(g_wake);
}

}  // namespace platform
