// GS front end: registers, vertex kick, transfers, CLUT, privileged registers, display output.
#include "gs.h"
#include "../savestate.h"
#include <windows.h>
#include "gs_state.h"
#include "gs_census.h"
#include "gs_gpu.h"
#include "../ee/kernel.h"
#include "../platform/platform.h"
#include "../hw/gspipe.h"
#include <atomic>

namespace gs {

State st;
u32 clut[512];

// privileged registers
static u64 g_pmode = 0, g_smode1 = 0, g_smode2 = 0, g_srfsh = 0, g_synch1 = 0, g_synch2 = 0, g_syncv = 0;
static u64 g_dispfb[2] = {0, 0}, g_display[2] = {0, 0};
static std::atomic<u64> g_flips{0};  // counted on the emulation thread
static u64 g_vram_gen = 0;  // bumped by anything that may change VRAM (draws, transfers)
void draw_stats_frame_end();
int g_gs_threads = -1;
static u64 g_extbuf = 0, g_extdata = 0, g_extwrite = 0, g_bgcolor = 0;
static std::atomic<u64> g_csr{0};    // event bits: set by both threads
static std::atomic<u32> g_imr{0x7F00};
// the emulation thread's copy of the display registers (reads); the variables above belong to the
// GS pipeline (present)
static u64 g_ee_priv[0x20];
static u64 g_busdir = 0;
static bool g_field = false;
static u32 g_crt_interlace = 1, g_crt_mode = 2, g_crt_field = 0;

// CLUT cache state
static u32 g_cbp0 = 0xFFFFFFFF, g_cbp1 = 0xFFFFFFFF;

// transfer state
static int g_trx_x = 0, g_trx_y = 0;
static bool g_trx_active = false;
static std::vector<u8> g_trx_pending;    // partial bytes for odd-size formats
static std::vector<u8> g_readback;       // local->host data
static size_t g_readback_pos = 0;

void init() {
    mem_init();
    raster_init(g_gs_threads);
    memset(&st, 0, sizeof(st));
    st.prmodecont = 1;
    st.colclamp = 1;
    st.q = 1.0f;
}

void set_crt(u32 interlace, u32 mode, u32 field) {
    g_crt_interlace = interlace;
    g_crt_mode = mode;
    g_crt_field = field;
    log_write(LOG_GS, "SetGsCrt(interlace=%u, mode=%u, field=%u)", interlace, mode, field);
}

u32 get_imr() { return g_imr; }
void put_imr(u32 v) { g_imr = v; }

u64 read_csr() {
    // FIFO empty (bit 14), REV/ID
    return g_csr.load() | (1ull << 14) | (0x1Bull << 16) | (0x55ull << 24) | (g_field ? (1ull << 13) : 0);
}

// SIGNAL / FINISH (GS pipeline): the interrupt goes to the emulation thread
static void raise_event(int bit, int mask_bit) {
    g_csr.fetch_or(1ull << bit);
    if (!(g_imr.load(std::memory_order_relaxed) & (1u << mask_bit))) gsp::post_intc(kernel::INTC_GS);
}

// after loading a savestate: a SIGNAL / FINISH event that is set and not masked but has no interrupt
// pending was saved between the GS and the interrupt (savestates of older versions); raise it
void reraise_events() {
    u64 csr = g_csr.load();
    u32 imr = g_imr.load();
    bool ev = ((csr & 1) && !(imr & (1u << 8))) || ((csr & 2) && !(imr & (1u << 9)));
    if (ev && !(kernel::intc_stat & (1u << kernel::INTC_GS))) {
        log_write(LOG_MAIN, "savestate: GS event without interrupt (csr %llx): raised", (unsigned long long)csr);
        kernel::raise_intc(kernel::INTC_GS);
    }
}

void present_frame();

void on_vblank_start() {
    g_field = !g_field;
    g_csr.fetch_or(1ull << 3);  // VSINT
    if (!(g_imr.load(std::memory_order_relaxed) & (1u << 11))) kernel::raise_intc(kernel::INTC_GS);  // VSMSK
    gsp::vblank();  // present_frame on the pipeline
}
void on_vblank_end() {}
u64 flip_count() { return g_flips.load(std::memory_order_relaxed); }

// emulation thread
u64 priv_read64(u32 addr) {
    addr &= 0x1FFFFFFF;
    switch (addr & 0x1FF0) {
    case 0x0000: case 0x0010: case 0x0020: case 0x0070: case 0x0080: case 0x0090: case 0x00A0: case 0x00E0:
        return g_ee_priv[(addr & 0x1F0) >> 4];
    case 0x1000: return read_csr();
    case 0x1010: return g_imr.load(std::memory_order_relaxed);
    case 0x1040: return g_busdir;
    case 0x1080:
        gsp::sync(gsp::SYNC_SIGLBL);
        return st.siglblid;
    }
    return 0;
}

int g_test_frame_delay_ms = 0;

// emulation thread: event bits, mask and the flip count are its own; the display registers are
// copied for reads and passed on to the pipeline
void priv_write64(u32 addr, u64 v) {
    addr &= 0x1FFFFFFF;
    u32 r = addr & 0x1FF0;
    if (r < 0x200) {
        if (r == 0x0090 && v != g_ee_priv[9]) {
            g_flips.fetch_add(1, std::memory_order_relaxed);
            if (g_test_frame_delay_ms) Sleep(g_test_frame_delay_ms);  // --frame-delay: simulate a heavy scene
        }
        g_ee_priv[r >> 4] = v;
        gsp::priv(r, v);
        return;
    }
    switch (r) {
    case 0x1000:
        // writing 1 clears event bits; bit 9 = reset
        g_csr.fetch_and(~(v & 0x1F));
        if (v & 0x200) {
            g_csr.store(0);
            gsp::priv(r, v);
        }
        break;
    case 0x1010: g_imr.store((u32)v); break;
    case 0x1040: g_busdir = v; break;
    case 0x1080: gsp::priv(r, v); break;
    }
}

// GS pipeline: its copy of the registers
void priv_apply(u32 reg, u64 v) {
    switch (reg) {
    case 0x0000: g_pmode = v; break;
    case 0x0010: g_smode1 = v; break;
    case 0x0020: g_smode2 = v; break;
    case 0x0030: g_srfsh = v; break;
    case 0x0040: g_synch1 = v; break;
    case 0x0050: g_synch2 = v; break;
    case 0x0060: g_syncv = v; break;
    case 0x0070: g_dispfb[0] = v; break;
    case 0x0080: g_display[0] = v; break;
    case 0x0090:
        if (v != g_dispfb[1]) {
            if (census_active()) census_frame(g_dispfb[0], v, g_pmode);
            draw_stats_frame_end();
        }
        g_dispfb[1] = v;
        break;
    case 0x00A0: g_display[1] = v; break;
    case 0x00B0: g_extbuf = v; break;
    case 0x00C0: g_extdata = v; break;
    case 0x00D0: g_extwrite = v; break;
    case 0x00E0: g_bgcolor = v; break;
    case 0x1000: st.vcount = 0; break;  // reset
    case 0x1080: st.siglblid = v; break;
    }
}

// ---------------------------------------------------------------------------
// CLUT
// ---------------------------------------------------------------------------
static void load_clut(u64 tex0) {
    u32 psm = bits(tex0, 20, 6);
    u32 cbp = bits(tex0, 37, 14);
    u32 cpsm = bits(tex0, 51, 4);
    u32 csm = bits(tex0, 55, 1);
    u32 csa = bits(tex0, 56, 5);
    u32 cld = bits(tex0, 61, 3);
    bool is8 = (psm == PSMT8 || psm == PSMT8H);
    bool is4 = (psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH);
    if (!is8 && !is4) return;
    bool load = false;
    switch (cld) {
    case 0: return;
    case 1: load = true; break;
    case 2: load = true; g_cbp0 = cbp; break;
    case 3: load = true; g_cbp1 = cbp; break;
    case 4: load = g_cbp0 != cbp; g_cbp0 = cbp; break;
    case 5: load = g_cbp1 != cbp; g_cbp1 = cbp; break;
    default: return;
    }
    if (!load) return;
    if (census_active()) census_clut(tex0, st.texclut);
    raster_clut_load(tex0, st.texclut);
}

// Load a CLUT from VRAM into dst (512 entries; CT16 entries in the low half of each word).
void exec_clut_load_m(const u32* mem, u32* dst, u64 tex0, u64 texclut) {
    u32 psm = bits(tex0, 20, 6);
    u32 cbp = bits(tex0, 37, 14);
    u32 cpsm = bits(tex0, 51, 4);
    u32 csm = bits(tex0, 55, 1);
    u32 csa = bits(tex0, 56, 5);
    bool is8 = (psm == PSMT8 || psm == PSMT8H);
    bool is4 = (psm == PSMT4 || psm == PSMT4HL || psm == PSMT4HH);
    bool ct16 = (cpsm == PSMCT16 || cpsm == PSMCT16S);
    // CSA offset addresses the 512-halfword buffer for CT16 CLUTs
    if (csm == 0) {
        if (is8) {
            for (int i = 0; i < 256; i++) {
                // CSM1 swizzle: entries 8-15 and 16-23 swapped within each 32-entry group
                int x = i & 15, y = i >> 4;
                u32 c = read_pixel_m(mem, cpsm, x, y, cbp, 1);
                int idx = (i & ~0x18) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);
                if (ct16) dst[(idx + csa * 16) & 511] = c;
                else dst[idx] = c;
            }
        } else {
            for (int i = 0; i < 16; i++) {
                int x = i & 7, y = i >> 3;
                u32 c = read_pixel_m(mem, cpsm, x, y, cbp, 1);
                if (ct16) dst[(i + csa * 16) & 511] = c;
                else dst[(i + (csa & 15) * 16) & 255] = c;
            }
        }
    } else {
        // CSM2: CLUT is a linear row in a CT16 buffer at TEXCLUT (COU, COV)
        u32 cbw = bits(texclut, 0, 6);
        u32 cou = bits(texclut, 6, 6) * 16;
        u32 cov = bits(texclut, 12, 10);
        int n = is8 ? 256 : 16;
        for (int i = 0; i < n; i++) {
            u32 c = read_pixel_m(mem, cpsm, cou + i, cov, cbp, cbw);
            dst[(i + (is4 ? csa * 16 : 0)) & 511] = c;
        }
    }
}

void exec_clut_load(u32* dst, u64 tex0, u64 texclut) { exec_clut_load_m(vram, dst, tex0, texclut); }

// Write the rows of an upload owned by worker k of n: destination row y with (y >> 3) % n == k.
void exec_upload_rows_m(u32* mem, const UploadInfo& u, const u8* data, size_t len, int k, int n) {
    int bpp = psm_bpp(u.dpsm);
    bool is24 = (u.dpsm == PSMCT24 || u.dpsm == PSMZ24);
    size_t bytes_pp2 = bpp == 4 ? 1 : (is24 ? 6 : (size_t)bpp / 4);  // bytes per 2 pixels
    size_t total_px = std::min<size_t>((size_t)u.w * u.h, len * 2 / bytes_pp2);
    for (u32 r = 0; r < u.h; r++) {
        int y = (int)((u.dy + r) & 2047);
        if (n > 1 && ((y >> 3) % n) != k) continue;
        size_t i0 = (size_t)r * u.w;
        if (i0 >= total_px) break;
        u32 cnt = (u32)std::min<size_t>(u.w, total_px - i0);
        int x0 = (int)u.dx;
        switch (bpp) {
        case 32:
            if (is24) {
                const u8* p = data + i0 * 3;
                for (u32 i = 0; i < cnt; i++, p += 3) write_pixel_m(mem, u.dpsm, (x0 + i) & 2047, y, u.dbp, u.dbw, p[0] | (p[1] << 8) | (p[2] << 16));
            } else if (u.dpsm == PSMCT32) {
                const u8* p = data + i0 * 4;
                for (u32 i = 0; i < cnt; i++, p += 4) memcpy(&mem[addr32((x0 + i) & 2047, y, u.dbp, u.dbw)], p, 4);
            } else {
                const u8* p = data + i0 * 4;
                for (u32 i = 0; i < cnt; i++, p += 4) {
                    u32 v;
                    memcpy(&v, p, 4);
                    write_pixel_m(mem, u.dpsm, (x0 + i) & 2047, y, u.dbp, u.dbw, v);
                }
            }
            break;
        case 16: {
            const u8* p = data + i0 * 2;
            if (u.dpsm == PSMCT16)
                for (u32 i = 0; i < cnt; i++, p += 2) memcpy(&((u16*)mem)[addr16((x0 + i) & 2047, y, u.dbp, u.dbw)], p, 2);
            else
                for (u32 i = 0; i < cnt; i++, p += 2) write_pixel_m(mem, u.dpsm, (x0 + i) & 2047, y, u.dbp, u.dbw, p[0] | (p[1] << 8));
            break;
        }
        case 8: {
            const u8* p = data + i0;
            if (u.dpsm == PSMT8)
                for (u32 i = 0; i < cnt; i++) ((u8*)mem)[addr8((x0 + i) & 2047, y, u.dbp, u.dbw)] = p[i];
            else
                for (u32 i = 0; i < cnt; i++) write_pixel_m(mem, u.dpsm, (x0 + i) & 2047, y, u.dbp, u.dbw, p[i]);
            break;
        }
        case 4:
            for (u32 i = 0; i < cnt; i++) {
                size_t pi = i0 + i;
                u32 nib = (data[pi >> 1] >> ((pi & 1) * 4)) & 0xF;
                write_pixel_m(mem, u.dpsm, (x0 + i) & 2047, y, u.dbp, u.dbw, nib);
            }
            break;
        }
    }
}

void exec_upload_rows(const UploadInfo& u, const u8* data, size_t len, int k, int n) { exec_upload_rows_m(vram, u, data, len, k, n); }

// ---------------------------------------------------------------------------
// Transfers
// ---------------------------------------------------------------------------
static bool g_async_upload = false;
static UploadInfo g_up_info;
static std::vector<u8> g_up_data;
static size_t g_up_need = 0;
static void finish_async_upload() {
    if (!g_async_upload) return;
    g_async_upload = false;
    if (!g_up_data.empty() && g_up_info.w && g_up_info.h) raster_upload(g_up_info, g_up_data.data(), g_up_data.size());
    g_up_data.clear();
}

static void trx_start() {
    u32 dir = bits(st.trxdir, 0, 2);
    finish_async_upload();
    g_async_upload = dir == 0 && raster_async();
    if (g_async_upload) {
        g_up_info = UploadInfo{bits(st.bitbltbuf, 32, 14), bits(st.bitbltbuf, 48, 6), bits(st.bitbltbuf, 56, 6),
                               bits(st.trxpos, 32, 11), bits(st.trxpos, 48, 11), bits(st.trxreg, 0, 12), bits(st.trxreg, 32, 12)};
        int bpp = psm_bpp(g_up_info.dpsm);
        bool is24 = (g_up_info.dpsm == PSMCT24 || g_up_info.dpsm == PSMZ24);
        size_t px = (size_t)g_up_info.w * g_up_info.h;
        g_up_need = bpp == 4 ? (px + 1) / 2 : px * (is24 ? 3 : (size_t)bpp / 8);
        g_up_data.clear();
    }
    if (!g_async_upload) {
        // transfers access VRAM directly: wait for queued primitives touching the same memory
        u32 h = bits(st.trxreg, 32, 12);
        if (dir == 0 || dir == 2)
            raster_sync_rows(bits(st.bitbltbuf, 32, 14), bits(st.bitbltbuf, 48, 6), bits(st.bitbltbuf, 56, 6),
                             (int)bits(st.trxpos, 48, 11), (int)(bits(st.trxpos, 48, 11) + h), true);
        if (dir == 1 || dir == 2)
            raster_sync_rows(bits(st.bitbltbuf, 0, 14), bits(st.bitbltbuf, 16, 6), bits(st.bitbltbuf, 24, 6),
                             (int)bits(st.trxpos, 16, 11), (int)(bits(st.trxpos, 16, 11) + h), false);
    }
    {
        static int n = 0;
        if (n++ < 200)
            LOG(LOG_GS, "TRX dir=%u bitblt=%016llx pos=%016llx reg=%016llx", dir, (unsigned long long)st.bitbltbuf,
                (unsigned long long)st.trxpos, (unsigned long long)st.trxreg);
    }
    if (census_active()) {
        u32 w = bits(st.trxreg, 0, 12), h = bits(st.trxreg, 32, 12);
        if (dir == 0)
            census_upload(bits(st.bitbltbuf, 32, 14), bits(st.bitbltbuf, 48, 6), bits(st.bitbltbuf, 56, 6),
                          bits(st.trxpos, 32, 11), bits(st.trxpos, 48, 11), w, h);
        else if (dir == 1)
            census_readback(bits(st.bitbltbuf, 0, 14), bits(st.bitbltbuf, 16, 6), bits(st.bitbltbuf, 24, 6),
                            bits(st.trxpos, 0, 11), bits(st.trxpos, 16, 11), w, h);
        else if (dir == 2)
            census_copy(bits(st.bitbltbuf, 0, 14), bits(st.bitbltbuf, 16, 6), bits(st.bitbltbuf, 24, 6),
                        bits(st.trxpos, 0, 11), bits(st.trxpos, 16, 11), bits(st.bitbltbuf, 32, 14),
                        bits(st.bitbltbuf, 48, 6), bits(st.bitbltbuf, 56, 6), bits(st.trxpos, 32, 11),
                        bits(st.trxpos, 48, 11), w, h);
    }
    g_trx_x = 0;
    g_trx_y = 0;
    g_trx_pending.clear();
    g_vram_gen++;
    if (dir == 0) {
        g_trx_active = true;
    } else if (dir == 1) {
        // local -> host: produce readback buffer now
        u32 sbp = bits(st.bitbltbuf, 0, 14), sbw = bits(st.bitbltbuf, 16, 6), spsm = bits(st.bitbltbuf, 24, 6);
        u32 sx = bits(st.trxpos, 0, 11), sy = bits(st.trxpos, 16, 11);
        u32 w = bits(st.trxreg, 0, 12), h = bits(st.trxreg, 32, 12);
        int bpp = psm_bpp(spsm);
        const u32* mem = vram;
        if (gpu::enabled() && !raster_sw_active()) {
            void raster_gpu_readback(const CopyInfo& r);
            raster_gpu_readback(CopyInfo{sbp, sbw, spsm, sx, sy, 0, 0, 0, 0, 0, w, h});
            mem = gpu::memory();
        }
        g_readback.clear();
        g_readback_pos = 0;
        u32 nib = 0;
        int nibcount = 0;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) {
                u32 p = read_pixel_m(mem, spsm, sx + x, sy + y, sbp, sbw);
                switch (bpp) {
                case 32:
                    if (spsm == PSMCT24 || spsm == PSMZ24) { for (int k = 0; k < 3; k++) g_readback.push_back((u8)(p >> (k * 8))); }
                    else { for (int k = 0; k < 4; k++) g_readback.push_back((u8)(p >> (k * 8))); }
                    break;
                case 16: g_readback.push_back((u8)p); g_readback.push_back((u8)(p >> 8)); break;
                case 8: g_readback.push_back((u8)p); break;
                case 4:
                    nib |= (p & 0xF) << (nibcount * 4);
                    if (++nibcount == 2) { g_readback.push_back((u8)nib); nib = 0; nibcount = 0; }
                    break;
                }
            }
        g_trx_active = false;
    } else if (dir == 2) {
        u32 sbp = bits(st.bitbltbuf, 0, 14), sbw = bits(st.bitbltbuf, 16, 6), spsm = bits(st.bitbltbuf, 24, 6);
        u32 dbp = bits(st.bitbltbuf, 32, 14), dbw = bits(st.bitbltbuf, 48, 6), dpsm = bits(st.bitbltbuf, 56, 6);
        u32 sx = bits(st.trxpos, 0, 11), sy = bits(st.trxpos, 16, 11);
        u32 dx = bits(st.trxpos, 32, 11), dy = bits(st.trxpos, 48, 11);
        u32 w = bits(st.trxreg, 0, 12), h = bits(st.trxreg, 32, 12);
        u32 order = bits(st.trxpos, 59, 2);
        {
            CopyInfo ci{sbp, sbw, spsm, sx, sy, dbp, dbw, dpsm, dx, dy, w, h};
            void raster_copy(const CopyInfo& ci);
            raster_copy(ci);
        }
        // same format, block aligned, non-overlapping: copy whole 256-byte blocks
        u32 bw_px = 8, bh_px = 8;  // block size in pixels
        switch (spsm) {
        case PSMCT16: case PSMCT16S: case PSMZ16: case PSMZ16S: bw_px = 16; bh_px = 8; break;
        case PSMT8: bw_px = 16; bh_px = 16; break;
        case PSMT4: bw_px = 32; bh_px = 16; break;
        }
        bool blockcopy = spsm == dpsm && sx % bw_px == 0 && dx % bw_px == 0 && sy % bh_px == 0 && dy % bh_px == 0 &&
                         w % bw_px == 0 && h % bh_px == 0 &&
                         (spsm == PSMCT32 || spsm == PSMCT24 || spsm == PSMZ32 || spsm == PSMZ24 || spsm == PSMCT16 || spsm == PSMT8);
        if (blockcopy) {
            // gather all source blocks first: source and destination may overlap (scrolling copies)
            static std::vector<u32> blocks;
            blocks.resize((size_t)(w / bw_px) * (h / bh_px) * 64);
            for (int pass = 0; pass < 2; pass++) {
            u32* bp = blocks.data();
            for (u32 by = 0; by < h; by += bh_px)
                for (u32 bx = 0; bx < w; bx += bw_px, bp += 64) {
                    u32 sa, da;  // word address of the block start
                    switch (spsm) {
                    case PSMCT16: sa = addr16(sx + bx, sy + by, sbp, sbw) >> 1; da = addr16(dx + bx, dy + by, dbp, dbw) >> 1; break;
                    case PSMT8: sa = addr8(sx + bx, sy + by, sbp, sbw) >> 2; da = addr8(dx + bx, dy + by, dbp, dbw) >> 2; break;
                    case PSMZ32: case PSMZ24: sa = addr32z(sx + bx, sy + by, sbp, sbw); da = addr32z(dx + bx, dy + by, dbp, dbw); break;
                    default: sa = addr32(sx + bx, sy + by, sbp, sbw); da = addr32(dx + bx, dy + by, dbp, dbw); break;
                    }
                    sa &= 0xFFFC0u;
                    da &= 0xFFFC0u;
                    if (pass == 0) memcpy(bp, &vram[sa], 256);
                    else if (spsm == PSMCT24 || spsm == PSMZ24)
                        for (int i = 0; i < 64; i++) vram[da + i] = (vram[da + i] & 0xFF000000u) | (bp[i] & 0xFFFFFF);
                    else memcpy(&vram[da], bp, 256);
                }
            }
            g_trx_active = false;
            return;
        }
        // copy through a temp buffer (handles overlap regardless of DIR)
        static std::vector<u32> tmp;
        tmp.resize((size_t)w * h);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) tmp[(size_t)y * w + x] = read_pixel(spsm, (sx + x) & 2047, (sy + y) & 2047, sbp, sbw);
        (void)order;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) write_pixel(dpsm, (dx + x) & 2047, (dy + y) & 2047, dbp, dbw, tmp[(size_t)y * w + x]);
        g_trx_active = false;
    } else {
        g_trx_active = false;
    }
}

void image_write(const u8* data, u32 qwc) {
    if (!g_trx_active) return;
    g_vram_gen++;
    if (g_async_upload) {
        size_t take = std::min<size_t>((size_t)qwc * 16, g_up_need - g_up_data.size());
        g_up_data.insert(g_up_data.end(), data, data + take);
        if (g_up_data.size() >= g_up_need) {
            g_trx_active = false;
            finish_async_upload();
        }
        return;
    }
    u32 dbp = bits(st.bitbltbuf, 32, 14), dbw = bits(st.bitbltbuf, 48, 6), dpsm = bits(st.bitbltbuf, 56, 6);
    u32 dx = bits(st.trxpos, 32, 11), dy = bits(st.trxpos, 48, 11);
    u32 w = bits(st.trxreg, 0, 12), h = bits(st.trxreg, 32, 12);
    if (!w || !h) return;
    int bpp = psm_bpp(dpsm);
    bool is24 = (dpsm == PSMCT24 || dpsm == PSMZ24);
    const u8* p = data;
    const u8* end = data + qwc * 16;
    // combine leftover bytes
    std::vector<u8> merged;
    if (!g_trx_pending.empty()) {
        merged = g_trx_pending;
        merged.insert(merged.end(), data, end);
        p = merged.data();
        end = merged.data() + merged.size();
        g_trx_pending.clear();
    }
    auto advance = [&]() {
        if (++g_trx_x >= (int)w) {
            g_trx_x = 0;
            if (++g_trx_y >= (int)h) g_trx_active = false;
        }
    };
    // fast path: whole row segments for the common formats (same result as the per-pixel loop below)
    if (!is24 && (bpp == 32 || bpp == 16 || bpp == 8)) {
        int bytes = bpp / 8;
        while (g_trx_active && end - p >= bytes) {
            int y = (dy + g_trx_y) & 2047;
            int n = std::min<int>((int)w - g_trx_x, (int)((end - p) / bytes));
            int x0 = (int)dx + g_trx_x;
            switch (dpsm) {
            case PSMCT32:
                for (int i = 0; i < n; i++, p += 4) memcpy(&vram[addr32((x0 + i) & 2047, y, dbp, dbw)], p, 4);
                break;
            case PSMZ32:
                for (int i = 0; i < n; i++, p += 4) memcpy(&vram[addr32z((x0 + i) & 2047, y, dbp, dbw)], p, 4);
                break;
            case PSMCT16:
                for (int i = 0; i < n; i++, p += 2) memcpy(&vram16()[addr16((x0 + i) & 2047, y, dbp, dbw)], p, 2);
                break;
            case PSMT8:
                for (int i = 0; i < n; i++, p += 1) vram8()[addr8((x0 + i) & 2047, y, dbp, dbw)] = *p;
                break;
            default:
                for (int i = 0; i < n; i++, p += bytes) {
                    u32 v = 0;
                    memcpy(&v, p, bytes);
                    write_pixel(dpsm, (x0 + i) & 2047, y, dbp, dbw, v);
                }
                break;
            }
            g_trx_x += n;
            if (g_trx_x >= (int)w) {
                g_trx_x = 0;
                if (++g_trx_y >= (int)h) g_trx_active = false;
            }
        }
        if (g_trx_active && p < end) g_trx_pending.assign(p, end);
        return;
    }
    while (g_trx_active && p < end) {
        int x = (dx + g_trx_x) & 2047, y = (dy + g_trx_y) & 2047;
        switch (bpp) {
        case 32:
            if (is24) {
                if (end - p < 3) goto leftover;
                write_pixel(dpsm, x, y, dbp, dbw, p[0] | (p[1] << 8) | (p[2] << 16));
                p += 3;
            } else {
                u32 v;
                memcpy(&v, p, 4);
                write_pixel(dpsm, x, y, dbp, dbw, v);
                p += 4;
            }
            advance();
            break;
        case 16:
            write_pixel(dpsm, x, y, dbp, dbw, p[0] | (p[1] << 8));
            p += 2;
            advance();
            break;
        case 8:
            write_pixel(dpsm, x, y, dbp, dbw, p[0]);
            p += 1;
            advance();
            break;
        case 4:
            write_pixel(dpsm, x, y, dbp, dbw, p[0] & 0xF);
            advance();
            if (g_trx_active) {
                write_pixel(dpsm, (dx + g_trx_x) & 2047, (dy + g_trx_y) & 2047, dbp, dbw, p[0] >> 4);
                advance();
            }
            p += 1;
            break;
        }
    }
    return;
leftover:
    g_trx_pending.assign(p, end);
}

void image_read(u8* dst, u32 qwc) {
    for (u32 i = 0; i < qwc * 16; i++) dst[i] = g_readback_pos < g_readback.size() ? g_readback[g_readback_pos++] : 0;
}

// ---------------------------------------------------------------------------
// Vertex kick / primitive assembly
// ---------------------------------------------------------------------------
static inline int cur_ctx() {
    u64 m = (st.prmodecont & 1) ? st.prim : st.prmode;
    return (int)bits(m, 9, 1);
}

static void vertex_kick(u64 xyz, bool has_fog, bool draw) {
    g_vram_gen++;
    int c = cur_ctx();
    u64 off = st.ctx[c].xyoffset;
    Vertex v;
    s32 x = (s32)bits(xyz, 0, 16) - (s32)bits(off, 0, 16);
    s32 y = (s32)bits(xyz, 16, 16) - (s32)bits(off, 32, 16);
    v.x = x;
    v.y = y;
    v.z = has_fog ? bits(xyz, 32, 24) : (u32)(xyz >> 32);
    v.f = has_fog ? (u8)bits(xyz, 56, 8) : (u8)bits(st.fog, 56, 8);
    v.r = (u8)bits(st.rgbaq, 0, 8);
    v.g = (u8)bits(st.rgbaq, 8, 8);
    v.b = (u8)bits(st.rgbaq, 16, 8);
    v.a = (u8)bits(st.rgbaq, 24, 8);
    u32 qbits = (u32)(st.rgbaq >> 32);
    memcpy(&v.q, &qbits, 4);
    u32 sb = (u32)st.st, tb = (u32)(st.st >> 32);
    memcpy(&v.s, &sb, 4);
    memcpy(&v.t, &tb, 4);
    v.u = (s32)bits(st.uv, 0, 14);
    v.v = (s32)bits(st.uv, 16, 14);

    int prim = (int)bits(st.prim, 0, 3);
    switch (prim) {
    case 0:  // point
        if (draw) draw_primitive(0, &v);
        st.vcount = 0;
        break;
    case 1:  // line
        st.vq[st.vcount++] = v;
        if (st.vcount == 2) {
            if (draw) draw_primitive(1, st.vq);
            st.vcount = 0;
        }
        break;
    case 2:  // line strip
        st.vq[st.vcount++] = v;
        if (st.vcount == 2) {
            if (draw) draw_primitive(1, st.vq);
            st.vq[0] = st.vq[1];
            st.vcount = 1;
        }
        break;
    case 3:  // triangle
        st.vq[st.vcount++] = v;
        if (st.vcount == 3) {
            if (draw) draw_primitive(3, st.vq);
            st.vcount = 0;
        }
        break;
    case 4:  // triangle strip
        st.vq[st.vcount++] = v;
        if (st.vcount == 3) {
            if (draw) draw_primitive(3, st.vq);
            st.vq[0] = st.vq[1];
            st.vq[1] = st.vq[2];
            st.vcount = 2;
        }
        break;
    case 5:  // triangle fan
        st.vq[st.vcount++] = v;
        if (st.vcount == 3) {
            if (draw) draw_primitive(3, st.vq);
            st.vq[1] = st.vq[2];
            st.vcount = 2;
        }
        break;
    case 6:  // sprite
        st.vq[st.vcount++] = v;
        if (st.vcount == 2) {
            if (draw) draw_primitive(6, st.vq);
            st.vcount = 0;
        }
        break;
    default:
        st.vcount = 0;
        break;
    }
}

void gif_tag_start() {}

void write_reg(u32 reg, u64 v) {
    switch (reg) {
    case 0x00:
        st.prim = v;
        st.vcount = 0;
        break;
    case 0x01: st.rgbaq = v; break;
    case 0x02: st.st = v; break;
    case 0x03: st.uv = v; break;
    case 0x04: vertex_kick(v, true, true); break;
    case 0x05: vertex_kick(v, false, true); break;
    case 0x06: case 0x07: {
        int c = reg - 0x06;
        st.ctx[c].tex0 = v;
        load_clut(v);
        break;
    }
    case 0x08: st.ctx[0].clamp = v; break;
    case 0x09: st.ctx[1].clamp = v; break;
    case 0x0A: st.fog = v; break;
    case 0x0C: vertex_kick(v, true, false); break;
    case 0x0D: vertex_kick(v, false, false); break;
    case 0x14: st.ctx[0].tex1 = v; break;
    case 0x15: st.ctx[1].tex1 = v; break;
    case 0x16: case 0x17: {
        // TEX2: update PSM and CLUT fields of TEX0
        int c = reg - 0x16;
        u64 mask = (0x3Full << 20) | (0x7FFFFFFFFull << 37);
        st.ctx[c].tex0 = (st.ctx[c].tex0 & ~mask) | (v & mask);
        load_clut(st.ctx[c].tex0);
        break;
    }
    case 0x18: st.ctx[0].xyoffset = v; break;
    case 0x19: st.ctx[1].xyoffset = v; break;
    case 0x1A: st.prmodecont = v; break;
    case 0x1B: st.prmode = v; break;
    case 0x1C: st.texclut = v; break;
    case 0x22: st.scanmsk = v; break;
    case 0x34: st.ctx[0].miptbp1 = v; break;
    case 0x35: st.ctx[1].miptbp1 = v; break;
    case 0x36: st.ctx[0].miptbp2 = v; break;
    case 0x37: st.ctx[1].miptbp2 = v; break;
    case 0x3B: st.texa = v; break;
    case 0x3D: st.fogcol = v; break;
    case 0x3F: break;  // TEXFLUSH
    case 0x40: st.ctx[0].scissor = v; break;
    case 0x41: st.ctx[1].scissor = v; break;
    case 0x42: st.ctx[0].alpha = v; break;
    case 0x43: st.ctx[1].alpha = v; break;
    case 0x44: st.dimx = v; break;
    case 0x45: st.dthe = v; break;
    case 0x46: st.colclamp = v; break;
    case 0x47: st.ctx[0].test = v; break;
    case 0x48: st.ctx[1].test = v; break;
    case 0x49: st.pabe = v; break;
    case 0x4A: st.ctx[0].fba = v; break;
    case 0x4B: st.ctx[1].fba = v; break;
    case 0x4C: st.ctx[0].frame = v; break;
    case 0x4D: st.ctx[1].frame = v; break;
    case 0x4E: st.ctx[0].zbuf = v; break;
    case 0x4F: st.ctx[1].zbuf = v; break;
    case 0x50: st.bitbltbuf = v; break;
    case 0x51: st.trxpos = v; break;
    case 0x52: st.trxreg = v; break;
    case 0x53:
        st.trxdir = v;
        trx_start();
        break;
    case 0x54: {
        u8 b[8];
        memcpy(b, &v, 8);
        // HWREG writes are 64-bit; pad to qword granularity via pending buffer
        g_trx_pending.insert(g_trx_pending.end(), b, b + 8);
        if (g_trx_pending.size() >= 16) {
            std::vector<u8> tmp = g_trx_pending;
            g_trx_pending.clear();
            u32 q = (u32)tmp.size() / 16;
            image_write(tmp.data(), q);
            g_trx_pending.insert(g_trx_pending.end(), tmp.begin() + q * 16, tmp.end());
        }
        break;
    }
    case 0x60: {  // SIGNAL
        u32 id = (u32)v, mask = (u32)(v >> 32);
        u32 cur = (u32)st.siglblid;
        st.siglblid = (st.siglblid & 0xFFFFFFFF00000000ull) | ((cur & ~mask) | (id & mask));
        raise_event(0, 8);
        break;
    }
    case 0x61:  // FINISH
        raise_event(1, 9);
        break;
    case 0x62: {  // LABEL
        u32 id = (u32)v, mask = (u32)(v >> 32);
        u32 cur = (u32)(st.siglblid >> 32);
        st.siglblid = (st.siglblid & 0xFFFFFFFFull) | ((u64)((cur & ~mask) | (id & mask)) << 32);
        break;
    }
    default:
        LOG(LOG_GS, "GS: unhandled register %02x = %016llx", reg, (unsigned long long)v);
        break;
    }
}

void write_packed(u32 reg, const u8* qw) {
    u64 lo, hi;
    memcpy(&lo, qw, 8);
    memcpy(&hi, qw + 8, 8);
    switch (reg) {
    case 0x0:  // PRIM
        write_reg(0x00, lo & 0x7FF);
        break;
    case 0x1: {  // RGBAQ: Q comes from ST
        u32 r = (u32)lo & 0xFF, g = (u32)(lo >> 32) & 0xFF, b = (u32)hi & 0xFF, a = (u32)(hi >> 32) & 0xFF;
        u32 qb;
        memcpy(&qb, &st.q, 4);
        st.rgbaq = r | (g << 8) | (b << 16) | ((u64)a << 24) | ((u64)qb << 32);
        break;
    }
    case 0x2: {  // ST (+Q)
        st.st = lo;
        u32 qb = (u32)hi;
        memcpy(&st.q, &qb, 4);
        break;
    }
    case 0x3:  // UV
        st.uv = (lo & 0x3FFF) | (((lo >> 32) & 0x3FFF) << 16);
        break;
    case 0x4: case 0xC: {  // XYZF2 / XYZF3
        u64 x = lo & 0xFFFF, y = (lo >> 32) & 0xFFFF;
        u64 z = (hi >> 4) & 0xFFFFFF, f = (hi >> 36) & 0xFF;
        bool adc = (hi >> 47) & 1;
        u64 v = x | (y << 16) | (z << 32) | (f << 56);
        vertex_kick(v, true, !(adc || reg == 0xC));
        break;
    }
    case 0x5: case 0xD: {  // XYZ2 / XYZ3
        u64 x = lo & 0xFFFF, y = (lo >> 32) & 0xFFFF;
        u64 z = hi & 0xFFFFFFFF;
        bool adc = (hi >> 47) & 1;
        u64 v = x | (y << 16) | (z << 32);
        vertex_kick(v, false, !(adc || reg == 0xD));
        break;
    }
    case 0x6: write_reg(0x06, lo); break;
    case 0x7: write_reg(0x07, lo); break;
    case 0x8: write_reg(0x08, lo); break;
    case 0x9: write_reg(0x09, lo); break;
    case 0xA:  // FOG
        st.fog = ((hi >> 36) & 0xFF) << 56;
        break;
    case 0xE:  // A+D
        write_reg((u32)(hi & 0xFF), lo);
        break;
    case 0xF:  // NOP
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Display output (PCRTC)
// ---------------------------------------------------------------------------
static std::vector<u32> g_out;

static inline u32 expand16(u32 c) {
    u32 r = (c & 0x1F) << 3, g = ((c >> 5) & 0x1F) << 3, b = ((c >> 10) & 0x1F) << 3;
    return r | (g << 8) | (b << 16) | ((c & 0x8000) ? 0x80000000u : 0);
}

static u32 read_display_pixel(int circuit, int x, int y) {
    u64 fb = g_dispfb[circuit];
    u32 fbp = bits(fb, 0, 9) * 32, fbw = bits(fb, 9, 6), psm = bits(fb, 15, 5);
    u32 dbx = bits(fb, 32, 11), dby = bits(fb, 43, 11);
    int px = (int)dbx + x, py = (int)dby + y;
    switch (psm) {
    case PSMCT32: return read_pixel(PSMCT32, px & 2047, py & 2047, fbp, fbw);
    case PSMCT24: return read_pixel(PSMCT24, px & 2047, py & 2047, fbp, fbw) | 0xFF000000u;
    case PSMCT16: return expand16(read_pixel(PSMCT16, px & 2047, py & 2047, fbp, fbw));
    case PSMCT16S: return expand16(read_pixel(PSMCT16S, px & 2047, py & 2047, fbp, fbw));
    }
    return 0;
}

// one row of a display buffer as RGBA8
static void read_display_row(int circuit, int y, int w, u32* out) {
    u64 fb = g_dispfb[circuit];
    u32 fbp = bits(fb, 0, 9) * 32, fbw = bits(fb, 9, 6), psm = bits(fb, 15, 5);
    int dbx = (int)bits(fb, 32, 11), py = ((int)bits(fb, 43, 11) + y) & 2047;
    switch (psm) {
    case PSMCT32:
        for (int x = 0; x < w; x++) out[x] = vram[addr32((dbx + x) & 2047, py, fbp, fbw)];
        break;
    case PSMCT24:
        for (int x = 0; x < w; x++) out[x] = vram[addr32((dbx + x) & 2047, py, fbp, fbw)] | 0xFF000000u;
        break;
    case PSMCT16:
        for (int x = 0; x < w; x++) out[x] = expand16(vram16()[addr16((dbx + x) & 2047, py, fbp, fbw)]);
        break;
    case PSMCT16S:
        for (int x = 0; x < w; x++) out[x] = expand16(vram16()[addr16s((dbx + x) & 2047, py, fbp, fbw)]);
        break;
    default:
        for (int x = 0; x < w; x++) out[x] = 0;
        break;
    }
}

extern u64 g_prims_drawn;
}
namespace vu { extern u64 g_vu1_starts, g_xgkicks, g_vu_instrs; }
namespace gs {

}  // namespace gs
#include "../memory.h"
u8* gs_dbg_ram() { return mem::ram; }
namespace gs {
double g_vram_dump_at = 0;
std::string g_vram_dump_path;

static void sync_display_buffer(int c, int h) {
    u64 fb = g_dispfb[c];
    int dby = (int)bits(fb, 43, 11);
    raster_sync_rows(bits(fb, 0, 9) * 32, bits(fb, 9, 6), bits(fb, 15, 5), dby, dby + h, false);
}

void present_frame() {
    if (g_vram_dump_at > 0 && kernel::emu_time() >= g_vram_dump_at) raster_flush();
    if (g_vram_dump_at > 0 && kernel::emu_time() >= g_vram_dump_at) {
        g_vram_dump_at = 0;
        FILE* f = fopen(g_vram_dump_path.c_str(), "wb");
        if (f) {
            fwrite(vram, 1, 4 * 1024 * 1024, f);
            fwrite(clut, 1, sizeof(clut), f);
            fclose(f);
            std::string rp = g_vram_dump_path.substr(0, g_vram_dump_path.find_last_of("/\\") + 1) + "ram.bin";
            FILE* r = fopen(rp.c_str(), "wb");
            if (r) {
                extern u8* (*gs_dbg_ram_fn)();
                fwrite(mem::ram, 1, 32 * 1024 * 1024, r);
                fclose(r);
            }
            std::string ip = g_vram_dump_path.substr(0, g_vram_dump_path.find_last_of("/\\") + 1) + "iop.bin";
            FILE* ir = fopen(ip.c_str(), "wb");
            if (ir) {
                fwrite(mem::iop_ram, 1, 2 * 1024 * 1024, ir);
                fclose(ir);
            }
            log_write(LOG_GS, "VRAM dumped to %s", g_vram_dump_path.c_str());
        }
    }
    static int frame = 0;
    if ((++frame % 60) == 0) {
        static u64 last_flips = 0;
        static double last_t = 0;
        double now = host_time_sec();
        static u64 last_px = 0;
        LOG(LOG_GS, "game fps %.1f (%llu flips / 60 vblanks in %.2fs), %.2f Mpixels shaded", (g_flips - last_flips) / (now - last_t),
            (unsigned long long)(g_flips - last_flips), now - last_t, (pixels_shaded() - last_px) / 1e6);
        last_px = pixels_shaded();
        LOG(LOG_GS, "raster: %s", raster_stats().c_str());
        if (gpu::enabled()) LOG(LOG_GS, "%s", gpu::stats().c_str());
        LOG(LOG_GS, "%s", gsp::stats().c_str());
        {
            static u64 last_vi = 0;
            LOG(LOG_GS, "vu: %.1f M instructions/s", (vu::g_vu_instrs - last_vi) / 1e6 / (now - last_t));
            last_vi = vu::g_vu_instrs;
        }
        last_flips = g_flips;
        last_t = now;
        LOG(LOG_GS, "frame %d: vu1=%llu xgkick=%llu prims=%llu pmode=%llx smode2=%llx dispfb1=%llx display1=%llx dispfb2=%llx display2=%llx",
            frame, (unsigned long long)vu::g_vu1_starts, (unsigned long long)vu::g_xgkicks, (unsigned long long)g_prims_drawn, (unsigned long long)g_pmode, (unsigned long long)g_smode2,
            (unsigned long long)g_dispfb[0], (unsigned long long)g_display[0], (unsigned long long)g_dispfb[1],
            (unsigned long long)g_display[1]);
    }
    PresentParams p;
    p.pmode = g_pmode;
    p.dispfb[0] = g_dispfb[0];
    p.dispfb[1] = g_dispfb[1];
    p.display[0] = g_display[0];
    p.display[1] = g_display[1];
    p.smode2 = g_smode2;
    p.bgcolor = g_bgcolor;
    bool en1 = g_pmode & 1, en2 = (g_pmode >> 1) & 1;
    if (!en1 && !en2) {
        p.w = 640;
        p.h = p.out_h = 448;
        p.field_mode = false;
    } else {
        // use the enabled circuit with the larger display area to define output size
        int main_c = en2 ? 1 : 0;
        u64 disp = g_display[main_c];
        u32 magh = bits(disp, 23, 4) + 1, magv = bits(disp, 27, 2) + 1;
        u32 dw = bits(disp, 32, 12) + 1, dh = bits(disp, 44, 11) + 1;
        int w = (int)(dw / magh), h = (int)(dh / magv);
        if (w <= 0 || w > 1024) w = 640;
        if (h <= 0 || h > 1024) h = 448;
        bool interlace = bits(g_smode2, 0, 1);
        bool ffmd = bits(g_smode2, 1, 1);
        p.field_mode = interlace && ffmd;  // field mode: each field rendered separately
        p.w = w;
        p.h = h;
        p.out_h = p.field_mode ? h * 2 : h;
    }
    // nothing new to show: the platform keeps displaying the last submitted frame
    {
        static u64 last_sig[9];
        u64 sig[9] = {g_vram_gen, g_pmode, g_dispfb[0], g_dispfb[1], g_display[0], g_display[1], g_smode2, g_bgcolor,
                      (u64)p.w << 32 | (u32)p.out_h};
        if (memcmp(sig, last_sig, sizeof(sig)) == 0) return;
        memcpy(last_sig, sig, sizeof(sig));
    }
    raster_present(p);
}

// one row of a display buffer (as given by a DISPFB value) as RGBA8
static void read_display_row_fb(u64 fb, int y, int w, u32* out) {
    u32 fbp = bits(fb, 0, 9) * 32, fbw = bits(fb, 9, 6), psm = bits(fb, 15, 5);
    int dbx = (int)bits(fb, 32, 11), py = ((int)bits(fb, 43, 11) + y) & 2047;
    switch (psm) {
    case PSMCT32:
        for (int x = 0; x < w; x++) out[x] = vram[addr32((dbx + x) & 2047, py, fbp, fbw)];
        break;
    case PSMCT24:
        for (int x = 0; x < w; x++) out[x] = vram[addr32((dbx + x) & 2047, py, fbp, fbw)] | 0xFF000000u;
        break;
    case PSMCT16:
        for (int x = 0; x < w; x++) out[x] = expand16(vram16()[addr16((dbx + x) & 2047, py, fbp, fbw)]);
        break;
    case PSMCT16S:
        for (int x = 0; x < w; x++) out[x] = expand16(vram16()[addr16s((dbx + x) & 2047, py, fbp, fbw)]);
        break;
    default:
        for (int x = 0; x < w; x++) out[x] = 0;
        break;
    }
}

void compose_present_rows(const PresentParams& p, u32* out, int k, int n) {
    bool en1 = p.pmode & 1, en2 = (p.pmode >> 1) & 1;
    u32 alp = bits(p.pmode, 8, 8);
    bool mmod = bits(p.pmode, 5, 1);
    bool slbg = bits(p.pmode, 7, 1);
    u32 bg = (u32)p.bgcolor & 0xFFFFFF;
    const int w = p.w;
    u32 r1[1024], r2[1024];
    for (int y = 0; y < p.out_h; y++) {
        if (n > 1 && ((y >> 3) % n) != k) continue;
        u32* row = out + (size_t)y * w;
        if (!en1 && !en2) {
            for (int x = 0; x < w; x++) row[x] = 0xFF000000u;
            continue;
        }
        int sy = p.field_mode ? y / 2 : y;
        if (en2) read_display_row_fb(p.dispfb[1], sy, w, r2);
        if (en1 && (!en2 || !slbg)) read_display_row_fb(p.dispfb[0], sy, w, r1);
        for (int x = 0; x < w; x++) {
            u32 c2 = en2 ? r2[x] : bg;
            if (en1 && en2 && !slbg) {
                u32 c1 = r1[x];
                u32 a = mmod ? alp : ((c1 >> 24) & 0xFF);
                a = a > 128 ? 128 : a;  // alpha 0x80 = 1.0
                u32 o = 0;
                for (int c = 0; c < 3; c++) {
                    int s1 = (c1 >> (c * 8)) & 0xFF, s2 = (c2 >> (c * 8)) & 0xFF;
                    int v = (s1 * (int)a + s2 * (int)(128 - a)) >> 7;
                    o |= (u32)(v > 255 ? 255 : v) << (c * 8);
                }
                row[x] = o | 0xFF000000u;
            } else if (en1 && !en2) {
                row[x] = r1[x] | 0xFF000000u;
            } else {
                row[x] = c2 | 0xFF000000u;
            }
        }
    }
}

}  // namespace gs

namespace gs {
void raster_state_vram(u8* vram, u32* clut512, bool load);
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    s.pod(st, "gs::State");
    s.arr(clut);
    s.v(g_pmode); s.v(g_smode1); s.v(g_smode2); s.v(g_srfsh); s.v(g_synch1); s.v(g_synch2); s.v(g_syncv);
    s.arr(g_dispfb); s.arr(g_display);
    u64 csr = g_csr.load();
    u32 imr = g_imr.load();
    s.v(g_extbuf); s.v(g_extdata); s.v(g_extwrite); s.v(g_bgcolor); s.v(csr); s.v(imr); s.v(g_busdir);
    if (s.loading) {
        g_csr.store(csr);
        g_imr.store(imr);
        u64 regs[9] = {g_pmode, g_smode1, g_smode2, 0, 0, 0, 0, g_dispfb[0], g_display[0]};
        for (int i = 0; i < 9; i++) g_ee_priv[i] = regs[i];
        g_ee_priv[9] = g_dispfb[1];
        g_ee_priv[10] = g_display[1];
        g_ee_priv[14] = g_bgcolor;
    }
    s.v(g_field); s.v(g_crt_interlace); s.v(g_crt_mode); s.v(g_crt_field); s.v(g_cbp0); s.v(g_cbp1);
    s.v(g_trx_x); s.v(g_trx_y); s.v(g_trx_active);
    s.vec(g_trx_pending); s.vec(g_readback);
    u64 rp = g_readback_pos;
    s.v(rp);
    g_readback_pos = (size_t)rp;
    // local memory, from / into the renderer
    std::vector<u8> vram;
    std::vector<u32> rclut(512);
    if (!s.loading && !s.counting()) {
        vram.resize(4u << 20);
        raster_state_vram(vram.data(), rclut.data(), false);
    }
    s.vec(vram);
    s.vec(rclut);
    if (s.loading && s.ok) {
        if (vram.size() != (4u << 20) || rclut.size() != 512) { s.fail("GS memory size"); return; }
        raster_state_vram(vram.data(), rclut.data(), true);
        g_vram_gen++;
    }
}
}  // namespace gs
