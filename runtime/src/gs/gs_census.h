// GS usage census (analysis tool, see gs_census.cpp).
#pragma once
#include "gs_state.h"

namespace gs {
struct DrawState;
void census_enable(double from, double until);
bool census_active();  // true inside the census window (reports once when it ends)
void census_draw(const DrawState& ds, int prim, const Vertex* v);
void census_upload(u32 dbp, u32 dbw, u32 dpsm, u32 dx, u32 dy, u32 w, u32 h);
void census_copy(u32 sbp, u32 sbw, u32 spsm, u32 sx, u32 sy, u32 dbp, u32 dbw, u32 dpsm, u32 dx, u32 dy, u32 w, u32 h);
void census_readback(u32 sbp, u32 sbw, u32 spsm, u32 sx, u32 sy, u32 w, u32 h);
void census_clut(u64 tex0, u64 texclut);
void census_frame(u64 dispfb1, u64 dispfb2, u64 pmode);
}  // namespace gs
