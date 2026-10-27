// Performance hooks: game code that busy-waits is replaced by an equivalent that lets the host
// thread sleep until the next emulated event (frees a CPU core for the renderer and audio threads).
#include "../common.h"
#include "../memory.h"
#include "kernel.h"
#include "ee_recomp.h"
#include <intrin.h>

// WaitVSync (0x145440): the counter at 0x216F08 is set to a0 + 1 and decremented by the vblank
// interrupt handler; the game spins until it reaches 0, then returns CSR bit 13 (field). Same
// registers on return; the wait passes the loop's resume point (savestates) like the game's loop.
int g_fr_trace = 0;  // --fr-trace=N: log the frame timing values at the next N WaitVSync calls (test)
extern "C" void hook_wait_vsync(EEContext* c) {
    if (g_fr_trace > 0 && kernel::emu_time() > 100.0) {
        g_fr_trace--;
        u32 d = mem::rd32(0x218B20), m = mem::rd32(0x218B30);
        float df, mf;
        memcpy(&df, &d, 4);
        memcpy(&mf, &m, 4);
        log_write(LOG_KERNEL, "fr trace: wait %u, rate %u, request %u, mode %u, delta %.4f, motion %.2f", c->r[4].uw[0], mem::ram[0x1EBAD8],
                  mem::ram[0x218AE8], mem::ram[0x218AF0], df, mf);
    }
    mem::wr32(0x216F08, c->r[4].uw[0] + 1);
    while (mem::rd32(0x216F08) != 0) {
        kernel::wait_idle_hint();         // sleeps until the next event (e.g. the vertical blank)
        ee_irq_check_at(c, 0x00145450u);  // interrupts, thread switches, savestate safe point
    }
    u64 csr = ee_rd64(0x12001000u);
    c->r[1].sd[0] = 0x12000000;
    c->r[2].sd[0] = (s64)((csr >> 13) & 1);
}

// FrameRate as a float (0x1AD3D0). The field's movement code multiplies the frame step by it to get
// "frames at its own rate" (walking speed): while the 60 fps option forces 60 these call sites get
// the rate the game asked for, so the steps halve with the doubled frame count instead of keeping
// their size (twice the speed). 0x2D7030 saves the rate to restore it after a scene played at 60
// (otherwise the forced 60 is restored and taken for the game's own rate). Its other uses divide by
// it (seconds timers, the main module's fades) and keep the real rate.
extern "C" void ee_001AD3D0(EEContext* c);
extern "C" void hook_frame_rate_float(EEContext* c) {
    static const u32 MOVE_SITES[] = {0x220924, 0x2209C4, 0x221214, 0x2212B4, 0x221B04, 0x221BA4, 0x222290, 0x2227CC, 0x22286C,
                                     0x23B12C, 0x23E344, 0x26A544, 0x282D74, 0x282E14, 0x283500, 0x283A3C, 0x283ADC, 0x295C10,
                                     0x2D7030};
    ee_001AD3D0(c);
    int r = kernel::native_frame_rate();
    if (!r) return;
    u32 site = c->r[31].uw[0] - 8;  // the jal (FIELD.BIN)
    for (u32 s : MOVE_SITES)
        if (s == site) { c->fpr[0].f = (float)r; return; }
}

// SetFrameRate (0x1AD380, f12 = frames per second): the 60 fps option keeps the game's own rate
extern "C" void ee_001AD380(EEContext* c);
extern "C" void hook_set_frame_rate(EEContext* c) {
    kernel::note_game_rate((int)c->fpr[12].f);
    ee_001AD380(c);
}

// Busy-wait loop of the game (config "idle_loops", e.g. the frame end waiting for the GS FINISH
// interrupt): an interrupt already pending is taken at once; otherwise the thread sleeps until the
// next event (vertical blank, timer, an interrupt from the GS pipeline thread).
u64 g_idle_loop_ticks = 0;  // time spent in these loops (statistics)
extern "C" void ee_idle_loop(EEContext* c, uint32_t target) {
    u64 t0 = __rdtsc();
    if (!g_ee_irq_pending) kernel::wait_idle_hint();
    ee_irq_check_at(c, target);
    g_idle_loop_ticks += __rdtsc() - t0;
}
