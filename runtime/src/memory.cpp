#include "memory.h"
#include "hw/hw.h"
#include <windows.h>

extern "C" {
uint8_t* g_ee_pagemap[256];
uint8_t* g_ee_ram;
uint8_t g_vu0_mem[4096] __attribute__((aligned(16)));
}

namespace mem {

u8* ram;
u8* spr;
u8* iop_ram;
u8 vu0_micro[4096] __attribute__((aligned(16)));
u8 vu1_micro[16384] __attribute__((aligned(16)));
u8 vu1_data[16384] __attribute__((aligned(16)));

static u8* alloc_pages(size_t sz) {
    void* p = VirtualAlloc(nullptr, sz, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) fatal("VirtualAlloc(%zu) failed", sz);
    return (u8*)p;
}

void init() {
    // RAM gets 32MB plus slack so 16MB-page accesses past the end don't fault.
    ram = alloc_pages(RAM_SIZE);
    spr = alloc_pages(16 * 1024 * 1024);
    iop_ram = alloc_pages(IOP_RAM_SIZE);
    g_ee_ram = ram;
    memset(g_ee_pagemap, 0, sizeof(g_ee_pagemap));
    for (u32 seg : {0x00u, 0x20u, 0x30u, 0x80u, 0xA0u}) {
        g_ee_pagemap[seg + 0] = ram;
        g_ee_pagemap[seg + 1] = ram + 0x1000000;
    }
    g_ee_pagemap[0x70] = spr;
}

std::string read_cstr(u32 addr, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        u8* p = ee_ptr(addr + (u32)i);
        if (!p || !*p) break;
        s.push_back((char)*p);
    }
    return s;
}

std::string iop_cstr(u32 addr, size_t max) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = (char)*iop_ptr(addr + (u32)i);
        if (!ch) break;
        s.push_back(ch);
    }
    return s;
}

void copy_to_ee(u32 addr, const void* src, size_t n) {
    const u8* s = (const u8*)src;
    for (size_t i = 0; i < n;) {
        u8* p = ee_ptr(addr + (u32)i);
        if (!p) { fatal("copy_to_ee: bad address %08x", addr + (u32)i); }
        // copy up to end of 16MB page
        size_t room = 0x1000000 - ((addr + i) & 0xFFFFFF);
        size_t k = n - i < room ? n - i : room;
        memcpy(p, s + i, k);
        i += k;
    }
}

void copy_from_ee(void* dst, u32 addr, size_t n) {
    u8* d = (u8*)dst;
    for (size_t i = 0; i < n;) {
        u8* p = ee_ptr(addr + (u32)i);
        if (!p) { fatal("copy_from_ee: bad address %08x", addr + (u32)i); }
        size_t room = 0x1000000 - ((addr + i) & 0xFFFFFF);
        size_t k = n - i < room ? n - i : room;
        memcpy(d + i, p, k);
        i += k;
    }
}

}  // namespace mem

// ---------------------------------------------------------------------------
// Slow path I/O accessors used by generated code
// ---------------------------------------------------------------------------
extern "C" {

uint8_t ee_io_rd8(uint32_t a) {
    u32 v = hw::read32(a & ~3u);
    return (u8)(v >> ((a & 3) * 8));
}
uint16_t ee_io_rd16(uint32_t a) {
    u32 v = hw::read32(a & ~3u);
    return (u16)(v >> ((a & 2) * 8));
}
uint32_t ee_io_rd32(uint32_t a) { return hw::read32(a); }
uint64_t ee_io_rd64(uint32_t a) { return hw::read64(a); }
void ee_io_rd128(uint32_t a, r128* out) { hw::read128(a, out); }
void ee_io_wr8(uint32_t a, uint8_t v) { hw::write8(a, v); }
void ee_io_wr16(uint32_t a, uint16_t v) { hw::write16(a, v); }
void ee_io_wr32(uint32_t a, uint32_t v) { hw::write32(a, v); }
void ee_io_wr64(uint32_t a, uint64_t v) { hw::write64(a, v); }
void ee_io_wr128(uint32_t a, const r128* v) { hw::write128(a, v); }

}
