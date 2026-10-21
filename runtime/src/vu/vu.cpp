// VU0/VU1 micro-mode interpreter with simplified pipeline modelling
// (FMAC flag latency, Q/P latency, integer branch hazard).
#include "vu.h"
#include "../savestate.h"
#include <algorithm>
#include "vu_internal.h"
#include "../memory.h"
#include "../hw/gif.h"
#include "../ee/kernel.h"
#include <cmath>
#include <map>
#include <vector>
#include <string>
#include <set>
#include <windows.h>
#include <immintrin.h>

namespace vu {

static VUState g_vu[2];

void init() {
    for (int i = 0; i < 2; i++) {
        VUState& v = g_vu[i];
        memset(&v, 0, sizeof(v));
        v.idx = i;
        v.vf[0].f[3] = 1.0f;
        v.micro = i ? mem::vu1_micro : mem::vu0_micro;
        v.data = i ? mem::vu1_data : g_vu0_mem;
        v.micro_size = i ? 16384 : 4096;
        v.data_qw = i ? 1024 : 256;
        v.last_vi_reg = -1;
        v.r = 0x3F800000;
    }
}

void invalidate_micro(int idx) { g_vu[idx].dec_valid = false; }

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------
static u8 decode_upper(u32 u) {
    u32 f = u & 63;
    if (f < 0x3C) {
        static const u8 t1[0x3C] = {
            U_ADDbc, U_ADDbc, U_ADDbc, U_ADDbc, U_SUBbc, U_SUBbc, U_SUBbc, U_SUBbc,
            U_MADDbc, U_MADDbc, U_MADDbc, U_MADDbc, U_MSUBbc, U_MSUBbc, U_MSUBbc, U_MSUBbc,
            U_MAXbc, U_MAXbc, U_MAXbc, U_MAXbc, U_MINIbc, U_MINIbc, U_MINIbc, U_MINIbc,
            U_MULbc, U_MULbc, U_MULbc, U_MULbc, U_MULq, U_MAXi, U_MULi, U_MINIi,
            U_ADDq, U_MADDq, U_ADDi, U_MADDi, U_SUBq, U_MSUBq, U_SUBi, U_MSUBi,
            U_ADD, U_MADD, U_MUL, U_MAX, U_SUB, U_MSUB, U_OPMSUB, U_MINI,
            U_INVALID, U_INVALID, U_INVALID, U_INVALID, U_INVALID, U_INVALID, U_INVALID, U_INVALID,
            U_INVALID, U_INVALID, U_INVALID, U_INVALID};
        return t1[f];
    }
    u32 idx = (u & 3) | ((u >> 4) & 0x7C);
    switch (idx) {
    case 0x00: case 0x01: case 0x02: case 0x03: return U_ADDAbc;
    case 0x04: case 0x05: case 0x06: case 0x07: return U_SUBAbc;
    case 0x08: case 0x09: case 0x0A: case 0x0B: return U_MADDAbc;
    case 0x0C: case 0x0D: case 0x0E: case 0x0F: return U_MSUBAbc;
    case 0x10: return U_ITOF0;
    case 0x11: return U_ITOF4;
    case 0x12: return U_ITOF12;
    case 0x13: return U_ITOF15;
    case 0x14: return U_FTOI0;
    case 0x15: return U_FTOI4;
    case 0x16: return U_FTOI12;
    case 0x17: return U_FTOI15;
    case 0x18: case 0x19: case 0x1A: case 0x1B: return U_MULAbc;
    case 0x1C: return U_MULAq;
    case 0x1D: return U_ABS;
    case 0x1E: return U_MULAi;
    case 0x1F: return U_CLIP;
    case 0x20: return U_ADDAq;
    case 0x21: return U_MADDAq;
    case 0x22: return U_ADDAi;
    case 0x23: return U_MADDAi;
    case 0x24: return U_SUBAq;
    case 0x25: return U_MSUBAq;
    case 0x26: return U_SUBAi;
    case 0x27: return U_MSUBAi;
    case 0x28: return U_ADDA;
    case 0x29: return U_MADDA;
    case 0x2A: return U_MULA;
    case 0x2C: return U_SUBA;
    case 0x2D: return U_MSUBA;
    case 0x2E: return U_OPMULA;
    case 0x2F: return U_NOP;
    }
    return U_INVALID;
}

static u8 decode_lower(u32 l) {
    u32 op = l >> 25;
    switch (op) {
    case 0x00: return L_LQ;
    case 0x01: return L_SQ;
    case 0x04: return L_ILW;
    case 0x05: return L_ISW;
    case 0x08: return L_IADDIU;
    case 0x09: return L_ISUBIU;
    case 0x10: return L_FCEQ;
    case 0x11: return L_FCSET;
    case 0x12: return L_FCAND;
    case 0x13: return L_FCOR;
    case 0x14: return L_FSEQ;
    case 0x15: return L_FSSET;
    case 0x16: return L_FSAND;
    case 0x17: return L_FSOR;
    case 0x18: return L_FMEQ;
    case 0x1A: return L_FMAND;
    case 0x1B: return L_FMOR;
    case 0x1C: return L_FCGET;
    case 0x20: return L_B;
    case 0x21: return L_BAL;
    case 0x24: return L_JR;
    case 0x25: return L_JALR;
    case 0x28: return L_IBEQ;
    case 0x29: return L_IBNE;
    case 0x2C: return L_IBLTZ;
    case 0x2D: return L_IBGTZ;
    case 0x2E: return L_IBLEZ;
    case 0x2F: return L_IBGEZ;
    case 0x40: break;
    default: return L_INVALID;
    }
    u32 f = l & 63;
    switch (f) {
    case 0x30: return L_IADD;
    case 0x31: return L_ISUB;
    case 0x32: return L_IADDI;
    case 0x34: return L_IAND;
    case 0x35: return L_IOR;
    case 0x3C: case 0x3D: case 0x3E: case 0x3F: break;
    default: return L_INVALID;
    }
    u32 idx = (l & 3) | ((l >> 4) & 0x7C);
    switch (idx) {
    case 0x30: return L_MOVE;
    case 0x31: return L_MR32;
    case 0x34: return L_LQI;
    case 0x35: return L_SQI;
    case 0x36: return L_LQD;
    case 0x37: return L_SQD;
    case 0x38: return L_DIV;
    case 0x39: return L_SQRT;
    case 0x3A: return L_RSQRT;
    case 0x3B: return L_WAITQ;
    case 0x3C: return L_MTIR;
    case 0x3D: return L_MFIR;
    case 0x3E: return L_ILWR;
    case 0x3F: return L_ISWR;
    case 0x40: return L_RNEXT;
    case 0x41: return L_RGET;
    case 0x42: return L_RINIT;
    case 0x43: return L_RXOR;
    case 0x64: return L_MFP;
    case 0x68: return L_XTOP;
    case 0x69: return L_XITOP;
    case 0x6C: return L_XGKICK;
    case 0x70: return L_ESADD;
    case 0x71: return L_ERSADD;
    case 0x72: return L_ELENG;
    case 0x73: return L_ERLENG;
    case 0x74: return L_EATANxy;
    case 0x75: return L_EATANxz;
    case 0x76: return L_ESUM;
    case 0x78: return L_ESQRT;
    case 0x79: return L_ERSQRT;
    case 0x7A: return L_ERCPR;
    case 0x7B: return L_WAITP;
    case 0x7C: return L_ESIN;
    case 0x7D: return L_EATAN;
    case 0x7E: return L_EEXP;
    case 0x2F: case 0x2C: return L_NOP;  // (nop encodings)
    }
    if (l == 0x8000033C) return L_NOP;
    return L_INVALID;
}

static void decode_all(VUState& v) {
    u32 n = v.micro_size / 8;
    for (u32 i = 0; i < n; i++) {
        Decoded& d = v.dec[i];
        memcpy(&d.lower, v.micro + i * 8, 4);
        memcpy(&d.upper, v.micro + i * 8 + 4, 4);
        d.ibit = (d.upper >> 31) & 1;
        d.ebit = (d.upper >> 30) & 1;
        d.mbit = (d.upper >> 29) & 1;
        d.dbit = (d.upper >> 28) & 1;
        d.tbit = (d.upper >> 27) & 1;
        d.uop = decode_upper(d.upper);
        d.lop = d.ibit ? L_NOP : decode_lower(d.lower);
        if (!d.ibit && d.lower == 0x8000033C) d.lop = L_NOP;
        d.valid = true;
    }
    v.dec_valid = true;
}

// ---------------------------------------------------------------------------
// Flags / pipeline
// ---------------------------------------------------------------------------
static inline u32 mac_for(float r, int k) {
    u32 m = 0;
    u32 u;
    memcpy(&u, &r, 4);
    if ((u & 0x7FFFFFFF) == 0) m |= 1u << (3 - k);
    if (u & 0x80000000) m |= 1u << (7 - k);
    return m;
}

// ---------------------------------------------------------------------------
// Execution
// ---------------------------------------------------------------------------
struct PendingWrite {
    int vf = -1;
    vf128 val;
    bool acc = false;
    vf128 accval;
};

static inline bool dmask(u32 dest, int k) { return (dest >> (3 - k)) & 1; }

static void exec_upper(VUState& v, const Decoded& d, PendingWrite& pw) {
    u32 u = d.upper;
    if (d.uop == U_NOP) return;
    u32 dest = DEST(u);
    int ft = FT(u), fs = FS(u), fd = FD(u), bc = BC(u);
    const __m128 dm = dest_mask(dest);
    // result into fd (flags: FMAC ops except MAX/MINI update MAC/status)
    auto to_fd = [&](__m128 r, bool flags) {
        r = vsat(r);
        if (flags) push_flags(v, mac_of(r, dest));
        if (fd) { pw.vf = fd; pw.val.v = _mm_blendv_ps(v.vf[fd].v, r, dm); }
    };
    auto to_acc = [&](__m128 r) {
        r = vsat(r);
        push_flags(v, mac_of(r, dest));
        pw.acc = true;
        pw.accval.v = _mm_blendv_ps(v.acc.v, r, dm);
    };
    __m128 S = vsat(v.vf[fs].v);
    switch (d.uop) {
    case U_ADDbc: case U_SUBbc: case U_MADDbc: case U_MSUBbc: case U_MAXbc: case U_MINIbc: case U_MULbc:
    case U_ADDAbc: case U_SUBAbc: case U_MADDAbc: case U_MSUBAbc: case U_MULAbc: {
        __m128 b = _mm_set1_ps(inf(v.vf[ft].f[bc]));
        __m128 A = vsat(v.acc.v);
        switch (d.uop) {
        case U_ADDbc: to_fd(_mm_add_ps(S, b), true); break;
        case U_SUBbc: to_fd(_mm_sub_ps(S, b), true); break;
        case U_MADDbc: to_fd(_mm_add_ps(A, _mm_mul_ps(S, b)), true); break;
        case U_MSUBbc: to_fd(_mm_sub_ps(A, _mm_mul_ps(S, b)), true); break;
        case U_MAXbc: to_fd(_mm_max_ps(S, b), false); break;
        case U_MINIbc: to_fd(_mm_min_ps(S, b), false); break;
        case U_MULbc: to_fd(_mm_mul_ps(S, b), true); break;
        case U_ADDAbc: to_acc(_mm_add_ps(S, b)); break;
        case U_SUBAbc: to_acc(_mm_sub_ps(S, b)); break;
        case U_MADDAbc: to_acc(_mm_add_ps(A, _mm_mul_ps(S, b))); break;
        case U_MSUBAbc: to_acc(_mm_sub_ps(A, _mm_mul_ps(S, b))); break;
        default: to_acc(_mm_mul_ps(S, b)); break;  // MULAbc
        }
        return;
    }
    default: break;
    }
    const __m128 I = _mm_set1_ps(inf(v.ireg)), Q = _mm_set1_ps(inf(v.q));
    switch (d.uop) {
    case U_MULq: to_fd(_mm_mul_ps(S, Q), true); break;
    case U_MAXi: to_fd(_mm_max_ps(S, I), false); break;
    case U_MULi: to_fd(_mm_mul_ps(S, I), true); break;
    case U_MINIi: to_fd(_mm_min_ps(S, I), false); break;
    case U_ADDq: to_fd(_mm_add_ps(S, Q), true); break;
    case U_MADDq: to_fd(_mm_add_ps(vsat(v.acc.v), _mm_mul_ps(S, Q)), true); break;
    case U_ADDi: to_fd(_mm_add_ps(S, I), true); break;
    case U_MADDi: to_fd(_mm_add_ps(vsat(v.acc.v), _mm_mul_ps(S, I)), true); break;
    case U_SUBq: to_fd(_mm_sub_ps(S, Q), true); break;
    case U_MSUBq: to_fd(_mm_sub_ps(vsat(v.acc.v), _mm_mul_ps(S, Q)), true); break;
    case U_SUBi: to_fd(_mm_sub_ps(S, I), true); break;
    case U_MSUBi: to_fd(_mm_sub_ps(vsat(v.acc.v), _mm_mul_ps(S, I)), true); break;
    case U_ADD: to_fd(_mm_add_ps(S, vsat(v.vf[ft].v)), true); break;
    case U_MADD: to_fd(_mm_add_ps(vsat(v.acc.v), _mm_mul_ps(S, vsat(v.vf[ft].v))), true); break;
    case U_MUL: to_fd(_mm_mul_ps(S, vsat(v.vf[ft].v)), true); break;
    case U_MAX: to_fd(_mm_max_ps(S, vsat(v.vf[ft].v)), false); break;
    case U_SUB: to_fd(_mm_sub_ps(S, vsat(v.vf[ft].v)), true); break;
    case U_MSUB: to_fd(_mm_sub_ps(vsat(v.acc.v), _mm_mul_ps(S, vsat(v.vf[ft].v))), true); break;
    case U_MINI: to_fd(_mm_min_ps(S, vsat(v.vf[ft].v)), false); break;
    case U_OPMSUB: case U_OPMULA: {
        // outer product: (sy*tz, sz*tx, sx*ty) on x, y, z
        __m128 T = vsat(v.vf[ft].v);
        __m128 p = _mm_mul_ps(_mm_shuffle_ps(S, S, _MM_SHUFFLE(3, 0, 2, 1)), _mm_shuffle_ps(T, T, _MM_SHUFFLE(3, 1, 0, 2)));
        const u32 xyz = 0xE;
        __m128 m = dest_mask(xyz);
        if (d.uop == U_OPMSUB) {
            __m128 r = vsat(_mm_sub_ps(vsat(v.acc.v), p));
            push_flags(v, mac_of(r, xyz));
            if (fd) { pw.vf = fd; pw.val.v = _mm_blendv_ps(v.vf[fd].v, r, m); }
        } else {
            __m128 r = vsat(p);
            push_flags(v, mac_of(r, xyz));
            pw.acc = true;
            pw.accval.v = _mm_blendv_ps(v.acc.v, r, m);
        }
        break;
    }
    case U_MULAq: to_acc(_mm_mul_ps(S, Q)); break;
    case U_MULAi: to_acc(_mm_mul_ps(S, I)); break;
    case U_ADDAq: to_acc(_mm_add_ps(S, Q)); break;
    case U_MADDAq: to_acc(_mm_add_ps(vsat(v.acc.v), _mm_mul_ps(S, Q))); break;
    case U_ADDAi: to_acc(_mm_add_ps(S, I)); break;
    case U_MADDAi: to_acc(_mm_add_ps(vsat(v.acc.v), _mm_mul_ps(S, I))); break;
    case U_SUBAq: to_acc(_mm_sub_ps(S, Q)); break;
    case U_MSUBAq: to_acc(_mm_sub_ps(vsat(v.acc.v), _mm_mul_ps(S, Q))); break;
    case U_SUBAi: to_acc(_mm_sub_ps(S, I)); break;
    case U_MSUBAi: to_acc(_mm_sub_ps(vsat(v.acc.v), _mm_mul_ps(S, I))); break;
    case U_ADDA: to_acc(_mm_add_ps(S, vsat(v.vf[ft].v))); break;
    case U_MADDA: to_acc(_mm_add_ps(vsat(v.acc.v), _mm_mul_ps(S, vsat(v.vf[ft].v)))); break;
    case U_MULA: to_acc(_mm_mul_ps(S, vsat(v.vf[ft].v))); break;
    case U_SUBA: to_acc(_mm_sub_ps(S, vsat(v.vf[ft].v))); break;
    case U_MSUBA: to_acc(_mm_sub_ps(vsat(v.acc.v), _mm_mul_ps(S, vsat(v.vf[ft].v)))); break;
    case U_ITOF0: case U_ITOF4: case U_ITOF12: case U_ITOF15: {
        static const float sc[4] = {1.0f, 1.0f / 16, 1.0f / 4096, 1.0f / 32768};
        __m128 r = _mm_mul_ps(_mm_cvtepi32_ps(v.vf[fs].vi), _mm_set1_ps(sc[d.uop - U_ITOF0]));
        if (ft) { pw.vf = ft; pw.val.v = _mm_blendv_ps(v.vf[ft].v, r, dm); }
        break;
    }
    case U_FTOI0: case U_FTOI4: case U_FTOI12: case U_FTOI15: {
        static const float sc[4] = {1.0f, 16.0f, 4096.0f, 32768.0f};
        __m128 x = _mm_mul_ps(S, _mm_set1_ps(sc[d.uop - U_FTOI0]));
        __m128i xi = _mm_castps_si128(x);
        __m128i cv = _mm_cvttps_epi32(x);
        // out of int32 range: saturate by sign (matches fpu_cvtw)
        __m128i big = _mm_cmpgt_epi32(_mm_and_si128(xi, _mm_set1_epi32(0x7F800000)), _mm_set1_epi32(0x4E800000));
        __m128i sat = _mm_xor_si128(_mm_set1_epi32(0x7FFFFFFF), _mm_srai_epi32(xi, 31));
        __m128 r = _mm_castsi128_ps(_mm_blendv_epi8(cv, sat, big));
        if (ft) { pw.vf = ft; pw.val.v = _mm_blendv_ps(v.vf[ft].v, r, dm); }
        break;
    }
    case U_ABS: {
        __m128 r = _mm_and_ps(S, _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)));
        if (ft) { pw.vf = ft; pw.val.v = _mm_blendv_ps(v.vf[ft].v, r, dm); }
        break;
    }
    case U_CLIP: {
        const vf128& T = v.vf[ft];
        float w = fabsf(inf(T.f[3]));
        u32 f = 0;
        float x = inf(v.vf[fs].f[0]), y = inf(v.vf[fs].f[1]), z = inf(v.vf[fs].f[2]);
        if (x > w) f |= 0x01;
        if (x < -w) f |= 0x02;
        if (y > w) f |= 0x04;
        if (y < -w) f |= 0x08;
        if (z > w) f |= 0x10;
        if (z < -w) f |= 0x20;
        u32 nc = ((latest_clip(v) << 6) | f) & 0xFFFFFF;
        push_clip(v, nc);
        break;
    }
    default:
        break;
    }
}

extern u64 g_xgkicks;
bool g_xgkick_mute = false;  // verify mode: the second run of a program must not draw twice
u64 g_xgkick_muted = 0;
void xgkick(VUState& v, u32 addr_qw) {
    if (g_xgkick_mute) {
        g_xgkick_muted = g_xgkick_muted * 31 + addr_qw + 1;
        return;
    }
    g_xgkicks++;
    // determine packet length (until EOP) and pass a contiguous copy to the GIF
    u32 mask = v.data_qw - 1;
    u32 a = addr_qw & mask;
    u32 total = 0;
    for (int guard = 0; guard < 4096; guard++) {
        const u8* tp = v.data + ((a + total) & mask) * 16;
        u64 lo;
        memcpy(&lo, tp, 8);
        u64 hi;
        memcpy(&hi, tp + 8, 8);
        u32 nloop = lo & 0x7FFF;
        bool eop = (lo >> 15) & 1;
        u32 flg = (lo >> 58) & 3;
        u32 nreg = (lo >> 60) & 0xF;
        if (!nreg) nreg = 16;
        u32 len;
        if (flg == 0) len = nloop * nreg;
        else if (flg == 1) len = (nloop * nreg + 1) / 2;
        else len = nloop;
        total += 1 + len;
        if (eop) break;
        if (total > v.data_qw) break;
    }
    static std::vector<u8> buf;
    buf.resize(total * 16);
    for (u32 i = 0; i < total; i++) memcpy(&buf[i * 16], v.data + ((a + i) & mask) * 16, 16);
    gif::path1(buf.data(), total);
}

static inline void set_vi(VUState& v, int r, u32 val) {
    if (r == 0) return;
    v.last_vi_reg = r;
    v.last_vi_old = v.vi[r];
    v.vi[r] = val & 0xFFFF;
}

// VI writes of the most recently executed instructions (index 0 = previous instruction), used for the
// conditional-branch VI hazard.
enum { VIW_ALU = 0, VIW_LOAD = 1, VIW_FLAG = 2 };
struct ViWrite {
    int reg;
    u32 old;
    int kind;
};
struct ViHist {
    ViWrite w[4];
};

// Value of a VI register as seen by a conditional branch. A VI written by the immediately preceding
// integer instruction(s) is not yet visible: the branch sees the value before that chain of writes.
// ILW/ILWR results have latency, so the branch stalls and sees the new value; flag reads (FMAND etc.)
// do not trigger the hazard either.
static inline u32 branch_vi(VUState& v, int r, const ViHist& h) {
    if (r == 0) return 0;
    for (int i = 0; i < 3; i++)
        if (h.w[i].reg == r && h.w[i].kind == VIW_LOAD) return v.vi[r];
    if (h.w[0].reg != r || h.w[0].kind != VIW_ALU) return v.vi[r];
    u32 val = h.w[0].old;
    for (int i = 1; i < 4 && h.w[i].reg == r && h.w[i].kind == VIW_ALU; i++) val = h.w[i].old;
    return val;
}

static float efu_atan(float x) { return atanf(x); }

// returns: 0 = continue, 1 = branch taken (target in *target)
static int exec_lower(VUState& v, const Decoded& d, u32 pc, u32* target, PendingWrite& pw, const ViHist& vh) {
    u32 l = d.lower;
    u32 dest = DEST(l);
    int it = FT(l) & 15, is = FS(l) & 15, id = FD(l) & 15;
    int ft = FT(l), fs = FS(l);
    s32 imm11 = (s32)(l << 21) >> 21;
    v.last_vi_reg = -1;
    switch (d.lop) {
    case L_NOP: break;
    case L_LQ: {
        vf128* m = memq(v, v.vi[is] + imm11);
        if (ft) { pw.vf = ft; pw.val.v = _mm_blendv_ps(v.vf[ft].v, _mm_loadu_ps(m->f), dest_mask(dest)); }
        break;
    }
    case L_SQ: {
        vf128* m = memq(v, v.vi[it] + imm11);
        _mm_storeu_ps(m->f, _mm_blendv_ps(_mm_loadu_ps(m->f), v.vf[fs].v, dest_mask(dest)));
        break;
    }
    case L_ILW: {
        vf128* m = memq(v, v.vi[is] + imm11);
        int k = (dest & 8) ? 0 : (dest & 4) ? 1 : (dest & 2) ? 2 : 3;
        set_vi(v, it, m->u[k]);
        break;
    }
    case L_ISW: {
        vf128* m = memq(v, v.vi[is] + imm11);
        for (int k = 0; k < 4; k++) if (dmask(dest, k)) m->u[k] = v.vi[it] & 0xFFFF;
        break;
    }
    case L_IADDIU: set_vi(v, it, v.vi[is] + ((l & 0x7FF) | ((l >> 10) & 0x7800))); break;
    case L_ISUBIU: set_vi(v, it, v.vi[is] - ((l & 0x7FF) | ((l >> 10) & 0x7800))); break;
    case L_FCEQ: set_vi(v, 1, (v.clip & 0xFFFFFF) == (l & 0xFFFFFF)); break;
    case L_FCSET: v.clip = v.clip_issued = l & 0xFFFFFF; break;
    case L_FCAND: set_vi(v, 1, (v.clip & l & 0xFFFFFF) != 0); break;
    case L_FCOR: set_vi(v, 1, ((v.clip | l) & 0xFFFFFF) == 0xFFFFFF); break;
    case L_FSEQ: {
        u32 imm12 = ((l >> 10) & 0x800) | (l & 0x7FF);
        set_vi(v, it, ((v.status | v.status_sticky) & 0xFFF) == imm12);
        break;
    }
    case L_FSSET: {
        u32 imm12 = ((l >> 10) & 0x800) | (l & 0x7FF);
        v.status_sticky = imm12 & 0xFC0;
        break;
    }
    case L_FSAND: {
        u32 imm12 = ((l >> 10) & 0x800) | (l & 0x7FF);
        set_vi(v, it, (v.status | v.status_sticky) & imm12);
        break;
    }
    case L_FSOR: {
        u32 imm12 = ((l >> 10) & 0x800) | (l & 0x7FF);
        set_vi(v, it, (v.status | v.status_sticky) | imm12);
        break;
    }
    case L_FMEQ: set_vi(v, it, (v.mac & 0xFFFF) == (v.vi[is] & 0xFFFF)); break;
    case L_FMAND: set_vi(v, it, v.mac & v.vi[is]); break;
    case L_FMOR: set_vi(v, it, v.mac | v.vi[is]); break;
    case L_FCGET: set_vi(v, it, v.clip & 0xFFF); break;
    case L_B: *target = pc + 8 + imm11 * 8; return 1;
    case L_BAL: set_vi(v, it, (pc + 16) / 8); *target = pc + 8 + imm11 * 8; return 1;
    case L_JR: *target = v.vi[is] * 8; return 1;  // jumps read the current VI (no branch delay hazard)
    case L_JALR: {
        u32 t = v.vi[is] * 8;
        set_vi(v, it, (pc + 16) / 8);
        *target = t;
        return 1;
    }
    case L_IBEQ: case L_IBNE: case L_IBLTZ: case L_IBGTZ: case L_IBLEZ: case L_IBGEZ: {
        s16 a = (s16)branch_vi(v, is, vh);
        s16 b = (s16)branch_vi(v, it, vh);
        bool c;
        switch (d.lop) {
        case L_IBEQ: c = a == b; break;
        case L_IBNE: c = a != b; break;
        case L_IBLTZ: c = a < 0; break;
        case L_IBGTZ: c = a > 0; break;
        case L_IBLEZ: c = a <= 0; break;
        default: c = a >= 0; break;
        }
        if (c) { *target = pc + 8 + imm11 * 8; return 1; }
        break;
    }
    case L_IADD: set_vi(v, id, v.vi[is] + v.vi[it]); break;
    case L_ISUB: set_vi(v, id, v.vi[is] - v.vi[it]); break;
    case L_IADDI: { s32 imm5 = (s32)(l << 21) >> 27; set_vi(v, it, v.vi[is] + imm5); break; }
    case L_IAND: set_vi(v, id, v.vi[is] & v.vi[it]); break;
    case L_IOR: set_vi(v, id, v.vi[is] | v.vi[it]); break;
    case L_MOVE:
        if (ft) { pw.vf = ft; pw.val.v = _mm_blendv_ps(v.vf[ft].v, v.vf[fs].v, dest_mask(dest)); }
        break;
    case L_MR32: {
        vf128 r = v.vf[ft];
        const vf128& s = v.vf[fs];
        u32 rot[4] = {s.u[1], s.u[2], s.u[3], s.u[0]};
        for (int k = 0; k < 4; k++) if (dmask(dest, k)) r.u[k] = rot[k];
        if (ft) { pw.vf = ft; pw.val = r; }
        break;
    }
    case L_LQI: {
        vf128* m = memq(v, v.vi[is]);
        if (ft) { pw.vf = ft; pw.val.v = _mm_blendv_ps(v.vf[ft].v, _mm_loadu_ps(m->f), dest_mask(dest)); }
        set_vi(v, is, v.vi[is] + 1);
        break;
    }
    case L_SQI: {
        vf128* m = memq(v, v.vi[it]);
        _mm_storeu_ps(m->f, _mm_blendv_ps(_mm_loadu_ps(m->f), v.vf[fs].v, dest_mask(dest)));
        set_vi(v, it, v.vi[it] + 1);
        break;
    }
    case L_LQD: {
        set_vi(v, is, v.vi[is] - 1);
        vf128* m = memq(v, v.vi[is]);
        if (ft) { pw.vf = ft; pw.val.v = _mm_blendv_ps(v.vf[ft].v, _mm_loadu_ps(m->f), dest_mask(dest)); }
        break;
    }
    case L_SQD: {
        set_vi(v, it, v.vi[it] - 1);
        vf128* m = memq(v, v.vi[it]);
        _mm_storeu_ps(m->f, _mm_blendv_ps(_mm_loadu_ps(m->f), v.vf[fs].v, dest_mask(dest)));
        break;
    }
    case L_DIV: case L_SQRT: case L_RSQRT: {
        if (v.q_busy) { v.q = v.q_pending; v.q_busy = false; }
        int fsf = (l >> 21) & 3, ftf = (l >> 23) & 3;
        float a = inf(v.vf[fs].f[fsf]), b = inf(v.vf[ft].f[ftf]);
        float r;
        int lat;
        if (d.lop == L_DIV) {
            if (b == 0.0f) {
                u32 sign = ((*(u32*)&a ^ *(u32*)&b) & 0x80000000u);
                u32 ru = sign | 0x7F7FFFFF;
                memcpy(&r, &ru, 4);
                v.status_sticky |= 0x800;
            } else r = clampf(a / b);
            lat = 7;
        } else if (d.lop == L_SQRT) {
            r = sqrtf(fabsf(b));
            lat = 7;
        } else {
            float s = sqrtf(fabsf(b));
            if (s == 0.0f) { u32 ru = (*(u32*)&a & 0x80000000u) | 0x7F7FFFFF; memcpy(&r, &ru, 4); }
            else r = clampf(a / s);
            lat = 13;
        }
        v.q_pending = r;
        v.q_ready = v.cycle + lat;
        v.q_busy = true;
        break;
    }
    case L_WAITQ:
        if (v.q_busy) { v.q = v.q_pending; v.q_busy = false; }
        break;
    case L_MTIR: { int fsf = (l >> 21) & 3; set_vi(v, it, v.vf[fs].u[fsf] & 0xFFFF); break; }
    case L_MFIR: {
        vf128 r = v.vf[ft];
        for (int k = 0; k < 4; k++) if (dmask(dest, k)) r.s[k] = (s16)v.vi[is];
        if (ft) { pw.vf = ft; pw.val = r; }
        break;
    }
    case L_ILWR: {
        vf128* m = memq(v, v.vi[is]);
        int k = (dest & 8) ? 0 : (dest & 4) ? 1 : (dest & 2) ? 2 : 3;
        set_vi(v, it, m->u[k]);
        break;
    }
    case L_ISWR: {
        vf128* m = memq(v, v.vi[is]);
        for (int k = 0; k < 4; k++) if (dmask(dest, k)) m->u[k] = v.vi[it] & 0xFFFF;
        break;
    }
    case L_RNEXT: case L_RGET: {
        if (d.lop == L_RNEXT) {
            u32 x = v.r & 0x7FFFFF;
            int b = ((x >> 4) ^ (x >> 22)) & 1;
            v.r = ((x << 1) | b) & 0x7FFFFF;
        }
        vf128 r = v.vf[ft];
        u32 f = 0x3F800000u | (v.r & 0x7FFFFF);
        for (int k = 0; k < 4; k++) if (dmask(dest, k)) r.u[k] = f;
        if (ft) { pw.vf = ft; pw.val = r; }
        break;
    }
    case L_RINIT: v.r = v.vf[fs].u[(l >> 21) & 3] & 0x7FFFFF; break;
    case L_RXOR: v.r = (v.r ^ v.vf[fs].u[(l >> 21) & 3]) & 0x7FFFFF; break;
    case L_MFP: {
        vf128 r = v.vf[ft];
        for (int k = 0; k < 4; k++) if (dmask(dest, k)) r.f[k] = v.p;
        if (ft) { pw.vf = ft; pw.val = r; }
        break;
    }
    case L_XTOP: set_vi(v, it, v.top); break;
    case L_XITOP: set_vi(v, it, v.itop); break;
    case L_XGKICK: xgkick(v, v.vi[is]); break;
    case L_WAITP:
        if (v.p_busy) { v.p = v.p_pending; v.p_busy = false; }
        break;
    case L_ESADD: case L_ERSADD: case L_ELENG: case L_ERLENG: case L_ESUM: case L_ESQRT: case L_ERSQRT: case L_ERCPR:
    case L_ESIN: case L_EATAN: case L_EEXP: case L_EATANxy: case L_EATANxz: {
        if (v.p_busy) { v.p = v.p_pending; v.p_busy = false; }
        const vf128& s = v.vf[fs];
        float x = inf(s.f[0]), y = inf(s.f[1]), z = inf(s.f[2]), w = inf(s.f[3]);
        float f = inf(s.f[(l >> 21) & 3]);
        float r = 0;
        int lat = 12;
        switch (d.lop) {
        case L_ESADD: r = x * x + y * y + z * z; lat = 11; break;
        case L_ERSADD: r = 1.0f / (x * x + y * y + z * z); lat = 18; break;
        case L_ELENG: r = sqrtf(x * x + y * y + z * z); lat = 18; break;
        case L_ERLENG: r = 1.0f / sqrtf(x * x + y * y + z * z); lat = 24; break;
        case L_ESUM: r = x + y + z + w; lat = 12; break;
        case L_ESQRT: r = sqrtf(fabsf(f)); lat = 12; break;
        case L_ERSQRT: r = 1.0f / sqrtf(fabsf(f)); lat = 18; break;
        case L_ERCPR: r = 1.0f / f; lat = 12; break;
        case L_ESIN: r = sinf(f); lat = 29; break;
        case L_EATAN: r = efu_atan(f); lat = 54; break;
        case L_EEXP: r = expf(-f); lat = 44; break;
        case L_EATANxy: r = atan2f(y, x); lat = 54; break;
        case L_EATANxz: r = atan2f(z, x); lat = 54; break;
        default: break;
        }
        v.p_pending = clampf(r);
        v.p_ready = v.cycle + lat;
        v.p_busy = true;
        break;
    }
    case L_INVALID:
    default: {
        static int warned = 0;
        if (warned++ < 10) log_write(LOG_VU, "VU%d: invalid lower op %08x at %04x", v.idx, l, pc);
        break;
    }
    }
    return 0;
}

u64 g_vu_instrs = 0;
// debug history (VU0 runs on the emulation thread, VU1 on the GS pipeline thread)
static thread_local u32 g_pc_hist[2048];
static thread_local bool g_hist_done = false;
static thread_local u32 g_pc_ring[256];
static thread_local u32 g_pc_ring_pos = 0;
// ---------------------------------------------------------------------------
// Ahead-of-time translated images (generated/vu/*.cpp register themselves at startup)
// ---------------------------------------------------------------------------
static std::map<u64, AotFn>& aot_table() {
    static std::map<u64, AotFn> t;
    return t;
}
int register_aot(u64 image_hash, AotFn fn) {
    aot_table()[image_hash] = fn;
    return 0;
}
bool g_vu_aot = true;      // --vu-interp disables translated code
bool g_vu_verify = false;  // --vu-verify: run interpreter and translation, compare results

static void run_interp(VUState& v);

static void prepare(VUState& v) {
    if (v.dec_valid) return;
    decode_all(v);
    u64 h = 1469598103934665603ull;
    for (u32 i = 0; i < v.micro_size; i++) h = (h ^ v.micro[i]) * 1099511628211ull;
    v.image_hash = h;
    auto it = aot_table().find(h);
    v.aot = (v.idx == 1 && it != aot_table().end()) ? it->second : nullptr;
    LOG(LOG_VU, "VU%d image %016llx: %s", v.idx, (unsigned long long)h, v.aot ? "translated" : "interpreted");
    if (v.idx == 1 && !v.aot) {
        // microprogram image without a translation: keep a copy next to the executable (vu_new/) so it
        // can be translated ahead of time for the next build
        static std::set<u64> saved;
        if (saved.insert(h).second) {
            std::string dir = exe_dir() + "/vu_new";
            CreateDirectoryA(dir.c_str(), nullptr);
            char fn[64];
            snprintf(fn, sizeof(fn), "/vu1img_%016llx.bin", (unsigned long long)h);
            std::string path = dir + fn;
            if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
                if (FILE* f = fopen(path.c_str(), "wb")) { fwrite(v.micro, 1, v.micro_size, f); fclose(f); }
            log_write(LOG_VU, "VU1 image %016llx has no translation (interpreted), saved to vu_new", (unsigned long long)h);
        }
    }
}

static bool same_state(const VUState& a, const VUState& b, std::string& why) {
    char t[160];
    for (int i = 1; i < 32; i++)
        if (memcmp(&a.vf[i], &b.vf[i], 16)) {
            snprintf(t, sizeof(t), "vf%d %08x %08x %08x %08x vs %08x %08x %08x %08x", i, a.vf[i].u[0], a.vf[i].u[1], a.vf[i].u[2],
                     a.vf[i].u[3], b.vf[i].u[0], b.vf[i].u[1], b.vf[i].u[2], b.vf[i].u[3]);
            why = t;
            return false;
        }
    for (int i = 1; i < 16; i++)
        if (a.vi[i] != b.vi[i]) { snprintf(t, sizeof(t), "vi%d %04x vs %04x", i, a.vi[i], b.vi[i]); why = t; return false; }
    if (memcmp(&a.acc, &b.acc, 16)) { why = "acc"; return false; }
    if (memcmp(&a.q, &b.q, 4)) { why = "q"; return false; }
    if (memcmp(&a.p, &b.p, 4)) { why = "p"; return false; }
    if (a.mac != b.mac) { snprintf(t, sizeof(t), "mac %x vs %x", a.mac, b.mac); why = t; return false; }
    if (a.clip != b.clip) { snprintf(t, sizeof(t), "clip %x vs %x", a.clip, b.clip); why = t; return false; }
    // sticky status bits are not compared: translated code skips MAC flag updates no instruction can
    // observe (tools/vu_recomp.py, only for images without status flag reads)
    if (a.status != b.status) { why = "status"; return false; }
    if (a.pc != b.pc) { snprintf(t, sizeof(t), "pc %x vs %x", a.pc, b.pc); why = t; return false; }
    if (a.r != b.r) { why = "R"; return false; }
    return true;
}

static void run(VUState& v) {
    prepare(v);
    if (v.idx == 1 && v.aot) {  // entry points of translated programs (analysis)
        static std::vector<u32> seen;
        if (std::find(seen.begin(), seen.end(), v.pc) == seen.end()) {
            seen.push_back(v.pc);
            log_write(LOG_VU, "VU1 entry %04x (image %016llx)", v.pc, (unsigned long long)v.image_hash);
        }
    }
    if (!v.aot || !g_vu_aot) {
        run_interp(v);
        return;
    }
    if (!g_vu_verify) {
        u32 entry = v.pc;
        if (!v.aot(v)) {
            // a dispatch target the translation does not enter: the interpreter continues
            static int n = 0;
            if (n++ < 20) log_write(LOG_VU, "VU1 translation fallback at %04x (entry %04x)", v.pc, entry);
            run_interp(v);
        }
        return;
    }
    // verify: interpreter result is authoritative; the translation runs on a copy of the state
    static VUState* snap = new VUState;
    static std::vector<u8> mem0, mem1;
    u32 entry = v.pc;
    *snap = v;
    mem0.assign(v.data, v.data + v.data_qw * 16);
    run_interp(v);
    g_xgkick_muted = 0;
    u64 kicks_interp = 0;
    (void)kicks_interp;
    mem1.assign(v.data, v.data + v.data_qw * 16);
    VUState* res = new VUState(*snap);
    memcpy(v.data, mem0.data(), mem0.size());
    g_xgkick_mute = true;
    bool ok = res->aot(*res);
    g_xgkick_mute = false;
    std::string why;
    static int reported = 0;
    if (!ok) why = "translation refused entry";
    else if (!same_state(v, *res, why)) {
    } else if (memcmp(v.data, mem1.data(), mem1.size())) {
        for (size_t i = 0; i < mem1.size(); i++)
            if (v.data[i] != mem1[i]) {
                char t[64];
                snprintf(t, sizeof(t), "data byte %zx", i);
                why = t;
                break;
            }
    }
    if (!why.empty() && reported++ < 30)
        log_write(LOG_VU, "VU verify mismatch: image %016llx entry %04x: %s", (unsigned long long)v.image_hash, entry, why.c_str());
    memcpy(v.data, mem1.data(), mem1.size());  // keep the interpreter's results
    delete res;
}

static void run_interp(VUState& v) {
    u32 pc = v.pc;
    u32 start_pc = pc;
    u64 executed = 0;
    bool end_pending = false;
    bool branch_pending = false;
    u32 branch_target = 0;
    ViHist vh;
    for (auto& w : vh.w) w = ViWrite{-1, 0, VIW_ALU};
    u32 mask = v.micro_size - 8;
    for (u64 guard = 0; guard < 50000000ull; guard++) {
        const Decoded& d = v.dec[(pc & mask) / 8];
        g_pc_ring[g_pc_ring_pos++ & 255] = pc;
        if (guard >= 49000000ull && !g_hist_done) g_pc_hist[(pc & mask) / 8]++;
        v.cycle++;
        advance_pipeline(v);
        PendingWrite upw, lpw;
        u32 tgt = 0;
        // upper reads current registers; lower too; then commit (upper wins on conflict)
        exec_upper(v, d, upw);
        int taken = 0;
        ViWrite cur{-1, 0, VIW_ALU};
        if (d.ibit) {
            memcpy(&v.ireg, &d.lower, 4);
        } else if (d.lop != L_NOP) {
            v.last_vi_reg = -1;
            taken = exec_lower(v, d, pc, &tgt, lpw, vh);
            cur.reg = v.last_vi_reg;
            cur.old = v.last_vi_old;
            switch (d.lop) {
            case L_ILW: case L_ILWR: cur.kind = VIW_LOAD; break;
            case L_FSEQ: case L_FSAND: case L_FSOR: case L_FMEQ: case L_FMAND: case L_FMOR:
            case L_FCEQ: case L_FCAND: case L_FCOR: case L_FCGET: cur.kind = VIW_FLAG; break;
            default: break;
            }
        }
        if (lpw.vf > 0) v.vf[lpw.vf] = lpw.val;
        if (upw.vf > 0) v.vf[upw.vf] = upw.val;
        if (upw.acc) v.acc = upw.accval;
        vh.w[3] = vh.w[2];
        vh.w[2] = vh.w[1];
        vh.w[1] = vh.w[0];
        vh.w[0] = cur;
        u32 next = pc + 8;
        if (branch_pending) {
            next = branch_target;
            branch_pending = false;
        }
        if (taken) {
            branch_pending = true;
            branch_target = tgt & mask;
        }
        executed++;
        if (end_pending) {
            v.pc = next & mask;
            break;
        }
        if (d.ebit) end_pending = true;
        pc = next & mask;
    }
    flush_pipeline(v);
    g_vu_instrs += executed;
    static int logged = 0, slow = 0;
    if (executed >= 50000000ull) {
        static bool dumped = false;
        if (!dumped) {
            dumped = true;
            if (FILE* f = fopen("vu1_micro.bin", "wb")) { fwrite(v.micro, 1, v.micro_size, f); fclose(f); }
            if (FILE* f = fopen("vu1_data.bin", "wb")) { fwrite(v.data, 1, v.data_qw * 16, f); fclose(f); }
            std::string s;
            char b[32];
            for (int i = 0; i < 16; i++) { snprintf(b, sizeof(b), "vi%d=%04x ", i, v.vi[i]); s += b; }
            log_write(LOG_VU, "VU%d stuck at pc %04x: %s", v.idx, pc, s.c_str());
            s.clear();
            for (int i = 0; i < 256; i++) { snprintf(b, sizeof(b), "%04x ", g_pc_ring[(g_pc_ring_pos + i) & 255]); s += b; }
            log_write(LOG_VU, "last pcs: %s", s.c_str());
            g_hist_done = true;
            s.clear();
            for (int i = 0; i < 2048; i++)
                if (g_pc_hist[i] > 1000) { snprintf(b, sizeof(b), "%04x:%u ", i * 8, g_pc_hist[i]); s += b; }
            log_write(LOG_VU, "hot pcs (last 1M): %s", s.c_str());
        }
    }
    if (executed >= 1000000 && slow++ < 20)
        log_write(LOG_VU, "VU%d: long run from %04x: %llu instrs, end pc %04x (guard hit=%d)", v.idx, start_pc,
                  (unsigned long long)executed, v.pc, executed >= 50000000ull);
    if (logged++ < 40) LOG(LOG_VU, "VU%d ran from %04x: %llu instrs, end pc %04x, kicks so far %llu", v.idx, start_pc,
                           (unsigned long long)executed, v.pc, (unsigned long long)g_xgkicks);
}

void vu1_set_top(u32 top, u32 itop) {
    g_vu[1].top = top;
    g_vu[1].itop = itop;
}
void vu0_set_itop(u32 itop) { g_vu[0].itop = itop; }

u64 g_vu1_starts = 0, g_xgkicks = 0;
// --vu-dump: record every distinct VU1 micro memory image (and the entry points used with it)
bool g_vu_dump = false;
static void note_image(VUState& v, u32 addr) {
    static u64 cur_hash = 0;
    static bool dirty = true;
    static std::map<u64, std::set<u32>> seen;
    if (!v.dec_valid) dirty = true;
    if (dirty) {
        u64 h = 1469598103934665603ull;
        for (u32 i = 0; i < v.micro_size; i++) h = (h ^ v.micro[i]) * 1099511628211ull;
        cur_hash = h;
        dirty = false;
        if (!seen.count(h)) {
            char fn[64];
            snprintf(fn, sizeof(fn), "vu1img_%016llx.bin", (unsigned long long)h);
            if (FILE* f = fopen(fn, "wb")) { fwrite(v.micro, 1, v.micro_size, f); fclose(f); }
            log_write(LOG_VU, "VU1 image %016llx (%zu distinct)", (unsigned long long)h, seen.size() + 1);
        }
    }
    auto& entries = seen[cur_hash];
    if (entries.insert(addr).second) {
        FILE* f = fopen("vu1_entries.txt", "a");
        if (f) { fprintf(f, "%016llx %04x\n", (unsigned long long)cur_hash, addr); fclose(f); }
    }
}

void vu1_start(u32 addr) {
    if (g_vu_dump) note_image(g_vu[1], addr & (g_vu[1].micro_size - 8));
    g_vu1_starts++;
    g_vu[1].pc = addr & (g_vu[1].micro_size - 8);
    run(g_vu[1]);
}
void vu1_continue() { run(g_vu[1]); }

void vu0_start(u32 addr) {
    g_vu[0].pc = addr & (g_vu[0].micro_size - 8);
    run(g_vu[0]);
}
void vu0_continue() { run(g_vu[0]); }

void vu0_callms_ctx(EEContext* c, u32 addr) {
    VUState& v = g_vu[0];
    // share VU0 register file with the EE context (macro mode registers)
    memcpy(v.vf, c->vf, sizeof(v.vf));
    v.acc = c->vacc;
    for (int i = 0; i < 16; i++) v.vi[i] = c->vi[i] & 0xFFFF;
    v.q = c->vq;
    v.ireg = c->vI;
    v.r = c->vr;
    v.clip = c->vclip;
    v.pc = addr & (v.micro_size - 8);
    run(v);
    memcpy(c->vf, v.vf, sizeof(v.vf));
    c->vacc = v.acc;
    for (int i = 1; i < 16; i++) c->vi[i] = v.vi[i];
    c->vq = v.q;
    c->vI = v.ireg;
    c->vr = v.r;
    c->vclip = v.clip;
    c->vmac = v.mac;
    c->vstatus = v.status | v.status_sticky;
}

}  // namespace vu
bool vu_dump_flag() { vu::g_vu_dump = true; return true; }
void vu_set_mode(int mode) {
    vu::g_vu_aot = mode != 0;
    vu::g_vu_verify = mode == 2;
}

namespace vu {
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    for (VUState& v : g_vu) {
        s.arr(v.vf); s.v(v.acc); s.arr(v.vi); s.v(v.q); s.v(v.p); s.v(v.ireg); s.v(v.r);
        s.v(v.mac); s.v(v.status); s.v(v.clip); s.v(v.status_sticky);
        for (auto& e : v.pend) s.pod(e, "vu::FlagEntry");
        s.v(v.phead); s.v(v.ptail); s.v(v.clip_issued);
        s.v(v.q_pending); s.v(v.q_ready); s.v(v.q_busy); s.v(v.p_pending); s.v(v.p_ready); s.v(v.p_busy);
        s.v(v.cycle); s.v(v.pc); s.v(v.top); s.v(v.itop); s.v(v.last_vi_reg); s.v(v.last_vi_old);
        if (s.loading) { v.dec_valid = false; v.aot = nullptr; }  // micro memory was restored
    }
}
}  // namespace vu
