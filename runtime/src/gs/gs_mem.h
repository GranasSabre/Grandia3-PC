// GS local memory (4MB VRAM) with PS2 swizzled addressing.
#pragma once
#include "../common.h"

namespace gs {

enum PSM {
    PSMCT32 = 0x00, PSMCT24 = 0x01, PSMCT16 = 0x02, PSMCT16S = 0x0A,
    PSMT8 = 0x13, PSMT4 = 0x14, PSMT8H = 0x1B, PSMT4HL = 0x24, PSMT4HH = 0x2C,
    PSMZ32 = 0x30, PSMZ24 = 0x31, PSMZ16 = 0x32, PSMZ16S = 0x3A
};

extern u32* vram;  // 1M words

extern const u8 blockTable32[4][8];
extern const u8 blockTable32Z[4][8];
extern const u8 blockTable16[8][4];
extern const u8 blockTable16S[8][4];
extern const u8 blockTable16Z[8][4];
extern const u8 blockTable16SZ[8][4];
extern const u8 blockTable8[4][8];
extern const u8 blockTable4[8][4];
extern const u8 columnTable32[8][8];
extern const u8 columnTable16[8][16];
extern const u8 columnTable8[16][16];
extern const u16 columnTable4[16][32];

void mem_init();

// word address of 32-bit pixel
static inline u32 addr32(int x, int y, u32 bp, u32 bw) {
    u32 page = ((y >> 5) * bw + (x >> 6)) * 32;
    u32 blk = bp + page + blockTable32[(y >> 3) & 3][(x >> 3) & 7];
    return ((blk & 0x3FFF) << 6) + columnTable32[y & 7][x & 7];
}
static inline u32 addr32z(int x, int y, u32 bp, u32 bw) {
    u32 page = ((y >> 5) * bw + (x >> 6)) * 32;
    u32 blk = bp + page + blockTable32Z[(y >> 3) & 3][(x >> 3) & 7];
    return ((blk & 0x3FFF) << 6) + columnTable32[y & 7][x & 7];
}
// halfword address of 16-bit pixel
static inline u32 addr16(int x, int y, u32 bp, u32 bw) {
    u32 page = ((y >> 6) * bw + (x >> 6)) * 32;
    u32 blk = bp + page + blockTable16[(y >> 3) & 7][(x >> 4) & 3];
    return ((blk & 0x3FFF) << 7) + columnTable16[y & 7][x & 15];
}
static inline u32 addr16s(int x, int y, u32 bp, u32 bw) {
    u32 page = ((y >> 6) * bw + (x >> 6)) * 32;
    u32 blk = bp + page + blockTable16S[(y >> 3) & 7][(x >> 4) & 3];
    return ((blk & 0x3FFF) << 7) + columnTable16[y & 7][x & 15];
}
static inline u32 addr16z(int x, int y, u32 bp, u32 bw) {
    u32 page = ((y >> 6) * bw + (x >> 6)) * 32;
    u32 blk = bp + page + blockTable16Z[(y >> 3) & 7][(x >> 4) & 3];
    return ((blk & 0x3FFF) << 7) + columnTable16[y & 7][x & 15];
}
static inline u32 addr16sz(int x, int y, u32 bp, u32 bw) {
    u32 page = ((y >> 6) * bw + (x >> 6)) * 32;
    u32 blk = bp + page + blockTable16SZ[(y >> 3) & 7][(x >> 4) & 3];
    return ((blk & 0x3FFF) << 7) + columnTable16[y & 7][x & 15];
}
// byte address of 8-bit pixel
static inline u32 addr8(int x, int y, u32 bp, u32 bw) {
    u32 page = ((y >> 6) * (bw >> 1) + (x >> 7)) * 32;
    u32 blk = bp + page + blockTable8[(y >> 4) & 3][(x >> 4) & 7];
    return ((blk & 0x3FFF) << 8) + columnTable8[y & 15][x & 15];
}
// nibble address of 4-bit pixel
static inline u32 addr4(int x, int y, u32 bp, u32 bw) {
    u32 page = ((y >> 7) * (bw >> 1) + (x >> 7)) * 32;
    u32 blk = bp + page + blockTable4[(y >> 4) & 7][(x >> 5) & 3];
    return ((blk & 0x3FFF) << 9) + columnTable4[y & 15][x & 31];
}

static inline u16* vram16() { return (u16*)vram; }
static inline u8* vram8() { return (u8*)vram; }

// Generic pixel read/write in a given format (raw value: 32/24/16 bit or index)
u32 read_pixel(u32 psm, int x, int y, u32 bp, u32 bw);
void write_pixel(u32 psm, int x, int y, u32 bp, u32 bw, u32 v);
// the same on another copy of the 4MB local memory
u32 read_pixel_m(const u32* mem, u32 psm, int x, int y, u32 bp, u32 bw);
void write_pixel_m(u32* mem, u32 psm, int x, int y, u32 bp, u32 bw, u32 v);

static inline bool psm_is_z(u32 psm) { return (psm & 0x30) == 0x30; }
int psm_bpp(u32 psm);

}  // namespace gs
