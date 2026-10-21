#pragma once
#include "../common.h"

namespace iop::gs_iop {
void register_servers();
void start(const std::string& args = "");
}  // namespace iop::gs_iop
