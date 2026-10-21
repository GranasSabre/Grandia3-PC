/* Runtime interface for recompiled IOP (R3000) code. */
#pragma once
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IOPContext {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t cop0[32];
    uint32_t cur_func;
    int thread_id;
    void* thread;
} IOPContext;

typedef void (*IOPFunc)(IOPContext*);
typedef struct IOPFuncEntry {
    uint32_t start;
    uint32_t end;
    IOPFunc fn;
} IOPFuncEntry;

/* Savestate resume (see ee_recomp.h): the function entry jumps to g_iop_resume */
extern uint32_t g_iop_resume;
static inline uint32_t iop_resume_take(void) { uint32_t r = g_iop_resume; g_iop_resume = 0; return r; }
void iop_resume_fail(IOPContext* c, uint32_t func);

extern uint8_t* g_iop_ram;   /* 2MB */
extern volatile int g_iop_irq_pending;

uint32_t iop_io_rd(uint32_t a, int size);
void iop_io_wr(uint32_t a, uint32_t v, int size);

#define IOP_INLINE static inline __attribute__((always_inline))
IOP_INLINE int iop_is_ram(uint32_t a) { return (a & 0x1FFFFFFFu) < 0x00800000u; }
IOP_INLINE uint8_t* iop_hptr(uint32_t a) { return g_iop_ram + (a & 0x1FFFFFu); }

IOP_INLINE uint32_t iop_rd8(uint32_t a) {
    if (__builtin_expect(iop_is_ram(a), 1)) return *iop_hptr(a);
    return iop_io_rd(a, 1) & 0xFF;
}
IOP_INLINE uint32_t iop_rd16(uint32_t a) {
    if (__builtin_expect(iop_is_ram(a), 1)) { uint16_t v; memcpy(&v, iop_hptr(a), 2); return v; }
    return iop_io_rd(a, 2) & 0xFFFF;
}
IOP_INLINE uint32_t iop_rd32(uint32_t a) {
    if (__builtin_expect(iop_is_ram(a), 1)) { uint32_t v; memcpy(&v, iop_hptr(a), 4); return v; }
    return iop_io_rd(a, 4);
}
IOP_INLINE void iop_wr8(uint32_t a, uint8_t v) {
    if (__builtin_expect(iop_is_ram(a), 1)) { *iop_hptr(a) = v; return; }
    iop_io_wr(a, v, 1);
}
IOP_INLINE void iop_wr16(uint32_t a, uint16_t v) {
    if (__builtin_expect(iop_is_ram(a), 1)) { memcpy(iop_hptr(a), &v, 2); return; }
    iop_io_wr(a, v, 2);
}
IOP_INLINE void iop_wr32(uint32_t a, uint32_t v) {
    if (__builtin_expect(iop_is_ram(a), 1)) { memcpy(iop_hptr(a), &v, 4); return; }
    iop_io_wr(a, v, 4);
}

IOP_INLINE void iop_lwl(IOPContext* c, int rt, uint32_t addr) {
    static const uint32_t MASK[4] = {0x00ffffff, 0x0000ffff, 0x000000ff, 0x00000000};
    static const uint8_t SH[4] = {24, 16, 8, 0};
    uint32_t s = addr & 3, mem = iop_rd32(addr & ~3u);
    if (rt) c->r[rt] = (c->r[rt] & MASK[s]) | (mem << SH[s]);
}
IOP_INLINE void iop_lwr(IOPContext* c, int rt, uint32_t addr) {
    static const uint32_t MASK[4] = {0x00000000, 0xff000000, 0xffff0000, 0xffffff00};
    static const uint8_t SH[4] = {0, 8, 16, 24};
    uint32_t s = addr & 3, mem = iop_rd32(addr & ~3u);
    if (rt) c->r[rt] = (c->r[rt] & MASK[s]) | (mem >> SH[s]);
}
IOP_INLINE void iop_swl(IOPContext* c, int rt, uint32_t addr) {
    static const uint32_t MASK[4] = {0xffffff00, 0xffff0000, 0xff000000, 0x00000000};
    static const uint8_t SH[4] = {24, 16, 8, 0};
    uint32_t s = addr & 3, mem = iop_rd32(addr & ~3u);
    iop_wr32(addr & ~3u, (c->r[rt] >> SH[s]) | (mem & MASK[s]));
}
IOP_INLINE void iop_swr(IOPContext* c, int rt, uint32_t addr) {
    static const uint32_t MASK[4] = {0x00000000, 0x000000ff, 0x0000ffff, 0x00ffffff};
    static const uint8_t SH[4] = {0, 8, 16, 24};
    uint32_t s = addr & 3, mem = iop_rd32(addr & ~3u);
    iop_wr32(addr & ~3u, (c->r[rt] << SH[s]) | (mem & MASK[s]));
}
IOP_INLINE void iop_div(IOPContext* c, int32_t a, int32_t b) {
    if (b == 0) { c->lo = a < 0 ? 1 : 0xFFFFFFFFu; c->hi = (uint32_t)a; }
    else if (a == (int32_t)0x80000000 && b == -1) { c->lo = 0x80000000u; c->hi = 0; }
    else { c->lo = (uint32_t)(a / b); c->hi = (uint32_t)(a % b); }
}
IOP_INLINE void iop_divu(IOPContext* c, uint32_t a, uint32_t b) {
    if (b == 0) { c->lo = 0xFFFFFFFFu; c->hi = a; }
    else { c->lo = a / b; c->hi = a % b; }
}

void iop_call(IOPContext* c, uint32_t addr);
void iop_syscall(IOPContext* c, uint32_t pc);
void iop_break(IOPContext* c, uint32_t pc);
void iop_invalid(IOPContext* c, uint32_t pc, uint32_t word);
uint32_t iop_mfc0(IOPContext* c, int reg);
void iop_mtc0(IOPContext* c, int reg, uint32_t v);
void iop_loop_check(IOPContext* c);

#define IOP_LOOP_CHECK(c) do { if (__builtin_expect(g_iop_irq_pending, 0)) iop_loop_check(c); } while (0)
void iop_loop_check_at(IOPContext* c, uint32_t target);
#define IOP_LOOP_CHECK_AT(c, t) do { if (__builtin_expect(g_iop_irq_pending, 0)) iop_loop_check_at(c, t); } while (0)
#ifdef EE_TRACE_FUNCS
#define IOP_FUNC_ENTRY(c, a) ((c)->cur_func = (a))
#else
#define IOP_FUNC_ENTRY(c, a) ((void)0)
#endif

#ifdef __cplusplus
}
#endif
