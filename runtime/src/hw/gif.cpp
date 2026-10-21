// GIF: GIFtag parsing for PATH1/2/3, feeding the GS.
#include "gif.h"
#include "../savestate.h"
#include "../gs/gs.h"

namespace gif {

struct PathState {
    u64 tag_lo = 0, tag_hi = 0;
    u32 nloop = 0;      // remaining loops
    u32 nreg = 0;
    u32 flg = 0;
    u32 reg_idx = 0;
    bool eop = false;
    bool active = false;  // inside a tag
};

static PathState g_path[3];
static u32 g_mode = 0, g_ctrl = 0;
static bool g_p3_mask = false;
static u32 g_last_tag[4];

static inline u32 tag_reg(const PathState& p, u32 i) { return (u32)(p.tag_hi >> (i * 4)) & 0xF; }

static void process(int pi, const u8* data, u32 qwc) {
    PathState& p = g_path[pi];
    u32 q = 0;
    while (q < qwc) {
        if (!p.active) {
            u64 lo, hi;
            memcpy(&lo, data + q * 16, 8);
            memcpy(&hi, data + q * 16 + 8, 8);
            memcpy(g_last_tag, data + q * 16, 16);
            q++;
            p.tag_lo = lo;
            p.tag_hi = hi;
            p.nloop = lo & 0x7FFF;
            p.eop = (lo >> 15) & 1;
            p.flg = (lo >> 58) & 3;
            p.nreg = (lo >> 60) & 0xF;
            if (p.nreg == 0) p.nreg = 16;
            p.reg_idx = 0;
            if ((lo >> 46) & 1) gs::write_reg(0x00, (lo >> 47) & 0x7FF);  // PRE -> PRIM
            gs::gif_tag_start();
            if (p.nloop == 0) continue;
            p.active = true;
        }
        switch (p.flg) {
        case 0:  // PACKED
            while (q < qwc && p.nloop) {
                u32 r = tag_reg(p, p.reg_idx);
                gs::write_packed(r, data + q * 16);
                q++;
                if (++p.reg_idx >= p.nreg) { p.reg_idx = 0; p.nloop--; }
            }
            break;
        case 1:  // REGLIST
            while (q < qwc && p.nloop) {
                for (int half = 0; half < 2 && p.nloop; half++) {
                    u32 r = tag_reg(p, p.reg_idx);
                    u64 v;
                    memcpy(&v, data + q * 16 + half * 8, 8);
                    if (r != 0xE && r != 0xF) gs::write_reg(r, v);
                    if (++p.reg_idx >= p.nreg) { p.reg_idx = 0; p.nloop--; }
                }
                q++;
            }
            break;
        default: {  // IMAGE
            u32 n = std::min(qwc - q, p.nloop);
            gs::image_write(data + q * 16, n);
            q += n;
            p.nloop -= n;
            break;
        }
        }
        if (p.nloop == 0) p.active = false;
    }
}

void path1(const u8* data, u32 qwc) { process(0, data, qwc); }
void path2(const u8* data, u32 qwc) { process(1, data, qwc); }
void path3_dma(const u8* data, u32 qwc) { process(2, data, qwc); }
void path3_fifo(const u8* data, u32 qwc) { process(2, data, qwc); }
void set_path3_mask(bool m) { g_p3_mask = m; }

u32 read_reg(u32 addr) {
    switch (addr & 0xFF0) {
    case 0x000: return g_ctrl;
    case 0x010: return g_mode;
    case 0x020: return (g_p3_mask ? 2 : 0) | (g_mode & 1) | ((g_mode & 4) << 3);  // STAT: idle
    case 0x040: return g_last_tag[0];
    case 0x050: return g_last_tag[1];
    case 0x060: return g_last_tag[2];
    case 0x070: return g_last_tag[3];
    case 0x080: return 0;
    case 0x090: return 0;
    case 0x0A0: return 0;
    }
    return 0;
}

void write_reg(u32 addr, u32 v) {
    switch (addr & 0xFF0) {
    case 0x000:
        g_ctrl = v & 8;
        if (v & 1) {  // reset
            for (auto& p : g_path) p = PathState();
        }
        break;
    case 0x010: g_mode = v; break;
    }
}

}  // namespace gif

namespace gif {
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    for (auto& p : g_path) s.pod(p, "gif::PathState");
    s.v(g_mode); s.v(g_ctrl); s.v(g_p3_mask); s.arr(g_last_tag);
}
}  // namespace gif
