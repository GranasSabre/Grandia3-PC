// IOP side HLE: SIF command / RPC dispatch, module loading, native servers.
#include "iop.h"
#include "../savestate.h"
#include "../memory.h"
#include "../hw/sif.h"
#include "../ee/kernel.h"
#include "iop_kernel.h"
#include "modules.h"
#include "pad.h"
#include "../spu2/spu2.h"
#include <map>

namespace iop {

// ---------------------------------------------------------------------------
// Heap
// ---------------------------------------------------------------------------
static u32 g_heap_ptr = 0x00058000;
static const u32 HEAP_END = 0x001FF000;
static std::map<u32, u32> g_allocs;

u32 alloc(u32 size, u32 align) {
    u32 a = (g_heap_ptr + align - 1) & ~(align - 1);
    if (a + size > HEAP_END) {
        log_write(LOG_IOP, "IOP heap exhausted (size %x)", size);
        return 0;
    }
    g_heap_ptr = a + size;
    g_allocs[a] = size;
    memset(mem::iop_ptr(a), 0, size);
    return a;
}

void free(u32 addr) { g_allocs.erase(addr); }

// ---------------------------------------------------------------------------
// RPC servers
// ---------------------------------------------------------------------------
static std::map<u32, RpcServer*> g_servers_by_sid;
static std::map<u32, RpcServer*> g_servers_by_sd;
static u32 g_next_sd = 0x00020000;

RpcServer* register_server(RpcServer* s) {
    if (!s->sd_addr) {
        s->sd_addr = g_next_sd;
        g_next_sd += 0x40;
    }
    g_servers_by_sid[s->sid] = s;
    g_servers_by_sd[s->sd_addr] = s;
    LOG(LOG_SIF, "RPC server registered sid=%08x sd=%08x buf=%08x", s->sid, s->sd_addr, s->buf);
    return s;
}

RpcServer* register_native_server(u32 sid, u32 bufsize, RpcFunc fn) {
    RpcServer* s = new RpcServer();
    s->sid = sid;
    s->buf = alloc(bufsize ? bufsize : 64);
    s->native = std::move(fn);
    return register_server(s);
}

// RPC_END packet payload (after 16-byte cmd header)
struct RendPayload {
    u32 rec_id, pkt_addr, rpc_id, client, cid, server, buff, cbuff;
};

void rpc_complete(u32 client, u32 rec_id, u32 pkt_addr, u32 rpc_id, u32 rdata_iop, u32 recv_ee, u32 recv_size) {
    if (recv_size && recv_ee && rdata_iop) {
        sif::ee_write(recv_ee, mem::iop_ptr(rdata_iop), recv_size);
    }
    RendPayload r{rec_id, pkt_addr, rpc_id, client, 0x8000000A, 0, 0, 0};
    sif::send_cmd_to_ee(0x80000008, &r, sizeof(r));
}

static void handle_bind(u32 a) {
    u32 rec_id = mem::iop_rd32(a + 16);
    u32 pkt_addr = mem::iop_rd32(a + 20);
    u32 rpc_id = mem::iop_rd32(a + 24);
    u32 client = mem::iop_rd32(a + 28);
    u32 sid = mem::iop_rd32(a + 32);
    auto it = g_servers_by_sid.find(sid);
    RendPayload r{rec_id, pkt_addr, rpc_id, client, 0x80000009, 0, 0, 0};
    if (it != g_servers_by_sid.end()) {
        r.server = it->second->sd_addr;
        r.buff = it->second->buf;
        r.cbuff = it->second->cbuf;
        LOG(LOG_SIF, "BIND sid=%08x client=%08x -> sd=%08x", sid, client, r.server);
    } else {
        static std::map<u32, int> warned;
        if (warned[sid]++ < 3) log_write(LOG_SIF, "BIND to unknown server %08x (client %08x)", sid, client);
    }
    sif::send_cmd_to_ee(0x80000008, &r, sizeof(r));
}

static void handle_call(u32 a) {
    u32 rec_id = mem::iop_rd32(a + 16);
    u32 pkt_addr = mem::iop_rd32(a + 20);
    u32 rpc_id = mem::iop_rd32(a + 24);
    u32 client = mem::iop_rd32(a + 28);
    u32 fno = mem::iop_rd32(a + 32);
    u32 send_size = mem::iop_rd32(a + 36);
    u32 receive = mem::iop_rd32(a + 40);
    u32 recv_size = mem::iop_rd32(a + 44);
    u32 rmode = mem::iop_rd32(a + 48);
    u32 server = mem::iop_rd32(a + 52);
    (void)rmode;
    auto it = g_servers_by_sd.find(server);
    if (it == g_servers_by_sd.end()) {
        log_write(LOG_SIF, "CALL to unknown server sd=%08x fno=%u", server, fno);
        rpc_complete(client, rec_id, pkt_addr, rpc_id, 0, 0, 0);
        return;
    }
    RpcServer* s = it->second;
    LOG(LOG_SIF, "CALL sid=%08x fno=%u ssize=%u recv=%08x rsize=%u", s->sid, fno, send_size, receive, recv_size);
    if (s->native) {
        u32 rdata = s->native(fno, s->buf, send_size);
        rpc_complete(client, rec_id, pkt_addr, rpc_id, rdata, receive, recv_size);
    } else {
        kern::queue_rpc_request(s, fno, send_size, client, rec_id, pkt_addr, rpc_id, receive, recv_size);
    }
}

static void handle_rdata(u32 a) {
    // SifRpcOtherDataPkt: rec_id, pkt_addr, rpc_id, client, src, dest, size
    u32 rec_id = mem::iop_rd32(a + 16);
    u32 pkt_addr = mem::iop_rd32(a + 20);
    u32 rpc_id = mem::iop_rd32(a + 24);
    u32 client = mem::iop_rd32(a + 28);
    u32 src = mem::iop_rd32(a + 32);
    u32 dest = mem::iop_rd32(a + 36);
    u32 size = mem::iop_rd32(a + 40);
    sif::ee_write(dest, mem::iop_ptr(src), size);
    RendPayload r{rec_id, pkt_addr, rpc_id, client, 0x8000000C, 0, 0, 0};
    sif::send_cmd_to_ee(0x80000008, &r, sizeof(r));
}

static double g_reboot_done_at = 0;

void sifcmd_received(u32 a) {
    u32 hdr = mem::iop_rd32(a);
    u32 cid = mem::iop_rd32(a + 8);
    u32 opt = mem::iop_rd32(a + 12);
    (void)hdr;
    LOG(LOG_SIF, "<- EE cmd %08x opt=%08x", cid, opt);
    switch (cid) {
    case 0x80000000:  // CHANGE_SADDR
        sif::set_ee_pktbuf(mem::iop_rd32(a + 16));
        break;
    case 0x80000001:  // SET_SREG (EE->IOP)
        break;
    case 0x80000002: {  // INIT_CMD
        LOG(LOG_SIF, "INIT_CMD opt=%u", opt);
        if (opt == 0) {
            // sceSifInitCmd: payload carries the EE receive buffer address
            sif::set_ee_pktbuf(mem::iop_rd32(a + 16));
        } else {
            // sceSifInitRpc: acknowledge by setting EE software register 0 (RPC init done)
            u32 payload[2] = {0, 1};
            sif::send_cmd_to_ee(0x80000001, payload, sizeof(payload));
        }
        break;
    }
    case 0x80000003: {  // RESET_CMD (IOP reboot)
        std::string arg = mem::iop_cstr(a + 24, 80);
        log_write(LOG_IOP, "IOP reboot requested: '%s'", arg.c_str());
        sif::clear_smflag(0x10000 | 0x20000 | 0x40000);
        g_reboot_done_at = kernel::emu_time() + 0.01;
        break;
    }
    case 0x80000009: handle_bind(a); break;
    case 0x8000000A: handle_call(a); break;
    case 0x8000000C: handle_rdata(a); break;
    default:
        if (!kern::dispatch_user_cmd(cid, a)) log_write(LOG_SIF, "Unhandled SIF cmd %08x", cid);
        break;
    }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------
void init() {
    kern::init();
    spu2::init();
    modules::register_builtin();
}

void run_pending() {
    spu2::advance_to(kernel::emu_time());  // generate audio up to the current emulated time
    if (g_reboot_done_at && kernel::emu_time() >= g_reboot_done_at) {
        g_reboot_done_at = 0;
        sif::set_smflag(0x10000 | 0x20000 | 0x40000);
        log_write(LOG_IOP, "IOP reboot complete");
    }
    kern::run_ready();
    sif::pump();
}

bool has_ready_work() { return kern::has_ready(); }
void on_vblank_start() {
    kern::vblank(true);
    pad::vblank();
}
void on_vblank_end() { kern::vblank(false); }

void load_module(const std::string& path, const std::string& args) {
    modules::load(path, args);
}

}  // namespace iop

namespace iop {
// The RPC servers come from the IOP modules the game loads at boot; a savestate needs the same set
// in this session (checked first, before anything changes).
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    u32 n = (u32)g_servers_by_sid.size();
    s.v(n);
    if (!s.loading) {
        for (auto& kv : g_servers_by_sid) {
            u32 sid = kv.first, sd = kv.second->sd_addr, buf = kv.second->buf;
            s.v(sid); s.v(sd); s.v(buf);
        }
    } else {
        for (u32 i = 0; i < n && s.ok; i++) {
            u32 sid = 0, sd = 0, buf = 0;
            s.v(sid); s.v(sd); s.v(buf);
            auto it = g_servers_by_sid.find(sid);
            if (it == g_servers_by_sid.end() || it->second->sd_addr != sd || it->second->buf != buf)
                s.fail("das Spiel ist noch nicht fertig gestartet");
        }
        if (!s.ok) return;
    }
    s.v(g_heap_ptr); s.map(g_allocs); s.v(g_next_sd); s.v(g_reboot_done_at);
}
}  // namespace iop
