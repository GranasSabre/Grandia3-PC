// HD texture packs: dumps of the textures the game samples and replacement images for them,
// both keyed by content (texel data + the palette entries it uses). See gs_texrep.cpp.
#pragma once
#include "../common.h"

namespace gs {
namespace texrep {

// a replacement image in GS 32-bit texel format (R | G << 8 | B << 16 | A << 24, alpha 0..128)
struct Image {
    int w = 0, h = 0;
    std::vector<u32> px;
};

// folders "textures" (replacements: <16 hex digit key>[anything].png, subfolders allowed) and "texdump"
void init(bool dump);
bool have_pack();         // at least one replacement image exists
bool translated(u64 key); // replaced from lang\de_tex: applies even with the HD textures off
bool dumping();

enum State { NONE, LOADING, READY };
// READY: *img stays valid until release() is called for the key
State lookup(u64 key, const Image** img);
void release(u64 key);    // the caller copied the image: free the memory (a later lookup loads again)

bool dumped(u64 key);     // already dumped (this session or an earlier one) or replaced
// writes texdump/<key>.png in the background (raw: texel data + palette, written as <key>.bin
// when not empty); info goes to texdump/index.txt
void dump(u64 key, int w, int h, std::vector<u32>&& px, const std::string& info, std::vector<u8>&& raw);

u64 hash(const void* data, size_t bytes, u64 seed);
}  // namespace texrep
}  // namespace gs
