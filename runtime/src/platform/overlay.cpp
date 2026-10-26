// In-game settings menu: drawn with GDI into a bitmap that the presenter blends over the game
// picture (platform.cpp draw_present). Opened with F1 or Back+Start on the controller; the game is
// paused while it is open. Changes apply at once and are saved when the menu closes.
#define UNICODE
#include "platform.h"
#include "../ee/kernel.h"
#include "../savestate.h"
#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

extern "C" int g_ee_patch[8];
namespace kernel {
extern std::atomic<bool> g_pause_req;
extern std::atomic<bool> g_pause_represent;
extern std::atomic<int> g_preview_vblanks;
}

namespace platform {

void set_fullscreen_public(bool on);
void restart_game();
HWND__* main_hwnd();

namespace {

struct Item {
    const wchar_t* label;   // nullptr: section header given in value()
    std::function<std::wstring()> value;
    std::function<void(int dir)> change;   // dir -1/+1 (left/right), 0 = confirm
    bool header = false;
};

std::vector<Item> g_items;
std::atomic<bool> g_open{false};
int g_sel = 1;
bool g_disc_confirm = false;
std::wstring g_note;
std::mutex g_mu;
std::vector<u32> g_px;
int g_w = 0, g_h = 0, g_target_h = 0;
u64 g_version = 1, g_drawn = 0;

std::wstring onoff(bool b) { return b ? L"An" : L"Aus"; }
int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void build_items() {
    Config& c = config();
    auto toggle = [](bool& b, std::function<void()> after = nullptr) {
        return [&b, after](int) { b = !b; if (after) after(); };
    };
    // options that change how the scene is drawn: the paused game runs a few frames (without
    // input) so the picture behind the menu shows the change at once
    auto drawn = [toggle](bool& b, std::function<void()> after = nullptr) {
        return toggle(b, [after] {
            if (after) after();
            kernel::g_preview_vblanks = 8;
        });
    };
    auto pct = [](int& v, int step) {
        return [&v, step](int dir) { v = clampi(v + (dir ? dir : 1) * step, 0, 100); };
    };
    g_items.clear();
    g_items.push_back({nullptr, [] { return std::wstring(L"Grafik"); }, nullptr, true});
    g_items.push_back({L"Widescreen", [&c] { return onoff(c.widescreen_hack); },
                       drawn(c.widescreen_hack, [] { apply_patches(); })});
    g_items.push_back({L"Menüs/Texte im 4:3-Format", [&c] { return onoff(c.hud_fix); }, drawn(c.hud_fix)});
    g_items.push_back({L"Große Dialogfenster", [&c] {
                           bool wanted = c.big_text == 1 || (c.big_text < 0 && is_steam_deck());
                           if (wanted && !c.widescreen_hack) return std::wstring(L"braucht Widescreen");
                           if (c.big_text >= 0) return onoff(c.big_text == 1);
                           return std::wstring(wanted ? L"Auto (an: Steam Deck)" : L"Auto (aus)");
                       },
                       [&c](int dir) {
                           // automatic -> on -> off
                           static const int ORDER[3] = {-1, 1, 0};
                           int i = c.big_text < 0 ? 0 : (c.big_text ? 1 : 2);
                           c.big_text = ORDER[(i + (dir < 0 ? 2 : 1)) % 3];
                           kernel::g_preview_vblanks = 8;
                       }});
    g_items.push_back({L"Kinobalken entfernen (experimentell)", [&c] { return onoff(c.remove_letterbox); },
                       drawn(c.remove_letterbox, [&c] { g_ee_patch[1] = g_ee_patch[2] = c.remove_letterbox; })});
    g_items.push_back({L"HD-Texturen", [&c] { return onoff(c.hd_textures); }, drawn(c.hd_textures)});
    g_items.push_back({L"Scharfe Schrift", [&c] { return onoff(c.sharp_text); }, drawn(c.sharp_text)});
    g_items.push_back({L"60 fps (experimentell)", [&c] { return onoff(c.fps60); },
                       toggle(c.fps60, [&c] { kernel::g_force60 = c.fps60; })});
    g_items.push_back({L"60 fps auch in Kämpfen (experimentell)", [&c] { return onoff(c.fps60_battle); }, toggle(c.fps60_battle)});
    g_items.push_back({L"Zwischensequenzen mit START überspringen", [&c] { return onoff(c.cutscene_skip); }, toggle(c.cutscene_skip)});
    g_items.push_back({L"Untertitel in Zwischensequenzen", [&c] { return onoff(c.subtitles); }, toggle(c.subtitles)});
    g_items.push_back({L"Filme mit START überspringen", [&c] { return onoff(c.movie_skip); },
                       toggle(c.movie_skip, [&c] { g_ee_patch[3] = c.movie_skip; })});
    g_items.push_back({L"Nachschärfen", [&c] { return c.sharpen ? std::to_wstring(c.sharpen) : std::wstring(L"Aus"); },
                       [&c](int dir) { c.sharpen = clampi(c.sharpen + (dir ? dir : 1) * 10, 0, 100); }});
    g_items.push_back({L"Bildfilter", [&c] {
                           static const wchar_t* N[3] = {L"Pixel", L"Weich", L"Scharf-weich"};
                           return std::wstring(N[clampi(c.filter, 0, 2)]);
                       },
                       [&c](int dir) { c.filter = (c.filter + (dir < 0 ? 2 : 1)) % 3; }});
    g_items.push_back({L"Seitenverhältnis", [&c] {
                           static const wchar_t* N[4] = {L"4:3", L"16:9", L"Fenster füllen", L"16:10 (Steam Deck)"};
                           return std::wstring(N[clampi(c.aspect, 0, 3)]);
                       },
                       [&c](int dir) {
                           c.aspect = (c.aspect + (dir < 0 ? 3 : 1)) % 4;
                           apply_patches();
                           kernel::g_preview_vblanks = 8;
                       }});
    g_items.push_back({L"Vollbild", [&c] { return onoff(c.fullscreen); },
                       toggle(c.fullscreen, [&c] { set_fullscreen_public(c.fullscreen); })});
    g_items.push_back({L"VSync", [&c] { return onoff(c.vsync); }, toggle(c.vsync)});
    g_items.push_back({L"Bildrate anzeigen", [&c] { return onoff(c.show_fps); }, toggle(c.show_fps)});
    g_items.push_back({L"Interne Auflösung (ab Neustart)", [&c] {
                           return c.internal_scale ? std::to_wstring(c.internal_scale) + L"x" : std::wstring(tr(L"Automatisch"));
                       },
                       [&c](int dir) { c.internal_scale = (c.internal_scale + (dir < 0 ? 8 : 1)) % 9; }});
    g_items.push_back({nullptr, [] { return std::wstring(L"Audio"); }, nullptr, true});
    g_items.push_back({L"Lautstärke gesamt", [&c] { return std::to_wstring(c.volume) + L" %"; }, pct(c.volume, 5)});
    g_items.push_back({L"Musik", [&c] { return std::to_wstring(c.vol_music) + L" %"; }, pct(c.vol_music, 5)});
    g_items.push_back({L"Sprache", [&c] { return std::to_wstring(c.vol_voice) + L" %"; }, pct(c.vol_voice, 5)});
    g_items.push_back({L"Effekte", [&c] { return std::to_wstring(c.vol_sfx) + L" %"; }, pct(c.vol_sfx, 5)});
    g_items.push_back({L"Videos", [&c] { return std::to_wstring(c.vol_movie) + L" %"; }, pct(c.vol_movie, 5)});
    g_items.push_back({nullptr, [] { return std::wstring(L"System"); }, nullptr, true});
    g_items.push_back({L"Sprache (Language)", [&c] { return language_name(c.language); }, [&c](int dir) {
                           std::vector<std::string> l = languages();
                           size_t i = 0;
                           while (i < l.size() && l[i] != c.language) i++;
                           if (i == l.size()) i = 0;
                           c.language = l[(i + (dir < 0 ? l.size() - 1 : 1)) % l.size()];
                           g_note = L"Die Spieltexte wechseln beim nächsten Start.";
                       }});
    g_items.push_back({L"Disc wechseln (Neustart)", [&c] {
                           if (g_disc_confirm) return std::wstring(L"Nochmal bestätigen!");
                           return std::wstring(c.disc == 2 ? L"Disc 2 → Disc 1" : L"Disc 1 → Disc 2");
                       },
                       [&c](int dir) {
                           if (dir != 0) return;
                           if (!g_disc_confirm) { g_disc_confirm = true; g_note = L"Nicht gespeicherter Fortschritt geht verloren."; return; }
                           c.disc = c.disc == 2 ? 1 : 2;
                           save_config();
                           restart_game();
                       }});
    g_items.push_back({nullptr, [] { return std::wstring(L"Savestates (Spielstand an jeder Stelle)"); }, nullptr, true});
    g_items.push_back({L"Slot", [&c] {
                           std::string when;
                           std::wstring v = std::to_wstring(c.state_slot);
                           if (ss::slot_exists(c.state_slot, &when)) v += L"  (" + std::wstring(when.begin(), when.end()) + L")";
                           else v += tr(L"  (leer)");
                           return v;
                       },
                       [&c](int dir) { c.state_slot = (c.state_slot - 1 + (dir < 0 ? 8 : 1)) % 9 + 1; }});
    g_items.push_back({L"Savestate speichern (F5, Back+R1)", [] { return std::wstring(L"speichern"); }, [&c](int dir) {
                           if (dir != 0) return;
                           overlay_set_open(false);
                           ss::request_save(c.state_slot);
                       }});
    g_items.push_back({L"Savestate laden (F9, Back+L1)", [] { return std::wstring(L"laden"); }, [&c](int dir) {
                           if (dir != 0) return;
                           overlay_set_open(false);
                           ss::request_load(c.state_slot);
                       }});
    g_items.push_back({L"Tasten-/Controllerbelegung …", [] { return std::wstring(L"öffnen"); }, [](int dir) {
                           if (dir != 0) return;
                           overlay_set_open(false);
                           open_settings(main_hwnd());
                       }});
    g_items.push_back({L"Zurück zum Spiel", [] { return std::wstring(); }, [](int dir) {
                           if (dir == 0) overlay_set_open(false);
                       }});
}

// GDI rendering of the panel at the requested height (BGRA, straight alpha)
void render_locked() {
    if (g_items.empty()) build_items();
    int rows = (int)g_items.size();
    const int BASE_W = 760, ROW = 30, TOP = 74, BOTTOM = 70;
    int base_h = TOP + rows * ROW + BOTTOM;
    double s = g_target_h > 0 ? (double)g_target_h / base_h : 1.0;
    if (s < 0.6) s = 0.6;
    int W = (int)(BASE_W * s), H = (int)(base_h * s);
    auto S = [s](int v) { return (int)(v * s + 0.5); };
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldbm = SelectObject(dc, bm);
    RECT all{0, 0, W, H};
    HBRUSH bg = CreateSolidBrush(RGB(18, 22, 34));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    RECT frame{S(6), S(6), W - S(6), H - S(6)};
    HBRUSH fr = CreateSolidBrush(RGB(150, 130, 70));
    FrameRect(dc, &frame, fr);
    DeleteObject(fr);
    SetBkMode(dc, TRANSPARENT);
    HFONT ftitle = CreateFontW(-S(30), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT frow = CreateFontW(-S(20), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT fhead = CreateFontW(-S(17), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT fsmall = CreateFontW(-S(16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SelectObject(dc, ftitle);
    SetTextColor(dc, RGB(235, 215, 150));
    RECT tr{S(28), S(16), W - S(28), S(60)};
    DrawTextW(dc, platform::tr(L"Einstellungen"), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    for (int i = 0; i < rows; i++) {
        const Item& it = g_items[i];
        int y = TOP + i * ROW;
        RECT r{S(20), S(y), W - S(20), S(y + ROW)};
        if (it.header) {
            SelectObject(dc, fhead);
            SetTextColor(dc, RGB(150, 170, 210));
            RECT hr = r;
            hr.left = S(28);
            DrawTextW(dc, platform::tr(it.value()).c_str(), -1, &hr, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            continue;
        }
        if (i == g_sel) {
            HBRUSH sb = CreateSolidBrush(RGB(60, 70, 110));
            FillRect(dc, &r, sb);
            DeleteObject(sb);
        }
        SelectObject(dc, frow);
        SetTextColor(dc, i == g_sel ? RGB(255, 255, 255) : RGB(205, 210, 220));
        RECT lr{S(40), S(y), W - S(260), S(y + ROW)};
        DrawTextW(dc, platform::tr(it.label), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        std::wstring v = it.value();
        if (!v.empty()) {
            bool action = v == L"öffnen" || v == L"speichern" || v == L"laden";
            v = platform::tr(v);
            if (i == g_sel && it.change && !action) v = L"‹  " + v + L"  ›";
            SetTextColor(dc, i == g_sel ? RGB(255, 225, 140) : RGB(220, 200, 140));
            RECT vr{W - S(260), S(y), W - S(40), S(y + ROW)};
            DrawTextW(dc, v.c_str(), -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(dc, fsmall);
    SetTextColor(dc, RGB(160, 165, 180));
    RECT nr{S(28), H - S(BOTTOM) + S(6), W - S(28), H - S(BOTTOM) + S(30)};
    DrawTextW(dc, platform::tr(g_note).c_str(), -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    RECT hr{S(28), H - S(40), W - S(28), H - S(14)};
    DrawTextW(dc, platform::tr(L"↑↓ Auswahl    ←→ ändern    Kreuz/Enter: auswählen    Kreis/Esc/F1: schließen"), -1, &hr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    GdiFlush();
    g_px.resize((size_t)W * H);
    const u32* src = (const u32*)bits;
    for (size_t k = 0; k < g_px.size(); k++) g_px[k] = (src[k] & 0xFFFFFF) | 0xEE000000u;  // slightly translucent
    g_w = W;
    g_h = H;
    SelectObject(dc, oldbm);
    DeleteObject(bm);
    DeleteDC(dc);
    DeleteObject(ftitle);
    DeleteObject(frow);
    DeleteObject(fhead);
    DeleteObject(fsmall);
    g_version++;
}

void move_sel(int d) {
    int n = (int)g_items.size();
    do { g_sel = (g_sel + d + n) % n; } while (g_items[g_sel].header);
    if (g_disc_confirm) { g_disc_confirm = false; g_note.clear(); }
}

void invoke(int dir) {
    Item& it = g_items[g_sel];
    if (!it.change) return;
    it.change(dir);
}

}  // namespace

bool overlay_open() { return g_open.load(std::memory_order_acquire); }

void overlay_set_open(bool on) {
    if (on == g_open.load()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    if (on) {
        build_items();
        if (g_sel <= 0 || g_sel >= (int)g_items.size() || g_items[g_sel].header) g_sel = 1;
        g_disc_confirm = false;
        g_note.clear();
        g_drawn = 0;
        g_open = true;
        kernel::g_pause_represent = true;
        kernel::g_pause_req = true;
    } else {
        g_open = false;
        kernel::g_pause_represent = false;
        kernel::g_pause_req = false;
        save_config();
    }
    g_version++;
}

bool overlay_handle_key(int vk) {
    if (!overlay_open()) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    switch (vk) {
    case VK_UP: move_sel(-1); break;
    case VK_DOWN: move_sel(1); break;
    case VK_LEFT: invoke(-1); break;
    case VK_RIGHT: invoke(1); break;
    case VK_RETURN: case VK_SPACE: {
        // actions run without the lock held (they may close the menu or open windows)
        Item it = g_items[g_sel];
        g_mu.unlock();
        if (it.change) it.change(0);
        g_mu.lock();
        break;
    }
    case VK_ESCAPE: case VK_F1: case VK_BACK:
        g_mu.unlock();
        overlay_set_open(false);
        g_mu.lock();
        return true;
    default: return true;
    }
    g_version++;
    return true;
}

// controller: Back+Start opens; D-pad/stick, A (cross) and B (circle) operate the menu
void overlay_poll_controller() {
    static WORD prev = 0;
    static double next_repeat = 0;
    XINPUT_STATE xs;
    if (XInputGetState(0, &xs) != ERROR_SUCCESS) { prev = 0; return; }
    WORD b = xs.Gamepad.wButtons;
    if (xs.Gamepad.sThumbLY > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
    if (xs.Gamepad.sThumbLY < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (xs.Gamepad.sThumbLX < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (xs.Gamepad.sThumbLX > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
    WORD fresh = b & ~prev;
    double t = host_time_sec();
    const WORD dirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
    if (fresh & dirs) next_repeat = t + 0.35;
    else if ((b & dirs) && t >= next_repeat) { fresh |= b & dirs; next_repeat = t + 0.09; }
    prev = b;
    bool combo = (b & XINPUT_GAMEPAD_BACK) && (b & XINPUT_GAMEPAD_START) && (fresh & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START));
    if (!overlay_open()) {
        if (combo) overlay_set_open(true);
        return;
    }
    if (combo || (fresh & XINPUT_GAMEPAD_B)) { overlay_set_open(false); return; }
    if (fresh & XINPUT_GAMEPAD_DPAD_UP) overlay_handle_key(VK_UP);
    if (fresh & XINPUT_GAMEPAD_DPAD_DOWN) overlay_handle_key(VK_DOWN);
    if (fresh & XINPUT_GAMEPAD_DPAD_LEFT) overlay_handle_key(VK_LEFT);
    if (fresh & XINPUT_GAMEPAD_DPAD_RIGHT) overlay_handle_key(VK_RIGHT);
    if (fresh & XINPUT_GAMEPAD_A) overlay_handle_key(VK_RETURN);
}

// note shown while a cutscene is skipped (GDI, BGRA)
static bool skip_banner(int target_h, std::vector<u32>& out, int& w, int& h, u64& version) {
    static std::vector<u32> px;
    static int bw = 0, bh = 0, for_h = 0;
    if (for_h != target_h || px.empty()) {
        double s = target_h / 860.0;
        if (s < 0.6) s = 0.6;
        int W = (int)(560 * s), H = (int)(110 * s);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldbm = SelectObject(dc, bm);
        RECT all{0, 0, W, H};
        HBRUSH bg = CreateSolidBrush(RGB(18, 22, 34));
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT f1 = CreateFontW(-(int)(30 * s), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT f2 = CreateFontW(-(int)(18 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(dc, f1);
        SetTextColor(dc, RGB(235, 215, 150));
        RECT r1{0, (int)(14 * s), W, (int)(62 * s)};
        DrawTextW(dc, platform::tr(L"Zwischensequenz wird übersprungen …"), -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, f2);
        SetTextColor(dc, RGB(160, 165, 180));
        RECT r2{0, (int)(62 * s), W, (int)(96 * s)};
        DrawTextW(dc, platform::tr(L"START: abbrechen"), -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
        px.resize((size_t)W * H);
        const u32* src = (const u32*)bits;
        for (size_t k = 0; k < px.size(); k++) px[k] = (src[k] & 0xFFFFFF) | 0xFF000000u;
        SelectObject(dc, oldbm);
        DeleteObject(bm);
        DeleteDC(dc);
        DeleteObject(f1);
        DeleteObject(f2);
        bw = W;
        bh = H;
        for_h = target_h;
    }
    const u64 BANNER_VERSION = 0x5C1900000000ull + (u64)for_h;
    if (version != BANNER_VERSION || out.size() != px.size()) out = px;
    w = bw;
    h = bh;
    version = BANNER_VERSION;
    return true;
}

// short note at the top of the picture (savestates)
static std::mutex g_nmu;
static std::wstring g_n1, g_n2;
static double g_nuntil = 0;
static u64 g_nver = 1;
void show_notice(const std::wstring& line1, const std::wstring& line2, double seconds) {
    std::lock_guard<std::mutex> lk(g_nmu);
    g_n1 = line1;
    g_n2 = line2;
    g_nuntil = host_time_sec() + seconds;
    g_nver++;
}
bool notice_active() { return host_time_sec() < g_nuntil; }

static bool notice_banner(int target_h, std::vector<u32>& out, int& w, int& h, u64& version) {
    static std::vector<u32> px;
    static int bw = 0, bh = 0, for_h = 0;
    static u64 for_ver = 0;
    std::wstring l1, l2;
    u64 ver;
    {
        std::lock_guard<std::mutex> lk(g_nmu);
        l1 = g_n1;
        l2 = g_n2;
        ver = g_nver;
    }
    if (for_h != target_h || for_ver != ver || px.empty()) {
        double s = target_h / 860.0;
        if (s < 0.6) s = 0.6;
        int W = (int)(520 * s), H = (int)((l2.empty() ? 60 : 96) * s);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldbm = SelectObject(dc, bm);
        RECT all{0, 0, W, H};
        HBRUSH bg = CreateSolidBrush(RGB(18, 22, 34));
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT f1 = CreateFontW(-(int)(26 * s), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT f2 = CreateFontW(-(int)(18 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(dc, f1);
        SetTextColor(dc, RGB(235, 215, 150));
        RECT r1{0, (int)(10 * s), W, (int)(50 * s)};
        DrawTextW(dc, l1.c_str(), -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (!l2.empty()) {
            SelectObject(dc, f2);
            SetTextColor(dc, RGB(170, 175, 190));
            RECT r2{0, (int)(50 * s), W, (int)(84 * s)};
            DrawTextW(dc, l2.c_str(), -1, &r2, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        GdiFlush();
        px.resize((size_t)W * H);
        const u32* src = (const u32*)bits;
        for (size_t k = 0; k < px.size(); k++) px[k] = (src[k] & 0xFFFFFF) | 0xE8000000u;
        SelectObject(dc, oldbm);
        DeleteObject(bm);
        DeleteDC(dc);
        DeleteObject(f1);
        DeleteObject(f2);
        bw = W;
        bh = H;
        for_h = target_h;
        for_ver = ver;
    }
    const u64 V = 0x707100000000ull + (for_ver << 16) + (u64)for_h;
    if (version != V || out.size() != px.size()) out = px;
    w = bw;
    h = bh;
    version = V;
    return true;
}

// frame rate label (option show_fps), drawn again when the number changes
bool fps_pixels(int target_h, int fps, std::vector<u32>& out, int& w, int& h, u64& version) {
    static std::vector<u32> px;
    static int bw = 0, bh = 0, for_h = 0, for_fps = -1;
    if (for_h != target_h || for_fps != fps || px.empty()) {
        double s = target_h / 860.0;
        if (s < 0.6) s = 0.6;
        int W = (int)(118 * s), H = (int)(40 * s);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldbm = SelectObject(dc, bm);
        RECT all{0, 0, W, H};
        HBRUSH bg = CreateSolidBrush(RGB(18, 22, 34));
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        HFONT f = CreateFontW(-(int)(24 * s), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        SelectObject(dc, f);
        // green at the target rate, yellow a bit below, red when clearly slower
        SetTextColor(dc, fps >= 57 || (fps >= 28 && fps <= 31) ? RGB(130, 230, 140) : fps >= 45 ? RGB(240, 210, 110) : RGB(240, 120, 110));
        wchar_t t[32];
        swprintf(t, 32, L"%d FPS", fps);
        RECT r{0, 0, W, H};
        DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        GdiFlush();
        px.resize((size_t)W * H);
        const u32* src = (const u32*)bits;
        for (size_t k = 0; k < px.size(); k++) px[k] = (src[k] & 0xFFFFFF) | 0xC0000000u;
        SelectObject(dc, oldbm);
        DeleteObject(bm);
        DeleteDC(dc);
        DeleteObject(f);
        bw = W;
        bh = H;
        for_h = target_h;
        for_fps = fps;
    }
    const u64 V = 0xF95000000000ull + ((u64)(u32)for_fps << 16) + (u64)for_h;
    if (version != V || out.size() != px.size()) out = px;
    w = bw;
    h = bh;
    version = V;
    return true;
}

bool overlay_pixels(int target_h, std::vector<u32>& out, int& w, int& h, u64& version) {
    if (!overlay_open()) {
        if (skip_active()) return skip_banner(target_h, out, w, h, version);
        return notice_active() && notice_banner(target_h, out, w, h, version);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (target_h != g_target_h || g_drawn != g_version) {
        g_target_h = target_h;
        render_locked();
        g_drawn = g_version;
    }
    if (version != g_version || out.size() != g_px.size()) out = g_px;  // the caller passes its last version
    w = g_w;
    h = g_h;
    version = g_version;
    return true;
}

}  // namespace platform
