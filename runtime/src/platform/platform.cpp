// Win32 window, D3D11 presentation, config file, input.
#include "platform.h"
#include "../memory.h"
#include "../ee/kernel.h"
#include "../savestate.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <xinput.h>
#include <commdlg.h>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <cmath>

namespace platform {

static Config g_cfg;
static std::string g_cfg_path;
static bool g_console = false;

Config& config() { return g_cfg; }
bool console_enabled() { return g_console; }

// libpad bit order: SELECT L3 R3 START UP RIGHT DOWN LEFT L2 R2 L1 R1 TRI CIRCLE CROSS SQUARE
static const char* PADNAMES[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                   "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
struct PadSrcName { u32 code; const char* name; };
static const PadSrcName PAD_SRC_NAMES[] = {
    {0x0001, "dpad_up"}, {0x0002, "dpad_down"}, {0x0004, "dpad_left"}, {0x0008, "dpad_right"}, {0x0010, "start"},
    {0x0020, "back"}, {0x0040, "lthumb"}, {0x0080, "rthumb"}, {0x0100, "lb"}, {0x0200, "rb"}, {0x1000, "a"},
    {0x2000, "b"}, {0x4000, "x"}, {0x8000, "y"}, {0x10000, "lt"}, {0x20000, "rt"}, {0, "none"},
};

static void default_keymap(Config& c) {
    int km[16] = {VK_BACK, 'Z', 'C', VK_RETURN, VK_UP, VK_RIGHT, VK_DOWN, VK_LEFT,
                  '1', '3', 'Q', 'E', 'W', 'D', 'S', 'A'};
    memcpy(c.keymap, km, sizeof(km));
}

void default_input(Config& c) {
    default_keymap(c);
    c.key_run = VK_SHIFT;
    u32 pm[16] = {0x0020, 0x0040, 0x0080, 0x0010, 0x0001, 0x0008, 0x0002, 0x0004,
                  0x10000, 0x20000, 0x0100, 0x0200, 0x8000, 0x2000, 0x1000, 0x4000};
    memcpy(c.padmap, pm, sizeof(pm));
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Steam Deck (the game runs there under Proton): Steam sets SteamDeck=1 for games; Proton's registry
// carries the BIOS product name (Jupiter = LCD model, Galileo = OLED model); the GPU is reported as
// "AMD Custom GPU 0405" / "... 0932" (RADV VANGOGH). --steamdeck simulates one.
bool g_force_steam_deck = false;
static int detect_steam_deck(std::string& why) {
    if (g_force_steam_deck) { why = "--steamdeck"; return 1; }
    char buf[256];
    DWORD n = GetEnvironmentVariableA("SteamDeck", buf, sizeof buf);
    if (n && n < sizeof buf && strcmp(buf, "1") == 0) { why = "SteamDeck=1"; return 1; }
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\BIOS", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        DWORD type = 0, len = sizeof buf - 1;
        bool deck = false;
        if (RegQueryValueExA(key, "SystemProductName", nullptr, &type, (BYTE*)buf, &len) == ERROR_SUCCESS && type == REG_SZ) {
            buf[len] = 0;
            deck = strcmp(buf, "Jupiter") == 0 || strcmp(buf, "Galileo") == 0;
            if (deck) why = std::string("BIOS ") + buf;
        }
        RegCloseKey(key);
        if (deck) return 1;
    }
    IDXGIFactory1* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac))) {
        IDXGIAdapter1* ad = nullptr;
        bool deck = false;
        if (SUCCEEDED(fac->EnumAdapters1(0, &ad))) {
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(ad->GetDesc1(&d))) {
                std::wstring s = d.Description;
                deck = s.find(L"VANGOGH") != std::wstring::npos || s.find(L"Custom GPU 0405") != std::wstring::npos ||
                       s.find(L"Custom GPU 0932") != std::wstring::npos;
                if (deck) why = "GPU";
            }
            ad->Release();
        }
        fac->Release();
        if (deck) return 1;
    }
    return 0;
}

static std::string g_deck_why;
bool is_steam_deck() {
    static int v = -1;
    if (v < 0) v = detect_steam_deck(g_deck_why);
    return v == 1;
}
std::string steam_deck_info() { return is_steam_deck() ? "yes (" + g_deck_why + ")" : "no"; }

void apply_patches() {
    g_ee_patch[0] = g_cfg.widescreen_hack;
    g_ee_patch[4] = g_cfg.widescreen_hack && g_cfg.aspect == 3;  // 16:10 variant (wins over patch 0)
}

float hud_factor() { return g_cfg.aspect == 3 ? 5.0f / 6.0f : 0.75f; }

std::string choose_disc_image(int disc) {
    wchar_t file[MAX_PATH * 4] = L"";
    wchar_t title[160];
    swprintf(title, 160, tr(L"Grandia III (USA) Disc %d: Abbild deiner Disc wählen (ISO)"), disc);
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = nullptr;  // before the game window exists
    of.lpstrFilter = L"ISO (*.iso)\0*.iso\0*.*\0*.*\0";
    of.lpstrFile = file;
    of.nMaxFile = MAX_PATH * 4;
    of.lpstrTitle = title;
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&of)) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, file, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, file, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void message(const wchar_t* text) { MessageBoxW(nullptr, text, L"Grandia III", MB_OK | MB_ICONINFORMATION); }

bool big_text_active() {
    return g_cfg.widescreen_hack && (g_cfg.big_text == 1 || (g_cfg.big_text < 0 && is_steam_deck()));
}

float text_scale() {
    if (!big_text_active()) return 1.0f;
    float k = g_cfg.text_scale;
    return k < 1.0f ? 1.0f : (k > 1.6f ? 1.6f : k);
}

void load_config(const std::string& path) {
    g_cfg_path = path;
    default_input(g_cfg);
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        // first start on a Steam Deck: widescreen picture in full screen (large conversation windows
        // are on there automatically)
        if (is_steam_deck()) {
            g_cfg.widescreen_hack = true;
            g_cfg.aspect = 3;  // 16:10 screen
            g_cfg.fullscreen = true;
        }
        save_config();
        return;
    }
    char line[512];
    static const char* KEYNAMES[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                       "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    while (fgets(line, sizeof(line), f)) {
        std::string l = trim(line);
        if (l.empty() || l[0] == ';' || l[0] == '#' || l[0] == '[') continue;
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        int iv = atoi(v.c_str());
        if (k == "width") g_cfg.window_width = iv;
        else if (k == "height") g_cfg.window_height = iv;
        else if (k == "fullscreen") g_cfg.fullscreen = iv != 0;
        else if (k == "vsync") g_cfg.vsync = iv != 0;
        else if (k == "aspect") g_cfg.aspect = iv;
        else if (k == "filter") g_cfg.filter = iv;
        else if (k == "internal_scale") g_cfg.internal_scale = iv < 0 ? 0 : (iv > 8 ? 8 : iv);
        else if (k == "widescreen_hack") g_cfg.widescreen_hack = iv != 0;
        else if (k == "remove_letterbox") g_cfg.remove_letterbox = iv != 0;
        else if (k == "hud_fix") g_cfg.hud_fix = iv != 0;
        else if (k == "vibration") g_cfg.vibration = iv != 0;
        else if (k == "disc") g_cfg.disc = iv == 2 ? 2 : 1;
        else if (k == "lod_scale") g_cfg.lod_scale = iv != 0;
        else if (k == "hd_textures") g_cfg.hd_textures = iv != 0;
        else if (k == "sharp_text") g_cfg.sharp_text = iv != 0;
        else if (k == "texture_dump") g_cfg.texture_dump = iv != 0;
        else if (k == "hd_lod_bias") g_cfg.hd_lod_bias = (float)atof(v.c_str());
        else if (k == "sharpen") g_cfg.sharpen = iv < 0 ? 0 : (iv > 100 ? 100 : iv);
        else if (k == "fps60") g_cfg.fps60 = iv != 0;
        else if (k == "fps60_battle") g_cfg.fps60_battle = iv != 0;
        else if (k == "movie_skip") g_cfg.movie_skip = iv != 0;
        else if (k == "cutscene_skip") g_cfg.cutscene_skip = iv != 0;
        else if (k == "state_slot") g_cfg.state_slot = iv < 1 ? 1 : iv > 9 ? 9 : iv;
        else if (k == "gs_thread") g_cfg.gs_thread = iv != 0;
        else if (k == "language") { g_cfg.language = v; for (char& ch : g_cfg.language) ch = (char)tolower((unsigned char)ch); }
        else if (k == "big_text") g_cfg.big_text = iv < 0 ? -1 : (iv ? 1 : 0);
        else if (k == "text_scale") { float f = (float)atof(v.c_str()); g_cfg.text_scale = f < 1.0f ? 1.0f : (f > 1.6f ? 1.6f : f); }
        else if (k == "show_fps") g_cfg.show_fps = iv != 0;
        else if (k == "integer_scaling") g_cfg.integer_scaling = iv != 0;
        else if (k == "frame_limit") g_cfg.frame_limit = iv;
        else if (k == "volume") g_cfg.volume = iv < 0 ? 0 : (iv > 100 ? 100 : iv);
        else if (k == "volume_music") g_cfg.vol_music = iv < 0 ? 0 : (iv > 100 ? 100 : iv);
        else if (k == "volume_voice") g_cfg.vol_voice = iv < 0 ? 0 : (iv > 100 ? 100 : iv);
        else if (k == "volume_sfx") g_cfg.vol_sfx = iv < 0 ? 0 : (iv > 100 ? 100 : iv);
        else if (k == "volume_movie") g_cfg.vol_movie = iv < 0 ? 0 : (iv > 100 ? 100 : iv);
        else if (k == "enabled" || k == "audio_enabled") g_cfg.audio_enabled = iv != 0;
        else if (k == "console") g_cfg.console = iv != 0;
        else if (k == "iso_path") g_cfg.iso_path = v;
        else if (k == "iso_path2") g_cfg.iso_path2 = v;
        else if (k == "save_dir") g_cfg.save_dir = v;
        else if (k == "key_run") g_cfg.key_run = (int)strtol(v.c_str(), nullptr, 0);
        else if (k == "renderer") g_cfg.renderer = (v == "software" || v == "0") ? 0 : 1;
        else {
            for (int i = 0; i < 16; i++) {
                if (k == std::string("key_") + KEYNAMES[i]) g_cfg.keymap[i] = (int)strtol(v.c_str(), nullptr, 0);
                if (k == std::string("pad_") + KEYNAMES[i])
                    for (const PadSrcName& p : PAD_SRC_NAMES)
                        if (v == p.name) g_cfg.padmap[i] = p.code;
            }
        }
    }
    fclose(f);
}

void save_config() {
    FILE* f = fopen(g_cfg_path.c_str(), "w");
    if (!f) return;
    static const char* KEYNAMES[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                       "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    fprintf(f, "; Grandia III native port configuration\n");
    fprintf(f, "[Video]\n");
    fprintf(f, "width = %d\nheight = %d\n", g_cfg.window_width, g_cfg.window_height);
    fprintf(f, "fullscreen = %d\n", g_cfg.fullscreen);
    fprintf(f, "vsync = %d\n", g_cfg.vsync);
    fprintf(f, "; aspect: 0 = 4:3, 1 = 16:9, 2 = stretch, 3 = 16:10 (Steam Deck; with widescreen a real 16:10 view)\n");
    fprintf(f, "aspect = %d\n", g_cfg.aspect);
    fprintf(f, "; filter: 0 = nearest, 1 = bilinear, 2 = sharp bilinear\n");
    fprintf(f, "filter = %d\n", g_cfg.filter);
    fprintf(f, "; renderer: gpu (Direct3D 11.3, high resolution) or software\nrenderer = %s\n", g_cfg.renderer ? "gpu" : "software");
    fprintf(f, "; internal resolution multiplier of the gpu renderer (0 = automatic from the screen height, 1 = native 512x448)\n");
    fprintf(f, "internal_scale = %d\n", g_cfg.internal_scale);
    fprintf(f, "; widescreen: 16:9 view of the 3D scenes (F6 toggles in game; 2D menus are stretched)\n");
    fprintf(f, "widescreen_hack = %d\n", g_cfg.widescreen_hack);
    fprintf(f, "; remove the black bars of cutscenes and battles (experimental)\nremove_letterbox = %d\n", g_cfg.remove_letterbox);
    fprintf(f, "; widescreen: menus, text and map keep their 4:3 proportions (gpu renderer)\nhud_fix = %d\n", g_cfg.hud_fix);
    fprintf(f, "; texture detail follows the internal resolution (sharper mip levels, gpu renderer)\nlod_scale = %d\n", g_cfg.lod_scale);
    fprintf(f, "; HD textures from the folder \"textures\" (gpu renderer, F8 toggles in game)\nhd_textures = %d\n", g_cfg.hd_textures);
    fprintf(f, "; sharp text: smooth glyph contours at high internal resolution (gpu renderer)\nsharp_text = %d\n", g_cfg.sharp_text);
    fprintf(f, "; write every new texture to the folder \"texdump\" (for making texture packs)\ntexture_dump = %d\n", g_cfg.texture_dump);
    fprintf(f, "; HD textures: detail level bias (negative = sharper, e.g. -0.5)\nhd_lod_bias = %.2f\n", g_cfg.hd_lod_bias);
    fprintf(f, "; 3D geometry and graphics commands on a processor core of their own (faster on most PCs)\ngs_thread = %d\n", g_cfg.gs_thread);
    fprintf(f, "; language of the game text and menus: en = original English, de = German (lang\\de.tsv); takes effect at the next start\nlanguage = %s\n", g_cfg.language.c_str());
    fprintf(f, "; larger conversation windows and text (widescreen only): -1 = automatic (on with a Steam Deck), 0 = off, 1 = on\nbig_text = %d\n", g_cfg.big_text);
    fprintf(f, "; their size factor, 1.0 .. 1.6\ntext_scale = %.2f\n", g_cfg.text_scale);
    fprintf(f, "; sharpening of the picture (contrast adaptive sharpening): 0 = off .. 100 (F4 toggles in game)\nsharpen = %d\n", g_cfg.sharpen);
    fprintf(f, "; 60 frames per second in the field (F3 toggles in game); fps60_battle: in battles too (experimental)\n");
    fprintf(f, "fps60 = %d\nfps60_battle = %d\n", g_cfg.fps60, g_cfg.fps60_battle);
    fprintf(f, "; START or SELECT skips movies (also those the original does not let you skip)\nmovie_skip = %d\n", g_cfg.movie_skip);
    fprintf(f, "; START skips cutscenes (they run invisibly at high speed until they end)\ncutscene_skip = %d\n", g_cfg.cutscene_skip);
    fprintf(f, "; savestate slot used by F5 (save) and F9 (load)\nstate_slot = %d\n", g_cfg.state_slot);
    fprintf(f, "integer_scaling = %d\n", g_cfg.integer_scaling);
    fprintf(f, "show_fps = %d\n", g_cfg.show_fps);
    fprintf(f, "frame_limit = %d\n", g_cfg.frame_limit);
    fprintf(f, "\n[Audio]\nenabled = %d\n; volume: 0..100\nvolume = %d\n", g_cfg.audio_enabled, g_cfg.volume);
    fprintf(f, "; separate volumes 0..100: music, speech, sound effects, movies\nvolume_music = %d\nvolume_voice = %d\nvolume_sfx = %d\nvolume_movie = %d\n",
            g_cfg.vol_music, g_cfg.vol_voice, g_cfg.vol_sfx, g_cfg.vol_movie);
    fprintf(f, "\n[Paths]\n; disc images of your own copy of Grandia III (USA): disc 1, disc 2\niso_path = %s\niso_path2 = %s\nsave_dir = %s\n",
            g_cfg.iso_path.c_str(), g_cfg.iso_path2.c_str(), g_cfg.save_dir.c_str());
    fprintf(f, "; inserted disc (1 or 2; the F1 menu switches and restarts)\ndisc = %d\n", g_cfg.disc);
    fprintf(f, "\n[Input]\n; Windows virtual-key codes (hex)\n");
    for (int i = 0; i < 16; i++) fprintf(f, "key_%s = 0x%02X\n", KEYNAMES[i], g_cfg.keymap[i]);
    fprintf(f, "; hold together with the direction keys to push the left analog stick fully (run)\nkey_run = 0x%02X\n", g_cfg.key_run);
    fprintf(f, "; controller vibration (0 = off)\nvibration = %d\n", g_cfg.vibration);
    fprintf(f, "; controller buttons: a b x y lb rb lt rt start back lthumb rthumb dpad_up dpad_down dpad_left dpad_right none\n");
    for (int i = 0; i < 16; i++) {
        const char* n = "none";
        for (const PadSrcName& p : PAD_SRC_NAMES)
            if (p.code == g_cfg.padmap[i]) n = p.name;
        fprintf(f, "pad_%s = %s\n", KEYNAMES[i], n);
    }
    fprintf(f, "\n[Debug]\nconsole = %d\n", g_cfg.console);
    fclose(f);
}

void init_console(bool enable) {
    g_console = enable;
    if (enable) {
        if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
}

void fatal_message(const char* msg) {
    MessageBoxA(nullptr, msg, "Grandia III", MB_OK | MB_ICONERROR);
}

// ---------------------------------------------------------------------------
// Video
// ---------------------------------------------------------------------------
static HWND g_hwnd;
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static IDXGISwapChain1* g_swap;
static ID3D11RenderTargetView* g_rtv;
static ID3D11Texture2D* g_tex;
static ID3D11ShaderResourceView* g_srv;
static int g_tex_w = 0, g_tex_h = 0;
static ID3D11VertexShader* g_vs;
static ID3D11PixelShader* g_ps;
static ID3D11SamplerState* g_samp_lin;
static ID3D11SamplerState* g_samp_pt;
static ID3D11Buffer* g_cb;
static ID3D11Buffer* g_cb_cas;
static ID3D11PixelShader* g_ps_overlay;
static ID3D11BlendState* g_blend_alpha;
static ID3D11RasterizerState* g_rs_present;   // no scissor, no culling (the renderer leaves its own state)
static ID3D11Texture2D* g_ovl_tex;
static ID3D11ShaderResourceView* g_ovl_srv;
static int g_ovl_w = 0, g_ovl_h = 0;
static u64 g_ovl_version = 0;
static std::vector<u32> g_ovl_px;
// frame rate label in the picture (option show_fps)
static ID3D11Texture2D* g_fps_tex;
static ID3D11ShaderResourceView* g_fps_srv;
static int g_fps_w = 0, g_fps_h = 0;
static u64 g_fps_version = 0;
static std::vector<u32> g_fps_px;
static float g_pic_x = 0, g_pic_y = 0;  // top-left of the game picture in the window
static ID3D11PixelShader* g_ps_cas;
static ID3D11Texture2D* g_mid;                 // scaled picture before sharpening (window size)
static ID3D11RenderTargetView* g_mid_rtv;
static ID3D11ShaderResourceView* g_mid_srv;
static int g_mid_w = 0, g_mid_h = 0;
static ID3D11Texture2D* g_bb_staging;          // --shots-final readback
static int g_bb_w = 0, g_bb_h = 0;
bool g_shots_final = false;
static bool g_final_shot_req = false;
static std::atomic<bool> g_closed{false};
static bool g_is_fullscreen = false;
static RECT g_windowed_rect;

static std::mutex g_frame_mutex;
static std::vector<u32> g_frame;
static int g_frame_w = 0, g_frame_h = 0;
static bool g_frame_new = false;
static int g_fps_count = 0;
static double g_fps_t0 = 0;
static double g_fps = 0;
static double g_game_fps = 0;  // frame buffer flips of the game per second
}  // namespace platform
namespace gs { u64 flip_count(); }
namespace platform {

bool window_closed() { return g_closed; }

static const char* SHADER_SRC = R"(
cbuffer CB : register(b0) { float4 rect; float4 texinfo; };
Texture2D tex : register(t0);
SamplerState samp : register(s0);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 t = float2(id & 1, id >> 1);  // quad corners as a 4-vertex triangle strip
    o.uv = t;
    float2 p = rect.xy + t * rect.zw;
    o.pos = float4(p.x * 2 - 1, 1 - p.y * 2, 0, 1);
    return o;
}
float4 ps_main(VSOut i) : SV_Target {
    float2 uv = i.uv;
    if (texinfo.z > 0.5) {
        // sharp bilinear: scale up with nearest to integer factor, then bilinear
        float2 texel = uv * texinfo.xy;
        float2 scale = max(floor(texinfo.w / texinfo.y), 1.0).xx;
        float2 tf = floor(texel) + 0.5;
        float2 f = frac(texel) - 0.5;
        float2 region = 0.5 - 0.5 / scale;
        float2 off = (clamp(f, -region, region) / (1.0 - 2.0 * region + 1e-5)) * (abs(f) > region ? 1.0 : 0.0);
        off = clamp(f - sign(f) * region, -0.5, 0.5) * scale;
        uv = (tf + clamp(off, -0.5, 0.5)) / texinfo.xy;
    }
    return float4(tex.Sample(samp, uv).rgb, 1);
}
float4 ps_overlay(VSOut i) : SV_Target { return tex.Sample(samp, i.uv); }
// AMD FidelityFX CAS (contrast adaptive sharpening, no scaling): texinfo.x = sharpness 0..1
float4 ps_cas(VSOut i) : SV_Target {
    int2 p = int2(i.pos.xy);
    float3 a = tex.Load(int3(p + int2(-1, -1), 0)).rgb, b = tex.Load(int3(p + int2(0, -1), 0)).rgb, c = tex.Load(int3(p + int2(1, -1), 0)).rgb;
    float3 d = tex.Load(int3(p + int2(-1, 0), 0)).rgb, e = tex.Load(int3(p, 0)).rgb, f = tex.Load(int3(p + int2(1, 0), 0)).rgb;
    float3 g = tex.Load(int3(p + int2(-1, 1), 0)).rgb, h = tex.Load(int3(p + int2(0, 1), 0)).rgb, k = tex.Load(int3(p + int2(1, 1), 0)).rgb;
    float3 mn = min(min(min(d, e), min(f, b)), h);
    float3 mn2 = min(mn, min(min(a, c), min(g, k)));
    mn += mn2;
    float3 mx = max(max(max(d, e), max(f, b)), h);
    float3 mx2 = max(mx, max(max(a, c), max(g, k)));
    mx += mx2;
    float3 amp = saturate(min(mn, 2.0 - mx) / max(mx, 1e-5));
    amp = sqrt(amp);
    float peak = -1.0 / lerp(8.0, 5.0, saturate(texinfo.x));
    float3 w = amp * peak;
    float3 o = saturate(((b + d) + (f + h)) * w + e) / (1.0 + 4.0 * w);
    return float4(saturate(o), 1);
}
)";

static bool create_rtv() {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    return SUCCEEDED(hr);
}

static void resize_swap() {
    if (!g_swap) return;
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);  // the context must not reference the back buffer
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    g_swap->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    create_rtv();
}

static void set_fullscreen(bool fs) {
    if (fs == g_is_fullscreen) return;
    g_is_fullscreen = fs;
    if (fs) {
        GetWindowRect(g_hwnd, &g_windowed_rect);
        HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfo(mon, &mi);
        SetWindowLongPtr(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        SetWindowLongPtr(g_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(g_hwnd, nullptr, g_windowed_rect.left, g_windowed_rect.top, g_windowed_rect.right - g_windowed_rect.left,
                     g_windowed_rect.bottom - g_windowed_rect.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOZORDER);
    }
}

static bool g_keys[256];
static void set_fullscreen(bool fs);
void set_fullscreen_public(bool fs) { set_fullscreen(fs); }
static std::atomic<bool> g_gpu_mode{false};       // GPU renderer: its thread presents, the UI thread only pumps messages
static std::atomic<bool> g_resize_pending{false};

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        set_vibration(0, 0);
        g_closed = true;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (g_gpu_mode) g_resize_pending = true;  // the GPU renderer thread owns the device context
        else resize_swap();
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == VK_RETURN) { set_fullscreen(!g_is_fullscreen); return 0; }
        if (wp == VK_F4) break;
        g_keys[wp & 0xFF] = true;
        return 0;
    case WM_KEYDOWN:
        if (overlay_open() && wp != VK_F11) { overlay_handle_key((int)wp); return 0; }
        if (wp == VK_F11) { set_fullscreen(!g_is_fullscreen); return 0; }
        if (wp == VK_F1) { overlay_set_open(true); return 0; }
        if (wp == VK_F5) { if (!(lp & 0x40000000)) ss::request_save(g_cfg.state_slot); return 0; }  // no key repeat
        if (wp == VK_F9) { if (!(lp & 0x40000000)) ss::request_load(g_cfg.state_slot); return 0; }
        if (wp == VK_F6) {  // widescreen on/off (projection patch + 16:9 output)
            g_cfg.widescreen_hack = !g_cfg.widescreen_hack;
            apply_patches();
            return 0;
        }
        if (wp == VK_F7) { g_cfg.show_fps = !g_cfg.show_fps; return 0; }
        if (wp == VK_F8) { g_cfg.hd_textures = !g_cfg.hd_textures; return 0; }
        if (wp == VK_F3) {  // 60 fps on/off
            g_cfg.fps60 = !g_cfg.fps60;
            kernel::g_force60 = g_cfg.fps60;
            return 0;
        }
        if (wp == VK_F4) {  // sharpening off / back to the last strength
            static int last = 60;
            if (g_cfg.sharpen) { last = g_cfg.sharpen; g_cfg.sharpen = 0; }
            else g_cfg.sharpen = last ? last : 60;
            return 0;
        }
        if (wp == VK_TAB) { g_cfg.frame_limit = 0; return 0; }
        g_keys[wp & 0xFF] = true;
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wp == VK_TAB) { g_cfg.frame_limit = 1; return 0; }
        g_keys[wp & 0xFF] = false;
        return 0;
    case WM_KILLFOCUS:
        memset(g_keys, 0, sizeof(g_keys));
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool init_video() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(GetModuleHandle(nullptr), MAKEINTRESOURCE(1));
    wc.lpszClassName = L"Grandia3Window";
    RegisterClassExW(&wc);
    RECT r{0, 0, g_cfg.window_width, g_cfg.window_height};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Grandia III", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;

    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 2, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx)) &&
        FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls + 1, 1, D3D11_SDK_VERSION, &g_dev, nullptr, &g_ctx))) {
        fatal_message("Failed to create Direct3D 11 device.");
        return false;
    }
    IDXGIDevice* dxdev;
    IDXGIAdapter* adapter;
    IDXGIFactory2* factory;
    g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev);
    dxdev->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (FAILED(factory->CreateSwapChainForHwnd(g_dev, g_hwnd, &sd, nullptr, nullptr, &g_swap))) {
        fatal_message("Failed to create swap chain.");
        return false;
    }
    factory->MakeWindowAssociation(g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    adapter->Release();
    dxdev->Release();
    create_rtv();

    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(SHADER_SRC, strlen(SHADER_SRC), "present", nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(SHADER_SRC, strlen(SHADER_SRC), "present", nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &psb, &err))) {
        fatal_message(err ? (const char*)err->GetBufferPointer() : "shader compile failed");
        return false;
    }
    g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
    vsb->Release();
    psb->Release();
    {
        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        g_dev->CreateRasterizerState(&rd, &g_rs_present);
    }
    ID3DBlob* ovb = nullptr;
    if (SUCCEEDED(D3DCompile(SHADER_SRC, strlen(SHADER_SRC), "present", nullptr, nullptr, "ps_overlay", "ps_5_0", 0, 0, &ovb, nullptr))) {
        g_dev->CreatePixelShader(ovb->GetBufferPointer(), ovb->GetBufferSize(), nullptr, &g_ps_overlay);
        ovb->Release();
    }
    {
        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        g_dev->CreateBlendState(&bd, &g_blend_alpha);
    }
    ID3DBlob* casb = nullptr;
    ID3DBlob* caserr = nullptr;
    if (SUCCEEDED(D3DCompile(SHADER_SRC, strlen(SHADER_SRC), "present", nullptr, nullptr, "ps_cas", "ps_5_0", 0, 0, &casb, &caserr))) {
        g_dev->CreatePixelShader(casb->GetBufferPointer(), casb->GetBufferSize(), nullptr, &g_ps_cas);
        casb->Release();
    } else {
        log_write(LOG_MAIN, "sharpening shader: %s", caserr ? (const char*)caserr->GetBufferPointer() : "compile failed");
    }
    if (caserr) caserr->Release();
    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smp, &g_samp_lin);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g_dev->CreateSamplerState(&smp, &g_samp_pt);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = 32;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    g_dev->CreateBuffer(&bd, nullptr, &g_cb_cas);

    ShowWindow(g_hwnd, SW_SHOW);
    if (g_cfg.fullscreen) set_fullscreen(true);
    return true;
}

static double g_shot_interval = 0, g_shot_last = 0;
static int g_shot_index = 0;
static std::string g_shot_dir;

void set_screenshot_interval(double sec, const std::string& dir) {
    g_shot_interval = sec;
    g_shot_dir = dir;
    CreateDirectoryA(dir.c_str(), nullptr);
}

static void write_bmp(const std::string& path, const u32* px, int w, int h, int stride) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    u32 rowsz = w * 4;
    u32 size = 54 + rowsz * h;
    u8 hdr[54] = {'B', 'M'};
    memcpy(hdr + 2, &size, 4);
    u32 off = 54, dib = 40, planes_bpp = 1 | (32 << 16);
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &dib, 4);
    memcpy(hdr + 18, &w, 4);
    s32 hh = -h;
    memcpy(hdr + 22, &hh, 4);
    memcpy(hdr + 26, &planes_bpp, 4);
    fwrite(hdr, 1, 54, f);
    std::vector<u32> row(w);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            u32 c = px[(size_t)y * stride + x];
            row[x] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
        }
        fwrite(row.data(), 4, w, f);
    }
    fclose(f);
}

double g_shot_from = 0, g_shot_until = 1e30;  // --shots-window=FROM:UNTIL (seconds)
void set_screenshot_window(double from, double until) {
    g_shot_from = from;
    g_shot_until = until;
}

void submit_frame(const u32* pixels, int width, int height, int stride) {
    if (g_shot_interval > 0) {
        double t = host_time_sec();
        if (t >= g_shot_from && t < g_shot_until && t - g_shot_last >= g_shot_interval) {
            g_shot_last = t;
            char name[64];
            snprintf(name, sizeof(name), "/shot_%04d.bmp", g_shot_index++);
            write_bmp(g_shot_dir + name, pixels, width, height, stride);
        }
    }
    std::lock_guard<std::mutex> lk(g_frame_mutex);
    g_frame.resize((size_t)width * height);
    for (int y = 0; y < height; y++) memcpy(&g_frame[(size_t)y * width], pixels + (size_t)y * stride, width * 4);
    g_frame_w = width;
    g_frame_h = height;
    g_frame_new = true;
    g_fps_count++;
}

static void draw_present(ID3D11ShaderResourceView* srv, int tex_w, int tex_h);

static void render() {
    {
        std::lock_guard<std::mutex> lk(g_frame_mutex);
        if (g_frame_new && g_frame_w > 0) {
            if (g_frame_w != g_tex_w || g_frame_h != g_tex_h) {
                if (g_srv) g_srv->Release();
                if (g_tex) g_tex->Release();
                D3D11_TEXTURE2D_DESC td{};
                td.Width = g_frame_w;
                td.Height = g_frame_h;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                g_dev->CreateTexture2D(&td, nullptr, &g_tex);
                g_dev->CreateShaderResourceView(g_tex, nullptr, &g_srv);
                g_tex_w = g_frame_w;
                g_tex_h = g_frame_h;
            }
            g_ctx->UpdateSubresource(g_tex, 0, nullptr, g_frame.data(), g_frame_w * 4, 0);
            g_frame_new = false;
        }
    }
    draw_present(g_srv, g_tex_w, g_tex_h);
}

// Draws the frame texture letterboxed into the back buffer and presents.
static void draw_present(ID3D11ShaderResourceView* srv, int tex_w, int tex_h) {
    RECT cr;
    GetClientRect(g_hwnd, &cr);
    float ww = (float)(cr.right - cr.left), wh = (float)(cr.bottom - cr.top);
    if (ww < 1 || wh < 1 || !g_rtv) return;
    float clear[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    if (srv) {
        float ar = g_cfg.aspect == 0 ? 4.0f / 3.0f : g_cfg.aspect == 1 ? 16.0f / 9.0f : g_cfg.aspect == 3 ? 16.0f / 10.0f : ww / wh;
        if (g_cfg.widescreen_hack && g_cfg.aspect == 0) ar = 16.0f / 9.0f;  // widescreen renders a 16:9 view
        float w = ww, h = wh;
        if (w / h > ar) w = h * ar;
        else h = w / ar;
        if (g_cfg.integer_scaling && tex_h > 0) {
            float k = floorf(h / tex_h);
            if (k >= 1) { h = k * tex_h; w = h * ar; }
        }
        float x = (ww - w) * 0.5f, y = (wh - h) * 0.5f;
        g_pic_x = x;
        g_pic_y = y;
        // sharpening: scale into an intermediate picture, then sharpen it into the back buffer
        bool cas = g_cfg.sharpen > 0 && g_ps_cas;
        if (cas && (g_mid_w != (int)ww || g_mid_h != (int)wh)) {
            if (g_mid_srv) g_mid_srv->Release();
            if (g_mid_rtv) g_mid_rtv->Release();
            if (g_mid) g_mid->Release();
            g_mid = nullptr; g_mid_rtv = nullptr; g_mid_srv = nullptr;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)ww;
            td.Height = (UINT)wh;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_mid))) {
                g_dev->CreateRenderTargetView(g_mid, nullptr, &g_mid_rtv);
                g_dev->CreateShaderResourceView(g_mid, nullptr, &g_mid_srv);
            }
            g_mid_w = (int)ww;
            g_mid_h = (int)wh;
        }
        if (cas && !g_mid_rtv) cas = false;
        ID3D11RenderTargetView* target = cas ? g_mid_rtv : g_rtv;
        if (cas) g_ctx->ClearRenderTargetView(g_mid_rtv, clear);
        D3D11_MAPPED_SUBRESOURCE m;
        g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        float cb[8] = {x / ww, y / wh, w / ww, h / wh, (float)tex_w, (float)tex_h, g_cfg.filter == 2 ? 1.0f : 0.0f, h};
        memcpy(m.pData, cb, sizeof(cb));
        g_ctx->Unmap(g_cb, 0);
        D3D11_VIEWPORT vp{0, 0, ww, wh, 0, 1};
        g_ctx->RSSetViewports(1, &vp);
        g_ctx->RSSetState(g_rs_present);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        g_ctx->OMSetRenderTargets(1, &target, nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->VSSetShader(g_vs, nullptr, 0);
        g_ctx->PSSetShader(g_ps, nullptr, 0);
        g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
        g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
        g_ctx->PSSetShaderResources(0, 1, &srv);
        ID3D11SamplerState* s = g_cfg.filter == 0 ? g_samp_pt : g_samp_lin;
        g_ctx->PSSetSamplers(0, 1, &s);
        g_ctx->Draw(4, 0);
        ID3D11ShaderResourceView* none = nullptr;
        g_ctx->PSSetShaderResources(0, 1, &none);
        if (cas) {
            float cc[8] = {0, 0, 1, 1, g_cfg.sharpen / 100.0f, 0, 0, 0};
            g_ctx->Map(g_cb_cas, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            memcpy(m.pData, cc, sizeof(cc));
            g_ctx->Unmap(g_cb_cas, 0);
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_ctx->VSSetConstantBuffers(0, 1, &g_cb_cas);
            g_ctx->PSSetConstantBuffers(0, 1, &g_cb_cas);
            g_ctx->PSSetShader(g_ps_cas, nullptr, 0);
            g_ctx->PSSetShaderResources(0, 1, &g_mid_srv);
            g_ctx->Draw(4, 0);
            g_ctx->PSSetShaderResources(0, 1, &none);
        }
    }
    if ((overlay_open() || skip_active() || notice_active()) && g_ps_overlay && g_blend_alpha) {
        bool top = !overlay_open() && !skip_active();  // a notice sits at the top
        int target = (int)(wh * 0.86f);
        int ow = 0, oh = 0;
        u64 ver = 0;
        if (overlay_pixels(target, g_ovl_px, ow, oh, ver) && ow > 0 && oh > 0) {
            if (!g_ovl_tex || ow != g_ovl_w || oh != g_ovl_h) {
                if (g_ovl_srv) g_ovl_srv->Release();
                if (g_ovl_tex) g_ovl_tex->Release();
                g_ovl_srv = nullptr;
                g_ovl_tex = nullptr;
                D3D11_TEXTURE2D_DESC td{};
                td.Width = (UINT)ow;
                td.Height = (UINT)oh;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_ovl_tex))) g_dev->CreateShaderResourceView(g_ovl_tex, nullptr, &g_ovl_srv);
                g_ovl_w = ow;
                g_ovl_h = oh;
                g_ovl_version = 0;
            }
            if (g_ovl_tex && ver != g_ovl_version) {
                g_ctx->UpdateSubresource(g_ovl_tex, 0, nullptr, g_ovl_px.data(), ow * 4, 0);
                g_ovl_version = ver;
            }
            if (g_ovl_srv) {
                float pw = (float)ow / ww, ph = (float)oh / wh;
                float cbo[8] = {(1 - pw) * 0.5f, top ? 0.04f : (1 - ph) * 0.5f, pw, ph, 0, 0, 0, 0};
                D3D11_MAPPED_SUBRESOURCE mo;
                g_ctx->Map(g_cb_cas, 0, D3D11_MAP_WRITE_DISCARD, 0, &mo);
                memcpy(mo.pData, cbo, sizeof(cbo));
                g_ctx->Unmap(g_cb_cas, 0);
                D3D11_VIEWPORT vpo{0, 0, ww, wh, 0, 1};
                g_ctx->RSSetViewports(1, &vpo);
                g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
                float bf[4] = {0, 0, 0, 0};
                g_ctx->OMSetBlendState(g_blend_alpha, bf, 0xFFFFFFFF);
                g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                g_ctx->IASetInputLayout(nullptr);
                g_ctx->VSSetShader(g_vs, nullptr, 0);
                g_ctx->VSSetConstantBuffers(0, 1, &g_cb_cas);
                g_ctx->PSSetConstantBuffers(0, 1, &g_cb_cas);
                g_ctx->PSSetShader(g_ps_overlay, nullptr, 0);
                g_ctx->PSSetShaderResources(0, 1, &g_ovl_srv);
                g_ctx->PSSetSamplers(0, 1, &g_samp_lin);
                g_ctx->Draw(4, 0);
                ID3D11ShaderResourceView* none = nullptr;
                g_ctx->PSSetShaderResources(0, 1, &none);
                g_ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
            }
        }
    }
    if (g_cfg.show_fps && !overlay_open() && g_ps_overlay && g_blend_alpha) {
        int ow = 0, oh = 0;
        u64 ver = 0;
        if (fps_pixels((int)(wh * 0.86f), std::min(60, (int)(g_game_fps + 0.5)), g_fps_px, ow, oh, ver) && ow > 0 && oh > 0) {
            if (!g_fps_tex || ow != g_fps_w || oh != g_fps_h) {
                if (g_fps_srv) g_fps_srv->Release();
                if (g_fps_tex) g_fps_tex->Release();
                g_fps_srv = nullptr;
                g_fps_tex = nullptr;
                D3D11_TEXTURE2D_DESC td{};
                td.Width = (UINT)ow;
                td.Height = (UINT)oh;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &g_fps_tex))) g_dev->CreateShaderResourceView(g_fps_tex, nullptr, &g_fps_srv);
                g_fps_w = ow;
                g_fps_h = oh;
                g_fps_version = 0;
            }
            if (g_fps_tex && ver != g_fps_version) {
                g_ctx->UpdateSubresource(g_fps_tex, 0, nullptr, g_fps_px.data(), ow * 4, 0);
                g_fps_version = ver;
            }
            if (g_fps_srv) {
                // top left corner of the game picture, a small margin inside
                float m = wh * 0.012f;
                float cbo[8] = {(g_pic_x + m) / ww, (g_pic_y + m) / wh, (float)ow / ww, (float)oh / wh, 0, 0, 0, 0};
                D3D11_MAPPED_SUBRESOURCE mo;
                g_ctx->Map(g_cb_cas, 0, D3D11_MAP_WRITE_DISCARD, 0, &mo);
                memcpy(mo.pData, cbo, sizeof(cbo));
                g_ctx->Unmap(g_cb_cas, 0);
                D3D11_VIEWPORT vpo{0, 0, ww, wh, 0, 1};
                g_ctx->RSSetViewports(1, &vpo);
                g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
                float bf[4] = {0, 0, 0, 0};
                g_ctx->OMSetBlendState(g_blend_alpha, bf, 0xFFFFFFFF);
                g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                g_ctx->IASetInputLayout(nullptr);
                g_ctx->VSSetShader(g_vs, nullptr, 0);
                g_ctx->VSSetConstantBuffers(0, 1, &g_cb_cas);
                g_ctx->PSSetConstantBuffers(0, 1, &g_cb_cas);
                g_ctx->PSSetShader(g_ps_overlay, nullptr, 0);
                g_ctx->PSSetShaderResources(0, 1, &g_fps_srv);
                g_ctx->PSSetSamplers(0, 1, &g_samp_lin);
                g_ctx->Draw(4, 0);
                ID3D11ShaderResourceView* none = nullptr;
                g_ctx->PSSetShaderResources(0, 1, &none);
                g_ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
            }
        }
    }
    if (g_final_shot_req) {
        g_final_shot_req = false;
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) {
            D3D11_TEXTURE2D_DESC td;
            bb->GetDesc(&td);
            if (!g_bb_staging || g_bb_w != (int)td.Width || g_bb_h != (int)td.Height) {
                if (g_bb_staging) g_bb_staging->Release();
                td.Usage = D3D11_USAGE_STAGING;
                td.BindFlags = 0;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                td.MiscFlags = 0;
                g_bb_staging = nullptr;
                g_dev->CreateTexture2D(&td, nullptr, &g_bb_staging);
                g_bb_w = (int)td.Width;
                g_bb_h = (int)td.Height;
            }
            if (g_bb_staging) {
                g_ctx->CopyResource(g_bb_staging, bb);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(g_ctx->Map(g_bb_staging, 0, D3D11_MAP_READ, 0, &m))) {
                    save_screenshot((const u32*)m.pData, g_bb_w, g_bb_h, (int)(m.RowPitch / 4));
                    g_ctx->Unmap(g_bb_staging, 0);
                }
            }
            bb->Release();
        }
    }
    g_swap->Present(g_cfg.vsync && !skip_active() ? 1 : 0, 0);
}

ID3D11Device* d3d_device() { return g_dev; }
ID3D11DeviceContext* d3d_context() { return g_ctx; }
void set_gpu_present(bool on) { g_gpu_mode = on; }

// GPU renderer thread: present a finished frame texture (mipmapped when larger than the window)
void present_texture(ID3D11ShaderResourceView* srv, int w, int h) {
    if (g_resize_pending.exchange(false)) resize_swap();
    if (skip_active()) {  // skipping: a black picture with the note, ten times a second
        static double last = 0;
        double now = host_time_sec();
        if (now - last < 0.1) return;
        last = now;
        srv = nullptr;
    }
    draw_present(srv, w, h);
    g_fps_count++;
}

bool shots_enabled() { return g_shot_interval > 0; }
HWND__* main_hwnd() { return g_hwnd; }
void request_final_shot() { g_final_shot_req = true; }

bool screenshot_due() {
    if (g_shot_interval <= 0) return false;
    double t = host_time_sec();
    return t >= g_shot_from && t < g_shot_until && t - g_shot_last >= g_shot_interval;
}

void save_screenshot(const u32* pixels, int width, int height, int stride) {
    g_shot_last = host_time_sec();
    char name[64];
    snprintf(name, sizeof(name), "/shot_%04d.bmp", g_shot_index++);
    write_bmp(g_shot_dir + name, pixels, width, height, stride);
}

extern double host_time_sec_platform();

double g_window_test_at = 0;  // --window-test=SEC: resize, fullscreen on/off (presentation robustness test)
double g_settings_test_at = 0;  // --settings-test=SEC: open the settings window (layout check)
static double g_settings_shot_at = 0;
double g_overlay_test_at = 0;
int g_overlay_test_downs = 4;  // --overlay-test=SEC[,N]: item N below the first is changed
double g_overlay_shot_at = 0;  // also --window-shot=SEC
std::vector<double> g_ram_dump_at;  // --ramdump=SEC,SEC,...
bool g_log_fps = false;             // --log-fps
double g_turbo_from = 0;            // --turbo-from=SEC: no frame limit from then on (throughput tests)
void run_message_loop() {
    MSG msg;
    g_fps_t0 = host_time_sec();
    int wt_step = 0;
    for (;;) {
        if (g_window_test_at > 0 && wt_step < 4 && host_time_sec() >= g_window_test_at + wt_step * 3) {
            if (wt_step == 0) SetWindowPos(g_hwnd, nullptr, 50, 50, 1500, 900, SWP_NOZORDER);
            else if (wt_step == 1) set_fullscreen(true);
            else if (wt_step == 2) set_fullscreen(false);
            else SetWindowPos(g_hwnd, nullptr, 80, 80, 900, 700, SWP_NOZORDER);
            log_write(LOG_MAIN, "window test step %d", wt_step);
            wt_step++;
        }
        if (!g_ram_dump_at.empty() && host_time_sec() >= g_ram_dump_at.front()) {
            // --ramdump=SEC[,SEC...]: EE main memory snapshots (analysis)
            char name[64];
            snprintf(name, sizeof name, "/ram_%03d.bin", (int)g_ram_dump_at.front());
            if (FILE* f = fopen((exe_dir() + name).c_str(), "wb")) {
                fwrite(mem::ram, 1, 32u << 20, f);
                fclose(f);
            }
            g_ram_dump_at.erase(g_ram_dump_at.begin());
        }
        if (g_overlay_test_at > 0 && host_time_sec() >= g_overlay_test_at) {
            // --overlay-test=SEC: open the in-game menu, move around, save a window screenshot
            g_overlay_test_at = 0;
            overlay_set_open(true);
            for (int k = 0; k < g_overlay_test_downs; k++) overlay_handle_key(VK_DOWN);
            overlay_handle_key(VK_LEFT);
            g_overlay_shot_at = host_time_sec() + 1.0;
        }
        if (g_overlay_shot_at > 0 && host_time_sec() >= g_overlay_shot_at) {
            g_overlay_shot_at = 0;
            g_final_shot_req = true;
        }
        if (g_settings_test_at > 0 && host_time_sec() >= g_settings_test_at) {
            g_settings_test_at = 0;
            open_settings(g_hwnd);
            g_settings_shot_at = host_time_sec() + 1.5;
        }
        if (g_settings_shot_at > 0 && host_time_sec() >= g_settings_shot_at) {
            // layout check: the settings window drawn into settings.bmp (independent of what is on top)
            g_settings_shot_at = 0;
            if (HWND sw = FindWindowW(L"Grandia3Settings", nullptr)) {
                RECT r;
                GetWindowRect(sw, &r);
                int w = r.right - r.left, h = r.bottom - r.top;
                HDC wdc = GetDC(sw);
                HDC mdc = CreateCompatibleDC(wdc);
                BITMAPINFO bi{};
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                void* bits = nullptr;
                HBITMAP bm = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
                HGDIOBJ old = SelectObject(mdc, bm);
                PrintWindow(sw, mdc, 2 /* PW_RENDERFULLCONTENT */);
                std::vector<u32> px((size_t)w * h);
                const u32* src = (const u32*)bits;
                for (size_t i = 0; i < px.size(); i++) {
                    u32 c = src[i];
                    px[i] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
                }
                write_bmp(exe_dir() + "/settings.bmp", px.data(), w, h, w);
                SelectObject(mdc, old);
                DeleteObject(bm);
                DeleteDC(mdc);
                ReleaseDC(sw, wdc);
            }
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                ExitProcess(0);
            }
            if (settings_dialog_message(&msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        overlay_poll_controller();
        if (g_gpu_mode) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 8, QS_ALLINPUT);
        } else {
            render();
        }
        double t = host_time_sec();
        if (g_turbo_from > 0 && t >= g_turbo_from) {
            g_cfg.frame_limit = 0;
            g_turbo_from = 0;
        }
        if (t - g_fps_t0 >= 1.0) {
            g_fps = g_fps_count / (t - g_fps_t0);
            if (g_log_fps) log_write(LOG_MAIN, "fps %.1f", g_fps);
            g_fps_count = 0;
            g_fps_t0 = t;
            wchar_t title[128];
            {
                // the game's frame rate (frame buffer flips), also shown in the picture
                static u64 last_flips = 0;
                static double last_t = 0;
                u64 fl = gs::flip_count();
                if (last_t > 0 && t > last_t) g_game_fps = (double)(fl - last_flips) / (t - last_t);
                last_flips = fl;
                last_t = t;
            }
            if (g_cfg.show_fps) swprintf(title, 128, L"Grandia III - %.0f fps   (F1: %ls)", g_game_fps, tr(L"Einstellungen"));
            else swprintf(title, 128, L"Grandia III   (F1: %ls)", tr(L"Einstellungen"));
            SetWindowTextW(g_hwnd, title);
        }
        if (!g_cfg.vsync && !g_gpu_mode) Sleep(1);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
struct AutoPress { double t; u32 mask; double dur; };  // bits 16..19: left stick up/right/down/left
static std::vector<AutoPress> g_autopress;

void add_autopress(const std::string& spec) {
    // spec: name:time,name:time  (button held for 0.2s)
    static const char* NAMES[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                    "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    size_t pos = 0;
    while (pos < spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            std::string n = item.substr(0, colon);
            double t = atof(item.c_str() + colon + 1);
            size_t colon2 = item.find(':', colon + 1);  // optional hold duration: name:time:seconds
            double dur = colon2 != std::string::npos ? atof(item.c_str() + colon2 + 1) : 0.25;
            for (int i = 0; i < 16; i++)
                if (n == NAMES[i]) g_autopress.push_back({t, 1u << i, dur});
            static const char* STICK[4] = {"lup", "lright", "ldown", "lleft"};
            for (int i = 0; i < 4; i++)
                if (n == STICK[i]) g_autopress.push_back({t, 1u << (16 + i), dur});
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
}

void restart_game() {
    set_vibration(0, 0);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    // same command line without per-run options that only make sense once (tests)
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    if (CreateProcessW(exe, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    ExitProcess(0);
}

void set_vibration(float small, float large) {
    XINPUT_VIBRATION v{};
    if (g_cfg.vibration) {
        v.wLeftMotorSpeed = (WORD)(large * 65535.0f);   // low-frequency motor
        v.wRightMotorSpeed = (WORD)(small * 65535.0f);  // high-frequency motor
    }
    XInputSetState(0, &v);
}

// ---------------------------------------------------------------------------
// Cutscene skip
// ---------------------------------------------------------------------------
static std::atomic<bool> g_skip{false};
static double g_skip_since = 0;
static int g_skip_session = 0;
bool skip_active() { return g_skip.load(std::memory_order_relaxed); }
int skip_session() { return g_skip_session; }
void skip_stop(const char* why) {
    if (!g_skip.exchange(false)) return;
    log_write(LOG_MAIN, "skip: end (%s) after %.1f s", why, host_time_sec() - g_skip_since);
}
static void skip_start() {
    g_skip_since = host_time_sec();
    g_skip_session++;
    g_skip = true;
    log_write(LOG_MAIN, "skip: start");
}
extern "C" int g_cinema;  // debug_hooks.cpp: cutscene camera active
extern "C" int game_flag(int n);  // debug_hooks.cpp

static PadState read_pad_now(int port);
PadState read_pad(int port) {
    // XInput is slow: one real reading per 4 ms (fast forward and the skip poll far more often)
    static PadState last{};
    static double last_t = -1;
    double now = host_time_sec();
    if (port == 0 && now - last_t < 0.004 && now >= last_t) return last;
    PadState s = read_pad_now(port);
    if (port == 0) { last = s; last_t = now; }
    return s;
}

static PadState read_pad_now(int port) {
    PadState s{};
    s.lx = s.ly = s.rx = s.ry = 0x80;
    if (port != 0 || overlay_open()) return s;
    u16 b = 0;
    u32 auto_stick = 0;
    {
        double now = host_time_sec();
        for (auto& a : g_autopress)
            if (now >= a.t && now < a.t + a.dur) {
                b |= (u16)a.mask;
                auto_stick |= a.mask >> 16;
            }
    }
    bool fg = GetForegroundWindow() == g_hwnd;
    bool key_stick = false;
    if (fg) {
        for (int i = 0; i < 16; i++)
            if (g_keys[g_cfg.keymap[i] & 0xFF]) b |= 1 << i;
        // run key held: direction keys become a fully deflected left analog stick (the game runs on
        // full stick deflection; the d-pad only walks)
        if (g_keys[g_cfg.key_run & 0xFF]) {
            int dx = ((b & 0x0020) ? 1 : 0) - ((b & 0x0080) ? 1 : 0);
            int dy = ((b & 0x0040) ? 1 : 0) - ((b & 0x0010) ? 1 : 0);
            if (dx || dy) {
                s.lx = dx < 0 ? 0x00 : dx > 0 ? 0xFF : 0x80;
                s.ly = dy < 0 ? 0x00 : dy > 0 ? 0xFF : 0x80;
                b &= ~0x00F0;
                key_stick = true;
            }
        }
    }
    XINPUT_STATE xs;
    if (XInputGetState(0, &xs) == ERROR_SUCCESS) {
        u32 w = xs.Gamepad.wButtons | (xs.Gamepad.bLeftTrigger > 64 ? 0x10000u : 0) | (xs.Gamepad.bRightTrigger > 64 ? 0x20000u : 0);
        // Back+Start together opens the settings menu: not passed to the game
        if ((w & XINPUT_GAMEPAD_BACK) && (w & XINPUT_GAMEPAD_START)) w &= ~(u32)(XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START);
        // Back+RB saves a savestate, Back+LB loads one (F5/F9 on a controller, e.g. the Steam Deck's
        // View button with R1/L1); the combination is not passed to the game
        {
            static bool rb_was = false, lb_was = false;
            bool back = (w & XINPUT_GAMEPAD_BACK) != 0;
            bool rb = back && (w & XINPUT_GAMEPAD_RIGHT_SHOULDER), lb = back && (w & XINPUT_GAMEPAD_LEFT_SHOULDER);
            if (rb && !rb_was) ss::request_save(g_cfg.state_slot);
            if (lb && !lb_was) ss::request_load(g_cfg.state_slot);
            rb_was = rb;
            lb_was = lb;
            if (rb || lb) w &= ~(u32)(XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_RIGHT_SHOULDER | XINPUT_GAMEPAD_LEFT_SHOULDER);
        }
        for (int i = 0; i < 16; i++)
            if (g_cfg.padmap[i] & w) b |= (u16)(1 << i);
        auto ax = [](SHORT v) { int r = 128 + v / 256; return (u8)(r < 0 ? 0 : r > 255 ? 255 : r); };
        auto ay = [](SHORT v) { int r = 127 - v / 256; return (u8)(r < 0 ? 0 : r > 255 ? 255 : r); };
        if (!key_stick) {
            s.lx = ax(xs.Gamepad.sThumbLX);
            s.ly = ay(xs.Gamepad.sThumbLY);
        }
        s.rx = ax(xs.Gamepad.sThumbRX);
        s.ry = ay(xs.Gamepad.sThumbRY);
    }
    if (auto_stick) {
        s.lx = (auto_stick & 2) ? 0xFF : (auto_stick & 8) ? 0x00 : 0x80;
        s.ly = (auto_stick & 4) ? 0xFF : (auto_stick & 1) ? 0x00 : 0x80;
    }
    {
        // START (fresh press) during a cutscene skips it: the game's own Scene Skip (SELECT, enabled
        // through hook_scene_skip_enabled) jumps to the end of the scene at once; a scene the game
        // marks as not skippable (flag 445) runs invisibly at high speed instead (skip_start: real
        // input ignored, START again cancels, cross pulsed to close text windows)
        static bool start_was = true;
        static double select_until = 0;
        bool start_now = (b & 0x0008) != 0, edge = start_now && !start_was;
        start_was = start_now;
        if (host_time_sec() < select_until) {
            b = (b & ~0x0008) | 0x0001;  // the injected SELECT press
        } else if (skip_active()) {
            if (edge) skip_stop("cancelled");
            double t = host_time_sec() - g_skip_since;
            b = fmod(t, 0.15) < 0.05 ? 0x4000 : 0;
            s.lx = s.ly = s.rx = s.ry = 0x80;
            if (!skip_active()) b = 0;
        } else if (edge && g_cfg.cutscene_skip && (g_cinema || kernel::flight_running()) && !kernel::battle_running() &&
                   !kernel::movie_running()) {
            b &= ~0x0008;
            if (!game_flag(445)) {
                select_until = host_time_sec() + 0.12;
                b |= 0x0001;
                log_write(LOG_MAIN, "skip: scene skip (SELECT)");
            } else {
                skip_start();
            }
        }
    }
    s.buttons = b;
    for (int i = 0; i < 12; i++) s.pressure[i] = 0;
    // pressure order: right left up down triangle circle cross square L1 R1 L2 R2
    static const u16 pbits[12] = {0x0020, 0x0080, 0x0010, 0x0040, 0x1000, 0x2000, 0x4000, 0x8000, 0x0400, 0x0800, 0x0100, 0x0200};
    for (int i = 0; i < 12; i++) s.pressure[i] = (b & pbits[i]) ? 0xFF : 0;
    return s;
}

}  // namespace platform
