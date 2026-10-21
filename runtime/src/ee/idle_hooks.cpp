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
extern "C" void hook_wait_vsync(EEContext* c) {
    mem::wr32(0x216F08, c->r[4].uw[0] + 1);
    while (mem::rd32(0x216F08) != 0) {
        kernel::wait_idle_hint();         // sleeps until the next event (e.g. the vertical blank)
        ee_irq_check_at(c, 0x00145450u);  // interrupts, thread switches, savestate safe point
    }
    u64 csr = ee_rd64(0x12001000u);
    c->r[1].sd[0] = 0x12000000;
    c->r[2].sd[0] = (s64)((csr >> 13) & 1);
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
