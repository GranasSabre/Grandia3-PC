// Software rasterizer for the GS.
//
// Primitives are decoded on the emulation thread (draw state snapshot incl. CLUT) and queued for
// worker threads. Each worker owns interleaved 8-row bands of the render target and processes the
// whole queue in order, so every pixel sees its primitives in submission order. VRAM block ranges
// written/read by queued primitives are tracked; a primitive that would read data a queued one is
// still writing (render-to-texture, glow/blur passes) or overwrite data still being read forces a
// flush first. Everything else touching VRAM (transfers, CLUT loads, display output) flushes too.
#include "gs_state.h"
#include "gs_drawstate.h"
#include "gs_census.h"
#include "gs_gpu.h"
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <immintrin.h>
#include <windows.h>
#include "../ee/kernel.h"
#include "../platform/platform.h"

namespace spu2 { extern u64 g_spu_ticks; }

namespace gs {

// DrawState: see gs_drawstate.h

// Rows of the render target processed by one worker: 8-row bands, band % n == k.
struct Rows {
    int k, n;
    bool owns(int y) const { return n == 1 || ((y >> 3) % n) == k; }
    // does this worker own any row in [y0, y1)?
    bool owns_any(int y0, int y1) const {
        if (n == 1) return true;
        int b0 = y0 >> 3, b1 = (y1 - 1) >> 3;
        if (b1 - b0 + 1 >= n) return true;
        for (int b = b0; b <= b1; b++)
            if (b % n == k) return true;
        return false;
    }
};

static inline bool is_paletted(u32 psm) {
    return psm == PSMT8 || psm == PSMT4 || psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH;
}

static DrawState g_cur;  // decoded state of the current primitive (emulation thread)

// Decodes the draw state from the GS registers into g_cur. Returns true if it changed.
static bool setup() {
    u64 m = (st.prmodecont & 1) ? st.prim : st.prmode;
    int c = (int)bits(m, 9, 1);
    const Context& cx = st.ctx[c];
    // Most primitives share the state of their predecessor: skip the decode when nothing changed.
    struct Key {
        u64 m;
        Context cx;
        u64 pabe, colclamp, texa, fogcol, dthe, dimx;
    };
    static Key last;
    static bool valid = false;
    Key k{m, cx, st.pabe, st.colclamp, st.texa, st.fogcol, st.dthe, st.dimx};
    if (valid && memcmp(&k, &last, sizeof(Key)) == 0) return false;
    last = k;
    valid = true;
    DrawState& ds = g_cur;
    ds.iip = bits(m, 3, 1);
    ds.tme = bits(m, 4, 1);
    ds.fge = bits(m, 5, 1);
    ds.abe = bits(m, 6, 1);
    ds.fst = bits(m, 8, 1);
    ds.fbp = bits(cx.frame, 0, 9) * 32;
    ds.fbw = bits(cx.frame, 16, 6);
    ds.fpsm = bits(cx.frame, 24, 6);
    ds.fbmsk = (u32)(cx.frame >> 32);
    ds.zbp = bits(cx.zbuf, 0, 9) * 32;
    ds.zpsm = bits(cx.zbuf, 24, 4) | 0x30;
    ds.zmsk = bits(cx.zbuf, 32, 1);
    ds.ate = bits(cx.test, 0, 1);
    ds.atst = bits(cx.test, 1, 3);
    ds.aref = bits(cx.test, 4, 8);
    ds.afail = bits(cx.test, 12, 2);
    ds.date = bits(cx.test, 14, 1);
    ds.datm = bits(cx.test, 15, 1);
    ds.zte = bits(cx.test, 16, 1);
    ds.ztst = bits(cx.test, 17, 2);
    ds.ba = bits(cx.alpha, 0, 2);
    ds.bb = bits(cx.alpha, 2, 2);
    ds.bc = bits(cx.alpha, 4, 2);
    ds.bd = bits(cx.alpha, 6, 2);
    ds.bfix = bits(cx.alpha, 32, 8);
    ds.pabe = bits(st.pabe, 0, 1);
    ds.fba = bits(cx.fba, 0, 1) ? 0x80 : 0;
    ds.colclamp = bits(st.colclamp, 0, 1);
    ds.tbp = bits(cx.tex0, 0, 14);
    ds.tbw = bits(cx.tex0, 14, 6);
    ds.tpsm = bits(cx.tex0, 20, 6);
    ds.tw = bits(cx.tex0, 26, 4);
    ds.th = bits(cx.tex0, 30, 4);
    ds.tcc = bits(cx.tex0, 34, 1);
    ds.tfx = bits(cx.tex0, 35, 2);
    ds.cpsm = bits(cx.tex0, 51, 4);
    ds.csa = bits(cx.tex0, 56, 5);
    if (ds.tw > 10) ds.tw = 10;
    if (ds.th > 10) ds.th = 10;
    ds.wms = bits(cx.clamp, 0, 2);
    ds.wmt = bits(cx.clamp, 2, 2);
    ds.minu = bits(cx.clamp, 4, 10);
    ds.maxu = bits(cx.clamp, 14, 10);
    ds.minv = bits(cx.clamp, 24, 10);
    ds.maxv = bits(cx.clamp, 34, 10);
    ds.lcm = bits(cx.tex1, 0, 1);
    ds.mxl = bits(cx.tex1, 2, 3);
    ds.mag_linear = bits(cx.tex1, 5, 1);
    u32 mmin = bits(cx.tex1, 6, 3);
    ds.min_linear = (mmin == 1 || mmin == 4 || mmin == 5);
    ds.mmin = mmin;
    ds.mtba = bits(cx.tex1, 9, 1);
    ds.miptbp1 = cx.miptbp1;
    ds.miptbp2 = cx.miptbp2;
    ds.lodl = bits(cx.tex1, 19, 2);
    s32 k12 = (s32)(bits(cx.tex1, 32, 12) << 20) >> 20;
    ds.lodk = k12 / 16.0f;
    ds.ta0 = bits(st.texa, 0, 8);
    ds.aem = bits(st.texa, 15, 1);
    ds.ta1 = bits(st.texa, 32, 8);
    ds.fogcol = (u32)st.fogcol & 0xFFFFFF;
    ds.scax0 = (int)bits(cx.scissor, 0, 11);
    ds.scax1 = (int)bits(cx.scissor, 16, 11);
    ds.scay0 = (int)bits(cx.scissor, 32, 11);
    ds.scay1 = (int)bits(cx.scissor, 48, 11);
    ds.dthe = bits(st.dthe, 0, 1);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int v = (int)bits(st.dimx, y * 16 + x * 4, 3);
            if (v & 4) v -= 8;
            ds.dimx[y][x] = v;
        }
    ds.fb24 = (ds.fpsm == PSMCT24 || ds.fpsm == PSMZ24);
    ds.fb16 = (ds.fpsm == PSMCT16 || ds.fpsm == PSMCT16S || ds.fpsm == PSMZ16 || ds.fpsm == PSMZ16S);
    ds.clut = nullptr;
    ds.tmem = vram;
    return true;
}

// ---------------------------------------------------------------------------
// Texture sampling
// ---------------------------------------------------------------------------
static inline u32 expand_ct16(const DrawState& ds, u32 c) {
    u32 r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
    u32 a;
    if (c & 0x8000) a = ds.ta1;
    else a = (ds.aem && (c & 0x7FFF) == 0) ? 0 : ds.ta0;
    return r | (g << 8) | (b << 16) | (a << 24);
}

static inline u32 clut_lookup(const DrawState& ds, u32 idx, bool is4) {
    bool ct16 = (ds.cpsm == PSMCT16 || ds.cpsm == PSMCT16S);
    if (ct16) {
        u32 e = ds.clut[(idx + ds.csa * 16) & 511] & 0xFFFF;
        return expand_ct16(ds, e);
    }
    if (is4) return ds.clut[(idx + (ds.csa & 15) * 16) & 255];
    return ds.clut[idx & 255];
}

static inline u32 fetch_texel(const DrawState& ds, int u, int v, u32 tbp) {
    const u32* vram = ds.tmem;
    auto vram16 = [vram]() { return (const u16*)vram; };
    auto vram8 = [vram]() { return (const u8*)vram; };
    switch (ds.tpsm) {
    case PSMCT32: return vram[addr32(u, v, tbp, ds.tbw)];
    case PSMCT24: {
        u32 c = vram[addr32(u, v, tbp, ds.tbw)] & 0xFFFFFF;
        u32 a = (ds.aem && c == 0) ? 0 : ds.ta0;
        return c | (a << 24);
    }
    case PSMCT16: return expand_ct16(ds, vram16()[addr16(u, v, tbp, ds.tbw)]);
    case PSMCT16S: return expand_ct16(ds, vram16()[addr16s(u, v, tbp, ds.tbw)]);
    case PSMT8: return clut_lookup(ds, vram8()[addr8(u, v, tbp, ds.tbw)], false);
    case PSMT4: {
        u32 a = addr4(u, v, tbp, ds.tbw);
        return clut_lookup(ds, (vram8()[a >> 1] >> ((a & 1) * 4)) & 0xF, true);
    }
    case PSMT8H: return clut_lookup(ds, vram[addr32(u, v, tbp, ds.tbw)] >> 24, false);
    case PSMT4HL: return clut_lookup(ds, (vram[addr32(u, v, tbp, ds.tbw)] >> 24) & 0xF, true);
    case PSMT4HH: return clut_lookup(ds, vram[addr32(u, v, tbp, ds.tbw)] >> 28, true);
    case PSMZ32: return vram[addr32z(u, v, tbp, ds.tbw)];
    case PSMZ24: return (vram[addr32z(u, v, tbp, ds.tbw)] & 0xFFFFFF) | (ds.ta0 << 24);
    case PSMZ16: return expand_ct16(ds, vram16()[addr16z(u, v, tbp, ds.tbw)]);
    case PSMZ16S: return expand_ct16(ds, vram16()[addr16sz(u, v, tbp, ds.tbw)]);
    }
    return 0;
}

static inline int wrap(int c, u32 mode, u32 size, u32 mn, u32 mx) {
    switch (mode) {
    case 0: return c & (int)(size - 1);
    case 1: return c < 0 ? 0 : (c >= (int)size ? (int)size - 1 : c);
    case 2: return c < (int)mn ? (int)mn : (c > (int)mx ? (int)mx : c);
    default: return (int)(((u32)c & mn) | mx);
    }
}

static inline u32 sample_nearest(const DrawState& ds, float u, float v, u32 tbp, u32 w, u32 h) {
    int iu = (int)floorf(u), iv = (int)floorf(v);
    iu = wrap(iu, ds.wms, w, ds.minu, ds.maxu);
    iv = wrap(iv, ds.wmt, h, ds.minv, ds.maxv);
    return fetch_texel(ds, iu, iv, tbp);
}

static inline u32 lerp_rgba(u32 a, u32 b, u32 f) {  // f in 0..256
    u32 r = 0;
    for (int k = 0; k < 4; k++) {
        int ca = (a >> (k * 8)) & 0xFF, cb = (b >> (k * 8)) & 0xFF;
        r |= (u32)((ca * (256 - (int)f) + cb * (int)f) >> 8) << (k * 8);
    }
    return r;
}

static inline u32 sample_bilinear(const DrawState& ds, float u, float v, u32 tbp, u32 w, u32 h) {
    float fu = u - 0.5f, fv = v - 0.5f;
    int u0 = (int)floorf(fu), v0 = (int)floorf(fv);
    u32 wu = (u32)((fu - u0) * 256.0f), wv = (u32)((fv - v0) * 256.0f);
    int ua = wrap(u0, ds.wms, w, ds.minu, ds.maxu), ub = wrap(u0 + 1, ds.wms, w, ds.minu, ds.maxu);
    int va = wrap(v0, ds.wmt, h, ds.minv, ds.maxv), vb = wrap(v0 + 1, ds.wmt, h, ds.minv, ds.maxv);
    u32 t00 = fetch_texel(ds, ua, va, tbp), t10 = fetch_texel(ds, ub, va, tbp);
    u32 t01 = fetch_texel(ds, ua, vb, tbp), t11 = fetch_texel(ds, ub, vb, tbp);
    return lerp_rgba(lerp_rgba(t00, t10, wu), lerp_rgba(t01, t11, wu), wv);
}

// texture formats with a dedicated fast path: 1 = PSMCT32, 2 = PSMT8 with a 32-bit CLUT, 0 = generic
template <int TC>
static inline u32 fetch_t(const DrawState& ds, int u, int v, u32 tbp) {
    if (TC == 1) return ds.tmem[addr32(u, v, tbp, ds.tbw)];
    if (TC == 2) return ds.clut[((const u8*)ds.tmem)[addr8(u, v, tbp, ds.tbw)]];
    return fetch_texel(ds, u, v, tbp);
}

// bilinear blend of 4 texels, same integer arithmetic as lerp_rgba (horizontal, then vertical)
static inline u32 bilerp(u32 t00, u32 t10, u32 t01, u32 t11, u32 wu, u32 wv) {
    const __m128i z = _mm_setzero_si128();
    __m128i A = _mm_unpacklo_epi8(_mm_set_epi32(0, 0, (int)t01, (int)t00), z);
    __m128i B = _mm_unpacklo_epi8(_mm_set_epi32(0, 0, (int)t11, (int)t10), z);
    __m128i h = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(A, _mm_set1_epi16((short)(256 - wu))),
                                             _mm_mullo_epi16(B, _mm_set1_epi16((short)wu))), 8);
    __m128i h1 = _mm_srli_si128(h, 8);
    __m128i r = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(h, _mm_set1_epi16((short)(256 - wv))),
                                             _mm_mullo_epi16(h1, _mm_set1_epi16((short)wv))), 8);
    return (u32)_mm_cvtsi128_si32(_mm_packus_epi16(r, r));
}

template <int TC>
static inline u32 sample_t(const DrawState& ds, float u, float v, float lod) {
    const u32 w = 1u << ds.tw, h = 1u << ds.th;
    const bool linear = (lod <= 0.0f) ? ds.mag_linear : ds.min_linear;
    if (!linear) {
        int iu = wrap((int)floorf(u), ds.wms, w, ds.minu, ds.maxu);
        int iv = wrap((int)floorf(v), ds.wmt, h, ds.minv, ds.maxv);
        return fetch_t<TC>(ds, iu, iv, ds.tbp);
    }
    float fu = u - 0.5f, fv = v - 0.5f;
    int u0 = (int)floorf(fu), v0 = (int)floorf(fv);
    u32 wu = (u32)((fu - u0) * 256.0f), wv = (u32)((fv - v0) * 256.0f);
    int ua = wrap(u0, ds.wms, w, ds.minu, ds.maxu), ub = wrap(u0 + 1, ds.wms, w, ds.minu, ds.maxu);
    int va = wrap(v0, ds.wmt, h, ds.minv, ds.maxv), vb = wrap(v0 + 1, ds.wmt, h, ds.minv, ds.maxv);
    u32 t00 = fetch_t<TC>(ds, ua, va, ds.tbp), t10 = fetch_t<TC>(ds, ub, va, ds.tbp);
    u32 t01 = fetch_t<TC>(ds, ua, vb, ds.tbp), t11 = fetch_t<TC>(ds, ub, vb, ds.tbp);
    return bilerp(t00, t10, t01, t11, wu, wv);
}

static inline u32 sample(const DrawState& ds, float u, float v, float lod) {
    u32 w = 1u << ds.tw, h = 1u << ds.th;
    bool linear = (lod <= 0.0f) ? ds.mag_linear : ds.min_linear;
    if (linear) return sample_bilinear(ds, u, v, ds.tbp, w, h);
    return sample_nearest(ds, u, v, ds.tbp, w, h);
}

// ---------------------------------------------------------------------------
// Pixel pipeline
// ---------------------------------------------------------------------------
// index of a frame buffer pixel: word index for 32-bit formats, halfword index for 16-bit ones
static inline u32 fb_index(const DrawState& ds, int x, int y) {
    switch (ds.fpsm) {
    case PSMCT32: case PSMCT24: return addr32(x, y, ds.fbp, ds.fbw);
    case PSMZ32: case PSMZ24: return addr32z(x, y, ds.fbp, ds.fbw);
    case PSMCT16: return addr16(x, y, ds.fbp, ds.fbw);
    case PSMCT16S: return addr16s(x, y, ds.fbp, ds.fbw);
    case PSMZ16: return addr16z(x, y, ds.fbp, ds.fbw);
    case PSMZ16S: return addr16sz(x, y, ds.fbp, ds.fbw);
    }
    return addr32(x, y, ds.fbp, ds.fbw);
}

static inline u32 read_fb(const DrawState& ds, u32 a) {
    if (ds.fb16) {
        u32 c = vram16()[a];
        return ((c & 0x1F) << 3) | (((c >> 5) & 0x1F) << 11) | (((c >> 10) & 0x1F) << 19) | ((c & 0x8000) << 16);
    }
    return vram[a];
}

static inline void write_fb(const DrawState& ds, u32 a, u32 c, u32 mask) {
    // mask: bits to preserve (FBMSK plus format specific)
    if (ds.fb16) {
        u32 c16 = ((c >> 3) & 0x1F) | (((c >> 11) & 0x1F) << 5) | (((c >> 19) & 0x1F) << 10) | ((c >> 16) & 0x8000);
        u32 m16 = ((mask >> 3) & 0x1F) | (((mask >> 11) & 0x1F) << 5) | (((mask >> 19) & 0x1F) << 10) | ((mask >> 16) & 0x8000);
        u16* p = &vram16()[a];
        *p = (u16)((*p & m16) | (c16 & ~m16));
        return;
    }
    if (ds.fb24) mask |= 0xFF000000u;
    u32& d = vram[a];
    d = (d & mask) | (c & ~mask);
}

static inline u32 z_index(const DrawState& ds, int x, int y) {
    switch (ds.zpsm) {
    case PSMZ16: return addr16z(x, y, ds.zbp, ds.fbw);
    case PSMZ16S: return addr16sz(x, y, ds.zbp, ds.fbw);
    }
    return addr32z(x, y, ds.zbp, ds.fbw);
}

static inline u32 read_z(const DrawState& ds, u32 a) {
    switch (ds.zpsm) {
    case PSMZ32: return vram[a];
    case PSMZ24: return vram[a] & 0xFFFFFF;
    case PSMZ16: case PSMZ16S: return vram16()[a];
    }
    return vram[a];
}

static inline void write_z(const DrawState& ds, u32 a, u32 z) {
    switch (ds.zpsm) {
    case PSMZ32: vram[a] = z; break;
    case PSMZ24: { u32& d = vram[a]; d = (d & 0xFF000000u) | (z & 0xFFFFFF); break; }
    case PSMZ16: case PSMZ16S: vram16()[a] = (u16)z; break;
    default: vram[a] = z; break;
    }
}

static inline u32 zmax(const DrawState& ds) {
    switch (ds.zpsm) {
    case PSMZ32: return 0xFFFFFFFFu;
    case PSMZ24: return 0xFFFFFF;
    default: return 0xFFFF;
    }
}

struct Frag {
    int r, g, b, a;
    u32 z;
    float s, t, q;  // or u, v in texels when fst
    int fog;
};

// Pixel pipeline, specialized on the most important state bits so unused stages compile away.
// SCISSOR: callers that already clipped to the scissor rectangle pass false.
template <bool TME, bool ABE, bool ZTE, bool SCISSOR, int TC = 0>
static inline void shade_t(const DrawState& ds, int x, int y, const Frag& f, float lod) {
    if (SCISSOR) {
        if (x < ds.scax0 || x > ds.scax1 || y < ds.scay0 || y > ds.scay1) return;
        if (x < 0 || y < 0 || x > 2047 || y > 2047) return;
    }
    int r = f.r, g = f.g, b = f.b, a = f.a;
    // texture
    if (TME) {
        float u, v;
        if (ds.fst) { u = f.s; v = f.t; }
        else {
            float rq = 1.0f / (f.q == 0.0f ? 1e-10f : f.q);
            u = f.s * rq * (float)(1 << ds.tw);
            v = f.t * rq * (float)(1 << ds.th);
        }
        u32 tc = sample_t<TC>(ds, u, v, lod);
        int tr = tc & 0xFF, tg = (tc >> 8) & 0xFF, tb = (tc >> 16) & 0xFF, ta = tc >> 24;
        switch (ds.tfx) {
        case 0:  // modulate
            r = (tr * r) >> 7; g = (tg * g) >> 7; b = (tb * b) >> 7;
            if (ds.tcc) a = (ta * a) >> 7;
            break;
        case 1:  // decal
            r = tr; g = tg; b = tb;
            if (ds.tcc) a = ta;
            break;
        case 2:  // highlight
            r = ((tr * r) >> 7) + a; g = ((tg * g) >> 7) + a; b = ((tb * b) >> 7) + a;
            if (ds.tcc) a = ta + a;
            break;
        default:  // highlight2
            r = ((tr * r) >> 7) + a; g = ((tg * g) >> 7) + a; b = ((tb * b) >> 7) + a;
            if (ds.tcc) a = ta;
            break;
        }
        r = r > 255 ? 255 : r;
        g = g > 255 ? 255 : g;
        b = b > 255 ? 255 : b;
        a = a > 255 ? 255 : a;
    }
    // fog
    if (ds.fge) {
        int fg = f.fog;
        int fr = ds.fogcol & 0xFF, fgc = (ds.fogcol >> 8) & 0xFF, fb = (ds.fogcol >> 16) & 0xFF;
        r = (r * fg + fr * (255 - fg)) >> 8;
        g = (g * fg + fgc * (255 - fg)) >> 8;
        b = (b * fg + fb * (255 - fg)) >> 8;
    }
    bool write_rgb = true, write_a = true, write_zbuf = !ds.zmsk;
    // alpha test
    if (ds.ate) {
        bool pass;
        switch (ds.atst) {
        case 0: pass = false; break;
        case 1: pass = true; break;
        case 2: pass = (u32)a < ds.aref; break;
        case 3: pass = (u32)a <= ds.aref; break;
        case 4: pass = (u32)a == ds.aref; break;
        case 5: pass = (u32)a >= ds.aref; break;
        case 6: pass = (u32)a > ds.aref; break;
        default: pass = (u32)a != ds.aref; break;
        }
        if (!pass) {
            switch (ds.afail) {
            case 0: return;                                         // KEEP
            case 1: write_zbuf = false; break;                      // FB_ONLY
            case 2: write_rgb = false; write_a = false; break;      // ZB_ONLY
            default: write_a = false; write_zbuf = false; break;    // RGB_ONLY
            }
        }
    }
    u32 fa = fb_index(ds, x, y);
    u32 dst = 0;
    bool need_dst = ds.date || ABE;
    if (need_dst) dst = read_fb(ds, fa);
    // destination alpha test
    if (ds.date && !ds.fb24) {
        bool bit = (dst >> 31) & 1;
        if (bit != ds.datm) return;
    }
    // depth test
    if (ZTE) {
        u32 z = f.z;
        u32 zm = zmax(ds);
        if (z > zm) z = zm;
        u32 za = z_index(ds, x, y);
        if (ds.ztst != 1) {
            u32 dz = read_z(ds, za);
            bool pass;
            switch (ds.ztst) {
            case 0: pass = false; break;
            case 2: pass = z >= dz; break;
            default: pass = z > dz; break;
            }
            if (!pass) return;
        }
        if (write_zbuf) write_z(ds, za, z);
    }
    if (!write_rgb && !write_a) return;
    // alpha blending
    if (ABE && !(ds.pabe && a < 0x80)) {
        int cd[3] = {(int)(dst & 0xFF), (int)((dst >> 8) & 0xFF), (int)((dst >> 16) & 0xFF)};
        int cs[3] = {r, g, b};
        int ad = (int)(dst >> 24);
        if (ds.fb24) ad = 0x80;
        int cc = ds.bc == 0 ? a : ds.bc == 1 ? ad : (int)ds.bfix;
        int out[3];
        for (int k = 0; k < 3; k++) {
            int A = ds.ba == 0 ? cs[k] : ds.ba == 1 ? cd[k] : 0;
            int B = ds.bb == 0 ? cs[k] : ds.bb == 1 ? cd[k] : 0;
            int D = ds.bd == 0 ? cs[k] : ds.bd == 1 ? cd[k] : 0;
            out[k] = (((A - B) * cc) >> 7) + D;
        }
        r = out[0];
        g = out[1];
        b = out[2];
    }
    // dither
    if (ds.dthe && ds.fb16) {
        int d = ds.dimx[y & 3][x & 3];
        r += d; g += d; b += d;
    }
    if (ds.colclamp) {
        r = r < 0 ? 0 : r > 255 ? 255 : r;
        g = g < 0 ? 0 : g > 255 ? 255 : g;
        b = b < 0 ? 0 : b > 255 ? 255 : b;
    } else {
        r &= 0xFF; g &= 0xFF; b &= 0xFF;
    }
    u32 outa = (u32)(a & 0xFF) | ds.fba;
    u32 c = (u32)r | ((u32)g << 8) | ((u32)b << 16) | ((outa & 0xFF) << 24);
    u32 mask = ds.fbmsk;
    if (!write_rgb) mask |= 0x00FFFFFF;
    if (!write_a) mask |= 0xFF000000;
    if (mask == 0xFFFFFFFFu) return;
    write_fb(ds, fa, c, mask);
}

static void shade(const DrawState& ds, int x, int y, const Frag& f, float lod) {
    if (ds.tme) {
        if (ds.abe) { if (ds.zte) shade_t<true, true, true, true>(ds, x, y, f, lod); else shade_t<true, true, false, true>(ds, x, y, f, lod); }
        else { if (ds.zte) shade_t<true, false, true, true>(ds, x, y, f, lod); else shade_t<true, false, false, true>(ds, x, y, f, lod); }
    } else {
        if (ds.abe) { if (ds.zte) shade_t<false, true, true, true>(ds, x, y, f, lod); else shade_t<false, true, false, true>(ds, x, y, f, lod); }
        else { if (ds.zte) shade_t<false, false, true, true>(ds, x, y, f, lod); else shade_t<false, false, false, true>(ds, x, y, f, lod); }
    }
}

// LOD for a fragment. Only the filter choice depends on it (base level is sampled), so callers
// skip it when both filters are equal.
static inline bool lod_needed(const DrawState& ds) { return ds.tme && !ds.fst && !ds.lcm && ds.mag_linear != ds.min_linear; }
static inline float compute_lod(const DrawState& ds, float q) {
    if (ds.lcm) return ds.lodk;
    float aq = fabsf(q);
    if (aq < 1e-20f) aq = 1e-20f;
    return log2f(1.0f / aq) * (float)(1 << ds.lodl) + ds.lodk;
}

// ---------------------------------------------------------------------------
// Primitives (each worker only rasterizes the rows it owns; returns pixels visited)
// ---------------------------------------------------------------------------
template <bool TME, bool ABE, bool ZTE, int TC>
static u64 draw_sprite(const DrawState& ds, const Rows& rows, const Vertex* v) {
    const Vertex& a = v[0];
    const Vertex& b = v[1];
    int x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x);
    int y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    // pixel x covered if x*16 >= x0 && x*16 < x1 (sample at pixel top-left)
    int px0 = (x0 + 15) >> 4, px1 = (x1 + 15) >> 4;
    int py0 = (y0 + 15) >> 4, py1 = (y1 + 15) >> 4;
    px0 = std::max(px0, ds.scax0);
    py0 = std::max(py0, ds.scay0);
    px1 = std::min(px1, ds.scax1 + 1);
    py1 = std::min(py1, ds.scay1 + 1);
    if (px0 >= px1 || py0 >= py1) return 0;
    if (!rows.owns_any(py0, py1)) return 0;
    Frag f;
    f.r = b.r; f.g = b.g; f.b = b.b; f.a = b.a;
    f.z = b.z;
    f.fog = b.f;
    float dx = (float)(b.x - a.x), dy = (float)(b.y - a.y);
    float s0, t0, s1, t1;
    if (ds.fst) {
        s0 = a.u / 16.0f; t0 = a.v / 16.0f;
        s1 = b.u / 16.0f; t1 = b.v / 16.0f;
    } else {
        s0 = a.s; t0 = a.t; s1 = b.s; t1 = b.t;
    }
    float q = b.q;
    float lod = lod_needed(ds) ? compute_lod(ds, q) : ds.lodk;
    float dsdx = dx != 0 ? (s1 - s0) * 16.0f / dx : 0.0f;
    float s_start = dx != 0 ? s0 + (s1 - s0) * ((px0 * 16 - a.x) / dx) : s0;
    f.q = q;
    u64 n = 0;
    for (int y = py0; y < py1; y++) {
        if (!rows.owns(y)) continue;
        float ty = dy != 0 ? ((y * 16 - a.y) / dy) : 0;
        f.t = t0 + (t1 - t0) * ty;
        f.s = s_start;
        for (int x = px0; x < px1; x++) {
            shade_t<TME, ABE, ZTE, false, TC>(ds, x, y, f, lod);
            f.s += dsdx;
        }
        n += px1 - px0;
    }
    return n;
}

template <bool TME, bool ABE, bool ZTE, int TC>
static u64 draw_triangle(const DrawState& ds, const Rows& rows, const Vertex* vin) {
    const Vertex* v0 = &vin[0];
    const Vertex* v1 = &vin[1];
    const Vertex* v2 = &vin[2];
    s64 area = (s64)(v1->x - v0->x) * (v2->y - v0->y) - (s64)(v2->x - v0->x) * (v1->y - v0->y);
    if (area == 0) return 0;
    if (area < 0) {
        std::swap(v1, v2);
        area = -area;
    }
    int minx = std::min({v0->x, v1->x, v2->x}), maxx = std::max({v0->x, v1->x, v2->x});
    int miny = std::min({v0->y, v1->y, v2->y}), maxy = std::max({v0->y, v1->y, v2->y});
    int px0 = std::max((minx + 15) >> 4, ds.scax0), px1 = std::min((maxx + 15) >> 4, ds.scax1 + 1);
    int py0 = std::max((miny + 15) >> 4, ds.scay0), py1 = std::min((maxy + 15) >> 4, ds.scay1 + 1);
    if (px0 >= px1 || py0 >= py1) return 0;
    if (!rows.owns_any(py0, py1)) return 0;  // skip the setup for triangles outside this worker's rows
    // edge setup: E(p) = A*p.x + B*p.y + C, inside (area > 0) when all three are >= 0
    struct Edge { s64 A, B, C; bool tl; };
    auto mk = [](const Vertex* a, const Vertex* b) {
        Edge e;
        e.A = -(s64)(b->y - a->y);
        e.B = (s64)(b->x - a->x);
        e.C = -(e.A * a->x + e.B * a->y);
        // top-left rule (y-down coordinates)
        e.tl = (b->y == a->y && b->x > a->x) || (b->y < a->y);
        return e;
    };
    Edge e0 = mk(v1, v2), e1 = mk(v2, v0), e2 = mk(v0, v1);
    // attribute plane equations a(x, y) = a0 + dadx * (x - x0) + dady * (y - y0), in pixel units
    const Vertex* flat = &vin[2];
    float fa[3][8];
    auto load = [&](const Vertex* v, float* o) {
        o[0] = v->r; o[1] = v->g; o[2] = v->b; o[3] = v->a;
        if (ds.fst) { o[4] = v->u / 16.0f; o[5] = v->v / 16.0f; o[6] = 1.0f; }
        else { o[4] = v->s; o[5] = v->t; o[6] = v->q; }
        o[7] = v->f;
    };
    load(v0, fa[0]);
    load(v1, fa[1]);
    load(v2, fa[2]);
    double X0 = v0->x / 16.0, Y0 = v0->y / 16.0;
    double ex1 = v1->x / 16.0 - X0, ey1 = v1->y / 16.0 - Y0;
    double ex2 = v2->x / 16.0 - X0, ey2 = v2->y / 16.0 - Y0;
    double det = ex1 * ey2 - ex2 * ey1;
    float dadx[8], dady[8];
    for (int i = 0; i < 8; i++) {
        double d1 = fa[1][i] - fa[0][i], d2 = fa[2][i] - fa[0][i];
        dadx[i] = (float)((d1 * ey2 - d2 * ey1) / det);
        dady[i] = (float)((d2 * ex1 - d1 * ex2) / det);
    }
    double zd1 = (double)v1->z - v0->z, zd2 = (double)v2->z - v0->z;
    double dzdx = (zd1 * ey2 - zd2 * ey1) / det, dzdy = (zd2 * ex1 - zd1 * ex2) / det;
    const bool need_lod = lod_needed(ds);
    // only the sign of the LOD matters (mag vs min filter): lod <= 0  <=>  |q| >= 2^(K / 2^L)
    const float qthr = need_lod ? exp2f(ds.lodk / (float)(1 << ds.lodl)) : 0.0f;
    const bool iip = ds.iip;
    const s64 step0 = e0.A * 16, step1 = e1.A * 16, step2 = e2.A * 16;
    u64 n = 0;
    for (int y = py0; y < py1; y++) {
        if (!rows.owns(y)) continue;
        s64 py = (s64)y * 16, px = (s64)px0 * 16;
        s64 w0 = e0.A * px + e0.B * py + e0.C;
        s64 w1 = e1.A * px + e1.B * py + e1.C;
        s64 w2 = e2.A * px + e2.B * py + e2.C;
        double ry = y - Y0, rx = px0 - X0;
        float at[8];
        for (int i = 0; i < 8; i++) at[i] = (float)(fa[0][i] + dadx[i] * rx + dady[i] * ry);
        double z = v0->z + dzdx * rx + dzdy * ry;
        bool entered = false;
        for (int x = px0; x < px1; x++) {
            bool inside = (w0 | w1 | w2) >= 0 && !((w0 == 0 && !e0.tl) || (w1 == 0 && !e1.tl) || (w2 == 0 && !e2.tl));
            if (inside) {
                entered = true;
                Frag f;
                if (iip) {
                    f.r = (int)at[0]; f.g = (int)at[1]; f.b = (int)at[2]; f.a = (int)at[3];
                } else {
                    f.r = flat->r; f.g = flat->g; f.b = flat->b; f.a = flat->a;
                }
                f.z = z >= 4294967295.0 ? 0xFFFFFFFFu : z <= 0.0 ? 0u : (u32)z;
                f.s = at[4];
                f.t = at[5];
                f.q = at[6];
                f.fog = (int)at[7];
                shade_t<TME, ABE, ZTE, false, TC>(ds, x, y, f, need_lod ? (fabsf(f.q) >= qthr ? -1.0f : 1.0f) : ds.lodk);
                n++;
            } else if (entered) {
                break;  // convex: once we leave the triangle on this row we are done
            }
            w0 += step0;
            w1 += step1;
            w2 += step2;
            for (int i = 0; i < 8; i++) at[i] += dadx[i];
            z += dzdx;
        }
    }
    return n;
}

static u64 draw_line(const DrawState& ds, const Rows& rows, const Vertex* v) {
    const Vertex& a = v[0];
    const Vertex& b = v[1];
    float x0 = a.x / 16.0f, y0 = a.y / 16.0f, x1 = b.x / 16.0f, y1 = b.y / 16.0f;
    float dx = x1 - x0, dy = y1 - y0;
    int steps = (int)std::max(fabsf(dx), fabsf(dy));
    if (steps <= 0) steps = 1;
    u64 n = 0;
    for (int i = 0; i < steps; i++) {
        float t = (float)i / steps;
        int py = (int)floorf(y0 + dy * t);
        if (!rows.owns(py)) continue;
        Frag f;
        if (ds.iip) {
            f.r = (int)(a.r + (b.r - a.r) * t);
            f.g = (int)(a.g + (b.g - a.g) * t);
            f.b = (int)(a.b + (b.b - a.b) * t);
            f.a = (int)(a.a + (b.a - a.a) * t);
        } else {
            f.r = b.r; f.g = b.g; f.b = b.b; f.a = b.a;
        }
        f.z = (u32)((double)a.z + ((double)b.z - (double)a.z) * t);
        if (ds.fst) {
            f.s = (a.u + (b.u - a.u) * t) / 16.0f;
            f.t = (a.v + (b.v - a.v) * t) / 16.0f;
            f.q = 1.0f;
        } else {
            f.s = a.s + (b.s - a.s) * t;
            f.t = a.t + (b.t - a.t) * t;
            f.q = a.q + (b.q - a.q) * t;
        }
        f.fog = (int)(a.f + (b.f - a.f) * t);
        shade(ds, (int)floorf(x0 + dx * t), py, f, ds.lodk);
        n++;
    }
    return n;
}

static u64 draw_point(const DrawState& ds, const Rows& rows, const Vertex* v) {
    int py = (v->y + 8) >> 4;
    if (!rows.owns(py)) return 0;
    Frag f;
    f.r = v->r; f.g = v->g; f.b = v->b; f.a = v->a;
    f.z = v->z;
    f.fog = v->f;
    if (ds.fst) { f.s = v->u / 16.0f; f.t = v->v / 16.0f; f.q = 1.0f; }
    else { f.s = v->s; f.t = v->t; f.q = v->q; }
    shade(ds, (v->x + 8) >> 4, py, f, ds.lodk);
    return 1;
}

bool g_gs_null = false;  // --gs-null: skip pixel work (measures the emulation side alone)
static u64 rasterize(const DrawState& ds, const Rows& rows, int prim, const Vertex* v) {
    if (g_gs_null) return 0;
    switch (prim) {
    case 0: return draw_point(ds, rows, v);
    case 1: return draw_line(ds, rows, v);
    case 3:
    case 6: {
        int tc = 0;
        if (ds.tme) {
            if (ds.tpsm == PSMCT32) tc = 1;
            else if (ds.tpsm == PSMT8 && ds.cpsm == PSMCT32) tc = 2;
        }
        int k = (ds.tme ? 4 : 0) | (ds.abe ? 2 : 0) | (ds.zte ? 1 : 0);
        bool tri = prim == 3;
#define GS_DISPATCH(K, T, A, Z, C) \
        case K * 4 + C: return tri ? draw_triangle<T, A, Z, C>(ds, rows, v) : draw_sprite<T, A, Z, C>(ds, rows, v);
        switch (k * 4 + tc) {
        GS_DISPATCH(0, false, false, false, 0)
        GS_DISPATCH(1, false, false, true, 0)
        GS_DISPATCH(2, false, true, false, 0)
        GS_DISPATCH(3, false, true, true, 0)
        GS_DISPATCH(4, true, false, false, 0)
        GS_DISPATCH(4, true, false, false, 1)
        GS_DISPATCH(4, true, false, false, 2)
        GS_DISPATCH(5, true, false, true, 0)
        GS_DISPATCH(5, true, false, true, 1)
        GS_DISPATCH(5, true, false, true, 2)
        GS_DISPATCH(6, true, true, false, 0)
        GS_DISPATCH(6, true, true, false, 1)
        GS_DISPATCH(6, true, true, false, 2)
        GS_DISPATCH(7, true, true, true, 0)
        GS_DISPATCH(7, true, true, true, 1)
        GS_DISPATCH(7, true, true, true, 2)
        }
#undef GS_DISPATCH
        return 0;
    }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Draw state snapshots and the primitive queue
// ---------------------------------------------------------------------------
struct StateSlot {
    DrawState ds;
    u32 id;        // unique per snapshot (slots are reused)
    u64 last_use;  // 1 + sequence number of the last command referencing this slot (0 = unused)
};
struct Cmd {
    u32 state;
    int prim;
    Vertex v[3];
};
struct alignas(64) Worker {
    std::atomic<u64> done{0};
    std::atomic<u64> pixels{0};
    std::atomic<u64> busy_ticks{0};
};

static const u32 STATE_SLOTS = 4096;
static const u32 CMD_RING = 1u << 15;
static StateSlot* g_states = nullptr;
static Cmd* g_cmds = nullptr;
static Worker g_workers[32];
static int g_nworkers = 0;
static std::atomic<u64> g_gpu_done{0};  // queue position of the GPU renderer thread
static bool g_gpu_on = false;
static bool g_gpu_compare = false;       // --gpu-compare: software composition of every frame for comparison
static u64 g_gpu_busy_ticks = 0;
static std::atomic<u64> g_write_seq{0};
static u64 g_wseq = 0;           // producer copy of g_write_seq
static u32 g_state_next = 0;
static int g_state_cur = -1;     // slot holding the snapshot of g_cur (+ CLUT), -1 = none
static u32 g_clut_version = 1;
static u32 g_state_clut_version = 0;
u64 g_wait_ticks_by[8];  // producer wait time by cause: 0 vram access, 4 overflow, 5 state slot, 6 ring full, 7 flush
struct Range { u32 lo, hi; u64 seq; u32 view; };  // view: base pointer, width and format of the buffer
static PresentParams g_present[4];
struct SnapCmd { u32 lo, hi; u64 done; };
static SnapCmd g_snapq[64];
static u32 g_snapq_next = 0;
static u32* g_snap = nullptr;  // VRAM snapshot sampled by primitives that texture from their own target
static u32 g_wclut[33][512];        // CLUT per worker (index 0 also serves the synchronous path)
struct ClutCmd { u64 tex0, texclut; };
static ClutCmd g_clutq[1024];
static u64 g_clutq_done[1024];
static u32 g_clutq_next = 0;
struct UploadRec { UploadInfo info; std::vector<u8> data; u64 done; };
static UploadRec g_upq[256];
static u32 g_upq_next = 0;

static u32 g_present_next = 0;
static u32* g_present_out = new u32[1024 * 2048];
static u64 g_barrier_seq = 0;    // commands with seq <= this are complete before any later command runs
static u64 g_sync_pixels = 0;    // pixels rasterized on the emulation thread (synchronous mode)
// flush statistics (reason index: 0 external, 1 texture dependency, 2 write-after-read, 3 self feedback, 4 range overflow)
u64 g_flush_count[5];
static u64 g_flush_wait_ticks = 0;

// "flushes t/w/s/o, producer wait, worker busy" summary since the last call (for the fps log line)
std::string raster_stats() {
    static u64 last_count[5], last_wait, last_busy, last_tsc;
    u64 busy = 0;
    for (int i = 0; i < g_nworkers; i++) busy += g_workers[i].busy_ticks.load(std::memory_order_relaxed);
    u64 now = __rdtsc();
    double span = (double)(now - last_tsc);
    char b[600];
    static u64 last_idle, last_spu;
    u64 idle = kernel::g_ee_idle_ticks;
    u64 spu = spu2::g_spu_ticks;
    snprintf(b, sizeof(b), "flush tex=%llu war=%llu self=%llu ovf=%llu ext=%llu, ee idle %.0f%%, spu %.1f%%, producer wait %.0f%%, workers busy %.0f%%",
             (unsigned long long)(g_flush_count[1] - last_count[1]), (unsigned long long)(g_flush_count[2] - last_count[2]),
             (unsigned long long)(g_flush_count[3] - last_count[3]), (unsigned long long)(g_flush_count[4] - last_count[4]),
             (unsigned long long)(g_flush_count[0] - last_count[0]), 100.0 * (idle - last_idle) / span, 100.0 * (spu - last_spu) / span, 100.0 * (g_flush_wait_ticks - last_wait) / span,
             g_nworkers ? 100.0 * (busy - last_busy) / span / g_nworkers : 0.0);
    {
        static u64 last_wb[32];
        std::string per = " [";
        for (int i = 0; i < g_nworkers; i++) {
            u64 wb = g_workers[i].busy_ticks.load(std::memory_order_relaxed);
            char t[16];
            snprintf(t, sizeof(t), "%s%.0f", i ? " " : "", 100.0 * (wb - last_wb[i]) / span);
            per += t;
            last_wb[i] = wb;
        }
        per += "]";
        if (g_gpu_on) {
            static u64 last_gb;
            char t[48];
            snprintf(t, sizeof(t), ", gpu thread busy %.0f%%", 100.0 * (g_gpu_busy_ticks - last_gb) / span);
            per += t;
            last_gb = g_gpu_busy_ticks;
        }
        strncat(b, per.c_str(), sizeof(b) - strlen(b) - 1);
    }
    {
        static u64 last_by[8];
        char t[160];
        snprintf(t, sizeof(t), " waits: vram %.0f%% slot %.0f%% ring %.0f%% flush %.0f%%", 100.0 * (g_wait_ticks_by[0] - last_by[0]) / span,
                 100.0 * (g_wait_ticks_by[5] - last_by[5]) / span, 100.0 * (g_wait_ticks_by[6] - last_by[6]) / span,
                 100.0 * (g_wait_ticks_by[7] - last_by[7]) / span);
        strncat(b, t, sizeof(b) - strlen(b) - 1);
        memcpy(last_by, g_wait_ticks_by, sizeof(last_by));
    }
    memcpy(last_count, g_flush_count, sizeof(last_count));
    last_wait = g_flush_wait_ticks;
    last_idle = idle;
    last_spu = spu;
    last_busy = busy;
    last_tsc = now;
    return b;
}

u64 pixels_shaded() {
    u64 n = g_sync_pixels;
    for (int i = 0; i < g_nworkers; i++) n += g_workers[i].pixels.load(std::memory_order_relaxed);
    return n;
}


static u64 min_done() {
    u64 m = g_wseq;
    for (int i = 0; i < g_nworkers; i++) m = std::min(m, g_workers[i].done.load(std::memory_order_acquire));
    if (g_gpu_on) m = std::min(m, g_gpu_done.load(std::memory_order_acquire));
    return m;
}
// commands go through the queue (software workers and/or the GPU renderer consume them)
static inline bool queued() { return g_nworkers > 0 || g_gpu_on; }
static inline bool gpu_only() { return g_gpu_on && g_nworkers == 0; }
static u64 sw_min_done() {
    u64 m = ~0ull;
    for (int i = 0; i < g_nworkers; i++) m = std::min(m, g_workers[i].done.load(std::memory_order_acquire));
    return m;
}

static void wait_done(u64 target) {
    if (g_write_seq.load(std::memory_order_relaxed) < target) {
        g_write_seq.store(g_wseq, std::memory_order_release);
        g_write_seq.notify_all();
    }
    for (int spins = 0; min_done() < target; spins++) {
        if (spins < 2000) _mm_pause();
        else std::this_thread::yield();
    }
}

void* g_worker_thread_handle[32];  // for the native profiler (--perf=raster)
static void worker_main(int k) {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), (HANDLE*)&g_worker_thread_handle[k], 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    Worker& w = g_workers[k];
    Rows rows{k, g_nworkers};
    u64 my = 0, pixels = 0;
    DrawState local{};
    u32 local_id = ~0u;
    for (;;) {
        u64 avail = g_write_seq.load(std::memory_order_acquire);
        if (my == avail) {
            int spins = 0;
            while ((avail = g_write_seq.load(std::memory_order_acquire)) == my && spins < 4000) {
                _mm_pause();
                spins++;
            }
            if (avail == my) {
                g_write_seq.wait(my, std::memory_order_acquire);
                continue;
            }
        }
        u64 t0 = __rdtsc();
        while (my < avail) {
            const Cmd& c = g_cmds[my & (CMD_RING - 1)];
            if (c.prim == -4) {
                const ClutCmd& cc = g_clutq[c.state];
                exec_clut_load(g_wclut[k], cc.tex0, cc.texclut);
            } else if (c.prim == -5) {
                const UploadRec& ur = g_upq[c.state];
                exec_upload_rows(ur.info, ur.data.data(), ur.data.size(), rows.k, rows.n);
            } else if (c.prim == -2) {
                if (!g_gpu_on || g_gpu_compare) compose_present_rows(g_present[c.state], g_present_out, rows.k, rows.n);
            } else if (c.prim == -7) {
                // local->local copy: applied to VRAM synchronously by the emulation thread already
            } else if (c.prim == -9) {
                // GPU renderer only (re-present while paused)
            } else if (c.prim == -3) {
                if (k == 0 && !g_gpu_on) {
                    const PresentParams& p = g_present[c.state];
                    platform::submit_frame(g_present_out, p.w, p.out_h, p.w);
                }
            } else if (c.prim == -6) {
                // snapshot of a block range for a primitive sampling its own target (split by block)
                const SnapCmd& sc = g_snapq[c.state];
                u32 len = sc.hi - sc.lo;
                u32 b0 = sc.lo + (u32)((u64)len * rows.k / rows.n), b1 = sc.lo + (u32)((u64)len * (rows.k + 1) / rows.n);
                for (u32 blk = b0; blk < b1; blk++) memcpy(&g_snap[(blk & 16383) * 64], &vram[(blk & 16383) * 64], 256);
            } else if (c.prim < 0) {
                // barrier: every worker finishes all earlier commands before anyone continues
                for (int spins = 0;; spins++) {
                    u64 m = ~0ull;
                    for (int i = 0; i < g_nworkers; i++) m = std::min(m, g_workers[i].done.load(std::memory_order_acquire));
                    if (m >= my) break;
                    if (spins < 20000) _mm_pause();
                    else std::this_thread::yield();
                }
            } else {
                const StateSlot& ss = g_states[c.state];
                if (ss.id != local_id) {  // worker-local copy pointing at this worker's CLUT
                    local = ss.ds;
                    local.clut = g_wclut[k];
                    local_id = ss.id;
                }
                pixels += rasterize(local, rows, c.prim, c.v);
            }
            my++;
            w.done.store(my, std::memory_order_release);
        }
        w.busy_ticks.fetch_add(__rdtsc() - t0, std::memory_order_relaxed);
        w.pixels.store(pixels, std::memory_order_relaxed);
    }
}

static void enqueue_special(int prim, u32 arg);
static CopyInfo g_readq;
static u8* g_state_vram = nullptr;   // savestate transfer (commands -10 save, -11 load)
static u32* g_state_clut = nullptr;
static CopyInfo g_copyq[64];
static u64 g_copyq_done[64];
static u32 g_copyq_next = 0;

// GPU renderer thread: consumes the same queue as the software workers, in order.
static void gpu_main() {
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), (HANDLE*)&g_worker_thread_handle[31], 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    u64 my = 0;
    for (;;) {
        u64 avail = g_write_seq.load(std::memory_order_acquire);
        if (my == avail) {
            int spins = 0;
            while ((avail = g_write_seq.load(std::memory_order_acquire)) == my && spins < 4000) {
                _mm_pause();
                spins++;
            }
            if (avail == my) {
                g_write_seq.wait(my, std::memory_order_acquire);
                continue;
            }
        }
        u64 t0 = __rdtsc();
        while (my < avail) {
            const Cmd& c = g_cmds[my & (CMD_RING - 1)];
            if (c.prim >= 0) {
                const StateSlot& ss = g_states[c.state];
                gpu::on_draw(ss.ds, ss.id, c.prim, c.v);
            } else if (c.prim == -8) {
                gpu::on_readback(g_readq);
            } else if (c.prim == -4) {
                const ClutCmd& cc = g_clutq[c.state];
                gpu::on_clut_load(cc.tex0, cc.texclut);
            } else if (c.prim == -5) {
                const UploadRec& ur = g_upq[c.state];
                gpu::on_upload(ur.info, ur.data.data(), ur.data.size());
            } else if (c.prim == -7) {
                gpu::on_copy(g_copyq[c.state]);
            } else if (c.prim == -9) {
                gpu::on_represent();
            } else if (c.prim == -10 || c.prim == -11) {
                gpu::state_vram(g_state_vram, g_state_clut, c.prim == -11);
            } else if (c.prim == -3) {
                const u32* sw = nullptr;
                if (g_gpu_compare) {
                    // the software composition of this frame is complete when all workers passed it
                    while (sw_min_done() <= my) _mm_pause();
                    sw = g_present_out;
                }
                gpu::on_present(g_present[c.state], sw);
            }
            my++;
            g_gpu_done.store(my, std::memory_order_release);
        }
        g_gpu_busy_ticks += __rdtsc() - t0;
    }
}

static bool g_gpu_req = false, g_gpu_req_compare = false;
static int g_gpu_req_scale = 0;
void raster_request_gpu(bool on, int scale, bool compare) {
    g_gpu_req = on;
    g_gpu_req_scale = scale;
    g_gpu_req_compare = compare;
}
u64 gpu_busy_ticks() { return g_gpu_busy_ticks; }

// Savestates: VRAM (4 MB) and the renderer's CLUT out of / into the renderer, after all queued work
void raster_state_vram(u8* vram, u32* clut512, bool load) {
    if (gpu_only()) {
        g_state_vram = vram;
        g_state_clut = clut512;
        enqueue_special(load ? -11 : -10, 0);
        g_write_seq.store(g_wseq, std::memory_order_release);
        g_write_seq.notify_all();
        wait_done(g_wseq);
    } else {
        raster_flush();
        if (!load) {
            memcpy(vram, gs::vram, 4u << 20);
            memcpy(clut512, g_wclut[0], 2048);
        } else {
            memcpy(gs::vram, vram, 4u << 20);
            for (auto& c : g_wclut) memcpy(c, clut512, 2048);
        }
    }
    if (load) {
        g_state_cur = -1;
        g_clut_version++;
        raster_flush();  // forgets the pending ranges
    }
}

void raster_represent() {
    if (!g_gpu_on) return;
    enqueue_special(-9, 0);
    g_write_seq.store(g_wseq, std::memory_order_release);
    g_write_seq.notify_all();
}

// local -> host transfer in GPU-only mode: the renderer brings its memory copy up to date for the
// area, the emulation thread then reads it
void raster_gpu_readback(const CopyInfo& r) {
    g_readq = r;
    enqueue_special(-8, 0);
    g_write_seq.store(g_wseq, std::memory_order_release);
    g_write_seq.notify_all();
    wait_done(g_wseq);
}

void raster_copy(const CopyInfo& ci) {
    if (!g_gpu_on) return;
    u32 slot = g_copyq_next++ & 63;
    if (g_copyq_done[slot] && min_done() < g_copyq_done[slot]) wait_done(g_copyq_done[slot]);
    g_copyq[slot] = ci;
    enqueue_special(-7, slot);
    g_copyq_done[slot] = g_wseq;
    g_write_seq.store(g_wseq, std::memory_order_release);
    g_write_seq.notify_all();
}

void raster_init(int threads) {
    g_states = new StateSlot[STATE_SLOTS]();
    g_cmds = new Cmd[CMD_RING]();
    g_snap = new u32[1024 * 1024]();
    if (threads < 0) {
        unsigned hc = std::thread::hardware_concurrency();
        threads = hc > 3 ? (int)hc - 2 : 0;  // keep the emulation thread and the OS some room
        if (threads > 16) threads = 16;
    }
    if (threads > 32) threads = 32;
    if (g_gpu_req && gpu::init(g_gpu_req_scale)) {
        g_gpu_on = true;
        g_gpu_compare = g_gpu_req_compare;
        platform::set_gpu_present(true);
        std::thread(gpu_main).detach();
        if (!g_gpu_compare) threads = 0;  // the software rasterizer only runs for comparisons
    }
    g_nworkers = threads <= 1 ? 0 : threads;
    for (int i = 0; i < g_nworkers; i++) std::thread(worker_main, i).detach();
    log_write(LOG_GS, "rasterizer: %d worker threads", g_nworkers);
}

// ---------------------------------------------------------------------------
// VRAM dependency tracking (block = 256 bytes, 16384 blocks)
// ---------------------------------------------------------------------------
// Ranges carry the sequence number (g_wseq after enqueue) of the last queued command touching them,
// so a dependent operation only waits for exactly those commands instead of draining the queue.
static std::vector<Range> g_written, g_read;
static std::vector<Range> g_uploaded;  // pending uploads (draws writing the same memory must wait)

static Range block_range(u32 bp, u32 bw, u32 psm, int y0, int y1) {
    int ph = 32, pw = 64;
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: ph = 64; break;
    case PSMT8: ph = 64; pw = 128; break;
    case PSMT4: ph = 128; pw = 128; break;
    }
    u32 ppr = std::max<u32>(1, (bw * 64 + pw - 1) / pw);  // pages per row
    if (y0 < 0) y0 = 0;
    if (y1 <= y0) y1 = y0 + 1;
    u32 lo = bp + (u32)(y0 / ph) * ppr * 32;
    u32 hi = bp + (u32)((y1 + ph - 1) / ph) * ppr * 32;
    return {lo, std::min<u32>(hi, 16384 + 4096), 0, bp | (bw << 14) | (psm << 20)};
}

// highest sequence number of queued commands touching [lo, hi) (0 = none)
static u64 pending_on(const std::vector<Range>& v, u32 lo, u32 hi) {
    u64 m = 0;
    for (const Range& x : v)
        if (lo < x.hi && x.lo < hi && x.seq > m) m = x.seq;
    return m;
}

static void wait_for(u64 seq, int reason) {
    if (!g_nworkers || seq == 0 || min_done() >= seq) return;
    g_flush_count[reason]++;
    u64 t0 = __rdtsc();
    wait_done(seq);
    u64 dt = __rdtsc() - t0;
    g_flush_wait_ticks += dt;
    g_wait_ticks_by[reason & 7] += dt;
}

static void prune(std::vector<Range>& v) {
    u64 done = min_done();
    v.erase(std::remove_if(v.begin(), v.end(), [done](const Range& x) { return x.seq <= done; }), v.end());
}

static void add_range(std::vector<Range>& v, Range r) {
    // only ranges of the same buffer are merged: merging different buffers would create
    // false overlaps with unrelated textures
    for (Range& x : v)
        if (x.view == r.view) {
            x.lo = std::min(x.lo, r.lo);
            x.hi = std::max(x.hi, r.hi);
            x.seq = std::max(x.seq, r.seq);
            return;
        }
    v.push_back(r);
    if (v.size() > 48) {
        prune(v);
        if (v.size() > 48) {
            g_flush_count[4]++;
            // commands may be enqueued but not yet published by the caller: publish before waiting
            g_write_seq.store(g_wseq, std::memory_order_release);
            g_write_seq.notify_all();
            wait_done(g_wseq);
            v.clear();
        }
    }
}

void raster_flush() {
    if (g_nworkers && min_done() < g_wseq) {
        u64 t0 = __rdtsc();
        wait_done(g_wseq);
        g_flush_wait_ticks += __rdtsc() - t0;
        g_wait_ticks_by[7] += __rdtsc() - t0;
    }
    g_written.clear();
    g_read.clear();
    g_uploaded.clear();
}

// Wait until queued primitives no longer conflict with a direct VRAM access of rows [y0, y1)
// of a buffer (reads conflict with queued writes, writes also with queued texture reads).
static void log_long_wait(const char* what, u32 lo, u32 hi, u64 t0, u64 need) {
    static int n = 0;
    u64 dt = __rdtsc() - t0;
    if (dt > 3000000 && kernel::emu_time() > 150 && n++ < 80)
        log_write(LOG_GS, "long vram wait %.2f Mcycles: %s blocks %x-%x (need seq %llu, queue %llu)", dt / 1e6, what, lo, hi,
                  (unsigned long long)need, (unsigned long long)g_wseq);
}

void raster_sync_rows(u32 bp, u32 bw, u32 psm, int y0, int y1, bool write) {
    if (!g_nworkers) return;
    Range r = block_range(bp, bw, psm, y0, y1);
    u64 need = pending_on(g_written, r.lo, r.hi);
    u64 need_r = write ? pending_on(g_read, r.lo, r.hi) : 0;
    u64 t0 = __rdtsc();
    wait_for(std::max(need, need_r), 0);
    log_long_wait(write ? (need_r > need ? "transfer write (texture still read)" : "transfer write (area being drawn)") : "transfer read",
                  r.lo, r.hi, t0, std::max(need, need_r));
}

void raster_before_vram_read(u32 blo, u32 bhi) {
    u64 need = pending_on(g_written, blo, bhi);
    u64 t0 = __rdtsc();
    wait_for(need, 0);
    log_long_wait("clut load", blo, bhi, t0, need);
}

void raster_clut_changed() { g_clut_version++; }

static void enqueue_special(int prim, u32 arg) {
    if (g_wseq - min_done() >= CMD_RING) {
        u64 t0 = __rdtsc();
        for (g_write_seq.store(g_wseq, std::memory_order_release), g_write_seq.notify_all(); g_wseq - min_done() >= CMD_RING;) _mm_pause();
        g_wait_ticks_by[6] += __rdtsc() - t0;
        g_flush_wait_ticks += __rdtsc() - t0;
    }
    Cmd& c = g_cmds[g_wseq & (CMD_RING - 1)];
    c.prim = prim;
    c.state = arg;
    if (prim == -1) g_barrier_seq = g_wseq;
    g_wseq++;
}

bool raster_async() { return queued(); }
bool raster_sw_active() { return !gpu_only(); }

// watchdog helper: queue position of the producer and every worker
std::string raster_debug_state() {
    std::string r = "wseq " + std::to_string(g_wseq) + " published " + std::to_string(g_write_seq.load()) + " done";
    for (int i = 0; i < g_nworkers; i++) r += " " + std::to_string(g_workers[i].done.load());
    if (g_nworkers) {
        u64 m = min_done();
        r += " | next cmd prim " + std::to_string(g_cmds[m & (CMD_RING - 1)].prim);
    }
    return r;
}

void raster_clut_load(u64 tex0, u64 texclut) {
    if (!queued()) {
        exec_clut_load(g_wclut[0], tex0, texclut);
        return;
    }
    if (gpu_only()) {
        u32 slot = g_clutq_next++ & 1023;
        if (g_clutq_done[slot] && min_done() < g_clutq_done[slot]) wait_done(g_clutq_done[slot]);
        g_clutq[slot] = ClutCmd{tex0, texclut};
        enqueue_special(-4, slot);
        g_clutq_done[slot] = g_wseq;
        g_write_seq.store(g_wseq, std::memory_order_release);
        g_write_seq.notify_all();
        return;
    }
    // source area of the CLUT
    u32 cbp = bits(tex0, 37, 14), cpsm = bits(tex0, 51, 4), csm = bits(tex0, 55, 1);
    Range r = csm == 0 ? block_range(cbp, 1, cpsm, 0, 16)
                       : block_range(cbp, bits(texclut, 0, 6), cpsm, (int)bits(texclut, 12, 10), (int)bits(texclut, 12, 10) + 1);
    if (std::max(pending_on(g_written, r.lo, r.hi), pending_on(g_uploaded, r.lo, r.hi)) > g_barrier_seq) {
        g_flush_count[1]++;
        enqueue_special(-1, 0);
    }
    u32 slot = g_clutq_next++ & 1023;
    if (g_clutq_done[slot] && min_done() < g_clutq_done[slot]) wait_done(g_clutq_done[slot]);
    g_clutq[slot] = ClutCmd{tex0, texclut};
    enqueue_special(-4, slot);
    g_clutq_done[slot] = g_wseq;
    r.seq = g_wseq;
    add_range(g_read, r);
    g_write_seq.store(g_wseq, std::memory_order_release);
    g_write_seq.notify_all();
}

void raster_upload(const UploadInfo& u, const u8* data, size_t len) {
    // the GPU renderer runs everything in queue order: dependencies only matter for the software
    // workers (tracking them in GPU-only mode filled the range lists and stalled on full flushes)
    bool track = !gpu_only();
    Range r = block_range(u.dbp, u.dbw, u.dpsm, (int)u.dy, (int)(u.dy + u.h));
    if (track) {
        // queued reads (textures, CLUT loads) and writes of this memory must complete first
        u64 need = std::max(pending_on(g_read, r.lo, r.hi), pending_on(g_written, r.lo, r.hi));
        need = std::max(need, pending_on(g_uploaded, r.lo, r.hi));
        if (need > g_barrier_seq) {
            g_flush_count[2]++;
            enqueue_special(-1, 0);
        }
    }
    u32 slot = g_upq_next++ & 255;
    UploadRec& ur = g_upq[slot];
    if (ur.done && min_done() < ur.done) {
        u64 t0 = __rdtsc();
        wait_done(ur.done);
        g_wait_ticks_by[6] += __rdtsc() - t0;
        g_flush_wait_ticks += __rdtsc() - t0;
    }
    ur.info = u;
    ur.data.assign(data, data + len);
    enqueue_special(-5, slot);
    ur.done = g_wseq;
    r.seq = g_wseq;
    if (track) {
        add_range(g_written, r);
        add_range(g_uploaded, r);
    }
    g_write_seq.store(g_wseq, std::memory_order_release);
    g_write_seq.notify_all();
}

void raster_present(const PresentParams& p) {
    if (!queued()) {
        compose_present_rows(p, g_present_out, 0, 1);
        platform::submit_frame(g_present_out, p.w, p.out_h, p.w);
        return;
    }
    // barrier, compose (each worker its rows), barrier, submit (worker 0). The slot ring is 4 deep and
    // the leading barrier of the 4th next present orders slot reuse after this one's submit.
    u32 slot = g_present_next++ & 3;
    static u64 slot_done[4];
    if (slot_done[slot] && min_done() < slot_done[slot]) wait_done(slot_done[slot]);  // practically never
    g_present[slot] = p;
    enqueue_special(-1, 0);
    enqueue_special(-2, slot);
    enqueue_special(-1, 0);
    enqueue_special(-3, slot);
    slot_done[slot] = g_wseq;
    g_write_seq.store(g_wseq, std::memory_order_release);
    g_write_seq.notify_all();
}

// ---------------------------------------------------------------------------
// Statistics (--draw-stats=SEC): per-state pixel totals of one frame
// ---------------------------------------------------------------------------
double g_draw_stats_at = -1;
static bool g_draw_stats_active = false;
struct DrawStat { u64 pixels, prims; };
static std::map<std::string, DrawStat> g_draw_stats;
void draw_stats_frame_end() {
    if (g_draw_stats_active) {
        std::vector<std::pair<u64, std::string>> v;
        u64 total = 0;
        for (auto& kv : g_draw_stats) { v.push_back({kv.second.pixels, kv.first + " prims=" + std::to_string(kv.second.prims)}); total += kv.second.pixels; }
        std::sort(v.rbegin(), v.rend());
        log_write(LOG_GS, "draw stats: %llu pixels in frame", (unsigned long long)total);
        for (size_t i = 0; i < v.size() && i < 25; i++) log_write(LOG_GS, "  %8llu  %s", (unsigned long long)v[i].first, v[i].second.c_str());
        g_draw_stats.clear();
        g_draw_stats_active = false;
        g_draw_stats_at = -1;
    } else if (g_draw_stats_at >= 0 && kernel::emu_time() >= g_draw_stats_at) {
        g_draw_stats_active = true;
    }
}

u64 g_prims_drawn = 0;
bool g_log_draws = false;
double g_log_draws_from = 0;

// ---------------------------------------------------------------------------
// Entry point (emulation thread)
// ---------------------------------------------------------------------------
static void alloc_state(const DrawState& d) {
    u32 slot = g_state_next++ % STATE_SLOTS;
    StateSlot& s = g_states[slot];
    if (g_nworkers && s.last_use && min_done() < s.last_use) {
        u64 t0 = __rdtsc();
        wait_done(s.last_use);
        g_wait_ticks_by[5] += __rdtsc() - t0;
        g_flush_wait_ticks += __rdtsc() - t0;
    }
    s.ds = d;
    s.ds.clut = g_wclut[0];
    s.id = g_state_next;
    g_state_cur = (int)slot;
}

static void enqueue_draw(int prim, const Vertex* v, int nv) {
    if (g_wseq - min_done() >= CMD_RING) {
        u64 t0 = __rdtsc();
        for (g_write_seq.store(g_wseq, std::memory_order_release), g_write_seq.notify_all(); g_wseq - min_done() >= CMD_RING;) _mm_pause();
        g_wait_ticks_by[6] += __rdtsc() - t0;
        g_flush_wait_ticks += __rdtsc() - t0;
    }
    Cmd& c = g_cmds[g_wseq & (CMD_RING - 1)];
    c.state = (u32)g_state_cur;
    c.prim = prim;
    memcpy(c.v, v, sizeof(Vertex) * nv);
    g_wseq++;
}

void draw_primitive(int prim, const Vertex* v) {
    bool changed = setup();
    const DrawState& ds = g_cur;
    g_prims_drawn++;
    if (census_active()) census_draw(ds, prim, v);
    static int logged = 0;
    bool boring = (prim == 6 && !ds.tme && v[1].r == 0 && v[1].g == 0 && v[1].b == 0);
    // GRANDIA_LOG_FB_READS: only draws sampling frame/depth buffers (post effects), host time window
    static int fb_reads = getenv("GRANDIA_LOG_FB_READS") ? 1 : 0;
    bool fb_read = fb_reads && ds.tme && (ds.tbp == 0 || ds.tbp == 0xe00 || ds.tbp == 0x1c00 || ds.tbp == 0x2a00) &&
                   host_time_sec() >= g_log_draws_from && host_time_sec() < g_log_draws_from + 0.2;
    if ((fb_read || (g_log_draws && !fb_reads)) && !boring && (fb_reads || kernel::emu_time() >= g_log_draws_from) && logged++ < 30000) {
        int n = prim == 3 ? 3 : prim == 0 ? 1 : 2;
        log_write(LOG_GS, "draw#%llu prim=%d fb=%x/%u/%02x msk=%08x z=%x/%02x zte=%d/%d tme=%d tex=%x/%u/%02x %ux%u tfx=%d fst=%d abe=%d(%d%d%d%d fix=%x) ate=%d/%d/%x sc=%d,%d-%d,%d filt=%d/%d lcm=%d K=%.2f L=%d wm=%d/%d",
                  (unsigned long long)g_prims_drawn, prim, ds.fbp, ds.fbw, ds.fpsm, ds.fbmsk, ds.zbp, ds.zpsm, ds.zte, ds.ztst, ds.tme,
                  ds.tbp, ds.tbw, ds.tpsm, 1u << ds.tw, 1u << ds.th, ds.tfx, ds.fst, ds.abe, ds.ba, ds.bb, ds.bc, ds.bd, ds.bfix,
                  ds.ate, ds.atst, ds.aref, ds.scax0, ds.scay0, ds.scax1, ds.scay1, ds.mag_linear, ds.min_linear, ds.lcm, ds.lodk,
                  ds.lodl, ds.wms, ds.wmt);
        for (int i = 0; i < n; i++)
            log_write(LOG_GS, "   v%d: x=%.2f y=%.2f z=%u rgba=%02x%02x%02x%02x uv=%.2f,%.2f stq=%f,%f,%f", i, v[i].x / 16.0f,
                      v[i].y / 16.0f, v[i].z, v[i].r, v[i].g, v[i].b, v[i].a, v[i].u / 16.0f, v[i].v / 16.0f, v[i].s, v[i].t, v[i].q);
    }
    int nv = prim == 3 ? 3 : prim == 0 ? 1 : 2;

    // snapshot the draw state when it changed (CLUTs live per worker, loaded in queue order)
    if (changed && g_state_cur >= 0 && same_draw_state(g_states[g_state_cur].ds, ds)) changed = false;
    if (changed || g_state_cur < 0) alloc_state(ds);
    StateSlot& slot = g_states[g_state_cur];

    // rows touched by the primitive (for dependency tracking)
    int miny = v[0].y, maxy = v[0].y;
    for (int i = 1; i < nv; i++) { miny = std::min(miny, v[i].y); maxy = std::max(maxy, v[i].y); }
    // exact pixel rows the rasterizer can touch (lines/points round to nearest, so allow one more)
    int ry0 = std::max(miny >> 4, ds.scay0), ry1 = std::min(((maxy + 15) >> 4) + (prim < 3 ? 1 : 0), ds.scay1 + 1);
    if (ry1 <= ry0) return;  // fully outside the scissor rows

    if (gpu_only()) {
        // no software rasterizer: nothing to track, the GPU renderer orders everything itself
        enqueue_draw(prim, v, nv);
        slot.last_use = g_wseq;
        g_write_seq.store(g_wseq, std::memory_order_release);
        g_write_seq.notify_all();
        return;
    }
    bool sync = g_nworkers == 0 || g_draw_stats_active;
    if (!sync) {
        Range fbr = block_range(ds.fbp, ds.fbw, ds.fpsm, ry0, ry1);
        bool zwrite = ds.zte && !ds.zmsk && ds.ztst != 0;
        Range zr = block_range(ds.zbp, ds.fbw, ds.zpsm, ry0, ry1);
        Range tr{0, 0};
        if (ds.tme) {
            // texture rows actually addressed by this primitive (+1 for bilinear), not the whole
            // declared texture: games keep small data (e.g. CLUTs) in unused parts of big textures
            int th = 1 << ds.th;
            int t0 = 0, t1 = th;
            if (ds.wmt != 0) {  // with REPEAT any row may be hit
                float tmin = 1e30f, tmax = -1e30f;
                for (int i = 0; i < nv; i++) {
                    float t = ds.fst ? v[i].v / 16.0f : (v[i].q != 0.0f ? v[i].t / v[i].q * th : 0.0f);
                    tmin = std::min(tmin, t);
                    tmax = std::max(tmax, t);
                }
                if (tmin > -1e6f && tmax < 1e6f) {
                    t0 = std::max(0, (int)floorf(tmin) - 1);
                    t1 = std::min(th, (int)ceilf(tmax) + 2);
                    if (ds.wmt == 2) {  // region clamp
                        t0 = std::max(t0, (int)ds.minv);
                        t1 = std::min(t1, (int)ds.maxv + 1);
                    }
                    if (t1 <= t0) { t0 = 0; t1 = th; }
                }
            }
            tr = block_range(ds.tbp, ds.tbw, ds.tpsm, t0, t1);
        }
        bool self_feedback = ds.tme && (tr.lo < fbr.hi && fbr.lo < tr.hi);
        if (self_feedback) {
            // a primitive sampling its own render target (full-screen filters): after all earlier
            // commands the workers copy the sampled blocks to a snapshot and the primitive samples the
            // snapshot, so every pixel sees the target as it was before the primitive (as with reads
            // ahead of the write position) and the rows can still be drawn in parallel
            g_flush_count[3]++;
            enqueue_special(-1, 0);
            u32 sn = g_snapq_next++ & 63;
            if (g_snapq[sn].done && min_done() < g_snapq[sn].done) wait_done(g_snapq[sn].done);
            g_snapq[sn].lo = tr.lo;
            g_snapq[sn].hi = tr.hi;
            enqueue_special(-6, sn);
            g_snapq[sn].done = g_wseq;
            enqueue_special(-1, 0);
            DrawState sd = slot.ds;
            sd.tmem = g_snap;
            alloc_state(sd);
            StateSlot& ss = g_states[g_state_cur];
            enqueue_draw(prim, v, nv);
            ss.last_use = g_wseq;
            g_state_cur = -1;  // following primitives sample VRAM again
            fbr.seq = zr.seq = g_wseq;
            add_range(g_written, fbr);
            if (zwrite) add_range(g_written, zr);
            g_write_seq.store(g_wseq, std::memory_order_release);
            g_write_seq.notify_all();
            return;
        } else {
            // read-after-write on the texture, write-after-read on frame/z buffer
            u64 raw = ds.tme ? pending_on(g_written, tr.lo, tr.hi) : 0;
            u64 war = pending_on(g_read, fbr.lo, fbr.hi);
            if (zwrite) war = std::max(war, pending_on(g_read, zr.lo, zr.hi));
            war = std::max(war, pending_on(g_uploaded, fbr.lo, fbr.hi));
            if (zwrite) war = std::max(war, pending_on(g_uploaded, zr.lo, zr.hi));
            if (std::max(raw, war) > g_barrier_seq) {
                static int nlog = 0;
                if (kernel::emu_time() > 150 && nlog++ < 300) {
                    // which tracked range caused the barrier (for analysis of false dependencies)
                    auto who = [&](const std::vector<Range>& v, Range q, const char* what) {
                        for (const Range& x : v)
                            if (q.lo < x.hi && x.lo < q.hi && x.seq > g_barrier_seq)
                                log_write(LOG_GS, "barrier %s: prim %d fb %x tex %x/%02x q[%x-%x] vs [%x-%x] view %x", what, prim, ds.fbp, ds.tbp,
                                          ds.tpsm, q.lo, q.hi, x.lo, x.hi, x.view);
                    };
                    if (raw > g_barrier_seq) who(g_written, tr, "RAW");
                    if (war > g_barrier_seq) { who(g_read, fbr, "WAR-fb"); if (zwrite) who(g_read, zr, "WAR-z"); who(g_uploaded, fbr, "WAW-up"); }
                }
                // dependency on queued primitives: let the workers synchronize among themselves
                // instead of stalling the emulation thread
                g_flush_count[raw >= war ? 1 : 2]++;
                for (g_write_seq.store(g_wseq, std::memory_order_release), g_write_seq.notify_all(); g_wseq - min_done() >= CMD_RING;) _mm_pause();
                g_cmds[g_wseq & (CMD_RING - 1)].prim = -1;
                g_barrier_seq = g_wseq;  // commands before the barrier (seq <= this) are complete afterwards
                g_wseq++;
            }
            enqueue_draw(prim, v, nv);
            slot.last_use = g_wseq;
            fbr.seq = zr.seq = tr.seq = g_wseq;
            add_range(g_written, fbr);
            if (zwrite) add_range(g_written, zr);
            if (ds.tme) add_range(g_read, tr);
            g_write_seq.store(g_wseq, std::memory_order_release);
            g_write_seq.notify_all();
            return;
        }
    }
    // synchronous path (single-threaded mode, statistics, self-feedback)
    if (g_nworkers) raster_flush();
    Rows all{0, 1};
    DrawState sds = slot.ds;
    sds.clut = g_wclut[0];
    u64 n = rasterize(sds, all, prim, v);
    g_sync_pixels += n;
    if (g_draw_stats_active) {
        char key[160];
        snprintf(key, sizeof(key), "prim=%d fb=%x/%02x tme=%d tex=%x/%02x %ux%u abe=%d(%d%d%d%d) zte=%d", prim, ds.fbp, ds.fpsm, ds.tme,
                 ds.tbp, ds.tpsm, 1u << ds.tw, 1u << ds.th, ds.abe, ds.ba, ds.bb, ds.bc, ds.bd, ds.zte);
        DrawStat& d = g_draw_stats[key];
        d.pixels += n;
        d.prims++;
    }
}

}  // namespace gs
// --perf=raster: worker 0, or the GPU renderer thread when there are no software workers
void* gs_worker0_handle() { return gs::g_worker_thread_handle[0] ? gs::g_worker_thread_handle[0] : gs::g_worker_thread_handle[31]; }
