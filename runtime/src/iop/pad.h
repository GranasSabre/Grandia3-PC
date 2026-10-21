#pragma once
#include "../common.h"

namespace iop::pad {
void register_servers();
void start(const std::string& args = "");
void vblank();
}  // namespace iop::pad
