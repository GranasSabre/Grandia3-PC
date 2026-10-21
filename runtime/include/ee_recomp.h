/* Runtime interface for recompiled Emotion Engine code.
 * Included by generated C code and by the C++ runtime. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <immintrin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EE_LIKELY(x) __builtin_expect(!!(x), 1)
#define EE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define EE_INLINE static inline __attribute__((always_inline))

typedef union r128 {
    uint64_t ud[2];
    int64_t sd[2];
    uint32_t uw[4];
    int32_t sw[4];
    uint16_t uh[8];
    int16_t sh[8];
    uint8_t ub[16];
    int8_t sb[16];
    __m128i v;
} __attribute__((aligned(16))) r128;

typedef union vf128 {
    float f[4];
    uint32_t u[4];
    int32_t s[4];
    __m128 v;
    __m128i vi;
} __attribute__((aligned(16))) vf128;

typedef union fpr32 {
    float f;
    uint32_t u;
    int32_t s;
} fpr32;

typedef struct EEContext {
    r128 r[32];
    r128 hi, lo;
    r128 scratch;
    uint64_t sa;
    fpr32 fpr[32];
    float facc;
    uint32_t fcond;
    uint32_t fcr31;
    /* VU0 (macro mode) */
    vf128 vf[32];
    vf128 vacc;
    uint32_t vi[16];
    float vq, vp;
    uint32_t vr;      /* R register (random) */
    float vI;         /* I register */
    uint32_t vstatus, vmac, vclip;
    uint32_t cop0[32];
    /* runtime bookkeeping */
    uint32_t cur_func;
    int thread_id;
    void* thread;
} EEContext;

typedef void (*EEFunc)(EEContext*);

typedef struct EEFuncEntry {
    uint32_t start;
    uint32_t end;
    EEFunc fn;    /* what callers use (a hook or HLE replacement, else the body) */
    EEFunc body;  /* the recompiled body with the resume points (0: none) */
} EEFuncEntry;

/* ------------------------------------------------------------------ */
/* Memory                                                               */
/* ------------------------------------------------------------------ */
/* Page map in 16MB granularity. NULL entry => slow I/O path. */
extern uint8_t* g_ee_pagemap[256];
extern uint8_t* g_ee_ram;            /* 32MB main RAM */
extern volatile int g_ee_irq_pending;
extern int g_ee_patch[8];               /* optional code patches (config "patches"), e.g. [0] widescreen */

uint8_t ee_io_rd8(uint32_t a);
uint16_t ee_io_rd16(uint32_t a);
uint32_t ee_io_rd32(uint32_t a);
uint64_t ee_io_rd64(uint32_t a);
void ee_io_rd128(uint32_t a, r128* out);
void ee_io_wr8(uint32_t a, uint8_t v);
void ee_io_wr16(uint32_t a, uint16_t v);
void ee_io_wr32(uint32_t a, uint32_t v);
void ee_io_wr64(uint32_t a, uint64_t v);
void ee_io_wr128(uint32_t a, const r128* v);

EE_INLINE uint8_t* ee_hptr(uint32_t a) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    return p ? p + (a & 0xFFFFFF) : 0;
}

EE_INLINE uint8_t ee_rd8(uint32_t a) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    if (EE_LIKELY(p)) return *(uint8_t*)(p + (a & 0xFFFFFF));
    return ee_io_rd8(a);
}
EE_INLINE uint16_t ee_rd16(uint32_t a) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    if (EE_LIKELY(p)) { uint16_t v; memcpy(&v, p + (a & 0xFFFFFF), 2); return v; }
    return ee_io_rd16(a);
}
EE_INLINE uint32_t ee_rd32(uint32_t a) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    if (EE_LIKELY(p)) { uint32_t v; memcpy(&v, p + (a & 0xFFFFFF), 4); return v; }
    return ee_io_rd32(a);
}
EE_INLINE uint64_t ee_rd64(uint32_t a) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    if (EE_LIKELY(p)) { uint64_t v; memcpy(&v, p + (a & 0xFFFFF8), 8); return v; }
    return ee_io_rd64(a & ~7u);
}
EE_INLINE void ee_rd128(uint32_t a, r128* out) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    if (EE_LIKELY(p)) { out->v = _mm_load_si128((const __m128i*)(p + (a & 0xFFFFF0))); return; }
    ee_io_rd128(a & ~15u, out);
}
EE_INLINE void ee_wr8(uint32_t a, uint8_t v) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    if (EE_LIKELY(p)) { p[a & 0xFFFFFF] = v; return; }
    ee_io_wr8(a, v);
}
EE_INLINE void ee_wr16(uint32_t a, uint16_t v) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    if (EE_LIKELY(p)) { memcpy(p + (a & 0xFFFFFF), &v, 2); return; }
    ee_io_wr16(a, v);
}
EE_INLINE void ee_wr32(uint32_t a, uint32_t v) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    if (EE_LIKELY(p)) { memcpy(p + (a & 0xFFFFFF), &v, 4); return; }
    ee_io_wr32(a, v);
}
EE_INLINE void ee_wr64(uint32_t a, uint64_t v) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    if (EE_LIKELY(p)) { memcpy(p + (a & 0xFFFFF8), &v, 8); return; }
    ee_io_wr64(a & ~7u, v);
}
EE_INLINE void ee_wr128(uint32_t a, const r128* v) {
    uint8_t* p = g_ee_pagemap[a >> 24];
    if (EE_LIKELY(p)) { _mm_store_si128((__m128i*)(p + (a & 0xFFFFF0)), v->v); return; }
    ee_io_wr128(a & ~15u, v);
}

/* unaligned load/store helpers (little endian) */
EE_INLINE void ee_lwl(EEContext* c, int rt, uint32_t addr) {
    static const uint32_t MASK[4] = {0x00ffffff, 0x0000ffff, 0x000000ff, 0x00000000};
    static const uint8_t SH[4] = {24, 16, 8, 0};
    uint32_t s = addr & 3;
    uint32_t mem = ee_rd32(addr & ~3u);
    if (!rt) return;
    c->r[rt].sd[0] = (int32_t)((c->r[rt].uw[0] & MASK[s]) | (mem << SH[s]));
}
EE_INLINE void ee_lwr(EEContext* c, int rt, uint32_t addr) {
    static const uint32_t MASK[4] = {0x00000000, 0xff000000, 0xffff0000, 0xffffff00};
    static const uint8_t SH[4] = {0, 8, 16, 24};
    uint32_t s = addr & 3;
    uint32_t mem = ee_rd32(addr & ~3u);
    if (!rt) return;
    mem = (c->r[rt].uw[0] & MASK[s]) | (mem >> SH[s]);
    if (s == 0) c->r[rt].sd[0] = (int32_t)mem;
    else c->r[rt].uw[0] = mem;
}
EE_INLINE void ee_ldl(EEContext* c, int rt, uint32_t addr) {
    static const uint64_t MASK[8] = {0x00ffffffffffffffull, 0x0000ffffffffffffull, 0x000000ffffffffffull,
        0x00000000ffffffffull, 0x0000000000ffffffull, 0x000000000000ffffull, 0x00000000000000ffull, 0};
    static const uint8_t SH[8] = {56, 48, 40, 32, 24, 16, 8, 0};
    uint32_t s = addr & 7;
    uint64_t mem = ee_rd64(addr & ~7u);
    if (!rt) return;
    c->r[rt].ud[0] = (c->r[rt].ud[0] & MASK[s]) | (mem << SH[s]);
}
EE_INLINE void ee_ldr(EEContext* c, int rt, uint32_t addr) {
    static const uint64_t MASK[8] = {0, 0xff00000000000000ull, 0xffff000000000000ull, 0xffffff0000000000ull,
        0xffffffff00000000ull, 0xffffffffff000000ull, 0xffffffffffff0000ull, 0xffffffffffffff00ull};
    static const uint8_t SH[8] = {0, 8, 16, 24, 32, 40, 48, 56};
    uint32_t s = addr & 7;
    uint64_t mem = ee_rd64(addr & ~7u);
    if (!rt) return;
    c->r[rt].ud[0] = (c->r[rt].ud[0] & MASK[s]) | (mem >> SH[s]);
}
EE_INLINE void ee_swl(EEContext* c, int rt, uint32_t addr) {
    static const uint32_t MASK[4] = {0xffffff00, 0xffff0000, 0xff000000, 0x00000000};
    static const uint8_t SH[4] = {24, 16, 8, 0};
    uint32_t s = addr & 3;
    uint32_t mem = ee_rd32(addr & ~3u);
    ee_wr32(addr & ~3u, (c->r[rt].uw[0] >> SH[s]) | (mem & MASK[s]));
}
EE_INLINE void ee_swr(EEContext* c, int rt, uint32_t addr) {
    static const uint32_t MASK[4] = {0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff};
    static const uint8_t SH[4] = {0, 8, 16, 24};
    uint32_t s = addr & 3;
    uint32_t mem = ee_rd32(addr & ~3u);
    ee_wr32(addr & ~3u, (c->r[rt].uw[0] << SH[s]) | (mem & MASK[s]));
}
EE_INLINE void ee_sdl(EEContext* c, int rt, uint32_t addr) {
    static const uint64_t MASK[8] = {0xffffffffffffff00ull, 0xffffffffffff0000ull, 0xffffffffff000000ull,
        0xffffffff00000000ull, 0xffffff0000000000ull, 0xffff000000000000ull, 0xff00000000000000ull, 0};
    static const uint8_t SH[8] = {56, 48, 40, 32, 24, 16, 8, 0};
    uint32_t s = addr & 7;
    uint64_t mem = ee_rd64(addr & ~7u);
    ee_wr64(addr & ~7u, (c->r[rt].ud[0] >> SH[s]) | (mem & MASK[s]));
}
EE_INLINE void ee_sdr(EEContext* c, int rt, uint32_t addr) {
    static const uint64_t MASK[8] = {0, 0x00000000000000ffull, 0x000000000000ffffull, 0x0000000000ffffffull,
        0x00000000ffffffffull, 0x000000ffffffffffull, 0x0000ffffffffffffull, 0x00ffffffffffffffull};
    static const uint8_t SH[8] = {0, 8, 16, 24, 32, 40, 48, 56};
    uint32_t s = addr & 7;
    uint64_t mem = ee_rd64(addr & ~7u);
    ee_wr64(addr & ~7u, (c->r[rt].ud[0] << SH[s]) | (mem & MASK[s]));
}

/* ------------------------------------------------------------------ */
/* Control / system                                                     */
/* ------------------------------------------------------------------ */
void ee_call(EEContext* c, uint32_t addr);          /* indirect call via dispatcher */
void ee_syscall(EEContext* c, uint32_t pc);
void ee_break(EEContext* c, uint32_t pc);
void ee_invalid(EEContext* c, uint32_t pc, uint32_t word);
void ee_eret(EEContext* c, uint32_t pc);
uint32_t ee_mfc0(EEContext* c, int reg);
void ee_mtc0(EEContext* c, int reg, uint32_t v);
void ee_ei(EEContext* c);
void ee_di(EEContext* c);
int ee_cpcond0(EEContext* c);
void ee_irq_check(EEContext* c);
uint32_t ee_cfc2(EEContext* c, int reg);
void ee_ctc2(EEContext* c, int reg, uint32_t v);
void vu0_callms(EEContext* c, uint32_t addr);
void vu0_callmsr(EEContext* c);
int vu0_cpcond(EEContext* c);

#define EE_LOOP_CHECK(c) do { if (EE_UNLIKELY(g_ee_irq_pending)) ee_irq_check(c); } while (0)
/* interrupt check on a backward branch; target = where a preempted thread continues after a restore */
void ee_irq_check_at(EEContext* c, uint32_t target);
#define EE_LOOP_CHECK_AT(c, t) do { if (EE_UNLIKELY(g_ee_irq_pending)) ee_irq_check_at(c, t); } while (0)
/* busy-wait loop (config "idle_loops"): the host thread sleeps until the next event */
void ee_idle_loop(EEContext* c, uint32_t target);

/* Savestate resume: a restored thread re-enters the recompiled bodies of its MIPS call chain. The
   function entry takes g_ee_resume (a resume point inside it) and jumps there. */
extern uint32_t g_ee_resume;
EE_INLINE uint32_t ee_resume_take(void) { uint32_t r = g_ee_resume; g_ee_resume = 0; return r; }
void ee_resume_fail(EEContext* c, uint32_t func);
#ifdef EE_TRACE_FUNCS
#define EE_FUNC_ENTRY(c, a) ((c)->cur_func = (a))
#else
#define EE_FUNC_ENTRY(c, a) ((void)0)
#endif

/* ------------------------------------------------------------------ */
/* Multiply / divide                                                    */
/* ------------------------------------------------------------------ */
EE_INLINE void ee_set_lohi(EEContext* c, int pipe, int rd, uint32_t lo, uint32_t hi) {
    c->lo.sd[pipe] = (int32_t)lo;
    c->hi.sd[pipe] = (int32_t)hi;
    if (rd) c->r[rd].sd[0] = (int32_t)lo;
}
EE_INLINE void ee_mult(EEContext* c, int rd, int rs, int rt) {
    int64_t r = (int64_t)c->r[rs].sw[0] * (int64_t)c->r[rt].sw[0];
    ee_set_lohi(c, 0, rd, (uint32_t)r, (uint32_t)(r >> 32));
}
EE_INLINE void ee_multu(EEContext* c, int rd, int rs, int rt) {
    uint64_t r = (uint64_t)c->r[rs].uw[0] * (uint64_t)c->r[rt].uw[0];
    ee_set_lohi(c, 0, rd, (uint32_t)r, (uint32_t)(r >> 32));
}
EE_INLINE void ee_mult1(EEContext* c, int rd, int rs, int rt) {
    int64_t r = (int64_t)c->r[rs].sw[0] * (int64_t)c->r[rt].sw[0];
    ee_set_lohi(c, 1, rd, (uint32_t)r, (uint32_t)(r >> 32));
}
EE_INLINE void ee_multu1(EEContext* c, int rd, int rs, int rt) {
    uint64_t r = (uint64_t)c->r[rs].uw[0] * (uint64_t)c->r[rt].uw[0];
    ee_set_lohi(c, 1, rd, (uint32_t)r, (uint32_t)(r >> 32));
}
EE_INLINE void ee_madd(EEContext* c, int rd, int rs, int rt) {
    int64_t acc = (int64_t)(((uint64_t)c->hi.uw[0] << 32) | c->lo.uw[0]);
    int64_t r = acc + (int64_t)c->r[rs].sw[0] * (int64_t)c->r[rt].sw[0];
    ee_set_lohi(c, 0, rd, (uint32_t)r, (uint32_t)((uint64_t)r >> 32));
}
EE_INLINE void ee_maddu(EEContext* c, int rd, int rs, int rt) {
    uint64_t acc = ((uint64_t)c->hi.uw[0] << 32) | c->lo.uw[0];
    uint64_t r = acc + (uint64_t)c->r[rs].uw[0] * (uint64_t)c->r[rt].uw[0];
    ee_set_lohi(c, 0, rd, (uint32_t)r, (uint32_t)(r >> 32));
}
EE_INLINE void ee_madd1(EEContext* c, int rd, int rs, int rt) {
    int64_t acc = (int64_t)(((uint64_t)c->hi.uw[2] << 32) | c->lo.uw[2]);
    int64_t r = acc + (int64_t)c->r[rs].sw[0] * (int64_t)c->r[rt].sw[0];
    ee_set_lohi(c, 1, rd, (uint32_t)r, (uint32_t)((uint64_t)r >> 32));
}
EE_INLINE void ee_maddu1(EEContext* c, int rd, int rs, int rt) {
    uint64_t acc = ((uint64_t)c->hi.uw[2] << 32) | c->lo.uw[2];
    uint64_t r = acc + (uint64_t)c->r[rs].uw[0] * (uint64_t)c->r[rt].uw[0];
    ee_set_lohi(c, 1, rd, (uint32_t)r, (uint32_t)(r >> 32));
}
EE_INLINE void ee_div_pipe(EEContext* c, int pipe, int rs, int rt) {
    int32_t a = c->r[rs].sw[0], b = c->r[rt].sw[0];
    if (b == 0) {
        c->lo.sd[pipe] = (a < 0) ? 1 : -1;
        c->hi.sd[pipe] = a;
    } else if (a == (int32_t)0x80000000 && b == -1) {
        c->lo.sd[pipe] = (int32_t)0x80000000;
        c->hi.sd[pipe] = 0;
    } else {
        c->lo.sd[pipe] = a / b;
        c->hi.sd[pipe] = a % b;
    }
}
EE_INLINE void ee_divu_pipe(EEContext* c, int pipe, int rs, int rt) {
    uint32_t a = c->r[rs].uw[0], b = c->r[rt].uw[0];
    if (b == 0) {
        c->lo.sd[pipe] = -1;
        c->hi.sd[pipe] = (int32_t)a;
    } else {
        c->lo.sd[pipe] = (int32_t)(a / b);
        c->hi.sd[pipe] = (int32_t)(a % b);
    }
}
EE_INLINE void ee_div(EEContext* c, int rd, int rs, int rt) { (void)rd; ee_div_pipe(c, 0, rs, rt); }
EE_INLINE void ee_divu(EEContext* c, int rd, int rs, int rt) { (void)rd; ee_divu_pipe(c, 0, rs, rt); }
EE_INLINE void ee_div1(EEContext* c, int rd, int rs, int rt) { (void)rd; ee_div_pipe(c, 1, rs, rt); }
EE_INLINE void ee_divu1(EEContext* c, int rd, int rs, int rt) { (void)rd; ee_divu_pipe(c, 1, rs, rt); }

/* ------------------------------------------------------------------ */
/* FPU (COP1) - PS2 FPU has no Inf/NaN; results clamp to +-FLT_MAX      */
/* ------------------------------------------------------------------ */
EE_INLINE float fpu_clamp(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    if (EE_UNLIKELY((u & 0x7F800000u) == 0x7F800000u)) { u = (u & 0x80000000u) | 0x7F7FFFFFu; memcpy(&f, &u, 4); }
    return f;
}
EE_INLINE float fpu_in(float f) {
    /* treat Inf/NaN bit patterns as max-magnitude numbers */
    uint32_t u; memcpy(&u, &f, 4);
    if (EE_UNLIKELY((u & 0x7F800000u) == 0x7F800000u)) { u = (u & 0x80000000u) | 0x7F7FFFFFu; memcpy(&f, &u, 4); }
    return f;
}
EE_INLINE float fpu_add(float a, float b) { return fpu_clamp(fpu_in(a) + fpu_in(b)); }
EE_INLINE float fpu_sub(float a, float b) { return fpu_clamp(fpu_in(a) - fpu_in(b)); }
EE_INLINE float fpu_mul(float a, float b) { return fpu_clamp(fpu_in(a) * fpu_in(b)); }
EE_INLINE float fpu_div(EEContext* c, float a, float b) {
    (void)c;
    if (EE_UNLIKELY(b == 0.0f)) {
        uint32_t ua, ub, r; memcpy(&ua, &a, 4); memcpy(&ub, &b, 4);
        r = ((ua ^ ub) & 0x80000000u) | 0x7F7FFFFFu;
        float f; memcpy(&f, &r, 4); return f;
    }
    return fpu_clamp(fpu_in(a) / fpu_in(b));
}
EE_INLINE float fpu_sqrt(EEContext* c, float a) { (void)c; return sqrtf(fabsf(fpu_in(a))); }
EE_INLINE float fpu_rsqrt(EEContext* c, float a, float b) {
    float s = sqrtf(fabsf(fpu_in(b)));
    return fpu_div(c, a, s);
}
EE_INLINE int32_t fpu_cvtw(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    if ((u & 0x7F800000u) <= 0x4E800000u) return (int32_t)f;
    return (u & 0x80000000u) ? (int32_t)0x80000000 : 0x7FFFFFFF;
}
EE_INLINE float fpu_max(float a, float b) { return a > b ? a : b; }
EE_INLINE float fpu_min(float a, float b) { return a < b ? a : b; }
uint32_t ee_cfc1(EEContext* c, int reg);
void ee_ctc1(EEContext* c, int reg, uint32_t v);

/* ------------------------------------------------------------------ */
/* VU0 macro instructions                                              */
/* ------------------------------------------------------------------ */
EE_INLINE float vu_clamp(float f) { return fpu_clamp(f); }
EE_INLINE float vu_in(float f) { return fpu_in(f); }

#define VU_DEST_LOOP(dest, body) do { \
    if ((dest) & 8) { const int k = 0; body; } \
    if ((dest) & 4) { const int k = 1; body; } \
    if ((dest) & 2) { const int k = 2; body; } \
    if ((dest) & 1) { const int k = 3; body; } } while (0)

/* compute into temp then commit, so fd may alias fs/ft */
#define VU_OP3(name, expr) \
EE_INLINE void vu0_##name(EEContext* c, int dest, int fd, int fs, int ft) { \
    vf128 t = c->vf[fd]; const vf128 s = c->vf[fs]; const vf128 u = c->vf[ft]; (void)u; \
    VU_DEST_LOOP(dest, t.f[k] = vu_clamp(expr)); \
    if (fd) c->vf[fd] = t; }
#define VU_OP3BC(name, expr) \
EE_INLINE void vu0_##name(EEContext* c, int dest, int fd, int fs, int ft, int bc) { \
    vf128 t = c->vf[fd]; const vf128 s = c->vf[fs]; const float b = vu_in(c->vf[ft].f[bc]); \
    VU_DEST_LOOP(dest, t.f[k] = vu_clamp(expr)); \
    if (fd) c->vf[fd] = t; }
#define VU_OPACC(name, expr) \
EE_INLINE void vu0_##name(EEContext* c, int dest, int fd, int fs, int ft) { \
    (void)fd; vf128 t = c->vacc; const vf128 s = c->vf[fs]; const vf128 u = c->vf[ft]; (void)u; \
    VU_DEST_LOOP(dest, t.f[k] = vu_clamp(expr)); \
    c->vacc = t; }
#define VU_OPACCBC(name, expr) \
EE_INLINE void vu0_##name(EEContext* c, int dest, int fd, int fs, int ft, int bc) { \
    (void)fd; vf128 t = c->vacc; const vf128 s = c->vf[fs]; const float b = vu_in(c->vf[ft].f[bc]); \
    VU_DEST_LOOP(dest, t.f[k] = vu_clamp(expr)); \
    c->vacc = t; }

#define S_ vu_in(s.f[k])
#define U_ vu_in(u.f[k])
#define A_ vu_in(c->vacc.f[k])
#define Q_ vu_in(c->vq)
#define I_ vu_in(c->vI)

VU_OP3(vadd, S_ + U_)
VU_OP3(vsub, S_ - U_)
VU_OP3(vmul, S_ * U_)
VU_OP3(vmadd, A_ + S_ * U_)
VU_OP3(vmsub, A_ - S_ * U_)
VU_OP3(vmax, S_ > U_ ? S_ : U_)
VU_OP3(vmini, S_ < U_ ? S_ : U_)
VU_OP3(vaddq, S_ + Q_)
VU_OP3(vsubq, S_ - Q_)
VU_OP3(vmulq, S_ * Q_)
VU_OP3(vmaddq, A_ + S_ * Q_)
VU_OP3(vmsubq, A_ - S_ * Q_)
VU_OP3(vaddi, S_ + I_)
VU_OP3(vsubi, S_ - I_)
VU_OP3(vmuli, S_ * I_)
VU_OP3(vmaddi, A_ + S_ * I_)
VU_OP3(vmsubi, A_ - S_ * I_)
VU_OP3(vmaxi, S_ > I_ ? S_ : I_)
VU_OP3(vminii, S_ < I_ ? S_ : I_)
VU_OP3BC(vaddbc, S_ + b)
VU_OP3BC(vsubbc, S_ - b)
VU_OP3BC(vmulbc, S_ * b)
VU_OP3BC(vmaddbc, A_ + S_ * b)
VU_OP3BC(vmsubbc, A_ - S_ * b)
VU_OP3BC(vmaxbc, S_ > b ? S_ : b)
VU_OP3BC(vminibc, S_ < b ? S_ : b)
VU_OPACC(vadda, S_ + U_)
VU_OPACC(vsuba, S_ - U_)
VU_OPACC(vmula, S_ * U_)
VU_OPACC(vmadda, A_ + S_ * U_)
VU_OPACC(vmsuba, A_ - S_ * U_)
VU_OPACC(vaddaq, S_ + Q_)
VU_OPACC(vsubaq, S_ - Q_)
VU_OPACC(vmulaq, S_ * Q_)
VU_OPACC(vmaddaq, A_ + S_ * Q_)
VU_OPACC(vmsubaq, A_ - S_ * Q_)
VU_OPACC(vaddai, S_ + I_)
VU_OPACC(vsubai, S_ - I_)
VU_OPACC(vmulai, S_ * I_)
VU_OPACC(vmaddai, A_ + S_ * I_)
VU_OPACC(vmsubai, A_ - S_ * I_)
VU_OPACCBC(vaddabc, S_ + b)
VU_OPACCBC(vsubabc, S_ - b)
VU_OPACCBC(vmulabc, S_ * b)
VU_OPACCBC(vmaddabc, A_ + S_ * b)
VU_OPACCBC(vmsubabc, A_ - S_ * b)

EE_INLINE void vu0_vopmula(EEContext* c, int dest, int fd, int fs, int ft) {
    (void)dest; (void)fd;
    const vf128 s = c->vf[fs], u = c->vf[ft];
    c->vacc.f[0] = vu_clamp(vu_in(s.f[1]) * vu_in(u.f[2]));
    c->vacc.f[1] = vu_clamp(vu_in(s.f[2]) * vu_in(u.f[0]));
    c->vacc.f[2] = vu_clamp(vu_in(s.f[0]) * vu_in(u.f[1]));
}
EE_INLINE void vu0_vopmsub(EEContext* c, int dest, int fd, int fs, int ft) {
    (void)dest;
    const vf128 s = c->vf[fs], u = c->vf[ft];
    vf128 t = c->vf[fd];
    t.f[0] = vu_clamp(vu_in(c->vacc.f[0]) - vu_in(s.f[1]) * vu_in(u.f[2]));
    t.f[1] = vu_clamp(vu_in(c->vacc.f[1]) - vu_in(s.f[2]) * vu_in(u.f[0]));
    t.f[2] = vu_clamp(vu_in(c->vacc.f[2]) - vu_in(s.f[0]) * vu_in(u.f[1]));
    if (fd) c->vf[fd] = t;
}
/* ft-destination ops: encoded as (dest, fd, fs, ft) by generator; destination is ft */
EE_INLINE void vu0_vmove(EEContext* c, int dest, int fd, int fs, int ft) {
    (void)fd; vf128 t = c->vf[ft]; const vf128 s = c->vf[fs];
    VU_DEST_LOOP(dest, t.u[k] = s.u[k]);
    if (ft) c->vf[ft] = t;
}
EE_INLINE void vu0_vmr32(EEContext* c, int dest, int fd, int fs, int ft) {
    (void)fd; vf128 t = c->vf[ft]; const vf128 s = c->vf[fs];
    vf128 r; r.u[0] = s.u[1]; r.u[1] = s.u[2]; r.u[2] = s.u[3]; r.u[3] = s.u[0];
    VU_DEST_LOOP(dest, t.u[k] = r.u[k]);
    if (ft) c->vf[ft] = t;
}
EE_INLINE void vu0_vabs(EEContext* c, int dest, int fd, int fs, int ft) {
    (void)fd; vf128 t = c->vf[ft]; const vf128 s = c->vf[fs];
    VU_DEST_LOOP(dest, t.f[k] = fabsf(vu_in(s.f[k])));
    if (ft) c->vf[ft] = t;
}
#define VU_ITOF(name, scale) \
EE_INLINE void vu0_##name(EEContext* c, int dest, int fd, int fs, int ft) { \
    (void)fd; vf128 t = c->vf[ft]; const vf128 s = c->vf[fs]; \
    VU_DEST_LOOP(dest, t.f[k] = (float)s.s[k] * (1.0f / (scale))); \
    if (ft) c->vf[ft] = t; }
VU_ITOF(vitof0, 1.0f)
VU_ITOF(vitof4, 16.0f)
VU_ITOF(vitof12, 4096.0f)
VU_ITOF(vitof15, 32768.0f)
EE_INLINE int32_t vu_ftoi(float f) { return fpu_cvtw(f); }
#define VU_FTOI(name, scale) \
EE_INLINE void vu0_##name(EEContext* c, int dest, int fd, int fs, int ft) { \
    (void)fd; vf128 t = c->vf[ft]; const vf128 s = c->vf[fs]; \
    VU_DEST_LOOP(dest, t.s[k] = vu_ftoi(vu_in(s.f[k]) * (scale))); \
    if (ft) c->vf[ft] = t; }
VU_FTOI(vftoi0, 1.0f)
VU_FTOI(vftoi4, 16.0f)
VU_FTOI(vftoi12, 4096.0f)
VU_FTOI(vftoi15, 32768.0f)

EE_INLINE void vu0_vclipw(EEContext* c, int dest, int fd, int fs, int ft) {
    (void)dest; (void)fd;
    float w = fabsf(vu_in(c->vf[ft].f[3]));
    const vf128 s = c->vf[fs];
    uint32_t f = 0;
    if (vu_in(s.f[0]) > +w) f |= 0x01;
    if (vu_in(s.f[0]) < -w) f |= 0x02;
    if (vu_in(s.f[1]) > +w) f |= 0x04;
    if (vu_in(s.f[1]) < -w) f |= 0x08;
    if (vu_in(s.f[2]) > +w) f |= 0x10;
    if (vu_in(s.f[2]) < -w) f |= 0x20;
    c->vclip = ((c->vclip << 6) | f) & 0xFFFFFF;
}
EE_INLINE void vu0_vdiv(EEContext* c, int fs, int fsf, int ft, int ftf) {
    float a = vu_in(c->vf[fs].f[fsf]), b = vu_in(c->vf[ft].f[ftf]);
    c->vq = fpu_div(c, a, b);
}
EE_INLINE void vu0_vsqrt(EEContext* c, int fs, int fsf, int ft, int ftf) {
    (void)fs; (void)fsf;
    c->vq = sqrtf(fabsf(vu_in(c->vf[ft].f[ftf])));
}
EE_INLINE void vu0_vrsqrt(EEContext* c, int fs, int fsf, int ft, int ftf) {
    float a = vu_in(c->vf[fs].f[fsf]);
    float b = sqrtf(fabsf(vu_in(c->vf[ft].f[ftf])));
    c->vq = fpu_div(c, a, b);
}
EE_INLINE void vu0_viadd(EEContext* c, int id, int is, int it) { if (id) c->vi[id] = (c->vi[is] + c->vi[it]) & 0xFFFF; }
EE_INLINE void vu0_visub(EEContext* c, int id, int is, int it) { if (id) c->vi[id] = (c->vi[is] - c->vi[it]) & 0xFFFF; }
EE_INLINE void vu0_viand(EEContext* c, int id, int is, int it) { if (id) c->vi[id] = (c->vi[is] & c->vi[it]) & 0xFFFF; }
EE_INLINE void vu0_vior(EEContext* c, int id, int is, int it) { if (id) c->vi[id] = (c->vi[is] | c->vi[it]) & 0xFFFF; }
EE_INLINE void vu0_viaddi(EEContext* c, int it, int is, int imm) { if (it) c->vi[it] = (c->vi[is] + imm) & 0xFFFF; }
EE_INLINE void vu0_vmtir(EEContext* c, int it, int fs, int fsf) { if (it) c->vi[it] = c->vf[fs].u[fsf] & 0xFFFF; }
EE_INLINE void vu0_vmfir(EEContext* c, int dest, int ft, int is) {
    vf128 t = c->vf[ft];
    int32_t v = (int16_t)c->vi[is];
    VU_DEST_LOOP(dest, t.s[k] = v);
    if (ft) c->vf[ft] = t;
}
extern uint8_t g_vu0_mem[4096];
EE_INLINE vf128* vu0_memq(uint32_t a) { return (vf128*)(g_vu0_mem + ((a * 16) & 0xFF0)); }
EE_INLINE void vu0_vlqi(EEContext* c, int dest, int ft, int is) {
    vf128* m = vu0_memq(c->vi[is]); vf128 t = c->vf[ft];
    VU_DEST_LOOP(dest, t.u[k] = m->u[k]);
    if (ft) c->vf[ft] = t;
    if (is) c->vi[is] = (c->vi[is] + 1) & 0xFFFF;
}
EE_INLINE void vu0_vlqd(EEContext* c, int dest, int ft, int is) {
    if (is) c->vi[is] = (c->vi[is] - 1) & 0xFFFF;
    vf128* m = vu0_memq(c->vi[is]); vf128 t = c->vf[ft];
    VU_DEST_LOOP(dest, t.u[k] = m->u[k]);
    if (ft) c->vf[ft] = t;
}
EE_INLINE void vu0_vsqi(EEContext* c, int dest, int fs, int it) {
    vf128* m = vu0_memq(c->vi[it]);
    VU_DEST_LOOP(dest, m->u[k] = c->vf[fs].u[k]);
    if (it) c->vi[it] = (c->vi[it] + 1) & 0xFFFF;
}
EE_INLINE void vu0_vsqd(EEContext* c, int dest, int fs, int it) {
    if (it) c->vi[it] = (c->vi[it] - 1) & 0xFFFF;
    vf128* m = vu0_memq(c->vi[it]);
    VU_DEST_LOOP(dest, m->u[k] = c->vf[fs].u[k]);
}
EE_INLINE void vu0_vilwr(EEContext* c, int dest, int it, int is) {
    vf128* m = vu0_memq(c->vi[is]);
    int k = (dest & 8) ? 0 : (dest & 4) ? 1 : (dest & 2) ? 2 : 3;
    if (it) c->vi[it] = m->u[k] & 0xFFFF;
}
EE_INLINE void vu0_viswr(EEContext* c, int dest, int it, int is) {
    vf128* m = vu0_memq(c->vi[is]);
    VU_DEST_LOOP(dest, m->u[k] = c->vi[it] & 0xFFFF);
}
EE_INLINE void vu0_vrnext(EEContext* c, int dest, int ft) {
    uint32_t x = c->vr;
    int b = ((x >> 4) ^ (x >> 22)) & 1;
    x = ((x << 1) | b) & 0x7FFFFF;
    c->vr = x;
    vf128 t = c->vf[ft];
    uint32_t f = 0x3F800000u | x;
    VU_DEST_LOOP(dest, t.u[k] = f);
    if (ft) c->vf[ft] = t;
}
EE_INLINE void vu0_vrget(EEContext* c, int dest, int ft) {
    vf128 t = c->vf[ft];
    uint32_t f = 0x3F800000u | (c->vr & 0x7FFFFF);
    VU_DEST_LOOP(dest, t.u[k] = f);
    if (ft) c->vf[ft] = t;
}
EE_INLINE void vu0_vrinit(EEContext* c, int fs, int fsf) { c->vr = c->vf[fs].u[fsf] & 0x7FFFFF; }
EE_INLINE void vu0_vrxor(EEContext* c, int fs, int fsf) { c->vr = (c->vr ^ c->vf[fs].u[fsf]) & 0x7FFFFF; }

/* ------------------------------------------------------------------ */
/* MMI (128-bit multimedia)                                            */
/* ------------------------------------------------------------------ */
#define MMI_FN(name) EE_INLINE void mmi_##name(EEContext* c, int rd, int rs, int rt, int sa)
#define MMI_D (rd ? &c->r[rd] : &c->scratch)
#define MMI_S c->r[rs]
#define MMI_T c->r[rt]

EE_INLINE int32_t sat32(int64_t v) { return v > 0x7FFFFFFF ? 0x7FFFFFFF : v < -0x7FFFFFFFll - 1 ? (int32_t)0x80000000 : (int32_t)v; }
EE_INLINE int16_t sat16(int32_t v) { return v > 0x7FFF ? 0x7FFF : v < -0x8000 ? -0x8000 : (int16_t)v; }
EE_INLINE int8_t sat8(int32_t v) { return v > 0x7F ? 0x7F : v < -0x80 ? -0x80 : (int8_t)v; }

MMI_FN(plzcw) {
    (void)rt; (void)sa; r128 s = MMI_S; r128* d = MMI_D;
    for (int i = 0; i < 2; i++) {
        uint32_t v = s.uw[i];
        if (v & 0x80000000u) v = ~v;
        d->uw[i] = v ? (uint32_t)(__builtin_clz(v) - 1) : 31;
    }
}
#define MMI_LANES(name, N, field, expr) MMI_FN(name) { (void)sa; r128 s = MMI_S, t = MMI_T, r; \
    for (int i = 0; i < N; i++) r.field[i] = (expr); *MMI_D = r; }
MMI_LANES(paddw, 4, uw, s.uw[i] + t.uw[i])
MMI_LANES(psubw, 4, uw, s.uw[i] - t.uw[i])
MMI_LANES(pcgtw, 4, uw, s.sw[i] > t.sw[i] ? 0xFFFFFFFFu : 0)
MMI_LANES(pmaxw, 4, sw, s.sw[i] > t.sw[i] ? s.sw[i] : t.sw[i])
MMI_LANES(paddh, 8, uh, (uint16_t)(s.uh[i] + t.uh[i]))
MMI_LANES(psubh, 8, uh, (uint16_t)(s.uh[i] - t.uh[i]))
MMI_LANES(pcgth, 8, uh, s.sh[i] > t.sh[i] ? 0xFFFF : 0)
MMI_LANES(pmaxh, 8, sh, s.sh[i] > t.sh[i] ? s.sh[i] : t.sh[i])
MMI_LANES(paddb, 16, ub, (uint8_t)(s.ub[i] + t.ub[i]))
MMI_LANES(psubb, 16, ub, (uint8_t)(s.ub[i] - t.ub[i]))
MMI_LANES(pcgtb, 16, ub, s.sb[i] > t.sb[i] ? 0xFF : 0)
MMI_LANES(paddsw, 4, sw, sat32((int64_t)s.sw[i] + t.sw[i]))
MMI_LANES(psubsw, 4, sw, sat32((int64_t)s.sw[i] - t.sw[i]))
MMI_LANES(paddsh, 8, sh, sat16((int32_t)s.sh[i] + t.sh[i]))
MMI_LANES(psubsh, 8, sh, sat16((int32_t)s.sh[i] - t.sh[i]))
MMI_LANES(paddsb, 16, sb, sat8((int32_t)s.sb[i] + t.sb[i]))
MMI_LANES(psubsb, 16, sb, sat8((int32_t)s.sb[i] - t.sb[i]))
MMI_LANES(pabsw, 4, sw, t.sw[i] == (int32_t)0x80000000 ? 0x7FFFFFFF : (t.sw[i] < 0 ? -t.sw[i] : t.sw[i]))
MMI_LANES(pabsh, 8, sh, t.sh[i] == -0x8000 ? 0x7FFF : (int16_t)(t.sh[i] < 0 ? -t.sh[i] : t.sh[i]))
MMI_LANES(pceqw, 4, uw, s.uw[i] == t.uw[i] ? 0xFFFFFFFFu : 0)
MMI_LANES(pminw, 4, sw, s.sw[i] < t.sw[i] ? s.sw[i] : t.sw[i])
MMI_LANES(pceqh, 8, uh, s.uh[i] == t.uh[i] ? 0xFFFF : 0)
MMI_LANES(pminh, 8, sh, s.sh[i] < t.sh[i] ? s.sh[i] : t.sh[i])
MMI_LANES(pceqb, 16, ub, s.ub[i] == t.ub[i] ? 0xFF : 0)
MMI_LANES(padduw, 4, uw, ((uint64_t)s.uw[i] + t.uw[i]) > 0xFFFFFFFFull ? 0xFFFFFFFFu : s.uw[i] + t.uw[i])
MMI_LANES(psubuw, 4, uw, s.uw[i] > t.uw[i] ? s.uw[i] - t.uw[i] : 0)
MMI_LANES(padduh, 8, uh, ((uint32_t)s.uh[i] + t.uh[i]) > 0xFFFF ? 0xFFFF : s.uh[i] + t.uh[i])
MMI_LANES(psubuh, 8, uh, s.uh[i] > t.uh[i] ? s.uh[i] - t.uh[i] : 0)
MMI_LANES(paddub, 16, ub, ((uint32_t)s.ub[i] + t.ub[i]) > 0xFF ? 0xFF : s.ub[i] + t.ub[i])
MMI_LANES(psubub, 16, ub, s.ub[i] > t.ub[i] ? s.ub[i] - t.ub[i] : 0)
MMI_LANES(pand, 4, uw, s.uw[i] & t.uw[i])
MMI_LANES(por, 4, uw, s.uw[i] | t.uw[i])
MMI_LANES(pxor, 4, uw, s.uw[i] ^ t.uw[i])
MMI_LANES(pnor, 4, uw, ~(s.uw[i] | t.uw[i]))
MMI_LANES(psllvw_dummy, 1, uw, 0)

MMI_FN(padsbh) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    for (int i = 0; i < 4; i++) r.uh[i] = (uint16_t)(s.uh[i] - t.uh[i]);
    for (int i = 4; i < 8; i++) r.uh[i] = (uint16_t)(s.uh[i] + t.uh[i]);
    *MMI_D = r; }
MMI_FN(pextlw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    r.uw[0] = t.uw[0]; r.uw[1] = s.uw[0]; r.uw[2] = t.uw[1]; r.uw[3] = s.uw[1]; *MMI_D = r; }
MMI_FN(pextuw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    r.uw[0] = t.uw[2]; r.uw[1] = s.uw[2]; r.uw[2] = t.uw[3]; r.uw[3] = s.uw[3]; *MMI_D = r; }
MMI_FN(pextlh) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    for (int i = 0; i < 4; i++) { r.uh[2 * i] = t.uh[i]; r.uh[2 * i + 1] = s.uh[i]; } *MMI_D = r; }
MMI_FN(pextuh) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    for (int i = 0; i < 4; i++) { r.uh[2 * i] = t.uh[4 + i]; r.uh[2 * i + 1] = s.uh[4 + i]; } *MMI_D = r; }
MMI_FN(pextlb) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    for (int i = 0; i < 8; i++) { r.ub[2 * i] = t.ub[i]; r.ub[2 * i + 1] = s.ub[i]; } *MMI_D = r; }
MMI_FN(pextub) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    for (int i = 0; i < 8; i++) { r.ub[2 * i] = t.ub[8 + i]; r.ub[2 * i + 1] = s.ub[8 + i]; } *MMI_D = r; }
MMI_FN(ppacw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    r.uw[0] = t.uw[0]; r.uw[1] = t.uw[2]; r.uw[2] = s.uw[0]; r.uw[3] = s.uw[2]; *MMI_D = r; }
MMI_FN(ppach) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    for (int i = 0; i < 4; i++) { r.uh[i] = t.uh[2 * i]; r.uh[4 + i] = s.uh[2 * i]; } *MMI_D = r; }
MMI_FN(ppacb) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    for (int i = 0; i < 8; i++) { r.ub[i] = t.ub[2 * i]; r.ub[8 + i] = s.ub[2 * i]; } *MMI_D = r; }
MMI_FN(pext5) { (void)sa; (void)rs; r128 t = MMI_T, r;
    for (int i = 0; i < 4; i++) {
        uint32_t v = t.uw[i];
        r.uw[i] = ((v & 0x1F) << 3) | ((v & 0x3E0) << 6) | ((v & 0x7C00) << 9) | ((v & 0x8000) << 16);
    } *MMI_D = r; }
MMI_FN(ppac5) { (void)sa; (void)rs; r128 t = MMI_T, r;
    for (int i = 0; i < 4; i++) {
        uint32_t v = t.uw[i];
        r.uw[i] = ((v >> 3) & 0x1F) | ((v >> 6) & 0x3E0) | ((v >> 9) & 0x7C00) | ((v >> 16) & 0x8000);
    } *MMI_D = r; }
MMI_FN(qfsrv) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    uint32_t sh = (uint32_t)(c->sa & 0xF) * 8;
    /* 256-bit {rs, rt} >> sh, take low 128 */
    uint8_t buf[32];
    memcpy(buf, t.ub, 16); memcpy(buf + 16, s.ub, 16);
    memcpy(r.ub, buf + sh / 8, 16);
    *MMI_D = r; }
MMI_FN(psllh) { (void)rs; r128 t = MMI_T, r; for (int i = 0; i < 8; i++) r.uh[i] = (uint16_t)(t.uh[i] << (sa & 15)); *MMI_D = r; }
MMI_FN(psrlh) { (void)rs; r128 t = MMI_T, r; for (int i = 0; i < 8; i++) r.uh[i] = (uint16_t)(t.uh[i] >> (sa & 15)); *MMI_D = r; }
MMI_FN(psrah) { (void)rs; r128 t = MMI_T, r; for (int i = 0; i < 8; i++) r.sh[i] = (int16_t)(t.sh[i] >> (sa & 15)); *MMI_D = r; }
MMI_FN(psllw) { (void)rs; r128 t = MMI_T, r; for (int i = 0; i < 4; i++) r.uw[i] = t.uw[i] << sa; *MMI_D = r; }
MMI_FN(psrlw) { (void)rs; r128 t = MMI_T, r; for (int i = 0; i < 4; i++) r.uw[i] = t.uw[i] >> sa; *MMI_D = r; }
MMI_FN(psraw) { (void)rs; r128 t = MMI_T, r; for (int i = 0; i < 4; i++) r.sw[i] = t.sw[i] >> sa; *MMI_D = r; }
MMI_FN(psllvw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    r.sd[0] = (int32_t)(t.uw[0] << (s.uw[0] & 31)); r.sd[1] = (int32_t)(t.uw[2] << (s.uw[2] & 31)); *MMI_D = r; }
MMI_FN(psrlvw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    r.sd[0] = (int32_t)(t.uw[0] >> (s.uw[0] & 31)); r.sd[1] = (int32_t)(t.uw[2] >> (s.uw[2] & 31)); *MMI_D = r; }
MMI_FN(psravw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    r.sd[0] = (int32_t)(t.sw[0] >> (s.uw[0] & 31)); r.sd[1] = (int32_t)(t.sw[2] >> (s.uw[2] & 31)); *MMI_D = r; }
MMI_FN(pmfhi) { (void)sa; (void)rs; (void)rt; *MMI_D = c->hi; }
MMI_FN(pmflo) { (void)sa; (void)rs; (void)rt; *MMI_D = c->lo; }
MMI_FN(pmthi) { (void)sa; (void)rd; (void)rt; c->hi = MMI_S; }
MMI_FN(pmtlo) { (void)sa; (void)rd; (void)rt; c->lo = MMI_S; }
MMI_FN(pinth) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    for (int i = 0; i < 4; i++) { r.uh[2 * i] = t.uh[i]; r.uh[2 * i + 1] = s.uh[4 + i]; } *MMI_D = r; }
MMI_FN(pinteh) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    for (int i = 0; i < 4; i++) { r.uh[2 * i] = t.uh[2 * i]; r.uh[2 * i + 1] = s.uh[2 * i]; } *MMI_D = r; }
MMI_FN(pcpyld) { (void)sa; r128 s = MMI_S, t = MMI_T, r; r.ud[0] = t.ud[0]; r.ud[1] = s.ud[0]; *MMI_D = r; }
MMI_FN(pcpyud) { (void)sa; r128 s = MMI_S, t = MMI_T, r; r.ud[0] = s.ud[1]; r.ud[1] = t.ud[1]; *MMI_D = r; }
MMI_FN(pcpyh) { (void)sa; (void)rs; r128 t = MMI_T, r;
    for (int i = 0; i < 4; i++) { r.uh[i] = t.uh[0]; r.uh[4 + i] = t.uh[4]; } *MMI_D = r; }
MMI_FN(pexeh) { (void)sa; (void)rs; r128 t = MMI_T, r = t;
    r.uh[0] = t.uh[2]; r.uh[2] = t.uh[0]; r.uh[4] = t.uh[6]; r.uh[6] = t.uh[4]; *MMI_D = r; }
MMI_FN(prevh) { (void)sa; (void)rs; r128 t = MMI_T, r;
    r.uh[0] = t.uh[3]; r.uh[1] = t.uh[2]; r.uh[2] = t.uh[1]; r.uh[3] = t.uh[0];
    r.uh[4] = t.uh[7]; r.uh[5] = t.uh[6]; r.uh[6] = t.uh[5]; r.uh[7] = t.uh[4]; *MMI_D = r; }
MMI_FN(pexew) { (void)sa; (void)rs; r128 t = MMI_T, r = t;
    r.uw[0] = t.uw[2]; r.uw[2] = t.uw[0]; *MMI_D = r; }
MMI_FN(prot3w) { (void)sa; (void)rs; r128 t = MMI_T, r = t;
    r.uw[0] = t.uw[1]; r.uw[1] = t.uw[2]; r.uw[2] = t.uw[0]; *MMI_D = r; }
MMI_FN(pexch) { (void)sa; (void)rs; r128 t = MMI_T, r = t;
    r.uh[1] = t.uh[2]; r.uh[2] = t.uh[1]; r.uh[5] = t.uh[6]; r.uh[6] = t.uh[5]; *MMI_D = r; }
MMI_FN(pexcw) { (void)sa; (void)rs; r128 t = MMI_T, r = t;
    r.uw[1] = t.uw[2]; r.uw[2] = t.uw[1]; *MMI_D = r; }
MMI_FN(pmultw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    int64_t a = (int64_t)s.sw[0] * t.sw[0], b = (int64_t)s.sw[2] * t.sw[2];
    c->lo.sd[0] = (int32_t)a; c->hi.sd[0] = (int32_t)(a >> 32);
    c->lo.sd[1] = (int32_t)b; c->hi.sd[1] = (int32_t)(b >> 32);
    r.sd[0] = a; r.sd[1] = b; *MMI_D = r; }
MMI_FN(pmultuw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    uint64_t a = (uint64_t)s.uw[0] * t.uw[0], b = (uint64_t)s.uw[2] * t.uw[2];
    c->lo.sd[0] = (int32_t)a; c->hi.sd[0] = (int32_t)(a >> 32);
    c->lo.sd[1] = (int32_t)b; c->hi.sd[1] = (int32_t)(b >> 32);
    r.ud[0] = a; r.ud[1] = b; *MMI_D = r; }
MMI_FN(pmaddw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    int64_t a = (int64_t)(((uint64_t)c->hi.uw[0] << 32) | c->lo.uw[0]) + (int64_t)s.sw[0] * t.sw[0];
    int64_t b = (int64_t)(((uint64_t)c->hi.uw[2] << 32) | c->lo.uw[2]) + (int64_t)s.sw[2] * t.sw[2];
    c->lo.sd[0] = (int32_t)a; c->hi.sd[0] = (int32_t)(a >> 32);
    c->lo.sd[1] = (int32_t)b; c->hi.sd[1] = (int32_t)(b >> 32);
    r.sd[0] = a; r.sd[1] = b; *MMI_D = r; }
MMI_FN(pmadduw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    uint64_t a = (((uint64_t)c->hi.uw[0] << 32) | c->lo.uw[0]) + (uint64_t)s.uw[0] * t.uw[0];
    uint64_t b = (((uint64_t)c->hi.uw[2] << 32) | c->lo.uw[2]) + (uint64_t)s.uw[2] * t.uw[2];
    c->lo.sd[0] = (int32_t)a; c->hi.sd[0] = (int32_t)(a >> 32);
    c->lo.sd[1] = (int32_t)b; c->hi.sd[1] = (int32_t)(b >> 32);
    r.ud[0] = a; r.ud[1] = b; *MMI_D = r; }
MMI_FN(pmsubw) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    int64_t a = (int64_t)(((uint64_t)c->hi.uw[0] << 32) | c->lo.uw[0]) - (int64_t)s.sw[0] * t.sw[0];
    int64_t b = (int64_t)(((uint64_t)c->hi.uw[2] << 32) | c->lo.uw[2]) - (int64_t)s.sw[2] * t.sw[2];
    c->lo.sd[0] = (int32_t)a; c->hi.sd[0] = (int32_t)(a >> 32);
    c->lo.sd[1] = (int32_t)b; c->hi.sd[1] = (int32_t)(b >> 32);
    r.sd[0] = a; r.sd[1] = b; *MMI_D = r; }
MMI_FN(pdivw) { (void)sa; (void)rd; r128 s = MMI_S, t = MMI_T;
    for (int i = 0; i < 2; i++) {
        int32_t a = s.sw[2 * i], b = t.sw[2 * i];
        if (b == 0) { c->lo.sd[i] = a < 0 ? 1 : -1; c->hi.sd[i] = a; }
        else if (a == (int32_t)0x80000000 && b == -1) { c->lo.sd[i] = (int32_t)0x80000000; c->hi.sd[i] = 0; }
        else { c->lo.sd[i] = a / b; c->hi.sd[i] = a % b; }
    } }
MMI_FN(pdivuw) { (void)sa; (void)rd; r128 s = MMI_S, t = MMI_T;
    for (int i = 0; i < 2; i++) {
        uint32_t a = s.uw[2 * i], b = t.uw[2 * i];
        if (b == 0) { c->lo.sd[i] = -1; c->hi.sd[i] = (int32_t)a; }
        else { c->lo.sd[i] = (int32_t)(a / b); c->hi.sd[i] = (int32_t)(a % b); }
    } }
MMI_FN(pdivbw) { (void)sa; (void)rd; r128 s = MMI_S, t = MMI_T;
    int16_t b = t.sh[0];
    for (int i = 0; i < 4; i++) {
        int32_t a = s.sw[i];
        if (b == 0) { c->lo.sw[i] = a < 0 ? 1 : -1; c->hi.sw[i] = a; }
        else if (a == (int32_t)0x80000000 && b == -1) { c->lo.sw[i] = (int32_t)0x80000000; c->hi.sw[i] = 0; }
        else { c->lo.sw[i] = a / b; c->hi.sw[i] = (int16_t)(a % b); }
    } }
MMI_FN(pmulth) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    int32_t p[8]; for (int i = 0; i < 8; i++) p[i] = (int32_t)s.sh[i] * t.sh[i];
    c->lo.sw[0] = p[0]; c->lo.sw[1] = p[1]; c->hi.sw[0] = p[2]; c->hi.sw[1] = p[3];
    c->lo.sw[2] = p[4]; c->lo.sw[3] = p[5]; c->hi.sw[2] = p[6]; c->hi.sw[3] = p[7];
    r.sw[0] = p[0]; r.sw[1] = p[2]; r.sw[2] = p[4]; r.sw[3] = p[6]; *MMI_D = r; }
MMI_FN(pmaddh) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    int32_t p[8]; for (int i = 0; i < 8; i++) p[i] = (int32_t)s.sh[i] * t.sh[i];
    c->lo.sw[0] += p[0]; c->lo.sw[1] += p[1]; c->hi.sw[0] += p[2]; c->hi.sw[1] += p[3];
    c->lo.sw[2] += p[4]; c->lo.sw[3] += p[5]; c->hi.sw[2] += p[6]; c->hi.sw[3] += p[7];
    r.sw[0] = c->lo.sw[0]; r.sw[1] = c->hi.sw[0]; r.sw[2] = c->lo.sw[2]; r.sw[3] = c->hi.sw[2]; *MMI_D = r; }
MMI_FN(pmsubh) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    int32_t p[8]; for (int i = 0; i < 8; i++) p[i] = (int32_t)s.sh[i] * t.sh[i];
    c->lo.sw[0] -= p[0]; c->lo.sw[1] -= p[1]; c->hi.sw[0] -= p[2]; c->hi.sw[1] -= p[3];
    c->lo.sw[2] -= p[4]; c->lo.sw[3] -= p[5]; c->hi.sw[2] -= p[6]; c->hi.sw[3] -= p[7];
    r.sw[0] = c->lo.sw[0]; r.sw[1] = c->hi.sw[0]; r.sw[2] = c->lo.sw[2]; r.sw[3] = c->hi.sw[2]; *MMI_D = r; }
MMI_FN(phmadh) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    int32_t p[4]; for (int i = 0; i < 4; i++) p[i] = (int32_t)s.sh[2 * i + 1] * t.sh[2 * i + 1] + (int32_t)s.sh[2 * i] * t.sh[2 * i];
    c->lo.sw[0] = p[0]; c->hi.sw[0] = p[1]; c->lo.sw[2] = p[2]; c->hi.sw[2] = p[3];
    r.sw[0] = p[0]; r.sw[1] = p[1]; r.sw[2] = p[2]; r.sw[3] = p[3]; *MMI_D = r; }
MMI_FN(phmsbh) { (void)sa; r128 s = MMI_S, t = MMI_T, r;
    int32_t p[4]; for (int i = 0; i < 4; i++) p[i] = (int32_t)s.sh[2 * i + 1] * t.sh[2 * i + 1] - (int32_t)s.sh[2 * i] * t.sh[2 * i];
    c->lo.sw[0] = p[0]; c->hi.sw[0] = p[1]; c->lo.sw[2] = p[2]; c->hi.sw[2] = p[3];
    r.sw[0] = p[0]; r.sw[1] = p[1]; r.sw[2] = p[2]; r.sw[3] = p[3]; *MMI_D = r; }
MMI_FN(pmfhl) { (void)rs; (void)rt; r128 r; r128 lo = c->lo, hi = c->hi;
    switch (sa) {
    case 0: r.uw[0] = lo.uw[0]; r.uw[1] = hi.uw[0]; r.uw[2] = lo.uw[2]; r.uw[3] = hi.uw[2]; break; /* LW */
    case 1: r.uw[0] = lo.uw[1]; r.uw[1] = hi.uw[1]; r.uw[2] = lo.uw[3]; r.uw[3] = hi.uw[3]; break; /* UW */
    case 2: r.sd[0] = sat32((int64_t)(((uint64_t)hi.uw[0] << 32) | lo.uw[0]));
            r.sd[1] = sat32((int64_t)(((uint64_t)hi.uw[2] << 32) | lo.uw[2])); break; /* SLW */
    case 3: r.sh[0] = sat16(lo.sw[0]); r.sh[1] = sat16(lo.sw[1]); r.sh[2] = sat16(hi.sw[0]); r.sh[3] = sat16(hi.sw[1]);
            r.sh[4] = sat16(lo.sw[2]); r.sh[5] = sat16(lo.sw[3]); r.sh[6] = sat16(hi.sw[2]); r.sh[7] = sat16(hi.sw[3]); break; /* LH */
    default: r.uh[0] = lo.uh[0]; r.uh[1] = lo.uh[2]; r.uh[2] = hi.uh[0]; r.uh[3] = hi.uh[2];
             r.uh[4] = lo.uh[4]; r.uh[5] = lo.uh[6]; r.uh[6] = hi.uh[4]; r.uh[7] = hi.uh[6]; break; /* SH */
    }
    *MMI_D = r; }
MMI_FN(pmthl) { (void)rd; (void)rt; r128 s = MMI_S;
    if (sa == 0) { c->lo.uw[0] = s.uw[0]; c->hi.uw[0] = s.uw[1]; c->lo.uw[2] = s.uw[2]; c->hi.uw[2] = s.uw[3]; } }
MMI_FN(pmaddw_dummy) { (void)c; (void)rd; (void)rs; (void)rt; (void)sa; }

#ifdef __cplusplus
}
#endif
