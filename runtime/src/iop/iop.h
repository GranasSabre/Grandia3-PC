#pragma once
#include "../common.h"
#include <functional>

namespace iop {

void init();
void run_pending();        // run ready IOP threads / deferred work until idle
bool has_ready_work();
void on_vblank_start();
void on_vblank_end();

// SIF command arrived from EE at IOP address
void sifcmd_received(u32 iop_addr);

// IOP memory heap (HLE AllocSysMemory)
u32 alloc(u32 size, u32 align = 64);
void free(u32 addr);

// RPC servers
typedef std::function<u32(u32 fno, u32 buf, u32 size)> RpcFunc;   // returns IOP addr of result data
struct RpcServer {
    u32 sid = 0;
    u32 sd_addr = 0;     // IOP address used as server handle
    u32 buf = 0, cbuf = 0;
    RpcFunc native;      // native HLE server
    // recompiled module server
    u32 iop_func = 0, iop_cfunc = 0;
    int queue_thread = -1;
};
RpcServer* register_native_server(u32 sid, u32 bufsize, RpcFunc fn);
RpcServer* register_server(RpcServer* s);

// send an RPC_END for a completed CALL
void rpc_complete(u32 client, u32 rec_id, u32 pkt_addr, u32 rpc_id, u32 rdata_iop, u32 recv_ee, u32 recv_size);

void load_module(const std::string& path, const std::string& args);

}  // namespace iop
