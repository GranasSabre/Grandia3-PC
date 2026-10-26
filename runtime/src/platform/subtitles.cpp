// Subtitles for voiced cutscenes (see subtitles.h).
#include "subtitles.h"
#include "platform.h"
#include "../iop/cdvd.h"
#include "../memory.h"
#include "../spu2/spu2.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

namespace subs {
namespace {

constexpr u32 HEADER = 0x800;      // track header (channels, rate, interleave, sizes)
constexpr u32 CHUNK = 0x800;       // stereo interleave: left chunk, right chunk, ...
constexpr u32 BLOCKS = 2 * 1024 * 1024 / 16;

struct Entry { u32 sector; u16 tag; };
struct Region { u32 iop, bytes; u16 tag; u32 entry_off; };   // IOP buffer filled from a track
struct Block { u16 tag; u32 off; };                          // 16-byte ADPCM block in sound RAM
struct Line { float start, end; std::wstring text; };

u32 g_stz_lsn = 0, g_stz_sectors = 0;
std::vector<Entry> g_entries;             // sorted by sector
std::map<u16, u32> g_rate;                // sample rate per track (from its header)
std::map<u16, std::vector<Line>> g_lines;
std::mutex g_mu;
Region g_regions[16];
int g_next_region = 0;
Block g_blocks[BLOCKS];
// diagnostics: disc origin of every sound RAM block (disc byte / 16 + 1, 0 = not from the disc)
struct SrcRegion { u32 iop, bytes, lsn; };
SrcRegion g_src_regions[32];
int g_next_src = 0;
u32 g_src[BLOCKS];

// movies MOVIE\GRMnn.MOV (key 0x1000 + nn): their audio reaches the same stream ring through an IOP
// buffer the disc reads do not fill directly, so the blocks sent after the movie's header read are
// counted instead (the file holds left and right chunks of 0x800 in turn, like a track)
struct Movie { u32 lsn, sectors; u16 key; };
std::vector<Movie> g_movies;
bool g_movie_active = false;
u16 g_movie_key = 0;
u32 g_movie_off = 0;

// current line, for pixels() and the log
u16 g_cur_tag = 0;
const Line* g_cur_line = nullptr;

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

void load_lines(const std::string& lang) {
    FILE* f = fopen((exe_dir() + "\\lang\\sub_" + lang + ".tsv").c_str(), "rb");
    if (!f && lang != "en") f = fopen((exe_dir() + "\\lang\\sub_en.tsv").c_str(), "rb");
    if (!f) return;
    char buf[4096];
    int n = 0;
    while (fgets(buf, sizeof buf, f)) {
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        size_t a = s.find('\t'), b = a == std::string::npos ? a : s.find('\t', a + 1),
               c = b == std::string::npos ? b : s.find('\t', b + 1);
        if (c == std::string::npos) continue;
        Line l{(float)atof(s.substr(a + 1, b - a - 1).c_str()), (float)atof(s.substr(b + 1, c - b - 1).c_str()), widen(s.substr(c + 1))};
        for (size_t k; (k = l.text.find(L"\\n")) != std::wstring::npos;) l.text.replace(k, 2, L"\n");
        g_lines[(u16)strtoul(s.substr(0, a).c_str(), nullptr, 16)].push_back(l);
        n++;
    }
    fclose(f);
    for (auto& [tag, v] : g_lines) std::sort(v.begin(), v.end(), [](const Line& x, const Line& y) { return x.start < y.start; });
    log_write(LOG_MAIN, "subtitles: %d lines for %zu cutscene tracks", n, g_lines.size());
}

// time within the track at the read position of a playing voice, or -1
float voice_time(int core, int voice, u16& tag) {
    if (!spu2::voice_playing(core, voice)) return -1;
    const Block& b = g_blocks[(spu2::voice_nax(core, voice) * 2 / 16) % BLOCKS];
    if (!b.tag) return -1;
    u32 chunk = b.off / CHUNK;
    if (chunk & 1) return -1;   // right channel: the left one gives the same time
    auto r = g_rate.find(b.tag);
    double samples = ((double)(chunk >> 1) * CHUNK + b.off % CHUNK) / 16 * 28;
    tag = b.tag;
    return (float)(samples / (r != g_rate.end() && r->second ? r->second : 24000));
}

}  // namespace

void init() {
    u32 size = 0, idx_lsn = 0, idx_size = 0;
    if (!cdvd::find_file("\\MUSIC\\GR3_STR.STZ;1", &g_stz_lsn, &size) || !cdvd::find_file("\\MUSIC\\GR3_STR.IDX;1", &idx_lsn, &idx_size)) {
        log_write(LOG_MAIN, "subtitles: no GR3_STR.STZ on this disc");
        return;
    }
    g_stz_sectors = size / 2048;
    std::vector<u32> idx((idx_size + 2047) / 2048 * 512);
    cdvd::read_sectors(idx_lsn, (u32)idx.size() / 512, idx.data());
    for (size_t i = 0; i < idx_size / 4; i++) {
        u32 tag = idx[i] >> 20, sec = idx[i] & 0xFFFFF;
        if (tag != 0xFFF && (sec || i == 0)) g_entries.push_back({sec, (u16)tag});
    }
    std::sort(g_entries.begin(), g_entries.end(), [](const Entry& a, const Entry& b) { return a.sector < b.sector; });
    for (int n = 1; n < 100; n++) {
        char name[32];
        snprintf(name, sizeof name, "\\MOVIE\\GRM%02d.MOV;1", n);
        u32 l = 0, s = 0;
        if (cdvd::find_file(name, &l, &s)) g_movies.push_back({l, s / 2048, (u16)(0x1000 + n)});
    }
    log_write(LOG_MAIN, "subtitles: %zu voice archive entries, %zu movies", g_entries.size(), g_movies.size());
    load_lines(platform::config().language);
}

void on_cd_read(u32 lsn, u32 count, u32 iop_addr) {
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_src_regions[g_next_src] = {iop_addr, count * 2048, lsn};
        g_next_src = (g_next_src + 1) % 32;
        for (const Movie& m : g_movies)
            if (lsn == m.lsn) {   // the player starts with the header sector
                g_movie_active = true;
                g_movie_key = m.key;
                g_movie_off = 0;
                g_rate[m.key] = *(const u32*)(mem::iop_ptr(iop_addr) + 4);
                for (Region& r : g_regions) r.bytes = 0;   // the movie reuses the track buffers
                log_write(LOG_MAIN, "subtitles: movie %02d starts", m.key - 0x1000);
            }
    }
    if (g_entries.empty() || lsn < g_stz_lsn || lsn >= g_stz_lsn + g_stz_sectors) return;
    u32 off = lsn - g_stz_lsn;
    auto it = std::upper_bound(g_entries.begin(), g_entries.end(), off, [](u32 o, const Entry& e) { return o < e.sector; });
    if (it == g_entries.begin()) return;
    --it;
    std::lock_guard<std::mutex> lk(g_mu);
    g_movie_active = false;
    u32 entry_off = (off - it->sector) * 2048;
    if (entry_off == 0) g_rate[it->tag] = *(const u32*)(mem::iop_ptr(iop_addr) + 4);
    g_regions[g_next_region] = {iop_addr, count * 2048, it->tag, entry_off};
    g_next_region = (g_next_region + 1) % 16;
}

void on_voice_trans(u32 iop_addr, u32 spu_addr, u32 bytes) {
    std::lock_guard<std::mutex> lk(g_mu);
    // the IOP buffer is a ring refilled by later reads: the newest read covering the source counts
    const Region* src = nullptr;
    for (int k = 1; k <= 16 && !src; k++) {
        const Region& r = g_regions[(g_next_region - k + 16) % 16];
        if (r.bytes && iop_addr - r.iop < r.bytes) src = &r;
    }
    const SrcRegion* org = nullptr;
    for (int k = 1; k <= 32 && !org; k++) {
        const SrcRegion& r = g_src_regions[(g_next_src - k + 32) % 32];
        if (r.bytes && iop_addr - r.iop < r.bytes) org = &r;
    }
    for (u32 i = 0; i < bytes; i += 16) {
        g_src[((spu_addr + i) / 16) % BLOCKS] = org ? (u32)(((u64)org->lsn * 2048 + (iop_addr - org->iop) + i) / 16 + 1) : 0;
        Block& b = g_blocks[((spu_addr + i) / 16) % BLOCKS];
        u32 eo = src ? src->entry_off + (iop_addr - src->iop) + i : 0;
        if (src && eo >= HEADER) b = {src->tag, eo - HEADER};
        else if (!src && g_movie_active) b = {g_movie_key, g_movie_off + i};
        else b = {0, 0};
    }
    // a chunk that crosses the end of the ring arrives as two transfers, so count every byte
    if (!src && g_movie_active) g_movie_off += bytes;
}

void on_key_on(int core, int voice, u32 spu_addr) {
    u32 s = g_src[(spu_addr / 16) % BLOCKS];
    if (!s) { log_write(LOG_SPU, "voice source: c%d v%02d %06x <- not from the disc", core, voice, spu_addr); return; }
    u64 disc = (u64)(s - 1) * 16;
    u32 flsn = 0;
    std::string name = cdvd::file_at((u32)(disc / 2048), &flsn);
    log_write(LOG_SPU, "voice source: c%d v%02d %06x <- %s +0x%llx", core, voice, spu_addr, name.c_str(),
              (unsigned long long)(disc - (u64)flsn * 2048));
}

bool pixels(int pic_w, int pic_h, std::vector<u32>& out, int& w, int& h, u64& version) {
    static std::vector<u32> px;
    static int bw = 0, bh = 0, for_w = 0, for_h = 0;
    static const Line* drawn = nullptr;
    static u64 serial = 0;
    if (g_lines.empty()) return false;
    // the voice that streams a track with subtitles decides
    const Line* line = nullptr;
    u16 tag = 0;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        static int diag = 0;
        if (g_log_enabled[LOG_SPU] && ++diag % 120 == 0)   // --log=spu: what the playing voices read
            for (int c = 0; c < 2; c++)
                for (int v = 0; v < 24; v++)
                    if (spu2::voice_playing(c, v)) {
                        u32 a = spu2::voice_nax(c, v) * 2;
                        const Block& b = g_blocks[(a / 16) % BLOCKS];
                        log_write(LOG_SPU, "subtitles diag: c%d v%02d nax %06x tag %03x off %06x", c, v, a, b.tag, b.off);
                    }
        for (int c = 0; c < 2 && !line; c++)
            for (int v = 0; v < 24 && !line; v++) {
                u16 t = 0;
                float s = voice_time(c, v, t);
                if (s < 0) continue;
                auto it = g_lines.find(t);
                if (it == g_lines.end()) continue;
                tag = t;
                for (const Line& l : it->second)
                    if (s >= l.start && s < l.end) { line = &l; break; }
                if (!line) tag = t;
            }
    }
    if (line != g_cur_line || tag != g_cur_tag) {
        if (tag != g_cur_tag) log_write(LOG_MAIN, "subtitles: track %03x", tag);
        if (line) {
            std::string u(WideCharToMultiByte(CP_UTF8, 0, line->text.c_str(), -1, nullptr, 0, nullptr, nullptr), '\0');
            WideCharToMultiByte(CP_UTF8, 0, line->text.c_str(), -1, u.data(), (int)u.size(), nullptr, nullptr);
            log_write(LOG_MAIN, "subtitles: %03x %.2f-%.2f %s", tag, line->start, line->end, u.c_str());
        }
        g_cur_line = line;
        g_cur_tag = tag;
    }
    if (!line) return false;
    if (line != drawn || pic_w != for_w || pic_h != for_h) {
        // white text with a dark outline, wrapped to 80% of the picture width
        int fs = std::max(14, pic_h * 42 / 1000), pad = std::max(2, fs / 9);
        HDC dc = CreateCompatibleDC(nullptr);
        HFONT f = CreateFontW(-fs, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HGDIOBJ oldf = SelectObject(dc, f);
        RECT r{0, 0, pic_w * 8 / 10, 0};
        DrawTextW(dc, line->text.c_str(), -1, &r, DT_CENTER | DT_WORDBREAK | DT_CALCRECT | DT_NOPREFIX);
        int W = r.right + 2 * pad + 2, H = r.bottom + 2 * pad + 2;
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ oldbm = SelectObject(dc, bm);
        memset(bits, 0, (size_t)W * H * 4);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        RECT tr{pad + 1, pad + 1, pad + 1 + r.right, pad + 1 + r.bottom};
        DrawTextW(dc, line->text.c_str(), -1, &tr, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
        GdiFlush();
        // text coverage, and the outline as its dilation (separable square max filter)
        const u32* src = (const u32*)bits;
        std::vector<u8> t((size_t)W * H), tmp((size_t)W * H), o((size_t)W * H);
        for (size_t k = 0; k < t.size(); k++) t[k] = (u8)((src[k] >> 8) & 0xFF);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                u8 m = 0;
                for (int d = -pad; d <= pad; d++) {
                    int xx = x + d;
                    if (xx >= 0 && xx < W) m = std::max(m, t[(size_t)y * W + xx]);
                }
                tmp[(size_t)y * W + x] = m;
            }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                u8 m = 0;
                for (int d = -pad; d <= pad; d++) {
                    int yy = y + d;
                    if (yy >= 0 && yy < H) m = std::max(m, tmp[(size_t)yy * W + x]);
                }
                o[(size_t)y * W + x] = m;
            }
        px.resize((size_t)W * H);
        for (size_t k = 0; k < px.size(); k++) {
            float at = t[k] / 255.0f, ao = o[k] / 255.0f * 0.85f;
            float a = at + ao * (1 - at);
            u32 g = a > 0 ? (u32)(255 * at / a + 0.5f) : 0;
            px[k] = ((u32)(a * 255 + 0.5f) << 24) | (g << 16) | (g << 8) | g;
        }
        SelectObject(dc, oldbm);
        DeleteObject(bm);
        SelectObject(dc, oldf);
        DeleteObject(f);
        DeleteDC(dc);
        bw = W;
        bh = H;
        for_w = pic_w;
        for_h = pic_h;
        drawn = line;
        serial++;
    }
    const u64 V = 0x5B5000000000ull + serial;
    if (version != V || out.size() != px.size()) out = px;
    w = bw;
    h = bh;
    version = V;
    return true;
}

}  // namespace subs
