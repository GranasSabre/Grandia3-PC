// GS usage census (--gs-census=FROM:UNTIL): which render targets, textures, transfers, CLUT sources and
// pixel pipeline features the game uses, and where textures come from (uploads, earlier draws in the
// same or another pixel format, copies). Analysis tool for the hardware renderer; costs nothing when off.
#include "gs_census.h"
#include "gs_drawstate.h"
#include "../ee/kernel.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <map>
#include <string>
#include <vector>

namespace gs {

static double g_from = -1, g_until = -1;
static bool g_on = false, g_reported = false;
static u64 g_frame = 0;

enum Kind : u8 { K_NONE, K_UPLOAD, K_DRAW, K_ZDRAW, K_COPY };
struct BlockOwner {
    u8 kind = K_NONE, psm = 0;
    u16 bw = 0;
    u32 bp = 0;
    u64 frame = 0;
};
static BlockOwner g_blocks[16384];

static std::map<std::string, u64> g_rt, g_tex, g_src, g_blend, g_atest, g_flags, g_up, g_copy, g_read, g_clut, g_disp, g_ztest;

static const char* psm_name(u32 psm) {
    switch (psm) {
    case PSMCT32: return "CT32"; case PSMCT24: return "CT24"; case PSMCT16: return "CT16"; case PSMCT16S: return "CT16S";
    case PSMT8: return "T8"; case PSMT4: return "T4"; case PSMT8H: return "T8H"; case PSMT4HL: return "T4HL"; case PSMT4HH: return "T4HH";
    case PSMZ32: return "Z32"; case PSMZ24: return "Z24"; case PSMZ16: return "Z16"; case PSMZ16S: return "Z16S";
    }
    return "?";
}

static std::string fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return b;
}

// block geometry of a pixel format
static void block_dims(u32 psm, int& bw, int& bh) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: bw = 16; bh = 8; break;
    case PSMT8: bw = 16; bh = 16; break;
    case PSMT4: bw = 32; bh = 16; break;
    default: bw = 8; bh = 8; break;  // 32-bit layouts (incl. T8H/T4HL/T4HH)
    }
}

static u32 block_of(u32 psm, int x, int y, u32 bp, u32 bw) {
    switch (psm) {
    case PSMCT16: return addr16(x, y, bp, bw) >> 7;
    case PSMCT16S: return addr16s(x, y, bp, bw) >> 7;
    case PSMZ16: return addr16z(x, y, bp, bw) >> 7;
    case PSMZ16S: return addr16sz(x, y, bp, bw) >> 7;
    case PSMT8: return addr8(x, y, bp, bw) >> 8;
    case PSMT4: return addr4(x, y, bp, bw) >> 9;
    case PSMZ32: case PSMZ24: return addr32z(x, y, bp, bw) >> 6;
    default: return addr32(x, y, bp, bw) >> 6;
    }
}

template <class F>
static void for_blocks(u32 psm, int x0, int y0, int x1, int y1, u32 bp, u32 bw, F f) {
    int bwd, bht;
    block_dims(psm, bwd, bht);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(2047, x1); y1 = std::min(2047, y1);
    for (int y = y0 & ~(bht - 1); y <= y1; y += bht)
        for (int x = x0 & ~(bwd - 1); x <= x1; x += bwd) f(block_of(psm, x, y, bp, bw) & 16383);
}

static void mark(u32 psm, int x0, int y0, int x1, int y1, u32 bp, u32 bw, Kind k) {
    for_blocks(psm, x0, y0, x1, y1, bp, bw, [&](u32 b) {
        BlockOwner& o = g_blocks[b];
        o.kind = k;
        o.psm = (u8)psm;
        o.bp = bp;
        o.bw = (u16)bw;
        o.frame = g_frame;
    });
}

void census_enable(double from, double until) {
    g_from = from;
    g_until = until;
}

static void report();

bool census_active() {
    if (g_from < 0 || g_reported) return false;
    double t = host_time_sec();
    if (t < g_from) return false;
    if (t >= g_until) {
        report();
        return false;
    }
    g_on = true;
    return true;
}

void census_draw(const DrawState& ds, int prim, const Vertex* v) {
    int n = prim == 3 ? 3 : prim == 0 ? 1 : 2;
    int minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < n; i++) {
        minx = std::min(minx, v[i].x); maxx = std::max(maxx, v[i].x);
        miny = std::min(miny, v[i].y); maxy = std::max(maxy, v[i].y);
    }
    int x0 = std::max(minx >> 4, ds.scax0), x1 = std::min((maxx + 15) >> 4, ds.scax1);
    int y0 = std::max(miny >> 4, ds.scay0), y1 = std::min((maxy + 15) >> 4, ds.scay1);
    if (x1 < x0 || y1 < y0) return;
    u64 area = (u64)(x1 - x0 + 1) * (y1 - y0 + 1);
    const char* pn = prim == 3 ? "tri" : prim == 6 ? "sprite" : prim == 1 ? "line" : "point";
    g_rt[fmt("fb %05x/%2u/%-5s msk=%08x %s", ds.fbp, ds.fbw, psm_name(ds.fpsm), ds.fbmsk, pn)]++;
    g_flags[fmt("prims %s", pn)]++;
    g_flags["pixels (bbox)"] += area;
    // texture source
    if (ds.tme) {
        int tw = 1 << ds.tw, th = 1 << ds.th;
        float umin = 1e30f, umax = -1e30f, vmin = 1e30f, vmax = -1e30f;
        for (int i = 0; i < n; i++) {
            float u, t;
            if (ds.fst) { u = v[i].u / 16.0f; t = v[i].v / 16.0f; }
            else {
                float q = v[i].q != 0.0f ? v[i].q : 1e-10f;
                u = v[i].s / q * tw;
                t = v[i].t / q * th;
            }
            umin = std::min(umin, u); umax = std::max(umax, u);
            vmin = std::min(vmin, t); vmax = std::max(vmax, t);
        }
        int u0 = (int)std::max(0.0f, std::floor(umin) - 1), u1 = (int)std::min((float)tw - 1, std::ceil(umax) + 1);
        int t0 = (int)std::max(0.0f, std::floor(vmin) - 1), t1 = (int)std::min((float)th - 1, std::ceil(vmax) + 1);
        if (ds.wms >= 2) { u0 = std::max(u0, (int)ds.minu); u1 = std::min(u1, (int)ds.maxu); }
        if (ds.wmt >= 2) { t0 = std::max(t0, (int)ds.minv); t1 = std::min(t1, (int)ds.maxv); }
        if (ds.wms == 0 && (umin < 0 || umax > tw)) { u0 = 0; u1 = tw - 1; }
        if (ds.wmt == 0 && (vmin < 0 || vmax > th)) { t0 = 0; t1 = th - 1; }
        std::map<std::string, int> kinds;
        for_blocks(ds.tpsm, u0, t0, u1, t1, ds.tbp, ds.tbw, [&](u32 b) {
            const BlockOwner& o = g_blocks[b];
            std::string k;
            switch (o.kind) {
            case K_NONE: k = "unwritten"; break;
            case K_UPLOAD: k = fmt("upload %s", psm_name(o.psm)); break;
            case K_COPY: k = fmt("copy %s", psm_name(o.psm)); break;
            case K_DRAW: case K_ZDRAW:
                k = fmt("%s %s%s%s", o.kind == K_ZDRAW ? "zbuf" : "rt", psm_name(o.psm),
                        o.psm != ds.tpsm ? " REINTERP" : "", o.frame == g_frame ? " thisframe" : "");
                if (o.bp != ds.tbp || o.bw != ds.tbw) k += fmt(" view %x/%u", o.bp, o.bw);
                break;
            }
            kinds[k]++;
        });
        std::string ks;
        for (auto& [k, c] : kinds) ks += (ks.empty() ? "" : ", ") + k;
        g_tex[fmt("tex %05x/%2u/%-5s %4dx%-4d clut=%s fb=%05x", ds.tbp, ds.tbw, psm_name(ds.tpsm), tw, th,
                  (ds.tpsm == PSMT8 || ds.tpsm == PSMT4 || ds.tpsm >= PSMT8H) && ds.tpsm < PSMZ32 ? psm_name(ds.cpsm) : "-", ds.fbp)]++;
        g_src[fmt("read %-5s from [%s]", psm_name(ds.tpsm), ks.c_str())]++;
        if (ds.tbp == ds.fbp) g_flags["texture == framebuffer (feedback)"]++;
        g_flags[fmt("tfx=%u tcc=%u", ds.tfx, ds.tcc)]++;
        g_flags[fmt("filter mag=%d min=%d mxl=%u", ds.mag_linear, ds.min_linear, ds.mxl)]++;
        g_flags[fmt("wrap %u/%u", ds.wms, ds.wmt)]++;
        g_flags[fmt("coords %s", ds.fst ? "UV" : "STQ")]++;
        if (ds.tpsm == PSMCT24 || ds.tpsm == PSMCT16 || ds.tpsm == PSMCT16S || (ds.cpsm == PSMCT16 && ds.tpsm >= PSMT8))
            g_flags[fmt("texa ta0=%x ta1=%x aem=%d", ds.ta0, ds.ta1, ds.aem)]++;
    }
    if (ds.abe || ds.pabe) g_blend[fmt("(%u-%u)*%u+%u fix=%02x fb=%s pabe=%d", ds.ba, ds.bb, ds.bc, ds.bd, ds.bfix, psm_name(ds.fpsm), ds.pabe)]++;
    else g_blend["off"]++;
    if (ds.ate) g_atest[fmt("atst=%u aref=%02x afail=%u", ds.atst, ds.aref, ds.afail)]++;
    else g_atest["off"]++;
    if (ds.date) g_flags[fmt("DATE datm=%d", ds.datm)]++;
    if (ds.fba) g_flags["FBA"]++;
    if (!ds.colclamp) g_flags["COLCLAMP=0 (wrap)"]++;
    if (ds.dthe) g_flags["dither"]++;
    if (ds.fge) g_flags["fog"]++;
    if (!ds.iip) g_flags["flat shading"]++;
    if (ds.zte) g_ztest[fmt("z %05x/%s ztst=%u zmsk=%d", ds.zbp, psm_name(ds.zpsm), ds.ztst, ds.zmsk)]++;
    else g_ztest["zte=0"]++;
    // destination blocks
    bool fbwrite = ds.fbmsk != 0xFFFFFFFFu;
    if (fbwrite) mark(ds.fpsm, x0, y0, x1, y1, ds.fbp, ds.fbw, K_DRAW);
    if (ds.zte && !ds.zmsk && ds.ztst != 0) mark(ds.zpsm, x0, y0, x1, y1, ds.zbp, ds.fbw, K_ZDRAW);
}

void census_upload(u32 dbp, u32 dbw, u32 dpsm, u32 dx, u32 dy, u32 w, u32 h) {
    g_up[fmt("upload %05x/%2u/%-5s %4ux%-4u", dbp, dbw, psm_name(dpsm), w, h)]++;
    // what was there before?
    std::map<std::string, int> kinds;
    for_blocks(dpsm, dx, dy, dx + w - 1, dy + h - 1, dbp, dbw, [&](u32 b) {
        const BlockOwner& o = g_blocks[b];
        if (o.kind == K_DRAW) kinds[fmt("over rt %s", psm_name(o.psm))]++;
    });
    for (auto& [k, c] : kinds) g_flags[fmt("upload %s %s", psm_name(dpsm), k.c_str())]++;
    mark(dpsm, dx, dy, dx + w - 1, dy + h - 1, dbp, dbw, K_UPLOAD);
}

void census_copy(u32 sbp, u32 sbw, u32 spsm, u32 sx, u32 sy, u32 dbp, u32 dbw, u32 dpsm, u32 dx, u32 dy, u32 w, u32 h) {
    std::map<std::string, int> kinds;
    for_blocks(spsm, sx, sy, sx + w - 1, sy + h - 1, sbp, sbw, [&](u32 b) {
        const BlockOwner& o = g_blocks[b];
        kinds[o.kind == K_DRAW ? "rt" : o.kind == K_UPLOAD ? "upload" : o.kind == K_COPY ? "copy" : o.kind == K_ZDRAW ? "zbuf" : "none"]++;
    });
    std::string ks;
    for (auto& [k, c] : kinds) ks += (ks.empty() ? "" : ",") + k;
    g_copy[fmt("copy %05x/%u/%s (%u,%u) -> %05x/%u/%s (%u,%u) %ux%u src=[%s]", sbp, sbw, psm_name(spsm), sx, sy, dbp, dbw,
               psm_name(dpsm), dx, dy, w, h, ks.c_str())]++;
    mark(dpsm, dx, dy, dx + w - 1, dy + h - 1, dbp, dbw, K_COPY);
}

void census_readback(u32 sbp, u32 sbw, u32 spsm, u32 sx, u32 sy, u32 w, u32 h) {
    g_read[fmt("readback %05x/%u/%s (%u,%u) %ux%u", sbp, sbw, psm_name(spsm), sx, sy, w, h)]++;
}

void census_clut(u64 tex0, u64 texclut) {
    u32 cbp = bits(tex0, 37, 14), cpsm = bits(tex0, 51, 4), csm = bits(tex0, 55, 1), psm = bits(tex0, 20, 6);
    BlockOwner& o = g_blocks[cbp & 16383];
    const char* src = o.kind == K_DRAW ? "rt" : o.kind == K_UPLOAD ? "upload" : o.kind == K_COPY ? "copy" : "none";
    g_clut[fmt("clut for %s cpsm=%s csm=%u src=%s%s", psm_name(psm), psm_name(cpsm), csm, src,
               o.kind == K_DRAW && o.frame == g_frame ? " thisframe" : "")]++;
}

void census_frame(u64 dispfb1, u64 dispfb2, u64 pmode) {
    g_frame++;
    g_disp[fmt("pmode=%llx dispfb1=%05x/%u/%s dispfb2=%05x/%u/%s", (unsigned long long)pmode, bits(dispfb1, 0, 9) * 32,
               bits(dispfb1, 9, 6), psm_name(bits(dispfb1, 15, 5)), bits(dispfb2, 0, 9) * 32, bits(dispfb2, 9, 6),
               psm_name(bits(dispfb2, 15, 5)))]++;
}

static void dump(const char* title, const std::map<std::string, u64>& m, size_t top) {
    std::vector<std::pair<u64, std::string>> v;
    for (auto& [k, c] : m) v.push_back({c, k});
    std::sort(v.rbegin(), v.rend());
    log_write(LOG_GS, "census --- %s (%zu distinct)", title, v.size());
    for (size_t i = 0; i < v.size() && i < top; i++) log_write(LOG_GS, "census %10llu  %s", (unsigned long long)v[i].first, v[i].second.c_str());
}

static void report() {
    if (g_reported) return;
    g_reported = true;
    log_write(LOG_GS, "census: %llu frames between %.1f and %.1f s", (unsigned long long)g_frame, g_from, g_until);
    dump("render targets", g_rt, 60);
    dump("textures", g_tex, 80);
    dump("texture sources", g_src, 80);
    dump("blend", g_blend, 40);
    dump("alpha test", g_atest, 30);
    dump("z", g_ztest, 30);
    dump("flags", g_flags, 80);
    dump("uploads", g_up, 60);
    dump("copies", g_copy, 60);
    dump("readbacks", g_read, 30);
    dump("clut loads", g_clut, 30);
    dump("display", g_disp, 20);
}

}  // namespace gs
