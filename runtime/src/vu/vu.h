#pragma once
#include "../common.h"
#include "ee_recomp.h"

namespace vu {

void init();
void vu1_start(u32 addr);
void vu1_continue();
void vu1_set_top(u32 top, u32 itop);
void vu0_start(u32 addr);
void vu0_continue();
void vu0_set_itop(u32 itop);
void invalidate_micro(int idx);

// VU0 micro programs called from EE macro code operate on the EE context's VU0 registers
void vu0_callms_ctx(EEContext* c, u32 addr);

}  // namespace vu
