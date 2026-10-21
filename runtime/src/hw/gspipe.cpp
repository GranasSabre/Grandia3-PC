// GS pipeline thread (see gspipe.h). One producer (emulation thread), one consumer (pipeline thread):
// records of a 16-byte header and a payload padded to 16 bytes in a ring; a record that would cross
// the end of the ring is preceded by a pad record filling the rest.
#include "gspipe.h"
#include <windows.h>
#include <atomic>
#include <intrin.h>
#include "vif.h"
#include "gif.h"
#include "../gs/gs.h"
#include "../ee/kernel.h"
#include "ee_recomp.h"

extern u64 g_idle_loop_ticks;  // idle_hooks.cpp
namespace gs {
void priv_apply(u32 reg, u64 v);
void present_frame();
}

namespace gsp {

enum Kind : u32 { K_PAD, K_VIF1, K_VIF1_WORDS, K_PATH3, K_VIF1_REG, K_GIF_REG, K_PRIV, K_VBLANK, K_REPRESENT };

struct Rec {
    u32 kind;  // Kind | aux << 16 (register offset)
    u32 len;   // payload bytes
    u64 arg;
};

static constexpr u64 RING = 32ull << 20;
static constexpr u32 CHUNK = 1u << 20;  // larger transfers are split (VIF and GIF accept data in pieces)

static bool g_threaded = false;
static u8* g_ring = nullptr;
static HANDLE g_thread = nullptr, g_wake_worker = nullptr, g_wake_producer = nullptr, g_wake_ee = nullptr;
static DWORD g_worker_tid = 0;
alignas(64) static std::atomic<u64> g_head{0};  // bytes published by the producer
alignas(64) static std::atomic<u64> g_tail{0};  // bytes executed by the worker
alignas(64) static std::atomic<int> g_worker_sleeping{0};
static std::atomic<int> g_producer_waiting{0};
static std::atomic<u32> g_intc_post{0};
static u64 g_prev_vblank_end = 0;  // ring position after the previous vertical blank record

// statistics (emulation thread, except the busy time)
static u64 g_syncs[SYNC_N], g_sync_waits = 0, g_vblank_waits = 0, g_full_waits = 0;
static u64 g_wait_ticks = 0, g_bytes = 0;
static std::atomic<u64> g_busy_ticks{0};

bool threaded() { return g_threaded; }
void* worker_handle() { return g_thread; }

static inline u32 pad16(u32 n) { return (n + 15) & ~15u; }

static void exec(const Rec& r, const u8* p) {
    u32 aux = r.kind >> 16;
    switch (r.kind & 0xFFFF) {
    case K_VIF1: vif::transfer(1, p, r.len / 16); break;
    case K_VIF1_WORDS: vif::transfer_tag_words(1, p); break;
    case K_PATH3: gif::path3_dma(p, r.len / 16); break;
    case K_VIF1_REG: vif::write_reg(0x10003C00u | (aux & 0x3FF), (u32)r.arg); break;
    case K_GIF_REG: gif::write_reg(0x10003000u | (aux & 0xFFF), (u32)r.arg); break;
    case K_PRIV: gs::priv_apply(aux, r.arg); break;
    case K_VBLANK: gs::present_frame(); break;
    case K_REPRESENT: gs::raster_represent(); break;
    }
}

// The producer waits until done() holds: a short spin, then the worker's wake event (the 1 ms
// timeout only guards the loop; the worker signals after every record while somebody waits).
template <class F>
static void wait_until(F done) {
    if (done()) return;
    u64 t0 = __rdtsc();
    for (int i = 0; i < 4000; i++) {
        _mm_pause();
        if (done()) { g_wait_ticks += __rdtsc() - t0; return; }
    }
    g_producer_waiting.fetch_add(1);
    while (!done()) WaitForSingleObject(g_wake_producer, 1);
    g_producer_waiting.fetch_sub(1);
    g_wait_ticks += __rdtsc() - t0;
}

static void publish(u64 head) {
    g_head.store(head);
    if (g_worker_sleeping.load()) SetEvent(g_wake_worker);
}

// room for a record of `bytes` (header included) at the head; returns the head position to write at
static u64 reserve(u64 bytes) {
    u64 h = g_head.load(std::memory_order_relaxed);
    u64 off = h & (RING - 1);
    if (off + bytes > RING) {
        u64 padlen = RING - off;
        if (h + padlen - g_tail.load() > RING) { g_full_waits++; wait_until([&] { return h + padlen - g_tail.load() <= RING; }); }
        Rec* r = (Rec*)(g_ring + off);
        r->kind = K_PAD;
        r->len = (u32)(padlen - 16);
        r->arg = 0;
        h += padlen;
        publish(h);
    }
    if (h + bytes - g_tail.load() > RING) { g_full_waits++; wait_until([&] { return h + bytes - g_tail.load() <= RING; }); }
    return h;
}

static void push(u32 kind, u32 aux, u64 arg, const void* data, u32 len) {
    if (!g_threaded) {
        Rec r{kind | (aux << 16), len, arg};
        exec(r, (const u8*)data);
        return;
    }
    u64 bytes = 16 + pad16(len);
    u64 h = reserve(bytes);
    Rec* r = (Rec*)(g_ring + (h & (RING - 1)));
    r->kind = kind | (aux << 16);
    r->len = len;
    r->arg = arg;
    if (len) memcpy(r + 1, data, len);
    g_bytes += len;
    publish(h + bytes);
}

static void push_data(u32 kind, const u8* data, u32 qwc) {
    while (qwc) {
        u32 n = std::min(qwc, CHUNK / 16);
        push(kind, 0, 0, data, n * 16);
        data += n * 16;
        qwc -= n;
    }
}

void vif1(const u8* data, u32 qwc) { push_data(K_VIF1, data, qwc); }
void vif1_words(const u8* words8) { push(K_VIF1_WORDS, 0, 0, words8, 8); }
void path3(const u8* data, u32 qwc) { push_data(K_PATH3, data, qwc); }
void vif1_reg(u32 addr, u32 v) { push(K_VIF1_REG, addr & 0x3FF, v, nullptr, 0); }
void gif_reg(u32 addr, u32 v) { push(K_GIF_REG, addr & 0xFFF, v, nullptr, 0); }
void priv(u32 reg, u64 v) { push(K_PRIV, reg & 0x1FF0, v, nullptr, 0); }
void represent() { push(K_REPRESENT, 0, 0, nullptr, 0); }

void vblank() {
    push(K_VBLANK, 0, 0, nullptr, 0);
    if (!g_threaded) return;
    // at most one frame in flight: the previous vertical blank must have been presented
    u64 prev = g_prev_vblank_end;
    g_prev_vblank_end = g_head.load(std::memory_order_relaxed);
    if (g_tail.load() < prev) {
        g_vblank_waits++;
        wait_until([&] { return g_tail.load() >= prev; });
    }
}

void sync_slow(int reason) {
    g_syncs[reason]++;
    u64 h = g_head.load(std::memory_order_relaxed);
    if (g_tail.load() >= h) return;
    g_sync_waits++;
    wait_until([&] { return g_tail.load() >= h; });
}

void post_intc(int cause) {
    if (!g_threaded || GetCurrentThreadId() != g_worker_tid) {
        kernel::raise_intc(cause);
        return;
    }
    g_intc_post.fetch_or(1u << cause);
    g_ee_irq_pending = 1;
    SetEvent(g_wake_ee);
}

void poll_events() {
    if (!g_intc_post.load(std::memory_order_relaxed)) return;
    u32 m = g_intc_post.exchange(0);
    for (int c = 0; m; c++, m >>= 1)
        if (m & 1) kernel::raise_intc(c);
}

void idle_sleep() {
    if (g_threaded) WaitForSingleObject(g_wake_ee, 1);
    else Sleep(1);
}

static DWORD WINAPI worker(void*) {
    u64 t = 0;
    for (;;) {
        u64 h = g_head.load();
        if (t == h) {
            // idle: spin a little (the producer sends in bursts), then sleep until woken
            bool more = false;
            for (int i = 0; i < 2000 && !more; i++) {
                _mm_pause();
                more = g_head.load(std::memory_order_acquire) != t;
            }
            if (!more) {
                g_worker_sleeping.store(1);
                if (g_head.load() == t) WaitForSingleObject(g_wake_worker, INFINITE);
                g_worker_sleeping.store(0);
            }
            continue;
        }
        u64 t0 = __rdtsc();
        while (t != h) {
            const Rec* r = (const Rec*)(g_ring + (t & (RING - 1)));
            if ((r->kind & 0xFFFF) != K_PAD) exec(*r, (const u8*)(r + 1));
            t += 16 + pad16(r->len);
            g_tail.store(t);
            if (g_producer_waiting.load()) SetEvent(g_wake_producer);
        }
        g_busy_ticks.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
    }
    return 0;
}

void start(bool threaded) {
    g_threaded = false;
    if (!threaded) {
        log_write(LOG_MAIN, "gs pipeline: on the emulation thread");
        return;
    }
    g_ring = (u8*)VirtualAlloc(nullptr, RING, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    g_wake_worker = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_wake_producer = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_wake_ee = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_ring || !g_wake_worker || !g_wake_producer || !g_wake_ee) {
        log_write(LOG_MAIN, "gs pipeline: thread not available, on the emulation thread");
        return;
    }
    g_threaded = true;
    g_thread = CreateThread(nullptr, 8 * 1024 * 1024, worker, nullptr, 0, &g_worker_tid);
    if (!g_thread) {
        g_threaded = false;
        log_write(LOG_MAIN, "gs pipeline: thread not created, on the emulation thread");
        return;
    }
    SetThreadDescription(g_thread, L"GS pipeline");
    log_write(LOG_MAIN, "gs pipeline: own thread");
}

std::string stats() {
    if (!g_threaded) return "gs pipeline on the emulation thread";
    static u64 last_idle_loop = 0;
    static u64 last_busy = 0, last_wait = 0, last_bytes = 0, last_tsc = 0, last_syncs[SYNC_N], last_sw = 0, last_vw = 0, last_fw = 0;
    u64 tsc = __rdtsc(), busy = g_busy_ticks.load(std::memory_order_relaxed);
    double span = last_tsc ? (double)(tsc - last_tsc) : 1.0;
    char b[512];
    static const char* names[SYNC_N] = {"vifreg", "gifreg", "vumem", "readback", "siglbl", "state", "other"};
    std::string syncs;
    for (int i = 0; i < SYNC_N; i++)
        if (g_syncs[i] != last_syncs[i]) {
            char t[48];
            snprintf(t, sizeof(t), " %s %llu", names[i], (unsigned long long)(g_syncs[i] - last_syncs[i]));
            syncs += t;
            last_syncs[i] = g_syncs[i];
        }
    u64 il = g_idle_loop_ticks;
    snprintf(b, sizeof(b), "gs pipeline: worker busy %.0f%%, game waits for it %.1f%%, emu waits %.1f%% (sync %llu, vblank %llu, full %llu), %.1f MB queued, syncs:%s",
             100.0 * (busy - last_busy) / span, 100.0 * (il - last_idle_loop) / span, 100.0 * (g_wait_ticks - last_wait) / span,
             (unsigned long long)(g_sync_waits - last_sw), (unsigned long long)(g_vblank_waits - last_vw),
             (unsigned long long)(g_full_waits - last_fw), (g_bytes - last_bytes) / 1048576.0, syncs.empty() ? " none" : syncs.c_str());
    last_busy = busy;
    last_idle_loop = il;
    last_wait = g_wait_ticks;
    last_bytes = g_bytes;
    last_tsc = tsc;
    last_sw = g_sync_waits;
    last_vw = g_vblank_waits;
    last_fw = g_full_waits;
    return b;
}

}  // namespace gsp
