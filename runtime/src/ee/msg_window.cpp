// Larger message windows (option big_text, widescreen only): the field's conversation windows are
// drawn by two objects of the game's 2D scene graph:
// - FIELD_SCENARIO_WINDOW (render method 0x2A11F0) owns the frame: a node (this+84) whose inner object
//   (+32) carries a uniform scale (+36, set by 0x1A65D0) and the window centre (+112, 512x448 screen
//   space). Children placed relative to the frame (the text origin, the "more" arrow) follow it.
// - FIELD_SCENARIO_MESSAGE_ANALYZE (render method 0x2A0400) adds each typed character to a glyph
//   list as a quad around (x + 8, 20 * line + 1) of 16 x 16 (0x278ED0 -> 0x27A270, also for the glow
//   copy); glyph quads ignore the node scale, so they are scaled where 0x27A270 builds them.
// The window is scaled around its centre and moved back inside the visible area if it would leave it.
#include "../common.h"
#include "../memory.h"
#include "../platform/platform.h"
#include "ee_recomp.h"
#include <cstring>

extern "C" void field_002A11F0(EEContext* c);
extern "C" void field_002A0400(EEContext* c);
extern "C" void field_0027A270(EEContext* c);
extern "C" void ee_001A65D0(EEContext* c);

static float g_glyph_scale = 1.0f;  // while the message text adds glyphs

static inline float rdf(u32 a) { float f; u32 v = mem::rd32(a); memcpy(&f, &v, 4); return f; }
static inline void wrf(u32 a, float f) { u32 v; memcpy(&v, &f, 4); mem::wr32(a, v); }

extern "C" void hook_msg_window(EEContext* c) {
    u32 self = c->r[4].uw[0];
    field_002A11F0(c);
    u32 node = mem::rd32(self + 84);
    u32 inner = node ? mem::rd32(node + 32) : 0;
    if (!inner || inner >= 0x2000000) return;
    float k = platform::text_scale();
    if (rdf(inner + 36) != k) {
        u64 a0 = c->r[4].ud[0];
        u32 f12 = c->fpr[12].u;
        c->r[4].ud[0] = inner;
        c->fpr[12].f = k;
        ee_001A65D0(c);  // scale (rebuilds the node matrix)
        c->r[4].ud[0] = a0;
        c->fpr[12].u = f12;
    }
    if (k == 1.0f) return;
    // keep the enlarged window inside the visible area: the frame spans the window size (message
    // object 0x219510: +12 width, +14 height) around the centre, plus room for the name plate
    u32 W = mem::rd32(0x219510);
    if (!W || W >= 0x2000000) return;
    float hw = (s16)mem::rd16(W + 12) * k * 0.5f + 12.0f, hh = (s16)mem::rd16(W + 14) * k * 0.5f + 20.0f;
    // widescreen with 4:3 menus: 2D x is narrowed to 3/4 around the centre, so the visible range is wider
    const platform::Config& cfg = platform::config();
    float x0 = 4.0f, x1 = 508.0f;
    if (cfg.widescreen_hack && cfg.hud_fix) {
        float hf = platform::hud_factor();
        x0 = 256.0f - 256.0f / hf + 4.0f;
        x1 = 256.0f + 256.0f / hf - 4.0f;
    }
    float cx = rdf(inner + 112), cy = rdf(inner + 116), nx = cx, ny = cy;
    if (nx - hw < x0) nx = x0 + hw;
    if (nx + hw > x1) nx = x1 - hw;
    if (ny - hh < 4.0f) ny = 4.0f + hh;
    if (ny + hh > 444.0f) ny = 444.0f - hh;
    if (nx != cx || ny != cy) {
        wrf(inner + 112, nx);
        wrf(inner + 116, ny);
        u64 a0 = c->r[4].ud[0];
        u32 f12 = c->fpr[12].u;
        c->r[4].ud[0] = inner;
        c->fpr[12].f = k;
        ee_001A65D0(c);  // the matrix takes the position from +112
        c->r[4].ud[0] = a0;
        c->fpr[12].u = f12;
    }
}

extern "C" void hook_msg_text(EEContext* c) {
    g_glyph_scale = platform::text_scale();
    field_002A0400(c);
    g_glyph_scale = 1.0f;
}

// glyph quad: centre a1 (x, y, z floats), size f12 x f13
extern "C" void hook_glyph_quad(EEContext* c) {
    float k = g_glyph_scale;
    if (k == 1.0f) {
        field_0027A270(c);
        return;
    }
    u32 p = c->r[5].uw[0];
    float x = rdf(p), y = rdf(p + 4);
    wrf(p, x * k);
    wrf(p + 4, y * k);
    c->fpr[12].f *= k;
    c->fpr[13].f *= k;
    field_0027A270(c);
    wrf(p, x);  // the caller places the glow copy from the same centre
    wrf(p + 4, y);
}

// ---------------------------------------------------------------------------------------------
// Battle conversation window (battle module, draw method 0x32B900): object +32 -> message state S;
// S+40/+44 = top-left of the box (512x448), S+676 = the main module's text box (content size +212,
// +216, 6 px border around it, name plate above). It draws in immediate mode: all its primitives go
// out through 0x166780 (type, vertex count, vertices of 24 bytes: float x, y, z, colour, float u, v),
// so they are scaled there around the box corner nearest to the screen edges.
extern "C" void battle_0032B900(EEContext* c);
extern "C" void ee_00166780(EEContext* c);
static bool g_bm_on = false;
static float g_bm_k = 1.0f, g_bm_ax = 0, g_bm_ay = 0;

extern "C" void hook_battle_msg_draw(EEContext* c) {
    float k = platform::text_scale();
    u32 S = mem::rd32(c->r[4].uw[0] + 32);
    if (k != 1.0f && S && S < 0x2000000 - 1024) {
        float x0 = rdf(S + 40), y0 = rdf(S + 44);
        float x1 = x0 + rdf(S + 676 + 212) + 12.0f, y1 = y0 + rdf(S + 676 + 216) + 12.0f;
        if (x1 > x0 && y1 > y0 && x1 - x0 < 1024 && y1 - y0 < 1024) {
            g_bm_k = k;
            g_bm_ax = (x0 + x1) * 0.5f < 256.0f ? x0 : x1;
            g_bm_ay = (y0 + y1) * 0.5f > 224.0f ? y1 : y0;
            g_bm_on = true;
        }
    }
    battle_0032B900(c);
    g_bm_on = false;
}

extern "C" void hook_prim_submit(EEContext* c) {
    if (g_bm_on) {
        u32 n = c->r[5].uw[0], p = c->r[6].uw[0];
        if (n < 65536 && p && p < 0x2000000 - n * 24)
            for (u32 i = 0; i < n; i++, p += 24) {
                wrf(p, g_bm_ax + (rdf(p) - g_bm_ax) * g_bm_k);
                wrf(p + 4, g_bm_ay + (rdf(p + 4) - g_bm_ay) * g_bm_k);
            }
    }
    ee_00166780(c);
}
