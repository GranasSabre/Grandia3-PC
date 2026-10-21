#pragma once
#include "../common.h"

namespace sif {

// IOP-side address of the SIF command receive buffer
constexpr u32 IOP_CMD_BUF = 0x00010000;

// EE syscalls
int set_dma(u32 trans_ptr, int n);
int dma_stat(int id);
void set_dchain();
u32 set_reg(u32 reg, u32 val);
u32 get_reg(u32 reg);
u32 hw_read(u32 addr);
void hw_write(u32 addr, u32 v);

// IOP -> EE
void ee_write(u32 ee_addr, const void* data, u32 size);
void send_cmd_to_ee(u32 cid, const void* payload, u32 payload_size, u32 extra_ee_dst = 0,
                    const void* extra = nullptr, u32 extra_size = 0);
void pump();       // deliver queued EE-bound packets when the EE consumed the previous one
bool ee_ready();   // EE side registered its receive buffer

void set_ee_pktbuf(u32 a);
void set_smflag(u32 bits);
void clear_smflag(u32 bits);

}  // namespace sif
