#pragma once
#include "../common.h"
#include "ee_recomp.h"

namespace hw {

void init();
u32 read32(u32 addr);
u64 read64(u32 addr);
void read128(u32 addr, r128* out);
void write8(u32 addr, u8 v);
void write16(u32 addr, u16 v);
void write32(u32 addr, u32 v);
void write64(u32 addr, u64 v);
void write128(u32 addr, const r128* v);

void timers_update();
double timers_next_event();

// DMA channel state
struct DmaChannel {
    u32 chcr = 0, madr = 0, qwc = 0, tadr = 0, asr0 = 0, asr1 = 0, sadr = 0;
};
extern DmaChannel dma[10];
extern u32 d_ctrl, d_pcr, d_sqwc, d_rbsr, d_rbor, d_stadr, d_enable;

}  // namespace hw
