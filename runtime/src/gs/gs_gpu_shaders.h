// HLSL sources of the GPU renderer (compiled at startup with D3DCompile).
#pragma once

namespace gs {
namespace gpu {

// Draw pipeline: the GS pixel pipeline of the software rasterizer (gs_draw.cpp, shade_t) on
// rasterizer ordered views holding raw GS pixel values at S x S resolution.
static const char* DRAW_HLSL = R"HLSL(
cbuffer CB : register(b0) {
    float4 vp;          // surface width, height (native pixels), scale S
    uint4 f0;           // fpsm kind (0 C32, 1 C24, 2 C16), fbmsk, fba, colclamp
    uint4 f1;           // dthe, ate, atst, aref
    uint4 f2;           // afail, date, datm, zte
    uint4 f3;           // ztst, zmsk, zfmt (0 Z32, 1 Z24, 2 Z16), abe
    uint4 f4;           // ba, bb, bc, bd
    uint4 f5;           // bfix, pabe, iip, tme
    uint4 f6;           // fst, tfx, tcc, tfmt
    uint4 f7;           // tw, th, wms, wmt
    uint4 f8;           // minu, maxu, minv, maxv
    uint4 f9;           // cpsm16, csa, ta0, ta1
    uint4 f10;          // aem, mag_linear, min_linear, need_lod
    float4 f11;         // lodk, qthr, texture scale (1 or S)
    uint4 f12;          // fge, fogcol
    int4 f13;           // texture origin in the surface (native texels), texture texel limits
    uint4 f14;          // mip levels bound (0 = none), mmin, lcm, L
    float4 f15;         // HD texture: on, scale k (HD texels per texel); sharp text: on, full alpha
    int4 dimx[4];
};
#define FPSM f0.x
#define FBMSK f0.y
#define FBA f0.z
#define COLCLAMP f0.w
#define DTHE f1.x
#define ATE f1.y
#define ATST f1.z
#define AREF f1.w
#define AFAIL f2.x
#define DATE f2.y
#define DATM f2.z
#define ZTE f2.w
#define ZTST f3.x
#define ZMSK f3.y
#define ZFMT f3.z
#define ABE f3.w
#define BA f4.x
#define BB f4.y
#define BC f4.z
#define BD f4.w
#define BFIX f5.x
#define PABE f5.y
#define IIP f5.z
#define TME f5.w
#define FST f6.x
#define TFX f6.y
#define TCC f6.z
#define TFMT f6.w
#define TW f7.x
#define TH f7.y
#define WMS f7.z
#define WMT f7.w
#define MINU f8.x
#define MAXU f8.y
#define MINV f8.z
#define MAXV f8.w
#define CPSM16 f9.x
#define CSA f9.y
#define TA0 f9.z
#define TA1 f9.w
#define AEM f10.x
#define MAGLIN f10.y
#define MINLIN f10.z
#define NEEDLOD f10.w
#define LODK f11.x
#define QTHR f11.y
#define TSCALE f11.z
#define MIPL f14.x
#define MMIN f14.y
#define LCM f14.z
#define LODL f14.w
#define HDTEX f15.x
#define HDK f15.y
#define FONTTEX f15.z
#define FONTAMAX f15.w
#define FGE f12.x
#define FOGCOL f12.y

RasterizerOrderedTexture2D<uint> fb : register(u0);
RasterizerOrderedTexture2D<uint> zb : register(u1);
Texture2D<uint> tex : register(t0);
Buffer<uint> clut : register(t1);
Texture2D<uint> texm1 : register(t2);  // mip levels 1-6
Texture2D<uint> texm2 : register(t3);
Texture2D<uint> texm3 : register(t4);
Texture2D<uint> texm4 : register(t5);
Texture2D<uint> texm5 : register(t6);
Texture2D<uint> texm6 : register(t7);
// HD replacement texture: 8-bit RGBA (alpha 0..128 like GS texels) with a full mip chain, sampled by
// the hardware (anisotropic, level of detail from the screen-space footprint at the internal resolution)
Texture2D<float4> hdtex : register(t8);
SamplerState hds[4] : register(s0);  // u wrap/clamp x v wrap/clamp

struct VSIn {
    float2 pos : POS;
    float z : ZV;
    float4 col : COL;
    float3 stq : STQ;
    float fog : FOG;
    float4 fcol : FCOL;
};
struct PSIn {
    float4 pos : SV_Position;
    noperspective float4 col : COL;
    nointerpolation float4 fcol : FCOL;
    noperspective float3 stq : STQ;
    noperspective float z : ZV;
    noperspective float fog : FOG;
};

PSIn vs_main(VSIn i) {
    PSIn o;
    float S = vp.z;
    // GS sample point x (pixel top-left) lands on the center of the first of its S x S pixels
    float2 d = i.pos * S + 0.5;
    o.pos = float4(d.x / (vp.x * S) * 2.0 - 1.0, 1.0 - d.y / (vp.y * S) * 2.0, 0.5, 1.0);
    o.col = i.col;
    o.fcol = i.fcol;
    o.stq = i.stq;
    o.z = i.z;
    o.fog = i.fog;
    return o;
}

uint4 unpack32(uint c) { return uint4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24); }
uint4 expand16(uint c) {
    uint a = (c & 0x8000) != 0 ? TA1 : ((AEM != 0 && (c & 0x7FFF) == 0) ? 0 : TA0);
    return uint4((c & 0x1F) << 3, ((c >> 5) & 0x1F) << 3, ((c >> 10) & 0x1F) << 3, a);
}
uint4 clut_lookup(uint idx, bool is4) {
    if (CPSM16 != 0) return expand16(clut[(idx + CSA * 16) & 511] & 0xFFFF);
    if (is4) return unpack32(clut[(idx + (CSA & 15) * 16) & 255]);
    return unpack32(clut[idx & 255]);
}
uint4 texel(uint raw) {
    switch (TFMT) {
    case 0: return unpack32(raw);
    case 1: { uint c = raw & 0xFFFFFF; return uint4(unpack32(c).xyz, (AEM != 0 && c == 0) ? 0 : TA0); }
    case 2: return expand16(raw & 0xFFFF);
    case 3: return clut_lookup(raw & 0xFF, false);
    case 4: return clut_lookup(raw & 0xF, true);
    case 5: return clut_lookup(raw >> 24, false);
    case 6: return clut_lookup((raw >> 24) & 0xF, true);
    case 7: return clut_lookup(raw >> 28, true);
    default: return uint4(unpack32(raw & 0xFFFFFF).xyz, TA0);
    }
}

// one axis of the texture address in texture texels at scale k (TSCALE for level 0, 1 for mips)
int wrap_axis(int c, uint mode, int size, int mn, int mx, int k) {
    int n = (c >= 0) ? c / k : -((-c + k - 1) / k);
    int sub = c - n * k;
    int m;
    switch (mode) {
    case 0: m = n & (size - 1); break;
    case 1: m = clamp(n, 0, size - 1); break;
    case 2: m = clamp(n, mn, mx); break;
    default: m = (n & mn) | mx; break;
    }
    if (mode == 1 || mode == 2) sub = (m > n) ? 0 : (m < n ? k - 1 : sub);
    return m * k + sub;
}
uint fetch_lv(int l, int u, int v) {
    if (l == 0) {
        int k = (int)TSCALE;
        int x = wrap_axis(u, WMS, (int)TW, (int)MINU, (int)MAXU, k);
        int y = wrap_axis(v, WMT, (int)TH, (int)MINV, (int)MAXV, k);
        return tex.Load(int3(x + f13.x * k, y + f13.y * k, 0));
    }
    int sw = max((int)TW >> l, 1), sh = max((int)TH >> l, 1);
    int x = wrap_axis(u, WMS, sw, (int)MINU >> l, (int)MAXU >> l, 1);
    int y = wrap_axis(v, WMT, sh, (int)MINV >> l, (int)MAXV >> l, 1);
    int3 p = int3(x, y, 0);
    switch (l) {
    case 1: return texm1.Load(p);
    case 2: return texm2.Load(p);
    case 3: return texm3.Load(p);
    case 4: return texm4.Load(p);
    case 5: return texm5.Load(p);
    default: return texm6.Load(p);
    }
}
uint4 lerp8(uint4 a, uint4 b, uint w) { return (a * (256 - w) + b * w) >> 8; }
// texel at level-0 coordinates (u, v) from mip level l
uint4 sample_lv(int l, float u, float v, bool lin) {
    float sc = (l == 0) ? TSCALE : 1.0 / (float)(1 << l);
    u *= sc;
    v *= sc;
    if (!lin) return texel(fetch_lv(l, (int)floor(u), (int)floor(v)));
    float fu = u - 0.5, fv = v - 0.5;
    float u0f = floor(fu), v0f = floor(fv);
    uint wu = (uint)((fu - u0f) * 256.0), wv = (uint)((fv - v0f) * 256.0);
    int u0 = (int)u0f, v0 = (int)v0f;
    uint4 t00 = texel(fetch_lv(l, u0, v0)), t10 = texel(fetch_lv(l, u0 + 1, v0));
    uint4 t01 = texel(fetch_lv(l, u0, v0 + 1)), t11 = texel(fetch_lv(l, u0 + 1, v0 + 1));
    return lerp8(lerp8(t00, t10, wu), lerp8(t01, t11, wu), wv);
}
uint4 sample_tex(float u, float v, float lod) {
    bool lin = (lod <= 0.0) ? (MAGLIN != 0) : (MINLIN != 0);
    return sample_lv(0, u, v, lin);
}
// GS mipmapping: LOD from Q (or K), level by MMIN (2/4 nearest level, 3/5 blend of two levels)
uint4 sample_mip(float u, float v, float q) {
    float lod = (LCM != 0) ? LODK : log2(1.0 / max(abs(q), 1e-30)) * (float)(1 << LODL) + LODK;
    if (lod <= 0.0) return sample_lv(0, u, v, MAGLIN != 0);
    bool lin = (MMIN == 4 || MMIN == 5);
    if (MMIN == 2 || MMIN == 4) return sample_lv(min((int)floor(lod + 0.5), (int)MIPL), u, v, lin);
    int l0 = min((int)floor(lod), (int)MIPL), l1 = min(l0 + 1, (int)MIPL);
    uint w = (uint)(frac(lod) * 256.0);
    return lerp8(sample_lv(l0, u, v, lin), sample_lv(l1, u, v, lin), w);
}

int4 sample_hd(float u, float v) {
    float k = HDK;
    if (WMS == 3 || WMT == 3) {
        // region repeat (bit masks): nearest texel of the full-size level
        int x = wrap_axis((int)floor(u * k), WMS, (int)TW, (int)MINU, (int)MAXU, (int)k);
        int y = wrap_axis((int)floor(v * k), WMT, (int)TH, (int)MINV, (int)MAXV, (int)k);
        return (int4)round(hdtex.Load(int3(x, y, 0)) * 255.0);
    }
    if (WMS == 2) u = clamp(u, (float)MINU + 0.5 / k, (float)MAXU + 1.0 - 0.5 / k);
    if (WMT == 2) v = clamp(v, (float)MINV + 0.5 / k, (float)MAXV + 1.0 - 0.5 / k);
    uint si = ((WMS == 1 || WMS == 2) ? 2u : 0u) + ((WMT == 1 || WMT == 2) ? 1u : 0u);
    float2 uv = float2(u / (float)TW, v / (float)TH);
    float4 c;
    switch (si) {
    case 0: c = hdtex.Sample(hds[0], uv); break;
    case 1: c = hdtex.Sample(hds[1], uv); break;
    case 2: c = hdtex.Sample(hds[2], uv); break;
    default: c = hdtex.Sample(hds[3], uv); break;
    }
    return (int4)round(c * 255.0);
}

// Text: glyphs (16 alpha levels, one color) sampled with Catmull-Rom interpolation and a steep
// contour at half coverage instead of bilinear blur. Drawn about 1:1 only: the soft glow behind
// the text uses the same kind of texture magnified and stays as it is.
float4 cr_weights(float t) {
    float t2 = t * t, t3 = t2 * t;
    return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}
int4 sample_font(float u, float v, float lod) {
    float texels = max(fwidth(u), fwidth(v)) * vp.z;  // texels per native pixel
    float fu = u - 0.5, fv = v - 0.5;
    float2 b = floor(float2(fu, fv));
    float4 wx = cr_weights(fu - b.x), wy = cr_weights(fv - b.y);
    float4 acc = 0;
    [unroll] for (int y = 0; y < 4; y++) {
        float4 row = 0;
        [unroll] for (int x = 0; x < 4; x++)
            row += (float4)texel(fetch_lv(0, (int)b.x - 1 + x, (int)b.y - 1 + y)) * wx[x];
        acc += row * wy[y];
    }
    float cov = saturate(acc.w / FONTAMAX);
    float w = clamp(fwidth(cov) * 0.75, 0.02, 0.5);
    float a = smoothstep(0.5 - w, 0.5 + w, cov) * FONTAMAX;
    int4 sharp = int4(clamp(acc.xyz, 0.0, 255.0), a);
    if (texels < 0.75 || texels > 1.5) return (int4)sample_tex(u, v, lod);
    return sharp;
}

void ps_main(PSIn i) {
    int2 P = int2(i.pos.xy);
    int S = (int)vp.z;
    int4 c = (IIP != 0) ? int4(i.col) : int4(i.fcol);
    int r = c.x, g = c.y, b = c.z, a = c.w;
    if (TME != 0) {
        float u, v;
        if (FST != 0) { u = i.stq.x; v = i.stq.y; }
        else {
            float q = (i.stq.z == 0.0) ? 1e-10 : i.stq.z;
            float rq = 1.0 / q;
            u = i.stq.x * rq * (float)TW;
            v = i.stq.y * rq * (float)TH;
        }
        int4 t;
        if (HDTEX != 0) t = sample_hd(u, v);
        else if (FONTTEX != 0) t = sample_font(u, v, (NEEDLOD != 0) ? ((abs(i.stq.z) >= QTHR) ? -1.0 : 1.0) : LODK);
        else if (MIPL != 0) t = (int4)sample_mip(u, v, (FST != 0) ? 1.0 : i.stq.z);
        else {
            float lod = (NEEDLOD != 0) ? ((abs(i.stq.z) >= QTHR) ? -1.0 : 1.0) : LODK;
            t = (int4)sample_tex(u, v, lod);
        }
        switch (TFX) {
        case 0:
            r = (t.x * r) >> 7; g = (t.y * g) >> 7; b = (t.z * b) >> 7;
            if (TCC != 0) a = (t.w * a) >> 7;
            break;
        case 1:
            r = t.x; g = t.y; b = t.z;
            if (TCC != 0) a = t.w;
            break;
        case 2:
            r = ((t.x * r) >> 7) + a; g = ((t.y * g) >> 7) + a; b = ((t.z * b) >> 7) + a;
            if (TCC != 0) a = t.w + a;
            break;
        default:
            r = ((t.x * r) >> 7) + a; g = ((t.y * g) >> 7) + a; b = ((t.z * b) >> 7) + a;
            if (TCC != 0) a = t.w;
            break;
        }
        r = min(r, 255); g = min(g, 255); b = min(b, 255); a = min(a, 255);
    }
    if (FGE != 0) {
        int fg = (int)i.fog;
        int fr = FOGCOL & 0xFF, fgc = (FOGCOL >> 8) & 0xFF, fbc = (FOGCOL >> 16) & 0xFF;
        r = (r * fg + fr * (255 - fg)) >> 8;
        g = (g * fg + fgc * (255 - fg)) >> 8;
        b = (b * fg + fbc * (255 - fg)) >> 8;
    }
    bool write_rgb = true, write_a = true, write_z = (ZMSK == 0);
    if (ATE != 0) {
        uint ua = (uint)a;
        bool apass;
        switch (ATST) {
        case 0: apass = false; break;
        case 1: apass = true; break;
        case 2: apass = ua < AREF; break;
        case 3: apass = ua <= AREF; break;
        case 4: apass = ua == AREF; break;
        case 5: apass = ua >= AREF; break;
        case 6: apass = ua > AREF; break;
        default: apass = ua != AREF; break;
        }
        if (!apass) {
            if (AFAIL == 0) discard;
            else if (AFAIL == 1) write_z = false;
            else if (AFAIL == 2) { write_rgb = false; write_a = false; }
            else { write_a = false; write_z = false; }
        }
    }
    // ordered section: everything below sees the results of earlier primitives at this pixel
    uint raw = fb[P];
    uint dst = raw;
    if (FPSM == 2)
        dst = ((raw & 0x1F) << 3) | (((raw >> 5) & 0x1F) << 11) | (((raw >> 10) & 0x1F) << 19) | ((raw & 0x8000) << 16);
    if (DATE != 0 && FPSM != 1) {
        if (((dst >> 31) & 1) != DATM) return;
    }
    if (ZTE != 0) {
        uint zm = (ZFMT == 0) ? 0xFFFFFFFF : ((ZFMT == 1) ? 0xFFFFFF : 0xFFFF);
        float zf = i.z;
        uint z = (zf >= 4294967295.0) ? 0xFFFFFFFF : ((zf <= 0.0) ? 0 : (uint)zf);
        z = min(z, zm);
        uint zraw = zb[P];
        if (ZTST != 1) {
            uint dz = zraw & zm;
            bool zpass = (ZTST == 0) ? false : ((ZTST == 2) ? (z >= dz) : (z > dz));
            if (!zpass) return;
        }
        if (write_z) zb[P] = (ZFMT == 1) ? ((zraw & 0xFF000000) | z) : ((ZFMT == 2) ? ((zraw & 0xFFFF0000) | z) : z);
    }
    if (!write_rgb && !write_a) return;
    if (ABE != 0 && !(PABE != 0 && a < 0x80)) {
        int3 cd = int3(dst & 0xFF, (dst >> 8) & 0xFF, (dst >> 16) & 0xFF);
        int3 cs = int3(r, g, b);
        int ad = (FPSM == 1) ? 0x80 : (int)(dst >> 24);
        int cc = (BC == 0) ? a : ((BC == 1) ? ad : (int)BFIX);
        int3 A = (BA == 0) ? cs : ((BA == 1) ? cd : int3(0, 0, 0));
        int3 B = (BB == 0) ? cs : ((BB == 1) ? cd : int3(0, 0, 0));
        int3 D = (BD == 0) ? cs : ((BD == 1) ? cd : int3(0, 0, 0));
        int3 o = (((A - B) * cc) >> 7) + D;
        r = o.x; g = o.y; b = o.z;
    }
    if (DTHE != 0 && FPSM == 2) {
        int2 N = P / S;
        int dd = dimx[N.y & 3][N.x & 3];
        r += dd; g += dd; b += dd;
    }
    if (COLCLAMP != 0) {
        r = clamp(r, 0, 255); g = clamp(g, 0, 255); b = clamp(b, 0, 255);
    } else {
        r &= 0xFF; g &= 0xFF; b &= 0xFF;
    }
    uint outa = ((uint)a & 0xFF) | FBA;
    uint col = (uint)r | ((uint)g << 8) | ((uint)b << 16) | ((outa & 0xFF) << 24);
    uint mask = FBMSK;
    if (!write_rgb) mask |= 0x00FFFFFF;
    if (!write_a) mask |= 0xFF000000;
    if (mask == 0xFFFFFFFF) return;
    if (FPSM == 2) {
        uint c16 = ((col >> 3) & 0x1F) | (((col >> 11) & 0x1F) << 5) | (((col >> 19) & 0x1F) << 10) | ((col >> 16) & 0x8000);
        uint m16 = ((mask >> 3) & 0x1F) | (((mask >> 11) & 0x1F) << 5) | (((mask >> 19) & 0x1F) << 10) | ((mask >> 16) & 0x8000);
        fb[P] = (raw & (m16 | 0xFFFF0000)) | (c16 & ~m16 & 0xFFFF);
    } else {
        if (FPSM == 1) mask |= 0xFF000000;
        fb[P] = (raw & mask) | (col & ~mask);
    }
}
)HLSL";

// Surface maintenance: upscale native data into a surface, read a surface back at native
// resolution (top-left sample of each S x S group), copy rectangles between surfaces.
static const char* UTIL_HLSL = R"HLSL(
cbuffer U : register(b0) {
    int4 rect;      // x0, y0, x1, y1 (destination pixels)
    int4 info;      // scale S, source offset x, y
};
Texture2D<uint> src : register(t0);
RWTexture2D<uint> dst : register(u0);

[numthreads(8, 8, 1)]
void cs_upscale(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(rect.x + (int)id.x, rect.y + (int)id.y);
    if (p.x >= rect.z || p.y >= rect.w) return;
    dst[p] = src.Load(int3(p / info.x, 0));
}

[numthreads(8, 8, 1)]
void cs_downsample(uint3 id : SV_DispatchThreadID) {
    int2 p = int2(rect.x + (int)id.x, rect.y + (int)id.y);
    if (p.x >= rect.z || p.y >= rect.w) return;
    dst[p] = src.Load(int3(p * info.x, 0));
}
)HLSL";

// Display: PCRTC merge of the two read circuits from surfaces into the output texture.
static const char* PRESENT_HLSL = R"HLSL(
cbuffer P : register(b0) {
    int4 c1;        // dbx, dby (native), psm kind (0 C32, 1 C24, 2 C16), enabled
    int4 c2;
    int4 m;         // alp, mmod, slbg, field mode
    int4 bg;        // background color, scale S
};
Texture2D<uint> s1 : register(t0);
Texture2D<uint> s2 : register(t1);
struct VSOut { float4 pos : SV_Position; };
VSOut vs_full(uint id : SV_VertexID) {
    VSOut o;
    float2 t = float2(id & 1, id >> 1);
    o.pos = float4(t.x * 2 - 1, 1 - t.y * 2, 0, 1);
    return o;
}
uint3 rgb_of(uint raw, int kind) {
    if (kind == 2) return uint3((raw & 0x1F) << 3, ((raw >> 5) & 0x1F) << 3, ((raw >> 10) & 0x1F) << 3);
    return uint3(raw & 0xFF, (raw >> 8) & 0xFF, (raw >> 16) & 0xFF);
}
uint alpha_of(uint raw, int kind) {
    if (kind == 2) return (raw & 0x8000) != 0 ? 0x80 : 0;
    return raw >> 24;
}
float4 ps_merge(VSOut i) : SV_Target {
    int2 P = int2(i.pos.xy);
    int S = bg.w;
    if (m.w != 0) P.y = (P.y / (2 * S)) * S + (P.y % S);  // field mode: each field line shown twice
    uint3 col2 = uint3(bg.x & 0xFF, (bg.x >> 8) & 0xFF, (bg.x >> 16) & 0xFF);
    uint raw1 = 0, raw2 = 0;
    if (c2.w != 0) { raw2 = s2.Load(int3(c2.x * S + P.x, c2.y * S + P.y, 0)); col2 = rgb_of(raw2, c2.z); }
    uint3 o = col2;
    if (c1.w != 0) {
        raw1 = s1.Load(int3(c1.x * S + P.x, c1.y * S + P.y, 0));
        uint3 col1 = rgb_of(raw1, c1.z);
        if (c2.w != 0 && m.z == 0) {
            uint a = (m.y != 0) ? (uint)m.x : (c1.z == 1 ? 0x80 : alpha_of(raw1, c1.z));
            a = min(a, 128u);
            o = min((col1 * a + col2 * (128 - a)) >> 7, 255u);
        } else if (c2.w == 0) {
            o = col1;
        }
    }
    return float4(o / 255.0, 1.0);
}
)HLSL";

}  // namespace gpu
}  // namespace gs
