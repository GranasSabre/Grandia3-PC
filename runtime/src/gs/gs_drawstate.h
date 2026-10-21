// Decoded GS draw state of one primitive (shared by the software rasterizer and analysis tools).
#pragma once
#include "gs_state.h"
#include <windows.h>
#undef DrawState  // winuser.h maps DrawState to DrawStateA

namespace gs {

struct DrawState {
    // frame / z buffer
    u32 fbp, fbw, fpsm, fbmsk;
    u32 zbp, zpsm;
    bool zmsk;
    // tests
    bool ate;
    u32 atst, aref, afail;
    bool date, datm;
    bool zte;
    u32 ztst;
    // blending
    bool abe;
    u32 ba, bb, bc, bd, bfix;
    bool pabe;
    u32 fba;
    bool colclamp;
    // texture
    bool tme, fst;
    u32 tbp, tbw, tpsm, tw, th, tcc, tfx, cpsm, csa;
    u32 wms, wmt, minu, maxu, minv, maxv;
    bool mag_linear, min_linear;
    u32 lcm, mxl, lodl;
    u32 mmin, mtba;           // raw minification filter (0-5), automatic mip base addresses
    u64 miptbp1, miptbp2;     // mip level base pointers/widths (levels 1-3, 4-6)
    float lodk;
    u32 ta0, ta1;
    bool aem;
    // misc
    bool fge;
    u32 fogcol;
    bool iip;
    int scax0, scax1, scay0, scay1;
    bool dthe;
    int dimx[4][4];
    bool fb24, fb16;
    const u32* clut;  // CLUT snapshot used by this primitive
    const u32* tmem;  // texture memory: VRAM, or a snapshot of it for primitives sampling their own target
};

}  // namespace gs
