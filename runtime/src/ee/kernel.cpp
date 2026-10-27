// EE kernel HLE: threads (as Win32 fibers), semaphores, interrupts, alarms, syscalls.
#include "kernel.h"
namespace texthook { void font_tick(); }
#include "../memory.h"
#include "../hw/hw.h"
#include "../hw/sif.h"
#include "../gs/gs.h"
#include "../iop/iop.h"
#include "ee_modules.h"
#include "../platform/platform.h"
#include "../hw/ipu.h"
#include "../hw/gspipe.h"
#include "../iop/cdvd.h"
#include "../savestate.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <immintrin.h>
#include <unordered_map>

extern "C" volatile int g_ee_irq_pending = 0;

namespace kernel {

u32 intc_stat = 0, intc_mask = 0;
u32 dmac_stat = 0;

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------
static constexpr double EE_CLOCK = 294912000.0;
static constexpr double FIELD_RATE = 59.94;              // NTSC
static constexpr double FIELD_PERIOD = 1.0 / FIELD_RATE;
static constexpr double VBLANK_LEN = FIELD_PERIOD * 22.0 / 262.5;
static constexpr double HSYNC_RATE = 15734.26;

static volatile bool g_frame_late = false;  // --perf: the current game frame missed a vertical blank
static double g_time_base = 0;     // host time at emu start
static double g_emu_time = 0;      // current emulated time (seconds)
static double g_next_vblank_s = FIELD_PERIOD;
bool g_force60 = false;  // --force60 / option fps60

// 60 fps: the game's framework (GA Target Platform Framework) waits refresh / FrameRate vsyncs per
// frame and scales its timing by the frame rate; the field and battle modules request 30 fps
// (SetFrameRate, applied at the end of a frame). With the option the requests are recorded and
// FrameRate stays 60; without it the last requested rate is restored.
static bool battle_loaded();
static u8 g_fr_game_rate = 0;   // frame_rate_option: the game's own rate (saved in savestates)
static bool g_fr_forced = false;  // FrameRate currently forced to 60
// The game's own frame rate while the 60 fps option forces 60 (else 0): the field turns the frame
// step into "frames at its rate" with FrameRate (hook_frame_rate_float)
int native_frame_rate() { return g_fr_forced && g_fr_game_rate && g_fr_game_rate != 60 ? g_fr_game_rate : 0; }
int g_test_step_mode = -1;  // --step-mode=N: test, forces the frame step mode byte
float g_test_time_scale = 0;  // --time-scale=F: test, forces the time scale float
static void frame_rate_option() {
    u8& game_rate = g_fr_game_rate;
    bool& forced = g_fr_forced;
    u8& rate = mem::ram[0x1EBAD8];     // FrameRate
    u8& request = mem::ram[0x218AE8];  // pending SetFrameRate value (0 = none; SetFrameRate 0x1AD380 stores it)
    if (request) game_rate = request;
    // battles keep the game's own rate (their timing was only checked in the field)
    bool battle = battle_loaded();  // every vertical blank (also feeds battle_running())
    bool force = g_force60 && (!battle || platform::config().fps60_battle);
    // The game advances a fixed 1/FrameRate per frame: a scene that cannot hold 60 fps runs in slow
    // motion against the streamed music and speech. Such scenes fall back to the game's own rate
    // for a while (doubling on repeated failures). Only sustained slowness counts: loading (disc
    // reads beyond music streaming, few flips) and movies (the IPU decodes) are not judged.
    static int win_vb = 0, bad = 0, good = 0;
    static u64 win_flips = 0, win_ipu = 0, win_sect = 0;
    static double hold_until = 0, hold_len = 10;
    if (force && forced && rate == 60) {
        if (++win_vb >= 60) {
            u64 f = gs::flip_count(), ic = ipu::command_count(), sc = ::cdvd::sectors_read();
            int flips = (int)(f - win_flips);
            bool skip = ic != win_ipu || sc - win_sect > 64 || flips < 20;
            win_flips = f; win_ipu = ic; win_sect = sc; win_vb = 0;
            if (skip) bad = -2;  // and the two seconds after it
            else if (flips <= 54) bad++;
            else if (bad > 0) bad = 0;
            else if (bad < 0) bad++;
            good = flips >= 57 ? good + 1 : 0;
            if (good >= 60) hold_len = 10;
            if (bad >= 3) {
                hold_until = g_emu_time + hold_len;
                log_write(LOG_KERNEL, "60 fps: scene too slow (%d fps), game rate for %.0f s", flips, hold_len);
                hold_len = std::min(hold_len * 2, 160.0);
                bad = good = 0;
            }
        }
    } else {
        win_vb = 0;
        bad = g_emu_time < hold_until + 1.0 ? 0 : -2;  // a retry after a fallback is judged at once
        win_flips = gs::flip_count(); win_ipu = ipu::command_count(); win_sect = ::cdvd::sectors_read();
    }
    if (g_emu_time < hold_until) force = false;
    if (force) {
        if (rate != 0) {                    // set once the INI was read
            if (rate != 60) game_rate = rate;  // the game's own rate (also a request applied before we saw it)
            request = 0;
            rate = 60;
            forced = true;
        }
    } else if (forced) {
        if (game_rate) rate = game_rate;
        forced = false;
    }
    static u8 logged = 0, logged_mode = 0xFF;
    if (g_test_step_mode >= 0) mem::ram[0x218AF0] = (u8)g_test_step_mode;
    if (g_test_time_scale > 0) memcpy(&mem::ram[0x1EBAE0], &g_test_time_scale, 4);
    u8 mode = mem::ram[0x218AF0];  // frame step mode: 0 fixed, 1 catch-up steps, 2 measured time
    if (rate != logged || mode != logged_mode) {
        log_write(LOG_KERNEL, "frame rate %d -> %d, step mode %d, time scale %.2f (game %d%s)", logged, rate, mode, [] { u32 u = mem::rd32(0x1EBAE0); float f; memcpy(&f, &u, 4); return (double)f; }(), game_rate,
                  forced ? ", 60 fps option" : "");
        logged = rate;
        logged_mode = mode;
    }
}
static double g_next_vblank_e = FIELD_PERIOD + VBLANK_LEN;
static double g_speed = 1.0;

double emu_time() { return g_emu_time; }
u64 cycles() { return (u64)(g_emu_time * EE_CLOCK); }
u32 hsync_count() { return (u32)(g_emu_time * HSYNC_RATE); }

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
enum { THS_RUN = 1, THS_READY = 2, THS_WAIT = 4, THS_SUSPEND = 8, THS_WAITSUSPEND = 12, THS_DORMANT = 16 };
enum { WAIT_NONE = 0, WAIT_SLEEP = 1, WAIT_SEMA = 2 };

struct Thread {
    int id = 0;
    int status = THS_DORMANT;
    u32 func = 0, stack = 0, stack_size = 0, gp = 0;
    int init_prio = 0, prio = 0;
    u32 attr = 0, option = 0;
    int wait_type = WAIT_NONE, wait_id = 0;
    int wakeup_count = 0;
    u32 arg = 0;
    s64 ready_seq = 0;
    EEContext* ctx = nullptr;
    void* fiber = nullptr;
    bool fiber_dead = false;   // fiber finished; delete when safe
    int wait_result = 0;
    bool suspended = false;
    // where the thread continues after a savestate restore (set when it blocks, see block_current)
    u32 resume_pc = 0;
    int resume_kind = 0;
};

// resume kinds: how a blocked thread continues at the MIPS level
enum { RK_NONE = 0,    // blocked somewhere without a resume point (no savestate possible)
       RK_FRESH,       // started, never ran: enters its function
       RK_PLAIN,       // continues at resume_pc
       RK_WAITSEMA,    // inside WaitSema: v0 = wait result, then resume_pc
       RK_SLEEP,       // inside SleepThread: v0 = thread id
       RK_ROTATE };    // inside RotateThreadReadyQueue: v0 = priority (a0)
static u32 g_resume_pc = 0;          // resume point of the code running now (syscall, loop check)
static volatile bool g_safe_due = false;  // a savestate action waits for the next safe point
static int g_resume_kind = RK_NONE;
static int g_override_depth = 0;     // inside a game syscall handler (SetSyscall): no resume points
static bool g_respawn = false;       // the scheduler replaces all thread fibers (resume test, restore)
static void safe_point(EEContext* c, u32 target);
static bool resume_test_due();

struct Sema {
    int id = 0;
    int count = 0, max_count = 0, init_count = 0;
    u32 attr = 0, option = 0;
    std::vector<int> waiters;
    bool used = false;
};

static const int MAX_THREADS = 256;
static const int MAX_SEMAS = 256;
static Thread g_threads[MAX_THREADS];
static Sema g_semas[MAX_SEMAS];
static Thread* g_cur = nullptr;       // running thread (nullptr = scheduler/idle)
static void* g_sched_fiber = nullptr;
static s64 g_seq = 0;
static std::vector<void*> g_dead_fibers;
static int g_dispatch_disabled = 0;

// interrupts
struct Handler { int id; u32 func; u32 arg; u32 gp; bool enabled; };
static std::vector<Handler> g_intc_handlers[16];
static std::vector<Handler> g_dmac_handlers[16];
static int g_handler_id = 1;
static bool g_ie = true;              // EIE
static bool g_in_handler = false;
static EEContext* g_irq_ctx = nullptr;
static constexpr u32 IRQ_STACK_TOP = 0x0007FF00;

// alarms
struct Alarm { int id; double when; u32 cb; u32 arg; u32 gp; u16 ticks; };
static std::vector<Alarm> g_alarms;
static int g_alarm_id = 1;

// vsync flag (SetVSyncFlag)
static u32 g_vsync_flag_ptr = 0, g_vsync_csr_ptr = 0;

// syscall overrides
static u32 g_syscall_override[256];

// heap
static u32 g_heap_start = 0, g_heap_end = 0;

extern bool g_watchdog;

static EEContext* alloc_ctx() {
    EEContext* c = (EEContext*)_aligned_malloc(sizeof(EEContext), 64);
    memset(c, 0, sizeof(EEContext));
    c->vf[0].f[3] = 1.0f;
    c->fcr31 = 0x01000001;
    return c;
}

EEContext* current_ctx() { return g_in_handler ? g_irq_ctx : (g_cur ? g_cur->ctx : nullptr); }
int current_thread_id() { return g_cur ? g_cur->id : 0; }

void notify_pending() { g_ee_irq_pending = 1; }

// ---------------------------------------------------------------------------
// Function dispatch
// ---------------------------------------------------------------------------
struct ModIndex {
    const EEModuleInfo* info;
    std::unordered_map<u32, const EEFuncEntry*> by_addr;
    const u8* image;  // reference image for overlay validation (nullptr for main)
    std::vector<const EEFuncEntry*> sorted;  // by start address (savestate resume lookup)
};
static std::vector<ModIndex> g_mods;
static ModIndex g_main_index;
static std::vector<EEFunc> g_main_flat;
static u32 g_main_base = 0, g_main_end = 0;
static std::unordered_map<u32, EEFunc> g_hle_funcs;

// overlay reference images loaded from disc (set by loader)
extern "C" const u8* overlay_image_for(const char* name, u32* size);

static bool module_matches(const ModIndex& m, const EEFuncEntry* e) {
    if (!m.image) return true;
    u32 off = e->start - m.info->base;
    u32 len = e->end - e->start;
    if (len > 256) len = 256;
    u8* p = mem::ee_ptr(e->start);
    return p && memcmp(p, m.image + off, len) == 0;
}

// battle running: most indirect calls into overlay code during the last frames went to the battle
// module (it stays in memory during field play, so presence alone says nothing)
static u32 g_disp_battle = 0, g_disp_other = 0;
static double g_last_flight = -100.0;  // emulated time of the last call into the flight module
static inline void count_dispatch(const ModIndex* m) {
    if (m->info->name[0] == 'b') g_disp_battle++;
    else {
        g_disp_other++;
        if (m->info->name[1] == 'l') g_last_flight = g_emu_time;
    }
}
// the flight module ran during the last two seconds (flight radio translation)
bool flight_running() { return g_emu_time - g_last_flight < 2.0; }
static double g_last_ipu = -100;  // emulated time of the last IPU activity (movie)
bool movie_running() { return g_emu_time - g_last_ipu < 1.0; }
bool battle_running();
static bool g_battle_now = false;  // last result of battle_loaded()
bool battle_running() { return g_battle_now; }
extern "C" int g_cinema;           // debug_hooks.cpp: cutscene camera active
static bool battle_loaded() {
    static int score = 0;  // > 0: battle
    if (g_disp_battle > g_disp_other) score = std::min(score + 1, 30);
    else if (g_disp_other > g_disp_battle) score = std::max(score - 1, -30);
    g_disp_battle = g_disp_other = 0;
    if (g_battle_now != (score > 0))
        log_write(LOG_KERNEL, "battle %s at %.1f s (FrameRate %d)", score > 0 ? "start" : "end", g_emu_time, mem::ram[0x1EBAD8]);
    g_battle_now = score > 0;
    return score > 0;
}

bool g_test_scene_skip_flag = false;  // --scene-skip-flag (test)
bool g_dispatch_log = false;  // --dispatch-log: indirect call targets per second -> dispatch_log.txt
static std::unordered_map<u32, u32> g_dhist;
static void dispatch_log_tick() {
    static FILE* f = nullptr;
    if (!f) f = fopen("dispatch_log.txt", "w");
    if (!f) return;
    fprintf(f, "%.1f", g_emu_time);
    for (auto& kv : g_dhist) fprintf(f, " %x:%u", kv.first, kv.second);
    fprintf(f, "\n");
    fflush(f);
    g_dhist.clear();
}

EEFunc lookup_func(u32 addr) {
    addr &= 0x1FFFFFFF;
    if (g_dispatch_log) g_dhist[addr]++;
    if (addr >= g_main_base && addr < g_main_end) {
        EEFunc f = g_main_flat[(addr - g_main_base) >> 2];
        if (f) return f;
    }
    // overlays: find a module containing addr whose code matches memory
    static const ModIndex* last_hit[4] = {};
    for (auto* h : last_hit) {
        if (!h) continue;
        auto it = h->by_addr.find(addr);
        if (it != h->by_addr.end() && module_matches(*h, it->second)) {
            count_dispatch(h);
            return it->second->fn;
        }
    }
    for (auto& m : g_mods) {
        if (addr < m.info->base || addr >= m.info->code_end) continue;
        auto it = m.by_addr.find(addr);
        if (it == m.by_addr.end()) continue;
        if (module_matches(m, it->second)) {
            memmove(&last_hit[1], &last_hit[0], sizeof(last_hit[0]) * 3);
            last_hit[0] = &m;
            count_dispatch(&m);
            return it->second->fn;
        }
    }
    return nullptr;
}

static void init_dispatch() {
    for (unsigned i = 0; i < g_ee_module_count; i++) {
        const EEModuleInfo* mi = &g_ee_modules[i];
        ModIndex m;
        m.info = mi;
        u32 sz = 0;
        bool is_main = strncmp(mi->name, "main", 4) == 0;
        m.image = is_main ? nullptr : overlay_image_for(mi->name, &sz);
        if (!is_main && !m.image) fatal("no reference image for overlay module %s", mi->name);
        for (unsigned k = 0; k < *mi->nfuncs; k++) m.by_addr[mi->funcs[k].start] = &mi->funcs[k];
        for (unsigned k = 0; k < *mi->nfuncs; k++) m.sorted.push_back(&mi->funcs[k]);
        std::sort(m.sorted.begin(), m.sorted.end(), [](const EEFuncEntry* a, const EEFuncEntry* b) { return a->start < b->start; });
        if (i == 0) {
            g_main_base = mi->base;
            g_main_end = mi->code_end;
            g_main_flat.assign((g_main_end - g_main_base) / 4, nullptr);
            for (unsigned k = 0; k < *mi->nfuncs; k++)
                g_main_flat[(mi->funcs[k].start - g_main_base) / 4] = mi->funcs[k].fn;
            g_main_index = std::move(m);
        } else {
            g_mods.push_back(std::move(m));
        }
    }
}

static void describe_addr(u32 addr) {
    for (unsigned i = 0; i < g_ee_module_count; i++) {
        const EEModuleInfo* mi = &g_ee_modules[i];
        if (addr < mi->base || addr >= mi->code_end) continue;
        const EEFuncEntry* best = nullptr;
        for (unsigned k = 0; k < *mi->nfuncs; k++) {
            const EEFuncEntry* e = &mi->funcs[k];
            if (e->start <= addr && addr < e->end) best = e;
        }
        if (best) log_write(LOG_KERNEL, "  module %s: inside function %08x-%08x", mi->name, best->start, best->end);
        else log_write(LOG_KERNEL, "  module %s: not inside any function", mi->name);
    }
}

}  // namespace kernel

using namespace kernel;

extern "C" void ee_call(EEContext* c, uint32_t addr) {
    EEFunc f = lookup_func(addr);
    if (!f) {
        log_write(LOG_KERNEL, "ee_call: no function at %08x (ra=%08x, cur_func=%08x)", addr, c->r[31].uw[0], c->cur_func);
        describe_addr(addr);
        fatal("Unresolved indirect call to %08x", addr);
    }
    f(c);
}

namespace kernel {

// ---------------------------------------------------------------------------
// Scheduling
// ---------------------------------------------------------------------------
static Thread* pick_ready() {
    Thread* best = nullptr;
    for (auto& t : g_threads) {
        if (t.status != THS_READY) continue;
        if (!best || t.prio < best->prio || (t.prio == best->prio && t.ready_seq < best->ready_seq)) best = &t;
    }
    return best;
}

static void make_ready(Thread* t, bool front = false) {
    t->status = THS_READY;
    t->ready_seq = front ? -(++g_seq) : ++g_seq;
}

static void switch_to_scheduler() {
    SwitchToFiber(g_sched_fiber);
}

// current thread blocks (status already set to WAIT etc.)
static void block_current() {
    if (!g_cur) fatal("block_current with no current thread");
    Thread* self = g_cur;
    self->resume_pc = g_resume_pc;
    self->resume_kind = g_override_depth ? RK_NONE : g_resume_kind;
    switch_to_scheduler();
    // resumed
    (void)self;
}

// preempt the current thread if a higher-priority thread is ready
static void maybe_preempt() {
    if (!g_cur || g_in_handler || g_dispatch_disabled) return;
    Thread* r = pick_ready();
    if (r && r->prio < g_cur->prio) {
        make_ready(g_cur, true);
        block_current();
    }
}

static void thread_exit_current();

static void WINAPI thread_fiber_entry(void* param) {
    Thread* t = (Thread*)param;
    EEContext* c = t->ctx;
    ee_call(c, t->func);
    // returned: ExitThread
    thread_exit_current();
}

static void setup_thread_ctx(Thread* t) {
    EEContext* c = t->ctx;
    memset(c, 0, sizeof(EEContext));
    c->vf[0].f[3] = 1.0f;
    c->fcr31 = 0x01000001;
    c->r[29].sd[0] = (s32)((t->stack + t->stack_size - 0x40) & ~15u);
    c->r[28].sd[0] = (s32)t->gp;
    c->r[4].sd[0] = (s32)t->arg;
    c->r[31].sd[0] = 0;
    c->thread_id = t->id;
    c->thread = t;
}

static void start_thread(Thread* t) {
    if (t->fiber) {
        // previous fiber is finished; schedule deletion
        g_dead_fibers.push_back(t->fiber);
        t->fiber = nullptr;
    }
    setup_thread_ctx(t);
    t->fiber = CreateFiberEx(64 * 1024, 4 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, thread_fiber_entry, t);
    if (!t->fiber) fatal("CreateFiber failed");
    t->prio = t->init_prio;
    t->wakeup_count = 0;
    t->wait_type = WAIT_NONE;
    t->resume_kind = RK_FRESH;
    make_ready(t);
}

static void remove_from_wait(Thread* t) {
    if (t->wait_type == WAIT_SEMA) {
        Sema& s = g_semas[t->wait_id];
        s.waiters.erase(std::remove(s.waiters.begin(), s.waiters.end(), t->id), s.waiters.end());
    }
    t->wait_type = WAIT_NONE;
}

static void thread_exit_current() {
    Thread* t = g_cur;
    LOGK("thread %d exits", t->id);
    t->status = THS_DORMANT;
    t->fiber_dead = true;
    switch_to_scheduler();
    fatal("dead thread resumed");
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
static void call_handler(const Handler& h, u32 a0, u32 a1, u32 a2) {
    EEContext* c = g_irq_ctx;
    c->r[4].sd[0] = (s32)a0;
    c->r[5].sd[0] = (s32)a1;
    c->r[6].sd[0] = (s32)a2;
    c->r[28].sd[0] = (s32)h.gp;
    c->r[29].sd[0] = (s32)IRQ_STACK_TOP;
    c->r[31].sd[0] = 0;
    ee_call(c, h.func);
}

static void deliver_interrupts() {
    if (g_in_handler || !g_ie) return;
    for (int guard = 0; guard < 8; guard++) {
        u32 pend_intc = intc_stat & intc_mask;
        u32 pend_dmac = (dmac_stat & 0x3FF) & ((dmac_stat >> 16) & 0x3FF);
        if (!pend_intc && !pend_dmac) break;
        g_in_handler = true;
        for (int cause = 0; cause < 15; cause++) {
            if (!(pend_intc & (1u << cause))) continue;
            intc_stat &= ~(1u << cause);
            for (auto& h : g_intc_handlers[cause])
                if (h.enabled) call_handler(h, cause, h.arg, 0);
        }
        for (int ch = 0; ch < 10; ch++) {
            if (!(pend_dmac & (1u << ch))) continue;
            dmac_stat &= ~(1u << ch);
            for (auto& h : g_dmac_handlers[ch])
                if (h.enabled) call_handler(h, ch, h.arg, 0);
        }
        g_in_handler = false;
    }
}

static void fire_alarms() {
    if (g_in_handler || !g_ie) return;
    for (size_t i = 0; i < g_alarms.size();) {
        if (g_alarms[i].when <= g_emu_time) {
            Alarm a = g_alarms[i];
            g_alarms.erase(g_alarms.begin() + i);
            g_in_handler = true;
            Handler h{a.id, a.cb, a.arg, a.gp, true};
            call_handler(h, a.id, a.ticks, a.arg);
            g_in_handler = false;
        } else {
            i++;
        }
    }
}

void raise_intc(int cause) {
    intc_stat |= 1u << cause;
    g_ee_irq_pending = 1;
}
void raise_dmac(int ch) {
    dmac_stat |= 1u << ch;
    g_ee_irq_pending = 1;
}
void intc_stat_write(u32 v) { intc_stat &= ~v; }
void intc_mask_write(u32 v) { intc_mask ^= v; g_ee_irq_pending = 1; }
void dmac_stat_write(u32 v) {
    dmac_stat &= ~(v & 0xFFFF);
    dmac_stat ^= (v & 0xFFFF0000);
    g_ee_irq_pending = 1;
}

std::atomic<bool> g_pause_req{false};  // settings window open: the game waits
std::atomic<bool> g_pause_represent{false};  // in-game menu open: keep presenting while paused
std::atomic<int> g_preview_vblanks{0};  // paused: run this many vertical blanks (a changed graphics option shows)

// --find-mdz (analysis): protect host pages of EE RAM holding compressed archive headers ("MDZ") and
// report the EE function that touches them first (locates the game's decompressor)
bool g_find_mdz = false;
static std::vector<std::pair<u8*, u32>> g_watch_pages;
static std::vector<u32> g_watch_seen;
static LONG CALLBACK watch_veh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    u8* a = (u8*)e->ExceptionRecord->ExceptionInformation[1];
    for (size_t i = 0; i < g_watch_pages.size(); i++) {
        u8* pg = g_watch_pages[i].first;
        if (a >= pg && a < pg + 4096) {
            Thread* t = g_cur;
            EEContext* c = g_in_handler ? g_irq_ctx : (t ? t->ctx : nullptr);
            log_write(LOG_KERNEL, "find-mdz: page %08x accessed at +%03x by EE func %08x (ra %08x)", g_watch_pages[i].second,
                      (unsigned)(a - pg), c ? c->cur_func : 0, c ? c->r[31].uw[0] : 0);
            DWORD old;
            VirtualProtect(pg, 4096, PAGE_READWRITE, &old);
            g_watch_pages.erase(g_watch_pages.begin() + (long)i);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
static void find_mdz_scan() {
    static bool init = false;
    if (!init) { AddVectoredExceptionHandler(1, watch_veh); init = true; }
    for (u32 off = 0; off < 32u * 1024 * 1024; off += 16) {
        bool prot = false;
        for (auto& w : g_watch_pages)
            if ((off & ~4095u) == w.second) prot = true;
        if (prot) { off = (off & ~4095u) + 4096 - 16; continue; }
        const u8* p = g_ee_ram + off;
        if (p[2] != 'M' || p[3] != 'D' || p[4] != 'Z' || p[5] != 0) continue;
        u32 page = off & ~4095u;
        if (std::find(g_watch_seen.begin(), g_watch_seen.end(), page) != g_watch_seen.end()) continue;
        g_watch_seen.push_back(page);
        DWORD old;
        VirtualProtect(g_ee_ram + page, 4096, PAGE_NOACCESS, &old);
        g_watch_pages.push_back({g_ee_ram + page, page});
        log_write(LOG_KERNEL, "find-mdz: header at %08x, watching", off);
        off = page + 4096 - 16;  // the rest of the page is protected now
    }
}

// --watch-text=SEC (analysis): find the code reading a message text. At SEC the first copy of a
// byte pattern (script text "Recover\nSave") in EE RAM is located and its page protected; every
// access to the pattern bytes is logged with the host code address (map file: generated function)
// and the EE return address. Single-stepping re-protects the page after each access.
double g_watch_text_at = 0;
double g_watch_mem_at = 0;  // --watch-mem (analysis)
u32 g_watch_mem_ptr = 0, g_watch_mem_off = 0, g_watch_mem_len = 0;
std::string g_watch_mem_find;
void watch_range(u32 lo, u32 hi);
static u8* g_wt_page = nullptr;
static u32 g_wt_bytes = 4096;  // watched pages (a multiple of 4096 from g_wt_page)
static u32 g_wt_lo = 0, g_wt_hi = 0;
static int g_wt_logged = 0;
static LONG CALLBACK watch_text_veh(EXCEPTION_POINTERS* e) {
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && g_wt_page) {
        DWORD old;
        VirtualProtect(g_wt_page, g_wt_bytes, PAGE_NOACCESS, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || !g_wt_page) return EXCEPTION_CONTINUE_SEARCH;
    u8* a = (u8*)e->ExceptionRecord->ExceptionInformation[1];
    if (a < g_wt_page || a >= g_wt_page + g_wt_bytes) return EXCEPTION_CONTINUE_SEARCH;
    u32 ee = (u32)(a - g_ee_ram);
    static std::set<u64> seen_rip;  // each reading place once
    if (ee >= g_wt_lo && ee < g_wt_hi && g_wt_logged < 400 && seen_rip.insert(e->ContextRecord->Rip).second) {
        g_wt_logged++;
        Thread* t = g_cur;
        EEContext* c = g_in_handler ? g_irq_ctx : (t ? t->ctx : nullptr);
        log_write(LOG_KERNEL, "watch-text: %s %08x by host rva %llx (ee ra %08x a0 %08x a1 %08x a2 %08x)",
                  e->ExceptionRecord->ExceptionInformation[0] ? "write" : "read", ee,
                  (unsigned long long)(e->ContextRecord->Rip - (u64)GetModuleHandleW(nullptr)),
                  c ? c->r[31].uw[0] : 0, c ? c->r[4].uw[0] : 0, c ? c->r[5].uw[0] : 0, c ? c->r[6].uw[0] : 0);
    }
    DWORD old;
    VirtualProtect(g_wt_page, g_wt_bytes, PAGE_READWRITE, &old);
    e->ContextRecord->EFlags |= 0x100;  // trap after the instruction: re-protect
    return EXCEPTION_CONTINUE_EXECUTION;
}
// --watch-msg (analysis): the text hook watches each message it hands to a window
void watch_range(u32 lo, u32 hi) {
    static bool veh = false;
    DWORD old;
    if (g_wt_page) VirtualProtect(g_wt_page, g_wt_bytes, PAGE_READWRITE, &old);
    if (!veh) { AddVectoredExceptionHandler(1, watch_text_veh); veh = true; }
    g_wt_lo = lo;
    g_wt_hi = hi;
    g_wt_page = g_ee_ram + (lo & ~4095u);
    g_wt_bytes = ((hi + 4095) & ~4095u) - (lo & ~4095u);
    VirtualProtect(g_wt_page, g_wt_bytes, PAGE_NOACCESS, &old);
}
static void watch_text_start() {
    static const u8 pat[] = {0x72, 0x85, 0x83, 0x8f, 0x96, 0x85, 0x92, 0x08, 0x73, 0x81, 0x96, 0x85};
    for (u32 off = 0; off + sizeof(pat) < 32u * 1024 * 1024; off++) {
        if (g_ee_ram[off] != pat[0] || memcmp(g_ee_ram + off, pat, sizeof(pat)) != 0) continue;
        log_write(LOG_KERNEL, "watch-text: pattern at %08x", off);
        g_wt_lo = off;
        g_wt_hi = off + 24;
        g_wt_page = g_ee_ram + (off & ~4095u);
        AddVectoredExceptionHandler(1, watch_text_veh);
        DWORD old;
        VirtualProtect(g_wt_page, 4096, PAGE_NOACCESS, &old);
        return;
    }
    log_write(LOG_KERNEL, "watch-text: pattern not found");
}

// Advance emulated time to host time; raise timed events.
static void update_time() {
    if (g_pause_req.load(std::memory_order_relaxed) && g_preview_vblanks.load(std::memory_order_relaxed) <= 0) {
        // pause: no emulated time passes while waiting
        double t0 = host_time_sec();
        double last = 0;
        while (g_pause_req.load(std::memory_order_acquire) && g_preview_vblanks.load(std::memory_order_relaxed) <= 0) {
            Sleep(5);
            // in-game menu: the GPU renderer shows the last picture again with the menu on top
            if (g_pause_represent.load(std::memory_order_relaxed) && host_time_sec() - last >= 0.033) {
                last = host_time_sec();
                static int logged = 0;
                if (logged++ < 3) log_write(LOG_KERNEL, "pause: re-present request");
                gsp::represent();
            }
        }
        g_time_base += host_time_sec() - t0;
    }
    {
        // fast forward (Tab held, frame_limit 0): emulated time runs 4 times as fast; the time base
        // moves so the emulated clock continues where it was
        // cutscene skip: time runs 100 times as fast; the game advances a fixed step per frame and
        // runs as fast as the emulation can (nothing is drawn or mixed meanwhile)
        static int limit = 1;
        int now_limit = platform::skip_active() ? 2 : platform::config().frame_limit;
        if (now_limit != limit) {
            limit = now_limit;
            double h = host_time_sec();
            g_speed = limit == 2 ? 100.0 : limit ? 1.0 : 4.0;
            g_time_base = h - g_emu_time / g_speed;
            log_write(LOG_KERNEL, "game speed x%.0f (%s)", g_speed, limit == 2 ? "cutscene skip" : limit ? "normal" : "fast forward");
        }
    }
    double host = host_time_sec() - g_time_base;
    double t = host * g_speed;
    if (t < g_emu_time) t = g_emu_time;
    g_emu_time = t;
    while (g_emu_time >= g_next_vblank_s) {
        static u32 nvb = 0;
        if (g_find_mdz && (++nvb % 20) == 0) find_mdz_scan();
        if (g_watch_mem_at > 0 && host_time_sec() >= g_watch_mem_at) {
            // --watch-mem=SEC:[*PTR+]OFF:LEN
            g_watch_mem_at = 0;
            u32 a = g_watch_mem_off;
            if (g_watch_mem_ptr) a += mem::rd32(g_watch_mem_ptr);
            if (!g_watch_mem_find.empty()) {  // "find" form: the first copy of an ASCII text in EE RAM
                const std::string& t = g_watch_mem_find;
                a = 0;
                for (u32 off = 0x100000; off + t.size() < 32u * 1024 * 1024; off++)
                    if (g_ee_ram[off] == (u8)t[0] && memcmp(g_ee_ram + off, t.data(), t.size()) == 0) { a = off; break; }
                if (!a) log_write(LOG_KERNEL, "watch-mem: text not found");
            }
            log_write(LOG_KERNEL, "watch-mem: %08x..%08x", a, a + g_watch_mem_len);
            watch_range(a, a + g_watch_mem_len);
        }
        if (g_watch_text_at > 0 && host_time_sec() >= g_watch_text_at) {
            g_watch_text_at = 0;
            watch_text_start();
        }
        {
            // profiler: the game missed this vertical blank (no flip since the previous one)
            static u64 last_flips = 0;
            u64 f = gs::flip_count();
            g_frame_late = (f == last_flips);
            last_flips = f;
        }
        {
            // debug: movie player state (5 = stopped by the libmpeg error callback ee_0012E050)
            static u32 last_mstate = 0xFFFFFFFFu;
            static int nlog = 0;
            u32 v = mem::rd32(0x212F00);
            if (v != last_mstate && nlog < 200) { nlog++; log_write(LOG_KERNEL, "movie state %u -> %u", last_mstate, v); }
            last_mstate = v;
        }
        if (g_dispatch_log) {
            static int n = 0;
            if (++n % 30 == 0) dispatch_log_tick();
        }
        if (g_test_scene_skip_flag) {  // test: native Scene Skip on (game flag 422)
            u32 ctx = mem::rd32(0x219520);
            if (ctx && ctx < 0x2000000) {
                u32 arr = mem::rd32(ctx + 4);
                if (arr && arr < 0x2000000) mem::ram[arr + 422 / 8] |= 1 << (422 % 8);
            }
        }
        {
            static u64 ipu_last = 0;
            u64 ic = ipu::command_count();
            if (ic != ipu_last) { ipu_last = ic; g_last_ipu = g_emu_time; }
        }
        if (platform::skip_active()) {
            // a field cutscene skip ends with the cutscene camera (two seconds without it), a flight
            // skip when the flight module is left; both at a battle, a movie or after three minutes
            // (a choice ends it in the text hook)
            static int off = 0, session = 0;
            static bool flight = false;
            static double t0 = 0;
            if (session != platform::skip_session()) {
                session = platform::skip_session();
                off = 0;
                flight = flight_running();
                t0 = host_time_sec();
            }
            off = (flight ? flight_running() : g_cinema != 0) ? 0 : off + 1;
            if (off > 120) platform::skip_stop(flight ? "flight over" : "cutscene over");
            else if (g_battle_now) platform::skip_stop("battle");
            else if (movie_running()) platform::skip_stop("movie");
            else if (host_time_sec() - t0 > 180) platform::skip_stop("time limit");
        }
        if (resume_test_due() || ss::pending()) g_safe_due = true;
        if (g_preview_vblanks.load(std::memory_order_relaxed) > 0) g_preview_vblanks.fetch_sub(1);
        gs::on_vblank_start();
        frame_rate_option();
        ::texthook::font_tick();
        raise_intc(INTC_VBLANK_S);
        if (g_vsync_flag_ptr) {
            mem::wr32(g_vsync_flag_ptr, 1);
            if (g_vsync_csr_ptr) mem::wr32(g_vsync_csr_ptr, (u32)gs::read_csr());
        }
        iop::on_vblank_start();
        g_next_vblank_s += FIELD_PERIOD;
        // if we fell far behind, skip
        if (g_emu_time - g_next_vblank_s > 0.25) {
            g_next_vblank_s = g_emu_time + FIELD_PERIOD;
            g_next_vblank_e = g_next_vblank_s + VBLANK_LEN;
        }
    }
    while (g_emu_time >= g_next_vblank_e) {
        gs::on_vblank_end();
        raise_intc(INTC_VBLANK_E);
        iop::on_vblank_end();
        g_next_vblank_e += FIELD_PERIOD;
        if (g_next_vblank_e < g_next_vblank_s - FIELD_PERIOD) g_next_vblank_e = g_next_vblank_s + VBLANK_LEN;
    }
    hw::timers_update();
}

static double next_event_time() {
    double t = std::min(g_next_vblank_s, g_next_vblank_e);
    for (auto& a : g_alarms) t = std::min(t, a.when);
    t = std::min(t, hw::timers_next_event());
    return t;
}

static void process_events() {
    gsp::poll_events();
    update_time();
    iop::run_pending();
    deliver_interrupts();
    fire_alarms();
}

void poll_time() {
    process_events();
}

u64 g_ee_idle_ticks = 0;
}  // namespace kernel
void* gs_worker0_handle();
namespace gs { std::string raster_debug_state(); }
namespace kernel {  // time (rdtsc) the emulation thread spent sleeping because the game was idle

void wait_idle_hint() {
    // A thread is busy-waiting on hardware state. Sleep a little to let time progress.
    double nt = next_event_time();
    double now = (host_time_sec() - g_time_base) * g_speed;
    if (nt > now) {
        double dt = (nt - now) / g_speed;  // host seconds (fast forward and cutscene skip run faster)
        u64 t0 = __rdtsc();
        if (dt > 0.002) gsp::idle_sleep();  // an interrupt from the GS pipeline ends it early
        else SwitchToThread();
        g_ee_idle_ticks += __rdtsc() - t0;
    }
    process_events();
    // a higher-priority thread made ready meanwhile runs at the next loop check (a thread
    // switch in the middle of a register access would have no savestate resume point)
    g_ee_irq_pending = 1;
}

}  // namespace kernel

extern "C" void ee_irq_check(EEContext* c) {
    (void)c;
    g_ee_irq_pending = 0;
    if (g_in_handler) { update_time(); return; }
    process_events();
    maybe_preempt();
}

// loop check of the recompiled code: a thread preempted here continues at the branch target
extern "C" void ee_irq_check_at(EEContext* c, uint32_t target) {
    u32 opc = g_resume_pc;
    int okind = g_resume_kind;
    g_resume_pc = target;
    g_resume_kind = RK_PLAIN;
    ee_irq_check(c);
    if (g_safe_due && g_cur && !g_in_handler && !g_override_depth) safe_point(c, target);
    g_resume_pc = opc;
    g_resume_kind = okind;
}

namespace kernel {

// ---------------------------------------------------------------------------
// Syscalls
// ---------------------------------------------------------------------------
static inline u32 A0(EEContext* c) { return c->r[4].uw[0]; }
static inline u32 A1(EEContext* c) { return c->r[5].uw[0]; }
static inline u32 A2(EEContext* c) { return c->r[6].uw[0]; }
static inline u32 A3(EEContext* c) { return c->r[7].uw[0]; }
static inline u32 T0(EEContext* c) { return c->r[8].uw[0]; }
static inline void RET(EEContext* c, s32 v) { c->r[2].sd[0] = v; }

static Thread* get_thread(int id) {
    if (id == 0) return g_cur;
    if (id < 0 || id >= MAX_THREADS || g_threads[id].id == 0) return nullptr;
    return &g_threads[id];
}

static int sys_create_thread(u32 param) {
    int id = -1;
    for (int i = 1; i < MAX_THREADS; i++)
        if (g_threads[i].id == 0) { id = i; break; }
    if (id < 0) return -1;
    Thread& t = g_threads[id];
    t = Thread();
    t.id = id;
    t.status = THS_DORMANT;
    t.func = mem::rd32(param + 4);
    t.stack = mem::rd32(param + 8);
    t.stack_size = mem::rd32(param + 12);
    t.gp = mem::rd32(param + 16);
    t.init_prio = t.prio = (int)mem::rd32(param + 20);
    t.attr = mem::rd32(param + 28);
    t.option = mem::rd32(param + 32);
    t.ctx = alloc_ctx();
    LOGK("CreateThread id=%d func=%08x stack=%08x size=%x gp=%08x prio=%d", id, t.func, t.stack, t.stack_size, t.gp, t.prio);
    return id;
}

static void wake_thread(Thread* t, int result) {
    remove_from_wait(t);
    t->wait_result = result;
    if (t->status == THS_WAITSUSPEND) t->status = THS_SUSPEND;
    else make_ready(t);
}

static int sema_signal(int sid) {
    if (sid <= 0 || sid >= MAX_SEMAS || !g_semas[sid].used) return -1;
    Sema& s = g_semas[sid];
    if (!s.waiters.empty()) {
        int tid = s.waiters.front();
        s.waiters.erase(s.waiters.begin());
        Thread* t = &g_threads[tid];
        t->wait_type = WAIT_NONE;
        t->wait_result = sid;
        if (t->status == THS_WAITSUSPEND) t->status = THS_SUSPEND;
        else make_ready(t);
    } else {
        if (s.count < s.max_count || s.max_count <= 0) s.count++;
    }
    return sid;
}

static const char* syscall_name(int n);

extern "C" void ee_syscall_impl(EEContext* c, int num);

}  // namespace kernel

extern "C" void ee_syscall(EEContext* c, uint32_t pc) {
    int num = (int)c->r[3].sw[0];
    u32 opc = kernel::g_resume_pc;
    int okind = kernel::g_resume_kind;
    kernel::g_resume_pc = pc + 4;  // a thread blocking in the syscall continues after it
    kernel::g_resume_kind = kernel::RK_PLAIN;
    kernel::ee_syscall_impl(c, num);
    kernel::g_resume_pc = opc;
    kernel::g_resume_kind = okind;
}

namespace kernel {

static void sys_exit(int code) {
    log_write(LOG_KERNEL, "Game called Exit(%d)", code);
    ExitProcess(0);
}

bool g_trace_syscalls = false;
bool g_watchdog = false;

void ee_syscall_impl(EEContext* c, int num) {
    int n = num < 0 ? -num : num;
    if (g_trace_syscalls)
        log_write(LOG_KERNEL, "syscall %d (0x%x) a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x thr=%d", num, n, A0(c), A1(c), A2(c),
                  A3(c), c->r[31].uw[0], g_cur ? g_cur->id : -1);
    if (n < 256 && g_syscall_override[n]) {
        g_override_depth++;
        ee_call(c, g_syscall_override[n]);
        g_override_depth--;
        return;
    }
    switch (n) {
    case 0x01:  // ResetEE
        RET(c, 0);
        break;
    case 0x02:  // SetGsCrt
        gs::set_crt(A0(c), A1(c), A2(c));
        RET(c, 0);
        break;
    case 0x04:  // Exit
        sys_exit((int)A0(c));
        break;
    case 0x06: {  // LoadExecPS2
        std::string fn = mem::read_cstr(A0(c));
        log_write(LOG_KERNEL, "LoadExecPS2(%s)", fn.c_str());
        sys_exit(0);
        break;
    }
    case 0x07:  // ExecPS2
        log_write(LOG_KERNEL, "ExecPS2(%08x)", A0(c));
        sys_exit(0);
        break;
    case 0x10:    // AddIntcHandler
    case 0x12: {  // AddDmacHandler
        int cause = (int)A0(c);
        Handler h{g_handler_id++, A1(c), A3(c), c->r[28].uw[0], true};
        auto& list = (n == 0x10) ? g_intc_handlers[cause & 15] : g_dmac_handlers[cause & 15];
        if ((s32)A2(c) < 0) list.push_back(h);   // next=-1: append
        else list.insert(list.begin(), h);
        LOGK("Add%sHandler(%d, %08x, next=%d, arg=%08x) -> %d", n == 0x10 ? "Intc" : "Dmac", cause, h.func, (s32)A2(c), h.arg, h.id);
        RET(c, h.id);
        break;
    }
    case 0x11:
    case 0x13: {  // Remove handler
        int cause = (int)A0(c), hid = (int)A1(c);
        auto& list = (n == 0x11) ? g_intc_handlers[cause & 15] : g_dmac_handlers[cause & 15];
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Handler& h) { return h.id == hid; }), list.end());
        RET(c, 0);
        break;
    }
    case 0x14:
    case 0x1A:  // EnableIntc
        if (!(intc_mask & (1u << (A0(c) & 15)))) intc_mask |= 1u << (A0(c) & 15);
        g_ee_irq_pending = 1;
        RET(c, 1);
        break;
    case 0x15:
    case 0x1B:  // DisableIntc
        intc_mask &= ~(1u << (A0(c) & 15));
        RET(c, 1);
        break;
    case 0x16:
    case 0x1C:  // EnableDmac
        dmac_stat |= 1u << (16 + (A0(c) & 15));
        g_ee_irq_pending = 1;
        RET(c, 1);
        break;
    case 0x17:
    case 0x1D:  // DisableDmac
        dmac_stat &= ~(1u << (16 + (A0(c) & 15)));
        RET(c, 1);
        break;
    case 0x18:
    case 0x1E: {  // SetAlarm(ticks, cb, arg)
        Alarm a;
        a.id = g_alarm_id++;
        a.ticks = (u16)A0(c);
        a.when = g_emu_time + (double)(A0(c) & 0xFFFF) / HSYNC_RATE;
        a.cb = A1(c);
        a.arg = A2(c);
        a.gp = c->r[28].uw[0];
        g_alarms.push_back(a);
        RET(c, a.id);
        break;
    }
    case 0x19:
    case 0x1F: {  // ReleaseAlarm
        int id = (int)A0(c);
        auto it = std::find_if(g_alarms.begin(), g_alarms.end(), [&](const Alarm& a) { return a.id == id; });
        if (it != g_alarms.end()) { g_alarms.erase(it); RET(c, id); }
        else RET(c, -1);
        break;
    }
    case 0x20:  // CreateThread
        RET(c, sys_create_thread(A0(c)));
        break;
    case 0x21: {  // DeleteThread
        Thread* t = get_thread((int)A0(c));
        if (!t || t == g_cur || t->status != THS_DORMANT) { RET(c, -1); break; }
        if (t->fiber) g_dead_fibers.push_back(t->fiber);
        t->fiber = nullptr;
        _aligned_free(t->ctx);
        *t = Thread();
        RET(c, (s32)A0(c));
        break;
    }
    case 0x22: {  // StartThread(id, arg)
        Thread* t = get_thread((int)A0(c));
        if (!t || t->status != THS_DORMANT) { RET(c, -1); break; }
        t->arg = A1(c);
        LOGK("StartThread(%d, %08x)", t->id, t->arg);
        start_thread(t);
        RET(c, t->id);
        maybe_preempt();
        break;
    }
    case 0x23:  // ExitThread
        thread_exit_current();
        break;
    case 0x24: {  // ExitDeleteThread
        Thread* t = g_cur;
        t->status = THS_DORMANT;
        t->fiber_dead = true;
        t->id = 0;  // freed; fiber/ctx leak is small
        switch_to_scheduler();
        break;
    }
    case 0x25:
    case 0x26: {  // TerminateThread
        Thread* t = get_thread((int)A0(c));
        if (!t || t == g_cur) { RET(c, -1); break; }
        remove_from_wait(t);
        t->status = THS_DORMANT;
        if (t->fiber) { g_dead_fibers.push_back(t->fiber); t->fiber = nullptr; }
        RET(c, t->id);
        break;
    }
    case 0x27:  // DisableDispatchThread
        g_dispatch_disabled = 1;
        RET(c, 0);
        break;
    case 0x28:  // EnableDispatchThread
        g_dispatch_disabled = 0;
        RET(c, 0);
        maybe_preempt();
        break;
    case 0x29:
    case 0x2A: {  // ChangeThreadPriority
        Thread* t = get_thread((int)A0(c));
        if (!t) { RET(c, -1); break; }
        int old = t->prio;
        t->prio = (int)A1(c);
        RET(c, old);
        if (num > 0) {
            if (t == g_cur) maybe_preempt();
            else if (t->status == THS_READY) maybe_preempt();
        }
        break;
    }
    case 0x2B:
    case 0x2C: {  // RotateThreadReadyQueue(prio)
        int prio = (int)A0(c);
        if (g_cur && g_cur->prio == prio && num > 0 && !g_in_handler) {
            // yield to other threads at same priority
            Thread* other = nullptr;
            for (auto& t : g_threads)
                if (t.status == THS_READY && t.prio == prio) { other = &t; break; }
            if (other) {
                make_ready(g_cur, false);
                g_resume_kind = RK_ROTATE;
                block_current();
            }
        } else {
            // move first ready thread of that prio to the end
            Thread* first = nullptr;
            for (auto& t : g_threads)
                if (t.status == THS_READY && t.prio == prio && (!first || t.ready_seq < first->ready_seq)) first = &t;
            if (first) first->ready_seq = ++g_seq;
        }
        RET(c, prio);
        break;
    }
    case 0x2D:
    case 0x2E: {  // ReleaseWaitThread
        Thread* t = get_thread((int)A0(c));
        if (!t || (t->status != THS_WAIT && t->status != THS_WAITSUSPEND)) { RET(c, -1); break; }
        wake_thread(t, -1);
        RET(c, t->id);
        if (num > 0) maybe_preempt();
        break;
    }
    case 0x2F:  // GetThreadId
        RET(c, g_cur ? g_cur->id : 0);
        break;
    case 0x30:
    case 0x31: {  // ReferThreadStatus(id, status*)
        Thread* t = get_thread((int)A0(c));
        if (!t) { RET(c, -1); break; }
        u32 p = A1(c);
        if (p) {
            int st = t->status;
            if (t == g_cur) st = THS_RUN;
            mem::wr32(p + 0, st);
            mem::wr32(p + 4, t->func);
            mem::wr32(p + 8, t->stack);
            mem::wr32(p + 12, t->stack_size);
            mem::wr32(p + 16, t->gp);
            mem::wr32(p + 20, t->init_prio);
            mem::wr32(p + 24, t->prio);
            mem::wr32(p + 28, t->attr);
            mem::wr32(p + 32, t->option);
            mem::wr32(p + 36, t->wait_type);
            mem::wr32(p + 40, t->wait_id);
            mem::wr32(p + 44, t->wakeup_count);
        }
        RET(c, t->status);
        break;
    }
    case 0x32: {  // SleepThread
        Thread* t = g_cur;
        if (t->wakeup_count > 0) { t->wakeup_count--; RET(c, t->id); break; }
        t->status = THS_WAIT;
        t->wait_type = WAIT_SLEEP;
        g_resume_kind = RK_SLEEP;
        block_current();
        RET(c, t->id);
        break;
    }
    case 0x33:
    case 0x34: {  // WakeupThread
        Thread* t = get_thread((int)A0(c));
        if (!t || t == g_cur) { RET(c, -1); break; }
        if ((t->status == THS_WAIT || t->status == THS_WAITSUSPEND) && t->wait_type == WAIT_SLEEP) wake_thread(t, 0);
        else t->wakeup_count++;
        RET(c, t->id);
        if (num > 0) maybe_preempt();
        break;
    }
    case 0x35:
    case 0x36: {  // CancelWakeupThread
        Thread* t = get_thread((int)A0(c));
        if (!t) { RET(c, -1); break; }
        int n0 = t->wakeup_count;
        t->wakeup_count = 0;
        RET(c, n0);
        break;
    }
    case 0x37:
    case 0x38: {  // SuspendThread
        Thread* t = get_thread((int)A0(c));
        if (!t || t == g_cur) { RET(c, -1); break; }
        if (t->status == THS_READY) t->status = THS_SUSPEND;
        else if (t->status == THS_WAIT) t->status = THS_WAITSUSPEND;
        RET(c, t->id);
        break;
    }
    case 0x39:
    case 0x3A: {  // ResumeThread
        Thread* t = get_thread((int)A0(c));
        if (!t) { RET(c, -1); break; }
        if (t->status == THS_SUSPEND) make_ready(t);
        else if (t->status == THS_WAITSUSPEND) t->status = THS_WAIT;
        RET(c, t->id);
        if (num > 0) maybe_preempt();
        break;
    }
    case 0x3C: {  // SetupThread(gp, stack, stack_size, args, root)
        u32 gp = A0(c), stack = A1(c), size = A2(c), args = A3(c);
        u32 sp = (stack == 0xFFFFFFFF) ? (mem::RAM_SIZE - size) : stack;
        sp = sp + size;
        Thread* t = g_cur;
        t->stack = sp - size;
        t->stack_size = size;
        t->gp = gp;
        // fill argument block: argc, argv[...], strings
        if (args) {
            const char* path = "cdrom0:\\SLUS_213.34;1";
            u32 strp = args + 4 + 16 * 4;
            mem::wr32(args, 1);
            mem::wr32(args + 4, strp);
            mem::copy_to_ee(strp, path, strlen(path) + 1);
        }
        c->r[28].sd[0] = (s32)gp;
        RET(c, (s32)(sp - 0x40));
        LOGK("SetupThread gp=%08x stack=%08x size=%x -> sp=%08x", gp, stack, size, sp);
        break;
    }
    case 0x3D: {  // SetupHeap(start, size)
        g_heap_start = A0(c);
        u32 size = A1(c);
        g_heap_end = (size == 0xFFFFFFFF) ? (g_cur ? g_cur->stack : mem::RAM_SIZE) : g_heap_start + size;
        LOGK("SetupHeap(%08x, %x) -> end %08x", g_heap_start, size, g_heap_end);
        RET(c, (s32)g_heap_start);
        break;
    }
    case 0x3E:  // EndOfHeap
        RET(c, (s32)g_heap_end);
        break;
    case 0x40: {  // CreateSema(param)
        u32 p = A0(c);
        int id = -1;
        for (int i = 1; i < MAX_SEMAS; i++)
            if (!g_semas[i].used) { id = i; break; }
        if (id < 0) { RET(c, -1); break; }
        Sema& s = g_semas[id];
        s = Sema();
        s.used = true;
        s.id = id;
        s.max_count = (int)mem::rd32(p + 4);
        s.init_count = s.count = (int)mem::rd32(p + 8);
        s.attr = mem::rd32(p + 16);
        s.option = mem::rd32(p + 20);
        LOGK("CreateSema -> %d (init %d max %d)", id, s.count, s.max_count);
        RET(c, id);
        break;
    }
    case 0x41: {  // DeleteSema
        int sid = (int)A0(c);
        if (sid <= 0 || sid >= MAX_SEMAS || !g_semas[sid].used) { RET(c, -1); break; }
        Sema& s = g_semas[sid];
        for (int tid : s.waiters) {
            Thread* t = &g_threads[tid];
            t->wait_type = WAIT_NONE;
            t->wait_result = -1;
            make_ready(t);
        }
        s = Sema();
        RET(c, sid);
        maybe_preempt();
        break;
    }
    case 0x42:
    case 0x43: {  // SignalSema / iSignalSema
        int r = sema_signal((int)A0(c));
        RET(c, r);
        if (num > 0) maybe_preempt();
        break;
    }
    case 0x44: {  // WaitSema
        int sid = (int)A0(c);
        if (sid <= 0 || sid >= MAX_SEMAS || !g_semas[sid].used) { RET(c, -1); break; }
        Sema& s = g_semas[sid];
        if (s.count > 0) { s.count--; RET(c, sid); break; }
        if (g_in_handler || !g_cur) { RET(c, -1); break; }
        Thread* t = g_cur;
        t->status = THS_WAIT;
        t->wait_type = WAIT_SEMA;
        t->wait_id = sid;
        s.waiters.push_back(t->id);
        g_resume_kind = RK_WAITSEMA;
        block_current();
        RET(c, t->wait_result);
        break;
    }
    case 0x45:
    case 0x46: {  // PollSema
        int sid = (int)A0(c);
        if (sid <= 0 || sid >= MAX_SEMAS || !g_semas[sid].used) { RET(c, -1); break; }
        Sema& s = g_semas[sid];
        if (s.count > 0) { s.count--; RET(c, sid); }
        else RET(c, -1);
        break;
    }
    case 0x47:
    case 0x48: {  // ReferSemaStatus
        int sid = (int)A0(c);
        if (sid <= 0 || sid >= MAX_SEMAS || !g_semas[sid].used) { RET(c, -1); break; }
        Sema& s = g_semas[sid];
        u32 p = A1(c);
        mem::wr32(p + 0, s.count);
        mem::wr32(p + 4, s.max_count);
        mem::wr32(p + 8, s.init_count);
        mem::wr32(p + 12, (u32)s.waiters.size());
        mem::wr32(p + 16, s.attr);
        mem::wr32(p + 20, s.option);
        RET(c, sid);
        break;
    }
    case 0x4A:  // SetOsdConfigParam
        RET(c, 0);
        break;
    case 0x4B: {  // GetOsdConfigParam(ptr)
        // screenType 4:3 (0), videoOutput RGB, language English (1)
        u32 v = (1u << 16);   // language field at bits 16..20
        mem::wr32(A0(c), v);
        RET(c, 0);
        break;
    }
    case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        RET(c, 0);
        break;
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: case 0x61: case 0x62: case 0x63: {
        // Enable/DisableIntcHandler, Enable/DisableDmacHandler (cause, hid)
        bool intc = (n <= 0x5F);
        bool enable = intc ? (n == 0x5C || n == 0x5D) : (n == 0x60 || n == 0x61);
        auto& list = intc ? g_intc_handlers[A0(c) & 15] : g_dmac_handlers[A0(c) & 15];
        for (auto& h : list)
            if (h.id == (int)A1(c)) h.enabled = enable;
        RET(c, 0);
        break;
    }
    case 0x64:
    case 0x68:  // FlushCache
        RET(c, 0);
        break;
    case 0x70:  // GsGetIMR
        RET(c, (s32)gs::get_imr());
        break;
    case 0x71:  // GsPutIMR
        gs::put_imr(A0(c));
        RET(c, 0);
        break;
    case 0x73:  // SetVSyncFlag
        g_vsync_flag_ptr = A0(c);
        g_vsync_csr_ptr = A1(c);
        RET(c, 0);
        break;
    case 0x74:  // SetSyscall(num, addr)
        LOGK("SetSyscall(%d, %08x)", (int)A0(c), A1(c));
        if (A0(c) < 256) g_syscall_override[A0(c)] = A1(c);
        RET(c, 0);
        break;
    case 0x76:  // SifDmaStat
        RET(c, sif::dma_stat((int)A0(c)));
        break;
    case 0x77:  // SifSetDma(trans, n)
        RET(c, sif::set_dma(A0(c), (int)A1(c)));
        break;
    case 0x78:  // SifSetDChain
        sif::set_dchain();
        RET(c, 0);
        break;
    case 0x79:  // SifSetReg
        RET(c, (s32)sif::set_reg(A0(c), A1(c)));
        break;
    case 0x7A:  // SifGetReg
        RET(c, (s32)sif::get_reg(A0(c)));
        break;
    case 0x7C:  // Deci2Call
        RET(c, -1);
        break;
    case 0x7E:  // MachineType
        RET(c, 0);
        break;
    case 0x7F:  // GetMemorySize
        RET(c, 32 * 1024 * 1024);
        break;
    default:
        log_write(LOG_KERNEL, "Unimplemented syscall %d (0x%x) a0=%08x a1=%08x", num, n, A0(c), A1(c));
        RET(c, 0);
        break;
    }
    // syscalls are safe points for interrupt delivery
    if (!g_in_handler && g_ee_irq_pending) ee_irq_check(c);
}


// ---------------------------------------------------------------------------
// Savestate resume: a restored thread has no native call stack. It continues at the MIPS level:
// the recompiled body containing its resume point is entered there (g_ee_resume, see
// ee_recomp.h); when that body returns (jr ra), the thread continues at ra in the caller's body,
// and so on down the MIPS call chain, which lives in EE RAM like everything else.
// ---------------------------------------------------------------------------
}  // namespace kernel
extern "C" uint32_t g_ee_resume = 0;
extern "C" void ee_resume_fail(EEContext* c, uint32_t func) {
    fatal("Savestate: no resume point in function %08x (ra %08x)", func, c->r[31].uw[0]);
}
namespace kernel {

// function whose code continues at pc: a body containing pc (at_start: pc is a function start,
// i.e. the fallthrough of the previous function)
static const EEFuncEntry* find_resume_func(u32 pc, bool* at_start) {
    auto search = [&](const ModIndex& m) -> const EEFuncEntry* {
        if (pc < m.info->base || pc > m.info->code_end) return nullptr;
        auto it = std::upper_bound(m.sorted.begin(), m.sorted.end(), pc,
                                   [](u32 v, const EEFuncEntry* e) { return v < e->start; });
        if (it == m.sorted.begin()) return nullptr;
        const EEFuncEntry* e = *(it - 1);
        if (e->start == pc) { *at_start = true; return module_matches(m, e) ? e : nullptr; }
        // pc == end: return point of a call at the very end (falls into the next function)
        if (pc <= e->end) { *at_start = false; return module_matches(m, e) ? e : nullptr; }
        return nullptr;
    };
    if (const EEFuncEntry* e = search(g_main_index)) return e;
    for (auto& m : g_mods)
        if (const EEFuncEntry* e = search(m)) return e;
    return nullptr;
}

static void resume_chain(EEContext* c, u32 pc) {
    for (;;) {
        if (pc == 0) return;  // the thread function returned
        bool at_start = false;
        const EEFuncEntry* e = find_resume_func(pc & 0x1FFFFFFF, &at_start);
        if (!e) fatal("Savestate: no code at resume point %08x", pc);
        if (at_start) {
            e->fn(c);
        } else {
            if (!e->body) fatal("Savestate: function %08x has no resume points", e->start);
            g_ee_resume = pc & 0x1FFFFFFF;
            e->body(c);
        }
        pc = c->r[31].uw[0];
    }
}

static void WINAPI resume_fiber_entry(void* param) {
    Thread* t = (Thread*)param;
    EEContext* c = t->ctx;
    int kind = t->resume_kind;
    t->resume_kind = RK_NONE;
    switch (kind) {
    case RK_FRESH: ee_call(c, t->func); thread_exit_current(); return;
    case RK_WAITSEMA: RET(c, t->wait_result); break;
    case RK_SLEEP: RET(c, t->id); break;
    case RK_ROTATE: RET(c, c->r[4].sw[0]); break;
    default: break;
    }
    resume_chain(c, t->resume_pc);
    thread_exit_current();
}

// all threads that exist can continue from their resume points (savestate possible now)
static bool threads_resumable(std::string* why) {
    for (auto& t : g_threads) {
        if (!t.id || t.status == THS_DORMANT || !t.fiber) continue;
        if (t.resume_kind == RK_NONE) {
            if (why) {
                char b[96];
                snprintf(b, sizeof b, "thread %d (status %d, wait %d) has no resume point", t.id, t.status, t.wait_type);
                *why = b;
            }
            return false;
        }
    }
    return true;
}

// replace every thread's fiber with one that continues from the resume point (as after a restore)
static void respawn_threads() {
    for (auto& t : g_threads) {
        if (!t.id || t.status == THS_DORMANT || !t.fiber) continue;
        DeleteFiber(t.fiber);
        t.fiber = CreateFiberEx(64 * 1024, 4 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, resume_fiber_entry, &t);
        if (!t.fiber) fatal("CreateFiber failed");
    }
}

// --resume-test=SEC[,PERIOD] (test): from SEC on, every PERIOD seconds all threads continue on new
// fibers from their resume points; the game must run on unaffected
double g_resume_test_at = 0, g_resume_test_period = 0;
static int g_resume_tests = 0;
static void resume_test_log() {
    g_resume_tests++;
    if (g_resume_tests <= 20 || g_resume_tests % 100 == 0) {
        std::string st;
        for (auto& t : g_threads)
            if (t.id && t.status != THS_DORMANT && t.fiber) {
                char b[64];
                snprintf(b, sizeof b, " %d:%d@%08x", t.id, t.resume_kind, t.resume_pc);
                st += b;
            }
        log_write(LOG_KERNEL, "resume test %d at %.2f s:%s", g_resume_tests, g_emu_time, st.c_str());
    }
}
static bool resume_test_due() { return g_resume_test_at > 0 && g_emu_time >= g_resume_test_at; }
static void resume_test_done() { g_resume_test_at = g_resume_test_period > 0 ? g_emu_time + g_resume_test_period : 0; }

// A safe point: the running thread is at a loop check (resume point target), every other thread
// blocked at its resume point. A restore (or the resume test) abandons the running thread's native
// stack: it is queued as ready at the resume point and the scheduler replaces all fibers.
static void safe_point(EEContext* c, u32 target) {
    (void)c;
    g_safe_due = false;
    Thread* self = g_cur;
    if (ss::load_pending()) {
        // the scheduler loads the state (this thread's native stack is abandoned; if loading
        // fails, the thread simply continues here)
        make_ready(self, true);
        switch_to_scheduler();
        return;
    }
    self->resume_pc = target;
    self->resume_kind = RK_PLAIN;
    std::string why;
    if (!threads_resumable(&why)) {
        static double last = 0;
        if (g_emu_time - last > 1.0) { last = g_emu_time; log_write(LOG_KERNEL, "safe point: %s", why.c_str()); }
        g_safe_due = true;  // try again
        return;
    }
    if (ss::save_pending() && ss::can_save(&why)) {
        // saved as the first ready thread at its resume point
        int st = self->status;
        s64 rs = self->ready_seq;
        make_ready(self, true);
        ss::do_save();
        self->status = st;
        self->ready_seq = rs;
    } else if (ss::save_pending()) {
        g_safe_due = true;
    }
    if (resume_test_due()) {
        resume_test_log();
        resume_test_done();
        make_ready(self, true);
        g_respawn = true;
        switch_to_scheduler();
        fatal("abandoned thread resumed");
    }
}

// scheduler idle point (no thread running): also a safe point
static void scheduler_safe_point() {
    if (g_respawn) {
        g_respawn = false;
        respawn_threads();
        return;
    }
    if (ss::load_pending()) ss::do_load();
    if (ss::save_pending()) {
        std::string why;
        if (threads_resumable(&why) && ss::can_save(&why)) ss::do_save();
        else g_safe_due = true;
    }
    if (resume_test_due()) {
        if (!threads_resumable(nullptr)) { g_safe_due = true; return; }
        resume_test_log();
        resume_test_done();
        respawn_threads();
    }
}


// ---------------------------------------------------------------------------
// Savestate: kernel objects and time
// ---------------------------------------------------------------------------
bool can_savestate(std::string* why) { return threads_resumable(why); }

void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    if (ver != 1) { s.fail("kernel version"); return; }
    s.v(intc_stat); s.v(intc_mask); s.v(dmac_stat);
    s.v(g_ie); s.v(g_dispatch_disabled); s.v(g_seq); s.v(g_handler_id); s.v(g_alarm_id);
    s.v(g_vsync_flag_ptr); s.v(g_vsync_csr_ptr);
    s.arr(g_syscall_override);
    s.v(g_heap_start); s.v(g_heap_end);
    s.v(g_emu_time); s.v(g_next_vblank_s); s.v(g_next_vblank_e);
    s.v(g_fr_game_rate); s.v(g_fr_forced);
    u32 nthreads = MAX_THREADS;
    s.v(nthreads);
    if (nthreads != MAX_THREADS) { s.fail("thread count"); return; }
    for (auto& t : g_threads) {
        s.v(t.id); s.v(t.status); s.v(t.func); s.v(t.stack); s.v(t.stack_size); s.v(t.gp);
        s.v(t.init_prio); s.v(t.prio); s.v(t.attr); s.v(t.option); s.v(t.wait_type); s.v(t.wait_id);
        s.v(t.wakeup_count); s.v(t.arg); s.v(t.ready_seq); s.v(t.wait_result); s.v(t.suspended);
        s.v(t.resume_pc); s.v(t.resume_kind);
        bool has_ctx = t.ctx != nullptr;
        s.v(has_ctx);
        if (has_ctx) {
            if (s.loading && !t.ctx) t.ctx = alloc_ctx();
            s.pod(*t.ctx, "EEContext");
            if (s.loading) t.ctx->thread = &t;
        }
        if (s.loading) t.fiber_dead = false;
    }
    u32 nsemas = MAX_SEMAS;
    s.v(nsemas);
    if (nsemas != MAX_SEMAS) { s.fail("semaphore count"); return; }
    for (auto& m : g_semas) {
        s.v(m.id); s.v(m.count); s.v(m.max_count); s.v(m.init_count); s.v(m.attr); s.v(m.option); s.v(m.used);
        s.vec(m.waiters);
    }
    for (int i = 0; i < 16; i++) { s.vec(g_intc_handlers[i]); s.vec(g_dmac_handlers[i]); }
    s.vec(g_alarms);
}

// after a restore: every thread continues on a new fiber from its resume point; the clock goes on
// from the saved emulated time
void after_state_load() {
    for (void* f : g_dead_fibers) DeleteFiber(f);
    g_dead_fibers.clear();
    for (auto& t : g_threads) {
        if (t.fiber) { DeleteFiber(t.fiber); t.fiber = nullptr; }
        if (!t.id || t.status == THS_DORMANT) continue;
        t.fiber = CreateFiberEx(64 * 1024, 4 * 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, resume_fiber_entry, &t);
        if (!t.fiber) fatal("CreateFiber failed");
    }
    g_cur = nullptr;
    g_in_handler = false;
    g_resume_pc = 0;
    g_resume_kind = RK_NONE;
    g_override_depth = 0;
    g_time_base = host_time_sec() - g_emu_time / g_speed;
    g_ee_irq_pending = 1;
}

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
void init() {
    memset(g_threads, 0, 0);
    g_time_base = host_time_sec();
    g_irq_ctx = alloc_ctx();
    init_dispatch();
}

static std::map<u32, int> g_samples;

// Native sampling profiler (--perf): samples the emulation thread's RIP every ms and writes
// "rva count" lines to perf_samples.txt (map RVAs to symbols with tools/perfmap.py).
bool g_native_perf = false;
double g_native_perf_from = 0;  // --perf-from=SEC: start sampling at this emulated time
static HANDLE g_emu_thread = nullptr;
static std::unordered_map<u64, u32> g_rip_samples, g_rip_late;  // late: while a missed frame is still being computed
int g_native_perf_target = 0;  // 0: emulation thread, 1: rasterizer worker 0
static void native_perf_sample() {
    HANDLE th = g_emu_thread;
    if (g_native_perf_target == 1) {
        th = (HANDLE)::gs_worker0_handle();
    } else if (g_native_perf_target == 2) {
        th = (HANDLE)gsp::worker_handle();
    }
    if (!th) return;
    if (SuspendThread(th) == (DWORD)-1) return;
    CONTEXT ctx;
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &ctx)) {
        static u64 base = 0, size = 0;
        if (!base) {
            base = (u64)GetModuleHandleW(nullptr);
            auto* dos = (IMAGE_DOS_HEADER*)base;
            auto* nt = (IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
            size = nt->OptionalHeader.SizeOfImage;
        }
        // Outside our image (Windows DLLs): unwind to the first caller inside the program and
        // attribute the sample there, tagged as external (bit 63).
        u64 rip = ctx.Rip, tag = 0;
        for (int depth = 0; depth < 24 && !(rip >= base && rip < base + size); depth++) {
            tag = 1ull << 63;
            DWORD64 img = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(rip, &img, nullptr);
            if (!fn) {  // leaf function: return address is on top of the stack
                ctx.Rip = *(u64*)ctx.Rsp;
                ctx.Rsp += 8;
            } else {
                void* handler = nullptr;
                DWORD64 frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, img, rip, fn, &ctx, &handler, &frame, nullptr);
            }
            rip = ctx.Rip;
            if (!rip) break;
        }
        u64 key = (rip >= base && rip < base + size) ? ((rip - base) | tag) : ~0ull;
        g_rip_samples[key]++;
        if (g_frame_late) g_rip_late[key]++;
    }
    ResumeThread(th);
}
static void native_perf_dump();
// Sampler thread: busy-waits a random 0.2-1.0 ms between samples so samples are not aligned with
// the OS timer tick (the emulation thread also wakes on it, which biased the old 1 ms sampler).
static DWORD WINAPI perf_thread(void*) {
    u64 x = 0x9E3779B97F4A7C15ull;
    double next = host_time_sec();
    u64 n = 0;
    for (;;) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        next += 0.0002 + (double)(x % 800) * 1e-6;
        while (host_time_sec() < next) _mm_pause();
        if (emu_time() < g_native_perf_from) continue;
        native_perf_sample();
        if (++n % 10000 == 0) native_perf_dump();
    }
    return 0;
}

static void native_perf_dump() {
    FILE* f = fopen("perf_samples.txt", "w");
    if (!f) return;
    for (auto& kv : g_rip_samples) fprintf(f, "%llx %u\n", (unsigned long long)kv.first, kv.second);
    fclose(f);
    if ((f = fopen("perf_late.txt", "w"))) {
        for (auto& kv : g_rip_late) fprintf(f, "%llx %u\n", (unsigned long long)kv.first, kv.second);
        fclose(f);
    }
}

static DWORD WINAPI ticker_thread(void*) {
    timeBeginPeriod(1);
    int n = 0;
    for (;;) {
        Sleep(1);
        g_ee_irq_pending = 1;

        if (g_watchdog) {
            Thread* t = g_cur;
            EEContext* c = g_in_handler ? g_irq_ctx : (t ? t->ctx : nullptr);
            g_samples[c ? c->cur_func : 0]++;
            if (n % 5000 == 4999) {
                std::vector<std::pair<int, u32>> v;
                for (auto& kv : g_samples) v.push_back({kv.second, kv.first});
                std::sort(v.rbegin(), v.rend());
                std::string s;
                for (size_t i = 0; i < v.size() && i < 12; i++) {
                    char b[40];
                    snprintf(b, sizeof(b), " %08x:%d", v[i].second, v[i].first);
                    s += b;
                }
                log_write(LOG_KERNEL, "[profile]%s", s.c_str());
                g_samples.clear();
            }
        }
        if (++n % 2000 == 0 && g_watchdog) {
            Thread* t = g_cur;
            EEContext* c = g_in_handler ? g_irq_ctx : (t ? t->ctx : nullptr);
            if (c)
                log_write(LOG_KERNEL, "[watchdog] thread %d func=%08x ra=%08x sp=%08x v0=%08x a0=%08x handler=%d", t ? t->id : -1,
                          c->cur_func, c->r[31].uw[0], c->r[29].uw[0], c->r[2].uw[0], c->r[4].uw[0], g_in_handler);
            else
                log_write(LOG_KERNEL, "[watchdog] idle");
            log_write(LOG_KERNEL, "[watchdog] raster %s", gs::raster_debug_state().c_str());
        }
    }
    return 0;
}

[[noreturn]] void run_main(u32 entry) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_emu_thread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    g_sched_fiber = ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (!g_sched_fiber) fatal("ConvertThreadToFiber failed");
    // main thread: id 1, priority 0
    Thread& t = g_threads[1];
    t = Thread();
    t.id = 1;
    t.func = entry;
    t.stack = 0x01F00000;
    t.stack_size = 0x100000;
    t.gp = 0;
    t.init_prio = t.prio = 0;
    t.ctx = alloc_ctx();
    start_thread(&t);
    CreateThread(nullptr, 0, ticker_thread, nullptr, 0, nullptr);
    if (g_native_perf) CreateThread(nullptr, 0, perf_thread, nullptr, 0, nullptr);

    for (;;) {
        for (void* f : g_dead_fibers) DeleteFiber(f);
        g_dead_fibers.clear();
        for (auto& th : g_threads)
            if (th.fiber_dead && th.fiber && th.status == THS_DORMANT) {
                DeleteFiber(th.fiber);
                th.fiber = nullptr;
                th.fiber_dead = false;
            }
        g_cur = nullptr;
        process_events();
        scheduler_safe_point();
        if (resume_test_due() || ss::pending()) g_safe_due = true;
        Thread* r = pick_ready();
        if (r) {
            g_cur = r;
            r->status = THS_RUN;
            SwitchToFiber(r->fiber);
            g_cur = nullptr;
            continue;
        }
        // idle: wait for next event
        double nt = next_event_time();
        double now = (host_time_sec() - g_time_base) * g_speed;
        double dt = nt - now;
        if (iop::has_ready_work()) continue;
        u64 t0 = __rdtsc();
        if (dt > 0.0015) Sleep((DWORD)((dt - 0.001) * 1000));
        else if (dt > 0) SwitchToThread();
        g_ee_idle_ticks += __rdtsc() - t0;
    }
}

}  // namespace kernel

// ---------------------------------------------------------------------------
// misc C entry points used by generated code
// ---------------------------------------------------------------------------
extern "C" {

void ee_break(EEContext* c, uint32_t pc) {
    log_write(LOG_KERNEL, "BREAK at %08x (ra=%08x)", pc, c->r[31].uw[0]);
}
void ee_invalid(EEContext* c, uint32_t pc, uint32_t word) {
    fatal("Invalid instruction %08x at %08x (ra=%08x)", word, pc, c->r[31].uw[0]);
}
void ee_eret(EEContext* c, uint32_t pc) {
    // libkernel uses "mtc0 ErrorEPC=ra; eret" to leave kernel mode after touching COP0 registers;
    // the generated code returns to the caller right after this call, which matches that pattern.
    if (c->cop0[30] != c->r[31].uw[0] && c->cop0[14] != c->r[31].uw[0])
        log_write(LOG_KERNEL, "ERET at %08x with EPC %08x / ErrorEPC %08x != ra %08x", pc, c->cop0[14], c->cop0[30], c->r[31].uw[0]);
}
uint32_t ee_mfc0(EEContext* c, int reg) {
    switch (reg) {
    case 9: return (uint32_t)kernel::cycles();   // Count
    case 25: return (uint32_t)(kernel::cycles() >> 1);  // performance counters
    case 12: return 0x70020C11 | (kernel::g_ie ? 0x10000 : 0);  // Status
    case 15: return 0x2E20;  // PRid
    default: return c->cop0[reg];
    }
}
void ee_mtc0(EEContext* c, int reg, uint32_t v) {
    c->cop0[reg] = v;
    if (reg == 12) {
        bool ie = (v & 0x10001) == 0x10001;
        kernel::g_ie = ie;
        if (ie) g_ee_irq_pending = 1;
    }
}
void ee_ei(EEContext* c) {
    (void)c;
    kernel::g_ie = true;
    if (!kernel::g_in_handler) {
        kernel::process_events();
        g_ee_irq_pending = 1;  // preemption at the next loop check (resume point there)
    }
}
void ee_di(EEContext* c) {
    (void)c;
    kernel::g_ie = false;
}
int ee_cpcond0(EEContext* c) {
    (void)c;
    return 1;  // DMA "all channels idle" condition
}
uint32_t ee_cfc1(EEContext* c, int reg) {
    if (reg == 0) return 0x2E30;
    if (reg == 31) return (c->fcr31 & ~0x00800000u) | (c->fcond ? 0x00800000u : 0);
    return 0;
}
void ee_ctc1(EEContext* c, int reg, uint32_t v) {
    if (reg == 31) {
        c->fcr31 = v;
        c->fcond = (v >> 23) & 1;
    }
}

}
int perf_target_raster() { kernel::g_native_perf_target = 1; return 1; }
int perf_target_gsp() { kernel::g_native_perf_target = 2; return 1; }
void watch_range_k(u32 lo, u32 hi) { kernel::watch_range(lo, hi); }
bool find_mdz_flag() { kernel::g_find_mdz = true; return true; }
bool scene_skip_flag_test() { kernel::g_test_scene_skip_flag = true; return true; }
double watch_mem_parse(const char* s) {
    double sec = atof(s);
    const char* p = strchr(s, ':');
    if (!p) return 0;
    p++;
    if (*p == '"') {  // SEC:"TEXT":LEN
        const char* e = strchr(p + 1, '"');
        if (!e) return 0;
        kernel::g_watch_mem_find.assign(p + 1, e);
        p = e + 1;
        if (*p == ':') kernel::g_watch_mem_len = (u32)strtoul(p + 1, nullptr, 16);
        kernel::g_watch_mem_at = sec;
        return sec;
    }
    if (*p == '*') {
        kernel::g_watch_mem_ptr = (u32)strtoul(p + 1, (char**)&p, 16);
        if (*p == '+') p++;
    }
    kernel::g_watch_mem_off = (u32)strtoul(p, (char**)&p, 16);
    if (*p == ':') kernel::g_watch_mem_len = (u32)strtoul(p + 1, nullptr, 16);
    kernel::g_watch_mem_at = sec;
    return sec;
}
