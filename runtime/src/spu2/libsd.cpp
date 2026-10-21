// libsd (IOP sound library) HLE on top of the SPU2 emulation core.
// Entry encodings follow the libsd headers: SD_VOICE(core, v) = core | (v << 1); parameter / switch /
// address register index in bits 8..15. Addresses passed in/out are SPU2 byte addresses.
#include "../common.h"
#include "../savestate.h"
#include "../memory.h"
#include "iop_recomp.h"
#include "spu2.h"

using namespace spu2;

static inline u32 A(IOPContext* c, int i) {
    if (i < 4) return c->r[4 + i];
    return mem::iop_rd32(c->r[29] + 16 + (i - 4) * 4);
}

// ---------------------------------------------------------------------------
// Effect presets (PS1/PS2 reverb parameter sets, in register order:
// dAPF1 dAPF2 vIIR vCOMB1..4 vWALL vAPF1 vAPF2 mLSAME mRSAME mLCOMB1 mRCOMB1 mLCOMB2 mRCOMB2 dLSAME dRSAME
// mLDIFF mRDIFF mLCOMB3 mRCOMB3 mLCOMB4 mRCOMB4 dLDIFF dRDIFF mLAPF1 mRAPF1 mLAPF2 mRAPF2 vLIN vRIN)
// ---------------------------------------------------------------------------
static const u16 EFFECT_SIZES[10] = {0x1, 0x26C, 0x1F4, 0x484, 0x6FE, 0xADE, 0xF6C, 0x1804, 0x1804, 0x3C0};
static const u16 EFFECT_PARAMS[10][32] = {
    // OFF
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    // ROOM
    {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
     0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000},
    // STUDIO_1
    {0x0033, 0x0025, 0x70F0, 0x4FA8, 0xBCE0, 0x4410, 0xC0F0, 0x9C00, 0x5280, 0x4EC0, 0x03E4, 0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266,
     0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135, 0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, 0x8000, 0x8000},
    // STUDIO_2
    {0x00B1, 0x007F, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xB4C0, 0x5280, 0x4EC0, 0x0904, 0x076B, 0x0824, 0x065F, 0x07A2, 0x0616,
     0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305, 0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, 0x8000, 0x8000},
    // STUDIO_3
    {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB, 0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973,
     0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0, 0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000},
    // HALL
    {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA, 0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1,
     0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1, 0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000},
    // SPACE
    {0x033D, 0x0231, 0x7E00, 0x5000, 0xB400, 0xB000, 0x4C00, 0xB000, 0x6000, 0x5400, 0x1ED6, 0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2,
     0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D, 0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, 0x8000, 0x8000},
    // ECHO
    {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
     0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    // DELAY
    {0x0003, 0x0003, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFD, 0x0FFD, 0x1009, 0x0009, 0x0000, 0x0000,
     0x1009, 0x0009, 0x1FFF, 0x1FFF, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1FFE, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    // PIPE
    {0x0017, 0x0013, 0x70F0, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0x8500, 0x5F80, 0x54C0, 0x0371, 0x02AF, 0x02E5, 0x01DF, 0x02B0, 0x01D7,
     0x0358, 0x026A, 0x01D6, 0x011E, 0x012D, 0x00B1, 0x011F, 0x0059, 0x01A0, 0x00E3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000},
};

struct EffectAttr { s32 core, mode; s16 depth_l, depth_r; s32 delay, feedback; };
static EffectAttr g_effect[2];
static u16 g_spdif = 0;

static void set_reverb_regs(int core, const u16* d) {
    ReverbRegs& r = g_core[core].rv;
    auto a = [&](int i) { return (u32)d[i] << 2; };  // units of 8 bytes -> halfwords
    r.apf1_size = a(0);
    r.apf2_size = a(1);
    r.iir_vol = (s16)d[2];
    r.comb1_vol = (s16)d[3];
    r.comb2_vol = (s16)d[4];
    r.comb3_vol = (s16)d[5];
    r.comb4_vol = (s16)d[6];
    r.wall_vol = (s16)d[7];
    r.apf1_vol = (s16)d[8];
    r.apf2_vol = (s16)d[9];
    r.same_l_dst = a(10);
    r.same_r_dst = a(11);
    r.comb1_l = a(12);
    r.comb1_r = a(13);
    r.comb2_l = a(14);
    r.comb2_r = a(15);
    r.same_l_src = a(16);
    r.same_r_src = a(17);
    r.diff_l_dst = a(18);
    r.diff_r_dst = a(19);
    r.comb3_l = a(20);
    r.comb3_r = a(21);
    r.comb4_l = a(22);
    r.comb4_r = a(23);
    r.diff_l_src = a(24);
    r.diff_r_src = a(25);
    r.apf1_l = a(26);
    r.apf1_r = a(27);
    r.apf2_l = a(28);
    r.apf2_r = a(29);
    r.in_l = (s16)d[30];
    r.in_r = (s16)d[31];
}

static u32 eea_bytes(int core) { return (g_core[core].eea_hi << 17) | 0x1FFFF; }

static void clear_effect_area(int core, int mode) {
    if (mode <= 0 || mode > 9) return;
    u32 size = (u32)EFFECT_SIZES[mode] << 4;
    u32 start = (eea_bytes(core) - (size - 2)) >> 1;
    u32 end = eea_bytes(core) >> 1;
    for (u32 a = start; a <= end; a++) g_ram[a & RAM_MASK] = 0;
}

static void set_effect_esa(int core, int mode) {
    u32 esa = eea_bytes(core) - (((u32)EFFECT_SIZES[mode] << 4) - 2);
    write_esa(core, (esa >> 1) & RAM_MASK);
}

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------
static void set_param(u16 entry, u16 val) {
    int core = entry & 1;
    int idx = (entry >> 8) & 0xFF;
    Core& c = g_core[core];
    if (idx < 8) {
        int vi = (entry >> 1) & 0x1F;
        if (vi >= 24) return;
        Voice& v = c.v[vi];
        switch (idx) {
        case 0: v.vol_l.write(val); break;
        case 1: v.vol_r.write(val); break;
        case 2: v.pitch = val; break;
        case 3: v.adsr1 = val; break;
        case 4: v.adsr2 = val; break;
        case 5: v.envx = (s16)val; break;
        case 6: v.vol_l.level = (s16)val; break;
        case 7: v.vol_r.level = (s16)val; break;
        }
        return;
    }
    switch (idx) {
    case 8: c.mmix = val; break;
    case 9: c.mvol_l.write(val); break;
    case 10: c.mvol_r.write(val); break;
    case 11: c.evol_l = (s16)val; break;
    case 12: c.evol_r = (s16)val; break;
    case 13: c.avol_l = (s16)val; break;
    case 14: c.avol_r = (s16)val; break;
    case 15: c.bvol_l = (s16)val; break;
    case 16: c.bvol_r = (s16)val; break;
    case 17: c.mvol_l.level = (s16)val; break;
    case 18: c.mvol_r.level = (s16)val; break;
    default: LOG(LOG_SPU, "sceSdSetParam: unknown entry %04x", entry); break;
    }
}

static u16 get_param(u16 entry) {
    int core = entry & 1;
    int idx = (entry >> 8) & 0xFF;
    Core& c = g_core[core];
    if (idx < 8) {
        int vi = (entry >> 1) & 0x1F;
        if (vi >= 24) return 0;
        Voice& v = c.v[vi];
        switch (idx) {
        case 0: return v.vol_l.reg;
        case 1: return v.vol_r.reg;
        case 2: return v.pitch;
        case 3: return v.adsr1;
        case 4: return v.adsr2;
        case 5: return (u16)v.envx;
        case 6: return (u16)v.vol_l.level;
        case 7: return (u16)v.vol_r.level;
        }
        return 0;
    }
    switch (idx) {
    case 8: return c.mmix;
    case 9: return c.mvol_l.reg;
    case 10: return c.mvol_r.reg;
    case 11: return (u16)c.evol_l;
    case 12: return (u16)c.evol_r;
    case 13: return (u16)c.avol_l;
    case 14: return (u16)c.avol_r;
    case 15: return (u16)c.bvol_l;
    case 16: return (u16)c.bvol_r;
    case 17: return (u16)c.mvol_l.level;
    case 18: return (u16)c.mvol_r.level;
    }
    return 0;
}

static void set_switch(u16 entry, u32 val) {
    Core& c = g_core[entry & 1];
    val &= 0xFFFFFF;
    switch ((entry >> 8) & 0xFF) {
    case 0x13: c.pmon = val & ~1u; break;
    case 0x14: c.non = val; break;
    case 0x15: c.kon_last = val; key_on(entry & 1, val); break;
    case 0x16: c.koff_last = val; key_off(entry & 1, val); break;
    case 0x17: c.endx = 0; break;  // writing ENDX clears it
    case 0x18: c.vmixl = val; break;
    case 0x19: c.vmixel = val; break;
    case 0x1A: c.vmixr = val; break;
    case 0x1B: c.vmixer = val; break;
    default: LOG(LOG_SPU, "sceSdSetSwitch: unknown entry %04x", entry); break;
    }
}

static u32 get_switch(u16 entry) {
    Core& c = g_core[entry & 1];
    switch ((entry >> 8) & 0xFF) {
    case 0x13: return c.pmon;
    case 0x14: return c.non;
    case 0x15: return c.kon_last;
    case 0x16: return c.koff_last;
    case 0x17: return c.endx;
    case 0x18: return c.vmixl;
    case 0x19: return c.vmixel;
    case 0x1A: return c.vmixr;
    case 0x1B: return c.vmixer;
    }
    return 0;
}

static void set_addr(u16 entry, u32 val) {
    int core = entry & 1;
    Core& c = g_core[core];
    u32 hw = (val >> 1) & 0xFFFF8;  // libsd writes (val >> 17) and (val >> 1) & 0xFFF8
    switch ((entry >> 8) & 0xFF) {
    case 0x1C: write_esa(core, hw); break;
    case 0x1D: c.eea_hi = (val >> 17) & 0xF; break;
    case 0x1E: c.tsa = hw; break;
    case 0x1F: c.irqa = hw; break;
    case 0x20: case 0x21: case 0x22: {
        int vi = (entry >> 1) & 0x1F;
        if (vi >= 24) break;
        Voice& v = c.v[vi];
        int idx = (entry >> 8) & 0xFF;
        if (idx == 0x20) v.ssa = hw;
        else if (idx == 0x21) v.lsa = hw;
        else v.cur = hw;
        break;
    }
    default: LOG(LOG_SPU, "sceSdSetAddr: unknown entry %04x", entry); break;
    }
}

static u32 get_addr(u16 entry) {
    int core = entry & 1;
    Core& c = g_core[core];
    switch ((entry >> 8) & 0xFF) {
    case 0x1C: return c.esa << 1;
    case 0x1D: return eea_bytes(core);
    case 0x1E: return c.tsa << 1;
    case 0x1F: return c.irqa << 1;
    case 0x20: case 0x21: case 0x22: {
        int vi = (entry >> 1) & 0x1F;
        if (vi >= 24) return 0;
        int idx = (entry >> 8) & 0xFF;
        if (idx == 0x20) return c.v[vi].ssa << 1;
        if (idx == 0x21) return c.v[vi].lsa << 1;
        return voice_nax(core, vi) << 1;
    }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
static void sd_init(int flag) {
    flag &= 1;
    for (int core = 0; core < 2; core++) {
        Core& c = g_core[core];
        adma_stop(core);
        if (flag == 0) {
            c.mvol_l.write(0);
            c.mvol_r.write(0);
        }
        for (int i = 0; i < 24; i++) {
            Voice& v = c.v[i];
            v.vol_l.write(0);
            v.vol_r.write(0);
            v.pitch = 0x3FFF;
            v.adsr1 = v.adsr2 = 0;
            v.ssa = 0x5000 >> 1;
            v.phase = ADSR_OFF;
            v.envx = 0;
        }
        c.endx = 0;
        c.pmon = c.non = 0;
        g_effect[core] = EffectAttr{core, 0, 0, 0, 0, 0};
    }
    // silent looping block at 0x5000 used as default voice start address
    static const u16 init_data[16] = {0x707, 0x707, 0x707, 0x707, 0x707, 0x707, 0x707, 0x707, 0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; i++) g_ram[(0x5000 >> 1) + i] = init_data[i];
    for (int core = 0; core < 2; core++) {
        Core& c = g_core[core];
        set_attr(core, (u16)(0x8000 | 0x4000 | (flag ? 0x80 : 0) | (core ? 1 : 0)));
        c.vmixl = c.vmixr = c.vmixel = c.vmixer = 0xFFFFFF;
        c.mmix = core ? 0xFFC : 0xFF0;
        if (flag == 0) {
            c.mvol_l.write(0);
            c.mvol_r.write(0);
            c.evol_l = c.evol_r = 0;
            c.eea_hi = core ? 0xF : 0xE;
        }
        c.avol_l = c.avol_r = core ? 0x7FFF : 0;  // core1 external input = core0 output
        c.bvol_l = c.bvol_r = 0;
        if (flag == 0) set_effect_esa(core, 0);
    }
}

static int set_effect_attr(int core, u32 attr_addr) {
    core &= 1;
    EffectAttr a;
    a.core = (s32)mem::iop_rd32(attr_addr);
    a.mode = (s32)mem::iop_rd32(attr_addr + 4);
    u32 d = mem::iop_rd32(attr_addr + 8);
    a.depth_l = (s16)(d & 0xFFFF);
    a.depth_r = (s16)(d >> 16);
    a.delay = (s32)mem::iop_rd32(attr_addr + 12);
    a.feedback = (s32)mem::iop_rd32(attr_addr + 16);
    u32 mode = (u32)a.mode;
    bool clear = false;
    int last_mode = 0;
    if (mode & 0x100) {
        mode &= ~0x100u;
        clear = true;
        last_mode = g_effect[core].mode;
    }
    if (mode > 9) return -1;
    u16 md[32];
    memcpy(md, EFFECT_PARAMS[mode], sizeof(md));
    g_effect[core] = a;
    g_effect[core].core = core;
    g_effect[core].mode = (s32)mode;
    if (mode == 7 || mode == 8) {  // ECHO / DELAY: delay time and feedback are user parameters
        g_effect[core].delay = 0x80;
        g_effect[core].feedback = mode == 7 ? 0x80 : 0;
        u16 delay = (u16)((a.delay + 1) << 5);
        u16 feedback = (u16)(((a.feedback << 7) + a.feedback) << 1);
        md[7] = feedback;
        md[10] = (u16)((delay << 1) - md[0]);
        md[11] = (u16)(delay - md[1]);
        md[16] = (u16)(md[17] + delay);
        md[12] = (u16)(md[13] + delay);
        md[27] = (u16)(md[29] + delay);
        md[26] = (u16)(md[28] + delay);
    } else {
        g_effect[core].delay = 0;
        g_effect[core].feedback = 0;
    }
    Core& c = g_core[core];
    bool fx_on = (c.attr & 0x80) != 0;
    if (fx_on) c.attr &= ~0x80;
    if (fx_on && clear) clear_effect_area(core, last_mode);
    c.evol_l = a.depth_l;
    c.evol_r = a.depth_r;
    set_reverb_regs(core, md);
    set_effect_esa(core, (int)mode);
    if (clear) clear_effect_area(core, (int)mode);
    if (fx_on) c.attr |= 0x80;
    return 0;
}

// ---------------------------------------------------------------------------
// Imports
// ---------------------------------------------------------------------------
extern "C" {

void iop_imp_libsd_4(IOPContext* c) {  // sceSdInit(flag)
    LOG(LOG_SPU, "sceSdInit(%x)", A(c, 0));
    sd_init((int)A(c, 0));
    c->r[2] = 0;
}

void iop_imp_libsd_5(IOPContext* c) {  // sceSdSetParam(entry, value)
    LOG(LOG_SPU, "sceSdSetParam(%04x, %04x)", A(c, 0) & 0xFFFF, A(c, 1) & 0xFFFF);
    set_param((u16)A(c, 0), (u16)A(c, 1));
}

void iop_imp_libsd_6(IOPContext* c) {  // sceSdGetParam(entry)
    c->r[2] = get_param((u16)A(c, 0));
}

void iop_imp_libsd_7(IOPContext* c) {  // sceSdSetSwitch(entry, value)
    LOG(LOG_SPU, "sceSdSetSwitch(%04x, %06x)", A(c, 0) & 0xFFFF, A(c, 1));
    set_switch((u16)A(c, 0), A(c, 1));
}

void iop_imp_libsd_8(IOPContext* c) {  // sceSdGetSwitch(entry)
    c->r[2] = get_switch((u16)A(c, 0));
}

void iop_imp_libsd_9(IOPContext* c) {  // sceSdSetAddr(entry, value)
    LOG(LOG_SPU, "sceSdSetAddr(%04x, %06x)", A(c, 0) & 0xFFFF, A(c, 1));
    set_addr((u16)A(c, 0), A(c, 1));
}

void iop_imp_libsd_10(IOPContext* c) {  // sceSdGetAddr(entry)
    c->r[2] = get_addr((u16)A(c, 0));
}

void iop_imp_libsd_11(IOPContext* c) {  // sceSdSetCoreAttr(entry, value)
    u16 entry = (u16)A(c, 0), val = (u16)A(c, 1);
    int core = entry & 1;
    LOG(LOG_SPU, "sceSdSetCoreAttr(%04x, %x)", entry, val);
    u16 attr = g_core[core].attr;
    switch (entry & ~1) {
    case 0x2: attr = (u16)((attr & ~0x80) | ((val & 1) << 7)); break;    // effect enable
    case 0x4: attr = (u16)((attr & ~0x40) | ((val & 1) << 6)); break;    // IRQ enable
    case 0x6: attr = (u16)((attr & ~0x4000) | ((val & 1) << 14)); break; // mute
    case 0x8: attr = (u16)((attr & ~0x3F00) | ((val & 0x3F) << 8)); break;  // noise clock
    case 0xA: g_spdif = val; break;
    }
    set_attr(core, attr);
}

void iop_imp_libsd_12(IOPContext* c) {  // sceSdGetCoreAttr(entry)
    u16 entry = (u16)A(c, 0);
    u16 attr = g_core[entry & 1].attr;
    u32 r = 0;
    switch (entry & ~1) {
    case 0x2: r = (attr >> 7) & 1; break;
    case 0x4: r = (attr >> 6) & 1; break;
    case 0x6: r = (attr >> 14) & 1; break;
    case 0x8: r = (attr >> 8) & 0x3F; break;
    case 0xA: r = g_spdif; break;
    }
    c->r[2] = r;
}

void iop_imp_libsd_17(IOPContext* c) {  // sceSdVoiceTrans(chan, mode, iopaddr, spuaddr, size)
    u32 chan = A(c, 0) & 1, mode = A(c, 1) & 0xFFFF, iop = A(c, 2), spu = A(c, 3), size = A(c, 4);
    LOG(LOG_SPU, "sceSdVoiceTrans(%u, %x, %08x, %06x, %x)", chan, mode, iop, spu, size);
    (void)chan;
    u32 bytes = (mode & 8) ? size : ((size + 63) & ~63u);  // DMA moves whole 64-byte blocks
    u32 hw = (spu >> 1) & RAM_MASK;
    if ((mode & 3) == 0) {
        for (u32 i = 0; i < bytes / 2; i++)
            g_ram[(hw + i) & RAM_MASK] = (u16)(mem::iop_ram[(iop + i * 2) & 0x1FFFFF] |
                                              (mem::iop_ram[(iop + i * 2 + 1) & 0x1FFFFF] << 8));
    } else if ((mode & 3) == 1) {
        for (u32 i = 0; i < bytes / 2; i++) {
            u16 v = g_ram[(hw + i) & RAM_MASK];
            mem::iop_ram[(iop + i * 2) & 0x1FFFFF] = (u8)v;
            mem::iop_ram[(iop + i * 2 + 1) & 0x1FFFFF] = (u8)(v >> 8);
        }
    } else {
        c->r[2] = (u32)-1;
        return;
    }
    c->r[2] = size;
}

void iop_imp_libsd_18(IOPContext* c) {  // sceSdBlockTrans(chan, mode, iopaddr, size, [startaddr])
    u32 chan = A(c, 0) & 1, mode = A(c, 1) & 0xFFFF, iop = A(c, 2), size = A(c, 3);
    LOG(LOG_SPU, "sceSdBlockTrans(%u, %x, %08x, %x)", chan, mode, iop, size);
    bool loop = (mode & 0x10) != 0;
    u32 half = loop ? size / 2 : size;
    s32 r = -1;
    switch (mode & 3) {
    case 0:  // write (sound data input, AutoDMA)
        r = adma_start_write((int)chan, iop, half, loop, 0, 0);
        break;
    case 1: {  // read (capture voice / output data from sound RAM)
        u32 area = (((mode & 0xF00) << 1) + 0x400) & RAM_MASK;
        u32 count = (mode >> 12) & 0xF;
        r = adma_start_read((int)chan, iop, half, loop, area, count);
        break;
    }
    case 2:  // stop
        c->r[2] = adma_stop((int)chan);
        return;
    case 3: {  // write from a start address inside the buffer
        u32 start = A(c, 4);
        u32 buff = 0;
        u32 off = start ? start - iop : 0;
        if (off > half) {
            if (!loop) break;
            off -= half;
            buff = 1;
        }
        if (off & 1023) off = (off & ~1023u) + 1024;
        r = adma_start_write((int)chan, iop, half, loop, off, buff);
        break;
    }
    }
    if (r < 0) LOG(LOG_SPU, "sceSdBlockTrans: failed (transfer busy?)");
    c->r[2] = (u32)r;
}

void iop_imp_libsd_19(IOPContext* c) {  // sceSdVoiceTransStatus(chan, flag): transfers complete instantly
    c->r[2] = 1;
}

void iop_imp_libsd_20(IOPContext* c) {  // sceSdBlockTransStatus(chan, flag)
    c->r[2] = adma_status((int)(A(c, 0) & 1));
}

void iop_imp_libsd_23(IOPContext* c) {  // sceSdSetEffectAttr(core, attr)
    u32 p = A(c, 1);
    LOG(LOG_SPU, "sceSdSetEffectAttr(%d, mode=%x depth=%d/%d delay=%d feedback=%d)", A(c, 0), mem::iop_rd32(p + 4),
        (s16)mem::iop_rd32(p + 8), (s16)(mem::iop_rd32(p + 8) >> 16), mem::iop_rd32(p + 12), mem::iop_rd32(p + 16));
    c->r[2] = (u32)set_effect_attr((int)A(c, 0), p);
}

void iop_imp_libsd_24(IOPContext* c) {  // sceSdGetEffectAttr(core, attr)
    const EffectAttr& e = g_effect[A(c, 0) & 1];
    u32 p = A(c, 1);
    mem::iop_wr32(p, (u32)e.core);
    mem::iop_wr32(p + 4, (u32)e.mode);
    mem::iop_wr32(p + 8, (u32)(u16)e.depth_l | ((u32)(u16)e.depth_r << 16));
    mem::iop_wr32(p + 12, (u32)e.delay);
    mem::iop_wr32(p + 16, (u32)e.feedback);
}

void iop_imp_libsd_25(IOPContext* c) {  // sceSdClearEffectWorkArea(core, chan, mode)
    int mode = (int)A(c, 2);
    if (mode > 9) {
        c->r[2] = (u32)-1;
        return;
    }
    clear_effect_area((int)(A(c, 0) & 1), mode);
    c->r[2] = 0;
}

}  // extern "C"

void libsd_state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    for (auto& e : g_effect) s.pod(e, "libsd EffectAttr");
    s.v(g_spdif);
}
