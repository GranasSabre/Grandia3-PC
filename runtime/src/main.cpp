// Grandia III native port - entry point.
#include "common.h"
#include "savestate.h"
#include <thread>
#include "memory.h"
#include "ee/kernel.h"
#include "hw/hw.h"
#include "hw/vif.h"
#include "hw/ipu.h"
#include "hw/gspipe.h"
#include "vu/vu.h"
#include "gs/gs.h"
#include "gs/gs_census.h"
#include "gs/gs_gpu.h"
#include "iop/iop.h"
#include "iop/cdvd.h"
#include "platform/platform.h"
#include "platform/audio.h"
#include "platform/subtitles.h"
#include "ee_modules.h"
#include <windows.h>
#include <map>
#include <mutex>

// default: MAIN KERNEL GS IOP CDVD MC GAME; chatty categories (SIF, PAD, SPU, IPU, ...) via --log=
bool g_log_enabled[LOG_COUNT] = {true, true, false, false, true, false, false, false, true, true, false, true, false, false, true};
static FILE* g_logfile = nullptr;
static std::mutex g_log_mutex;
static LARGE_INTEGER g_qpf, g_qpc0;

static const char* LOG_NAMES[LOG_COUNT] = {"main", "kern", "hw", "dma", "gs", "vif", "vu", "sif", "iop", "cdvd",
                                           "pad", "mc", "spu", "ipu", "game"};

double host_time_sec() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_qpc0.QuadPart) / (double)g_qpf.QuadPart;
}
u64 host_time_us() { return (u64)(host_time_sec() * 1e6); }

void log_write(int cat, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_log_mutex);
    double t = host_time_sec();
    if (g_logfile) {
        fprintf(g_logfile, "[%9.4f][%s] %s\n", t, LOG_NAMES[cat], buf);
        fflush(g_logfile);
    }
    if (platform::console_enabled()) {
        fprintf(stdout, "[%9.4f][%s] %s\n", t, LOG_NAMES[cat], buf);
        fflush(stdout);
    }
}

void fatal(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_write(LOG_MAIN, "FATAL: %s", buf);
    platform::fatal_message(buf);
    ExitProcess(1);
}

// ---------------------------------------------------------------------------
// Overlay reference images (for dispatch validation)
// ---------------------------------------------------------------------------
static std::map<std::string, std::vector<u8>> g_overlay_images;

extern "C" const u8* overlay_image_for(const char* name, u32* size) {
    auto it = g_overlay_images.find(name);
    if (it == g_overlay_images.end()) return nullptr;
    *size = (u32)it->second.size();
    return it->second.data();
}

static std::vector<u8> read_disc_file(const std::string& path) {
    u32 lsn, size;
    if (!cdvd::find_file(path, &lsn, &size)) fatal("File not found on disc: %s", path.c_str());
    std::vector<u8> d(((size + 2047) & ~2047u));
    cdvd::read_sectors(lsn, (u32)d.size() / 2048, d.data());
    d.resize(size);
    return d;
}

static void load_overlay_images() {
    std::map<std::string, std::vector<u8>> files;
    for (const EEOverlaySource* s = g_ee_overlay_sources; s->name; s++) {
        auto it = files.find(s->file);
        if (it == files.end()) it = files.emplace(s->file, read_disc_file(s->file)).first;
        const auto& f = it->second;
        if (s->offset + s->size > f.size()) fatal("Overlay %s out of range in %s", s->name, s->file);
        g_overlay_images[s->name] = std::vector<u8>(f.begin() + s->offset, f.begin() + s->offset + s->size);
    }
    log_write(LOG_MAIN, "Loaded %zu overlay reference images", g_overlay_images.size());
}

static u32 load_elf(const std::vector<u8>& elf) {
    u32 entry, phoff;
    u16 phnum;
    memcpy(&entry, &elf[24], 4);
    memcpy(&phoff, &elf[28], 4);
    memcpy(&phnum, &elf[44], 2);
    for (int i = 0; i < phnum; i++) {
        u32 ph[8];
        memcpy(ph, &elf[phoff + i * 32], 32);
        if (ph[0] != 1) continue;
        u32 off = ph[1], vaddr = ph[2], filesz = ph[4], memsz = ph[5];
        if (memsz) memset(mem::ram + (vaddr & 0x1FFFFFF), 0, memsz);
        if (filesz) memcpy(mem::ram + (vaddr & 0x1FFFFFF), &elf[off], filesz);
        log_write(LOG_MAIN, "ELF segment %08x filesz %x memsz %x", vaddr, filesz, memsz);
    }
    return entry;
}

std::string exe_dir() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p);
    return s.substr(0, s.find_last_of("\\/"));
}

static bool file_exists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool file_exists_u8(const std::string& p) {  // UTF-8 path (chosen in a file dialog)
    int n = MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p.c_str(), -1, w.data(), n);
    DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool g_trace_syscalls_flag = false;
bool gs_log_draws_flag = false;
namespace kernel { extern bool g_dispatch_log; }
namespace gs { extern bool g_gs_null; extern int g_gs_threads; extern double g_draw_stats_at; extern double g_log_draws_from; extern bool g_log_draws; extern double g_vram_dump_at; extern std::string g_vram_dump_path; }
static bool g_watchdog_flag = false;
namespace hw { extern bool g_poll_stats; }
namespace gs { namespace gpu { extern bool g_no_uav_overlap, g_gpu_timing; } }
namespace kernel { extern bool g_trace_syscalls; extern bool g_watchdog; extern bool g_native_perf; }

static DWORD WINAPI emu_thread(void*) {
    kernel::g_trace_syscalls = g_trace_syscalls_flag;
    kernel::g_watchdog = g_watchdog_flag;
    gs::g_log_draws = gs_log_draws_flag;
    hw::init();
    vif::init();
    vu::init();
    ipu::init();
    iop::init();
    load_overlay_images();
    // boot executable named in SYSTEM.CNF (disc 2: SLUS_213.45, identical code)
    std::string boot = "\\SLUS_213.34";
    {
        std::vector<u8> cnf = read_disc_file("\\SYSTEM.CNF");
        std::string t(cnf.begin(), cnf.end());
        size_t a = t.find("cdrom0:");
        if (a != std::string::npos) {
            size_t b = t.find_first_of(";\r\n", a);
            boot = t.substr(a + 7, b == std::string::npos ? std::string::npos : b - a - 7);
        }
    }
    log_write(LOG_MAIN, "Disc %d, boot file %s", platform::config().disc, boot.c_str());
    std::vector<u8> elf = read_disc_file(boot);
    u32 entry = load_elf(elf);
    kernel::init();
    log_write(LOG_MAIN, "Starting EE at %08x", entry);
    kernel::run_main(entry);
    return 0;
}

static bool g_gpu_compare_flag = false;
static int g_gs_thread_flag = -1;
namespace texthook { extern bool g_watch_msg; }  // --gs-thread=0/1 overrides the setting
extern "C" { int g_ee_patch[8]; }

int main(int argc, char** argv) {
    QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&g_qpc0);
    std::string dir = exe_dir();
    platform::Config& cfg = platform::config();
    platform::load_config(dir + "\\grandia3.ini");
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--console") cfg.console = true;
        else if (a.rfind("--log=", 0) == 0) {
            std::string cats = a.substr(6);
            for (int c = 0; c < LOG_COUNT; c++)
                if (cats.find(LOG_NAMES[c]) != std::string::npos) g_log_enabled[c] = true;
        } else if (a.rfind("--iso=", 0) == 0) cfg.iso_path = a.substr(6);
        else if (a == "--trace-syscalls") g_trace_syscalls_flag = true;
        else if (a == "--watchdog") g_watchdog_flag = true;
        else if (a == "--perf") kernel::g_native_perf = true;
        else if (a.rfind("--perf-from=", 0) == 0) { kernel::g_native_perf = true; kernel::g_native_perf_from = atof(a.c_str() + 12); }
        else if (a == "--perf=raster") { kernel::g_native_perf = true; extern int perf_target_raster(); perf_target_raster(); }
        else if (a == "--watch-msg") texthook::g_watch_msg = true;
        else if (a == "--steamdeck") platform::g_force_steam_deck = true;
        else if (a.rfind("--watch-mem=", 0) == 0) {
            // SEC:[*PTR+]OFF:LEN (hex addresses)
            namespace k = kernel;
            extern double watch_mem_parse(const char*);
            watch_mem_parse(a.c_str() + 12);
        }
        else if (a == "--perf=gsp") { kernel::g_native_perf = true; extern int perf_target_gsp(); perf_target_gsp(); }
        else if (a.rfind("--gs-thread=", 0) == 0) g_gs_thread_flag = atoi(a.c_str() + 12);
        else if (a.rfind("--gs-threads=", 0) == 0) gs::g_gs_threads = atoi(a.c_str() + 13);
        else if (a == "--gs-null") gs::g_gs_null = true;
        else if (a == "--widescreen") cfg.widescreen_hack = true;
        else if (a.rfind("--aspect=", 0) == 0) cfg.aspect = atoi(a.c_str() + 9);
        else if (a.rfind("--language=", 0) == 0) cfg.language = a.substr(11);
        else if (a == "--find-mdz") { extern bool find_mdz_flag(); find_mdz_flag(); }
        else if (a.rfind("--disc=", 0) == 0) cfg.disc = atoi(a.c_str() + 7) == 2 ? 2 : 1;
        else if (a == "--no-letterbox") cfg.remove_letterbox = true;
        else if (a == "--dump-textures") cfg.texture_dump = true;
        else if (a == "--no-hd-textures") cfg.hd_textures = false;
        else if (a == "--hd-ab") gs::gpu::g_hd_ab = true;
        else if (a == "--sharpen-ab") gs::gpu::g_sharpen_ab = true;
        else if (a == "--shots-final") platform::g_shots_final = true;
        else if (a.rfind("--sharpen=", 0) == 0) cfg.sharpen = atoi(a.c_str() + 10);
        else if (a.rfind("--hd-lod-bias=", 0) == 0) cfg.hd_lod_bias = (float)atof(a.c_str() + 14);
        else if (a.rfind("--window-shot=", 0) == 0) platform::g_overlay_shot_at = atof(a.c_str() + 14);  // the window picture with overlays
        else if (a.rfind("--window-test=", 0) == 0) platform::g_window_test_at = atof(a.c_str() + 14);
        else if (a.rfind("--settings-test=", 0) == 0) platform::g_settings_test_at = atof(a.c_str() + 16);
        else if (a.rfind("--overlay-test=", 0) == 0) {
            platform::g_overlay_test_at = atof(a.c_str() + 15);
            if (const char* k = strchr(a.c_str() + 15, ',')) platform::g_overlay_test_downs = atoi(k + 1);
        }
        else if (a.rfind("--watch-text=", 0) == 0) kernel::g_watch_text_at = atof(a.c_str() + 13);
        else if (a == "--log-fps") platform::g_log_fps = true;
        else if (a.rfind("--turbo-from=", 0) == 0) platform::g_turbo_from = atof(a.c_str() + 13);
        else if (a.rfind("--frame-delay=", 0) == 0) gs::g_test_frame_delay_ms = atoi(a.c_str() + 14);
        else if (a == "--scene-skip-flag") { extern bool scene_skip_flag_test(); scene_skip_flag_test(); }
        else if (a == "--dispatch-log") kernel::g_dispatch_log = true;
        else if (a == "--uav-overlap") gs::gpu::g_no_uav_overlap = false;
        else if (a == "--poll-stats") hw::g_poll_stats = true;
        else if (a == "--gpu-timing") gs::gpu::g_gpu_timing = true;
        else if (a == "--text-log") { extern void texthook_set_log(bool); texthook_set_log(true); }
        else if (a == "--force60") cfg.fps60 = true;
        else if (a == "--force60-battle") cfg.fps60 = cfg.fps60_battle = true;
        else if (a.rfind("--state-save=", 0) == 0 || a.rfind("--state-load=", 0) == 0) {
            // test: --state-save=SEC:SLOT / --state-load=SEC:SLOT (host seconds after start)
            double at = 0;
            int slot = 1;
            sscanf(a.c_str() + 13, "%lf:%d", &at, &slot);
            bool save = a[8] == 's' && a[9] == 'a';
            std::thread([at, slot, save] {
                while (host_time_sec() < at) Sleep(50);
                if (save) ss::request_save(slot);
                else ss::request_load(slot);
            }).detach();
        }
        else if (a.rfind("--resume-test=", 0) == 0)
            sscanf(a.c_str() + 14, "%lf,%lf", &kernel::g_resume_test_at, &kernel::g_resume_test_period);
        else if (a.rfind("--vol=", 0) == 0) sscanf(a.c_str() + 6, "%d,%d,%d,%d", &cfg.vol_music, &cfg.vol_voice, &cfg.vol_sfx, &cfg.vol_movie);
        else if (a.rfind("--ramdump=", 0) == 0) {
            for (const char* p = a.c_str() + 10; *p;) {
                platform::g_ram_dump_at.push_back(atof(p));
                while (*p && *p != ',') p++;
                if (*p == ',') p++;
            }
        }
        else if (a.rfind("--renderer=", 0) == 0) cfg.renderer = a.substr(11) == "gpu" ? 1 : 0;
        else if (a.rfind("--scale=", 0) == 0) cfg.internal_scale = atoi(a.c_str() + 8);
        else if (a == "--gpu-compare") { g_gpu_compare_flag = true; cfg.renderer = 1; cfg.internal_scale = 1; }
        else if (a.rfind("--gs-census=", 0) == 0) {
            double f = 0, u = 1e30;
            sscanf(a.c_str() + 12, "%lf:%lf", &f, &u);
            gs::census_enable(f, u);
        }
        else if (a == "--ipu-trace") { extern void ipu_enable_trace(); ipu_enable_trace(); }
        else if (a == "--vu-interp") { extern void vu_set_mode(int); vu_set_mode(0); }
        else if (a == "--vu-verify") { extern void vu_set_mode(int); vu_set_mode(2); }
        else if (a.rfind("--shots-window=", 0) == 0) {
            double f = 0, u = 1e30;
            sscanf(a.c_str() + 15, "%lf:%lf", &f, &u);
            platform::set_screenshot_window(f, u);
        }
        else if (a == "--vu-dump") { extern bool vu_dump_flag(); vu_dump_flag(); }
        else if (a.rfind("--draw-stats=", 0) == 0) gs::g_draw_stats_at = atof(a.c_str() + 13);
        else if (a.rfind("--press=", 0) == 0) platform::add_autopress(a.substr(8));
        else if (a == "--log-draws") { extern bool gs_log_draws_flag; gs_log_draws_flag = true; }
        else if (a.rfind("--log-draws-from=", 0) == 0) { gs_log_draws_flag = true; gs::g_log_draws_from = atof(a.substr(17).c_str()); }
        else if (a.rfind("--vramdump=", 0) == 0) { gs::g_vram_dump_at = atof(a.substr(11).c_str()); gs::g_vram_dump_path = dir + "/vram.bin"; }
        else if (a.rfind("--wav=", 0) == 0) audio::set_wav_path(a.substr(6));
        else if (a.rfind("--shots=", 0) == 0) platform::set_screenshot_interval(atof(a.substr(8).c_str()), dir + "/shots");
    }
    platform::init_console(cfg.console);
    g_logfile = fopen((dir + "\\grandia3.log").c_str(), "w");
    log_write(LOG_MAIN, "Steam Deck: %s", platform::steam_deck_info().c_str());

    // the game data: the player's own disc image (iso_path / iso_path2, chosen at the first start) or
    // an unpacked disc (data\disc.idx next to the executable; disc 2: data\disc2.idx)
    bool disc_ok = false;
    std::string& iso_cfg = cfg.disc == 2 ? cfg.iso_path2 : cfg.iso_path;
    {
        std::string idx = cfg.disc == 2 ? "disc2.idx" : "disc.idx";
        if (iso_cfg.empty() && file_exists(dir + "\\data\\" + idx)) disc_ok = cdvd::open_data_dir(dir + "\\data", idx);
    }
    std::string iso = iso_cfg;
    if (!disc_ok && (iso.empty() || !file_exists_u8(iso))) {
        iso.clear();
        std::string n = std::to_string(cfg.disc);
        for (std::string cand : {dir + "\\disc" + n + ".iso", dir + "\\Grandia III (USA) (Disc " + n + ").iso"})
            if (file_exists(cand)) { iso = cand; break; }
    }
    bool asked = false;
    while (!disc_ok) {
        if (iso.empty() || !file_exists_u8(iso)) {
            if (!asked)
                platform::message(cfg.disc == 2 ? platform::tr(L"Für Disc 2 braucht Grandia III das Abbild (ISO) deiner Disc 2 von Grandia III (USA). Bitte im nächsten Fenster auswählen.")
                                               : platform::tr(L"Grandia III braucht ein Abbild (ISO) deiner eigenen Disc 1 von Grandia III (USA). Bitte im nächsten Fenster auswählen; es wird nicht verändert oder kopiert."));
            asked = true;
            iso = platform::choose_disc_image(cfg.disc);
            if (iso.empty()) return 1;  // cancelled
        }
        // the right disc: its boot program (SYSTEM.CNF)
        const char* boot = cfg.disc == 2 ? "\\SLUS_213.45;1" : "\\SLUS_213.34;1";
        u32 lsn = 0, size = 0;
        if (cdvd::open_image(iso) && cdvd::find_file(boot, &lsn, &size)) {
            disc_ok = true;
            if (iso != iso_cfg) {
                iso_cfg = iso;
                platform::save_config();
            }
            break;
        }
        cdvd::close_image();
        wchar_t t[256];
        swprintf(t, 256, platform::tr(L"Das ist kein Abbild von Grandia III (USA) Disc %d. Bitte das passende auswählen."), cfg.disc);
        platform::message(t);
        iso.clear();
    }
    mem::init();
    subs::init();
    platform::apply_patches();
    kernel::g_force60 = cfg.fps60;
    g_ee_patch[1] = g_ee_patch[2] = cfg.remove_letterbox;
    g_ee_patch[3] = cfg.movie_skip;
    if (!platform::init_video()) return 1;
    gs::raster_request_gpu(cfg.renderer != 0, cfg.internal_scale, g_gpu_compare_flag);
    gs::init();
    gsp::start(g_gs_thread_flag >= 0 ? g_gs_thread_flag != 0 : cfg.gs_thread);
    audio::init();
    CreateThread(nullptr, 8 * 1024 * 1024, emu_thread, nullptr, 0, nullptr);
    platform::run_message_loop();
    return 0;
}
