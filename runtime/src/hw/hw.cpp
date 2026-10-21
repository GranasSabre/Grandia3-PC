// EE hardware register dispatch, DMAC, timers, INTC.
#include "hw.h"
#include "../savestate.h"
#include "../memory.h"
#include "../ee/kernel.h"
#include "../gs/gs.h"
#include "gif.h"
#include "vif.h"
#include "gspipe.h"
#include "ipu.h"
#include "sif.h"
#include <windows.h>
#include <cmath>
#include <string>
#include <unordered_map>

namespace hw {

DmaChannel dma[10];
u32 d_ctrl = 1, d_pcr = 0, d_sqwc = 0, d_rbsr = 0, d_rbor = 0, d_stadr = 0, d_enable = 0x1201;

static const u32 DMA_BASE[10] = {0x10008000, 0x10009000, 0x1000A000, 0x1000B000, 0x1000B400,
                                 0x1000C000, 0x1000C400, 0x1000C800, 0x1000D000, 0x1000D400};

static int dma_channel_of(u32 addr) {
    for (int i = 0; i < 10; i++)
        if (addr >= DMA_BASE[i] && addr < DMA_BASE[i] + 0x100) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// Timers
// ---------------------------------------------------------------------------
struct Timer {
    u32 mode = 0, comp = 0, hold = 0;
    double count = 0;     // fractional count
    double last = 0;      // emu time of last update
    double rate() const {
        switch (mode & 3) {
        case 0: return 147456000.0;
        case 1: return 147456000.0 / 16;
        case 2: return 147456000.0 / 256;
        default: return 15734.26;
        }
    }
};
static Timer g_tim[4];

static void timer_update(int i, double now) {
    Timer& t = g_tim[i];
    double dt = now - t.last;
    t.last = now;
    if (!(t.mode & 0x80) || dt <= 0) return;
    double before = t.count;
    double after = before + dt * t.rate();
    u32 comp = t.comp & 0xFFFF;
    bool fire = false;
    if (comp > before && comp <= after) {
        if (t.mode & 0x100) { if (!(t.mode & 0x400)) fire = true; t.mode |= 0x400; }
        if (t.mode & 0x40) {  // ZRET
            double span = comp ? comp : 65536;
            after = fmod(after - comp, span);
        }
    }
    if (after >= 65536.0) {
        if (t.mode & 0x200) { if (!(t.mode & 0x800)) fire = true; t.mode |= 0x800; }
        after = fmod(after, 65536.0);
    }
    t.count = after;
    if (fire) kernel::raise_intc(kernel::INTC_TIM0 + i);
}

void timers_update() {
    double now = kernel::emu_time();
    for (int i = 0; i < 4; i++) timer_update(i, now);
}

double timers_next_event() {
    double best = 1e30;
    double now = kernel::emu_time();
    for (int i = 0; i < 4; i++) {
        Timer& t = g_tim[i];
        if (!(t.mode & 0x80) || !(t.mode & 0x300)) continue;
        double target = (t.mode & 0x100) && (t.comp & 0xFFFF) > t.count ? (t.comp & 0xFFFF) : 65536.0;
        double when = now + (target - t.count) / t.rate();
        if (when < best) best = when;
    }
    return best;
}

static u32 timer_read(u32 addr) {
    int i = (addr >> 11) & 3;
    timer_update(i, kernel::emu_time());
    switch (addr & 0x7F0) {
    case 0x00: return (u32)g_tim[i].count & 0xFFFF;
    case 0x10: return g_tim[i].mode;
    case 0x20: return g_tim[i].comp;
    case 0x30: return g_tim[i].hold;
    }
    return 0;
}

static void timer_write(u32 addr, u32 v) {
    int i = (addr >> 11) & 3;
    Timer& t = g_tim[i];
    timer_update(i, kernel::emu_time());
    switch (addr & 0x7F0) {
    case 0x00: t.count = v & 0xFFFF; break;
    case 0x10:
        t.mode = (v & 0x3FF) | (t.mode & 0xC00 & ~(v & 0xC00));
        break;
    case 0x20: t.comp = v & 0xFFFF; break;
    case 0x30: t.hold = v & 0xFFFF; break;
    }
    kernel::notify_pending();
}

// ---------------------------------------------------------------------------
// DMA
// ---------------------------------------------------------------------------
static inline u8* qw_ptr(u32 addr) { return mem::dma_ptr(addr & ~15u); }

typedef void (*Sink)(const u8* data, u32 qwc);

static void sink_for(int ch, const u8* data, u32 qwc) {
    switch (ch) {
    case 0: vif::transfer(0, data, qwc); break;
    case 1: gsp::vif1(data, qwc); break;   // GS pipeline thread
    case 2: gsp::path3(data, qwc); break;
    case 4: ipu::dma_in(data, qwc); break;
    default: LOG(LOG_DMA, "DMA ch%d: unhandled sink (%u qw)", ch, qwc); break;
    }
}

static void transfer_block(int ch, u32 addr, u32 qwc) {
    if (!qwc) return;
    // a block may wrap within SPR; RAM blocks are contiguous
    if (addr & 0x80000000u) {
        u32 off = addr & 0x3FF0;
        while (qwc) {
            u32 n = std::min<u32>(qwc, (0x4000 - off) / 16);
            sink_for(ch, mem::spr + off, n);
            qwc -= n;
            off = 0;
        }
    } else {
        sink_for(ch, qw_ptr(addr), qwc);
    }
}

static void tte_send(int ch, const u8* tag) {
    if (ch == 0) vif::transfer_tag_words(0, tag + 8);
    else if (ch == 1) gsp::vif1_words(tag + 8);
}

static void source_chain(int ch) {
    DmaChannel& d = dma[ch];
    bool tte = (d.chcr >> 6) & 1;
    bool tie = (d.chcr >> 7) & 1;
    // finish any pending normal block first
    if (d.qwc) {
        transfer_block(ch, d.madr, d.qwc);
        d.madr += d.qwc * 16;
        d.qwc = 0;
        u32 lastid = (d.chcr >> 28) & 7;
        if (lastid == 0 || lastid == 7 || (((d.chcr >> 31) & 1) && tie)) return;
    }
    for (int guard = 0; guard < 1000000; guard++) {
        const u8* tp = qw_ptr(d.tadr);
        u64 tag;
        memcpy(&tag, tp, 8);
        u32 qwc = tag & 0xFFFF;
        u32 id = (tag >> 28) & 7;
        bool irq = (tag >> 31) & 1;
        u32 addr = (u32)(tag >> 32) & 0xFFFFFFF0u;
        d.chcr = (d.chcr & 0xFFFF) | ((u32)(tag >> 16) & 0xFFFF0000u);
        if (tte) tte_send(ch, tp);
        bool end = false;
        switch (id) {
        case 0:  // refe
            transfer_block(ch, addr, qwc);
            d.madr = addr + qwc * 16;
            d.tadr += 16;
            end = true;
            break;
        case 1:  // cnt
            d.madr = d.tadr + 16;
            transfer_block(ch, d.madr, qwc);
            d.madr += qwc * 16;
            d.tadr = d.madr;
            break;
        case 2:  // next
            d.madr = d.tadr + 16;
            transfer_block(ch, d.madr, qwc);
            d.madr += qwc * 16;
            d.tadr = addr;
            break;
        case 3:  // ref
        case 4:  // refs
            transfer_block(ch, addr, qwc);
            d.madr = addr + qwc * 16;
            d.tadr += 16;
            break;
        case 5: {  // call
            d.madr = d.tadr + 16;
            transfer_block(ch, d.madr, qwc);
            d.madr += qwc * 16;
            int asp = (d.chcr >> 4) & 3;
            if (asp == 0) d.asr0 = d.madr;
            else if (asp == 1) d.asr1 = d.madr;
            else { LOG(LOG_DMA, "DMA ch%d call stack overflow", ch); end = true; }
            d.chcr = (d.chcr & ~0x30u) | ((u32)(asp + 1) << 4);
            d.tadr = addr;
            break;
        }
        case 6: {  // ret
            d.madr = d.tadr + 16;
            transfer_block(ch, d.madr, qwc);
            d.madr += qwc * 16;
            int asp = (d.chcr >> 4) & 3;
            if (asp == 2) { d.tadr = d.asr1; asp = 1; }
            else if (asp == 1) { d.tadr = d.asr0; asp = 0; }
            else end = true;
            d.chcr = (d.chcr & ~0x30u) | ((u32)asp << 4);
            break;
        }
        case 7:  // end
            d.madr = d.tadr + 16;
            transfer_block(ch, d.madr, qwc);
            d.madr += qwc * 16;
            end = true;
            break;
        }
        if (end || (irq && tie)) return;
    }
    LOG(LOG_DMA, "DMA ch%d: chain did not terminate", ch);
}

static inline int mfifo_mode() { return (d_ctrl >> 2) & 3; }
static inline u32 ring_wrap(u32 a) { return (d_rbor & ~d_rbsr) | (a & d_rbsr); }

static void spr_transfer(int ch) {
    DmaChannel& d = dma[ch];
    int mode = (d.chcr >> 2) & 3;
    bool to_spr = (ch == 9);
    bool mfifo = (ch == 8) && mfifo_mode() >= 2;
    auto copy_qw = [&](u32 ram_addr, u32 spr_addr, u32 n) {
        for (u32 i = 0; i < n; i++) {
            u32 ra = ram_addr + i * 16;
            if (mfifo) ra = ring_wrap(ra);
            u8* r = mem::dma_ptr(ra);
            u8* s = mem::spr + ((spr_addr + i * 16) & 0x3FF0);
            if (to_spr) memcpy(s, r, 16);
            else memcpy(r, s, 16);
        }
    };
    if (mode == 0) {
        copy_qw(d.madr, d.sadr, d.qwc);
        d.madr += d.qwc * 16;
        if (mfifo) d.madr = ring_wrap(d.madr);
        d.sadr = (d.sadr + d.qwc * 16) & 0x3FF0;
        d.qwc = 0;
    } else if (mode == 2) {  // interleave
        u32 sqwc = d_sqwc & 0xFF, tqwc = (d_sqwc >> 16) & 0xFF;
        if (!tqwc) tqwc = d.qwc;
        u32 left = d.qwc;
        while (left) {
            u32 n = std::min(left, tqwc);
            copy_qw(d.madr, d.sadr, n);
            d.madr += n * 16;
            d.sadr = (d.sadr + n * 16) & 0x3FF0;
            left -= n;
            if (left) d.madr += sqwc * 16;
        }
        d.qwc = 0;
    } else if (to_spr) {
        // source chain into SPR
        bool tte = (d.chcr >> 6) & 1;
        for (int guard = 0; guard < 100000; guard++) {
            u64 tag;
            memcpy(&tag, qw_ptr(d.tadr), 8);
            u32 qwc = tag & 0xFFFF, id = (tag >> 28) & 7;
            u32 addr = (u32)(tag >> 32) & 0xFFFFFFF0u;
            bool irq = (tag >> 31) & 1;
            if (tte) {
                memcpy(mem::spr + (d.sadr & 0x3FF0), qw_ptr(d.tadr), 16);
                d.sadr = (d.sadr + 16) & 0x3FF0;
            }
            bool end = false;
            u32 src;
            switch (id) {
            case 0: src = addr; d.tadr += 16; end = true; break;
            case 1: src = d.tadr + 16; d.tadr = src + qwc * 16; break;
            case 2: src = d.tadr + 16; d.tadr = addr; break;
            case 3: case 4: src = addr; d.tadr += 16; break;
            case 7: src = d.tadr + 16; end = true; break;
            default: src = d.tadr + 16; end = true; break;
            }
            copy_qw(src, d.sadr, qwc);
            d.sadr = (d.sadr + qwc * 16) & 0x3FF0;
            if (end || (irq && ((d.chcr >> 7) & 1))) break;
        }
    } else {
        // fromSPR destination chain: tags are in SPR
        for (int guard = 0; guard < 100000; guard++) {
            u64 tag;
            memcpy(&tag, mem::spr + (d.sadr & 0x3FF0), 8);
            d.sadr = (d.sadr + 16) & 0x3FF0;
            u32 qwc = tag & 0xFFFF, id = (tag >> 28) & 7;
            u32 addr = (u32)(tag >> 32) & 0xFFFFFFF0u;
            bool irq = (tag >> 31) & 1;
            d.madr = addr;
            copy_qw(d.madr, d.sadr, qwc);
            d.sadr = (d.sadr + qwc * 16) & 0x3FF0;
            d.madr += qwc * 16;
            if (id == 7 || (irq && ((d.chcr >> 7) & 1))) break;
        }
    }
}

static void dma_complete(int ch) {
    dma[ch].chcr &= ~0x100u;
    kernel::raise_dmac(ch);
}

// Ring buffer data block (may wrap around the end of the ring)
static void transfer_ring(int ch, u32 addr, u32 qwc) {
    while (qwc) {
        u32 off = addr & d_rbsr;
        u32 room = (d_rbsr + 16 - off) / 16;
        u32 n = std::min(qwc, room);
        sink_for(ch, qw_ptr(ring_wrap(addr)), n);
        qwc -= n;
        addr = ring_wrap(addr + n * 16);
    }
}

// Drain an MFIFO consumer channel (VIF1 or GIF) up to the fromSPR write pointer.
static void mfifo_drain(int ch) {
    DmaChannel& d = dma[ch];
    if (!(d.chcr & 0x100)) return;
    bool tte = (d.chcr >> 6) & 1;
    bool tie = (d.chcr >> 7) & 1;
    for (int guard = 0; guard < 1000000; guard++) {
        d.tadr = ring_wrap(d.tadr);
        if (d.tadr == ring_wrap(dma[8].madr)) {
            kernel::raise_dmac(14);  // MFIFO empty (MEIS)
            return;
        }
        const u8* tp = qw_ptr(d.tadr);
        u64 tag;
        memcpy(&tag, tp, 8);
        u32 qwc = tag & 0xFFFF;
        u32 id = (tag >> 28) & 7;
        bool irq = (tag >> 31) & 1;
        u32 addr = (u32)(tag >> 32) & 0xFFFFFFF0u;
        LOG(LOG_DMA, "MFIFO ch%d tag@%08x id=%u qwc=%x addr=%08x irq=%d (wptr %08x)", ch, d.tadr, id, qwc, addr, irq, dma[8].madr);
        d.chcr = (d.chcr & 0xFFFF) | ((u32)(tag >> 16) & 0xFFFF0000u);
        if (tte) tte_send(ch, tp);
        bool end = false;
        switch (id) {
        case 0:  // refe
            transfer_block(ch, addr, qwc);
            d.tadr += 16;
            end = true;
            break;
        case 1:  // cnt
            transfer_ring(ch, d.tadr + 16, qwc);
            d.tadr = d.tadr + 16 + qwc * 16;
            break;
        case 2:  // next
            transfer_ring(ch, d.tadr + 16, qwc);
            d.tadr = addr;
            break;
        case 3:
        case 4:  // ref / refs
            transfer_block(ch, addr, qwc);
            d.tadr += 16;
            break;
        case 7:  // end
            transfer_ring(ch, d.tadr + 16, qwc);
            d.tadr = d.tadr + 16 + qwc * 16;
            end = true;
            break;
        default:
            LOG(LOG_DMA, "MFIFO: unsupported tag id %u", id);
            d.tadr += 16;
            end = true;
            break;
        }
        d.tadr = ring_wrap(d.tadr);
        if (end || (irq && tie)) {
            dma_complete(ch);
            return;
        }
    }
}

static void dma_start(int ch) {
    DmaChannel& d = dma[ch];
    int mode = (d.chcr >> 2) & 3;
    LOG(LOG_DMA, "DMA start ch%d chcr=%08x madr=%08x qwc=%x tadr=%08x", ch, d.chcr, d.madr, d.qwc, d.tadr);
    switch (ch) {
    case 0:
    case 1:
    case 2:
        if (ch == 1 && !(d.chcr & 1)) {
            // VIF1 -> memory (readback from GS via VIF1 FIFO)
            gsp::sync(gsp::SYNC_READBACK);
            vif::vif1_readback(d.madr, d.qwc);
            d.madr += d.qwc * 16;
            d.qwc = 0;
            break;
        }
        if (mode == 1 && ((mfifo_mode() == 2 && ch == 1) || (mfifo_mode() == 3 && ch == 2))) {
            mfifo_drain(ch);
            return;
        }
        if (mode == 1) source_chain(ch);
        else {
            transfer_block(ch, d.madr, d.qwc);
            d.madr += d.qwc * 16;
            d.qwc = 0;
        }
        break;
    case 3:  // fromIPU
        ipu::dma_out_start();
        return;  // IPU completes asynchronously
    case 4:  // toIPU
        LOG(LOG_IPU, "DMAC: toIPU start request chcr=%08x madr=%08x qwc=%x", d.chcr, d.madr, d.qwc);
        ipu::dma_in_start();
        return;
    case 5:  // SIF0 (IOP->EE) armed
    case 6:
    case 7:
        return;
    case 8:
    case 9:
        spr_transfer(ch);
        if (ch == 8 && mfifo_mode() >= 2) {
            dma_complete(ch);
            mfifo_drain(mfifo_mode() == 2 ? 1 : 2);
            return;
        }
        break;
    }
    dma_complete(ch);
}

static u32 dma_read(int ch, u32 reg) {
    DmaChannel& d = dma[ch];
    switch (reg) {
    case 0x00: return d.chcr;
    case 0x10: return d.madr;
    case 0x20: return d.qwc;
    case 0x30: return d.tadr;
    case 0x40: return d.asr0;
    case 0x50: return d.asr1;
    case 0x80: return d.sadr;
    }
    return 0;
}

static void dma_write(int ch, u32 reg, u32 v) {
    DmaChannel& d = dma[ch];
    LOG(LOG_DMA, "DMA ch%d write reg %02x = %08x (d_ctrl=%08x)", ch, reg, v, d_ctrl);
    switch (reg) {
    case 0x00: {
        bool was = d.chcr & 0x100;
        d.chcr = v;
        // channel 4 (toIPU) may be re-armed while still marked busy by the IPU feeding logic
        if ((!was || ch == 4) && (v & 0x100)) {
            if (d_ctrl & 1) dma_start(ch);
        }
        break;
    }
    case 0x10: d.madr = v; break;
    case 0x20: d.qwc = v & 0xFFFF; break;
    case 0x30: d.tadr = v; break;
    case 0x40: d.asr0 = v; break;
    case 0x50: d.asr1 = v; break;
    case 0x80: d.sadr = v & 0x3FF0; break;
    }
}

// called by IPU when its transfer finished
void dma_finish_external(int ch) {
    dma_complete(ch);
}
}  // namespace hw
void dma_finish_external_ipu(int ch) { hw::dma_finish_external(ch); }
namespace hw {

// ---------------------------------------------------------------------------
// Busy-wait detection
// ---------------------------------------------------------------------------
static u32 g_last_poll_addr = 0;
static int g_poll_count = 0;

bool g_poll_stats = false;  // --poll-stats: log which registers the game busy-waits on
static void poll_stat(u32 addr) {
    static std::unordered_map<u32, u32> n;
    static std::unordered_map<u64, u32> callers;  // native return addresses (image offsets), for the map file
    static double last = 0;
    n[addr]++;
    {
        void* fr[6];
        USHORT k = RtlCaptureStackBackTrace(1, 6, fr, nullptr);
        u64 base = (u64)GetModuleHandleW(nullptr);
        for (USHORT j = 0; j < k; j++) callers[(u64)fr[j] - base]++;
    }
    double t = host_time_sec();
    if (t - last < 2.0) return;
    last = t;
    std::string s;
    for (auto& kv : n) {
        char b[32];
        snprintf(b, sizeof b, " %08x:%u", kv.first, kv.second);
        s += b;
    }
    log_write(LOG_MAIN, "polls (2 s):%s", s.c_str());
    s.clear();
    for (auto& kv : callers) {
        char b[40];
        snprintf(b, sizeof b, " %llx:%u", (unsigned long long)kv.first, kv.second);
        s += b;
    }
    log_write(LOG_MAIN, "poll callers:%s", s.c_str());
    callers.clear();
    n.clear();
}
static void note_poll(u32 addr) {
    if (addr == g_last_poll_addr) {
        if (++g_poll_count >= 64) {
            g_poll_count = 0;
            if (g_poll_stats) poll_stat(addr);
            kernel::wait_idle_hint();
        }
    } else {
        g_last_poll_addr = addr;
        g_poll_count = 0;
    }
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------
static u32 g_sio_line_len = 0;
static char g_sio_line[512];

static u32 misc_regs[0x1000 / 4];

void init() {}

u32 read32(u32 a) {
    u32 addr = a & 0x1FFFFFFF;
    if (addr >= 0x10000000 && addr < 0x10002000) {
        // the counter is no busy-wait sign: the game reads timer 0 to time every frame's updates,
        // thousands of times in a row (sleeping there made frames miss their vertical blank)
        if ((addr & 0x7F0) != 0x00) note_poll(addr);
        return timer_read(addr);
    }
    if (addr >= 0x10002000 && addr < 0x10003000) return ipu::read_reg(addr);
    if (addr >= 0x10003000 && addr < 0x10003800) {
        note_poll(addr);
        gsp::sync(gsp::SYNC_GIF_REG);
        return gif::read_reg(addr);
    }
    if (addr >= 0x10003800 && addr < 0x10004000) {
        note_poll(addr);
        if (addr & 0x400) gsp::sync(gsp::SYNC_VIF_REG);
        return vif::read_reg(addr);
    }
    if (addr >= 0x10007000 && addr < 0x10008000) {
        r128 q;
        ipu::fifo_read(&q);
        return q.uw[0];
    }
    if (addr >= 0x10008000 && addr < 0x1000E000) {
        int ch = dma_channel_of(addr);
        if (ch < 0) return 0;
        note_poll(addr);
        {
            u32 v = dma_read(ch, addr & 0xF0);
            static int dbg = 0;
            if ((addr & 0xF0) == 0 && (v & 0x100) && dbg++ < 5) LOG(LOG_DMA, "poll ch%d chcr=%08x qwc=%x", ch, v, dma[ch].qwc);
            return v;
        }
    }
    switch (addr) {
    case 0x1000E000: return d_ctrl;
    case 0x1000E010: note_poll(addr); return kernel::dmac_stat;
    case 0x1000E020: return d_pcr;
    case 0x1000E030: return d_sqwc;
    case 0x1000E040: return d_rbsr;
    case 0x1000E050: return d_rbor;
    case 0x1000E060: return d_stadr;
    case 0x1000F000: note_poll(addr); kernel::poll_time(); return kernel::intc_stat;
    case 0x1000F010: return kernel::intc_mask;
    case 0x1000F520: return d_enable;
    case 0x1000F130: return 0;
    case 0x1000F200: case 0x1000F210: case 0x1000F220: case 0x1000F230: case 0x1000F240: case 0x1000F260:
        return sif::hw_read(addr);
    case 0x1000F430: return 0;
    case 0x1000F440: return 0;
    }
    if (addr >= 0x11000000 && addr < 0x11010000) {
        u32 off = addr & 0xFFFF;
        u8* p;
        if (off >= 0x8000) gsp::sync(gsp::SYNC_VU_MEM);  // VU1 belongs to the GS pipeline
        if (off < 0x4000) p = mem::vu0_micro + (off & 0xFFF);
        else if (off < 0x8000) p = g_vu0_mem + (off & 0xFFF);
        else if (off < 0xC000) p = mem::vu1_micro + (off & 0x3FFF);
        else p = mem::vu1_data + (off & 0x3FFF);
        u32 v;
        memcpy(&v, p, 4);
        return v;
    }
    if (addr >= 0x12000000 && addr < 0x12002000) {
        note_poll(addr);
        u64 v = gs::priv_read64(addr & ~7u);
        return (addr & 4) ? (u32)(v >> 32) : (u32)v;
    }
    if (addr >= 0x1C000000 && addr < 0x1C200000) return mem::iop_rd32(addr);
    if (addr >= 0x1FC00000 && addr < 0x20000000) return 0;
    if (addr >= 0x1000F000 && addr < 0x10010000) return misc_regs[(addr & 0xFFF) / 4];
    LOG(LOG_HW, "read32 unknown %08x", a);
    return 0;
}

u64 read64(u32 a) {
    u32 addr = a & 0x1FFFFFFF;
    if (addr >= 0x12000000 && addr < 0x12002000) {
        note_poll(addr);
        return gs::priv_read64(addr);
    }
    return (u64)read32(a) | ((u64)read32(a + 4) << 32);
}

void read128(u32 a, r128* out) {
    u32 addr = a & 0x1FFFFFFF;
    if (addr >= 0x10007000 && addr < 0x10007010) { ipu::fifo_read(out); return; }
    if (addr >= 0x10005000 && addr < 0x10006000) {
        gsp::sync(gsp::SYNC_READBACK);
        vif::fifo_read(1, out);
        return;
    }
    for (int i = 0; i < 4; i++) out->uw[i] = read32(a + i * 4);
}

void write32(u32 a, u32 v) {
    u32 addr = a & 0x1FFFFFFF;
    if (addr >= 0x10000000 && addr < 0x10002000) { timer_write(addr, v); return; }
    if (addr >= 0x10002000 && addr < 0x10003000) { ipu::write_reg(addr, v); return; }
    if (addr >= 0x10003000 && addr < 0x10003800) { gsp::gif_reg(addr, v); return; }
    if (addr >= 0x10003800 && addr < 0x10004000) {
        if (addr & 0x400) gsp::vif1_reg(addr, v);
        else vif::write_reg(addr, v);
        return;
    }
    if (addr >= 0x10008000 && addr < 0x1000E000) {
        int ch = dma_channel_of(addr);
        if (ch >= 0) dma_write(ch, addr & 0xF0, v);
        return;
    }
    switch (addr) {
    case 0x1000E000: d_ctrl = v; return;
    case 0x1000E010: kernel::dmac_stat_write(v); return;
    case 0x1000E020: d_pcr = v; return;
    case 0x1000E030: d_sqwc = v; return;
    case 0x1000E040: d_rbsr = v; return;
    case 0x1000E050: d_rbor = v; return;
    case 0x1000E060: d_stadr = v; return;
    case 0x1000F000: kernel::intc_stat_write(v); return;
    case 0x1000F010: kernel::intc_mask_write(v); return;
    case 0x1000F590: {
        bool was_suspended = d_enable & 0x10000;
        d_enable = v;
        if (was_suspended && !(v & 0x10000)) ipu::kick();
        return;
    }
    case 0x1000F180: {  // SIO TX (kernel debug output)
        char ch = (char)v;
        if (ch == '\n' || g_sio_line_len >= sizeof(g_sio_line) - 1) {
            g_sio_line[g_sio_line_len] = 0;
            LOG(LOG_GAME, "[sio] %s", g_sio_line);
            g_sio_line_len = 0;
        } else if (ch != '\r') {
            g_sio_line[g_sio_line_len++] = ch;
        }
        return;
    }
    case 0x1000F200: case 0x1000F210: case 0x1000F220: case 0x1000F230: case 0x1000F240: case 0x1000F260:
        sif::hw_write(addr, v);
        return;
    }
    if (addr >= 0x11000000 && addr < 0x11010000) {
        u32 off = addr & 0xFFFF;
        u8* p;
        if (off >= 0x8000) gsp::sync(gsp::SYNC_VU_MEM);
        if (off < 0x4000) p = mem::vu0_micro + (off & 0xFFF);
        else if (off < 0x8000) p = g_vu0_mem + (off & 0xFFF);
        else if (off < 0xC000) p = mem::vu1_micro + (off & 0x3FFF);
        else p = mem::vu1_data + (off & 0x3FFF);
        memcpy(p, &v, 4);
        return;
    }
    if (addr >= 0x12000000 && addr < 0x12002000) {
        u64 old = gs::priv_read64(addr & ~7u);
        u64 nv = (addr & 4) ? ((old & 0xFFFFFFFFull) | ((u64)v << 32)) : ((old & ~0xFFFFFFFFull) | v);
        gs::priv_write64(addr & ~7u, nv);
        return;
    }
    if (addr >= 0x1C000000 && addr < 0x1C200000) { mem::iop_wr32(addr, v); return; }
    if (addr >= 0x1000F000 && addr < 0x10010000) { misc_regs[(addr & 0xFFF) / 4] = v; return; }
    LOG(LOG_HW, "write32 unknown %08x = %08x", a, v);
}

void write8(u32 a, u8 v) {
    u32 addr = a & 0x1FFFFFFF;
    if (addr == 0x1000F180) { write32(addr, v); return; }
    if (addr >= 0x10008000 && addr < 0x1000E000) {
        // byte write to CHCR (e.g. to set STR bit at offset 1)
        u32 base = addr & ~3u;
        u32 old = read32(base);
        int sh = (addr & 3) * 8;
        write32(base, (old & ~(0xFFu << sh)) | ((u32)v << sh));
        return;
    }
    if (addr >= 0x11000000 && addr < 0x11010000) {
        u32 base = addr & ~3u;
        u32 old = read32(base);
        int sh = (addr & 3) * 8;
        write32(base, (old & ~(0xFFu << sh)) | ((u32)v << sh));
        return;
    }
    LOG(LOG_HW, "write8 %08x = %02x", a, v);
}

void write16(u32 a, u16 v) {
    u32 addr = a & 0x1FFFFFFF;
    u32 base = addr & ~3u;
    u32 old = read32(base);
    int sh = (addr & 2) * 8;
    write32(base, (old & ~(0xFFFFu << sh)) | ((u32)v << sh));
}

void write64(u32 a, u64 v) {
    u32 addr = a & 0x1FFFFFFF;
    if (addr >= 0x12000000 && addr < 0x12002000) { gs::priv_write64(addr, v); return; }
    if (addr >= 0x10004000 && addr < 0x10008000) {
        r128 q;
        q.ud[0] = v;
        q.ud[1] = 0;
        write128(a, &q);
        return;
    }
    write32(a, (u32)v);
    write32(a + 4, (u32)(v >> 32));
}

void write128(u32 a, const r128* v) {
    u32 addr = a & 0x1FFFFFFF;
    if (addr >= 0x10004000 && addr < 0x10005000) { vif::transfer(0, v->ub, 1); return; }
    if (addr >= 0x10005000 && addr < 0x10006000) { gsp::vif1(v->ub, 1); return; }
    if (addr >= 0x10006000 && addr < 0x10007000) { gsp::path3(v->ub, 1); return; }
    if (addr >= 0x10007010 && addr < 0x10008000) { ipu::fifo_write(v); return; }
    for (int i = 0; i < 4; i++) write32(a + i * 4, v->uw[i]);
}

}  // namespace hw

namespace hw {
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    for (auto& d : dma) { s.v(d.chcr); s.v(d.madr); s.v(d.qwc); s.v(d.tadr); s.v(d.asr0); s.v(d.asr1); s.v(d.sadr); }
    s.v(d_ctrl); s.v(d_pcr); s.v(d_sqwc); s.v(d_rbsr); s.v(d_rbor); s.v(d_stadr); s.v(d_enable);
    for (auto& t : g_tim) { s.v(t.mode); s.v(t.comp); s.v(t.hold); s.v(t.count); s.v(t.last); }
    s.arr(misc_regs);
}
}  // namespace hw
