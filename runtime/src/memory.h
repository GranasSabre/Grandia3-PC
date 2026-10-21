#pragma once
#include "common.h"
#include "ee_recomp.h"

namespace mem {

constexpr u32 RAM_SIZE = 32 * 1024 * 1024;
constexpr u32 SPR_SIZE = 16 * 1024;
constexpr u32 IOP_RAM_SIZE = 2 * 1024 * 1024;

extern u8* ram;
extern u8* spr;
extern u8* iop_ram;
extern u8 vu0_micro[4096];
extern u8 vu1_micro[16384];
extern u8 vu1_data[16384];

void init();

// Physical EE address (as used by DMA) -> host pointer. Handles SPR bit for DMA.
inline u8* dma_ptr(u32 addr, bool spr_bit_valid = true) {
    if (spr_bit_valid && (addr & 0x80000000u)) return spr + (addr & (SPR_SIZE - 1));
    return ram + (addr & (RAM_SIZE - 1));
}

// Virtual EE address -> host pointer for RAM/scratchpad; nullptr otherwise.
inline u8* ee_ptr(u32 addr) {
    u8* p = g_ee_pagemap[addr >> 24];
    return p ? p + (addr & 0xFFFFFF) : nullptr;
}

inline u32 rd32(u32 a) { return ee_rd32(a); }
inline u16 rd16(u32 a) { return ee_rd16(a); }
inline u8 rd8(u32 a) { return ee_rd8(a); }
inline void wr32(u32 a, u32 v) { ee_wr32(a, v); }
inline void wr16(u32 a, u16 v) { ee_wr16(a, v); }
inline void wr8(u32 a, u8 v) { ee_wr8(a, v); }

std::string read_cstr(u32 addr, size_t max = 1024);
void copy_to_ee(u32 addr, const void* src, size_t n);
void copy_from_ee(void* dst, u32 addr, size_t n);

// IOP memory
inline u8* iop_ptr(u32 addr) { return iop_ram + (addr & (IOP_RAM_SIZE - 1)); }
inline u32 iop_rd32(u32 a) { u32 v; memcpy(&v, iop_ptr(a), 4); return v; }
inline void iop_wr32(u32 a, u32 v) { memcpy(iop_ptr(a), &v, 4); }
std::string iop_cstr(u32 addr, size_t max = 1024);

}  // namespace mem
