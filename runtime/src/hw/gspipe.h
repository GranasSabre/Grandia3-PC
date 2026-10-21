#pragma once
#include "../common.h"
#include <string>

// GS pipeline thread: VIF1 (with the VU1 programs it starts), the GIF and the GS front end run on a
// thread of their own, like the render thread of a PC game. The emulation thread copies VIF1/GIF DMA
// data and register writes into a ring and goes on; DMA completes at once as before. Reads of
// pipeline state (VIF1/GIF registers, VU1 memory, GS readbacks, savestates) first wait until the
// worker has executed everything queued (sync). Without the thread every command runs at once.
namespace gsp {

enum SyncReason { SYNC_VIF_REG, SYNC_GIF_REG, SYNC_VU_MEM, SYNC_READBACK, SYNC_SIGLBL, SYNC_STATE, SYNC_OTHER, SYNC_N };

void start(bool threaded);  // after gs::init
bool threaded();

// emulation thread
void vif1(const u8* data, u32 qwc);   // VIF1 data (DMA channel 1, VIF1 FIFO)
void vif1_words(const u8* words8);    // the two upper words of a DMA tag (TTE)
void path3(const u8* data, u32 qwc);  // GIF DMA channel 2, GIF FIFO
void vif1_reg(u32 addr, u32 v);       // VIF1 register write
void gif_reg(u32 addr, u32 v);        // GIF register write
void priv(u32 reg, u64 v);            // GS privileged register write (the pipeline's copy)
void vblank();                        // vertical blank: present the picture
void represent();                     // paused game: present the last picture again
void sync_slow(int reason);
inline void sync(int reason) {
    if (threaded()) sync_slow(reason);
}
void poll_events();         // interrupts raised by the pipeline thread
void idle_sleep();          // emulation thread idle: sleep about 1 ms, woken by pipeline interrupts

// pipeline thread (or the emulation thread without the pipeline thread)
void post_intc(int cause);

std::string stats();
void* worker_handle();

}  // namespace gsp
