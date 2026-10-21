#pragma once
#include "../common.h"

namespace iop::mc {
void register_servers();
void start(const std::string& args = "");
}  // namespace iop::mc
