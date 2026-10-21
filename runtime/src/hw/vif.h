#pragma once
#include "../common.h"
#include "ee_recomp.h"

namespace vif {

void init();
void transfer(int idx, const u8* data, u32 qwc);
void transfer_tag_words(int idx, const u8* words8);
u32 read_reg(u32 addr);
void write_reg(u32 addr, u32 v);
void fifo_read(int idx, r128* out);
void vif1_readback(u32 madr, u32 qwc);

}  // namespace vif
