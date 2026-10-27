// Diagnostic hooks: the game's debug printf output and the movie player's libmpeg error callback
// go to the log (printf output is lost on a retail PS2).
#include "../common.h"
#include "../memory.h"
#include "../platform/platform.h"
#include "kernel.h"
#include "ee_recomp.h"
#include <algorithm>
#include <cstring>
#include <string>

extern "C" void ee_0010C480(EEContext* c);
extern "C" void ee_00120F80(EEContext* c);
extern "C" void ee_0012E050(EEContext* c);
extern "C" void ipu_trace_note(const char* s);
extern "C" void field_002A8400(EEContext* c);
extern "C" void field_002615F0(EEContext* c);
extern "C" int g_ee_patch[8];

namespace {

std::string guest_str(u32 p, size_t max = 256) {
    const u8* s = mem::ee_ptr(p);
    std::string r;
    if (!s) return "(bad ptr)";
    for (size_t i = 0; i < max && s[i]; i++) r += (char)s[i];
    return r;
}

// minimal printf over the EE argument registers a1..a3, t0..t3 (integer and string arguments)
std::string format(EEContext* c) {
    std::string fmt = guest_str(c->r[4].uw[0], 512), out;
    int arg = 5;
    auto next = [&]() -> u64 { return arg <= 11 ? c->r[arg++].ud[0] : 0; };
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        size_t j = i + 1;
        while (j < fmt.size() && strchr("-+ #0123456789.*lhqLjzt", fmt[j])) j++;
        if (j >= fmt.size()) break;
        std::string spec = fmt.substr(i, j - i);
        for (char ch : {'l', 'h', 'q', 'L', 'j', 'z', 't'}) spec.erase(std::remove(spec.begin(), spec.end(), ch), spec.end());
        char conv = fmt[j], b[512];
        if (conv == '%') out += '%';
        else if (conv == 's') { std::string s = guest_str((u32)next()); snprintf(b, sizeof(b), (spec + "s").c_str(), s.c_str()); out += b; }
        else if (strchr("dic", conv)) { snprintf(b, sizeof(b), (spec + conv).c_str(), (int)(s32)next()); out += b; }
        else if (strchr("uxXo", conv)) { snprintf(b, sizeof(b), (spec + conv).c_str(), (unsigned)next()); out += b; }
        else if (conv == 'p') { snprintf(b, sizeof(b), "%08x", (unsigned)next()); out += b; }
        else { next(); out += "<?>"; }
        i = j;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

void log_printf(EEContext* c) {
    static int n = 0;
    if (n >= 3000) return;
    n++;
    log_write(LOG_KERNEL, "[ee printf] %s", format(c).c_str());
}

}  // namespace

extern "C" void hook_printf_a(EEContext* c) { log_printf(c); ee_0010C480(c); }
extern "C" void hook_printf_b(EEContext* c) { log_printf(c); ee_00120F80(c); }

// sceMpeg error callback of the movie player (sets the player state 5 = stop): a1 = callback data,
// +4 = error message
extern "C" void hook_mpeg_error(EEContext* c) {
    u32 cb = c->r[5].uw[0];
    std::string msg = guest_str(mem::rd32(cb + 4));
    log_write(LOG_IPU, "movie: MPEG decoding error '%s'", msg.c_str());
    ipu_trace_note(msg.c_str());
    ee_0012E050(c);
}

// Field camera update (0x2A8400): stores 1 to 0x218C88 while a cutscene camera is active (black
// borders) and 0 otherwise. The original decision is kept as the cutscene indicator even when the
// borders are removed (patch 1 forces the store to 0): the body runs without the patch, then the
// variable is cleared.
int g_cinema = 0;
extern "C" void hook_field_cinema(EEContext* c) {
    int lb = g_ee_patch[1];
    g_ee_patch[1] = 0;
    field_002A8400(c);
    g_ee_patch[1] = lb;
    int v = mem::rd32(0x218C88) != 0;
    if (v != g_cinema) log_write(LOG_KERNEL, "cinema camera %d", v);
    g_cinema = v;
    if (lb) mem::wr32(0x218C88, 0);
}

// Camera projection setup (f12 horizontal field of view in degrees, f13 aspect, f14/f15 near/far).
// Widescreen in the shape of the window (aspect 4, e.g. 21:9): the horizontal view widens by the
// window's ratio to 4:3 and the vertical view stays as in 4:3. The fixed patches 0/4 do the same
// with constants for 16:9 and 16:10.
extern "C" void ee_0013B450(EEContext* c);
extern "C" void hook_projection(EEContext* c) {
    const platform::Config& cfg = platform::config();
    if (cfg.widescreen_hack && cfg.aspect == 4) {
        const float half_rad = 3.14159265f / 360.0f;  // degrees of the full angle -> radians of the half
        float r = platform::view_aspect() * 0.75f;
        c->fpr[12].f = atanf(tanf(c->fpr[12].f * half_rad) * r) / half_rad;
        c->fpr[13].f *= r;
    }
    ee_0013B450(c);
}


// Game flags (bit array at *(*(0x219520) + 4), as the game's get/set flag functions use it)
extern "C" int game_flag(int n) {
    u32 ctx = mem::rd32(0x219520);
    if (!ctx || ctx >= 0x2000000) return 0;
    u32 arr = mem::rd32(ctx + 4);
    if (!arr || arr >= 0x2000000) return 0;
    return (mem::ram[arr + n / 8] >> (n % 8)) & 1;
}

// The original's Scene Skip (an option unlocked for a new game after the ending): 0x2615F0 returns
// "flag 422 (Scene Skip on) and not flag 445 (this scene may not be skipped)"; then SELECT skips
// the current scene in the field and any movie. With the port option the setting counts as on,
// without writing it to the save (a new game copies the real setting: caller 0x264EB0).
extern "C" void hook_scene_skip_enabled(EEContext* c) {
    if (platform::config().cutscene_skip && c->r[31].uw[0] != 0x00264EC8u) {
        c->r[2].ud[0] = game_flag(445) ? 0 : 1;
        return;
    }
    field_002615F0(c);
}
