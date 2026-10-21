// SIF: EE<->IOP DMA and mailbox registers (EE side), packet queue to the EE.
#include "sif.h"
#include "../savestate.h"
#include "../memory.h"
#include "../ee/kernel.h"
#include "../iop/iop.h"
#include <deque>

namespace sif {

static u32 g_mscom = 0;       // EE main addr (SIF_REG_MAINADDR)
static u32 g_smcom = IOP_CMD_BUF;
static u32 g_msflg = 0;
static u32 g_smflg = 0x10000 | 0x20000 | 0x40000;  // SIFINIT | CMDINIT | BOOTEND
static u32 g_ctrl = 0;
static u32 g_sregs[32];
static int g_dma_id = 1;
static u32 g_ee_pktbuf = 0;

struct EEPacket {
    std::vector<u8> data;
};
static std::deque<EEPacket> g_queue;

void set_smflag(u32 bits) { g_smflg |= bits; }
void clear_smflag(u32 bits) { g_smflg &= ~bits; }
bool ee_ready() { return g_ee_pktbuf != 0; }

void ee_write(u32 ee_addr, const void* data, u32 size) {
    if (!size) return;
    mem::copy_to_ee(ee_addr & 0x1FFFFFFF, data, size);
}

void send_cmd_to_ee(u32 cid, const void* payload, u32 payload_size, u32 extra_ee_dst, const void* extra, u32 extra_size) {
    if (extra && extra_size) ee_write(extra_ee_dst, extra, extra_size);
    EEPacket p;
    u32 psize = 16 + payload_size;
    p.data.resize((psize + 15) & ~15u, 0);
    u32 hdr[4];
    hdr[0] = (psize & 0xFF) | ((extra_size & 0xFFFFFF) << 8);
    hdr[1] = extra_ee_dst;
    hdr[2] = cid;
    hdr[3] = 0;
    memcpy(p.data.data(), hdr, 16);
    if (payload_size) memcpy(p.data.data() + 16, payload, payload_size);
    g_queue.push_back(std::move(p));
    pump();
}

void pump() {
    if (g_queue.empty() || !g_ee_pktbuf) return;
    u8* buf = mem::ee_ptr(g_ee_pktbuf);
    if (!buf) return;
    if (buf[0] != 0) return;  // previous packet not consumed yet
    // only deliver if the EE has the SIF0 interrupt enabled, otherwise it would be lost
    EEPacket p = std::move(g_queue.front());
    g_queue.pop_front();
    memcpy(buf, p.data.data(), p.data.size());
    LOG(LOG_SIF, "-> EE packet cid=%08x size=%u", ((u32*)p.data.data())[2], (u32)p.data.size());
    kernel::raise_dmac(kernel::DMAC_SIF0);
}

int set_dma(u32 trans_ptr, int n) {
    for (int i = 0; i < n; i++) {
        u32 t = trans_ptr + i * 16;
        u32 src = mem::rd32(t + 0);
        u32 dst = mem::rd32(t + 4);
        u32 size = mem::rd32(t + 8);
        u32 attr = mem::rd32(t + 12);
        size = (size + 3) & ~3u;
        LOG(LOG_SIF, "SifSetDma src=%08x dst=%08x size=%x attr=%x", src, dst, size, attr);
        if (size > 0x200000) size = 0x200000;
        std::vector<u8> tmp(size);
        mem::copy_from_ee(tmp.data(), src & 0x1FFFFFFF, size);
        memcpy(mem::iop_ptr(dst), tmp.data(), size);
        if ((dst & 0x1FFFFF) == IOP_CMD_BUF) iop::sifcmd_received(IOP_CMD_BUF);
    }
    return g_dma_id++;
}

int dma_stat(int id) {
    (void)id;
    return -1;  // completed
}

void set_dchain() {}

u32 set_reg(u32 reg, u32 val) {
    if (reg & 0x80000000u) {
        u32 i = reg & 31;
        u32 old = g_sregs[i];
        g_sregs[i] = val;
        return old;
    }
    switch (reg) {
    case 1: g_mscom = val; if (!g_ee_pktbuf) g_ee_pktbuf = val; LOG(LOG_SIF, "MAINADDR=%08x", val); return 0;
    case 2: return 0;
    case 3: g_msflg |= val; return 0;
    case 4: g_smflg &= ~val; return 0;
    }
    return 0;
}

u32 get_reg(u32 reg) {
    if (reg & 0x80000000u) return g_sregs[reg & 31];
    switch (reg) {
    case 1: return g_mscom;
    case 2: return g_smcom;
    case 3: return g_msflg;
    case 4: return g_smflg;
    }
    return 0;
}

u32 hw_read(u32 addr) {
    switch (addr) {
    case 0x1000F200: return g_mscom;
    case 0x1000F210: return g_smcom;
    case 0x1000F220: return g_msflg;
    case 0x1000F230: return g_smflg;
    case 0x1000F240: return g_ctrl | 0xF0000102;
    }
    return 0;
}

void hw_write(u32 addr, u32 v) {
    switch (addr) {
    case 0x1000F200: g_mscom = v; break;
    case 0x1000F220: g_msflg |= v; break;
    case 0x1000F230: g_smflg &= ~v; break;
    case 0x1000F240: g_ctrl = v; break;
    }
}

// called by IOP sifcmd HLE when EE sends CHANGE_SADDR
void set_ee_pktbuf(u32 a) {
    g_ee_pktbuf = a & 0x1FFFFFFF;
    LOG(LOG_SIF, "EE packet buffer = %08x", a);
}

}  // namespace sif

namespace sif {
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    s.v(g_mscom); s.v(g_smcom); s.v(g_msflg); s.v(g_smflg); s.v(g_ctrl); s.arr(g_sregs); s.v(g_dma_id); s.v(g_ee_pktbuf);
    u32 n = (u32)g_queue.size();
    s.v(n);
    if (s.loading) g_queue.assign(n, EEPacket());
    for (auto& p : g_queue) s.vec(p.data);
}
}  // namespace sif
