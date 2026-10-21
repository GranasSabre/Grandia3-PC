// VIF0/VIF1: VIFcode processing, UNPACK into VU memory, MPG uploads, VU kicks, DIRECT to GIF.
#include "vif.h"
#include "../savestate.h"
#include "../platform/platform.h"
#include "gif.h"
#include "../memory.h"
#include "../ee/kernel.h"
#include "../vu/vu.h"
#include "../gs/gs.h"
#include "gspipe.h"

namespace vif {

struct VifState {
    int idx;
    // registers
    u32 stat = 0, fbrst = 0, err = 0, mark = 0;
    u32 cycle_cl = 0, cycle_wl = 0;
    u32 mode = 0, num = 0, mask = 0, code = 0;
    u32 itops = 0, base = 0, ofst = 0, tops = 0, itop = 0, top = 0;
    u32 row[4] = {0, 0, 0, 0}, col[4] = {0, 0, 0, 0};
    bool dbf = false;
    // command parser state
    u32 cmd = 0;         // current VIFcode
    u32 remaining = 0;   // words remaining for current command
    u32 sub = 0;         // sub-progress (e.g., words received into STROW/STCOL)
    // unpack state
    u32 up_addr = 0, up_num = 0, up_cl = 0, up_wl_cnt = 0;
    u32 up_vn = 0, up_vl = 0;
    bool up_usn = false, up_mask = false;
    u32 up_bits_needed = 0;
    u32 bitbuf[8];
    u32 bitbuf_words = 0;
    // mpg
    u32 mpg_addr = 0;
    // direct
    u8 direct_buf[16];
    u32 direct_fill = 0;
    bool irq_stall = false;
};

static VifState g_vif[2];
static std::vector<u32> g_upbuf[2];  // UNPACK payload being gathered (VIF1 runs on the GS pipeline thread)

void init() {
    g_vif[0] = VifState();
    g_vif[1] = VifState();
    g_vif[0].idx = 0;
    g_vif[1].idx = 1;
}

static inline u32 vu_mem_qw(int idx) { return idx ? 1024 : 256; }
static inline u8* vu_data(int idx) { return idx ? mem::vu1_data : g_vu0_mem; }

static u32 unpack_words_for(u32 cmd, u32 num) {
    u32 vn = (cmd >> 2) & 3, vl = cmd & 3;
    u32 bits_per_elem = (32 >> vl) * (vn + 1);
    if (vl == 3) bits_per_elem = 16;  // V4-5
    if (num == 0) num = 256;
    u32 bits = bits_per_elem * num;
    return (bits + 31) / 32;
}

// Write one qword of unpacked data with masking/mode applied
static void unpack_write(VifState& v, u32 qaddr, u32 data[4], u32 cycle_row) {
    u32* dst = (u32*)(vu_data(v.idx) + (qaddr % vu_mem_qw(v.idx)) * 16);
    for (int k = 0; k < 4; k++) {
        u32 m = 0;
        if (v.up_mask) m = (v.mask >> ((cycle_row > 3 ? 3 : cycle_row) * 8 + k * 2)) & 3;
        u32 val;
        switch (m) {
        case 0:
            val = data[k];
            if (v.mode == 1) val = val + v.row[k];
            else if (v.mode == 2) { val = val + v.row[k]; v.row[k] = val; }
            break;
        case 1: val = v.row[k]; break;
        case 2: val = v.col[cycle_row > 3 ? 3 : cycle_row]; break;
        default: continue;  // write protect
        }
        dst[k] = val;
    }
}

// Decode one element from raw words starting at bit offset
static void decode_element(VifState& v, const u32* src, u32 elem_index, u32 out[4]) {
    u32 vn = v.up_vn, vl = v.up_vl;
    if (vl == 3) {  // V4-5 (16-bit RGBA5551)
        u32 h = (src[elem_index / 2] >> ((elem_index & 1) * 16)) & 0xFFFF;
        out[0] = (h & 0x1F) << 3;
        out[1] = ((h >> 5) & 0x1F) << 3;
        out[2] = ((h >> 10) & 0x1F) << 3;
        out[3] = ((h >> 15) & 1) << 7;
        return;
    }
    u32 n = vn + 1;
    u32 width = 32 >> vl;
    u32 vals[4];
    for (u32 k = 0; k < 4; k++) {
        u32 comp = elem_index * n + (k < n ? k : (k % n));
        if (vn == 2 && k == 3) comp = elem_index * n + 3;  // V3: W gets the next word (hardware quirk)
        u32 bit = comp * width;
        u32 w = src[bit / 32];
        u32 val;
        if (width == 32) val = w;
        else {
            val = (w >> (bit % 32)) & ((1u << width) - 1);
            if (!v.up_usn) {
                if (width == 16) val = (u32)(s32)(s16)val;
                else if (width == 8) val = (u32)(s32)(s8)val;
            }
        }
        vals[k] = val;
    }
    if (vn == 0) {  // S: broadcast
        for (int k = 0; k < 4; k++) out[k] = vals[0];
    } else if (vn == 1) {  // V2: x y x y (z,w undefined -> repeat)
        out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[0]; out[3] = vals[1];
    } else {
        for (int k = 0; k < 4; k++) out[k] = vals[k];
    }
}

// Fast path for the common case (no write mask, skipping write): the format is a template
// parameter, so the per-element work is a few loads and shifts. Same results as decode_element +
// unpack_write.
template <int VN, int VL, bool USN>
static inline u32 fetch(const u32* src, u32 comp) {
    constexpr u32 width = 32u >> VL;
    if constexpr (width == 32) {
        return src[comp];
    } else {
        u32 bit = comp * width;
        u32 val = (src[bit >> 5] >> (bit & 31)) & ((1u << width) - 1);
        if constexpr (!USN) {
            if constexpr (width == 16) val = (u32)(s32)(s16)val;
            else val = (u32)(s32)(s8)val;
        }
        return val;
    }
}

template <int VN, int VL, bool USN>
static void unpack_fast(VifState& v, const u32* src, u32 num, u32 cl, u32 wl, u32 addr) {
    u32* base = (u32*)vu_data(v.idx);
    const u32 memqw = vu_mem_qw(v.idx);
    const u32 mode = v.mode;
    u32 cyc = 0;
    for (u32 e = 0; e < num; e++) {
        u32 d[4];
        if constexpr (VL == 3) {  // V4-5 (16-bit RGBA5551)
            u32 h = (src[e / 2] >> ((e & 1) * 16)) & 0xFFFF;
            d[0] = (h & 0x1F) << 3;
            d[1] = ((h >> 5) & 0x1F) << 3;
            d[2] = ((h >> 10) & 0x1F) << 3;
            d[3] = ((h >> 15) & 1) << 7;
        } else if constexpr (VN == 0) {
            d[0] = d[1] = d[2] = d[3] = fetch<VN, VL, USN>(src, e);
        } else if constexpr (VN == 1) {
            d[0] = d[2] = fetch<VN, VL, USN>(src, e * 2);
            d[1] = d[3] = fetch<VN, VL, USN>(src, e * 2 + 1);
        } else if constexpr (VN == 2) {
            d[0] = fetch<VN, VL, USN>(src, e * 3);
            d[1] = fetch<VN, VL, USN>(src, e * 3 + 1);
            d[2] = fetch<VN, VL, USN>(src, e * 3 + 2);
            d[3] = fetch<VN, VL, USN>(src, e * 3 + 3);  // W gets the next word (hardware quirk)
        } else {
            d[0] = fetch<VN, VL, USN>(src, e * 4);
            d[1] = fetch<VN, VL, USN>(src, e * 4 + 1);
            d[2] = fetch<VN, VL, USN>(src, e * 4 + 2);
            d[3] = fetch<VN, VL, USN>(src, e * 4 + 3);
        }
        u32* dst = base + (addr % memqw) * 4;
        if (mode == 1) {
            for (int k = 0; k < 4; k++) dst[k] = d[k] + v.row[k];
        } else if (mode == 2) {
            for (int k = 0; k < 4; k++) { v.row[k] += d[k]; dst[k] = v.row[k]; }
        } else {
            memcpy(dst, d, 16);
        }
        addr++;
        if (++cyc >= wl) {
            addr += cl - wl;
            cyc = 0;
        }
    }
}

typedef void (*UnpackFn)(VifState&, const u32*, u32, u32, u32, u32);
template <int VN, int VL> static constexpr UnpackFn pick(bool usn) { return usn ? unpack_fast<VN, VL, true> : unpack_fast<VN, VL, false>; }
static UnpackFn unpack_fn(u32 vn, u32 vl, bool usn) {
    static const UnpackFn tbl[4][4][2] = {
        {{pick<0, 0>(false), pick<0, 0>(true)}, {pick<0, 1>(false), pick<0, 1>(true)}, {pick<0, 2>(false), pick<0, 2>(true)}, {nullptr, nullptr}},
        {{pick<1, 0>(false), pick<1, 0>(true)}, {pick<1, 1>(false), pick<1, 1>(true)}, {pick<1, 2>(false), pick<1, 2>(true)}, {nullptr, nullptr}},
        {{pick<2, 0>(false), pick<2, 0>(true)}, {pick<2, 1>(false), pick<2, 1>(true)}, {pick<2, 2>(false), pick<2, 2>(true)}, {nullptr, nullptr}},
        {{pick<3, 0>(false), pick<3, 0>(true)}, {pick<3, 1>(false), pick<3, 1>(true)}, {pick<3, 2>(false), pick<3, 2>(true)}, {pick<3, 3>(false), pick<3, 3>(true)}},
    };
    return tbl[vn & 3][vl & 3][usn ? 1 : 0];
}

static void do_unpack(VifState& v, const u32* words) {
    // words contains the full packet payload
    u32 num = v.up_num ? v.up_num : 256;
    u32 cl = v.cycle_cl, wl = v.cycle_wl;
    u32 addr = v.up_addr;
    if (wl == 0) wl = cl ? cl : 1;
    if (cl == 0) cl = wl;
    if (!v.up_mask && wl <= cl) {
        if (UnpackFn f = unpack_fn(v.up_vn, v.up_vl, v.up_usn)) {
            f(v, words, num, cl, wl, addr);
            return;
        }
    }
    u32 elem = 0;
    if (wl <= cl) {
        // skipping write: write WL qwords per CL-cycle, skip CL-WL
        u32 cyc = 0;
        for (u32 i = 0; i < num; i++) {
            u32 d[4];
            decode_element(v, words, elem++, d);
            unpack_write(v, addr, d, cyc);
            addr++;
            cyc++;
            if (cyc >= wl) {
                addr += cl - wl;
                cyc = 0;
            }
        }
    } else {
        // filling write: per WL cycle, first CL from data, rest filled (mask/row/col)
        u32 cyc = 0;
        u32 written = 0;
        while (written < num) {
            u32 d[4] = {0, 0, 0, 0};
            if (cyc < cl) decode_element(v, words, elem++, d);
            else {
                // fill: data from row/col per mask; treat as masked write with data = row
                for (int k = 0; k < 4; k++) d[k] = v.row[k];
            }
            unpack_write(v, addr, d, cyc);
            addr++;
            written++;
            cyc++;
            if (cyc >= wl) cyc = 0;
        }
    }
}

static void vu_kick(VifState& v, u32 addr, bool cont) {
    if (v.idx == 1) {
        // double buffering: TOP/ITOP latched at kick
        v.top = v.tops;
        v.itop = v.itops;
        v.dbf = !v.dbf;
        v.tops = v.dbf ? v.base + v.ofst : v.base;
        vu::vu1_set_top(v.top, v.itop);
        // skipping a cutscene: VU1 only transforms geometry for the GS, nothing is drawn
        if (platform::skip_active()) return;
        if (cont) vu::vu1_continue();
        else vu::vu1_start(addr);
    } else {
        vu::vu0_set_itop(v.itops);
        if (cont) vu::vu0_continue();
        else vu::vu0_start(addr);
    }
}

// Process VIF data words
static void process_words(VifState& v, const u32* w, u32 n) {
    u32 i = 0;
    while (i < n) {
        if (v.remaining == 0) {
            // new VIFcode
            u32 code = w[i++];
            v.code = code;
            u32 cmd = (code >> 24) & 0x7F;
            u32 imm = code & 0xFFFF;
            u32 num = (code >> 16) & 0xFF;
            v.cmd = code;
            v.sub = 0;
            if (cmd >= 0x60) {
                // UNPACK
                v.up_vn = (cmd >> 2) & 3;
                v.up_vl = cmd & 3;
                v.up_mask = (cmd >> 4) & 1;
                v.up_num = num;
                v.up_usn = (imm >> 14) & 1;
                u32 a = imm & 0x3FF;
                if (v.idx == 1 && (imm & 0x8000)) a += v.tops;
                v.up_addr = a;
                v.remaining = unpack_words_for(cmd, num);
                v.bitbuf_words = 0;
                continue;
            }
            switch (cmd) {
            case 0x00: break;  // NOP
            case 0x01: v.cycle_cl = imm & 0xFF; v.cycle_wl = (imm >> 8) & 0xFF; break;
            case 0x02: v.ofst = imm & 0x3FF; v.dbf = false; v.tops = v.base; break;
            case 0x03: v.base = imm & 0x3FF; break;
            case 0x04: v.itops = imm & 0x3FF; break;
            case 0x05: v.mode = imm & 3; break;
            case 0x06: gif::set_path3_mask((imm >> 15) & 1); break;
            case 0x07: v.mark = imm; break;
            case 0x10: case 0x11: case 0x13: break;  // FLUSHE/FLUSH/FLUSHA: VU runs synchronously
            case 0x14: LOG(LOG_VIF, "VIF%d MSCAL %04x", v.idx, imm * 8); vu_kick(v, imm * 8, false); break;          // MSCAL
            case 0x15: vu_kick(v, imm * 8, false); break;          // MSCALF
            case 0x17: vu_kick(v, 0, true); break;                 // MSCNT
            case 0x20: v.remaining = 1; break;                     // STMASK
            case 0x30: case 0x31: v.remaining = 4; break;          // STROW/STCOL
            case 0x4A:                                             // MPG
                v.mpg_addr = imm * 8;
                v.remaining = (num ? num : 256) * 2;
                LOG(LOG_VIF, "VIF%d MPG addr=%04x size=%u instrs", v.idx, v.mpg_addr, num ? num : 256);
                break;
            case 0x50: case 0x51:                                  // DIRECT / DIRECTHL
                v.remaining = (imm ? imm : 65536) * 4;
                v.direct_fill = 0;
                break;
            default:
                LOG(LOG_VIF, "VIF%d: unknown command %02x (code %08x)", v.idx, cmd, code);
                break;
            }
            if (code & 0x80000000u) {
                // interrupt bit: raise VIF interrupt (no stall emulation)
                if (v.idx) gsp::post_intc(kernel::INTC_VIF1);
                else kernel::raise_intc(kernel::INTC_VIF0);
            }
            continue;
        }
        // payload of current command
        u32 cmd = (v.cmd >> 24) & 0x7F;
        if (cmd >= 0x60) {
            // gather the whole unpack payload, then decode
            u32 total = unpack_words_for(cmd, (v.cmd >> 16) & 0xFF);
            std::vector<u32>& buf = g_upbuf[v.idx];
            if (v.sub == 0) buf.assign(total + 4, 0);
            u32 take = std::min(v.remaining, n - i);
            memcpy(&buf[v.sub], &w[i], take * 4);
            v.sub += take;
            v.remaining -= take;
            i += take;
            // skipping a cutscene: VU1 does not run, its geometry data is not needed
            if (v.remaining == 0 && !(v.idx == 1 && platform::skip_active())) do_unpack(v, buf.data());
            continue;
        }
        switch (cmd) {
        case 0x20:
            v.mask = w[i++];
            v.remaining--;
            break;
        case 0x30:
            v.row[v.sub++] = w[i++];
            v.remaining--;
            break;
        case 0x31:
            v.col[v.sub++] = w[i++];
            v.remaining--;
            break;
        case 0x4A: {
            u32 take = std::min(v.remaining, n - i);
            u8* micro = v.idx ? mem::vu1_micro : mem::vu0_micro;
            u32 msize = v.idx ? 16384 : 4096;
            for (u32 k = 0; k < take; k++) {
                memcpy(micro + ((v.mpg_addr + (v.sub + k) * 4) % msize), &w[i + k], 4);
            }
            v.sub += take;
            v.remaining -= take;
            i += take;
            if (v.remaining == 0) vu::invalidate_micro(v.idx);
            break;
        }
        case 0x50:
        case 0x51: {
            // pass qwords to GIF PATH2
            while (i < n && v.remaining) {
                if (v.direct_fill == 0 && v.remaining >= 4 && n - i >= 4) {
                    u32 qws = std::min(v.remaining, n - i) / 4;
                    gif::path2((const u8*)&w[i], qws);
                    i += qws * 4;
                    v.remaining -= qws * 4;
                    continue;
                }
                memcpy(v.direct_buf + v.direct_fill, &w[i], 4);
                v.direct_fill += 4;
                i++;
                v.remaining--;
                if (v.direct_fill == 16) {
                    gif::path2(v.direct_buf, 1);
                    v.direct_fill = 0;
                }
            }
            break;
        }
        default:
            i++;
            v.remaining--;
            break;
        }
    }
}

void transfer(int idx, const u8* data, u32 qwc) {
    process_words(g_vif[idx], (const u32*)data, qwc * 4);
}

void transfer_tag_words(int idx, const u8* words8) {
    process_words(g_vif[idx], (const u32*)words8, 2);
}

u32 read_reg(u32 addr) {
    int idx = (addr & 0x400) ? 1 : 0;
    VifState& v = g_vif[idx];
    switch (addr & 0x3F0) {
    case 0x000: {  // STAT
        u32 s = 0;
        if (v.remaining) s |= 3;  // VPS: waiting for data
        if (idx == 1 && v.dbf) s |= 1u << 7;
        return s;
    }
    case 0x010: return v.fbrst;
    case 0x020: return v.err;
    case 0x030: return v.mark;
    case 0x040: return v.cycle_cl | (v.cycle_wl << 8);
    case 0x050: return v.mode;
    case 0x060: return v.num;
    case 0x070: return v.mask;
    case 0x080: return v.code;
    case 0x090: return v.itops;
    case 0x0A0: return v.base;
    case 0x0B0: return v.ofst;
    case 0x0C0: return v.tops;
    case 0x0D0: return v.itop;
    case 0x0E0: return v.top;
    case 0x100: return v.row[0];
    case 0x110: return v.row[1];
    case 0x120: return v.row[2];
    case 0x130: return v.row[3];
    case 0x140: return v.col[0];
    case 0x150: return v.col[1];
    case 0x160: return v.col[2];
    case 0x170: return v.col[3];
    }
    return 0;
}

void write_reg(u32 addr, u32 val) {
    int idx = (addr & 0x400) ? 1 : 0;
    VifState& v = g_vif[idx];
    switch (addr & 0x3F0) {
    case 0x010:  // FBRST
        if (val & 1) {  // reset
            u32 keep_idx = v.idx;
            v = VifState();
            v.idx = keep_idx;
        }
        break;
    case 0x030: v.mark = val; break;
    }
}

void fifo_read(int idx, r128* out) {
    (void)idx;
    gs::image_read(out->ub, 1);
}

void vif1_readback(u32 madr, u32 qwc) {
    std::vector<u8> tmp(qwc * 16);
    gs::image_read(tmp.data(), qwc);
    for (u32 i = 0; i < qwc; i++) memcpy(mem::dma_ptr(madr + i * 16), &tmp[i * 16], 16);
}

}  // namespace vif

namespace vif {
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    for (auto& v : g_vif) s.pod(v, "vif::VifState");
}
}  // namespace vif
