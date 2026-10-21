// GS register state shared by the GS front end and the rasterizer.
#pragma once
#include "../common.h"
#include "gs_mem.h"
#include <string>

namespace gs {

struct Vertex {
    s32 x, y;     // 12.4 fixed point, window coordinates (offset already applied)
    u32 z;
    u8 r, g, b, a;
    float q;
    float s, t;
    s32 u, v;     // 10.4 fixed
    u8 f;
};

struct Context {
    u64 tex0, tex1, tex2, clamp, xyoffset, miptbp1, miptbp2, scissor, alpha, test, fba, frame, zbuf;
};

struct State {
    u64 prim = 0, prmodecont = 1, prmode = 0;
    u64 rgbaq = 0, st = 0, uv = 0, fog = 0;
    float q = 1.0f;
    u64 texclut = 0, scanmsk = 0, texa = 0, fogcol = 0, dimx = 0, dthe = 0, colclamp = 1, pabe = 0;
    u64 bitbltbuf = 0, trxpos = 0, trxreg = 0, trxdir = 0;
    u64 siglblid = 0;
    Context ctx[2];
    // vertex queue
    Vertex vq[4];
    int vcount = 0;
    int fan_first = 0;
};

extern State st;
extern u32 clut[512];   // CLUT buffer (1KB as 512 halfwords is 256 32-bit entries; store as 32-bit entries)

// rasterizer entry
void draw_primitive(int prim, const Vertex* v);
void raster_init(int threads);                  // threads < 0: automatic
void raster_flush();                            // wait until all queued primitives are drawn
void raster_before_vram_read(u32 blo, u32 bhi); // wait for queued primitives writing this block range
void raster_sync_rows(u32 bp, u32 bw, u32 psm, int y0, int y1, bool write);  // before direct VRAM access
void raster_clut_changed();
u64 pixels_shaded();

// Display output: snapshot of the PCRTC registers for one presented frame. The frame is composed
// by the rasterizer workers after all earlier drawing completed, so the emulation thread never
// waits for the display buffer.
struct PresentParams {
    u64 pmode, dispfb[2], display[2], smode2, bgcolor;
    int w, h, out_h;
    bool field_mode;
};
void compose_present_rows(const PresentParams& p, u32* out, int k, int n);  // rows (y >> 3) % n == k

// CLUT loads and host->local uploads run in order on the rasterizer workers (each worker keeps its own
// CLUT copy, uploads are split by destination row), so the emulation thread never waits for them.
struct UploadInfo {
    u32 dbp, dbw, dpsm, dx, dy, w, h;
};
void exec_clut_load(u32* dst, u64 tex0, u64 texclut);                                   // reads VRAM
void exec_upload_rows(const UploadInfo& u, const u8* data, size_t len, int k, int n);    // writes VRAM
void exec_clut_load_m(const u32* mem, u32* dst, u64 tex0, u64 texclut);                   // same on another memory copy
void exec_upload_rows_m(u32* mem, const UploadInfo& u, const u8* data, size_t len, int k, int n);
void raster_clut_load(u64 tex0, u64 texclut);
bool raster_async();                                // true: uploads must go through raster_upload_*
bool raster_sw_active();                            // the software rasterizer draws (keeps vram current)
void raster_upload(const UploadInfo& u, const u8* data, size_t len);
void raster_present(const PresentParams& p);
std::string raster_stats();
void raster_request_gpu(bool on, int scale, bool compare);   // before init(), after platform::init_video

static inline u32 bits(u64 v, int lo, int n) { return (u32)((v >> lo) & ((1ull << n) - 1)); }

}  // namespace gs
