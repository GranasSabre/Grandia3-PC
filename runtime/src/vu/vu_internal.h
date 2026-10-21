// VU state and pipeline helpers shared by the interpreter (vu.cpp) and the ahead-of-time
// translated microprograms (generated/vu/*.cpp), so both implement identical semantics.
#pragma once
#include "../common.h"
#include "ee_recomp.h"
#include <immintrin.h>
#include <cmath>

namespace vu {

enum UOp : u8 {
    U_NOP, U_ADDbc, U_SUBbc, U_MADDbc, U_MSUBbc, U_MAXbc, U_MINIbc, U_MULbc, U_MULq, U_MAXi, U_MULi, U_MINIi,
    U_ADDq, U_MADDq, U_ADDi, U_MADDi, U_SUBq, U_MSUBq, U_SUBi, U_MSUBi, U_ADD, U_MADD, U_MUL, U_MAX, U_SUB, U_MSUB,
    U_OPMSUB, U_MINI, U_ADDAbc, U_SUBAbc, U_MADDAbc, U_MSUBAbc, U_ITOF0, U_ITOF4, U_ITOF12, U_ITOF15, U_FTOI0,
    U_FTOI4, U_FTOI12, U_FTOI15, U_MULAbc, U_MULAq, U_ABS, U_MULAi, U_CLIP, U_ADDAq, U_MADDAq, U_ADDAi, U_MADDAi,
    U_SUBAq, U_MSUBAq, U_SUBAi, U_MSUBAi, U_ADDA, U_MADDA, U_MULA, U_SUBA, U_MSUBA, U_OPMULA, U_INVALID
};

enum LOp : u8 {
    L_NOP, L_LQ, L_SQ, L_ILW, L_ISW, L_IADDIU, L_ISUBIU, L_FCEQ, L_FCSET, L_FCAND, L_FCOR, L_FSEQ, L_FSSET, L_FSAND,
    L_FSOR, L_FMEQ, L_FMAND, L_FMOR, L_FCGET, L_B, L_BAL, L_JR, L_JALR, L_IBEQ, L_IBNE, L_IBLTZ, L_IBGTZ, L_IBLEZ,
    L_IBGEZ, L_IADD, L_ISUB, L_IADDI, L_IAND, L_IOR, L_MOVE, L_MR32, L_LQI, L_SQI, L_LQD, L_SQD, L_DIV, L_SQRT,
    L_RSQRT, L_WAITQ, L_MTIR, L_MFIR, L_ILWR, L_ISWR, L_RNEXT, L_RGET, L_RINIT, L_RXOR, L_MFP, L_XTOP, L_XITOP,
    L_XGKICK, L_ESADD, L_ERSADD, L_ELENG, L_ERLENG, L_EATANxy, L_EATANxz, L_ESUM, L_ESQRT, L_ERSQRT, L_ERCPR,
    L_WAITP, L_ESIN, L_EATAN, L_EEXP, L_INVALID
};

struct Decoded {
    u32 upper, lower;
    u8 uop, lop;
    bool ibit, ebit, mbit, dbit, tbit;
    bool valid;
};

struct FlagEntry {
    u64 ready;
    u32 mac, status_bits, clip;
    bool has_mac, has_clip;
};

struct VUState {
    int idx;
    vf128 vf[32];
    vf128 acc;
    u32 vi[16];
    float q, p, ireg;
    u32 r;
    u32 mac, status, clip;           // visible flags
    u32 status_sticky;
    FlagEntry pend[16];              // FIFO of flag results in flight (ready cycles are monotonic)
    u32 phead, ptail;
    u32 clip_issued;                 // clip value including not yet visible CLIP results
    float q_pending;
    u64 q_ready;
    bool q_busy;
    float p_pending;
    u64 p_ready;
    bool p_busy;
    u64 cycle;
    u32 pc;
    u8* micro;
    u8* data;
    u32 micro_size, data_qw;
    u32 top, itop;
    Decoded dec[2048];
    bool dec_valid;
    u64 image_hash;                  // FNV-1a of micro memory (valid with dec_valid)
    bool (*aot)(struct VUState& v);  // ahead-of-time translation of this image, if any
    // integer hazard
    int last_vi_reg;
    u32 last_vi_old;
};

static inline float clampf(float f) { return fpu_clamp(f); }
static inline float inf(float f) { return fpu_in(f); }

static inline void advance_pipeline(VUState& v) {
    // commit flag results that have completed
    while (v.phead != v.ptail) {
        const FlagEntry& e = v.pend[v.phead & 15];
        if (e.ready > v.cycle) break;
        if (e.has_mac) {
            v.mac = e.mac;
            v.status = (v.status & ~0xFu) | e.status_bits;
            v.status_sticky |= e.status_bits << 6;
        }
        if (e.has_clip) v.clip = e.clip;
        v.phead++;
    }
    if (v.q_busy && v.cycle >= v.q_ready) { v.q = v.q_pending; v.q_busy = false; }
    if (v.p_busy && v.cycle >= v.p_ready) { v.p = v.p_pending; v.p_busy = false; }
}

static inline void flush_pipeline(VUState& v) {
    v.cycle += 8;
    advance_pipeline(v);
    if (v.q_busy) { v.q = v.q_pending; v.q_busy = false; }
    if (v.p_busy) { v.p = v.p_pending; v.p_busy = false; }
    v.clip_issued = v.clip;
}

static inline FlagEntry& flag_slot(VUState& v) {
    if (v.ptail - v.phead >= 16) {  // pipeline full: stall until the oldest result lands
        v.cycle = v.pend[v.phead & 15].ready;
        advance_pipeline(v);
    }
    return v.pend[v.ptail++ & 15];
}

static inline void push_flags(VUState& v, u32 mac) {
    u32 st = ((mac & 0x000F) ? 1u : 0u) | ((mac & 0x00F0) ? 2u : 0u) | ((mac & 0x0F00) ? 4u : 0u) | ((mac & 0xF000) ? 8u : 0u);
    flag_slot(v) = FlagEntry{v.cycle + 4, mac, st, 0, true, false};
}

static inline void push_clip(VUState& v, u32 clip) {
    flag_slot(v) = FlagEntry{v.cycle + 4, 0, 0, clip, false, true};
    v.clip_issued = clip;
}

// pending clip value chain: CLIP builds on the latest issued clip value
static inline u32 latest_clip(VUState& v) { return v.clip_issued; }

// ---------------------------------------------------------------------------
// SSE helpers: PS2 floats have no Inf/NaN; such bit patterns act as +-max on input and
// results saturate to +-max.
// ---------------------------------------------------------------------------
static inline __m128 vsat(__m128 x) {
    const __m128i e = _mm_set1_epi32(0x7F800000);
    __m128i xi = _mm_castps_si128(x);
    __m128i sp = _mm_cmpeq_epi32(_mm_and_si128(xi, e), e);
    if (_mm_movemask_epi8(sp) == 0) return x;
    __m128i rep = _mm_or_si128(_mm_and_si128(xi, _mm_set1_epi32((int)0x80000000)), _mm_set1_epi32(0x7F7FFFFF));
    return _mm_castsi128_ps(_mm_blendv_epi8(xi, rep, sp));
}

alignas(16) static const u32 g_lane_mask[16][4] = {
    {0, 0, 0, 0}, {0, 0, 0, ~0u}, {0, 0, ~0u, 0}, {0, 0, ~0u, ~0u},
    {0, ~0u, 0, 0}, {0, ~0u, 0, ~0u}, {0, ~0u, ~0u, 0}, {0, ~0u, ~0u, ~0u},
    {~0u, 0, 0, 0}, {~0u, 0, 0, ~0u}, {~0u, 0, ~0u, 0}, {~0u, 0, ~0u, ~0u},
    {~0u, ~0u, 0, 0}, {~0u, ~0u, 0, ~0u}, {~0u, ~0u, ~0u, 0}, {~0u, ~0u, ~0u, ~0u}};
static const u8 g_rev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};

static inline __m128 dest_mask(u32 dest) { return _mm_load_ps((const float*)g_lane_mask[dest]); }

// MAC flags (zero bits 3..0, sign bits 7..4, x is the highest bit) of the written lanes
static inline u32 mac_of(__m128 r, u32 dest) {
    u32 lanes = g_rev4[dest];
    __m128i ri = _mm_castps_si128(r);
    __m128i z = _mm_cmpeq_epi32(_mm_and_si128(ri, _mm_set1_epi32(0x7FFFFFFF)), _mm_setzero_si128());
    u32 zm = (u32)_mm_movemask_ps(_mm_castsi128_ps(z)) & lanes;
    u32 sm = (u32)_mm_movemask_ps(r) & lanes;
    return g_rev4[zm] | ((u32)g_rev4[sm] << 4);
}

#define DEST(u) (((u) >> 21) & 15)
#define FT(u) (((u) >> 16) & 31)
#define FS(u) (((u) >> 11) & 31)
#define FD(u) (((u) >> 6) & 31)
#define BC(u) ((u) & 3)

static inline vf128* memq(VUState& v, u32 a) { return (vf128*)(v.data + (a & (v.data_qw - 1)) * 16); }

void xgkick(VUState& v, u32 addr_qw);

// Ahead-of-time translated microprogram image: runs from v.pc until the end bit, returns false if
// it cannot handle the entry point (the interpreter then runs it).
typedef bool (*AotFn)(VUState& v);
int register_aot(u64 image_hash, AotFn fn);

}  // namespace vu
