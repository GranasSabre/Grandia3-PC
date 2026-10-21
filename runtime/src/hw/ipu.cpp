// IPU (Image Processing Unit): MPEG-2 macroblock decoder used by libmpeg for FMV playback.
#include "ipu.h"
#include "../savestate.h"
#include "hw.h"
#include "mpeg_tables.h"
#include "../memory.h"
#include "../ee/kernel.h"
#include <cmath>
#include <deque>

namespace mpeg {

void build_lut(Lut& lut, const VlcEntry* t, bool sign_bit, const char* name) {
    memset(lut.len, 0, sizeof(lut.len));
    (void)sign_bit;
    for (const VlcEntry* e = t; e->code; e++) {
        u32 code = 0;
        int len = 0;
        for (const char* p = e->code; *p; p++) {
            if (*p == ' ') continue;
            code = (code << 1) | (*p == '1');
            len++;
        }
        if (len > 16) { log_write(LOG_IPU, "VLC table %s: code too long", name); continue; }
        u32 base = code << (16 - len);
        u32 count = 1u << (16 - len);
        for (u32 i = 0; i < count; i++) {
            if (lut.len[base + i]) log_write(LOG_IPU, "VLC table %s: conflict at %s", name, e->code);
            lut.v1[base + i] = (s16)e->v1;
            lut.v2[base + i] = (s16)e->v2;
            lut.len[base + i] = (u8)len;
        }
    }
}

}  // namespace mpeg

void dma_finish_external_ipu(int ch);

namespace ipu {

using namespace mpeg;

static Lut* L_mba, *L_mbi, *L_mbp, *L_mbb, *L_cbp, *L_mc, *L_dmv, *L_dcl, *L_dcc, *L_b14, *L_b15;

// ---------------------------------------------------------------------------
// Registers / state
// ---------------------------------------------------------------------------
static u32 g_cmd_data = 0;
static u32 g_top = 0;
static bool g_busy = false;
static u32 g_ctrl = 0;   // IFC/OFC computed on read
static u32 g_cmd = 0;    // pending command word
static bool g_cmd_pending = false;
static u8 g_iq[64], g_niq[64];  // raster order
static u16 g_vqclut[16];
static u32 g_th0 = 0, g_th1 = 0;
static s32 g_dc_pred[3];

// Input: bytes buffered from the toIPU DMA. bitpos counts bits consumed from g_in.front().
static std::deque<u8> g_in;
static u32 g_bitpos = 0;   // bits consumed of the head qword (0..127)
static const size_t IN_MAX_QW = 10;  // FIFO (8) + internal buffer (2)

// Output FIFO (qword granular data produced by commands, drained by fromIPU DMA)
static std::deque<u8> g_out;

static inline u32 ctrl_pct() { return (g_ctrl >> 24) & 7; }
static inline bool ctrl_as() { return (g_ctrl >> 20) & 1; }
static inline bool ctrl_ivf() { return (g_ctrl >> 21) & 1; }
static inline bool ctrl_qst() { return (g_ctrl >> 22) & 1; }
static inline bool ctrl_mp1() { return (g_ctrl >> 23) & 1; }
static inline u32 ctrl_idp() { return (g_ctrl >> 16) & 3; }

// ---------------------------------------------------------------------------
// toIPU DMA pull
// ---------------------------------------------------------------------------
static bool g_dma_in_active = false;
static bool g_dma_out_active = false;

static bool g_in_last_block = false;

// pull one qword from DMA channel 4 (normal or source chain); false if none available
// A channel whose STR bit the game cleared (or a globally suspended DMAC) transfers nothing.
static inline bool dma_channel_running(int ch) { return (hw::dma[ch].chcr & 0x100) && !(hw::d_enable & 0x10000); }

static bool dma_pull_qword(u8* out) {
    if (!g_dma_in_active || !dma_channel_running(4)) return false;
    hw::DmaChannel& d = hw::dma[4];
    int mode = (d.chcr >> 2) & 3;
    for (int guard = 0; guard < 64; guard++) {
        if (d.qwc > 0) {
            memcpy(out, mem::dma_ptr(d.madr & ~15u), 16);
            d.madr += 16;
            d.qwc--;
            if (d.qwc == 0 && (mode != 1 || g_in_last_block)) {
                g_dma_in_active = false;
                dma_finish_external_ipu(4);
            }
            return true;
        }
        if (mode != 1 || g_in_last_block) return false;
        u64 tag;
        memcpy(&tag, mem::dma_ptr(d.tadr & ~15u), 8);
        u32 qwc = tag & 0xFFFF, id = (tag >> 28) & 7;
        u32 addr = (u32)(tag >> 32) & 0xFFFFFFF0u;
        bool tirq = (tag >> 31) & 1;
        d.chcr = (d.chcr & 0xFFFF) | ((u32)(tag >> 16) & 0xFFFF0000u);
        bool end = false;
        switch (id) {
        case 0: d.madr = addr; d.tadr += 16; end = true; break;           // refe
        case 1: d.madr = d.tadr + 16; d.tadr = d.madr + qwc * 16; break;   // cnt
        case 2: d.madr = d.tadr + 16; d.tadr = addr; break;                // next
        case 3: case 4: d.madr = addr; d.tadr += 16; break;                // ref
        default: d.madr = d.tadr + 16; end = true; break;                  // end
        }
        d.qwc = qwc;
        if (end || (tirq && ((d.chcr >> 7) & 1))) g_in_last_block = true;
    }
    return false;
}

static void dma_in_check_complete() {
    if (!g_dma_in_active) return;
    hw::DmaChannel& d = hw::dma[4];
    int mode = (d.chcr >> 2) & 3;
    if (d.qwc == 0 && (mode != 1 || g_in_last_block)) {
        LOG(LOG_IPU, "toIPU DMA complete (madr=%08x)", d.madr);
        g_dma_in_active = false;
        dma_finish_external_ipu(4);
    }
}

static void fill_input() {
    while (g_in.size() / 16 < IN_MAX_QW) {
        u8 q[16];
        if (!dma_pull_qword(q)) break;
        g_in.insert(g_in.end(), q, q + 16);
    }
    dma_in_check_complete();
}

// ---------------------------------------------------------------------------
// Bit reader over g_in (transactional: consumption committed explicitly)
// ---------------------------------------------------------------------------
struct Reader {
    u32 pos;  // bit position relative to g_in.front()
    bool stall = false;
    bool have(u32 nbits) {
        while (g_in.size() * 8 < pos + nbits) {
            u8 q[16];
            if (!dma_pull_qword(q)) { stall = true; return false; }
            g_in.insert(g_in.end(), q, q + 16);
        }
        return true;
    }
    u32 peek(u32 n) {  // n <= 32
        if (n == 0) return 0;
        if (!have(n)) return 0;
        u64 v = 0;
        u32 byte = pos >> 3, off = pos & 7;
        for (u32 i = 0; i < 5 && byte + i < g_in.size(); i++) v |= (u64)g_in[byte + i] << (32 - 8 * i);
        v <<= off;
        return (u32)((v >> (40 - n)) & ((n == 32) ? 0xFFFFFFFFull : ((1ull << n) - 1)));
    }
    // peek up to 16 bits, padding with zeros if data runs out (for VLC lookup)
    bool short_read = false;
    u32 peek16() {
        short_read = !have(16);
        stall = false;
        u64 v = 0;
        u32 byte = pos >> 3, off = pos & 7;
        for (u32 i = 0; i < 3; i++) if (byte + i < g_in.size()) v |= (u64)g_in[byte + i] << (16 - 8 * i);
        v <<= off;
        return (u32)((v >> 8) & 0xFFFF);
    }
    void skip(u32 n) { if (have(n)) pos += n; }
    u32 get(u32 n) { u32 v = peek(n); if (!stall) pos += n; return v; }
    s32 vlc(const Lut* t, s32* v2 = nullptr) {
        u32 c = peek16();
        u32 len = t->len[c];
        if (!len) {
            if (short_read) { stall = true; return 0; }
            return INT32_MIN;
        }
        if (!have(len)) return 0;
        pos += len;
        if (v2) *v2 = t->v2[c];
        return t->v1[c];
    }
};

static Reader g_r;

struct Hist { u32 cmd, data, ctrl, bytes_before, bits; };
static Hist g_hist[64];
static int g_hist_pos = 0;
static u64 g_total_consumed_bits = 0;
static void dump_hist() {
    for (int i = 0; i < 64; i++) {
        Hist& h = g_hist[(g_hist_pos + i) & 63];
        log_write(LOG_IPU, "  hist cmd=%08x data=%08x ctrl=%08x bits=%u", h.cmd, h.data, h.ctrl, h.bits);
    }
}

static void commit(const Reader& r) {
    g_total_consumed_bits += r.pos - g_bitpos;
    // Drop only fully consumed qwords so g_in.front() stays qword aligned with the toIPU
    // source; IPU_BP then reports BP/IFC/FP like hardware (games save/restore the stream
    // position as MADR - 16*(IFC+FP) + BP/8).
    u32 qw = r.pos / 128;
    if (qw * 16 > g_in.size()) qw = (u32)(g_in.size() / 16);
    g_in.erase(g_in.begin(), g_in.begin() + qw * 16);
    g_bitpos = r.pos - qw * 128;
}

static Reader begin() {
    Reader r;
    r.pos = g_bitpos;
    return r;
}

// ---------------------------------------------------------------------------
// IDCT (separable float reference)
// ---------------------------------------------------------------------------
static float g_cos[8][8];
static void idct_init() {
    for (int x = 0; x < 8; x++)
        for (int u = 0; u < 8; u++) {
            double cu = u == 0 ? sqrt(0.125) : 0.5;
            g_cos[x][u] = (float)(cu * cos((2 * x + 1) * u * 3.14159265358979323846 / 16.0));
        }
}
static void idct(s16* blk) {
    float tmp[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            float s = 0;
            for (int u = 0; u < 8; u++) s += g_cos[x][u] * blk[y * 8 + u];
            tmp[y * 8 + x] = s;
        }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++) {
            float s = 0;
            for (int v = 0; v < 8; v++) s += g_cos[y][v] * tmp[v * 8 + x];
            int r = (int)floorf(s + 0.5f);
            blk[y * 8 + x] = (s16)(r < -256 ? -256 : r > 255 ? 255 : r);
        }
}

// ---------------------------------------------------------------------------
// Block decoding
// ---------------------------------------------------------------------------
static inline int qscale(u32 qsc) { return ctrl_qst() ? non_linear_qscale[qsc & 31] : (int)(qsc & 31) * 2; }

// returns false on stall / error
static bool decode_intra_block(Reader& r, int cc, int qs, s16* blk) {
    memset(blk, 0, 64 * sizeof(s16));
    // DC
    s32 size = r.vlc(cc == 0 ? L_dcl : L_dcc);
    if (r.stall) return false;
    if (size == INT32_MIN) return false;
    s32 diff = 0;
    if (size) {
        u32 bits = r.get(size);
        if (r.stall) return false;
        diff = (bits & (1u << (size - 1))) ? (s32)bits : (s32)bits - (1 << size) + 1;
    }
    g_dc_pred[cc] += diff;
    blk[0] = (s16)(g_dc_pred[cc] << (3 - ctrl_idp()));
    const Lut* t = ctrl_ivf() ? L_b15 : L_b14;
    const u8* scan = ctrl_as() ? alternate : zigzag;
    const u8* w = g_iq;
    int i = 0;
    s32 sum = blk[0];
    for (int guard = 0; guard < 128; guard++) {
        s32 level;
        s32 run = r.vlc(t, &level);
        if (r.stall) return false;
        if (run == INT32_MIN) return false;
        if (run == EOB_RUN) break;
        if (run == ESC_RUN) {
            run = (s32)r.get(6);
            u32 lv = r.get(12);
            if (r.stall) return false;
            level = (lv & 0x800) ? (s32)lv - 4096 : (s32)lv;
        } else {
            u32 s = r.get(1);
            if (r.stall) return false;
            if (s) level = -level;
        }
        i += run + 1;
        if (i > 63) break;
        int j = scan[i];
        s32 val = (level * qs * w[j]) / 16;
        if (val > 2047) val = 2047;
        if (val < -2048) val = -2048;
        blk[j] = (s16)val;
        sum += val;
    }
    if (!(sum & 1)) blk[63] ^= 1;  // mismatch control
    idct(blk);
    return true;
}

static bool decode_nonintra_block(Reader& r, int qs, s16* blk) {
    memset(blk, 0, 64 * sizeof(s16));
    const u8* scan = ctrl_as() ? alternate : zigzag;
    const u8* w = g_niq;
    int i = -1;
    s32 sum = 0;
    bool first = true;
    for (int guard = 0; guard < 128; guard++) {
        s32 level, run;
        if (first) {
            // first coefficient: "1s" means run 0 level 1
            u32 b = r.peek(1);
            if (r.stall) return false;
            if (b) {
                r.skip(1);
                run = 0;
                level = 1;
                u32 s = r.get(1);
                if (r.stall) return false;
                if (s) level = -1;
                goto have;
            }
        }
        run = r.vlc(L_b14, &level);
        if (r.stall) return false;
        if (run == INT32_MIN) return false;
        if (run == EOB_RUN) break;
        if (run == ESC_RUN) {
            run = (s32)r.get(6);
            u32 lv = r.get(12);
            if (r.stall) return false;
            level = (lv & 0x800) ? (s32)lv - 4096 : (s32)lv;
        } else {
            u32 s = r.get(1);
            if (r.stall) return false;
            if (s) level = -level;
        }
    have:
        first = false;
        i += run + 1;
        if (i > 63) break;
        {
            int j = scan[i];
            s32 sgn = level < 0 ? -1 : 1;
            s32 val = ((2 * level + sgn) * qs * w[j]) / 32;
            if (val > 2047) val = 2047;
            if (val < -2048) val = -2048;
            blk[j] = (s16)val;
            sum += val;
        }
    }
    if (!(sum & 1)) blk[63] ^= 1;
    idct(blk);
    return true;
}

// ---------------------------------------------------------------------------
// Output helpers
// ---------------------------------------------------------------------------
static void drain_output();

static void out_push(const void* data, size_t n) {
    const u8* p = (const u8*)data;
    g_out.insert(g_out.end(), p, p + n);
    drain_output();
}

static inline u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

// YCbCr RAW8 macroblock (Y 16x16, Cb 8x8, Cr 8x8) -> RGB32 or RGB16
static void csc_mb(const u8* y, const u8* cb, const u8* cr, bool rgb16, bool dither) {
    u32 rgb32[256];
    for (int py = 0; py < 16; py++)
        for (int px = 0; px < 16; px++) {
            int Y = y[py * 16 + px];
            int Cb = cb[(py / 2) * 8 + px / 2] - 128;
            int Cr = cr[(py / 2) * 8 + px / 2] - 128;
            float yy = 1.164f * (Y - 16);
            int r = (int)(yy + 1.596f * Cr + 0.5f);
            int g = (int)(yy - 0.391f * Cb - 0.813f * Cr + 0.5f);
            int b = (int)(yy + 2.018f * Cb + 0.5f);
            u8 R = clamp8(r), G = clamp8(g), B = clamp8(b);
            u32 a = 0x80;
            if (R < g_th0 && G < g_th0 && B < g_th0) a = 0;
            else if (R < g_th1 && G < g_th1 && B < g_th1) a = 0x40;
            rgb32[py * 16 + px] = R | (G << 8) | (B << 16) | (a << 24);
        }
    if (!rgb16) {
        out_push(rgb32, sizeof(rgb32));
        return;
    }
    static const int dith[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    u16 rgb16v[256];
    for (int i = 0; i < 256; i++) {
        u32 c = rgb32[i];
        int d = dither ? dith[(i / 16) & 3][(i % 16) & 3] : 0;
        int r = clamp8((int)(c & 0xFF) + d) >> 3, g = clamp8((int)((c >> 8) & 0xFF) + d) >> 3, b = clamp8((int)((c >> 16) & 0xFF) + d) >> 3;
        rgb16v[i] = (u16)(r | (g << 5) | (b << 10) | ((c >> 24) ? 0x8000 : 0));
    }
    out_push(rgb16v, sizeof(rgb16v));
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
enum { CMD_BCLR = 0, CMD_IDEC = 1, CMD_BDEC = 2, CMD_VDEC = 3, CMD_FDEC = 4, CMD_SETIQ = 5, CMD_SETVQ = 6, CMD_CSC = 7,
       CMD_PACK = 8, CMD_SETTH = 9 };

static FILE* g_trace = nullptr;
bool g_ipu_trace = false;  // --ipu-trace: write every IPU command to ipu_trace.txt
static u32 g_cmd_func = 0, g_cmd_ra = 0;
static u64 g_cmd_count = 0;
u64 command_count() { return g_cmd_count; }
static void cmd_complete() {
    g_cmd_count++;
    if (g_ipu_trace && !g_trace) g_trace = fopen("ipu_trace.txt", "w");
    static int lines = 0;
    if (g_trace && lines++ < 20000000) fprintf(g_trace, "%08x %08x %08x %llu %08x %08x\n", g_cmd, g_cmd_data, g_ctrl, (unsigned long long)g_total_consumed_bits, g_cmd_func, g_cmd_ra);
    g_hist[g_hist_pos & 63] = Hist{g_cmd, g_cmd_data, g_ctrl, 0, (u32)g_total_consumed_bits};
    g_hist_pos++;
    {
        static int n = 0;
        u32 op = g_cmd >> 28;
        if (op != 2 && op != 3 && op != 4 && n++ < 3000) LOG(LOG_IPU, "  -> done data=%08x top=%08x ctrl=%08x out=%zu", g_cmd_data, g_top, g_ctrl, g_out.size());
    }
    g_busy = false;
    g_cmd_pending = false;
    kernel::raise_intc(kernel::INTC_IPU);
}

// IPU_TOP: the next 32 bits of the input. With fewer than 32 bits buffered (the toIPU DMA has no
// more data yet) the hardware sets TOP.BUSY and TOP holds the remaining bits; libmpeg then takes
// (-BP) & 31 of them as valid and waits for data. Zeros without BUSY would look like a start code
// to libmpeg (it ended slices early and aborted movies with "slice_start_code out of range").
static bool g_top_busy = false;
static void update_top() {
    Reader r = begin();
    g_top_busy = !r.have(32);
    u64 v = 0;
    u32 byte = r.pos >> 3, off = r.pos & 7;
    for (u32 i = 0; i < 5; i++) if (byte + i < g_in.size()) v |= (u64)g_in[byte + i] << (32 - 8 * i);
    v <<= off;
    g_top = (u32)(v >> 8);
}

// BDEC state for resumability is simple: retry whole macroblock on stall
static bool exec_bdec(u32 c) {
    Reader r = begin();
    u32 fb = c & 0x3F;
    r.skip(fb);
    if (r.stall) return false;
    bool mbi = (c >> 27) & 1;
    bool dcr = (c >> 26) & 1;
    bool dt = (c >> 25) & 1;
    u32 qsc = (c >> 16) & 31;
    int qs = qscale(qsc);
    if (dcr) {
        int v = 1 << (7 + ctrl_idp());
        g_dc_pred[0] = g_dc_pred[1] = g_dc_pred[2] = v;
    }
    s32 saved_dc[3] = {g_dc_pred[0], g_dc_pred[1], g_dc_pred[2]};
    alignas(16) s16 blocks[6][64];
    u32 cbp = 0x3F;
    if (!mbi) {
        s32 v = r.vlc(L_cbp);
        if (r.stall || v == INT32_MIN) {
            memcpy(g_dc_pred, saved_dc, sizeof(saved_dc));
            return false;
        }
        cbp = (u32)v;
        g_ctrl = (g_ctrl & ~(0x3Fu << 8)) | (cbp << 8);
    }
    for (int b = 0; b < 6; b++) {
        bool ok;
        if (mbi) ok = decode_intra_block(r, b < 4 ? 0 : b - 3, qs, blocks[b]);
        else if (cbp & (0x20 >> b)) ok = decode_nonintra_block(r, qs, blocks[b]);
        else { memset(blocks[b], 0, sizeof(blocks[b])); ok = true; }
        if (!ok) {
            if (!r.stall) {
                log_write(LOG_IPU, "IPU BDEC: bitstream error (cmd %08x ctrl %08x cbp %x blk %d)", c, g_ctrl, cbp, b);
                dump_hist();
                g_ctrl |= 1u << 14;
            }
            memcpy(g_dc_pred, saved_dc, sizeof(saved_dc));
            if (r.stall) return false;
            commit(r);
            return true;
        }
    }
    commit(r);
    // assemble RAW16 macroblock: Y (16x16) then Cb (8x8) then Cr (8x8)
    alignas(16) s16 mb[384];
    for (int b = 0; b < 4; b++) {
        int bx = (b & 1) * 8, by = (b >> 1) * 8;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int oy;
                if (dt) oy = (b >> 1) + y * 2;  // field DCT: interleave lines
                else oy = by + y;
                mb[oy * 16 + bx + x] = blocks[b][y * 8 + x];
            }
    }
    memcpy(mb + 256, blocks[4], 128);
    memcpy(mb + 320, blocks[5], 128);
    out_push(mb, sizeof(mb));
    return true;
}

static bool exec_vdec(u32 c) {
    Reader r = begin();
    r.skip(c & 0x3F);
    if (r.stall) return false;
    u32 tbl = (c >> 26) & 3;
    s32 v = 0;
    switch (tbl) {
    case 0: {
        s32 x = r.vlc(L_mba);
        if (r.stall) return false;
        if (x == INT32_MIN) v = 0;
        else if (x == 34) v = 0x23;           // escape
        else if (x == 35) v = ctrl_mp1() ? 0x22 : 0;  // stuffing
        else v = x;
        break;
    }
    case 1: {
        u32 pct = ctrl_pct();
        const Lut* t = pct == 2 ? L_mbp : pct == 3 ? L_mbb : L_mbi;
        s32 x = r.vlc(t);
        if (r.stall) return false;
        v = (x == INT32_MIN) ? 0 : x;
        // frame_pred_frame_dct = 1: motion type implicitly frame
        if ((v & MB_FWD) && pct == 2) v |= 0x80;
        if ((v & (MB_FWD | MB_BWD)) && pct == 3) v |= 0x80;
        break;
    }
    case 2: {
        s32 x = r.vlc(L_mc);
        if (r.stall) return false;
        if (x == INT32_MIN) v = 0;
        else if (x == 0) v = 0;
        else {
            u32 s = r.get(1);
            if (r.stall) return false;
            v = s ? -x : x;
        }
        break;
    }
    case 3: {
        s32 x = r.vlc(L_dmv);
        if (r.stall) return false;
        v = (x == INT32_MIN) ? 0 : x;
        break;
    }
    }
    commit(r);
    g_cmd_data = ((u32)v & 0xFFFF) | 0x10000;
    if (v == 0 && tbl == 0) {
        g_ctrl |= 1u << 14;  // ECD
        static int n = 0;
        if (n++ < 50) log_write(LOG_IPU, "IPU VDEC: error code (cmd %08x bits %llu)", c, (unsigned long long)g_total_consumed_bits);
    }
    update_top();
    return true;
}

static bool exec_fdec(u32 c) {
    Reader r = begin();
    r.skip(c & 0x3F);
    if (r.stall) return false;
    u32 v = r.peek(32);
    if (r.stall) return false;
    commit(r);
    g_cmd_data = v;
    g_top = v;
    return true;
}

static bool exec_setiq(u32 c) {
    Reader r = begin();
    r.skip(c & 0x3F);
    u8 m[64];
    for (int i = 0; i < 64; i++) {
        m[i] = (u8)r.get(8);
        if (r.stall) return false;
    }
    commit(r);
    u8* dst = ((c >> 27) & 1) ? g_niq : g_iq;
    for (int i = 0; i < 64; i++) dst[zigzag[i]] = m[i];
    return true;
}

static bool exec_setvq(u32 c) {
    Reader r = begin();
    r.skip(c & 0x3F);
    for (int i = 0; i < 16; i++) {
        u32 lo = r.get(8), hi = r.get(8);
        if (r.stall) return false;
        g_vqclut[i] = (u16)(lo | (hi << 8));
    }
    commit(r);
    return true;
}

// CSC: progress counter so partial work survives stalls
static u32 g_csc_done = 0;

static bool exec_csc(u32 c) {
    u32 mbc = c & 0x7FF;
    bool ofm = (c >> 27) & 1, dte = (c >> 26) & 1;
    while (g_csc_done < mbc) {
        Reader r = begin();
        u8 raw[384];
        for (int i = 0; i < 384; i++) {
            raw[i] = (u8)r.get(8);
            if (r.stall) return false;
        }
        commit(r);
        {
            static int n = 0;
            if (g_csc_done == 0 && n++ < 40) {
                std::string s;
                char b[8];
                for (int i : {0, 1, 2, 16, 128, 129, 240, 255, 256, 257, 264, 288, 320, 321, 328, 352}) { snprintf(b, sizeof(b), "%d ", raw[i]); s += b; }
                log_write(LOG_IPU, "CSC mb0 raw: %s", s.c_str());
            }
        }
        csc_mb(raw, raw + 256, raw + 320, ofm, dte);
        g_csc_done++;
    }
    return true;
}

static bool exec_pack(u32 c) {
    u32 mbc = c & 0x7FF;
    bool ofm = (c >> 27) & 1;
    while (g_csc_done < mbc) {
        Reader r = begin();
        u32 px[256];
        for (int i = 0; i < 256; i++) {
            u32 b0 = r.get(8), b1 = r.get(8), b2 = r.get(8), b3 = r.get(8);
            if (r.stall) return false;
            px[i] = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
        }
        commit(r);
        if (!ofm) {  // INDX4
            u8 out[128];
            for (int i = 0; i < 128; i++) out[i] = 0;
            out_push(out, sizeof(out));
        } else {
            u16 out[256];
            for (int i = 0; i < 256; i++) {
                u32 p = px[i];
                out[i] = (u16)(((p >> 3) & 0x1F) | (((p >> 11) & 0x1F) << 5) | (((p >> 19) & 0x1F) << 10) | ((p >> 31) << 15));
            }
            out_push(out, sizeof(out));
        }
        g_csc_done++;
    }
    return true;
}

// IDEC: full intra picture decode -> CSC output. Progress kept per macroblock.
static struct {
    bool active;
    int qs;
    u32 mbcount;
} g_idec;

static bool exec_idec(u32 c) {
    bool ofm = (c >> 27) & 1, dte = (c >> 26) & 1, sgn = (c >> 25) & 1, dtd = (c >> 24) & 1;
    (void)sgn;
    if (!g_idec.active) {
        Reader r = begin();
        r.skip(c & 0x3F);
        if (r.stall) return false;
        commit(r);
        g_idec.active = true;
        g_idec.qs = qscale((c >> 16) & 31);
        g_idec.mbcount = 0;
        int v = 1 << (7 + ctrl_idp());
        g_dc_pred[0] = g_dc_pred[1] = g_dc_pred[2] = v;
    }
    for (int guard = 0; guard < 100000; guard++) {
        Reader r = begin();
        s32 saved_dc[3] = {g_dc_pred[0], g_dc_pred[1], g_dc_pred[2]};
        int qs = g_idec.qs;
        // macroblock_address_increment (except first)
        if (g_idec.mbcount > 0) {
            // check for start code (end of picture data)
            u32 sc = r.peek(23);
            if (r.stall) return false;
            if (sc == 0) {
                g_ctrl |= 1u << 15;  // SCD
                commit(r);
                g_idec.active = false;
                return true;
            }
            s32 inc = r.vlc(L_mba);
            if (r.stall) return false;
            while (inc == 34) { inc = r.vlc(L_mba); if (r.stall) return false; }
            if (inc == INT32_MIN) { g_idec.active = false; g_ctrl |= 1u << 14; log_write(LOG_IPU, "IPU IDEC: error code (cmd %08x mb %u)", c, g_idec.mbcount); return true; }
        }
        // macroblock type (I picture)
        s32 mbt = r.vlc(L_mbi);
        if (r.stall) return false;
        if (mbt == INT32_MIN) { g_idec.active = false; g_ctrl |= 1u << 14; log_write(LOG_IPU, "IPU IDEC: error code (cmd %08x mb %u)", c, g_idec.mbcount); return true; }
        bool dct_type = false;
        if (dtd) { dct_type = r.get(1); if (r.stall) return false; }
        if (mbt & MB_QUANT) {
            u32 q = r.get(5);
            if (r.stall) return false;
            qs = qscale(q);
        }
        alignas(16) s16 blocks[6][64];
        bool ok = true;
        for (int b = 0; b < 6 && ok; b++) ok = decode_intra_block(r, b < 4 ? 0 : b - 3, qs, blocks[b]);
        if (!ok) {
            memcpy(g_dc_pred, saved_dc, sizeof(saved_dc));
            if (r.stall) return false;
            g_idec.active = false;
            g_ctrl |= 1u << 14;
            log_write(LOG_IPU, "IPU IDEC: block error (cmd %08x mb %u)", c, g_idec.mbcount);
            return true;
        }
        commit(r);
        g_idec.qs = qs;
        u8 y[256], cb[64], cr[64];
        for (int b = 0; b < 4; b++) {
            int bx = (b & 1) * 8, by = (b >> 1) * 8;
            for (int yy = 0; yy < 8; yy++)
                for (int xx = 0; xx < 8; xx++) {
                    int oy = dct_type ? ((b >> 1) + yy * 2) : (by + yy);
                    y[oy * 16 + bx + xx] = clamp8(blocks[b][yy * 8 + xx]);
                }
        }
        for (int i = 0; i < 64; i++) { cb[i] = clamp8(blocks[4][i]); cr[i] = clamp8(blocks[5][i]); }
        csc_mb(y, cb, cr, ofm, dte);
        g_idec.mbcount++;
    }
    return true;
}

static void try_execute() {
    if (!g_cmd_pending) return;
    fill_input();
    u32 c = g_cmd;
    bool done = false;
    switch (c >> 28) {
    case CMD_BCLR:
        g_in.clear();
        g_bitpos = c & 0x7F;
        done = true;
        break;
    case CMD_IDEC: done = exec_idec(c); break;
    case CMD_BDEC: done = exec_bdec(c); break;
    case CMD_VDEC: done = exec_vdec(c); break;
    case CMD_FDEC: done = exec_fdec(c); break;
    case CMD_SETIQ: done = exec_setiq(c); break;
    case CMD_SETVQ: done = exec_setvq(c); break;
    case CMD_CSC: done = exec_csc(c); break;
    case CMD_PACK: done = exec_pack(c); break;
    case CMD_SETTH:
        g_th0 = c & 0x1FF;
        g_th1 = (c >> 16) & 0x1FF;
        done = true;
        break;
    default:
        done = true;
        break;
    }
    if (done) cmd_complete();
    fill_input();
}

// ---------------------------------------------------------------------------
// fromIPU DMA drain
// ---------------------------------------------------------------------------
static void drain_output() {
    if (!g_dma_out_active || !dma_channel_running(3)) return;
    hw::DmaChannel& d = hw::dma[3];
    while (d.qwc > 0 && g_out.size() >= 16) {
        u8 q[16];
        for (int i = 0; i < 16; i++) q[i] = g_out[i];
        g_out.erase(g_out.begin(), g_out.begin() + 16);
        memcpy(mem::dma_ptr(d.madr & ~15u), q, 16);
        d.madr += 16;
        d.qwc--;
    }
    if (d.qwc == 0) {
        g_dma_out_active = false;
        dma_finish_external_ipu(3);
    }
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------
void init() {
    L_mba = new Lut; build_lut(*L_mba, B1, false, "B1");
    L_mbi = new Lut; build_lut(*L_mbi, B2, false, "B2");
    L_mbp = new Lut; build_lut(*L_mbp, B3, false, "B3");
    L_mbb = new Lut; build_lut(*L_mbb, B4, false, "B4");
    L_cbp = new Lut; build_lut(*L_cbp, B9, false, "B9");
    L_mc = new Lut; build_lut(*L_mc, B10, false, "B10");
    L_dmv = new Lut; build_lut(*L_dmv, B11, false, "B11");
    L_dcl = new Lut; build_lut(*L_dcl, B12, false, "B12");
    L_dcc = new Lut; build_lut(*L_dcc, B13, false, "B13");
    L_b14 = new Lut; build_lut(*L_b14, B14, true, "B14");
    L_b15 = new Lut; build_lut(*L_b15, B15, true, "B15");
    idct_init();
    for (int i = 0; i < 64; i++) { g_iq[i] = 16; g_niq[i] = 16; }
}

static u32 in_qwords() { return (u32)((g_in.size() + 15) / 16); }

u32 read_reg(u32 addr) {
    switch (addr & 0xF0) {
    case 0x00:  // IPU_CMD data
        if (addr & 4) return g_busy ? 0x80000000u : 0;
        return g_cmd_data;
    case 0x10: {  // IPU_CTRL
        if (g_busy) {
            static int n = 0;
            if (n == 300000) {
                dump_hist();
                std::string hex;
                for (size_t i = 0; i < g_in.size() && i < 164; i++) { char b[4]; snprintf(b, 4, "%02x", g_in[i]); hex += b; }
                log_write(LOG_IPU, "in-buffer: %s", hex.c_str());
            }
            if ((n++ % 200000) == 0)
                log_write(LOG_IPU, "IPU busy: cmd=%08x in=%zu bitpos=%u out=%zu dmain=%d (madr=%08x qwc=%x chcr=%08x) dmaout=%d (qwc=%x)",
                          g_cmd, g_in.size(), g_bitpos, g_out.size(), g_dma_in_active, hw::dma[4].madr, hw::dma[4].qwc,
                          hw::dma[4].chcr, g_dma_out_active, hw::dma[3].qwc);
        }
        u32 ifc = std::min<u32>(in_qwords(), 8);
        u32 ofc = std::min<u32>((u32)(g_out.size() / 16), 8);
        return (g_ctrl & ~0x800000FFu) | ifc | (ofc << 4) | (g_busy ? 0x80000000u : 0);
    }
    case 0x20: {  // IPU_BP
        u32 q = in_qwords();
        u32 fp = std::min<u32>(q, 2);
        u32 ifc = q > fp ? std::min<u32>(q - fp, 15) : 0;
        return (g_bitpos & 0x7F) | (ifc << 8) | (fp << 16);
    }
    case 0x30:  // IPU_TOP
        update_top();
        if (addr & 4) return g_top_busy ? 0x80000000u : 0;
        return g_top;
    }
    return 0;
}

void write_reg(u32 addr, u32 v) {
    switch (addr & 0xF0) {
    case 0x00:  // IPU_CMD
        {
            static int n = 0;
            u32 op = v >> 28;
            if (op != 2 && op != 3 && op != 4 && n++ < 3000) LOG(LOG_IPU, "IPU cmd %08x (in=%zu bytes, bitpos=%u, out=%zu)", v, g_in.size(), g_bitpos, g_out.size());
        }
        g_cmd = v;
        if (EEContext* c = kernel::current_ctx()) { g_cmd_func = c->cur_func; g_cmd_ra = c->r[31].uw[0]; }
        g_cmd_pending = true;
        g_busy = true;
        g_csc_done = 0;
        g_idec.active = false;
        if ((v >> 28) == CMD_BDEC || (v >> 28) == CMD_IDEC) g_ctrl &= ~(3u << 14);  // clear ECD/SCD
        try_execute();
        break;
    case 0x10:  // IPU_CTRL
        {
            static int n = 0;
            if (n++ < 50) LOG(LOG_IPU, "IPU_CTRL write %08x", v);
        }
        if (v & 0x40000000u) {  // reset
            g_in.clear();
            g_out.clear();
            g_bitpos = 0;
            g_busy = false;
            g_cmd_pending = false;
            g_ctrl = 0;
            g_dc_pred[0] = g_dc_pred[1] = g_dc_pred[2] = 128;
        }
        g_ctrl = (g_ctrl & ~0x07F30000u) | (v & 0x07F30000u);
        break;
    }
}

void fifo_read(r128* out) {
    for (int i = 0; i < 16; i++) {
        out->ub[i] = g_out.empty() ? 0 : g_out.front();
        if (!g_out.empty()) g_out.pop_front();
    }
}

void fifo_write(const r128* v) {
    g_in.insert(g_in.end(), v->ub, v->ub + 16);
    try_execute();
}

// DMAC state changed (channel re-enabled / suspend released): continue stalled work
void kick() {
    fill_input();
    try_execute();
    drain_output();
}

void dma_in(const u8* data, u32 qwc) { g_in.insert(g_in.end(), data, data + qwc * 16); }

void dma_in_start() {
    LOG(LOG_IPU, "toIPU DMA start chcr=%08x madr=%08x qwc=%x tadr=%08x", hw::dma[4].chcr, hw::dma[4].madr, hw::dma[4].qwc, hw::dma[4].tadr);
    // Normal mode with QWC = 0 transfers 0x10000 qwords (the count is decremented before it is
    // tested). The movie player relies on it at the end of a stream: it sends its 16-byte
    // sequence_end_code buffer with QWC = 0, and libmpeg waits for the IPU without a way out.
    if (((hw::dma[4].chcr >> 2) & 3) == 0 && hw::dma[4].qwc == 0) {
        static int n = 0;
        if (n++ < 20) log_write(LOG_IPU, "toIPU DMA with QWC 0 from %08x: 0x10000 qwords", hw::dma[4].madr);
        hw::dma[4].qwc = 0x10000;
    }
    g_dma_in_active = true;
    g_in_last_block = false;
    fill_input();
    try_execute();
    fill_input();
}

void dma_out_start() {
    if (hw::dma[3].qwc != 0x30) LOG(LOG_IPU, "fromIPU DMA start madr=%08x qwc=%x (out=%zu)", hw::dma[3].madr, hw::dma[3].qwc, g_out.size());
    g_dma_out_active = true;
    drain_output();
    try_execute();
}

}  // namespace ipu
void ipu_enable_trace() { ipu::g_ipu_trace = true; }
extern "C" void ipu_trace_note(const char* s) {
    if (ipu::g_trace) fprintf(ipu::g_trace, "# %s\n", s);
    ipu::dump_hist();
}

namespace ipu {
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    s.v(g_cmd_data); s.v(g_top); s.v(g_busy); s.v(g_ctrl); s.v(g_cmd); s.v(g_cmd_pending);
    s.arr(g_iq); s.arr(g_niq); s.arr(g_vqclut); s.v(g_th0); s.v(g_th1); s.arr(g_dc_pred);
    s.deq(g_in); s.v(g_bitpos); s.deq(g_out);
    s.v(g_dma_in_active); s.v(g_dma_out_active); s.v(g_in_last_block); s.v(g_top_busy);
}
}  // namespace ipu
