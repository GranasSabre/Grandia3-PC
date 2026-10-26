// Translations: the field script's message opcode calls 0x001DA6A0(params, text) to open a message
// window. The hook looks the original text up (FNV-1a hash of its bytes up to the 0x0D terminator)
// in lang/<language>.tsv and hands the window a replacement placed in unused low EE memory.
// German letters: seven unused kana glyphs of the font are replaced in EE RAM by ä ö ü Ä Ö Ü ß
// (made from the game's own a o u A O U glyphs plus dots) with matching advance widths.
//
// Script text encoding: byte = font code + 0x20; ASCII letters keep their codes, space = 0xA0,
// line break 0x08, end 0x0D (0xFF follows). Font: width table (s8 offset, u8 advance) per glyph,
// then 16x16 4 bpp glyphs; glyph = code - 0x20.
#include "../common.h"
#include <windows.h>
#include "../memory.h"
#include "../platform/platform.h"
#include "ee_recomp.h"
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void ee_001DA6A0(EEContext* c);
namespace systext { void tick(); }
void watch_range_k(u32 lo, u32 hi);  // kernel.cpp (analysis)

namespace texthook {

static std::unordered_map<u64, std::vector<u8>> g_tr;
static bool g_loaded = false;
static bool g_log_texts = false;
bool g_watch_msg = false;          // --watch-msg (analysis): log the code reading each message   // --text-log: every message seen goes to lang/seen.tsv
static FILE* g_seen = nullptr;

static const u32 BUF_BASE = 0x00040000, BUF_SLOT = 0x1000, BUF_SLOTS = 16;  // unused low EE RAM
static u32 g_slot = 0;

// Extra letters: glyph slots (unused kana) and the letters they are made from. German 0x98..0x9E
// (dots as before), French 0x9F..0xB0 (accent templates, see fr_glyphs.py for a preview).
// accent: 'd' dots, 'a' acute, 'g' grave, 'c' circumflex, 'k' cedilla, 0 = ß
struct Extra { const char* utf8; u8 glyph; char base; bool upper; char accent; bool german; };
static const Extra EXTRA[] = {
    {"\xC3\xA4", 0x98, 'a', false, 'd', true}, {"\xC3\xB6", 0x99, 'o', false, 'd', true}, {"\xC3\xBC", 0x9A, 'u', false, 'd', true},
    {"\xC3\x84", 0x9B, 'A', true, 'd', true},  {"\xC3\x96", 0x9C, 'O', true, 'd', true},  {"\xC3\x9C", 0x9D, 'U', true, 'd', true},
    {"\xC3\x9F", 0x9E, 0, false, 0, true},
    {"\xC3\xA0", 0x9F, 'a', false, 'g', false}, {"\xC3\xA2", 0xA0, 'a', false, 'c', false}, {"\xC3\xA7", 0xA1, 'c', false, 'k', false},
    {"\xC3\xA9", 0xA2, 'e', false, 'a', false}, {"\xC3\xA8", 0xA3, 'e', false, 'g', false}, {"\xC3\xAA", 0xA4, 'e', false, 'c', false},
    {"\xC3\xAB", 0xA5, 'e', false, 'd', false}, {"\xC3\xAE", 0xA6, 'i', false, 'c', false}, {"\xC3\xAF", 0xA7, 'i', false, 'd', false},
    {"\xC3\xB4", 0xA8, 'o', false, 'c', false}, {"\xC3\xB9", 0xA9, 'u', false, 'g', false}, {"\xC3\xBB", 0xAA, 'u', false, 'c', false},
    {"\xC3\x80", 0xAB, 'A', true, 'g', false},  {"\xC3\x87", 0xAC, 'C', true, 'k', false},  {"\xC3\x89", 0xAD, 'E', true, 'a', false},
    {"\xC3\x88", 0xAE, 'E', true, 'g', false},  {"\xC3\x8A", 0xAF, 'E', true, 'c', false},  {"\xC3\x94", 0xB0, 'O', true, 'c', false},
};

// French accent templates: columns cx-2..cx+3 (cx = left of the two centre columns), '#' = ink
static const char* accent_tpl(char accent, int kind, int row) {
    static const char* LOW[4][3] = {{"...##.", "..##..", ".##..."}, {".##...", "..##..", "...##."},
                                    {"..##..", ".####.", "##..##"}, {"##..##", "##..##", "......"}};
    static const char* CAP[4][2] = {{"...##.", ".##..."}, {".##...", "...##."}, {"..##..", "##..##"}, {"##..##", "......"}};
    static const char* NARROW[2][2] = {{".#.", "#.#"}, {"#.#", "..."}};  // over i: cx-1..cx+1
    int a = accent == 'a' ? 0 : accent == 'g' ? 1 : accent == 'c' ? 2 : 3;
    if (kind == 0) return row < 3 ? LOW[a][row] : nullptr;
    if (kind == 1) return row < 2 ? CAP[a][row] : nullptr;
    return row < 2 ? NARROW[a == 2 ? 0 : 1][row] : nullptr;
}

// argument bytes of a control code inside message text (0E speaker/face, 13 FF pause, 12 style)
static inline size_t ctrl_args(u8 b) { return b == 0x0E ? 3 : b == 0x13 ? 2 : b == 0x12 ? 1 : 0; }

u64 fnv(const u8* p, size_t n) {
    u64 h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// UTF-8 translation text -> script bytes
static std::vector<u8> encode(const std::string& s) {
    std::vector<u8> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '\\' && i + 1 < s.size() && s[i + 1] == 'n') { out.push_back(0x08); i += 2; continue; }
        if (ch == '{') {  // control sequence kept from the original: {hex bytes}
            size_t e = s.find('}', i);
            if (e != std::string::npos) {
                for (size_t k = i + 1; k + 1 < e; k += 2) out.push_back((u8)strtoul(s.substr(k, 2).c_str(), nullptr, 16));
                i = e + 1;
                continue;
            }
        }
        if (ch == ' ') { out.push_back(0xA0); i++; continue; }
        if (ch < 0x80) { out.push_back((u8)(ch + 0x20)); i++; continue; }
        bool done = false;
        for (const Extra& e : EXTRA) {
            size_t n = strlen(e.utf8);
            if (s.compare(i, n, e.utf8) == 0) { out.push_back((u8)(e.glyph + 0x40)); i += n; done = true; break; }
        }
        if (!done) {
            // typographic quotes/dashes -> ASCII
            if (s.compare(i, 3, "\xE2\x80\x9E") == 0 || s.compare(i, 3, "\xE2\x80\x9C") == 0 || s.compare(i, 3, "\xE2\x80\x9D") == 0) { out.push_back('"' + 0x20); i += 3; }
            else if (s.compare(i, 3, "\xE2\x80\x93") == 0 || s.compare(i, 3, "\xE2\x80\x94") == 0) { out.push_back('-' + 0x20); i += 3; }
            else if (s.compare(i, 3, "\xE2\x80\xA6") == 0) { for (int k = 0; k < 3; k++) out.push_back('.' + 0x20); i += 3; }
            // French: guillemets -> ", oe ligature spelled out, narrow/no-break spaces -> space
            else if (s.compare(i, 2, "\xC2\xAB") == 0 || s.compare(i, 2, "\xC2\xBB") == 0) { out.push_back('"' + 0x20); i += 2; }
            else if (s.compare(i, 2, "\xC5\x93") == 0) { out.push_back('o' + 0x20); out.push_back('e' + 0x20); i += 2; }
            else if (s.compare(i, 2, "\xC5\x92") == 0) { out.push_back('O' + 0x20); out.push_back('e' + 0x20); i += 2; }
            else if (s.compare(i, 2, "\xC2\xA0") == 0) { out.push_back(0xA0); i += 2; }
            else if (s.compare(i, 3, "\xE2\x80\xAF") == 0) { out.push_back(0xA0); i += 3; }
            else { out.push_back('?' + 0x20); i++; while (i < s.size() && ((unsigned char)s[i] & 0xC0) == 0x80) i++; }
        }
    }
    return out;
}

static void load() {
    g_loaded = true;
    // the game's own text is English: other languages come from lang\<code>.tsv
    std::string lang = platform::config().language;
    std::string path = exe_dir() + "\\lang\\" + lang + ".tsv";
    FILE* f = lang == "en" ? nullptr : fopen(path.c_str(), "rb");
    if (f) {
        char line[4096];
        int n = 0;
        while (fgets(line, sizeof line, f)) {
            if (line[0] == '#' || strlen(line) < 18 || line[16] != '\t') continue;
            u64 key = strtoull(std::string(line, 16).c_str(), nullptr, 16);
            std::string text(line + 17);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            if (text.empty()) continue;
            g_tr[key] = encode(text);
            n++;
        }
        fclose(f);
        log_write(LOG_MAIN, "translation %s: %d texts", lang.c_str(), n);
    }
    if (g_log_texts) g_seen = fopen((exe_dir() + "\\lang\\seen.tsv").c_str(), "ab");
}

// ---------------------------------------------------------------- font
static u32 g_font_tbl = 0;   // EE address of the width table (0: not found yet)

static inline u8* glyph_ptr(u32 tbl, int g) { return mem::ee_ptr(tbl + 0x380 + (u32)g * 128); }
static inline u8* width_ptr(u32 tbl, int g) { return mem::ee_ptr(tbl + (u32)g * 2); }

static void put(u8* gl, int x, int y, int v) {
    if (x < 0 || x > 15 || y < 0 || y > 15) return;
    u8& b = gl[y * 8 + x / 2];
    if (x & 1) b = (u8)((b & 0x0F) | (v << 4));
    else b = (u8)((b & 0xF0) | v);
}
static int get(const u8* gl, int x, int y) { u8 b = gl[y * 8 + x / 2]; return (x & 1) ? b >> 4 : b & 15; }

// ß, 16x16 hex rows (drawn to match the font's stroke)
static const char* SZ[16] = {
    "0000000000000000", "0000000000000000", "0000000000000000", "00000059a9500000", "000006ffffe40000",
    "00000ed1015f9000", "00001f6000af8000", "00001f5007fb1000", "00001f504fc10000", "00001f5007fb3000",
    "00001f50002df100", "00001f50000af500", "00001f54007ff200", "00001f5effffa000", "0000085669830000",
    "0000000000000000"};

static void make_glyphs(u32 tbl) {
    for (const Extra& e : EXTRA) {
        u8* dst = glyph_ptr(tbl, e.glyph);
        if (!dst) return;
        u8 tmp[128];
        if (e.base && !e.german) {
            int bg = e.base - 0x20;
            memcpy(tmp, glyph_ptr(tbl, bg), 128);
            u8 wd[2];
            memcpy(wd, width_ptr(tbl, bg), 2);
            int top = 16, x0 = 16, x1 = -1;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++)
                    if (get(tmp, x, y) > 4) { top = std::min(top, y); x0 = std::min(x0, x); x1 = std::max(x1, x); }
            int cx = (x0 + x1) / 2, kind = 0, left = cx - 2, row0 = top - 4;
            if (e.base == 'i') {  // no dot, two pixels wider for the accent
                memset(tmp, 0, 5 * 8);
                top = 5;
                wd[0] = (u8)(wd[0] + 1);
                wd[1] = (u8)(wd[1] + 2);
                kind = 2, left = cx - 1, row0 = top - 3;
            } else if (e.accent == 'k') {
                left = cx - 2, row0 = 14;
            } else if (e.upper) {
                // squeeze the capital: drop the two rows most like the row above them (not adjacent)
                int drop[2] = {-1, -1};
                for (int n = 0; n < 2; n++) {
                    int best = -1, bd = 1 << 30;
                    for (int y = top + 1; y < 15; y++) {
                        if (y == drop[0] || y == drop[0] - 1 || y == drop[0] + 1) continue;
                        int d = 0;
                        for (int x = 0; x < 16; x++) d += abs(get(tmp, x, y) - get(tmp, x, y - 1));
                        int score = d * 16 + abs(y - 8);
                        if (score < bd) bd = score, best = y;
                    }
                    drop[n] = best;
                }
                u8 sq[128];
                memset(sq, 0, 128);
                int out = 15;
                for (int y = 15; y >= 0 && out >= 2; y--) {
                    if (y == drop[0] || y == drop[1]) continue;
                    memcpy(sq + out * 8, tmp + y * 8, 8);
                    out--;
                }
                memcpy(tmp, sq, 128);
                kind = 1, row0 = 0;
            }
            for (int r = 0; r < 3; r++) {
                const char* line = e.accent == 'k' ? (r < 2 ? (r ? ".##..." : "..##..") : nullptr) : accent_tpl(e.accent, kind, r);
                if (!line) break;
                for (int c = 0; line[c]; c++)
                    if (line[c] == '#') put(tmp, left + c, row0 + r, 15);
            }
            memcpy(dst, tmp, 128);
            memcpy(width_ptr(tbl, e.glyph), wd, 2);
        } else if (e.base) {
            int bg = e.base - 0x20;
            memcpy(tmp, glyph_ptr(tbl, bg), 128);
            // ink extent of the base letter
            int top = 16, x0 = 16, x1 = -1;
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++)
                    if (get(tmp, x, y) > 4) { top = std::min(top, y); x0 = std::min(x0, x); x1 = std::max(x1, x); }
            int cx = (x0 + x1) / 2, dx = std::max(2, (x1 - x0 + 1) / 3);
            int dy = e.upper ? std::max(0, top - 2) : top - 3;
            int h = e.upper ? 1 : 2;
            for (int k = -1; k <= 1; k += 2) {
                int px = cx + k * dx - (k < 0 ? 1 : 0);
                for (int yy = 0; yy < h; yy++)
                    for (int xx = 0; xx < 2; xx++) put(tmp, px + xx, dy + yy, 15);
            }
            memcpy(dst, tmp, 128);
            memcpy(width_ptr(tbl, e.glyph), width_ptr(tbl, bg), 2);
        } else {
            memset(tmp, 0, 128);
            for (int y = 0; y < 16; y++)
                for (int x = 0; x < 16; x++) {
                    char c = SZ[y][x];
                    put(tmp, x, y, c <= '9' ? c - '0' : c - 'a' + 10);
                }
            memcpy(dst, tmp, 128);
            memcpy(width_ptr(tbl, e.glyph), width_ptr(tbl, 'B' - 0x20), 2);
        }
    }
}

// called every few vertical blanks: find the font in EE RAM (after it was loaded) and keep it patched
void font_tick() {
    if (!g_loaded) load();
    systext::tick();
    if (g_tr.empty()) return;
    static const u8 sig[16] = {0xfb, 0x05, 0xfa, 0x03, 0xfe, 0x04, 0xfd, 0x0a, 0xfd, 0x0a, 0xfe, 0x0c, 0xfe, 0x0c, 0xfe, 0x02};
    if (g_font_tbl) {
        u8* t = mem::ee_ptr(g_font_tbl);
        if (t && memcmp(t, sig, 16) == 0) {
            // still there: re-patch if the game reloaded it
            if (width_ptr(g_font_tbl, 0x9E)[1] != width_ptr(g_font_tbl, 'B' - 0x20)[1]) make_glyphs(g_font_tbl);
            return;
        }
        g_font_tbl = 0;
    }
    static int skip = 0;
    if (skip-- > 0) return;
    skip = 30;
    for (u32 a = 0x100000; a + 16 < 32u * 1024 * 1024; a += 4) {
        const u8* p = mem::ram + a;
        if (p[0] == 0xfb && memcmp(p, sig, 16) == 0) {
            g_font_tbl = a;
            make_glyphs(a);
            log_write(LOG_MAIN, "translation: font width table at %08x, German letters added", a);
            return;
        }
    }
}

}  // namespace texthook

using namespace texthook;

extern "C" void hook_msg_open(EEContext* c) {
    if (!g_loaded) load();
    u32 p = c->r[5].uw[0];
    const u8* t = mem::ee_ptr(p);
    // a choice (script command 9 before the window parameters) needs the player: the skip stops
    if (t && p >= 10 && platform::skip_active() && *mem::ee_ptr(p - 10) == 0x09) platform::skip_stop("choice");
    if (g_log_texts && t && p >= 12) {
        const u8* b = mem::ee_ptr(p - 12);
        log_write(LOG_MAIN, "msg_open %08x ra=%08x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x | %02x %02x %02x %02x", p, c->r[31].uw[0],
                  b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], t[0], t[1], t[2], t[3]);
        void* fr[16];
        USHORT k = RtlCaptureStackBackTrace(0, 16, fr, nullptr);
        u64 base = (u64)GetModuleHandleW(nullptr);
        std::string st;
        for (USHORT j = 0; j < k; j++) { char x[24]; snprintf(x, sizeof x, " %llx", (unsigned long long)((u64)fr[j] - base)); st += x; }
        log_write(LOG_MAIN, "msg_open stack:%s", st.c_str());
    }
    if (t && (!g_tr.empty() || g_seen)) {
        // end of the message: 0x0D, control arguments skipped (a speaker id can be 0x0D)
        size_t n = 0;
        while (n < 2048 && t[n] != 0x0D) n += 1 + ctrl_args(t[n]);
        n = std::min<size_t>(n, 2048);
        // speaker prefix 0E 00 <id u16> stays; the key covers the visible text
        size_t pre = (n >= 4 && t[0] == 0x0E) ? 4 : 0;
        u64 key = fnv(t + pre, n - pre);
        if (g_seen) {
            std::string en;
            for (size_t i = pre; i < n; i++) {
                u8 b = t[i];
                if (b == 8) en += "\\n";
                else if (b == 0xA0) en += ' ';
                else if (b < 0x40 || (i + 1 < n && t[i + 1] >= 0xF0)) {  // control sequence / symbol as {hex}
                    size_t k = b < 0x20 ? 1 + ctrl_args(b) : 2;
                    if (b >= 0x20 && !(i + 1 < n && t[i + 1] >= 0xF0)) k = 1;
                    char hx[4];
                    en += '{';
                    for (size_t j = 0; j < k && i + j < n; j++) { snprintf(hx, sizeof hx, "%02x", t[i + j]); en += hx; }
                    en += '}';
                    i += k - 1;
                } else en += (char)(b - 0x20);
            }
            fprintf(g_seen, "%016llx\t%s\n", (unsigned long long)key, en.c_str());
            fflush(g_seen);
        }
        auto it = g_tr.find(key);
        if (it != g_tr.end()) {
            u32 dst = BUF_BASE + (g_slot++ % BUF_SLOTS) * BUF_SLOT;
            u8* d = mem::ee_ptr(dst);
            memcpy(d, t, pre);
            size_t m = std::min<size_t>(it->second.size(), BUF_SLOT - 2 - pre);
            memcpy(d + pre, it->second.data(), m);
            d[pre + m] = 0x0D;
            d[pre + m + 1] = 0xFF;
            c->r[5].ud[0] = dst;
            if (g_watch_msg) ::watch_range_k(dst, dst + (u32)it->second.size());
            // window size like the game's converter: widest line of (advance + 1) per character, 20 px per line
            u8* pa = mem::ee_ptr(c->r[4].uw[0]);
            u16 ow = (u16)(pa[6] | pa[7] << 8), oh = (u16)(pa[8] | pa[9] << 8);
            if (g_font_tbl && pa && ow > 0 && ow < 1000 && oh > 0 && oh < 1000) {
                // widest line and most lines over all pages (07 = next page); control sequences skipped
                int w = 0, lw = 0, lines = 1, maxlines = 1;
                const std::vector<u8>& tx = it->second;
                for (size_t i = 0; i < m; i++) {
                    u8 b = tx[i];
                    if (b == 0x08) { w = std::max(w, lw); lw = 0; lines++; maxlines = std::max(maxlines, lines); continue; }
                    if (b == 0x07) { w = std::max(w, lw); lw = 0; lines = 1; continue; }
                    if (b < 0x20) { i += ctrl_args(b); continue; }
                    if (i + 1 < m && tx[i + 1] >= 0xF0) { lw += 13; i++; continue; }  // button symbol
                    if (b < 0x40) continue;
                    int g = b - 0x40;
                    lw += width_ptr(g_font_tbl, g)[1] + 1;
                }
                w = std::max(w, lw);
                lines = maxlines;
                pa[6] = (u8)w;
                pa[7] = (u8)(w >> 8);
                int h = lines * 20;
                pa[8] = (u8)h;
                pa[9] = (u8)(h >> 8);
                // the same values in the loaded script (x y w h style precede the text)
                u8* sc = mem::ee_ptr(p - 9);
                if (sc && (sc[4] | sc[5] << 8) == ow && (sc[6] | sc[7] << 8) == oh) {
                    sc[4] = (u8)w; sc[5] = (u8)(w >> 8);
                    sc[6] = (u8)h; sc[7] = (u8)(h >> 8);
                }
                static int logged = 0;
                if (logged++ < 20)
                    log_write(LOG_MAIN, "translation: window %ux%u -> %dx%d (params %02x%02x %02x%02x %02x%02x %02x%02x %02x%02x %02x)", ow, oh, w, h,
                              pa[0], pa[1], pa[2], pa[3], pa[4], pa[5], pa[6], pa[7], pa[8], pa[9], pa[10]);
            }
        }
    }
    ee_001DA6A0(c);
}

void texthook_set_log(bool on) { texthook::g_log_texts = on; }
