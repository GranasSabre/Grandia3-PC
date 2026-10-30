// Savestates: file format, slots, and the order in which the subsystems are saved and restored.
//
// File: header (magic, format version, layout signature, emulated time, save time, disc), then the
// sections (name, size, data) compressed with XPRESS (Windows compression API). The layout
// signature lists the sizes of all structures stored as raw bytes; a build in which one of them
// changed refuses the file before anything is touched.
#include "savestate.h"
#include "memory.h"
#include "ee/kernel.h"
#include "hw/ipu.h"
#include "hw/gspipe.h"
#include "platform/platform.h"
#include <windows.h>
#include <compressapi.h>
#include <atomic>
#include <ctime>
#include <memory>
#include <thread>

namespace hw { void state(ss::Stream&); }
namespace sif { void state(ss::Stream&); }
namespace gif { void state(ss::Stream&); }
namespace vif { void state(ss::Stream&); }
namespace vu { void state(ss::Stream&); }
namespace ipu { void state(ss::Stream&); }
namespace spu2 { void state(ss::Stream&); }
namespace gs { void state(ss::Stream&); void reraise_events(); }
namespace iop { void state(ss::Stream&); }
namespace iop::kern { void state(ss::Stream&); bool can_savestate(std::string* why); void after_state_load(); }
namespace iop::pad { void state(ss::Stream&); }
namespace iop::mc { void state(ss::Stream&); bool files_open(); }
void libsd_state(ss::Stream&);
void ioman_state(ss::Stream&);

namespace ss {

static void mem_state(Stream& s) {
    u32 ver = 1;
    s.v(ver);
    s.raw(mem::ram, mem::RAM_SIZE);
    s.raw(mem::spr, mem::SPR_SIZE);
    s.raw(mem::iop_ram, mem::IOP_RAM_SIZE);
    s.arr(mem::vu0_micro);
    s.arr(mem::vu1_micro);
    s.arr(mem::vu1_data);
    s.raw(g_vu0_mem, 4096);
}

struct Section { const char* name; void (*fn)(Stream&); };
// Load order: "iop" first (it checks that this session has the same IOP modules before changing
// anything), the kernel after memory (its threads refer to EE RAM), GS after memory.
static const Section SECTIONS[] = {
    {"iop", iop::state},       {"mem", mem_state},   {"kernel", kernel::state}, {"hw", hw::state},
    {"sif", sif::state},       {"gif", gif::state},  {"vif", vif::state},       {"vu", vu::state},
    {"ipu", ipu::state},       {"gs", gs::state},    {"spu2", spu2::state},     {"libsd", libsd_state},
    {"iopkern", iop::kern::state}, {"ioman", ioman_state}, {"mc", iop::mc::state}, {"pad", iop::pad::state},
};

static const char MAGIC[8] = {'G', 'R', '3', 'S', 'T', 'A', 'T', 'E'};
static const u32 FORMAT = 1;
struct Header {
    char magic[8];
    u32 format;
    u32 compressed;  // 1: XPRESS_HUFF
    double emu_time;
    s64 saved_at;    // time()
    u32 disc;
    u32 sig_size;
    u64 raw_size;
    u64 data_size;
};

// ---------------------------------------------------------------------------
// Slots and requests
// ---------------------------------------------------------------------------
static std::string exe_dir() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p);
    return s.substr(0, s.find_last_of("\\/"));
}
std::string slot_path(int slot) { return exe_dir() + "\\savestates\\slot" + std::to_string(slot) + ".g3s"; }

static bool read_header(FILE* f, Header& h) {
    return fread(&h, sizeof(h), 1, f) == 1 && memcmp(h.magic, MAGIC, 8) == 0;
}

bool slot_exists(int slot, std::string* when) {
    FILE* f = fopen(slot_path(slot).c_str(), "rb");
    if (!f) return false;
    Header h;
    bool ok = read_header(f, h);
    fclose(f);
    if (ok && when) {
        time_t t = (time_t)h.saved_at;
        struct tm lt;
        localtime_s(&lt, &t);
        char b[32];
        strftime(b, sizeof b, "%d.%m.%Y %H:%M", &lt);
        *when = b;
    }
    return ok;
}

static std::atomic<int> g_save_slot{-1}, g_load_slot{-1};
static double g_req_time = 0;  // emulated time of the request (gives up after a while)
static std::atomic<bool> g_writing{false};

void request_save(int slot) {
    if (g_writing) { platform::show_notice(platform::tr(L"Speichern läuft noch …"), L"", 2.0); return; }
    g_req_time = -1;
    g_save_slot = slot;
}
void request_load(int slot) {
    if (!slot_exists(slot)) {
        platform::show_notice(L"Slot " + std::to_wstring(slot) + platform::tr(L" ist leer"), L"", 2.0);
        return;
    }
    g_load_slot = slot;
}
bool pending() { return g_save_slot >= 0 || g_load_slot >= 0; }
bool save_pending() { return g_save_slot >= 0; }
bool load_pending() { return g_load_slot >= 0; }

static std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

bool can_save(std::string* why) {
    std::string w;
    bool ok = true;
    if (platform::skip_active()) { w = "Zwischensequenz wird übersprungen"; ok = false; }
    else if (kernel::movie_running()) { w = "während eines Films nicht möglich"; ok = false; }
    else if (iop::mc::files_open()) { w = "Speicherkarte wird gerade benutzt"; ok = false; }
    else if (!iop::kern::can_savestate(&w)) ok = false;
    else if (!kernel::can_savestate(&w)) ok = false;
    if (!ok) {
        // retried at the next safe points for a few seconds, then given up
        double now = kernel::emu_time();
        if (g_req_time < 0) g_req_time = now;
        if (now - g_req_time > 3.0) {
            log_write(LOG_MAIN, "savestate: not possible now (%s)", w.c_str());
            platform::show_notice(platform::tr(L"Speichern gerade nicht möglich"), platform::tr(widen(w)), 3.0);
            g_save_slot = -1;
        }
        if (why) *why = w;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Layout signature: sizes of the raw structures (a counting pass over the current state)
// ---------------------------------------------------------------------------
static std::string layout_signature() {
    Stream s(nullptr);
    for (const Section& sec : SECTIONS) sec.fn(s);
    std::string sig;
    for (auto& kv : s.layout()) sig += kv.first + "=" + std::to_string(kv.second) + ";";
    return sig;
}

// ---------------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------------
// The compression API (cabinet.dll, Windows 8 and later) is looked up when first needed: imported
// directly, one missing export in an older Wine/Proton stops the program before it even starts.
// Without it savestates are written uncompressed.
struct CabinetApi {
    decltype(&CreateCompressor) create_compressor = nullptr;
    decltype(&Compress) compress = nullptr;
    decltype(&CloseCompressor) close_compressor = nullptr;
    decltype(&CreateDecompressor) create_decompressor = nullptr;
    decltype(&Decompress) decompress = nullptr;
    decltype(&CloseDecompressor) close_decompressor = nullptr;
};

static const CabinetApi& cabinet() {
    static const CabinetApi api = [] {
        CabinetApi a;
        if (HMODULE m = LoadLibraryW(L"cabinet.dll")) {
            a.create_compressor = (decltype(a.create_compressor))(void*)GetProcAddress(m, "CreateCompressor");
            a.compress = (decltype(a.compress))(void*)GetProcAddress(m, "Compress");
            a.close_compressor = (decltype(a.close_compressor))(void*)GetProcAddress(m, "CloseCompressor");
            a.create_decompressor = (decltype(a.create_decompressor))(void*)GetProcAddress(m, "CreateDecompressor");
            a.decompress = (decltype(a.decompress))(void*)GetProcAddress(m, "Decompress");
            a.close_decompressor = (decltype(a.close_decompressor))(void*)GetProcAddress(m, "CloseDecompressor");
        }
        if (!a.create_compressor || !a.compress || !a.close_compressor)
            log_write(LOG_MAIN, "savestate: no compression API, states are written uncompressed");
        return a;
    }();
    return api;
}

static bool compress_xpress(const std::vector<u8>& in, std::vector<u8>& out) {
    const CabinetApi& api = cabinet();
    if (!api.create_compressor || !api.compress || !api.close_compressor) return false;
    COMPRESSOR_HANDLE c = nullptr;
    if (!api.create_compressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &c)) return false;
    SIZE_T need = 0;
    api.compress(c, (PVOID)in.data(), in.size(), nullptr, 0, &need);
    out.resize(need);
    SIZE_T got = 0;
    bool ok = api.compress(c, (PVOID)in.data(), in.size(), out.data(), out.size(), &got);
    api.close_compressor(c);
    out.resize(ok ? got : 0);
    return ok;
}

void do_save() {
    int slot = g_save_slot.exchange(-1);
    if (slot < 0) return;
    double t0 = host_time_sec();
    gsp::sync(gsp::SYNC_STATE);  // the GS pipeline has executed everything
    // interrupts it raised meanwhile belong to the saved state (a FINISH the game waits for at the
    // end of the frame: without it the game waits forever after loading)
    gsp::poll_events();
    auto payload = std::make_shared<std::vector<u8>>();
    payload->reserve(48u << 20);
    for (const Section& sec : SECTIONS) {
        u8 nl = (u8)strlen(sec.name);
        payload->push_back(nl);
        payload->insert(payload->end(), sec.name, sec.name + nl);
        size_t at = payload->size();
        payload->resize(at + 8);
        std::vector<u8> data;
        Stream s(&data);
        sec.fn(s);
        u64 n = data.size();
        memcpy(payload->data() + at, &n, 8);
        payload->insert(payload->end(), data.begin(), data.end());
    }
    auto sig = std::make_shared<std::string>(layout_signature());
    Header h{};
    memcpy(h.magic, MAGIC, 8);
    h.format = FORMAT;
    h.emu_time = kernel::emu_time();
    h.saved_at = (s64)time(nullptr);
    h.disc = (u32)platform::config().disc;
    h.sig_size = (u32)sig->size();
    h.raw_size = payload->size();
    double t1 = host_time_sec();
    log_write(LOG_MAIN, "savestate: slot %d captured (%.1f MB, %.0f ms)", slot, payload->size() / 1048576.0, (t1 - t0) * 1000);
    // compression and writing run beside the emulation
    g_writing = true;
    std::thread([payload, sig, h, slot]() mutable {
        std::vector<u8> comp;
        h.compressed = compress_xpress(*payload, comp) ? 1 : 0;
        const std::vector<u8>& data = h.compressed ? comp : *payload;
        h.data_size = data.size();
        std::string path = slot_path(slot);
        CreateDirectoryA((exe_dir() + "\\savestates").c_str(), nullptr);
        std::string tmp = path + ".tmp";
        FILE* f = fopen(tmp.c_str(), "wb");
        bool ok = f && fwrite(&h, sizeof h, 1, f) == 1 && fwrite(sig->data(), 1, sig->size(), f) == sig->size() &&
                  fwrite(data.data(), 1, data.size(), f) == data.size();
        if (f) ok = (fclose(f) == 0) && ok;
        if (ok) ok = MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        log_write(LOG_MAIN, "savestate: slot %d %s (%.1f MB)", slot, ok ? "written" : "WRITE FAILED", data.size() / 1048576.0);
        platform::show_notice(ok ? platform::tr(L"Gespeichert in Slot ") + std::to_wstring(slot) : std::wstring(platform::tr(L"Speichern fehlgeschlagen")), L"", 2.0);
        g_writing = false;
    }).detach();
}

// ---------------------------------------------------------------------------
// Load (from the scheduler: no thread running)
// ---------------------------------------------------------------------------
static bool read_file(int slot, Header& h, std::string& sig, std::vector<u8>& payload, std::string& err) {
    FILE* f = fopen(slot_path(slot).c_str(), "rb");
    if (!f) { err = "Datei nicht gefunden"; return false; }
    bool ok = read_header(f, h);
    if (ok && h.format != FORMAT) { err = "anderes Savestate-Format"; ok = false; }
    std::vector<u8> data;
    if (ok) {
        sig.resize(h.sig_size);
        data.resize(h.data_size);
        ok = fread(sig.data(), 1, sig.size(), f) == sig.size() && fread(data.data(), 1, data.size(), f) == data.size();
        if (!ok) err = "Datei unvollständig";
    }
    fclose(f);
    if (!ok) return false;
    if (!h.compressed) { payload.swap(data); return true; }
    const CabinetApi& api = cabinet();
    DECOMPRESSOR_HANDLE d = nullptr;
    if (!api.create_decompressor || !api.decompress || !api.close_decompressor ||
        !api.create_decompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, nullptr, &d)) { err = "Entpacken nicht möglich"; return false; }
    payload.resize(h.raw_size);
    SIZE_T got = 0;
    ok = api.decompress(d, data.data(), data.size(), payload.data(), payload.size(), &got) && got == h.raw_size;
    api.close_decompressor(d);
    if (!ok) err = "Datei beschädigt";
    return ok;
}

void do_load() {
    int slot = g_load_slot.exchange(-1);
    if (slot < 0) return;
    double t0 = host_time_sec();
    gsp::sync(gsp::SYNC_STATE);
    gsp::poll_events();  // the old state's pending interrupts: replaced by the loaded state
    Header h;
    std::string sig, err;
    std::vector<u8> payload;
    if (!read_file(slot, h, sig, payload, err)) {
        log_write(LOG_MAIN, "savestate: slot %d not loaded: %s", slot, err.c_str());
        platform::show_notice(platform::tr(L"Laden fehlgeschlagen"), platform::tr(widen(err)), 3.0);
        return;
    }
    if (h.disc != (u32)platform::config().disc) {
        platform::show_notice(platform::tr(L"Laden nicht möglich"), platform::tr(L"Der Spielstand gehört zu Disc ") + std::to_wstring(h.disc), 3.0);
        return;
    }
    if (sig != layout_signature()) {
        log_write(LOG_MAIN, "savestate: slot %d has another layout:\n  file: %s\n  this: %s", slot, sig.c_str(), layout_signature().c_str());
        platform::show_notice(platform::tr(L"Laden nicht möglich"), platform::tr(L"Savestate einer anderen Programmversion"), 3.0);
        return;
    }
    // sections by name
    std::map<std::string, std::pair<const u8*, size_t>> secs;
    for (size_t p = 0; p < payload.size();) {
        u8 nl = payload[p];
        std::string name((const char*)&payload[p + 1], nl);
        u64 n;
        memcpy(&n, &payload[p + 1 + nl], 8);
        secs[name] = {&payload[p + 9 + nl], (size_t)n};
        p += 9 + nl + n;
    }
    for (const Section& sec : SECTIONS) {
        auto it = secs.find(sec.name);
        if (it == secs.end()) {
            platform::show_notice(platform::tr(L"Laden nicht möglich"), platform::tr(L"Savestate unvollständig"), 3.0);
            return;
        }
    }
    platform::skip_stop("savestate");
    bool first = true;
    for (const Section& sec : SECTIONS) {
        auto& d = secs[sec.name];
        Stream s(d.first, d.second);
        sec.fn(s);
        if (!s.ok) {
            log_write(LOG_MAIN, "savestate: section %s: %s", sec.name, s.error.c_str());
            if (first) {  // nothing changed yet (the IOP check comes first)
                platform::show_notice(platform::tr(L"Laden nicht möglich"), platform::tr(widen(s.error)), 3.0);
                return;
            }
            fatal("Savestate (Slot %d) konnte nicht vollständig geladen werden: %s: %s", slot, sec.name, s.error.c_str());
        }
        first = false;
    }
    gs::reraise_events();
    kernel::after_state_load();
    iop::kern::after_state_load();
    log_write(LOG_MAIN, "savestate: slot %d loaded (emulated time %.1f s, %.0f ms)", slot, h.emu_time, (host_time_sec() - t0) * 1000);
    platform::show_notice(L"Slot " + std::to_wstring(slot) + platform::tr(L" geladen"), L"", 2.0);
}

}  // namespace ss
