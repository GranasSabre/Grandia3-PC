#pragma once
#include "../common.h"

namespace gif {

void path1(const u8* data, u32 qwc);       // from VU1 XGKICK
void path2(const u8* data, u32 qwc);       // from VIF1 DIRECT
void path3_dma(const u8* data, u32 qwc);   // GIF DMA channel
void path3_fifo(const u8* data, u32 qwc);  // CPU writes to GIF FIFO
u32 read_reg(u32 addr);
void write_reg(u32 addr, u32 v);
void set_path3_mask(bool m);

}  // namespace gif
