// Decoded GS draw state of one primitive (shared by the software rasterizer and analysis tools).
#pragma once
#include "gs_state.h"
#include <cstring>
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

// Same drawing result for the same primitive (the CLUT and texture memory pointers aside): register
// writes that change nothing the rasterizers use (primitive type, XYOFFSET, unused bits) keep the batch
inline bool same_draw_state(const DrawState& a, const DrawState& b) {
    if (a.fbp != b.fbp || a.fbw != b.fbw || a.fpsm != b.fpsm || a.fbmsk != b.fbmsk || a.zbp != b.zbp || a.zpsm != b.zpsm ||
        a.zmsk != b.zmsk || a.ate != b.ate || a.atst != b.atst || a.aref != b.aref || a.afail != b.afail || a.date != b.date ||
        a.datm != b.datm || a.zte != b.zte || a.ztst != b.ztst || a.abe != b.abe || a.ba != b.ba || a.bb != b.bb ||
        a.bc != b.bc || a.bd != b.bd || a.bfix != b.bfix || a.pabe != b.pabe || a.fba != b.fba || a.colclamp != b.colclamp ||
        a.tme != b.tme || a.fst != b.fst || a.tbp != b.tbp || a.tbw != b.tbw || a.tpsm != b.tpsm || a.tw != b.tw ||
        a.th != b.th || a.tcc != b.tcc || a.tfx != b.tfx || a.cpsm != b.cpsm || a.csa != b.csa || a.wms != b.wms ||
        a.wmt != b.wmt || a.minu != b.minu || a.maxu != b.maxu || a.minv != b.minv || a.maxv != b.maxv || a.mag_linear != b.mag_linear ||
        a.min_linear != b.min_linear || a.lcm != b.lcm || a.mxl != b.mxl || a.lodl != b.lodl || a.mmin != b.mmin || a.mtba != b.mtba ||
        a.miptbp1 != b.miptbp1 || a.miptbp2 != b.miptbp2 || a.lodk != b.lodk || a.ta0 != b.ta0 || a.ta1 != b.ta1 || a.aem != b.aem ||
        a.fge != b.fge || a.fogcol != b.fogcol || a.iip != b.iip || a.scax0 != b.scax0 || a.scax1 != b.scax1 || a.scay0 != b.scay0 ||
        a.scay1 != b.scay1 || a.dthe != b.dthe || a.fb24 != b.fb24 || a.fb16 != b.fb16)
        return false;
    return memcmp(a.dimx, b.dimx, sizeof a.dimx) == 0;
}

}  // namespace gs
