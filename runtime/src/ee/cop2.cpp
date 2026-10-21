// COP2 (VU0 macro mode) control registers and micro-program calls.
#include "../common.h"
#include "ee_recomp.h"
#include "../vu/vu.h"

extern "C" {

uint32_t ee_cfc2(EEContext* c, int reg) {
    if (reg < 16) return c->vi[reg] & 0xFFFF;
    switch (reg) {
    case 16: return c->vstatus;
    case 17: return c->vmac;
    case 18: return c->vclip;
    case 20: return c->vr;
    case 21: { uint32_t u; memcpy(&u, &c->vI, 4); return u; }
    case 22: { uint32_t u; memcpy(&u, &c->vq, 4); return u; }
    case 29: return 0;  // VPU-STAT: VU0/VU1 idle
    default: return c->cop0[0];
    }
}

void ee_ctc2(EEContext* c, int reg, uint32_t v) {
    if (reg < 16) { if (reg) c->vi[reg] = v & 0xFFFF; return; }
    switch (reg) {
    case 16: c->vstatus = v; break;
    case 18: c->vclip = v & 0xFFFFFF; break;
    case 20: c->vr = v & 0x7FFFFF; break;
    case 21: memcpy(&c->vI, &v, 4); break;
    case 22: memcpy(&c->vq, &v, 4); break;
    case 28: break;  // FBRST: VU resets (nothing to do)
    default: break;
    }
}

void vu0_callms(EEContext* c, uint32_t addr) { vu::vu0_callms_ctx(c, addr); }
void vu0_callmsr(EEContext* c) { vu::vu0_callms_ctx(c, c->cop0[27] * 8); }
int vu0_cpcond(EEContext* c) {
    (void)c;
    return 0;
}

}
