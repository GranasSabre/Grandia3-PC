// GPU renderer: the GS pixel pipeline in a pixel shader on rasterizer ordered views (Direct3D 11.3),
// at an integer multiple S of the native resolution.
//
// Memory model (in the spirit of a hardware GS renderer with a texture cache):
//  - shadow: a CPU copy of the 4MB local memory, current for host->local uploads, CLUT loads and
//    texture decoding
//  - surfaces: frame and z buffers as R32_UINT textures at S x S resolution holding raw GS pixel
//    values (32-bit color words, 16-bit color, z). The draw shader performs texturing, texture
//    function, fog, alpha test, destination alpha test, depth test, blending, dithering, masking
//    and format conversion like the software rasterizer, with per-pixel ordered read-modify-write
//  - per 256-byte block: the surface holding the newest data (or the shadow copy) and whether
//    the shadow copy is current. Before a surface is drawn to or sampled, its stale blocks are
//    refreshed from the shadow copy (reading back the owning surface first if needed). Textures
//    come from a surface when the memory was drawn in the same layout, otherwise they are
//    decoded from the shadow copy (cached until the memory changes).
// Commands arrive in queue order on the renderer thread (gs_draw.cpp); the software rasterizer
// keeps running in parallel and still serves local->host readbacks.
#include "gs_gpu.h"
#include "gs_drawstate.h"
#include "gs_gpu_shaders.h"
#include "gs_texrep.h"
#include "../platform/platform.h"
#include "../ee/kernel.h"
#include <d3d11_3.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <map>

namespace gs {
namespace gpu {

static bool g_enabled = false;
static int S = 1;
static ID3D11Device* dev = nullptr;
static ID3D11DeviceContext* ctx = nullptr;
static u32* shadow = nullptr;
static bool g_texrep_on = false;   // HD textures: dumping, or a texture pack exists
double g_hud_log_at = 0;
bool g_hd_ab = false;              // --hd-ab: screenshot pairs with HD textures on/off
static int g_last_ow = 0, g_last_oh = 0;  // size of the last presented picture (re-present)
bool g_sharpen_ab = false;         // --sharpen-ab: window screenshot pairs with sharpening on/off
static size_t REP_BUDGET = (size_t)1536 << 20;  // video memory for HD textures (set from the adapter in init)

template <class T> static void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
static void unbind_draw();
static void init_uav_overlap();

// ---------------------------------------------------------------------------
// Layouts
// ---------------------------------------------------------------------------
enum Layout { L_C32, L_C16, L_C16S, L_Z32, L_Z16, L_Z16S, L_NONE };
static Layout layout_of(u32 psm) {
    switch (psm) {
    case PSMCT32: case PSMCT24: case PSMT8H: case PSMT4HL: case PSMT4HH: return L_C32;
    case PSMCT16: return L_C16;
    case PSMCT16S: return L_C16S;
    case PSMZ32: case PSMZ24: return L_Z32;
    case PSMZ16: return L_Z16;
    case PSMZ16S: return L_Z16S;
    }
    return L_NONE;
}
static inline bool is16(Layout l) { return l == L_C16 || l == L_C16S || l == L_Z16 || l == L_Z16S; }
static int psm_family(u32 psm) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 16;
    case PSMT8: return 8;
    case PSMT4: return 4;
    }
    return 32;
}
static inline u32 raw_addr(Layout l, int x, int y, u32 bp, u32 bw) {
    switch (l) {
    case L_C32: return addr32(x, y, bp, bw);
    case L_Z32: return addr32z(x, y, bp, bw);
    case L_C16: return addr16(x, y, bp, bw);
    case L_C16S: return addr16s(x, y, bp, bw);
    case L_Z16: return addr16z(x, y, bp, bw);
    default: return addr16sz(x, y, bp, bw);
    }
}
static inline u32 raw_block(Layout l, int x, int y, u32 bp, u32 bw) {
    u32 a = raw_addr(l, x, y, bp, bw);
    return (is16(l) ? (a >> 7) : (a >> 6)) & 16383;
}
static inline u32 shadow_read_raw(Layout l, int x, int y, u32 bp, u32 bw) {
    u32 a = raw_addr(l, x, y, bp, bw);
    return is16(l) ? ((const u16*)shadow)[a] : shadow[a];
}
static inline void shadow_write_raw(Layout l, int x, int y, u32 bp, u32 bw, u32 v) {
    u32 a = raw_addr(l, x, y, bp, bw);
    if (is16(l)) ((u16*)shadow)[a] = (u16)v;
    else shadow[a] = v;
}
// block geometry of any pixel format (for uploads and textures)
static void psm_block_dims(u32 psm, int& bw, int& bh) {
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: bw = 16; bh = 8; break;
    case PSMT8: bw = 16; bh = 16; break;
    case PSMT4: bw = 32; bh = 16; break;
    default: bw = 8; bh = 8; break;
    }
}
static u32 psm_block(u32 psm, int x, int y, u32 bp, u32 bw) {
    switch (psm) {
    case PSMCT16: return (addr16(x, y, bp, bw) >> 7) & 16383;
    case PSMCT16S: return (addr16s(x, y, bp, bw) >> 7) & 16383;
    case PSMZ16: return (addr16z(x, y, bp, bw) >> 7) & 16383;
    case PSMZ16S: return (addr16sz(x, y, bp, bw) >> 7) & 16383;
    case PSMT8: return (addr8(x, y, bp, bw) >> 8) & 16383;
    case PSMT4: return (addr4(x, y, bp, bw) >> 9) & 16383;
    case PSMZ32: case PSMZ24: return (addr32z(x, y, bp, bw) >> 6) & 16383;
    default: return (addr32(x, y, bp, bw) >> 6) & 16383;
    }
}
// blocks of the rectangle [x0, x1) x [y0, y1) in format psm; full: whether the block lies completely inside
template <class F>
static void for_rect_blocks(u32 psm, u32 bp, u32 bw, int x0, int y0, int x1, int y1, F f) {
    int cw, ch;
    psm_block_dims(psm, cw, ch);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    x1 = std::min(2048, x1); y1 = std::min(2048, y1);
    for (int y = y0 & ~(ch - 1); y < y1; y += ch)
        for (int x = x0 & ~(cw - 1); x < x1; x += cw) {
            bool full = x >= x0 && y >= y0 && x + cw <= x1 && y + ch <= y1;
            f(psm_block(psm, x, y, bp, bw), full);
        }
}

// ---------------------------------------------------------------------------
// Per-block state
// ---------------------------------------------------------------------------
static s16 g_owner[16384];     // surface with the newest data, -1: the shadow copy
static u8 g_shadow_ok[16384];  // shadow copy holds the newest data
static u64 g_stamp[16384];     // last write (texture cache validation)
static u64 g_stamp_next = 1;
// per 8KB page (32 blocks): blocks whose shadow copy is stale, and the newest write stamp
static u16 g_page_stale[512];
static u64 g_page_stamp[512];
static inline void set_shadow_ok(u32 b, bool ok) {
    if (g_shadow_ok[b] == (u8)ok) return;
    g_shadow_ok[b] = ok;
    if (ok) g_page_stale[b >> 5]--;
    else g_page_stale[b >> 5]++;
}
static inline void stamp_block(u32 b) {
    g_stamp[b] = g_stamp_next++;
    g_page_stamp[b >> 5] = g_stamp[b];
}

// statistics
static u64 st_draws, st_batches, st_prims, st_refresh_cells, st_readback_cells, st_readbacks, st_tex_decodes, st_tex_hits,
    st_stale_texels, st_self, st_surfaces;

// ---------------------------------------------------------------------------
// Surfaces
// ---------------------------------------------------------------------------
struct Surface {
    int id = -1;
    u32 bp = 0, bw = 0;
    Layout lay = L_C32;
    int w = 0, h = 0;      // native size
    int cbw = 8, cbh = 8;  // cell (block) size in pixels
    int cw = 0, ch = 0;    // cells per row / column
    ID3D11Texture2D* tex = nullptr;
    ID3D11UnorderedAccessView* uav = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    std::vector<u16> cell_block;   // block of each cell
    std::vector<u8> dirty;         // cell content stale
    std::vector<int> block_cell;   // block -> cell (-1: not part of this surface)
    int ndirty = 0;
    u64 last_use = 0;
};
static std::vector<Surface*> g_surf;  // index = id; nullptr for free ids
static u64 g_frame = 0;

static Surface* surf_by_id(int id) { return (id >= 0 && id < (int)g_surf.size()) ? g_surf[id] : nullptr; }

static bool create_tex(Surface* s) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = s->w * S;
    td.Height = s->h * S;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R32_UINT;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &s->tex))) return false;
    dev->CreateUnorderedAccessView(s->tex, nullptr, &s->uav);
    dev->CreateShaderResourceView(s->tex, nullptr, &s->srv);
    return true;
}

static void build_cells(Surface* s) {
    int ph = is16(s->lay) ? 64 : 32;
    s->cbw = is16(s->lay) ? 16 : 8;
    s->cbh = 8;
    s->h = (s->h + ph - 1) / ph * ph;
    s->cw = s->w / s->cbw;
    s->ch = s->h / s->cbh;
    s->cell_block.assign((size_t)s->cw * s->ch, 0);
    s->block_cell.assign(16384, -1);
    for (int cy = 0; cy < s->ch; cy++)
        for (int cx = 0; cx < s->cw; cx++) {
            u32 b = raw_block(s->lay, cx * s->cbw, cy * s->cbh, s->bp, s->bw);
            int ci = cy * s->cw + cx;
            s->cell_block[ci] = (u16)b;
            if (s->block_cell[b] < 0) s->block_cell[b] = ci;
        }
}

static Surface* create_surface(u32 bp, u32 bw, Layout lay, int h) {
    Surface* s = new Surface;
    s->bp = bp;
    s->bw = std::max<u32>(bw, 1);
    s->lay = lay;
    s->w = (int)s->bw * 64;
    s->h = std::min(std::max(h, 32), 2048);
    build_cells(s);
    if (!create_tex(s)) {
        delete s;
        return nullptr;
    }
    s->dirty.assign(s->cell_block.size(), 1);
    s->ndirty = (int)s->dirty.size();
    int id = -1;
    for (int i = 0; i < (int)g_surf.size(); i++)
        if (!g_surf[i]) { id = i; break; }
    if (id < 0) { id = (int)g_surf.size(); g_surf.push_back(nullptr); }
    s->id = id;
    g_surf[id] = s;
    st_surfaces++;
    log_write(LOG_GS, "gpu: surface %d %05x/%u layout %d %dx%d (x%d)", id, bp, s->bw, (int)lay, s->w, s->h, S);
    return s;
}

static void grow_surface(Surface* s, int h) {
    h = std::min(h, 2048);
    if (h <= s->h) return;
    ID3D11Texture2D* old = s->tex;
    ID3D11UnorderedAccessView* olduav = s->uav;
    ID3D11ShaderResourceView* oldsrv = s->srv;
    int oldh = s->h;
    std::vector<u8> olddirty = s->dirty;
    s->tex = nullptr; s->uav = nullptr; s->srv = nullptr;
    s->h = h;
    build_cells(s);
    create_tex(s);
    unbind_draw();
    D3D11_BOX box{0, 0, 0, (UINT)(s->w * S), (UINT)(oldh * S), 1};
    ctx->CopySubresourceRegion(s->tex, 0, 0, 0, 0, old, 0, &box);
    s->dirty.assign(s->cell_block.size(), 1);
    for (size_t i = 0; i < olddirty.size() && i < s->dirty.size(); i++) s->dirty[i] = olddirty[i];
    s->ndirty = 0;
    for (u8 d : s->dirty) s->ndirty += d;
    release(old); release(olduav); release(oldsrv);
}

// ---------------------------------------------------------------------------
// Pipeline objects
// ---------------------------------------------------------------------------
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11InputLayout* g_il;
static ID3D11ComputeShader* g_cs_up;
static ID3D11ComputeShader* g_cs_down;
static ID3D11VertexShader* g_vs_full;
static ID3D11PixelShader* g_ps_merge;
static ID3D11Buffer* g_cb;       // draw constants
static ID3D11Buffer* g_cbu;      // util constants
static ID3D11Buffer* g_cbp;      // present constants
static ID3D11Buffer* g_vb;
static const UINT VB_BYTES = 16 << 20;
static ID3D11RasterizerState* g_rs;
static ID3D11RasterizerState* g_rs_noscissor;
static ID3D11Buffer* g_clut_buf;
static ID3D11SamplerState* g_hd_samp[4];     // HD textures: u wrap/clamp x v wrap/clamp, anisotropic
static ID3D11ShaderResourceView* g_clut_srv;
static ID3D11Texture2D* g_native;           // native-resolution scratch (refresh source / readback target)
static ID3D11ShaderResourceView* g_native_srv;
static ID3D11UnorderedAccessView* g_native_uav;
static ID3D11Texture2D* g_staging;          // CPU readback of g_native
static ID3D11Texture2D* g_dummy_tex;        // bound when a slot is unused
static ID3D11UnorderedAccessView* g_dummy_uav;
static ID3D11ShaderResourceView* g_dummy_srv;
static ID3D11Texture2D* g_out;              // display composition
static ID3D11RenderTargetView* g_out_rtv;
static ID3D11ShaderResourceView* g_out_srv;
static int g_out_w = 0, g_out_h = 0, g_out_mips = 1;
static ID3D11Texture2D* g_out_staging;
static ID3D11Texture2D* g_snap_tex;         // copy of a surface sampled while drawn to
static ID3D11ShaderResourceView* g_snap_srv;
static int g_snap_w = 0, g_snap_h = 0;

// Compiled shaders are cached next to the executable (shadercache/), keyed by source and entry point.
static bool compile(const char* src, const char* entry, const char* target, ID3DBlob** out) {
    u64 h = 1469598103934665603ull;
    for (const char* p : {src, entry, target})
        for (; *p; p++) h = (h ^ (u8)*p) * 1099511628211ull;
    std::string dir = exe_dir() + "/shadercache";
    char name[64];
    snprintf(name, sizeof(name), "/%016llx.bin", (unsigned long long)h);
    std::string path = dir + name;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        bool ok = false;
        if (n > 0 && SUCCEEDED(D3DCreateBlob((SIZE_T)n, out))) ok = fread((*out)->GetBufferPointer(), 1, (size_t)n, f) == (size_t)n;
        fclose(f);
        if (ok) return true;
        release(*out);
    }
    ID3DBlob* err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), entry, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out, &err);
    if (FAILED(hr)) {
        log_write(LOG_GS, "gpu: shader %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
        release(err);
        return false;
    }
    release(err);
    CreateDirectoryA(dir.c_str(), nullptr);
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite((*out)->GetBufferPointer(), 1, (*out)->GetBufferSize(), f);
        fclose(f);
    }
    return true;
}

static void make_tex2d(int w, int h, DXGI_FORMAT f, UINT bind, D3D11_USAGE usage, UINT cpu, ID3D11Texture2D** t, int mips = 1,
                       UINT misc = 0) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = mips;
    td.ArraySize = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Usage = usage;
    td.BindFlags = bind;
    td.CPUAccessFlags = cpu;
    td.MiscFlags = misc;
    dev->CreateTexture2D(&td, nullptr, t);
}

static ID3D11Buffer* make_cb(UINT bytes) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = (bytes + 15) & ~15u;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* b = nullptr;
    dev->CreateBuffer(&bd, nullptr, &b);
    return b;
}
static void write_cb(ID3D11Buffer* b, const void* data, size_t n) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, n);
        ctx->Unmap(b, 0);
    }
}

bool init(int scale) {
    dev = platform::d3d_device();
    ctx = platform::d3d_context();
    if (!dev || !ctx) return false;
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 o2{};
    if (FAILED(dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS2, &o2, sizeof(o2))) || !o2.ROVsSupported) {
        log_write(LOG_GS, "gpu: rasterizer ordered views not supported, using the software renderer");
        return false;
    }
    init_uav_overlap();
    if (scale <= 0) {
        // automatic: native height 448 times S should cover the window height
        int wh = platform::config().window_height;
        if (GetSystemMetrics(SM_CYSCREEN) > 0) wh = std::max(wh, (int)GetSystemMetrics(SM_CYSCREEN));
        // One internal line per screen line or more. No supersampling: the rasterizer-ordered views
        // make every extra pixel expensive (x7 for a 1440p screen dropped the flight scene and battles
        // to 42-55 fps even on an RTX 5070, x4 holds 60).
        int target = wh;
        scale = std::max(1, std::min(8, (target + 447) / 448));
        // integrated graphics (little dedicated memory): at most x2
        IDXGIDevice* dd = nullptr;
        IDXGIAdapter* ad = nullptr;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&ad)) &&
            SUCCEEDED(ad->GetDesc(&desc)) && desc.DedicatedVideoMemory < ((size_t)1 << 30))
            scale = std::min(scale, 2);
        if (ad) ad->Release();
        if (dd) dd->Release();
        // Steam Deck: its graphics chip held 13-30 fps at x3 (a player's log), x2 fills its 800 lines
        if (platform::is_steam_deck()) scale = std::min(scale, 2);
    }
    S = std::max(1, std::min(8, scale));
    ID3DBlob *vsb = nullptr, *psb = nullptr, *cs1 = nullptr, *cs2 = nullptr, *vsf = nullptr, *psm = nullptr;
    if (!compile(DRAW_HLSL, "vs_main", "vs_5_0", &vsb) || !compile(DRAW_HLSL, "ps_main", "ps_5_0", &psb) ||
        !compile(UTIL_HLSL, "cs_upscale", "cs_5_0", &cs1) || !compile(UTIL_HLSL, "cs_downsample", "cs_5_0", &cs2) ||
        !compile(PRESENT_HLSL, "vs_full", "vs_5_0", &vsf) || !compile(PRESENT_HLSL, "ps_merge", "ps_5_0", &psm))
        return false;
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    dev->CreateComputeShader(cs1->GetBufferPointer(), cs1->GetBufferSize(), nullptr, &g_cs_up);
    dev->CreateComputeShader(cs2->GetBufferPointer(), cs2->GetBufferSize(), nullptr, &g_cs_down);
    dev->CreateVertexShader(vsf->GetBufferPointer(), vsf->GetBufferSize(), nullptr, &g_vs_full);
    dev->CreatePixelShader(psm->GetBufferPointer(), psm->GetBufferSize(), nullptr, &g_ps_merge);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        {"POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"ZV", 0, DXGI_FORMAT_R32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"STQ", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FOG", 0, DXGI_FORMAT_R32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"FCOL", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(dev->CreateInputLayout(il, 6, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_il))) return false;
    release(vsb); release(psb); release(cs1); release(cs2); release(vsf); release(psm);

    g_cb = make_cb(512);
    g_cbu = make_cb(32);
    g_cbp = make_cb(64);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = VB_BYTES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &g_vb);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = FALSE;
    rd.ScissorEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g_rs);
    rd.ScissorEnable = FALSE;
    dev->CreateRasterizerState(&rd, &g_rs_noscissor);
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 512 * 4;
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dev->CreateBuffer(&cb, nullptr, &g_clut_buf);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_UINT;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    sd.Buffer.FirstElement = 0;
    sd.Buffer.NumElements = 512;
    dev->CreateShaderResourceView(g_clut_buf, &sd, &g_clut_srv);
    for (int i = 0; i < 4; i++) {
        D3D11_SAMPLER_DESC smp{};
        smp.Filter = D3D11_FILTER_ANISOTROPIC;
        smp.MaxAnisotropy = 16;
        smp.AddressU = (i & 2) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        smp.AddressV = (i & 1) ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP;
        smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        smp.MipLODBias = platform::config().hd_lod_bias;
        smp.MaxLOD = D3D11_FLOAT32_MAX;
        smp.ComparisonFunc = D3D11_COMPARISON_NEVER;
        dev->CreateSamplerState(&smp, &g_hd_samp[i]);
    }
    make_tex2d(2048, 2048, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, &g_native);
    dev->CreateShaderResourceView(g_native, nullptr, &g_native_srv);
    dev->CreateUnorderedAccessView(g_native, nullptr, &g_native_uav);
    make_tex2d(2048, 2048, DXGI_FORMAT_R32_UINT, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_staging);
    make_tex2d(1, 1, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, D3D11_USAGE_DEFAULT, 0, &g_dummy_tex);
    dev->CreateUnorderedAccessView(g_dummy_tex, nullptr, &g_dummy_uav);
    dev->CreateShaderResourceView(g_dummy_tex, nullptr, &g_dummy_srv);
    shadow = new u32[1024 * 1024]();
    {
        static const u32 zero[512] = {};
        ctx->UpdateSubresource(g_clut_buf, 0, nullptr, zero, 0, 0);
    }
    for (int i = 0; i < 16384; i++) { g_owner[i] = -1; g_shadow_ok[i] = 1; }
    texrep::init(platform::config().texture_dump);
    {
        // a third of the dedicated video memory, 1-4 GB
        IDXGIDevice* dd = nullptr;
        IDXGIAdapter* ad = nullptr;
        DXGI_ADAPTER_DESC desc{};
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&desc))) {
            size_t b = desc.DedicatedVideoMemory / 3;
            REP_BUDGET = std::max((size_t)1 << 30, std::min(b, (size_t)4 << 30));
        }
        if (ad) ad->Release();
        if (dd) dd->Release();
        log_write(LOG_GS, "gpu: HD texture budget %zu MB", REP_BUDGET >> 20);
    }
    g_texrep_on = texrep::dumping() || texrep::have_pack();
    g_enabled = true;
    log_write(LOG_GS, "gpu: renderer active, internal resolution x%d", S);
    return true;
}

bool enabled() { return g_enabled; }
int scale() { return S; }

// ---------------------------------------------------------------------------
// Bound draw state (avoids redundant device context calls between batches)
// ---------------------------------------------------------------------------
static struct Bound {
    bool pipeline = false;
    ID3D11UnorderedAccessView* uav[2] = {nullptr, nullptr};
    ID3D11ShaderResourceView* srv[2] = {nullptr, nullptr};
    ID3D11ShaderResourceView* hd = nullptr;
    float vpw = 0, vph = 0;
    D3D11_RECT sc{-1, -1, -1, -1};
} g_bound;
static UINT g_vb_pos = 0;

// NVIDIA: D3D11 drivers wait for every draw that writes a UAV before starting the next one that
// uses it. The draws only reach the surfaces through rasterizer ordered views, which keep the
// order of overlapping pixels by themselves, so that wait is switched off (NvAPI UAV overlap,
// entry points looked up in the driver's nvapi64.dll) for runs of plain draws. Copies, compute
// passes, presentation and draws that sample a surface switch it back on (unbind_draw / flush).
using NvUavFn = int(__cdecl*)(IUnknown*);
static NvUavFn g_nv_begin = nullptr, g_nv_end = nullptr;
static bool g_overlap = false;
bool g_no_uav_overlap = true;  // default off (no measurable gain); --uav-overlap enables it
static void init_uav_overlap() {
    if (g_no_uav_overlap) return;
    HMODULE m = LoadLibraryW(L"nvapi64.dll");
    if (!m) return;
    using QI = void*(__cdecl*)(unsigned int);
    QI qi = (QI)(void*)GetProcAddress(m, "nvapi_QueryInterface");
    if (!qi) return;
    auto init = (int(__cdecl*)())qi(0x0150E828u);  // NvAPI_Initialize
    auto begin = (NvUavFn)qi(0x65B93CA8u);         // NvAPI_D3D11_BeginUAVOverlap
    auto end = (NvUavFn)qi(0x2216A357u);           // NvAPI_D3D11_EndUAVOverlap
    if (!init || !begin || !end || init() != 0) return;
    g_nv_begin = begin;
    g_nv_end = end;
    log_write(LOG_GS, "gpu: NVIDIA UAV overlap available");
}
static void uav_overlap(bool on) {
    if (on == g_overlap || !g_nv_begin) return;
    if ((on ? g_nv_begin : g_nv_end)(ctx) != 0) {
        g_nv_begin = g_nv_end = nullptr;  // not supported: stay synchronized
        log_write(LOG_GS, "gpu: UAV overlap failed, disabled");
        return;
    }
    g_overlap = on;
}

// release surfaces from the draw bindings before other uses (copies, compute, presentation)
static void unbind_draw() {
    uav_overlap(false);
    if (g_bound.uav[0] || g_bound.uav[1]) {
        ID3D11UnorderedAccessView* nuav[2] = {nullptr, nullptr};
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 2, nuav, nullptr);
        g_bound.uav[0] = g_bound.uav[1] = nullptr;
    }
    if (g_bound.srv[0] || g_bound.srv[1]) {
        ID3D11ShaderResourceView* nsrv[2] = {nullptr, nullptr};
        ctx->PSSetShaderResources(0, 2, nsrv);
        g_bound.srv[0] = g_bound.srv[1] = nullptr;
    }
}
// something else changed the pipeline state
static void forget_bound() {
    unbind_draw();
    g_bound.pipeline = false;
    g_bound.vpw = g_bound.vph = 0;
    g_bound.sc = D3D11_RECT{-1, -1, -1, -1};
}

// ---------------------------------------------------------------------------
// Refresh / readback
// ---------------------------------------------------------------------------
static void dispatch_util(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* src, ID3D11UnorderedAccessView* dst, int x0, int y0, int x1,
                          int y1) {
    if (x1 <= x0 || y1 <= y0) return;
    unbind_draw();
    int cbd[8] = {x0, y0, x1, y1, S, 0, 0, 0};
    write_cb(g_cbu, cbd, sizeof(cbd));
    ctx->CSSetShader(cs, nullptr, 0);
    ctx->CSSetConstantBuffers(0, 1, &g_cbu);
    ctx->CSSetShaderResources(0, 1, &src);
    ctx->CSSetUnorderedAccessViews(0, 1, &dst, nullptr);
    ctx->Dispatch((UINT)(x1 - x0 + 7) / 8, (UINT)(y1 - y0 + 7) / 8, 1);
    ID3D11ShaderResourceView* nsrv = nullptr;
    ID3D11UnorderedAccessView* nuav = nullptr;
    ctx->CSSetShaderResources(0, 1, &nsrv);
    ctx->CSSetUnorderedAccessViews(0, 1, &nuav, nullptr);
}

// Copy the given cells of surface o into the shadow copy (native resolution, top-left samples).
static void readback_cells(Surface* o, const std::vector<int>& cells) {
    if (cells.empty()) return;
    unbind_draw();
    int x0 = 1 << 30, y0 = 1 << 30, x1 = 0, y1 = 0;
    for (int ci : cells) {
        int cx = ci % o->cw, cy = ci / o->cw;
        x0 = std::min(x0, cx * o->cbw); y0 = std::min(y0, cy * o->cbh);
        x1 = std::max(x1, (cx + 1) * o->cbw); y1 = std::max(y1, (cy + 1) * o->cbh);
    }
    dispatch_util(g_cs_down, o->srv, g_native_uav, x0, y0, x1, y1);
    D3D11_BOX box{(UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1};
    ctx->CopySubresourceRegion(g_staging, 0, x0, y0, 0, g_native, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_staging, 0, D3D11_MAP_READ, 0, &m))) return;
    const u8* base = (const u8*)m.pData;
    for (int ci : cells) {
        int cx = ci % o->cw, cy = ci / o->cw;
        for (int y = cy * o->cbh; y < (cy + 1) * o->cbh; y++) {
            const u32* row = (const u32*)(base + (size_t)y * m.RowPitch);
            for (int x = cx * o->cbw; x < (cx + 1) * o->cbw; x++) shadow_write_raw(o->lay, x, y, o->bp, o->bw, row[x]);
        }
        u32 b = o->cell_block[ci];
        set_shadow_ok(b, true);
    }
    ctx->Unmap(g_staging, 0);
    st_readbacks++;
    st_readback_cells += cells.size();
}

// Make the shadow copy current for these blocks (reading back the owning surfaces).
static const char* g_why = "";
// Blocks among these that were last drawn in the same pixel size as psm (only those keep data a
// partial CPU write or a CLUT load may depend on; memory drawn in another size is being reused).
static std::vector<u32> same_family(const std::vector<u32>& blocks, u32 psm);
static u32 g_display_fbp[2] = {~0u, ~0u};  // the two most recently displayed frame buffers
static void make_shadow_current(const std::vector<u32>& blocks) {
    std::unordered_map<int, std::vector<int>> by_owner;
    for (u32 b : blocks) {
        if (g_shadow_ok[b]) continue;
        Surface* o = surf_by_id(g_owner[b]);
        if (!o || o->block_cell[b] < 0) { set_shadow_ok(b, true); continue; }
        by_owner[o->id].push_back(o->block_cell[b]);
    }
    for (auto& [id, cells] : by_owner) {
        static int nlog = 0;
        if (nlog++ < 60)
            log_write(LOG_GS, "gpu readback (%s): surface %d %05x/%u lay %d, %zu cells, first block %x", g_why, id, g_surf[id]->bp,
                      g_surf[id]->bw, (int)g_surf[id]->lay, cells.size(), g_surf[id]->cell_block[cells[0]]);
        std::sort(cells.begin(), cells.end());
        cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
        readback_cells(g_surf[id], cells);
    }
}

// Bring the stale cells of s inside the native rectangle up to date. skip: cells about to be
// completely overwritten (not refreshed, considered current).
static u64 st_gpu_copy_cells = 0, st_skip_cells = 0;
struct PixRect { int x0, y0, x1, y1; };
// skip: rectangles the caller will overwrite completely; stale cells inside one are not refreshed
static void refresh(Surface* s, int x0, int y0, int x1, int y1, const std::vector<PixRect>* skip = nullptr) {
    if (!s || s->ndirty == 0) return;
    int cx0 = std::max(0, x0 / s->cbw), cx1 = std::min(s->cw, (x1 + s->cbw - 1) / s->cbw);
    int cy0 = std::max(0, y0 / s->cbh), cy1 = std::min(s->ch, (y1 + s->cbh - 1) / s->cbh);
    if (cx1 <= cx0 || cy1 <= cy0) return;
    if (skip && !skip->empty()) {
        for (int cy = cy0; cy < cy1; cy++)
            for (int cx = cx0; cx < cx1; cx++) {
                int ci = cy * s->cw + cx;
                if (!s->dirty[ci]) continue;
                int px = cx * s->cbw, py = cy * s->cbh;
                for (const PixRect& r : *skip)
                    if (px >= r.x0 && py >= r.y0 && px + s->cbw <= r.x1 && py + s->cbh <= r.y1) {
                        s->dirty[ci] = 0;
                        s->ndirty--;
                        st_skip_cells++;
                        break;
                    }
            }
    }
    // Cells whose newest data is in another surface of the same pixel size: blocks keep their
    // internal pixel arrangement in every layout of that size, so the S x S cells are copied on
    // the GPU (runs of consecutive cells merged).
    unbind_draw();
    std::vector<u32> need;
    for (int cy = cy0; cy < cy1; cy++) {
        Surface* ro = nullptr;
        int rsx = 0, rsy = 0, rdx = 0, rn = 0;
        auto emit = [&]() {
            if (!rn) return;
            D3D11_BOX box{(UINT)(rsx * S), (UINT)(rsy * S), 0, (UINT)((rsx + rn * s->cbw) * S), (UINT)((rsy + s->cbh) * S), 1};
            ctx->CopySubresourceRegion(s->tex, 0, rdx * S, cy * s->cbh * S, 0, ro->tex, 0, &box);
            rn = 0;
        };
        for (int cx = cx0; cx < cx1; cx++) {
            int ci = cy * s->cw + cx;
            if (!s->dirty[ci]) { emit(); continue; }
            u32 b = s->cell_block[ci];
            if (g_shadow_ok[b]) { emit(); continue; }
            Surface* o = surf_by_id(g_owner[b]);
            int oc = o ? o->block_cell[b] : -1;
            if (!o || o == s || oc < 0 || is16(o->lay) != is16(s->lay)) {
                emit();
                need.push_back(b);
                continue;
            }
            int sx = (oc % o->cw) * o->cbw, sy = (oc / o->cw) * o->cbh;
            if (rn && ro == o && sy == rsy && sx == rsx + rn * s->cbw) rn++;
            else {
                emit();
                ro = o; rsx = sx; rsy = sy; rdx = cx * s->cbw; rn = 1;
            }
            s->dirty[ci] = 0;
            s->ndirty--;
            st_gpu_copy_cells++;
        }
        emit();
    }
    if (!need.empty()) {
        static char why[96];
        snprintf(why, sizeof(why), "refresh %05x/%u lay %d", s->bp, s->bw, (int)s->lay);
        g_why = why;
        make_shadow_current(need);
    }
    // upload the stale cells row by row (runs of consecutive cells) and upscale them
    static std::vector<u32> buf;
    for (int cy = cy0; cy < cy1; cy++) {
        int cx = cx0;
        while (cx < cx1) {
            if (!s->dirty[cy * s->cw + cx]) { cx++; continue; }
            int run0 = cx;
            while (cx < cx1 && s->dirty[cy * s->cw + cx]) cx++;
            int px0 = run0 * s->cbw, px1 = cx * s->cbw, py0 = cy * s->cbh, py1 = py0 + s->cbh;
            int rw = px1 - px0;
            buf.resize((size_t)rw * s->cbh);
            for (int y = py0; y < py1; y++)
                for (int x = px0; x < px1; x++) buf[(size_t)(y - py0) * rw + (x - px0)] = shadow_read_raw(s->lay, x, y, s->bp, s->bw);
            D3D11_BOX box{(UINT)px0, (UINT)py0, 0, (UINT)px1, (UINT)py1, 1};
            ctx->UpdateSubresource(g_native, 0, &box, buf.data(), rw * 4, 0);
            dispatch_util(g_cs_up, g_native_srv, s->uav, px0 * S, py0 * S, px1 * S, py1 * S);
            for (int c = run0; c < cx; c++) {
                s->dirty[cy * s->cw + c] = 0;
                s->ndirty--;
                st_refresh_cells++;
            }
        }
    }
}

// Ownership after the GPU wrote cells of s.
static void mark_written(Surface* s, const std::vector<u8>& written, int cx0, int cy0, int cx1, int cy1) {
    for (int cy = cy0; cy < cy1; cy++)
        for (int cx = cx0; cx < cx1; cx++) {
            int ci = cy * s->cw + cx;
            if (!written[ci]) continue;
            u32 b = s->cell_block[ci];
            stamp_block(b);
            if (s->dirty[ci]) { s->dirty[ci] = 0; s->ndirty--; }
            if (g_owner[b] == s->id && !g_shadow_ok[b]) continue;  // nobody refreshed from it since
            g_owner[b] = (s16)s->id;
            set_shadow_ok(b, false);
            for (Surface* t : g_surf) {
                if (!t || t == s) continue;
                int tc = t->block_cell[b];
                if (tc >= 0 && !t->dirty[tc]) { t->dirty[tc] = 1; t->ndirty++; }
            }
        }
}

// The CPU (shadow copy) wrote these blocks.
static void mark_cpu_written(u32 b) {
    stamp_block(b);
    g_owner[b] = -1;
    set_shadow_ok(b, true);
    for (Surface* t : g_surf) {
        if (!t) continue;
        int tc = t->block_cell[b];
        if (tc >= 0 && !t->dirty[tc]) { t->dirty[tc] = 1; t->ndirty++; }
    }
}

static std::vector<u32> same_family(const std::vector<u32>& blocks, u32 psm) {
    std::vector<u32> r;
    int fam = psm_family(psm);
    for (u32 b : blocks) {
        if (g_shadow_ok[b]) continue;
        Surface* o = surf_by_id(g_owner[b]);
        if (o && (is16(o->lay) ? 16 : 32) == fam) r.push_back(b);
    }
    return r;
}

static Surface* find_surface(u32 bp, u32 bw, Layout lay) {
    bw = std::max<u32>(bw, 1);
    for (Surface* s : g_surf)
        if (s && s->bp == bp && s->bw == bw && s->lay == lay) return s;
    return nullptr;
}
static Surface* get_surface(u32 bp, u32 bw, Layout lay, int h) {
    Surface* s = find_surface(bp, bw, lay);
    if (!s) s = create_surface(bp, bw, lay, h);
    else if (h > s->h) grow_surface(s, h);
    if (s) s->last_use = g_frame;
    return s;
}

// ---------------------------------------------------------------------------
// Decoded textures (from the shadow copy)
// ---------------------------------------------------------------------------
struct TexEntry {
    u32 tbp, tbw, tpsm, tw, th;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    std::vector<u16> blocks;   // sorted, unique
    std::vector<u16> pages;    // pages of those blocks
    u64 stamp = 0;
    u64 hash = 0;              // raw memory content when decoded
    bool decoded = false;
    u64 last_use = 0;
    // HD textures (only kept while dumping or with a texture pack)
    std::vector<u8> data;      // decoded texels (indices or colors)
    u64 data_hash = 0;
    u64 rep_inputs = ~0ull;    // palette/texture state the key below was computed for
    u64 rep_key = 0;
    u64 novel_frame = 0;       // last frame its content was new (movies: new every frame)
    int novel_run = 0;
};
static u64 st_tex_rehash = 0, st_tex_replaced = 0, st_tex_dumped = 0;

// content of the blocks in the shadow copy (detects re-uploads of identical data)
static u64 blocks_hash(const std::vector<u16>& blocks) {
    u64 h = 0x9E3779B97F4A7C15ull;
    for (u16 b : blocks) {
        const u64* p = (const u64*)(shadow + (size_t)b * 64);
        for (int i = 0; i < 32; i++) h = (h ^ p[i]) * 0x100000001B3ull + (h >> 29);
    }
    return h;
}

// Texture decode block by block (one address computation per block, column tables inside).
static void decode_blocks(u32 tpsm, u32 tbp, u32 tbw, u32 tw, u32 th, u8* out, int bpp) {
    int cw, ch;
    psm_block_dims(tpsm, cw, ch);
    const u8* m8 = (const u8*)shadow;
    const u16* m16 = (const u16*)shadow;
    for (u32 by = 0; by < th; by += ch)
        for (u32 bx = 0; bx < tw; bx += cw) {
            u32 blk = psm_block(tpsm, (int)bx, (int)by, tbp, tbw);
            u32 yn = std::min<u32>(ch, th - by), xn = std::min<u32>(cw, tw - bx);
            for (u32 yy = 0; yy < yn; yy++) {
                size_t row = (size_t)(by + yy) * tw + bx;
                switch (tpsm) {
                case PSMT8: {
                    const u8* src = m8 + ((size_t)blk << 8);
                    for (u32 xx = 0; xx < xn; xx++) out[row + xx] = src[columnTable8[yy][xx]];
                    break;
                }
                case PSMT4: {
                    u32 base = blk << 9;
                    for (u32 xx = 0; xx < xn; xx++) {
                        u32 a = base + columnTable4[yy][xx];
                        out[row + xx] = (m8[a >> 1] >> ((a & 1) * 4)) & 0xF;
                    }
                    break;
                }
                case PSMCT32: case PSMCT24: case PSMZ32: case PSMZ24: {
                    const u32* src = shadow + ((size_t)blk << 6);
                    for (u32 xx = 0; xx < xn; xx++) ((u32*)out)[row + xx] = src[columnTable32[yy][xx]];
                    break;
                }
                case PSMT8H: case PSMT4HL: case PSMT4HH: {
                    const u32* src = shadow + ((size_t)blk << 6);
                    int sh = tpsm == PSMT4HH ? 28 : 24;
                    u32 mask = tpsm == PSMT8H ? 0xFF : 0xF;
                    for (u32 xx = 0; xx < xn; xx++) out[row + xx] = (u8)((src[columnTable32[yy][xx]] >> sh) & mask);
                    break;
                }
                default: {  // 16-bit formats
                    const u16* src = m16 + ((size_t)blk << 7);
                    for (u32 xx = 0; xx < xn; xx++) ((u16*)out)[row + xx] = src[columnTable16[yy][xx]];
                    break;
                }
                }
            }
        }
}
static std::unordered_map<u64, TexEntry> g_tex;

static u64 tex_key(u32 tbp, u32 tbw, u32 tpsm, u32 tw, u32 th) {
    return (u64)tbp | ((u64)tbw << 14) | ((u64)tpsm << 20) | ((u64)tw << 26) | ((u64)th << 38);
}

static TexEntry* decoded_texture(u32 tbp, u32 tbw, u32 tpsm, u32 tw, u32 th) {
    u64 key = tex_key(tbp, tbw, tpsm, tw, th);
    TexEntry& e = g_tex[key];
    if (e.blocks.empty()) {
        e.tbp = tbp; e.tbw = tbw; e.tpsm = tpsm; e.tw = tw; e.th = th;
        for_rect_blocks(tpsm, tbp, tbw, 0, 0, (int)tw, (int)th, [&](u32 b, bool) { e.blocks.push_back((u16)b); });
        std::sort(e.blocks.begin(), e.blocks.end());
        e.blocks.erase(std::unique(e.blocks.begin(), e.blocks.end()), e.blocks.end());
        for (u16 b : e.blocks)
            if (e.pages.empty() || e.pages.back() != (b >> 5)) e.pages.push_back(b >> 5);
    }
    e.last_use = g_frame;
    if (e.decoded) {
        // nothing written to its pages since the decode?
        bool ok = true;
        for (u16 p : e.pages)
            if (g_page_stamp[p] > e.stamp) { ok = false; break; }
        if (!ok) {
            ok = true;
            for (u16 b : e.blocks)
                if (g_stamp[b] > e.stamp) { ok = false; break; }
        }
        bool all_ok = true;
        if (!ok)
            for (u16 p : e.pages)
                if (g_page_stale[p]) { all_ok = false; break; }
        if (!ok && all_ok && blocks_hash(e.blocks) == e.hash) {
            // rewritten with the same data (the game re-uploads textures every frame)
            ok = true;
            st_tex_rehash++;
        }
        if (ok) {
            e.stamp = g_stamp_next - 1;
            st_tex_hits++;
            return &e;
        }
    }
    for (u16 b : e.blocks)
        if (!g_shadow_ok[b]) st_stale_texels++;
    DXGI_FORMAT f;
    int bpp;
    switch (tpsm) {
    case PSMT8: case PSMT4: case PSMT8H: case PSMT4HL: case PSMT4HH: f = DXGI_FORMAT_R8_UINT; bpp = 1; break;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: f = DXGI_FORMAT_R16_UINT; bpp = 2; break;
    default: f = DXGI_FORMAT_R32_UINT; bpp = 4; break;
    }
    if (!e.tex) {
        make_tex2d((int)tw, (int)th, f, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, &e.tex);
        dev->CreateShaderResourceView(e.tex, nullptr, &e.srv);
    }
    static std::vector<u8> buf;
    buf.resize((size_t)tw * th * bpp);
    decode_blocks(tpsm, tbp, tbw, tw, th, buf.data(), bpp);
    ctx->UpdateSubresource(e.tex, 0, nullptr, buf.data(), tw * bpp, 0);
    if (g_texrep_on) {
        e.data = buf;
        e.data_hash = texrep::hash(buf.data(), buf.size(), tpsm | tw << 8 | (u64)th << 24);
        e.rep_inputs = ~0ull;
    }
    e.stamp = g_stamp_next - 1;
    e.hash = blocks_hash(e.blocks);
    e.decoded = true;
    st_tex_decodes++;
    return &e;
}

static void evict_textures() {
    if (g_tex.size() < 768) return;
    std::vector<std::pair<u64, u64>> v;
    for (auto& [k, e] : g_tex) v.push_back({e.last_use, k});
    std::sort(v.begin(), v.end());
    for (size_t i = 0; i < v.size() / 4; i++) {
        TexEntry& e = g_tex[v[i].second];
        release(e.srv);
        release(e.tex);
        g_tex.erase(v[i].second);
    }
}

// ---------------------------------------------------------------------------
// Batches
// ---------------------------------------------------------------------------
struct GV {
    float x, y, z;
    float r, g, b, a;
    float s, t, q;
    float fog;
    float fr, fg, fb, fa;
};
static_assert(sizeof(GV) == 60, "vertex layout");

static std::vector<GV> g_verts;
static DrawState g_bds;           // state of the current batch
static u32 g_bid = ~0u;           // its state id
static bool g_bvalid = false;
static int g_bx0, g_by0, g_bx1, g_by1;    // destination bounds (native pixels, clipped)
static float g_bu0, g_bv0, g_bu1, g_bv1;  // texture coordinate bounds (texels)
static bool g_bself = false;      // one primitive per batch (texture overlaps the target)
static bool g_cfg_mips = true;    // use the game's mip levels
static bool g_bhud = false;       // batch of widescreen-corrected 2D elements
static bool g_ball_sprites = true;            // batch consists of sprites only
static std::vector<PixRect> g_bsprites;       // their exact pixel rectangles
static int g_bprims = 0;
// batch break diagnostics (stats line): 0 self, 1 hud, 2 same state, 3.. changed field groups, 14 clut, 15 all flushes
static u64 st_why[16];
static DrawState g_prev_raw;
static u32 g_clut[512];
static bool g_clut_valid = false;

static int tex_format_code(u32 tpsm, bool decoded) {
    switch (tpsm) {
    case PSMCT32: case PSMZ32: return 0;
    case PSMCT24: return 1;
    case PSMZ24: return 8;
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: return 2;
    case PSMT8: return 3;
    case PSMT4: return 4;
    case PSMT8H: return decoded ? 3 : 5;
    case PSMT4HL: return decoded ? 4 : 6;
    case PSMT4HH: return decoded ? 4 : 7;
    }
    return 0;
}

struct DrawCB {
    float vp[4];
    u32 f0[4], f1[4], f2[4], f3[4], f4[4], f5[4], f6[4], f7[4], f8[4], f9[4], f10[4];
    float f11[4];
    u32 f12[4];
    s32 f13[4];
    u32 f14[4];
    float f15[4];
    s32 dimx[16];
};

static void flush();

// GS memory blocks [lo, hi) of the page rows holding rows y0..y1 of a buffer (hi may pass the end of
// memory, which wraps)
static void row_blocks(u32 bp, u32 bw, u32 psm, int y0, int y1, u32& lo, u32& hi) {
    int ph = 32, pw = 64;
    switch (psm) {
    case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: ph = 64; break;
    case PSMT8: case PSMT8H: ph = 64; pw = 128; break;
    case PSMT4: case PSMT4HL: case PSMT4HH: ph = 128; pw = 128; break;
    }
    if (psm == PSMT8H || psm == PSMT4HL || psm == PSMT4HH) { ph = 32; pw = 64; }  // stored in 32-bit pixels
    u32 ppr = std::max<u32>(1, (bw * 64 + pw - 1) / pw);
    y0 = std::max(y0, 0);
    y1 = std::max(y1, y0 + 1);
    lo = bp + (u32)(y0 / ph) * ppr * 32;
    hi = bp + (u32)((y1 + ph - 1) / ph) * ppr * 32;
}
// Does a primitive of this state sample memory that its own batch can write (color or depth inside
// the scissor rows)? Only then must the primitives be drawn one at a time. The check is by memory, so
// a texture stored next to a buffer (e.g. right after a z buffer whose surface was made taller) is no
// feedback.
static bool samples_own_writes(const DrawState& ds) {
    int th = 1 << ds.th, t0 = 0, t1 = th;
    if (ds.wmt == 2) { t0 = std::min<int>(ds.minv, th - 1); t1 = std::min<int>(ds.maxv + 1, th); }
    u32 tl, thi;
    row_blocks(ds.tbp, ds.tbw, ds.tpsm, t0, std::max(t1, t0 + 1), tl, thi);
    auto hit = [&](u32 bp, u32 psm) {
        u32 l, h;
        row_blocks(bp, ds.fbw, psm, ds.scay0, ds.scay1 + 1, l, h);
        // compare in both orders of the 16384-block wrap
        for (int k = -1; k <= 1; k++) {
            s64 o = (s64)k * 16384;
            if ((s64)tl < (s64)h + o && (s64)l + o < (s64)thi) return true;
        }
        return false;
    };
    if (ds.fbmsk != 0xFFFFFFFFu && hit(ds.fbp, ds.fpsm)) return true;
    return ds.zte && !ds.zmsk && ds.ztst != 0 && hit(ds.zbp, ds.zpsm);
}

// HD replacement of a decoded texture: 32-bit texels at k times the size, own mip levels
struct Rep {
    ID3D11Texture2D* tex = nullptr;          // RGBA8 with a full mip chain
    ID3D11ShaderResourceView* srv = nullptr;
    int k = 0;
    bool none = false;   // no usable replacement for the key
    u64 last_use = 0;
    size_t bytes = 0;
};
static std::unordered_map<u64, Rep> g_rep;
static size_t g_rep_bytes = 0;

// least recently used HD textures leave video memory when over budget (reloaded when needed)
static void evict_reps() {
    if (g_rep_bytes <= REP_BUDGET) return;
    std::vector<std::pair<u64, u64>> v;
    for (auto& [k, r] : g_rep)
        if (!r.none && r.last_use + 2 < g_frame) v.push_back({r.last_use, k});
    std::sort(v.begin(), v.end());
    for (auto& [lu, k] : v) {
        if (g_rep_bytes <= REP_BUDGET * 3 / 4) break;
        Rep& r = g_rep[k];
        release(r.srv);
        release(r.tex);
        g_rep_bytes -= r.bytes;
        g_rep.erase(k);
    }
}
static u64 g_clut_version = 1;

// Texture source of the batch: a surface drawn in the same layout, or a decoded copy.
struct TexBind {
    ID3D11ShaderResourceView* srv = nullptr;
    float tscale = 1;
    int ox = 0, oy = 0;
    int tfmt = 0;
    Surface* surf = nullptr;
    Rep* rep = nullptr;
};

static inline u32 cpu_expand16(u32 c, const DrawState& ds) {
    u32 a = (c & 0x8000) ? ds.ta1 : ((ds.aem && (c & 0x7FFF) == 0) ? 0 : ds.ta0);
    return ((c & 0x1F) << 3) | (((c >> 5) & 0x1F) << 11) | (((c >> 10) & 0x1F) << 19) | (a << 24);
}

// texels of a decoded texture as 32-bit colors (the shader's texel())
static void cpu_colors(const DrawState& ds, const TexEntry* e, int tfmt, std::vector<u32>& out) {
    size_t n = (size_t)e->tw * e->th;
    out.resize(n);
    bool c16 = ds.cpsm == PSMCT16 || ds.cpsm == PSMCT16S;
    const u8* d8 = e->data.data();
    const u16* d16 = (const u16*)e->data.data();
    const u32* d32 = (const u32*)e->data.data();
    for (size_t i = 0; i < n; i++) {
        u32 c;
        switch (tfmt) {
        case 0: c = d32[i]; break;
        case 1: { u32 v = d32[i] & 0xFFFFFF; c = v | ((ds.aem && v == 0) ? 0 : ds.ta0) << 24; break; }
        case 2: c = cpu_expand16(d16[i], ds); break;
        case 3: c = c16 ? cpu_expand16(g_clut[(d8[i] + ds.csa * 16) & 511] & 0xFFFF, ds) : g_clut[d8[i]]; break;
        case 4: c = c16 ? cpu_expand16(g_clut[((d8[i] & 15) + ds.csa * 16) & 511] & 0xFFFF, ds) : g_clut[((d8[i] & 15) + (ds.csa & 15) * 16) & 255]; break;
        default: c = (d32[i] & 0xFFFFFF) | ds.ta0 << 24; break;
        }
        out[i] = c;
    }
}

// key of texel data + the palette entries / alpha expansion it is shown with
static u64 rep_key(const DrawState& ds, const TexEntry* e, int tfmt) {
    bool c16 = ds.cpsm == PSMCT16 || ds.cpsm == PSMCT16S;
    bool pal = tfmt == 3 || tfmt == 4;
    u64 k = e->data_hash;
    if (pal) {
        u32 n = tfmt == 3 ? 256 : 16, ent[256];
        for (u32 i = 0; i < n; i++)
            ent[i] = c16 ? (g_clut[(i + ds.csa * 16) & 511] & 0xFFFF) : (tfmt == 4 ? g_clut[(i + (ds.csa & 15) * 16) & 255] : g_clut[i]);
        k = texrep::hash(ent, n * 4, k ^ (c16 ? 0x16 : 0x32));
    }
    if ((pal && c16) || tfmt == 1 || tfmt == 2 || tfmt == 8) {
        u32 t[3] = {ds.ta0, ds.ta1, (u32)ds.aem};
        k = texrep::hash(t, sizeof t, k);
    }
    return k;
}

static void create_rep(Rep& r, const texrep::Image& img, u32 tw, u32 th, u64 key) {
    int k = img.w / (int)tw;
    if (k < 1 || k > 8 || img.w != k * (int)tw || img.h != k * (int)th) {
        log_write(LOG_GS, "HD texture %016llx: %dx%d is no integer multiple of %ux%u, ignored", (unsigned long long)key, img.w, img.h, tw, th);
        r.none = true;
        return;
    }
    // GS texel layout R | G << 8 | B << 16 | A << 24 is RGBA8 in memory; mips by the GPU
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)img.w;
    td.Height = (UINT)img.h;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    if (FAILED(dev->CreateTexture2D(&td, nullptr, &r.tex)) || FAILED(dev->CreateShaderResourceView(r.tex, nullptr, &r.srv))) {
        release(r.tex);
        r.none = true;
        return;
    }
    ctx->UpdateSubresource(r.tex, 0, nullptr, img.px.data(), img.w * 4, 0);
    ctx->GenerateMips(r.srv);
    r.k = k;
    r.bytes = (size_t)img.w * img.h * 4 * 4 / 3;
    g_rep_bytes += r.bytes;
}

// dump / replace a decoded texture (not for memory the GPU has drawn: render targets change)
static void apply_texrep(const DrawState& ds, TexEntry* e, TexBind& tb) {
    bool pal = tb.tfmt == 3 || tb.tfmt == 4;
    u64 in = e->data_hash * 0x9E3779B97F4A7C15ull ^ (pal ? g_clut_version : 0) * 0xC2B2AE3D27D4EB4Full ^
             ((u64)ds.csa | (u64)ds.cpsm << 8 | (u64)ds.ta0 << 16 | (u64)ds.ta1 << 24 | (u64)ds.aem << 32);
    if (in != e->rep_inputs) {
        e->rep_inputs = in;
        e->rep_key = rep_key(ds, e, tb.tfmt);
        if (texrep::dumping()) {
            if (!texrep::dumped(e->rep_key)) {
                // movies and other per-frame content: new texels every frame, not worth dumping
                if (e->novel_frame != g_frame) {
                    e->novel_run = (g_frame - e->novel_frame <= 2) ? e->novel_run + 1 : 1;
                    e->novel_frame = g_frame;
                }
                if (e->novel_run < 8) {
                    std::vector<u32> px;
                    cpu_colors(ds, e, tb.tfmt, px);
                    char info[160];
                    snprintf(info, sizeof info, "psm=%02x tbp=%04x tbw=%u cpsm=%02x csa=%u frame=%llu", ds.tpsm, ds.tbp, ds.tbw, ds.cpsm, ds.csa,
                             (unsigned long long)g_frame);
                    // raw texels + the 256 palette entries as the shader indexes them (CSA applied)
                    std::vector<u8> raw(e->data);
                    if (tb.tfmt == 3 || tb.tfmt == 4) {
                        bool c16 = ds.cpsm == PSMCT16 || ds.cpsm == PSMCT16S;
                        u32 ent[256];
                        for (u32 i = 0; i < 256; i++)
                            ent[i] = c16 ? (g_clut[(i + ds.csa * 16) & 511] & 0xFFFF) : (tb.tfmt == 4 ? g_clut[((i & 15) + (ds.csa & 15) * 16) & 255] : g_clut[i]);
                        raw.insert(raw.end(), (u8*)ent, (u8*)ent + sizeof ent);
                    }
                    texrep::dump(e->rep_key, (int)e->tw, (int)e->th, std::move(px), info, std::move(raw));
                    st_tex_dumped++;
                }
            } else e->novel_run = 0;
        }
    }
    if (!texrep::have_pack() || (!platform::config().hd_textures && !texrep::translated(e->rep_key))) return;
    auto it = g_rep.find(e->rep_key);
    if (it == g_rep.end()) {
        const texrep::Image* img = nullptr;
        texrep::State st = texrep::lookup(e->rep_key, &img);
        if (st == texrep::LOADING) return;
        Rep& r = g_rep[e->rep_key];
        if (st == texrep::NONE) r.none = true;
        else {
            create_rep(r, *img, e->tw, e->th, e->rep_key);
            texrep::release(e->rep_key);
        }
        it = g_rep.find(e->rep_key);
    }
    Rep& r = it->second;
    if (r.none) return;
    r.last_use = g_frame;
    tb.srv = r.srv;
    tb.tscale = (float)r.k;
    tb.tfmt = 0;
    tb.rep = &r;
    st_tex_replaced++;
}

static TexBind resolve_texture(const DrawState& ds, Surface* fs, Surface* zs) {
    TexBind tb;
    int tw = 1 << ds.tw, th = 1 << ds.th;
    // texel rows/columns this batch can touch
    int u0 = 0, u1 = tw, v0 = 0, v1 = th;
    if (ds.wms != 0 && g_bu0 > -1e6f && g_bu1 < 1e6f) { u0 = std::max(0, (int)floorf(g_bu0) - 1); u1 = std::min(tw, (int)ceilf(g_bu1) + 2); }
    if (ds.wmt != 0 && g_bv0 > -1e6f && g_bv1 < 1e6f) { v0 = std::max(0, (int)floorf(g_bv0) - 1); v1 = std::min(th, (int)ceilf(g_bv1) + 2); }
    if (ds.wms == 2) { u0 = std::max(u0, (int)ds.minu); u1 = std::min(u1, (int)ds.maxu + 1); }
    if (ds.wmt == 2) { v0 = std::max(v0, (int)ds.minv); v1 = std::min(v1, (int)ds.maxv + 1); }
    if (u1 <= u0) { u0 = 0; u1 = tw; }
    if (v1 <= v0) { v0 = 0; v1 = th; }
    // memory drawn by the GPU only matters if a page of the texture has such blocks
    TexEntry* geo = nullptr;
    {
        u64 key = tex_key(ds.tbp, ds.tbw, ds.tpsm, (u32)tw, (u32)th);
        auto it = g_tex.find(key);
        if (it != g_tex.end() && !it->second.pages.empty()) geo = &it->second;
    }
    bool maybe_gpu = true;
    if (geo) {
        maybe_gpu = false;
        for (u16 p : geo->pages)
            if (g_page_stale[p]) { maybe_gpu = true; break; }
    }
    std::vector<u32> blocks;
    if (maybe_gpu) for_rect_blocks(ds.tpsm, ds.tbp, ds.tbw, u0, v0, u1, v1, [&](u32 b, bool) { blocks.push_back(b); });
    // drawn memory in a compatible layout: sample the surface
    Layout tl = layout_of(ds.tpsm);
    bool gpu_owned = false;
    for (u32 b : blocks)
        if (!g_shadow_ok[b]) { gpu_owned = true; break; }
    if (tl != L_NONE && gpu_owned) {
        Surface* best = nullptr;
        int ox = 0, oy = 0;
        for (Surface* s : g_surf) {
            if (!s || s->lay != tl || s->bw != std::max<u32>(ds.tbw, 1)) continue;
            if (ds.tbp < s->bp) continue;
            u32 d = ds.tbp - s->bp;
            if (d % 32) continue;  // page aligned offsets only
            u32 page = d / 32;
            int ph = is16(tl) ? 64 : 32;
            int px = (int)(page % s->bw) * 64, py = (int)(page / s->bw) * ph;
            if (py >= s->h) continue;
            // prefer the surface that owns the first sampled block
            if (!best || g_owner[blocks[0]] == s->id) { best = s; ox = px; oy = py; }
        }
        if (best) {
            refresh(best, ox + u0, oy + v0, ox + u1, oy + v1);
            tb.srv = best->srv;
            tb.tscale = (float)S;
            tb.ox = ox;
            tb.oy = oy;
            tb.tfmt = tex_format_code(ds.tpsm, false);
            tb.surf = best;
            return tb;
        }
    }
    if (gpu_owned) { g_why = "texture decode"; make_shadow_current(blocks); }
    TexEntry* e = decoded_texture(ds.tbp, ds.tbw, ds.tpsm, (u32)tw, (u32)th);
    tb.srv = e->srv;
    tb.tscale = 1;
    tb.tfmt = tex_format_code(ds.tpsm, true);
    if (g_texrep_on && !gpu_owned && !e->data.empty()) apply_texrep(ds, e, tb);
    return tb;
}

static void bind_snapshot(Surface* s, TexBind& tb) {
    if (!g_snap_tex || g_snap_w < s->w * S || g_snap_h < s->h * S) {
        release(g_snap_srv);
        release(g_snap_tex);
        g_snap_w = std::max(g_snap_w, s->w * S);
        g_snap_h = std::max(g_snap_h, s->h * S);
        make_tex2d(g_snap_w, g_snap_h, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, &g_snap_tex);
        dev->CreateShaderResourceView(g_snap_tex, nullptr, &g_snap_srv);
    }
    unbind_draw();
    D3D11_BOX box{0, 0, 0, (UINT)(s->w * S), (UINT)(s->h * S), 1};
    ctx->CopySubresourceRegion(g_snap_tex, 0, 0, 0, 0, s->tex, 0, &box);
    tb.srv = g_snap_srv;
    st_self++;
}

static void flush() {
    if (!g_bvalid || g_verts.empty()) {
        g_verts.clear();
        g_bprims = 0;
        return;
    }
    st_why[15]++;
    const DrawState& ds = g_bds;
    Layout fl = layout_of(ds.fpsm);
    if (fl == L_NONE || fl == L_Z32 || fl == L_Z16 || fl == L_Z16S) {
        // drawing into a z format as color: same layouts as z buffers
    }
    if (fl == L_NONE) { g_verts.clear(); g_bprims = 0; return; }
    int need_h = std::min(2048, std::max(ds.scay1 + 1, g_by1));
    Surface* fs = get_surface(ds.fbp, ds.fbw, fl, need_h);
    Surface* zs = nullptr;
    bool zuse = ds.zte && (ds.ztst != 1 || !ds.zmsk);
    if (zuse) {
        zs = get_surface(ds.zbp, ds.fbw, layout_of(ds.zpsm), fs->h);
        if (zs && zs->h < fs->h) grow_surface(zs, fs->h);
    }
    if (!fs) { g_verts.clear(); g_bprims = 0; return; }
    if (zs && zs->h < fs->h) zs = nullptr;
    TexBind tb;
    if (ds.tme) tb = resolve_texture(ds, fs, zs);
    // target content inside the batch bounds must be current, except where sprites overwrite
    // everything without reading it (clears, full-screen copies)
    bool blend_reads_dst = ds.abe && (ds.ba == 1 || ds.bb == 1 || ds.bd == 1 || ds.bc == 1) &&
                           !(ds.ba == 0 && ds.bb == 1 && ds.bc == 2 && ds.bfix == 0x80 && ds.bd == 1);
    bool color_overwrites = g_ball_sprites && ds.fbmsk == 0 && ds.fpsm != PSMCT24 && !ds.date && (!ds.ate || ds.atst == 1) &&
                            !blend_reads_dst;
    bool z_overwrites = g_ball_sprites && ds.zte && ds.ztst == 1 && !ds.zmsk;
    refresh(fs, g_bx0, g_by0, g_bx1, g_by1, color_overwrites ? &g_bsprites : nullptr);
    if (zs) refresh(zs, g_bx0, g_by0, g_bx1, g_by1, z_overwrites ? &g_bsprites : nullptr);
    if (ds.tme && tb.surf && (tb.surf == fs || tb.surf == zs)) bind_snapshot(tb.surf, tb);
    int nmips = 0;
    ID3D11ShaderResourceView* mip_srv[6] = {};
    if (ds.tme && ds.mxl > 0 && ds.mmin >= 2 && ds.mmin <= 5 && !ds.mtba && !tb.surf && !tb.rep && g_cfg_mips) {
        nmips = std::min<int>(ds.mxl, 6);
        for (int l = 1; l <= nmips; l++) {
            u64 m = l <= 3 ? ds.miptbp1 : ds.miptbp2;
            int k = (l - 1) % 3;
            u32 tbp = bits(m, k * 20, 14), tbw = bits(m, k * 20 + 14, 6);
            u32 w = std::max(1u, (1u << ds.tw) >> l), h = std::max(1u, (1u << ds.th) >> l);
            TexEntry* e = decoded_texture(tbp, tbw, ds.tpsm, w, h);
            mip_srv[l - 1] = e->srv;
        }
    }

    // constants
    DrawCB c{};
    if (tb.rep) { c.f15[0] = 1.0f; c.f15[1] = (float)tb.rep->k; }
    // text: 4 bit glyph caches with a palette of one color and an alpha ramp get sharp contours
    if (ds.tme && S > 1 && tb.tfmt == 4 && !tb.surf && !tb.rep && platform::config().sharp_text &&
        ds.cpsm != PSMCT16 && ds.cpsm != PSMCT16S) {
        const u32* pal = &g_clut[(ds.csa & 15) * 16];
        u32 amin = 255, amax = 0;
        bool one_color = true;
        for (int i = 0; i < 16; i++) {
            if ((pal[i] & 0xFFFFFF) != (pal[0] & 0xFFFFFF)) one_color = false;
            amin = std::min(amin, pal[i] >> 24);
            amax = std::max(amax, pal[i] >> 24);
        }
        if (one_color && amin == 0 && amax >= 0x40) { c.f15[2] = 1.0f; c.f15[3] = (float)amax; }
    }
    c.vp[0] = (float)fs->w; c.vp[1] = (float)fs->h; c.vp[2] = (float)S;
    u32 fpk = (ds.fpsm == PSMCT24 || ds.fpsm == PSMZ24) ? 1 : (is16(fl) ? 2 : 0);
    c.f0[0] = fpk; c.f0[1] = ds.fbmsk; c.f0[2] = ds.fba; c.f0[3] = ds.colclamp;
    c.f1[0] = ds.dthe; c.f1[1] = ds.ate; c.f1[2] = ds.atst; c.f1[3] = ds.aref;
    c.f2[0] = ds.afail; c.f2[1] = ds.date; c.f2[2] = ds.datm; c.f2[3] = zs ? 1 : 0;
    u32 zf = ds.zpsm == PSMZ32 ? 0 : ds.zpsm == PSMZ24 ? 1 : 2;
    c.f3[0] = ds.ztst; c.f3[1] = ds.zmsk; c.f3[2] = zf; c.f3[3] = ds.abe;
    c.f4[0] = ds.ba; c.f4[1] = ds.bb; c.f4[2] = ds.bc; c.f4[3] = ds.bd;
    c.f5[0] = ds.bfix; c.f5[1] = ds.pabe; c.f5[2] = ds.iip; c.f5[3] = ds.tme;
    c.f6[0] = ds.fst; c.f6[1] = ds.tfx; c.f6[2] = ds.tcc; c.f6[3] = (u32)tb.tfmt;
    c.f7[0] = 1u << ds.tw; c.f7[1] = 1u << ds.th; c.f7[2] = ds.wms; c.f7[3] = ds.wmt;
    c.f8[0] = ds.minu; c.f8[1] = ds.maxu; c.f8[2] = ds.minv; c.f8[3] = ds.maxv;
    c.f9[0] = (ds.cpsm == PSMCT16 || ds.cpsm == PSMCT16S) ? 1 : 0; c.f9[1] = ds.csa; c.f9[2] = ds.ta0; c.f9[3] = ds.ta1;
    bool need_lod = ds.tme && !ds.fst && !ds.lcm && ds.mag_linear != ds.min_linear;
    c.f10[0] = ds.aem; c.f10[1] = ds.mag_linear; c.f10[2] = ds.min_linear; c.f10[3] = need_lod;
    // the game picks texture detail for 512x448: at S times the resolution the same texel density
    // is reached log2(S) levels finer (sharper mip levels, magnification filter where it was minified)
    float lodk = ds.lodk - (platform::config().lod_scale ? log2f((float)S) : 0.0f);
    c.f11[0] = lodk; c.f11[1] = need_lod ? exp2f(lodk / (float)(1 << ds.lodl)) : 0.0f; c.f11[2] = tb.tscale;
    c.f12[0] = ds.fge; c.f12[1] = ds.fogcol;
    c.f13[0] = tb.ox; c.f13[1] = tb.oy;
    c.f14[0] = (u32)nmips; c.f14[1] = ds.mmin; c.f14[2] = ds.lcm; c.f14[3] = ds.lodl;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) c.dimx[y * 4 + x] = ds.dimx[y][x];
    write_cb(g_cb, &c, sizeof(c));

    // vertices: appended to a ring buffer, discarded only when it wraps
    size_t bytes = g_verts.size() * sizeof(GV);
    if (bytes > VB_BYTES) bytes = VB_BYTES / sizeof(GV) / 3 * 3 * sizeof(GV);
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_vb_pos + bytes > VB_BYTES) {
        mode = D3D11_MAP_WRITE_DISCARD;
        g_vb_pos = 0;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(g_vb, 0, mode, 0, &m))) { g_verts.clear(); return; }
    memcpy((u8*)m.pData + g_vb_pos, g_verts.data(), bytes);
    ctx->Unmap(g_vb, 0);
    UINT first = g_vb_pos / sizeof(GV);
    g_vb_pos += (UINT)bytes;

    if (!g_bound.pipeline) {
        UINT stride = sizeof(GV), offset = 0;
        ctx->IASetVertexBuffers(0, 1, &g_vb, &stride, &offset);
        ctx->IASetInputLayout(g_il);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g_vs, nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, &g_cb);
        ctx->PSSetShader(g_ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &g_cb);
        ctx->RSSetState(g_rs);
        ctx->PSSetSamplers(0, 4, g_hd_samp);
        g_bound.pipeline = true;
        g_bound.hd = nullptr;
    }
    ID3D11UnorderedAccessView* uavs[2] = {fs->uav, zs ? zs->uav : g_dummy_uav};
    if (uavs[0] != g_bound.uav[0] || uavs[1] != g_bound.uav[1]) {
        ctx->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 2, uavs, nullptr);
        g_bound.uav[0] = uavs[0];
        g_bound.uav[1] = uavs[1];
    }
    ID3D11ShaderResourceView* srvs[2] = {tb.srv ? tb.srv : g_dummy_srv, g_clut_srv};
    if (srvs[0] != g_bound.srv[0] || srvs[1] != g_bound.srv[1]) {
        ctx->PSSetShaderResources(0, 2, srvs);
        g_bound.srv[0] = srvs[0];
        g_bound.srv[1] = srvs[1];
    }
    if (tb.rep && tb.rep->srv != g_bound.hd) {
        ctx->PSSetShaderResources(8, 1, &tb.rep->srv);
        g_bound.hd = tb.rep->srv;
    }
    if (nmips) {
        ID3D11ShaderResourceView* ms[6];
        for (int l = 0; l < 6; l++) ms[l] = l < nmips ? mip_srv[l] : g_dummy_srv;
        ctx->PSSetShaderResources(2, 6, ms);
    }
    float vpw = (float)(fs->w * S), vph = (float)(fs->h * S);
    if (vpw != g_bound.vpw || vph != g_bound.vph) {
        D3D11_VIEWPORT vp{0, 0, vpw, vph, 0, 1};
        ctx->RSSetViewports(1, &vp);
        g_bound.vpw = vpw;
        g_bound.vph = vph;
    }
    D3D11_RECT sc{ds.scax0 * S, ds.scay0 * S, (ds.scax1 + 1) * S, (ds.scay1 + 1) * S};
    if (memcmp(&sc, &g_bound.sc, sizeof(sc)) != 0) {
        ctx->RSSetScissorRects(1, &sc);
        g_bound.sc = sc;
    }
    uav_overlap(!tb.surf);  // a surface sampled as a texture must be complete
    if (!platform::skip_active()) ctx->Draw((UINT)(bytes / sizeof(GV)), first);  // skipping a cutscene: nothing to see
    st_batches++;

    // ownership of what was drawn (bounds of the batch, in cells)
    bool color_write = ds.fbmsk != 0xFFFFFFFFu;
    auto mark = [&](Surface* s) {
        int cx0 = std::max(0, g_bx0 / s->cbw), cx1 = std::min(s->cw, (g_bx1 + s->cbw - 1) / s->cbw);
        int cy0 = std::max(0, g_by0 / s->cbh), cy1 = std::min(s->ch, (g_by1 + s->cbh - 1) / s->cbh);
        if (cx1 <= cx0 || cy1 <= cy0) return;
        static std::vector<u8> w;
        w.assign(s->cell_block.size(), 0);
        for (int cy = cy0; cy < cy1; cy++)
            for (int cx = cx0; cx < cx1; cx++) w[cy * s->cw + cx] = 1;
        mark_written(s, w, cx0, cy0, cx1, cy1);
    };
    if (color_write) mark(fs);
    if (zs && ds.zte && !ds.zmsk && ds.ztst != 0) mark(zs);
    g_verts.clear();
    g_bprims = 0;
}

static inline void bounds_add(const DrawState& ds, int x0, int y0, int x1, int y1) {
    // pixel bounds [x0, x1) x [y0, y1), clipped to the scissor
    x0 = std::max(x0, ds.scax0); y0 = std::max(y0, ds.scay0);
    x1 = std::min(x1, ds.scax1 + 1); y1 = std::min(y1, ds.scay1 + 1);
    if (x1 <= x0 || y1 <= y0) return;
    g_bx0 = std::min(g_bx0, x0); g_by0 = std::min(g_by0, y0);
    g_bx1 = std::max(g_bx1, x1); g_by1 = std::max(g_by1, y1);
}

static inline void uv_add(const DrawState& ds, const Vertex& v) {
    float u, t;
    if (ds.fst) { u = v.u / 16.0f; t = v.v / 16.0f; }
    else {
        float q = v.q != 0.0f ? v.q : 1e-10f;
        u = v.s / q * (float)(1 << ds.tw);
        t = v.t / q * (float)(1 << ds.th);
    }
    if (!(u > -1e7f && u < 1e7f)) u = u > 0 ? 1e7f : -1e7f;
    if (!(t > -1e7f && t < 1e7f)) t = t > 0 ? 1e7f : -1e7f;
    g_bu0 = std::min(g_bu0, u); g_bu1 = std::max(g_bu1, u);
    g_bv0 = std::min(g_bv0, t); g_bv1 = std::max(g_bv1, t);
}

static inline GV mkv(const DrawState& ds, const Vertex& v, const Vertex& flat) {
    GV o;
    o.x = v.x / 16.0f;
    o.y = v.y / 16.0f;
    o.z = (float)v.z;
    o.r = v.r; o.g = v.g; o.b = v.b; o.a = v.a;
    if (ds.fst) { o.s = v.u / 16.0f; o.t = v.v / 16.0f; o.q = 1.0f; }
    else { o.s = v.s; o.t = v.t; o.q = v.q; }
    o.fog = v.f;
    o.fr = flat.r; o.fg = flat.g; o.fb = flat.b; o.fa = flat.a;
    return o;
}

// Widescreen HUD correction: 2D elements (all vertices at z = 0 without perspective, no depth test)
// drawn into a displayed frame buffer are narrowed to their 4:3 proportions around the screen center.
// Full-width layers (backgrounds, fades, screen filters) keep the full width.
static u64 st_hud = 0;
struct HudSpan { int x0 = 1 << 30, x1 = -(1 << 30), width = 0, y0 = 1 << 30, y1 = -(1 << 30), n = 0; };
static std::unordered_map<u32, HudSpan> g_hud_span;  // this frame: x range of the 2D pieces per texture
static std::unordered_set<u32> g_hud_fullwidth;       // textures whose pieces spanned the width last frame
static void hud_frame_end() {
    static int log_frames = 0;
    if (g_hud_log_at > 0 && host_time_sec() >= g_hud_log_at) { g_hud_log_at = host_time_sec() + 3.0; log_frames = 1; }
    if (log_frames > 0) {
        log_frames--;
        log_write(LOG_GS, "hud frame %llu: %zu textures", (unsigned long long)g_frame, g_hud_span.size());
        for (auto& [k, s] : g_hud_span)
            log_write(LOG_GS, "hud key %06x tbp %04x tw %u th %u%s n %d x %d..%d y %d..%d", k, k & 0x3FFF, (k >> 14) & 15, (k >> 18) & 15,
                      (k >> 22) & 1 ? " black" : "", s.n, s.x0 >> 4, s.x1 >> 4, s.y0 >> 4, s.y1 >> 4);
    }
    g_hud_fullwidth.clear();
    for (auto& [k, s] : g_hud_span)
        if (s.width > 0 && s.x0 <= 4 * 16 && s.x1 >= (s.width - 4) * 16) g_hud_fullwidth.insert(k);
    g_hud_span.clear();
}
static bool hud_transform(const DrawState& ds, int prim, const Vertex* v, Vertex* out) {
    const platform::Config& cfg = platform::config();
    if (!cfg.widescreen_hack || !cfg.hud_fix) return false;
    if (ds.fbp != g_display_fbp[0] && ds.fbp != g_display_fbp[1]) return false;
    // screen effects (blur, depth of field, dimming in battles) sample the rendered picture, often in
    // column strips: they keep the full width; menus and text use uploaded textures
    if (ds.tme && !g_shadow_ok[ds.tbp & 16383]) return false;
    int n = prim == 3 ? 3 : prim == 0 ? 1 : 2;
    if (ds.zte && ds.ztst != 1) return false;
    int minx = v[0].x, maxx = v[0].x, miny = v[0].y, maxy = v[0].y;
    for (int i = 0; i < n; i++) {
        if (v[i].z != 0) return false;
        if (ds.tme && !ds.fst && fabsf(v[i].q - 1.0f) > 1e-3f) return false;
        minx = std::min(minx, v[i].x);
        maxx = std::max(maxx, v[i].x);
        miny = std::min(miny, v[i].y);
        maxy = std::max(maxy, v[i].y);
    }
    int width = (int)ds.fbw * 64;
    if (maxx - minx >= (width - 24) * 16) return false;  // full-width layer
    // full-height strips: clears and backgrounds drawn in columns
    if (maxy - miny >= 400 * 16) return false;
    // layers made of many small pieces that together span the whole width (the battle tutorial's
    // dimming mask with a spotlight hole around the HUD): learned per texture from the previous frame.
    // They are narrowed like the HUD (the hole follows it) but their outer edges stay at the frame
    // edges, so the layer still covers the whole picture. (key: texture, its size, and whether the
    // pieces are black - the mask shares an atlas with coloured glows of the HUD)
    bool black = v[0].r == 0 && v[0].g == 0 && v[0].b == 0;
    u32 key = (ds.tme ? (ds.tbp & 16383) : 0x3FFFu) | (ds.tw & 15) << 14 | (ds.th & 15) << 18 | (black ? 1u << 22 : 0);
    HudSpan& sp = g_hud_span[key];
    sp.x0 = std::min(sp.x0, minx);
    sp.x1 = std::max(sp.x1, maxx);
    sp.y0 = std::min(sp.y0, miny);
    sp.y1 = std::max(sp.y1, maxy);
    sp.n++;
    sp.width = width;
    bool pin_edges = g_hud_fullwidth.count(key) != 0;
    int cx = width * 8;  // center in 12.4 fixed point
    float hf = platform::hud_factor();
    for (int i = 0; i < n; i++) {
        out[i] = v[i];
        if (pin_edges && (v[i].x <= 4 * 16 || v[i].x >= (width - 4) * 16)) continue;
        out[i].x = cx + (int)lrintf((v[i].x - cx) * hf);
    }
    st_hud++;
    return true;
}

void on_draw(const DrawState& ds, u32 state_id, int prim, const Vertex* vin) {
    st_draws++;
    Vertex hv[3];
    bool hud = hud_transform(ds, prim, vin, hv);
    const Vertex* v = hud ? hv : vin;
    if (state_id != g_bid || hud != g_bhud || g_bself) {
        if (g_bvalid && !g_verts.empty()) {
            const DrawState& p = g_prev_raw;
            if (g_bself) st_why[0]++;
            else if (hud != g_bhud) st_why[1]++;
            else {
                bool d[11] = {
                    p.fbp != ds.fbp || p.fbw != ds.fbw || p.fpsm != ds.fpsm || p.fbmsk != ds.fbmsk,
                    p.zbp != ds.zbp || p.zpsm != ds.zpsm || p.zmsk != ds.zmsk || p.zte != ds.zte || p.ztst != ds.ztst,
                    p.ate != ds.ate || p.atst != ds.atst || p.aref != ds.aref || p.afail != ds.afail || p.date != ds.date || p.datm != ds.datm,
                    p.abe != ds.abe || p.ba != ds.ba || p.bb != ds.bb || p.bc != ds.bc || p.bd != ds.bd || p.bfix != ds.bfix || p.pabe != ds.pabe ||
                        p.fba != ds.fba || p.colclamp != ds.colclamp,
                    p.tbp != ds.tbp || p.tbw != ds.tbw || p.tpsm != ds.tpsm || p.tw != ds.tw || p.th != ds.th,
                    p.tcc != ds.tcc || p.tfx != ds.tfx || p.cpsm != ds.cpsm || p.csa != ds.csa || p.ta0 != ds.ta0 || p.ta1 != ds.ta1 || p.aem != ds.aem,
                    p.wms != ds.wms || p.wmt != ds.wmt || p.minu != ds.minu || p.maxu != ds.maxu || p.minv != ds.minv || p.maxv != ds.maxv,
                    p.mag_linear != ds.mag_linear || p.min_linear != ds.min_linear || p.lcm != ds.lcm || p.mxl != ds.mxl || p.lodl != ds.lodl ||
                        p.mmin != ds.mmin || p.mtba != ds.mtba || p.miptbp1 != ds.miptbp1 || p.miptbp2 != ds.miptbp2 || p.lodk != ds.lodk,
                    p.fge != ds.fge || p.fogcol != ds.fogcol || p.iip != ds.iip || p.fst != ds.fst || p.tme != ds.tme,
                    p.scax0 != ds.scax0 || p.scax1 != ds.scax1 || p.scay0 != ds.scay0 || p.scay1 != ds.scay1,
                    p.dthe != ds.dthe || memcmp(p.dimx, ds.dimx, sizeof p.dimx) != 0};
                bool any = false;
                for (int i = 0; i < 11; i++) if (d[i]) { st_why[3 + i]++; any = true; }
                if (!any) st_why[2]++;
            }
        }
        g_prev_raw = ds;
        flush();
        g_bds = ds;
        g_bhud = hud;
        if (hud) {
            // the scissor of a narrowed element (text boxes clip their contents) narrows with it
            int width = (int)ds.fbw * 64, cx = width / 2;
            if (ds.scax0 > 0 || ds.scax1 < width - 1) {
                float hf = platform::hud_factor();
                g_bds.scax0 = (int)floorf(cx + (ds.scax0 - cx) * hf);
                g_bds.scax1 = (int)ceilf(cx + (ds.scax1 + 1 - cx) * hf) - 1;
            }
        }
        g_bid = state_id;
        g_bvalid = true;
        g_bx0 = g_by0 = 1 << 30;
        g_bx1 = g_by1 = -(1 << 30);
        g_bu0 = g_bv0 = 1e30f;
        g_bu1 = g_bv1 = -1e30f;
        g_ball_sprites = true;
        g_bsprites.clear();
        // a texture in memory the batch can write: each primitive must see the previous one's result
        g_bself = ds.tme && samples_own_writes(ds);
    }
    const DrawState& b = g_bds;
    if (prim != 6) g_ball_sprites = false;
    switch (prim) {
    case 3: {
        int minx = std::min({v[0].x, v[1].x, v[2].x}), maxx = std::max({v[0].x, v[1].x, v[2].x});
        int miny = std::min({v[0].y, v[1].y, v[2].y}), maxy = std::max({v[0].y, v[1].y, v[2].y});
        bounds_add(b, (minx + 15) >> 4, (miny + 15) >> 4, (maxx + 15) >> 4, (maxy + 15) >> 4);
        for (int i = 0; i < 3; i++) g_verts.push_back(mkv(b, v[i], v[2]));
        if (b.tme) for (int i = 0; i < 3; i++) uv_add(b, v[i]);
        break;
    }
    case 6: {
        const Vertex& a = v[0];
        const Vertex& c = v[1];
        bounds_add(b, (std::min(a.x, c.x) + 15) >> 4, (std::min(a.y, c.y) + 15) >> 4, (std::max(a.x, c.x) + 15) >> 4,
                   (std::max(a.y, c.y) + 15) >> 4);
        if (g_ball_sprites && g_bsprites.size() < 64) {
            PixRect r{std::max((std::min(a.x, c.x) + 15) >> 4, b.scax0), std::max((std::min(a.y, c.y) + 15) >> 4, b.scay0),
                      std::min((std::max(a.x, c.x) + 15) >> 4, b.scax1 + 1), std::min((std::max(a.y, c.y) + 15) >> 4, b.scay1 + 1)};
            if (r.x1 > r.x0 && r.y1 > r.y0) g_bsprites.push_back(r);
        }
        GV p00 = mkv(b, a, c), p11 = mkv(b, c, c);
        // corners: position and texture coordinates from the two vertices, everything else from the second
        GV p10 = p11, p01 = p11, q00 = p11;
        q00.x = p00.x; q00.y = p00.y; q00.s = p00.s; q00.t = p00.t;
        p10.x = p11.x; p10.y = p00.y; p10.s = p11.s; p10.t = p00.t;
        p01.x = p00.x; p01.y = p11.y; p01.s = p00.s; p01.t = p11.t;
        g_verts.push_back(q00); g_verts.push_back(p10); g_verts.push_back(p01);
        g_verts.push_back(p10); g_verts.push_back(p11); g_verts.push_back(p01);
        if (b.tme) { uv_add(b, a); uv_add(b, c); }
        break;
    }
    case 1: {
        // line: a quad one native pixel wide across the minor axis
        const Vertex& a = v[0];
        const Vertex& c = v[1];
        GV p0 = mkv(b, a, c), p1 = mkv(b, c, c);
        float dx = fabsf(p1.x - p0.x), dy = fabsf(p1.y - p0.y);
        GV p2 = p0, p3 = p1;
        if (dx >= dy) { p2.y += 1; p3.y += 1; } else { p2.x += 1; p3.x += 1; }
        g_verts.push_back(p0); g_verts.push_back(p1); g_verts.push_back(p2);
        g_verts.push_back(p1); g_verts.push_back(p3); g_verts.push_back(p2);
        bounds_add(b, (int)floorf(std::min(p0.x, p1.x)), (int)floorf(std::min(p0.y, p1.y)), (int)ceilf(std::max(p0.x, p1.x)) + 1,
                   (int)ceilf(std::max(p0.y, p1.y)) + 1);
        if (b.tme) { uv_add(b, a); uv_add(b, c); }
        break;
    }
    case 0: {
        const Vertex& a = v[0];
        GV p = mkv(b, a, a);
        p.x = (float)((a.x + 8) >> 4);
        p.y = (float)((a.y + 8) >> 4);
        GV p10 = p, p01 = p, p11 = p;
        p10.x += 1; p01.y += 1; p11.x += 1; p11.y += 1;
        g_verts.push_back(p); g_verts.push_back(p10); g_verts.push_back(p01);
        g_verts.push_back(p10); g_verts.push_back(p11); g_verts.push_back(p01);
        bounds_add(b, (int)p.x, (int)p.y, (int)p.x + 1, (int)p.y + 1);
        if (b.tme) uv_add(b, a);
        break;
    }
    }
    st_prims++;
    g_bprims++;
    if (g_verts.size() * sizeof(GV) > VB_BYTES - 1024) flush();
}

// ---------------------------------------------------------------------------
// Other commands
// ---------------------------------------------------------------------------
void on_clut_load(u64 tex0, u64 texclut) {
    // the CLUT source must be current in the shadow copy
    u32 cbp = bits(tex0, 37, 14), cpsm = bits(tex0, 51, 4), csm = bits(tex0, 55, 1);
    std::vector<u32> blocks;
    if (csm == 0) for_rect_blocks(cpsm, cbp, 1, 0, 0, 16, 16, [&](u32 b, bool) { blocks.push_back(b); });
    else for_rect_blocks(cpsm, cbp, bits(texclut, 0, 6), bits(texclut, 6, 6) * 16, bits(texclut, 12, 10), bits(texclut, 6, 6) * 16 + 256,
                         bits(texclut, 12, 10) + 1, [&](u32 b, bool) { blocks.push_back(b); });
    bool stale = false;
    for (u32 b : blocks) stale |= !g_shadow_ok[b];
    if (stale) {
        std::vector<u32> need = same_family(blocks, cpsm);
        for (u32 b : blocks) set_shadow_ok(b, true);  // reused memory: shadow data is what counts
        if (!need.empty()) { flush(); g_why = "clut"; make_shadow_current(need); }
    }
    u32 tmp[512];
    memcpy(tmp, g_clut, sizeof(tmp));
    exec_clut_load_m(shadow, tmp, tex0, texclut);
    if (memcmp(tmp, g_clut, sizeof(tmp)) != 0 || !g_clut_valid) {
        if (!g_verts.empty()) st_why[14]++;
        flush();
        memcpy(g_clut, tmp, sizeof(tmp));
        ctx->UpdateSubresource(g_clut_buf, 0, nullptr, g_clut, 0, 0);
        g_clut_valid = true;
        g_clut_version++;
    }
}

void on_upload(const UploadInfo& u, const u8* data, size_t len) {
    flush();
    // blocks only partly covered keep data from before: make the shadow copy current for them
    std::vector<u32> partial, all;
    for_rect_blocks(u.dpsm, u.dbp, u.dbw, (int)u.dx, (int)u.dy, (int)(u.dx + u.w), (int)(u.dy + u.h), [&](u32 b, bool full) {
        all.push_back(b);
        if (!full && !g_shadow_ok[b]) partial.push_back(b);
    });
    // keep the rest of a partly written block only where it can be seen: blocks of the displayed
    // frame buffers (elsewhere the game reuses scratch memory, e.g. CLUTs over blur buffers)
    partial = same_family(partial, u.dpsm);
    partial.erase(std::remove_if(partial.begin(), partial.end(), [](u32 b) {
        Surface* o = surf_by_id(g_owner[b]);
        return !o || (o->bp != g_display_fbp[0] && o->bp != g_display_fbp[1]);
    }), partial.end());
    if (!partial.empty()) { g_why = "partial upload"; make_shadow_current(partial); }
    exec_upload_rows_m(shadow, u, data, len, 0, 1);
    for (u32 b : all) mark_cpu_written(b);
}

void on_copy(const CopyInfo& c) {
    flush();
    std::vector<u32> src;
    for_rect_blocks(c.spsm, c.sbp, c.sbw, (int)c.sx, (int)c.sy, (int)(c.sx + c.w), (int)(c.sy + c.h), [&](u32 b, bool) { src.push_back(b); });
    std::vector<u32> partial, all;
    for_rect_blocks(c.dpsm, c.dbp, c.dbw, (int)c.dx, (int)c.dy, (int)(c.dx + c.w), (int)(c.dy + c.h), [&](u32 b, bool full) {
        all.push_back(b);
        if (!full) partial.push_back(b);
    });
    src.insert(src.end(), partial.begin(), partial.end());
    g_why = "copy";
    make_shadow_current(src);
    static std::vector<u32> tmp;
    tmp.resize((size_t)c.w * c.h);
    for (u32 y = 0; y < c.h; y++)
        for (u32 x = 0; x < c.w; x++)
            tmp[(size_t)y * c.w + x] = read_pixel_m(shadow, c.spsm, (int)((c.sx + x) & 2047), (int)((c.sy + y) & 2047), c.sbp, c.sbw);
    for (u32 y = 0; y < c.h; y++)
        for (u32 x = 0; x < c.w; x++)
            write_pixel_m(shadow, c.dpsm, (int)((c.dx + x) & 2047), (int)((c.dy + y) & 2047), c.dbp, c.dbw, tmp[(size_t)y * c.w + x]);
    for (u32 b : all) mark_cpu_written(b);
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
static void ensure_out(int w, int h) {
    if (g_out && g_out_w == w && g_out_h == h) return;
    release(g_out_rtv);
    release(g_out_srv);
    release(g_out);
    release(g_out_staging);
    g_out_w = w;
    g_out_h = h;
    g_out_mips = 1;
    while ((w >> g_out_mips) >= 64 && (h >> g_out_mips) >= 64 && g_out_mips < 4) g_out_mips++;
    make_tex2d(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0, &g_out,
               g_out_mips, g_out_mips > 1 ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rd.Texture2D.MipSlice = 0;
    dev->CreateRenderTargetView(g_out, &rd, &g_out_rtv);
    dev->CreateShaderResourceView(g_out, nullptr, &g_out_srv);
}

static u64 g_cmp_frames = 0, g_cmp_bad_frames = 0;

// GPU time per frame (timestamp queries, read a few frames later): average and maximum per second
bool g_gpu_timing = false;  // --gpu-timing
static void gpu_timing_frame() {
    static const int N = 6;
    static ID3D11Query *dis[N], *t0[N], *t1[N];
    static bool open[N];
    static int cur = -1;
    static double sum = 0, mx = 0, last_log = 0;
    static int cnt = 0;
    if (!dis[0]) {
        for (int i = 0; i < N; i++) {
            D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            dev->CreateQuery(&d, &dis[i]);
            d.Query = D3D11_QUERY_TIMESTAMP;
            dev->CreateQuery(&d, &t0[i]);
            dev->CreateQuery(&d, &t1[i]);
        }
    }
    if (cur >= 0) {
        ctx->End(t1[cur]);
        ctx->End(dis[cur]);
        open[cur] = true;
    }
    // collect finished frames
    for (int i = 0; i < N; i++) {
        if (!open[i] || i == cur) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        UINT64 a, b;
        if (ctx->GetData(dis[i], &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx->GetData(t0[i], &a, sizeof a, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        if (ctx->GetData(t1[i], &b, sizeof b, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        open[i] = false;
        if (dj.Disjoint || !dj.Frequency) continue;
        double ms = (double)(b - a) * 1000.0 / (double)dj.Frequency;
        sum += ms;
        mx = std::max(mx, ms);
        cnt++;
    }
    cur = (cur + 1) % N;
    if (open[cur]) open[cur] = false;  // not read back in time: reuse
    ctx->Begin(dis[cur]);
    ctx->End(t0[cur]);
    double now = host_time_sec();
    if (now - last_log >= 1.0 && cnt) {
        log_write(LOG_MAIN, "gpu frame time avg %.2f ms, max %.2f ms (%d frames, %llu batches)", sum / cnt, mx, cnt,
                  (unsigned long long)st_batches);
        sum = mx = 0;
        cnt = 0;
        last_log = now;
    }
}

void on_present(const PresentParams& p, const u32* sw_frame) {
    flush();
    if (g_gpu_timing) gpu_timing_frame();
    {
        // double buffering: the last two distinct buffers shown are the frame buffers
        u32 b = ((p.pmode >> 1) & 1) ? bits(p.dispfb[1], 0, 9) * 32 : bits(p.dispfb[0], 0, 9) * 32;
        if (b != g_display_fbp[1]) {
            g_display_fbp[0] = g_display_fbp[1];
            g_display_fbp[1] = b;
        }
    }
    g_frame++;
    hud_frame_end();
    if (g_frame % 600 == 0) evict_textures();
    evict_reps();
    bool en1 = p.pmode & 1, en2 = (p.pmode >> 1) & 1;
    struct PCB { s32 c1[4], c2[4], m[4], bg[4]; } pc{};
    Surface* s1 = nullptr;
    Surface* s2 = nullptr;
    auto setup = [&](int ci, s32* cc, Surface*& surf) {
        u64 fb = p.dispfb[ci];
        u32 fbp = bits(fb, 0, 9) * 32, fbw = bits(fb, 9, 6), psm = bits(fb, 15, 5);
        int dbx = (int)bits(fb, 32, 11), dby = (int)bits(fb, 43, 11);
        Layout l = layout_of(psm);
        if (l != L_C32 && l != L_C16 && l != L_C16S) return;
        int rows = p.field_mode ? (p.out_h + 1) / 2 : p.out_h;
        surf = get_surface(fbp, fbw, l, dby + rows);
        if (!surf) return;
        refresh(surf, dbx, dby, dbx + p.w, dby + rows);
        cc[0] = dbx; cc[1] = dby; cc[2] = psm == PSMCT24 ? 1 : (is16(l) ? 2 : 0); cc[3] = 1;
    };
    if (en1) setup(0, pc.c1, s1);
    if (en2) setup(1, pc.c2, s2);
    pc.m[0] = (s32)bits(p.pmode, 8, 8); pc.m[1] = (s32)bits(p.pmode, 5, 1); pc.m[2] = (s32)bits(p.pmode, 7, 1); pc.m[3] = p.field_mode;
    pc.bg[0] = (s32)(p.bgcolor & 0xFFFFFF); pc.bg[3] = S;
    int ow = p.w * S, oh = p.out_h * S;
    forget_bound();
    ensure_out(ow, oh);
    write_cb(g_cbp, &pc, sizeof(pc));
    ctx->OMSetRenderTargets(1, &g_out_rtv, nullptr);
    D3D11_VIEWPORT vp{0, 0, (float)ow, (float)oh, 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rs_noscissor);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(g_vs_full, nullptr, 0);
    ctx->PSSetShader(g_ps_merge, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &g_cbp);
    ID3D11ShaderResourceView* srvs[2] = {s1 ? s1->srv : g_dummy_srv, s2 ? s2->srv : g_dummy_srv};
    ctx->PSSetShaderResources(0, 2, srvs);
    ctx->Draw(4, 0);
    ID3D11ShaderResourceView* nsrv[2] = {nullptr, nullptr};
    ctx->PSSetShaderResources(0, 2, nsrv);
    ID3D11RenderTargetView* nrtv = nullptr;
    ctx->OMSetRenderTargets(1, &nrtv, nullptr);

    bool shot = platform::screenshot_due();
    // --hd-ab: every screenshot is followed by one of the next rendered frame with HD textures toggled
    // (the displayed buffer was drawn a frame earlier: wait for the third frame with new draws)
    static int ab_state = 0, ab_frames = 0;
    static u64 ab_batches = 0;
    bool ab_second = false;
    if (g_hd_ab && ab_state == 1 && st_batches > ab_batches) {
        ab_batches = st_batches;
        if (++ab_frames >= 3) { shot = true; ab_second = true; }
    }
    // --sharpen-ab: the next presented frame without sharpening (post effect: no delay)
    static int sh_state = 0, sh_saved = 0;
    if (g_sharpen_ab && sh_state == 1) {
        platform::request_final_shot();
        sh_state = 3;  // restored after this frame is presented
        shot = false;
    } else if (g_sharpen_ab && shot) {
        platform::request_final_shot();
        sh_saved = platform::config().sharpen;
        sh_state = 2;  // set below, after this frame is presented
        shot = false;
    } else if (shot && platform::g_shots_final && !g_hd_ab) {
        platform::request_final_shot();
        shot = false;
    }
    if (shot || sw_frame) {
        if (!g_out_staging) make_tex2d(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ, &g_out_staging);
        ctx->CopySubresourceRegion(g_out_staging, 0, 0, 0, 0, g_out, 0, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_out_staging, 0, D3D11_MAP_READ, 0, &m))) {
            if (shot) platform::save_screenshot((const u32*)m.pData, ow, oh, (int)(m.RowPitch / 4));
            if (shot && g_hd_ab) {
                platform::config().hd_textures = !platform::config().hd_textures;
                ab_state = ab_second ? 0 : 1;
                ab_batches = st_batches;
                ab_frames = 0;
            }
            if (sw_frame && S == 1) {
                // comparison with the software rasterizer (validation mode)
                u64 bad = 0;
                int maxd = 0;
                for (int y = 0; y < oh; y++) {
                    const u32* g = (const u32*)((const u8*)m.pData + (size_t)y * m.RowPitch);
                    const u32* s = sw_frame + (size_t)y * ow;
                    for (int x = 0; x < ow; x++) {
                        u32 a = g[x] & 0xFFFFFF, b = s[x] & 0xFFFFFF;
                        if (a != b) {
                            bad++;
                            for (int k = 0; k < 3; k++) maxd = std::max(maxd, std::abs((int)((a >> (k * 8)) & 0xFF) - (int)((b >> (k * 8)) & 0xFF)));
                        }
                    }
                }
                g_cmp_frames++;
                if (bad) {
                    g_cmp_bad_frames++;
                    static int logged = 0;
                    if (logged++ < 200 || g_frame % 60 == 0)
                        log_write(LOG_GS, "gpu compare: frame %llu: %llu pixels differ (max channel diff %d)", (unsigned long long)g_frame,
                                  (unsigned long long)bad, maxd);
                    static u64 last_dump = 0;
                    if (bad > 200 && g_frame - last_dump > 300 && platform::shots_enabled()) {
                        last_dump = g_frame;
                        platform::save_screenshot((const u32*)m.pData, ow, oh, (int)(m.RowPitch / 4));
                        platform::save_screenshot(sw_frame, ow, oh, ow);
                    }
                }
            }
            ctx->Unmap(g_out_staging, 0);
        }
    }
    if (g_out_mips > 1) ctx->GenerateMips(g_out_srv);
    g_last_ow = ow;
    g_last_oh = oh;
    platform::present_texture(g_out_srv, ow, oh);
    if (sh_state == 2) { platform::config().sharpen = 0; sh_state = 1; }
    else if (sh_state == 3) { platform::config().sharpen = sh_saved; sh_state = 0; }
    forget_bound();
}

void on_represent() {
    static int logged = 0;
    if (logged++ < 3) log_write(LOG_GS, "gpu: re-present %d x %d (srv %p)", g_last_ow, g_last_oh, (void*)g_out_srv);
    if (!g_out_srv || g_last_ow <= 0) return;
    platform::present_texture(g_out_srv, g_last_ow, g_last_oh);
    forget_bound();
}

void on_readback(const CopyInfo& r) {
    flush();
    std::vector<u32> blocks;
    for_rect_blocks(r.spsm, r.sbp, r.sbw, (int)r.sx, (int)r.sy, (int)(r.sx + r.w), (int)(r.sy + r.h), [&](u32 b, bool) { blocks.push_back(b); });
    g_why = "readback to EE";
    make_shadow_current(blocks);
}

const u32* memory() { return shadow; }

std::string stats() {
    static u64 l_gcopy, l_skip, l_rehash;
    static u64 l_draws, l_batches, l_refresh, l_rbc, l_rb, l_dec, l_hits, l_stale, l_self, l_rep, l_dump;
    char b[1200];
    snprintf(b, sizeof(b), "gpu: %llu prims in %llu batches, refresh %llu cells (gpu copies %llu, skipped %llu), readback %llu cells in %llu, tex decode %llu hit %llu (same data %llu) stale %llu, self %llu, surfaces %d, compare bad %llu/%llu, hd textures %llu, dumped %llu",
             (unsigned long long)(st_draws - l_draws), (unsigned long long)(st_batches - l_batches), (unsigned long long)(st_refresh_cells - l_refresh),
             (unsigned long long)(st_gpu_copy_cells - l_gcopy), (unsigned long long)(st_skip_cells - l_skip),
             (unsigned long long)(st_readback_cells - l_rbc), (unsigned long long)(st_readbacks - l_rb), (unsigned long long)(st_tex_decodes - l_dec),
             (unsigned long long)(st_tex_hits - l_hits), (unsigned long long)(st_tex_rehash - l_rehash), (unsigned long long)(st_stale_texels - l_stale), (unsigned long long)(st_self - l_self),
             (int)std::count_if(g_surf.begin(), g_surf.end(), [](Surface* s) { return s != nullptr; }), (unsigned long long)g_cmp_bad_frames,
             (unsigned long long)g_cmp_frames, (unsigned long long)(st_tex_replaced - l_rep), (unsigned long long)(st_tex_dumped - l_dump));
    {
        static u64 l_why[16];
        u64 w[16];
        for (int i = 0; i < 16; i++) { w[i] = st_why[i] - l_why[i]; l_why[i] = st_why[i]; }
        u64 ondraw = w[0] + w[1] + w[2];
        for (int i = 3; i < 14; i++) ondraw += 0;
        char t[400];
        snprintf(t, sizeof t, " | breaks: all %llu self %llu hud %llu same %llu clut %llu; fields frame %llu z %llu test %llu blend %llu tex %llu texmode %llu clamp %llu filter %llu misc %llu scissor %llu dither %llu",
                 (unsigned long long)w[15], (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[14],
                 (unsigned long long)w[3], (unsigned long long)w[4], (unsigned long long)w[5], (unsigned long long)w[6], (unsigned long long)w[7],
                 (unsigned long long)w[8], (unsigned long long)w[9], (unsigned long long)w[10], (unsigned long long)w[11], (unsigned long long)w[12],
                 (unsigned long long)w[13]);
        strncat(b, t, sizeof(b) - strlen(b) - 1);
    }
    l_rep = st_tex_replaced;
    l_dump = st_tex_dumped;
    l_gcopy = st_gpu_copy_cells;
    l_skip = st_skip_cells;
    l_rehash = st_tex_rehash;
    l_draws = st_draws; l_batches = st_batches; l_refresh = st_refresh_cells; l_rbc = st_readback_cells; l_rb = st_readbacks;
    l_dec = st_tex_decodes; l_hits = st_tex_hits; l_stale = st_stale_texels; l_self = st_self;
    return b;
}

// Savestates (renderer thread, in queue order): saving brings the shadow copy fully up to date and
// copies it out; loading replaces it, so every surface and cached texture refreshes from it.
void state_vram(u8* vram, u32* clut, bool load) {
    flush();
    if (!load) {
        std::vector<u32> stale;
        for (u32 b = 0; b < 16384; b++)
            if (!g_shadow_ok[b]) stale.push_back(b);
        g_why = "savestate";
        make_shadow_current(stale);
        memcpy(vram, shadow, 4u << 20);
        memcpy(clut, g_clut, sizeof(g_clut));
        return;
    }
    memcpy(shadow, vram, 4u << 20);
    for (u32 b = 0; b < 16384; b++) mark_cpu_written(b);
    memcpy(g_clut, clut, sizeof(g_clut));
    ctx->UpdateSubresource(g_clut_buf, 0, nullptr, g_clut, 0, 0);
    g_clut_valid = true;
    g_clut_version++;
}

}  // namespace gpu
}  // namespace gs
