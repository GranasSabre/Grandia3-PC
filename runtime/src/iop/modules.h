#pragma once
#include "../common.h"

namespace iop::modules {

void register_builtin();
void load(const std::string& path, const std::string& args);
void hexdump(int cat, const char* title, const u8* p, u32 n);

}  // namespace iop::modules
