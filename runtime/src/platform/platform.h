#pragma once
#include "../common.h"
#include <atomic>
#include <string>
#include <vector>

extern "C" int g_ee_patch[8];
struct ID3D11Device;
struct HWND__;
struct tagMSG;
struct ID3D11DeviceContext;
struct ID3D11ShaderResourceView;

namespace platform {

struct Config {
    // video
    int window_width = 1280;
    int window_height = 960;
    bool fullscreen = false;
    bool vsync = true;
    int aspect = 0;          // 0 = 4:3, 1 = 16:9 (stretch for widescreen hack), 2 = stretch to window, 3 = 16:10 (Steam Deck),
                             // 4 = shape of the window (ultrawide: the widescreen view follows it)
    int filter = 1;          // 0 = nearest, 1 = bilinear, 2 = sharp bilinear
    int internal_scale = 0;  // GPU renderer: internal resolution multiplier (0 = automatic from the screen height)
    int renderer = 1;        // 1 = GPU (Direct3D 11.3), 0 = software
    bool widescreen_hack = false;
    bool remove_letterbox = false;
    bool vibration = true;          // controller rumble
    bool camera_invert_x = false;   // right stick left/right reversed (field camera turns the other way)
    int disc = 1;                   // which disc is "inserted" (1 or 2); switching restarts the game
    bool lod_scale = true;          // GPU renderer: texture detail level follows the internal resolution
    bool hud_fix = true;            // widescreen: 2D elements keep their 4:3 proportions (GPU renderer)  // no black bars in cutscenes and battles (PCSX2 patch, marked "may cause error")
    bool hd_textures = true;        // GPU renderer: replacement images from the "textures" folder
    bool sharp_text = true;         // GPU renderer: glyphs get sharp contours at high resolution
    bool texture_dump = false;      // GPU renderer: write every new texture to "texdump"
    float hd_lod_bias = -0.75f;       // HD textures: mip level bias (negative = sharper, more shimmer)
    bool fps60 = false;             // 60 frames per second in field and battle (experimental, F3 toggles)
    bool fps60_battle = false;      // with fps60: battles at 60 too (experimental)
    bool movie_skip = true;         // START/SELECT skip every movie (patch 3)
    bool cutscene_skip = true;      // START skips in-engine cutscenes (fast invisible run)
    bool subtitles = true;          // subtitles for voiced cutscenes (lang\sub_<language>.tsv)
    int state_slot = 1;             // savestate slot for F5 (save) / F9 (load), 1..9
    int sharpen = 60;               // contrast adaptive sharpening of the output, 0 = off .. 100 (F4 toggles)
    bool show_fps = false;
    bool integer_scaling = false;
    int frame_limit = 1;     // 1 = 60 Hz pacing, 0 = unlimited (fast forward)
    bool gs_thread = true;
    std::string language = "en";  // game text and interface: "en" (original English text) or a lang\<code>.tsv
    int big_text = -1;       // larger conversation windows (widescreen): -1 = automatic (on with a Steam Deck), 0 = off, 1 = on
    float text_scale = 1.25f;  // their size factor (1.0 .. 1.6)   // VIF1/VU1/GS front end on a thread of their own (gspipe.h)
    // audio
    bool audio_enabled = true;
    int volume = 100;        // 0..100
    int vol_music = 100, vol_voice = 100, vol_sfx = 100, vol_movie = 100;  // per category, 0..100
    // misc
    bool console = false;
    std::string iso_path;   // disc images of the player's own copy (UTF-8): disc 1, disc 2
    std::string iso_path2;
    std::string save_dir;
    // input: virtual-key codes for PS2 buttons
    int keymap[16];
    int key_run = 0x10;  // held: direction keys drive the left analog stick fully (run) instead of the d-pad
    // controller (XInput) source of each PS2 button: XINPUT_GAMEPAD_* bit, 0x10000 = LT, 0x20000 = RT, 0 = none
    u32 padmap[16];
};

Config& config();
// interface language (i18n.cpp): texts are written in German, tr() gives the English version
bool ui_german();
const wchar_t* tr(const wchar_t* de);
std::wstring tr(const std::wstring& de);
std::vector<std::string> languages();           // "en" + every lang\<code>.tsv
std::wstring language_name(const std::string& code);
bool is_steam_deck();     // running on a Steam Deck (Proton): environment, BIOS product name or GPU
std::string steam_deck_info();
extern bool g_force_steam_deck;  // --steamdeck
bool big_text_active();   // option big_text in effect (needs widescreen)
float text_scale();       // conversation window scale in effect (1 when off)
void apply_patches();     // optional game code patches from the settings (widescreen 16:9 / 16:10)
float view_aspect();      // aspect ratio of the widescreen 3D view (16:9, 16:10 or the window's)
float hud_factor();       // widescreen with 4:3 menus: horizontal factor of 2D elements (3/4, 5/6 for 16:10)
void load_config(const std::string& path);
void save_config();
void init_console(bool enable);
bool console_enabled();
void fatal_message(const char* msg);
std::string choose_disc_image(int disc);  // first start: file dialog for the player's disc image (UTF-8 path, "" = cancelled)
void message(const wchar_t* text);        // information box

// Video
bool init_video();
void run_message_loop();
// Called by the emulation thread with a finished frame (RGBA8, width x height).
void submit_frame(const u32* pixels, int width, int height, int stride_pixels);
bool window_closed();
void set_screenshot_interval(double sec, const std::string& dir);
void set_screenshot_window(double from, double until);

// GPU renderer support: the renderer thread owns the device context and presents its own frames
ID3D11Device* d3d_device();
ID3D11DeviceContext* d3d_context();
void set_gpu_present(bool on);
void present_texture(ID3D11ShaderResourceView* srv, int w, int h);
bool screenshot_due();
extern bool g_shots_final;     // --shots-final: screenshots of the window output (after scaling and sharpening)
void request_final_shot();     // save the next presented window image as a screenshot
bool shots_enabled();
extern double g_window_test_at;
extern double g_settings_test_at;
extern double g_overlay_test_at;
extern int g_overlay_test_downs;
extern double g_overlay_shot_at;
extern std::vector<double> g_ram_dump_at;
extern bool g_log_fps;
extern double g_turbo_from;
void save_screenshot(const u32* pixels, int width, int height, int stride);

// Input: PS2 digital buttons (active-high bitmask in libpad order) + analog sticks
struct PadState {
    u16 buttons;     // bit set = pressed (SELECT=0x0001 ... SQUARE=0x8000, libpad order)
    u8 rx, ry, lx, ly;
    u8 pressure[12];
};
PadState read_pad(int port);
void set_vibration(float small, float large);  // DualShock motors -> XInput controller 1
void default_input(Config& c);
void open_settings(HWND__* parent);

// in-game settings menu (overlay.cpp)
bool overlay_open();
// cutscene skip: START during a cutscene runs it invisibly at high speed until it ends
bool skip_active();
int skip_session();  // counts skip starts
void skip_stop(const char* why);
// short note at the top of the picture (savestates), any thread
void show_notice(const std::wstring& line1, const std::wstring& line2, double seconds);
bool notice_active();
void overlay_set_open(bool on);
bool overlay_handle_key(int vk);      // window thread, key down while the menu is open
void overlay_poll_controller();       // window thread, every loop iteration
// panel pixels (BGRA) for a target height; false when closed. Copies only when version changed.
bool overlay_pixels(int target_h, std::vector<u32>& out, int& w, int& h, u64& version);
bool fps_pixels(int target_h, int fps, std::vector<u32>& out, int& w, int& h, u64& version);  // frame rate label
HWND__* main_hwnd();
bool settings_open();
bool settings_dialog_message(tagMSG* m);
void set_fullscreen_public(bool fs);
void restart_game();  // start a new instance with the current settings and end this one
void add_autopress(const std::string& spec);

}  // namespace platform
