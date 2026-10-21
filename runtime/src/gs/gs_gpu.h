// GPU renderer (Direct3D 11.3 rasterizer ordered views), see gs_gpu.cpp.
#pragma once
#include "gs_state.h"

namespace gs {
struct DrawState;

struct CopyInfo {  // local -> local transfer
    u32 sbp, sbw, spsm, sx, sy, dbp, dbw, dpsm, dx, dy, w, h;
};

namespace gpu {
// Creates the renderer on the platform's device (after platform::init_video). scale: internal
// resolution multiplier (0 = automatic from the window height). False if the GPU lacks support.
bool init(int scale);
bool enabled();
int scale();

// Commands, in queue order, on the renderer thread
void on_draw(const DrawState& ds, u32 state_id, int prim, const Vertex* v);
void on_clut_load(u64 tex0, u64 texclut);
void on_upload(const UploadInfo& u, const u8* data, size_t len);
void on_copy(const CopyInfo& c);
// sw_frame: the software rasterizer's composition of the same frame (comparison mode), or null
void on_present(const PresentParams& p, const u32* sw_frame);
void on_readback(const CopyInfo& r);
void on_represent();                   // present the last composed picture again   // make the memory copy current for the source rectangle
const u32* memory();                   // the renderer's copy of local memory
void state_vram(u8* vram, u32* clut, bool load);  // savestates (4 MB VRAM, 512 CLUT entries)
std::string stats();
extern bool g_hd_ab;
extern bool g_sharpen_ab;              // --sharpen-ab: window screenshot pairs with sharpening on and off                   // --hd-ab: screenshot pairs with HD textures on and off
}  // namespace gpu
}  // namespace gs
