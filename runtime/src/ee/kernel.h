#pragma once
#include "../common.h"
#include "ee_recomp.h"

namespace kernel {

// INTC causes
enum {
    INTC_GS = 0, INTC_SBUS = 1, INTC_VBLANK_S = 2, INTC_VBLANK_E = 3, INTC_VIF0 = 4, INTC_VIF1 = 5,
    INTC_VU0 = 6, INTC_VU1 = 7, INTC_IPU = 8, INTC_TIM0 = 9, INTC_TIM1 = 10, INTC_TIM2 = 11,
    INTC_TIM3 = 12, INTC_SFIFO = 13, INTC_VU0WD = 14
};
// DMAC channels
enum {
    DMAC_VIF0 = 0, DMAC_VIF1 = 1, DMAC_GIF = 2, DMAC_FROM_IPU = 3, DMAC_TO_IPU = 4, DMAC_SIF0 = 5,
    DMAC_SIF1 = 6, DMAC_SIF2 = 7, DMAC_FROM_SPR = 8, DMAC_TO_SPR = 9
};

extern u32 intc_stat, intc_mask;
extern u32 dmac_stat;   // D_STAT: low 16 = status, high 16 = mask

void init();
[[noreturn]] void run_main(u32 entry);

void raise_intc(int cause);
void raise_dmac(int channel);
void intc_stat_write(u32 v);
void intc_mask_write(u32 v);
void dmac_stat_write(u32 v);

// called from HW polling loops to let time advance
void poll_time();
void wait_idle_hint();  // game is spinning on HW state: let time pass

EEContext* current_ctx();
extern double g_native_perf_from;
extern bool g_force60;   // --force60: frame rate experiment
extern double g_watch_text_at;
extern u64 g_ee_idle_ticks;
extern double g_resume_test_at, g_resume_test_period;  // --resume-test=SEC[,PERIOD]
int current_thread_id();
double emu_time();      // seconds of emulated time
bool flight_running();  // the flight module ran within the last two seconds
bool movie_running();   // the IPU decoded within the last second
bool battle_running();  // a battle is loaded
u64 cycles();           // EE cycle counter (294.912 MHz)
u32 hsync_count();

// Called by IOP side when it wants EE to process pending work.
void notify_pending();

// Function dispatch
EEFunc lookup_func(u32 addr);

}  // namespace kernel
namespace ss { class Stream; }
namespace kernel {
// savestates
bool can_savestate(std::string* why);  // every thread blocked at a resume point
void state(ss::Stream& s);             // threads, semaphores, handlers, alarms, time
void after_state_load();               // new fibers continuing from the resume points

}  // namespace kernel
