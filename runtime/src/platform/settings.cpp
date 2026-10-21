// Settings window (F1): graphics, audio and button assignment for keyboard and controller.
// Runs on the UI thread; the emulation pauses while it is open.
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include "platform.h"
#include "../ee/kernel.h"
#include <windows.h>
#include <commctrl.h>
#include <xinput.h>
#include <cstdio>
#include <string>

namespace kernel { extern std::atomic<bool> g_pause_req; }

namespace platform {

void set_fullscreen_public(bool fs);

// display order of the PS2 buttons (index into libpad bit order), last row: run modifier
static const int ROW_BIT[17] = {14, 13, 15, 12, 10, 11, 8, 9, 3, 0, 4, 6, 7, 5, 1, 2, -1};
static const wchar_t* ROW_NAME[17] = {L"Kreuz (\x2715)", L"Kreis (\x25CB)", L"Quadrat (\x25A1)", L"Dreieck (\x25B3)", L"L1", L"R1", L"L2", L"R2",
                                      L"Start", L"Select", L"Steuerkreuz hoch", L"Steuerkreuz runter", L"Steuerkreuz links",
                                      L"Steuerkreuz rechts", L"L3 (linker Stick drücken)", L"R3 (rechter Stick drücken)",
                                      L"Rennen (gedrückt halten)"};

struct PadSrc { u32 code; const wchar_t* name; };
static const PadSrc PAD_SOURCES[] = {
    {0x1000, L"A"}, {0x2000, L"B"}, {0x4000, L"X"}, {0x8000, L"Y"}, {0x0100, L"LB"}, {0x0200, L"RB"}, {0x10000, L"LT"},
    {0x20000, L"RT"}, {0x0010, L"Start"}, {0x0020, L"Back"}, {0x0001, L"Steuerkreuz hoch"}, {0x0002, L"Steuerkreuz runter"},
    {0x0004, L"Steuerkreuz links"}, {0x0008, L"Steuerkreuz rechts"}, {0x0040, L"Linker Stick drücken"},
    {0x0080, L"Rechter Stick drücken"},
};

std::wstring pad_source_name(u32 code) {
    if (!code) return L"–";
    for (const PadSrc& p : PAD_SOURCES)
        if (p.code == code) return tr(p.name);
    return L"?";
}

static std::wstring key_name(int vk) {
    if (!vk) return L"–";
    UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    switch (vk) {
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: case VK_PRIOR: case VK_NEXT: case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE: case VK_DIVIDE: case VK_NUMLOCK: case VK_RCONTROL: case VK_RMENU:
        sc |= 0x100;
        break;
    }
    wchar_t b[64];
    if (sc && GetKeyNameTextW((LONG)(sc << 16), b, 64) > 0) return b;
    swprintf(b, 64, tr(L"Taste 0x%02X"), vk);
    return b;
}

enum {
    ID_RENDERER = 100, ID_SCALE, ID_ASPECT, ID_FILTER, ID_WIDESCREEN, ID_FULLSCREEN, ID_VSYNC, ID_FPS, ID_VOLUME, ID_VOLLABEL,
    ID_LIST, ID_LETTERBOX, ID_HUD, ID_HDTEX, ID_FPS60, ID_SHARP, ID_SHARPLABEL, ID_CATVOL0, ID_CATLABEL0 = ID_CATVOL0 + 4, ID_DISC = ID_CATLABEL0 + 4, ID_SETKEY, ID_SETPAD, ID_CLEARPAD, ID_DEFAULTS, ID_SAVE, ID_CANCEL, ID_STATUS, ID_TIMER = 1
};

static HWND g_wnd = nullptr, g_parent = nullptr;
static HWND g_disc;
static HWND g_catvol[4], g_catlabel[4];  // music, speech, sound effects, movies
static HWND g_list, g_status, g_renderer, g_scale, g_aspect, g_filter, g_ws, g_lb, g_hud, g_hdtex, g_fps60, g_sharp, g_sharplabel, g_fs, g_vsync, g_fps, g_vol, g_vollabel;
static Config g_edit;
static int g_capture = 0;  // 1 keyboard, 2 controller
static int g_capture_row = -1;
static bool g_prev_keys[256];
static u32 g_prev_pad = 0;
static HFONT g_font = nullptr;
static int g_dpi = 96;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static void fill_list() {
    ListView_DeleteAllItems(g_list);
    for (int r = 0; r < 17; r++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = r;
        it.pszText = (LPWSTR)tr(ROW_NAME[r]);
        ListView_InsertItem(g_list, &it);
        int bit = ROW_BIT[r];
        std::wstring k = key_name(bit >= 0 ? g_edit.keymap[bit] : g_edit.key_run);
        std::wstring p = bit >= 0 ? pad_source_name(g_edit.padmap[bit]) : std::wstring(tr(L"Stick ganz durchdrücken"));
        ListView_SetItemText(g_list, r, 1, (LPWSTR)k.c_str());
        ListView_SetItemText(g_list, r, 2, (LPWSTR)p.c_str());
    }
}

static void set_status(const wchar_t* t) { SetWindowTextW(g_status, t); }

static int selected_row() { return ListView_GetNextItem(g_list, -1, LVNI_SELECTED); }

static void start_capture(int kind) {
    int r = selected_row();
    if (r < 0) { set_status(tr(L"Bitte zuerst eine Zeile in der Liste auswählen.")); return; }
    if (kind == 2 && ROW_BIT[r] < 0) { set_status(tr(L"Rennen geht mit dem Controller über den Analogstick.")); return; }
    g_capture = kind;
    g_capture_row = r;
    for (int vk = 0; vk < 256; vk++) g_prev_keys[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
    XINPUT_STATE xs;
    g_prev_pad = ~0u;  // ignore buttons already held
    if (XInputGetState(0, &xs) == ERROR_SUCCESS)
        g_prev_pad = xs.Gamepad.wButtons | (xs.Gamepad.bLeftTrigger > 64 ? 0x10000u : 0) | (xs.Gamepad.bRightTrigger > 64 ? 0x20000u : 0);
    std::wstring t = std::wstring(kind == 1 ? tr(L"Drücke jetzt die Taste für \"") : tr(L"Drücke jetzt die Controller-Taste für \"")) +
                     tr(ROW_NAME[r]) + tr(L"\"  (Esc = abbrechen)");
    set_status(t.c_str());
}

static void poll_capture() {
    if (!g_capture) return;
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        g_capture = 0;
        set_status(tr(L"Abgebrochen."));
        return;
    }
    if (g_capture == 1) {
        for (int vk = 8; vk < 255; vk++) {
            bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            bool was = g_prev_keys[vk];
            g_prev_keys[vk] = down;
            if (!down || was) continue;
            if (vk >= VK_LSHIFT && vk <= VK_RMENU) continue;  // the generic VK_SHIFT/CONTROL/MENU codes are used
            if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_F1 || vk == VK_ESCAPE) continue;
            int bit = ROW_BIT[g_capture_row];
            if (bit >= 0) g_edit.keymap[bit] = vk;
            else g_edit.key_run = vk;
            g_capture = 0;
            fill_list();
            ListView_SetItemState(g_list, g_capture_row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            set_status((tr(L"Zugewiesen: ") + key_name(vk)).c_str());
            return;
        }
    } else {
        XINPUT_STATE xs;
        if (XInputGetState(0, &xs) != ERROR_SUCCESS) {
            set_status(tr(L"Kein Controller gefunden (XInput, Anschluss 1).  Esc = abbrechen"));
            return;
        }
        u32 now = xs.Gamepad.wButtons | (xs.Gamepad.bLeftTrigger > 64 ? 0x10000u : 0) | (xs.Gamepad.bRightTrigger > 64 ? 0x20000u : 0);
        if (g_prev_pad == ~0u) g_prev_pad = now;
        u32 fresh = now & ~g_prev_pad;
        g_prev_pad = now;
        for (const PadSrc& p : PAD_SOURCES)
            if (fresh & p.code) {
                g_edit.padmap[ROW_BIT[g_capture_row]] = p.code;
                g_capture = 0;
                fill_list();
                ListView_SetItemState(g_list, g_capture_row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                set_status((std::wstring(tr(L"Zugewiesen: ")) + tr(p.name)).c_str());
                return;
            }
    }
}

static void update_sharp_label() {
    int v = (int)SendMessageW(g_sharp, TBM_GETPOS, 0, 0);
    wchar_t b[32];
    if (v == 0) swprintf(b, 32, tr(L"aus"));
    else swprintf(b, 32, L"%d", v);
    SetWindowTextW(g_sharplabel, b);
}

static void update_cat_label(int i) {
    wchar_t b[32];
    swprintf(b, 32, L"%d %%", (int)SendMessageW(g_catvol[i], TBM_GETPOS, 0, 0));
    SetWindowTextW(g_catlabel[i], b);
}

static void update_vol_label() {
    wchar_t b[32];
    swprintf(b, 32, L"%d %%", (int)SendMessageW(g_vol, TBM_GETPOS, 0, 0));
    SetWindowTextW(g_vollabel, b);
}

static void apply_and_close() {
    Config& c = config();
    bool restart;
    g_edit.renderer = (int)SendMessageW(g_renderer, CB_GETCURSEL, 0, 0) == 0 ? 1 : 0;
    g_edit.internal_scale = (int)SendMessageW(g_scale, CB_GETCURSEL, 0, 0);
    if (g_edit.internal_scale == 7) g_edit.internal_scale = 8;
    restart = c.renderer != g_edit.renderer || c.internal_scale != g_edit.internal_scale;
    g_edit.aspect = (int)SendMessageW(g_aspect, CB_GETCURSEL, 0, 0);
    g_edit.filter = (int)SendMessageW(g_filter, CB_GETCURSEL, 0, 0);
    g_edit.widescreen_hack = SendMessageW(g_ws, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edit.remove_letterbox = SendMessageW(g_lb, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edit.hud_fix = SendMessageW(g_hud, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edit.hd_textures = SendMessageW(g_hdtex, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edit.fps60 = SendMessageW(g_fps60, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edit.sharpen = (int)SendMessageW(g_sharp, TBM_GETPOS, 0, 0);
    g_edit.fullscreen = SendMessageW(g_fs, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edit.vsync = SendMessageW(g_vsync, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edit.show_fps = SendMessageW(g_fps, BM_GETCHECK, 0, 0) == BST_CHECKED;
    g_edit.volume = (int)SendMessageW(g_vol, TBM_GETPOS, 0, 0);
    g_edit.vol_music = (int)SendMessageW(g_catvol[0], TBM_GETPOS, 0, 0);
    g_edit.vol_voice = (int)SendMessageW(g_catvol[1], TBM_GETPOS, 0, 0);
    g_edit.vol_sfx = (int)SendMessageW(g_catvol[2], TBM_GETPOS, 0, 0);
    g_edit.vol_movie = (int)SendMessageW(g_catvol[3], TBM_GETPOS, 0, 0);
    int disc = (int)SendMessageW(g_disc, CB_GETCURSEL, 0, 0) == 1 ? 2 : 1;
    bool disc_changed = disc != c.disc;
    g_edit.disc = disc;
    // renderer and resolution apply at the next start (only read when the renderer starts)
    bool fs_changed = c.fullscreen != g_edit.fullscreen;
    c = g_edit;
    save_config();
    apply_patches();
    kernel::g_force60 = c.fps60;
    g_ee_patch[1] = g_ee_patch[2] = c.remove_letterbox;
    if (disc_changed) {
        // like swapping discs on the console: the game starts over with the other disc
        if (MessageBoxW(g_wnd, disc == 2 ? tr(L"Das Spiel wird jetzt mit Disc 2 neu gestartet.\nNicht gespeicherter Fortschritt geht verloren.")
                                         : tr(L"Das Spiel wird jetzt mit Disc 1 neu gestartet.\nNicht gespeicherter Fortschritt geht verloren."),
                        tr(L"Disc wechseln"), MB_OKCANCEL | MB_ICONINFORMATION) == IDOK)
            restart_game();
        c.disc = disc == 2 ? 1 : 2;  // cancelled: stay with the running disc
        save_config();
    } else if (restart) {
        MessageBoxW(g_wnd, tr(L"Renderer und interne Auflösung werden beim nächsten Start des Spiels übernommen."), tr(L"Einstellungen"),
                    MB_OK | MB_ICONINFORMATION);
    }
    DestroyWindow(g_wnd);
    if (fs_changed) set_fullscreen_public(c.fullscreen);
}

static HWND mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND h_ = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_wnd, (HMENU)(INT_PTR)id,
                              GetModuleHandleW(nullptr), nullptr);
    SendMessageW(h_, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h_;
}
static HWND combo(int x, int y, int w, int id, std::initializer_list<const wchar_t*> items, int sel) {
    HWND c = mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, x, y, w, 300, id);
    for (const wchar_t* it : items) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)it);
    SendMessageW(c, CB_SETCURSEL, sel, 0);
    return c;
}
static HWND check(int x, int y, int w, int id, const wchar_t* t, bool on) {
    HWND c = mk(WC_BUTTONW, t, BS_AUTOCHECKBOX | WS_TABSTOP, x, y, w, 22, id);
    SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    return c;
}
static void label(int x, int y, int w, const wchar_t* t) { mk(WC_STATICW, t, 0, x, y + 3, w, 20, -1); }

static void build_controls() {
    const Config& c = g_edit;
    mk(WC_BUTTONW, tr(L"Grafik"), BS_GROUPBOX, 12, 8, 330, 430, -1);
    label(24, 36, 120, L"Renderer:");
    g_renderer = combo(150, 34, 180, ID_RENDERER, {tr(L"GPU (hohe Auflösung)"), L"Software"}, c.renderer ? 0 : 1);
    label(24, 68, 120, tr(L"Interne Auflösung:"));
    int sc = c.internal_scale >= 8 ? 7 : c.internal_scale;
    g_scale = combo(150, 66, 180, ID_SCALE,
                    {tr(L"Automatisch"), L"1x  (512 x 448)", L"2x  (1024 x 896)", L"3x  (1536 x 1344)", L"4x  (2048 x 1792)",
                     L"5x  (2560 x 2240)", L"6x  (3072 x 2688)", L"8x  (4096 x 3584)"},
                    sc);
    label(24, 100, 120, tr(L"Seitenverhältnis:"));
    g_aspect = combo(150, 98, 180, ID_ASPECT, {L"4:3", L"16:9", tr(L"Fenster füllen"), L"16:10 (Steam Deck)"}, c.aspect);
    label(24, 132, 120, tr(L"Bildfilter:"));
    g_filter = combo(150, 130, 180, ID_FILTER, {tr(L"Pixel (scharf)"), tr(L"Bilinear (weich)"), tr(L"Scharf-bilinear")}, c.filter);
    g_ws = check(24, 166, 310, ID_WIDESCREEN, tr(L"Widescreen (Taste F6)"), c.widescreen_hack);
    g_hud = check(24, 192, 310, ID_HUD, tr(L"Menüs/Texte im 4:3-Format (Widescreen)"), c.hud_fix);
    g_lb = check(24, 218, 310, ID_LETTERBOX, tr(L"Kinobalken entfernen (experimentell)"), c.remove_letterbox);
    g_fs = check(24, 244, 310, ID_FULLSCREEN, tr(L"Vollbild (Taste F11 / Alt+Enter)"), c.fullscreen);
    g_vsync = check(24, 270, 310, ID_VSYNC, tr(L"VSync (kein Tearing)"), c.vsync);
    g_fps = check(24, 296, 310, ID_FPS, tr(L"Bildrate anzeigen (F7)"), c.show_fps);
    g_hdtex = check(24, 322, 310, ID_HDTEX, tr(L"HD-Texturen (Ordner \"textures\", Taste F8)"), c.hd_textures);
    g_fps60 = check(24, 348, 310, ID_FPS60, tr(L"60 fps (experimentell, Taste F3)"), c.fps60);
    label(24, 378, 90, tr(L"Nachschärfen:"));
    g_sharp = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 110, 376, 170, 28, ID_SHARP);
    SendMessageW(g_sharp, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g_sharp, TBM_SETPOS, TRUE, c.sharpen);
    g_sharplabel = mk(WC_STATICW, L"", 0, 286, 381, 50, 20, ID_SHARPLABEL);
    update_sharp_label();
    mk(WC_STATICW, tr(L"Renderer/Auflösung gelten ab nächstem Start."), 0, 24, 408, 310, 20, -1);

    mk(WC_BUTTONW, L"Audio / Disc", BS_GROUPBOX, 12, 444, 330, 216, -1);
    label(24, 626, 90, L"Disc:");
    g_disc = combo(110, 624, 220, ID_DISC, {L"Disc 1", tr(L"Disc 2 (Neustart)")}, c.disc == 2 ? 1 : 0);
    {
        static const wchar_t* NAMES[4] = {L"Musik:", L"Sprache:", L"Effekte:", L"Videos:"};
        const int vals[4] = {c.vol_music, c.vol_voice, c.vol_sfx, c.vol_movie};
        for (int i = 0; i < 4; i++) {
            int y = 502 + i * 30;
            label(24, y + 2, 90, tr(NAMES[i]));
            g_catvol[i] = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 110, y, 170, 28, ID_CATVOL0 + i);
            SendMessageW(g_catvol[i], TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(g_catvol[i], TBM_SETPOS, TRUE, vals[i]);
            g_catlabel[i] = mk(WC_STATICW, L"", 0, 286, y + 5, 50, 20, ID_CATLABEL0 + i);
            update_cat_label(i);
        }
    }
    label(24, 472, 90, tr(L"Gesamt:"));
    g_vol = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, 110, 470, 170, 28, ID_VOLUME);
    SendMessageW(g_vol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendMessageW(g_vol, TBM_SETPOS, TRUE, c.volume);
    g_vollabel = mk(WC_STATICW, L"", 0, 286, 475, 50, 20, ID_VOLLABEL);
    update_vol_label();

    mk(WC_BUTTONW, tr(L"Steuerung"), BS_GROUPBOX, 354, 8, 520, 440, -1);
    g_list = mk(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, 366, 32, 496, 330, ID_LIST,
                WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (LPWSTR)tr(L"PS2-Taste");
    col.cx = S(190);
    ListView_InsertColumn(g_list, 0, &col);
    col.pszText = (LPWSTR)tr(L"Tastatur");
    col.cx = S(130);
    ListView_InsertColumn(g_list, 1, &col);
    col.pszText = (LPWSTR)L"Controller";
    col.cx = S(150);
    ListView_InsertColumn(g_list, 2, &col);
    fill_list();
    mk(WC_BUTTONW, tr(L"Tastaturtaste ändern"), BS_PUSHBUTTON | WS_TABSTOP, 366, 370, 160, 28, ID_SETKEY);
    mk(WC_BUTTONW, tr(L"Controllertaste ändern"), BS_PUSHBUTTON | WS_TABSTOP, 532, 370, 170, 28, ID_SETPAD);
    mk(WC_BUTTONW, tr(L"Standardbelegung"), BS_PUSHBUTTON | WS_TABSTOP, 708, 370, 154, 28, ID_DEFAULTS);
    g_status = mk(WC_STATICW, tr(L"Zeile auswählen (Doppelklick = Tastaturtaste ändern)."), 0, 366, 410, 496, 34, ID_STATUS);

    mk(WC_BUTTONW, tr(L"Speichern"), BS_DEFPUSHBUTTON | WS_TABSTOP, 642, 508, 112, 30, ID_SAVE);
    mk(WC_BUTTONW, tr(L"Abbrechen"), BS_PUSHBUTTON | WS_TABSTOP, 762, 508, 112, 30, ID_CANCEL);
}

void default_input(Config& c);

static LRESULT CALLBACK settings_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SETKEY: start_capture(1); return 0;
        case ID_SETPAD: start_capture(2); return 0;
        case ID_DEFAULTS:
            default_input(g_edit);
            fill_list();
            set_status(tr(L"Standardbelegung wiederhergestellt (wird mit \"Speichern\" übernommen)."));
            return 0;
        case ID_SAVE: apply_and_close(); return 0;
        case ID_CANCEL: DestroyWindow(h); return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR* n = (NMHDR*)lp;
        if (n->idFrom == ID_LIST && n->code == NM_DBLCLK) start_capture(1);
        break;
    }
    case WM_HSCROLL:
        if ((HWND)lp == g_vol) update_vol_label();
        if ((HWND)lp == g_sharp) update_sharp_label();
        for (int i = 0; i < 4; i++)
            if ((HWND)lp == g_catvol[i]) update_cat_label(i);
        break;
    case WM_TIMER:
        poll_capture();
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        KillTimer(h, ID_TIMER);
        g_wnd = nullptr;
        g_capture = 0;
        if (g_parent) {
            EnableWindow(g_parent, TRUE);
            SetForegroundWindow(g_parent);
        }
        kernel::g_pause_req = false;
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

bool settings_open() { return g_wnd != nullptr; }

// keyboard navigation between the controls (not while a key is being captured)
bool settings_dialog_message(MSG* m) { return g_wnd && !g_capture && IsDialogMessageW(g_wnd, m); }

void open_settings(HWND parent) {
    if (g_wnd) { SetForegroundWindow(g_wnd); return; }
    static bool reg = false;
    if (!reg) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = settings_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        wc.lpszClassName = L"Grandia3Settings";
        RegisterClassExW(&wc);
        reg = true;
    }
    g_parent = parent;
    g_edit = config();
    g_dpi = (int)GetDpiForWindow(parent);
    if (g_dpi < 96) g_dpi = 96;
    if (g_font) DeleteObject(g_font);
    g_font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    RECT r{0, 0, S(886), S(672)};  // the audio/disc group ends at 660
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, (UINT)g_dpi);
    RECT pr;
    GetWindowRect(parent, &pr);
    int w = r.right - r.left, hh = r.bottom - r.top;
    int x = pr.left + ((pr.right - pr.left) - w) / 2, y = pr.top + ((pr.bottom - pr.top) - hh) / 2;
    g_wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"Grandia3Settings", tr(L"Grandia III  -  Einstellungen"), style, x, y, w, hh, parent,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_wnd) return;
    build_controls();
    kernel::g_pause_req = true;  // the game waits while the settings are open
    set_vibration(0, 0);
    EnableWindow(parent, FALSE);
    ShowWindow(g_wnd, SW_SHOW);
    SetTimer(g_wnd, ID_TIMER, 30, nullptr);
}

}  // namespace platform
