// SPU2 emulation core. See spu2.h.
//
// Behaviour follows the documented PS1 SPU / PS2 SPU2 hardware (ADPCM format, envelope generator,
// 4-point gaussian interpolation, noise LFSR, reverb formula with 39-tap FIR resampling) and the
// core routing of the SPU2 (MMIX gates, core0 output chained into core1's external input, AutoDMA
// sound data input). Output: 48 kHz stereo, pushed to the audio backend.
#include <immintrin.h>
#include "../savestate.h"
#include "spu2.h"
#include "../memory.h"
#include "../platform/audio.h"
#include "../platform/platform.h"
#include <algorithm>

namespace spu2 {

u16 g_ram[RAM_HW];
Core g_core[2];
int g_cat_gain[CAT_COUNT] = {256, 256, 256, 256};
// core 0 voices 0/1: music stream, voices 8/9: speech stream; everything else: sound effects
static inline int voice_cat(int core, int vi) {
    if (core == 0 && (vi == 0 || vi == 1)) return CAT_MUSIC;
    if (core == 0 && (vi == 8 || vi == 9)) return CAT_VOICE;
    return CAT_SFX;
}

// 4-point gaussian interpolation table (PS1/PS2 SPU)
static const s16 GAUSS[512] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 3, 3,
    3, 4, 4, 5, 5, 6, 7, 7, 8, 9, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 21, 22, 24, 25, 27, 28, 30, 32, 33, 35, 37,
    39, 41, 44, 46, 48, 51, 53, 56, 58, 61, 64, 67, 70, 73, 77, 80,
    84, 87, 91, 95, 99, 103, 107, 111, 116, 120, 125, 130, 135, 140, 145, 150,
    156, 161, 167, 173, 179, 186, 192, 199, 205, 212, 219, 227, 234, 242, 250, 257,
    266, 274, 283, 291, 300, 309, 319, 328, 338, 348, 358, 369, 379, 390, 401, 412,
    424, 436, 448, 460, 473, 485, 498, 512, 525, 539, 553, 567, 582, 597, 612, 627,
    643, 659, 675, 692, 708, 726, 743, 761, 779, 797, 816, 835, 854, 874, 894, 914,
    935, 956, 977, 999, 1020, 1043, 1066, 1089, 1112, 1136, 1160, 1184, 1209, 1234, 1260, 1286,
    1312, 1339, 1366, 1394, 1422, 1450, 1479, 1508, 1537, 1567, 1598, 1628, 1660, 1691, 1723, 1756,
    1789, 1822, 1856, 1890, 1924, 1959, 1995, 2031, 2067, 2104, 2141, 2179, 2217, 2256, 2295, 2334,
    2374, 2415, 2456, 2497, 2539, 2582, 2624, 2668, 2712, 2756, 2801, 2846, 2892, 2938, 2985, 3032,
    3079, 3128, 3176, 3225, 3275, 3325, 3376, 3427, 3479, 3531, 3584, 3637, 3691, 3745, 3799, 3855,
    3910, 3967, 4023, 4081, 4138, 4197, 4255, 4315, 4374, 4435, 4495, 4557, 4619, 4681, 4744, 4807,
    4871, 4935, 5000, 5065, 5131, 5197, 5264, 5332, 5399, 5468, 5536, 5606, 5676, 5746, 5817, 5888,
    5959, 6032, 6104, 6177, 6251, 6325, 6400, 6475, 6550, 6626, 6702, 6779, 6856, 6934, 7012, 7091,
    7170, 7249, 7329, 7409, 7490, 7571, 7653, 7735, 7817, 7900, 7983, 8066, 8150, 8234, 8319, 8404,
    8489, 8575, 8661, 8748, 8834, 8922, 9009, 9097, 9185, 9273, 9362, 9451, 9541, 9630, 9720, 9811,
    9901, 9992, 10083, 10174, 10266, 10358, 10450, 10542, 10635, 10727, 10820, 10913, 11007, 11100, 11194, 11288,
    11382, 11476, 11571, 11665, 11760, 11855, 11950, 12045, 12140, 12236, 12331, 12427, 12522, 12618, 12714, 12809,
    12905, 13001, 13097, 13193, 13289, 13385, 13481, 13577, 13673, 13769, 13865, 13961, 14056, 14152, 14248, 14343,
    14439, 14534, 14630, 14725, 14820, 14915, 15010, 15104, 15199, 15293, 15387, 15481, 15575, 15669, 15762, 15855,
    15948, 16041, 16133, 16226, 16317, 16409, 16500, 16592, 16682, 16773, 16863, 16953, 17042, 17131, 17220, 17308,
    17396, 17484, 17571, 17658, 17744, 17830, 17916, 18001, 18086, 18170, 18254, 18337, 18420, 18502, 18584, 18665,
    18746, 18826, 18905, 18985, 19063, 19141, 19219, 19295, 19372, 19447, 19522, 19597, 19671, 19744, 19816, 19888,
    19959, 20030, 20100, 20169, 20238, 20306, 20373, 20439, 20505, 20570, 20634, 20698, 20760, 20822, 20884, 20944,
    21004, 21063, 21121, 21178, 21235, 21290, 21345, 21399, 21452, 21505, 21556, 21607, 21657, 21706, 21754, 21801,
    21848, 21893, 21938, 21982, 22025, 22066, 22107, 22148, 22187, 22225, 22262, 22299, 22334, 22369, 22402, 22435,
    22467, 22498, 22527, 22556, 22584, 22611, 22637, 22662, 22686, 22709, 22731, 22752, 22772, 22791, 22809, 22826,
    22842, 22857, 22872, 22885, 22897, 22908, 22918, 22927, 22935, 22942, 22948, 22953, 22957, 22960, 22962, 22963,
};

// 39-tap FIR filter used to resample reverb input/output between 48 kHz and 24 kHz
static const s32 RV_FIR[39] = {-1,    0, 2,     0,     -10, 0,     35,  0,     -103, 0,     266, 0,    -616,
                               0,     1332, 0, -2960, 0,   10246, 16384, 10246, 0,   -2960, 0,   1332, 0,
                               -616,  0,    266, 0,   -103, 0,    35,    0,     -10, 0,     2,    0,    -1};

static inline s32 clamp16(s32 v) { return v < -0x8000 ? -0x8000 : (v > 0x7FFF ? 0x7FFF : v); }
static inline s32 mul15(s32 a, s32 b) { return (a * b) >> 15; }

// ---------------------------------------------------------------------------
// Envelope generator (shared by ADSR and volume sweep).
// rate = (shift << 2) | step; rate_mask = bits that, when all set, freeze the envelope.
// ---------------------------------------------------------------------------
static inline void env_tick(s32& level, u32& counter, u32 rate, u32 rate_mask, bool exp, bool dec, bool inv) {
    int shift = (int)(rate >> 2);
    s32 step = 7 - (s32)(rate & 3);
    if (dec != inv) step = ~step;  // +7..+4 -> -8..-5
    if (shift < 11) step <<= (11 - shift);
    u32 inc = shift > 11 ? (0x8000u >> std::min(shift - 11, 31)) : 0x8000u;
    if (exp) {
        if (!dec && level > 0x6000) {
            if (shift < 10) step >>= 2;
            else if (shift >= 11) inc >>= 2;
            else { step >>= 1; inc >>= 1; }
        } else if (dec) {
            step = (step * level) >> 15;
        }
    }
    if ((rate & rate_mask) != rate_mask && inc == 0) inc = 1;
    counter += inc;
    if (!(counter & 0x8000)) return;
    counter = 0;
    level += step;
    if (!dec) level = level < -0x8000 ? -0x8000 : (level > 0x7FFF ? 0x7FFF : level);
    else if (inv) level = level < -0x8000 ? -0x8000 : (level > 0 ? 0 : level);
    else if (level < 0) level = 0;
}

void Volume::write(u16 v) {
    reg = v;
    if (!(v & 0x8000)) level = (s16)(u16)(v << 1);
    counter = 0;
}

void Volume::tick() {
    if (!(reg & 0x8000)) return;
    env_tick(level, counter, reg & 0x7F, 0x7F, (reg & 0x4000) != 0, (reg & 0x2000) != 0, (reg & 0x1000) != 0);
}

static inline void adsr_tick(Voice& v) {
    switch (v.phase) {
    case ADSR_ATTACK:
        env_tick(v.envx, v.env_counter, (v.adsr1 >> 8) & 0x7F, 0x7F, (v.adsr1 & 0x8000) != 0, false, false);
        if (v.envx >= 0x7FFF) {
            v.envx = 0x7FFF;
            v.phase = ADSR_DECAY;
            v.env_counter = 0;
        }
        break;
    case ADSR_DECAY: {
        env_tick(v.envx, v.env_counter, ((v.adsr1 >> 4) & 0xF) << 2, 0x7C, true, true, false);
        s32 sl = ((v.adsr1 & 0xF) + 1) * 0x800;
        if (v.envx <= sl) {
            v.phase = ADSR_SUSTAIN;
            v.env_counter = 0;
        }
        break;
    }
    case ADSR_SUSTAIN:
        env_tick(v.envx, v.env_counter, (v.adsr2 >> 6) & 0x7F, 0x7F, (v.adsr2 & 0x8000) != 0, (v.adsr2 & 0x4000) != 0,
                 false);
        break;
    case ADSR_RELEASE:
        env_tick(v.envx, v.env_counter, (v.adsr2 & 0x1F) << 2, 0x7C, (v.adsr2 & 0x20) != 0, true, false);
        if (v.envx <= 0) {
            v.envx = 0;
            v.phase = ADSR_OFF;
        }
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// ADPCM
// ---------------------------------------------------------------------------
static void decode_block(Voice& v) {
    static const s32 F0[5] = {0, 60, 115, 98, 122};
    static const s32 F1[5] = {0, 0, -52, -55, -60};
    u32 a = v.cur & RAM_MASK;
    u16 hdr = g_ram[a];
    v.flags = (u8)(hdr >> 8);
    if (v.flags & 4) v.lsa = a;  // loop start: remember block address
    int shift = hdr & 0xF;
    if (shift > 12) shift = 9;
    int filt = (hdr >> 4) & 7;
    if (filt > 4) filt = 4;
    v.dec[0] = v.dec[28];
    v.dec[1] = v.dec[29];
    v.dec[2] = v.dec[30];
    s32 p1 = v.prev1, p2 = v.prev2;
    const s32 f0 = F0[filt], f1 = F1[filt];
    for (int i = 0; i < 7; i++) {
        u16 w = g_ram[(a + 1 + i) & RAM_MASK];
        for (int n = 0; n < 4; n++) {
            s32 s = (s16)(u16)(((w >> (n * 4)) & 0xF) << 12);
            s >>= shift;
            s += (p1 * f0 + p2 * f1 + 32) >> 6;
            s = clamp16(s);
            v.dec[3 + i * 4 + n] = (s16)s;
            p2 = p1;
            p1 = s;
        }
    }
    v.prev1 = p1;
    v.prev2 = p2;
}

static inline void next_block(Core& c, Voice& v, int vi) {
    if (v.flags & 1) {  // loop end
        c.endx |= 1u << vi;
        v.cur = v.lsa;
        if (!(v.flags & 2)) {  // end + mute
            v.phase = ADSR_RELEASE;
            v.envx = 0;
        }
    } else {
        v.cur = (v.cur + 8) & RAM_MASK;
    }
    decode_block(v);
}

u32 voice_nax(int core, int voice) {
    Voice& v = g_core[core & 1].v[voice % 24];
    // address of the data word currently being consumed (header at +0, 4 samples per data word)
    return (v.cur + 1 + ((v.counter >> 12) >> 2)) & RAM_MASK;
}

void key_on(int core, u32 mask) {
    Core& c = g_core[core & 1];
    for (int i = 0; i < 24; i++) {
        if (!(mask & (1u << i))) continue;
        Voice& v = c.v[i];
        LOG(LOG_SPU, "kon c%d v%02d ssa=%06x lsa=%06x pitch=%04x vol=%04x/%04x mix=%d%d", core & 1, i, v.ssa * 2, v.lsa * 2, v.pitch,
            v.vol_l.reg, v.vol_r.reg, (c.vmixl >> i) & 1, (c.vmixel >> i) & 1);
        v.cur = v.ssa & RAM_MASK;
        v.counter = 0;
        v.prev1 = v.prev2 = 0;
        memset(v.dec, 0, sizeof(v.dec));
        decode_block(v);
        v.envx = 0;
        v.env_counter = 0;
        v.phase = ADSR_ATTACK;
        v.outx = 0;
        c.endx &= ~(1u << i);
    }
}

void key_off(int core, u32 mask) {
    Core& c = g_core[core & 1];
    for (int i = 0; i < 24; i++) {
        if (!(mask & (1u << i))) continue;
        Voice& v = c.v[i];
        if (v.phase != ADSR_OFF && v.phase != ADSR_RELEASE) {
            v.phase = ADSR_RELEASE;
            v.env_counter = 0;
        }
    }
}

void write_esa(int core, u32 hw_addr) {
    Core& c = g_core[core & 1];
    c.esa = hw_addr & RAM_MASK;
    c.rv_pos = 0;
}

void set_attr(int core, u16 v) { g_core[core & 1].attr = v; }

// ---------------------------------------------------------------------------
// AutoDMA
// ---------------------------------------------------------------------------
int adma_start_write(int core, u32 iop_addr, u32 size, bool loop, u32 start_offset, u32 start_buff) {
    Adma& a = g_core[core & 1].adma;
    if (a.active || size == 0) return -1;
    a.active = true;
    a.loop = loop;
    a.read = false;
    a.base = iop_addr & 0x1FFFFF;
    a.half = size;
    a.buff = start_buff & 1;
    a.madr = a.base + a.half * a.buff + std::min(start_offset, size);
    return 0;
}

int adma_start_read(int core, u32 iop_addr, u32 size, bool loop, u32 area_hw, u32 count) {
    Adma& a = g_core[core & 1].adma;
    if (a.active || size == 0) return -1;
    a.active = true;
    a.loop = loop;
    a.read = true;
    a.base = iop_addr & 0x1FFFFF;
    a.half = size;
    a.buff = 0;
    a.madr = a.base;
    a.read_area = area_hw & RAM_MASK;
    a.read_count = count ? count : 1;
    return 0;
}

u32 adma_status(int core) {
    Adma& a = g_core[core & 1].adma;
    u32 m = a.active ? a.madr : 0;
    return (a.buff << 24) | (m & 0xFFFFFF);
}

u32 adma_stop(int core) {
    u32 r = adma_status(core);
    g_core[core & 1].adma.active = false;
    return r;
}

// Advance the DMA by n bytes, calling fn(iop_addr, bytes) for each contiguous piece.
template <typename F>
static void adma_transfer(Core& c, u32 n, F fn) {
    Adma& a = c.adma;
    u32 done = 0;
    while (done < n && a.active) {
        u32 half_end = a.base + a.half * (a.buff + 1);
        u32 k = std::min(n - done, half_end - a.madr);
        fn(a.madr, done, k);
        a.madr += k;
        done += k;
        if (a.madr >= half_end) {
            if (a.loop) {
                a.buff ^= 1;
                a.madr = a.base + a.half * a.buff;
            } else {
                a.active = false;
                c.mmix &= 0xFF3F;  // libsd clears the memory-input dry gates after a one-shot transfer
            }
        }
    }
}

// Called at the start of every 256-sample half of the input buffer.
static void adma_block(Core& c, int core, u32 in_pos) {
    Adma& a = c.adma;
    u32 in_l = 0x2000 + core * 0x400 + (in_pos & 0x100);
    if (!a.active) {
        // no data: input is silent
        memset(&g_ram[in_l], 0, 256 * 2);
        memset(&g_ram[in_l + 0x200], 0, 256 * 2);
        return;
    }
    if (!a.read) {
        u8 tmp[1024];
        memset(tmp, 0, sizeof(tmp));
        adma_transfer(c, 1024, [&](u32 src, u32 off, u32 k) {
            for (u32 i = 0; i < k; i++) tmp[off + i] = mem::iop_ram[(src + i) & 0x1FFFFF];
        });
        memcpy(&g_ram[in_l], tmp, 512);
        memcpy(&g_ram[in_l + 0x200], tmp + 512, 512);
    } else {
        // capture: copy the half that was just completed from each source area
        u32 prev = (in_pos & 0x100) ^ 0x100;
        u32 bytes = 512 * a.read_count;
        u8 tmp[512 * 16];
        for (u32 k = 0; k < a.read_count && k < 16; k++)
            memcpy(tmp + k * 512, &g_ram[(a.read_area + k * 0x200 + prev) & RAM_MASK], 512);
        adma_transfer(c, bytes, [&](u32 dst, u32 off, u32 k) {
            for (u32 i = 0; i < k; i++) mem::iop_ram[(dst + i) & 0x1FFFFF] = tmp[off + i];
        });
    }
}

// ---------------------------------------------------------------------------
// Reverb
// ---------------------------------------------------------------------------
static inline u32 rv_index(const Core& c, u32 start, u32 size, s32 off) {
    s64 x = ((s64)c.rv_pos + off) % (s64)size;
    if (x < 0) x += size;
    return (start + (u32)x) & RAM_MASK;
}

static inline s32 fir(const s32* buf) {
    s32 acc = 0;
    for (int i = 0; i < 39; i += 2) acc += buf[i] * RV_FIR[i];  // odd taps are zero except the center
    acc += buf[19] * RV_FIR[19];
    return acc;
}

// Process one sample of reverb for a core. Returns wet output (before EVOL).
static void reverb(Core& c, int core, s32 in_l, s32 in_r, bool right, s32& out_l, s32& out_r) {
    u32 start = c.esa;
    u32 end = eea(core);
    u32 p = c.rv_bufpos;
    c.rv_down[0][p] = c.rv_down[0][p + 64] = clamp16(in_l);
    c.rv_down[1][p] = c.rv_down[1][p + 64] = clamp16(in_r);

    s32 out = 0;
    if (end > start) {
        const ReverbRegs& r = c.rv;
        u32 size = end - start + 1;
        int ch = right ? 1 : 0;
        s32 in = clamp16(fir(&c.rv_down[ch][p + 64 - 38]) >> 15);
        in = mul15(in, right ? r.in_r : r.in_l);
        auto rd = [&](s32 off) -> s32 { return (s16)g_ram[rv_index(c, start, size, off)]; };
        s32 same_src = (s32)(right ? r.same_r_src : r.same_l_src);
        s32 same_dst = (s32)(right ? r.same_r_dst : r.same_l_dst);
        s32 diff_src = (s32)(right ? r.diff_l_src : r.diff_r_src);
        s32 diff_dst = (s32)(right ? r.diff_r_dst : r.diff_l_dst);
        s32 apf1_dst = (s32)(right ? r.apf1_r : r.apf1_l);
        s32 apf2_dst = (s32)(right ? r.apf2_r : r.apf2_l);
        s32 apf1_src = apf1_dst - (s32)r.apf1_size;
        s32 apf2_src = apf2_dst - (s32)r.apf2_size;

        s32 same_prev = rd(same_dst - 1);
        s32 diff_prev = rd(diff_dst - 1);
        s32 same = mul15(r.iir_vol, in + mul15(r.wall_vol, rd(same_src)) - same_prev) + same_prev;
        s32 diff = mul15(r.iir_vol, in + mul15(r.wall_vol, rd(diff_src)) - diff_prev) + diff_prev;
        out = mul15(r.comb1_vol, rd((s32)(right ? r.comb1_r : r.comb1_l))) +
              mul15(r.comb2_vol, rd((s32)(right ? r.comb2_r : r.comb2_l))) +
              mul15(r.comb3_vol, rd((s32)(right ? r.comb3_r : r.comb3_l))) +
              mul15(r.comb4_vol, rd((s32)(right ? r.comb4_r : r.comb4_l)));
        s32 a1s = rd(apf1_src);
        s32 apf1 = out - mul15(r.apf1_vol, a1s);
        out = a1s + mul15(r.apf1_vol, apf1);
        s32 a2s = rd(apf2_src);
        s32 apf2 = out - mul15(r.apf2_vol, a2s);
        out = a2s + mul15(r.apf2_vol, apf2);
        if (c.attr & 0x80) {  // effect enable: write back into the work area
            g_ram[rv_index(c, start, size, same_dst)] = (u16)clamp16(same);
            g_ram[rv_index(c, start, size, diff_dst)] = (u16)clamp16(diff);
            g_ram[rv_index(c, start, size, apf1_dst)] = (u16)clamp16(apf1);
            g_ram[rv_index(c, start, size, apf2_dst)] = (u16)clamp16(apf2);
        }
        out = clamp16(out);
        if (right) c.rv_pos = (c.rv_pos + 1) % size;
        c.rv_up[ch][p] = c.rv_up[ch][p + 64] = out;
        c.rv_up[ch ^ 1][p] = c.rv_up[ch ^ 1][p + 64] = 0;
    } else {
        c.rv_up[0][p] = c.rv_up[0][p + 64] = 0;
        c.rv_up[1][p] = c.rv_up[1][p + 64] = 0;
    }
    c.rv_bufpos = (p + 1) & 63;
    out_l = clamp16(fir(&c.rv_up[0][p + 64 - 38]) >> 14);
    out_r = clamp16(fir(&c.rv_up[1][p + 64 - 38]) >> 14);
}

// ---------------------------------------------------------------------------
// Mixing
// ---------------------------------------------------------------------------
static u64 g_sample_count = 0;
static u32 g_in_pos = 0;   // position in the 512-sample input/output areas

static inline void noise_tick(Core& c) {
    u32 clk = (c.attr >> 8) & 0x3F;
    int shift = (int)(clk >> 2);
    s32 step = (s32)(clk & 3) + 4;
    c.noise_timer -= step;
    if (c.noise_timer < 0) {
        u32 lv = c.noise_level;
        u32 parity = ((lv >> 15) ^ (lv >> 12) ^ (lv >> 11) ^ (lv >> 10) ^ 1) & 1;
        c.noise_level = ((lv << 1) | parity) & 0xFFFF;
        c.noise_timer += 0x20000 >> shift;
        if (c.noise_timer < 0) c.noise_timer += 0x20000 >> shift;
    }
}

struct Mix { s32 dl, dr, wl, wr; };

static inline void mix_voices(Core& c, int core, Mix& m, u32 out_pos) {
    m = Mix{0, 0, 0, 0};
    if (c.non) noise_tick(c);
    for (int vi = 0; vi < 24; vi++) {
        Voice& v = c.v[vi];
        if (v.phase == ADSR_OFF) {
            v.outx = 0;
            continue;
        }
        u32 bit = 1u << vi;
        s32 s;
        if (c.non & bit) {
            s = (s16)c.noise_level;
        } else {
            u32 si = v.counter >> 12;
            u32 ii = (v.counter >> 4) & 0xFF;
            const s16* d = &v.dec[si];
            s = ((GAUSS[0xFF - ii] * d[0]) >> 15) + ((GAUSS[0x1FF - ii] * d[1]) >> 15) +
                ((GAUSS[0x100 + ii] * d[2]) >> 15) + ((GAUSS[ii] * d[3]) >> 15);
        }
        s32 o = mul15(s, v.envx);
        v.outx = o;
        // pitch counter
        u32 step = v.pitch;
        if (vi > 0 && (c.pmon & bit)) {
            s32 factor = c.v[vi - 1].outx + 0x8000;
            step = (u32)((((s32)(s16)step) * factor) >> 15) & 0xFFFF;
        }
        if (step > 0x3FFF) step = 0x4000;
        v.counter += step;
        while (v.counter >= (28u << 12)) {
            v.counter -= 28u << 12;
            next_block(c, v, vi);
        }
        adsr_tick(v);
        s32 l = mul15(o, v.vol_l.level), r = mul15(o, v.vol_r.level);
        int g = g_cat_gain[voice_cat(core, vi)];
        if (g != 256) { l = (l * g) >> 8; r = (r * g) >> 8; }
        v.vol_l.tick();
        v.vol_r.tick();
        if (c.vmixl & bit) m.dl += l;
        if (c.vmixr & bit) m.dr += r;
        if (c.vmixel & bit) m.wl += l;
        if (c.vmixer & bit) m.wr += r;
    }
    // voice 1 / voice 3 output capture areas
    g_ram[(core ? 0xC00 : 0x400) + out_pos] = (u16)clamp16(c.v[1].outx);
    g_ram[(core ? 0xE00 : 0x600) + out_pos] = (u16)clamp16(c.v[3].outx);
}

// Mix one core: returns output before master volume.
static inline void mix_core(Core& c, int core, const Mix& vm, s32 ext_l, s32 ext_r, bool right, u32 out_pos,
                            s32& out_l, s32& out_r) {
    u16 g = c.mmix;
    if (core == 0) g &= 0xFF0;
    u32 in_idx = 0x2000 + core * 0x400 + out_pos;
    s32 in_l = mul15((s16)g_ram[in_idx], c.bvol_l);
    s32 in_r = mul15((s16)g_ram[in_idx + 0x200], c.bvol_r);
    if (g_cat_gain[CAT_MOVIE] != 256) {  // AutoDMA input: movie sound
        in_l = (in_l * g_cat_gain[CAT_MOVIE]) >> 8;
        in_r = (in_r * g_cat_gain[CAT_MOVIE]) >> 8;
    }
    s32 dl = 0, dr = 0, wl = 0, wr = 0;
    if (g & 0x800) dl += vm.dl;
    if (g & 0x400) dr += vm.dr;
    if (g & 0x200) wl += vm.wl;
    if (g & 0x100) wr += vm.wr;
    if (g & 0x080) dl += in_l;
    if (g & 0x040) dr += in_r;
    if (g & 0x020) wl += in_l;
    if (g & 0x010) wr += in_r;
    if (g & 0x008) dl += ext_l;
    if (g & 0x004) dr += ext_r;
    if (g & 0x002) wl += ext_l;
    if (g & 0x001) wr += ext_r;
    s32 rl = 0, rr = 0;
    bool need_rv = (c.attr & 0x80) || c.evol_l || c.evol_r;
    if (need_rv) {
        reverb(c, core, wl, wr, right, rl, rr);
        rl = mul15(rl, c.evol_l);
        rr = mul15(rr, c.evol_r);
    }
    out_l = clamp16(dl + rl);
    out_r = clamp16(dr + rr);
    u32 mo = core ? 0x1800 : 0x1000;
    g_ram[mo + out_pos] = (u16)out_l;
    g_ram[mo + 0x200 + out_pos] = (u16)out_r;
    g_ram[mo + 0x400 + out_pos] = (u16)clamp16(rl);
    g_ram[mo + 0x600 + out_pos] = (u16)clamp16(rr);
}

static void render(s16* out, int n) {
    for (int i = 0; i < n; i++) {
        u32 pos = g_in_pos;
        if ((pos & 0xFF) == 0) {
            adma_block(g_core[0], 0, pos);
            adma_block(g_core[1], 1, pos);
        }
        bool right = (g_sample_count & 1) != 0;
        Mix m0, m1;
        mix_voices(g_core[0], 0, m0, pos);
        mix_voices(g_core[1], 1, m1, pos);
        s32 l0, r0;
        mix_core(g_core[0], 0, m0, 0, 0, right, pos, l0, r0);
        Core& c0 = g_core[0];
        l0 = clamp16(mul15(l0, c0.mvol_l.level));
        r0 = clamp16(mul15(r0, c0.mvol_r.level));
        c0.mvol_l.tick();
        c0.mvol_r.tick();
        // core0 output -> core1 external input (also visible in sound RAM)
        g_ram[0x800 + pos] = (u16)l0;
        g_ram[0xA00 + pos] = (u16)r0;
        Core& c1 = g_core[1];
        s32 el = mul15(l0, c1.avol_l), er = mul15(r0, c1.avol_r);
        s32 l1, r1;
        mix_core(c1, 1, m1, el, er, right, pos, l1, r1);
        l1 = clamp16(mul15(l1, c1.mvol_l.level));
        r1 = clamp16(mul15(r1, c1.mvol_r.level));
        c1.mvol_l.tick();
        c1.mvol_r.tick();
        out[i * 2] = (s16)l1;
        out[i * 2 + 1] = (s16)r1;
        g_in_pos = (pos + 1) & 0x1FF;
        g_sample_count++;
    }
}

// Cutscene skip: the voices advance (pitch counter, block flags, loops, envelopes, AutoDMA) so
// streams and the sound driver progress as usual, but nothing is decoded or mixed.
static void advance_silent(int n) {
    for (int i = 0; i < n; i++) {
        u32 pos = g_in_pos;
        if ((pos & 0xFF) == 0) {
            adma_block(g_core[0], 0, pos);
            adma_block(g_core[1], 1, pos);
        }
        for (int ci = 0; ci < 2; ci++) {
            Core& c = g_core[ci];
            for (int vi = 0; vi < 24; vi++) {
                Voice& v = c.v[vi];
                if (v.phase == ADSR_OFF) continue;
                v.outx = 0;
                u32 step = v.pitch;
                if (step > 0x3FFF) step = 0x4000;
                v.counter += step;
                while (v.counter >= (28u << 12)) {
                    v.counter -= 28u << 12;
                    if (v.flags & 1) {  // loop end
                        c.endx |= 1u << vi;
                        v.cur = v.lsa;
                        if (!(v.flags & 2)) {
                            v.phase = ADSR_RELEASE;
                            v.envx = 0;
                        }
                    } else {
                        v.cur = (v.cur + 8) & RAM_MASK;
                    }
                    u32 a = v.cur & RAM_MASK;
                    v.flags = (u8)(g_ram[a] >> 8);
                    if (v.flags & 4) v.lsa = a;
                }
                adsr_tick(v);
            }
        }
        g_in_pos = (pos + 1) & 0x1FF;
        g_sample_count++;
    }
}

// after a silent stretch: the sample buffers of the playing voices are decoded again
static void resync_voices() {
    for (Core& c : g_core)
        for (Voice& v : c.v)
            if (v.phase != ADSR_OFF) decode_block(v);
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
static bool g_inited = false;
static double g_t0 = -1;
static s64 g_done = 0;
static double g_gap_max = 0, g_last_call = -1, g_stat_t = 0, g_cpu = 0;
static u64 g_skipped = 0;

void init() {
    memset(g_ram, 0, sizeof(g_ram));
    for (int c = 0; c < 2; c++) g_core[c] = Core();
    g_sample_count = 0;
    g_in_pos = 0;
    g_t0 = -1;
    g_done = 0;
    g_inited = true;
}

u64 g_spu_ticks = 0;  // rdtsc ticks spent in advance_to (profiling)
static void advance_to_impl(double t);
void advance_to(double t) {
    u64 t0 = __rdtsc();
    {
        const platform::Config& c = platform::config();
        g_cat_gain[CAT_MUSIC] = c.vol_music * 256 / 100;
        g_cat_gain[CAT_VOICE] = c.vol_voice * 256 / 100;
        g_cat_gain[CAT_SFX] = c.vol_sfx * 256 / 100;
        g_cat_gain[CAT_MOVIE] = c.vol_movie * 256 / 100;
    }
    advance_to_impl(t);
    g_spu_ticks += __rdtsc() - t0;
}
static void advance_to_impl(double t) {
    if (!g_inited) return;
    if (g_t0 < 0) {
        g_t0 = t;
        return;
    }
    // stall statistics (host time between calls), logged every few seconds
    if (g_log_enabled[LOG_SPU]) {
        double h = host_time_sec();
        if (g_last_call >= 0 && h - g_last_call > g_gap_max) g_gap_max = h - g_last_call;
        g_last_call = h;
        if (h - g_stat_t >= 5.0) {
            LOG(LOG_SPU, "spu2: max gap between updates %.1f ms, skipped %llu samples, mixing cost %.1f ms/s",
                g_gap_max * 1000.0, (unsigned long long)g_skipped, g_cpu * 1000.0 / (h - g_stat_t));
            g_gap_max = 0;
            g_cpu = 0;
            g_stat_t = h;
        }
    }
    s64 target = (s64)((t - g_t0) * RATE);
    s64 n = target - g_done;
    if (n <= 0) return;
    static bool silent = false;
    if (platform::skip_active()) {
        silent = true;
        if (n > 8 * RATE) { g_skipped += (u64)(n - 8 * RATE); n = 8 * RATE; }
        advance_silent((int)n);
        g_done = target;
        return;
    }
    if (silent) {
        silent = false;
        resync_voices();
    }
    if (n > RATE) {  // more than a second behind (long stall): don't try to catch up all of it
        g_skipped += (u64)(n - RATE);
        g_done = target - RATE;
        n = RATE;
    }
    static s16 buf[512 * 2];
    double c0 = g_log_enabled[LOG_SPU] ? host_time_sec() : 0;
    while (n > 0) {
        int k = (int)std::min<s64>(n, 512);
        render(buf, k);
        audio::push(buf, k);
        n -= k;
        g_done += k;
    }
    if (g_log_enabled[LOG_SPU]) g_cpu += host_time_sec() - c0;
}

}  // namespace spu2

namespace spu2 {
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    s.arr(g_ram);
    for (auto& c : g_core) s.pod(c, "spu2::Core");
    s.v(g_sample_count); s.v(g_in_pos); s.v(g_t0); s.v(g_done);
}
}  // namespace spu2
