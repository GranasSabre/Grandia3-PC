#pragma once
#include "../common.h"
#include "ee_recomp.h"

namespace ipu {

void init();
u32 read_reg(u32 addr);
void write_reg(u32 addr, u32 v);
void fifo_read(r128* out);
void fifo_write(const r128* v);
void dma_in(const u8* data, u32 qwc);
void dma_in_start();
void dma_out_start();
void kick();
u64 command_count();  // completed commands (movie playback detection)

}  // namespace ipu
