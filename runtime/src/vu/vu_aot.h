// Helpers for ahead-of-time translated VU microprograms (generated/vu/*.cpp, see tools/vu_recomp.py).
// Every helper mirrors the corresponding step of the interpreter in vu.cpp. The interpreter commits
// pipeline results at the start of every instruction; translated code commits lazily right before a
// value is read, which yields the same observable values.
#pragma once
#include "vu_internal.h"
#include <cstring>

namespace vu {

extern u64 g_vu_instrs;

// VI write history entry for the conditional branch hazard: kind << 24 | reg << 16 | old value
enum { VIW_ALU_ = 0, VIW_LOAD_ = 1, VIW_FLAG_ = 2 };
#define VIW_ALU 0
#define VIW_LOAD 1
#define VIW_FLAG 2
#define HZ(reg, kind, old) (((u32)(kind) << 24) | ((u32)(reg) << 16) | ((u32)(old) & 0xFFFF))
static const u32 HZ_NONE = 0x00FF0000u;

static inline u32 hz_reg(u32 h) { return (h >> 16) & 0xFF; }
static inline u32 hz_kind(u32 h) { return h >> 24; }
static inline u32 hz_old(u32 h) { return h & 0xFFFF; }

// value of VI register r as seen by a conditional branch (same rule as branch_vi in vu.cpp)
static inline u32 aot_bvi(const VUState& v, int r, u32 h0, u32 h1, u32 h2, u32 h3) {
    if (r == 0) return 0;
    const u32 rr = (u32)r;
    if ((hz_reg(h0) == rr && hz_kind(h0) == VIW_LOAD) || (hz_reg(h1) == rr && hz_kind(h1) == VIW_LOAD) ||
        (hz_reg(h2) == rr && hz_kind(h2) == VIW_LOAD))
        return v.vi[r];
    if (hz_reg(h0) != rr || hz_kind(h0) != VIW_ALU) return v.vi[r];
    u32 val = hz_old(h0);
    if (hz_reg(h1) == rr && hz_kind(h1) == VIW_ALU) {
        val = hz_old(h1);
        if (hz_reg(h2) == rr && hz_kind(h2) == VIW_ALU) {
            val = hz_old(h2);
            if (hz_reg(h3) == rr && hz_kind(h3) == VIW_ALU) val = hz_old(h3);
        }
    }
    return val;
}

// commit flag results that are visible at cycle n
static inline void aot_sync(VUState& v, u64 n) {
    while (v.phead != v.ptail) {
        const FlagEntry& e = v.pend[v.phead & 15];
        if (e.ready > n) break;
        if (e.has_mac) {
            v.mac = e.mac;
            v.status = (v.status & ~0xFu) | e.status_bits;
            v.status_sticky |= e.status_bits << 6;
        }
        if (e.has_clip) v.clip = e.clip;
        v.phead++;
    }
}

// Q/P results visible at cycle n
static inline void aot_qp(VUState& v, u64 n) {
    if (v.q_busy && n >= v.q_ready) { v.q = v.q_pending; v.q_busy = false; }
    if (v.p_busy && n >= v.p_ready) { v.p = v.p_pending; v.p_busy = false; }
}

static inline FlagEntry& aot_slot(VUState& v) {
    if (v.ptail - v.phead >= 16) {  // the oldest entry is long visible: commit it to make room
        const FlagEntry& e = v.pend[v.phead & 15];
        if (e.has_mac) {
            v.mac = e.mac;
            v.status = (v.status & ~0xFu) | e.status_bits;
            v.status_sticky |= e.status_bits << 6;
        }
        if (e.has_clip) v.clip = e.clip;
        v.phead++;
    }
    return v.pend[v.ptail++ & 15];
}

static inline void aot_push_mac(VUState& v, u64 n, u32 mac) {
    u32 st = ((mac & 0x000F) ? 1u : 0u) | ((mac & 0x00F0) ? 2u : 0u) | ((mac & 0x0F00) ? 4u : 0u) | ((mac & 0xF000) ? 8u : 0u);
    aot_slot(v) = FlagEntry{n + 4, mac, st, 0, true, false};
}

static inline void aot_clip(VUState& v, u64 n, const vf128& S, const vf128& T) {
    float w = fabsf(inf(T.f[3]));
    u32 f = 0;
    float x = inf(S.f[0]), y = inf(S.f[1]), z = inf(S.f[2]);
    if (x > w) f |= 0x01;
    if (x < -w) f |= 0x02;
    if (y > w) f |= 0x04;
    if (y < -w) f |= 0x08;
    if (z > w) f |= 0x10;
    if (z < -w) f |= 0x20;
    u32 nc = ((v.clip_issued << 6) | f) & 0xFFFFFF;
    aot_slot(v) = FlagEntry{n + 4, 0, 0, nc, false, true};
    v.clip_issued = nc;
}

static inline __m128 aot_ftoi(__m128 S, float scale) {
    __m128 x = _mm_mul_ps(S, _mm_set1_ps(scale));
    __m128i xi = _mm_castps_si128(x);
    __m128i cv = _mm_cvttps_epi32(x);
    __m128i big = _mm_cmpgt_epi32(_mm_and_si128(xi, _mm_set1_epi32(0x7F800000)), _mm_set1_epi32(0x4E800000));
    __m128i sat = _mm_xor_si128(_mm_set1_epi32(0x7FFFFFFF), _mm_srai_epi32(xi, 31));
    return _mm_castsi128_ps(_mm_blendv_epi8(cv, sat, big));
}

// outer product (sy*tz, sz*tx, sx*ty) on x, y, z
static inline __m128 aot_outer(__m128 S, __m128 T) {
    return _mm_mul_ps(_mm_shuffle_ps(S, S, _MM_SHUFFLE(3, 0, 2, 1)), _mm_shuffle_ps(T, T, _MM_SHUFFLE(3, 1, 0, 2)));
}

// DIV (kind 0), SQRT (1), RSQRT (2): result arrives in Q after a latency
static inline void aot_div(VUState& v, u64 n, int kind, float af, float bf) {
    if (v.q_busy) { v.q = v.q_pending; v.q_busy = false; }
    float a = inf(af), b = inf(bf);
    float r;
    int lat;
    if (kind == 0) {
        if (b == 0.0f) {
            u32 ua, ub;
            memcpy(&ua, &a, 4);
            memcpy(&ub, &b, 4);
            u32 ru = ((ua ^ ub) & 0x80000000u) | 0x7F7FFFFF;
            memcpy(&r, &ru, 4);
            v.status_sticky |= 0x800;
        } else r = clampf(a / b);
        lat = 7;
    } else if (kind == 1) {
        r = sqrtf(fabsf(b));
        lat = 7;
    } else {
        float s = sqrtf(fabsf(b));
        if (s == 0.0f) {
            u32 ua;
            memcpy(&ua, &a, 4);
            u32 ru = (ua & 0x80000000u) | 0x7F7FFFFF;
            memcpy(&r, &ru, 4);
        } else r = clampf(a / s);
        lat = 13;
    }
    v.q_pending = r;
    v.q_ready = n + lat;
    v.q_busy = true;
}

enum {
    EFU_ESADD, EFU_ERSADD, EFU_ELENG, EFU_ERLENG, EFU_ESUM, EFU_ESQRT, EFU_ERSQRT, EFU_ERCPR, EFU_ESIN, EFU_EATAN,
    EFU_EEXP, EFU_EATANxy, EFU_EATANxz
};

// elementary function unit: result arrives in P after a latency
static inline void aot_efu(VUState& v, u64 n, int op, const vf128& s, int field) {
    if (v.p_busy) { v.p = v.p_pending; v.p_busy = false; }
    float x = inf(s.f[0]), y = inf(s.f[1]), z = inf(s.f[2]), w = inf(s.f[3]);
    float f = inf(s.f[field]);
    float r = 0;
    int lat = 12;
    switch (op) {
    case EFU_ESADD: r = x * x + y * y + z * z; lat = 11; break;
    case EFU_ERSADD: r = 1.0f / (x * x + y * y + z * z); lat = 18; break;
    case EFU_ELENG: r = sqrtf(x * x + y * y + z * z); lat = 18; break;
    case EFU_ERLENG: r = 1.0f / sqrtf(x * x + y * y + z * z); lat = 24; break;
    case EFU_ESUM: r = x + y + z + w; lat = 12; break;
    case EFU_ESQRT: r = sqrtf(fabsf(f)); lat = 12; break;
    case EFU_ERSQRT: r = 1.0f / sqrtf(fabsf(f)); lat = 18; break;
    case EFU_ERCPR: r = 1.0f / f; lat = 12; break;
    case EFU_ESIN: r = sinf(f); lat = 29; break;
    case EFU_EATAN: r = atanf(f); lat = 54; break;
    case EFU_EEXP: r = expf(-f); lat = 44; break;
    case EFU_EATANxy: r = atan2f(y, x); lat = 54; break;
    case EFU_EATANxz: r = atan2f(z, x); lat = 54; break;
    default: break;
    }
    v.p_pending = clampf(r);
    v.p_ready = n + lat;
    v.p_busy = true;
}

}  // namespace vu
