// IOP kernel HLE: threads (fibers) for recompiled IOP modules, vblank handlers, RPC loop.
#include "iop_kernel.h"
#include "../savestate.h"
#include "../memory.h"
#include "../ee/kernel.h"
#include <windows.h>
#include <algorithm>
#include <deque>
#include <map>
#include <unordered_map>

extern "C" {
uint8_t* g_iop_ram;
volatile int g_iop_irq_pending = 0;
}

namespace iop::kern {

enum { T_DORMANT, T_READY, T_RUN, T_DELAY, T_SLEEP, T_RPC };

struct Thread {
    int id = 0;
    int status = T_DORMANT;
    u32 entry = 0, attr = 0, prio = 0, stacksize = 0, stack = 0, option = 0, arg = 0;
    double wake_time = 0;
    int wakeup_count = 0;
    IOPContext* ctx = nullptr;
    void* fiber = nullptr;
    u32 rpc_qd = 0;
    bool yielded = false;
    double run_start = 0;
    // savestates: where the thread continues after a restore (set when it blocks)
    u32 resume_pc = 0;
    int resume_kind = 0;
    // the RPC request being executed by rpc_loop (completed after a restore when its handler returns)
    bool in_req = false;
    u32 req_sid = 0, req_fno = 0, req_size = 0, req_client = 0, req_rec_id = 0, req_pkt_addr = 0, req_rpc_id = 0;
    u32 req_receive = 0, req_recv_size = 0, req_sp = 0;
};
// resume kinds of IOP threads (RET0: v0 = 0, then resume_pc; RPC: waits in rpc_loop)
enum { IRK_NONE = 0, IRK_RET0, IRK_PLAIN, IRK_RPC };

static std::map<int, Thread*> g_threads;
static int g_next_tid = 1;
static Thread* g_cur = nullptr;
static void* g_caller = nullptr;
static bool g_in_run = false;
static IOPContext* g_irq = nullptr;
static std::unordered_map<u32, IOPFunc> g_funcs;

struct VblankHandler { int edge, prio; u32 func, arg; };
static std::vector<VblankHandler> g_vblank;

struct RpcRequest { RpcServer* s; u32 fno, size, client, rec_id, pkt_addr, rpc_id, receive, recv_size; };
struct RpcQueue { int thread = 0; std::deque<RpcRequest> reqs; std::vector<RpcServer*> servers; };
static std::map<u32, RpcQueue> g_queues;

IOPContext* irq_ctx() { return g_irq; }

static IOPContext* new_ctx() {
    IOPContext* c = (IOPContext*)calloc(1, sizeof(IOPContext));
    return c;
}

void init() {
    g_iop_ram = mem::iop_ram;
    g_irq = new_ctx();
}

void register_resume_table(const IOPFuncEntry* table, unsigned count);
void register_module_funcs(const IOPFuncEntry* table, unsigned count) {
    for (unsigned i = 0; i < count; i++) g_funcs[table[i].start] = table[i].fn;
    register_resume_table(table, count);
}

void call_func(IOPContext* c, u32 addr) {
    auto it = g_funcs.find(addr);
    if (it == g_funcs.end()) fatal("IOP: no function at %08x (ra=%08x)", addr, c->r[31]);
    it->second(c);
}

int current_thread() { return g_cur ? g_cur->id : 0; }

static void switch_out() {
    SwitchToFiber(g_caller);
}

static void WINAPI thread_entry(void* p) {
    Thread* t = (Thread*)p;
    IOPContext* c = t->ctx;
    call_func(c, t->entry);
    LOG(LOG_IOP, "IOP thread %d returned", t->id);
    t->status = T_DORMANT;
    switch_out();
    fatal("dead IOP thread resumed");
}

int create_thread(u32 entry, u32 attr, u32 prio, u32 stacksize, u32 option) {
    Thread* t = new Thread();
    t->id = g_next_tid++;
    t->entry = entry;
    t->attr = attr;
    t->prio = prio;
    t->stacksize = stacksize;
    t->option = option;
    t->stack = iop::alloc(stacksize, 16);
    t->ctx = new_ctx();
    g_threads[t->id] = t;
    LOG(LOG_IOP, "IOP CreateThread %d entry=%08x prio=%u stack=%08x+%x", t->id, entry, prio, t->stack, stacksize);
    return t->id;
}

int start_thread(int id, u32 arg) {
    auto it = g_threads.find(id);
    if (it == g_threads.end()) return -1;
    Thread* t = it->second;
    t->arg = arg;
    IOPContext* c = t->ctx;
    memset(c, 0, sizeof(*c));
    c->r[29] = (t->stack + t->stacksize - 0x20) & ~15u;
    c->r[4] = arg;
    c->r[28] = 0;  // gp set by module wrapper
    c->thread_id = id;
    c->thread = t;
    if (!t->fiber) t->fiber = CreateFiberEx(64 * 1024, 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, thread_entry, t);
    t->status = T_READY;
    LOG(LOG_IOP, "IOP StartThread %d arg=%08x", id, arg);
    return 0;
}

void delay_current(double seconds) {
    Thread* t = g_cur;
    if (!t) return;
    t->resume_pc = t->ctx->r[31];  // called from the module as an import: continue at the return address
    t->resume_kind = IRK_RET0;
    t->status = T_DELAY;
    t->wake_time = kernel::emu_time() + seconds;
    switch_out();
}

void sleep_current() {
    Thread* t = g_cur;
    if (!t) return;
    if (t->wakeup_count > 0) { t->wakeup_count--; return; }
    t->resume_pc = t->ctx->r[31];
    t->resume_kind = IRK_RET0;
    t->status = T_SLEEP;
    switch_out();
}

void wakeup(int id) {
    auto it = g_threads.find(id);
    if (it == g_threads.end()) return;
    Thread* t = it->second;
    if (t->status == T_SLEEP) t->status = T_READY;
    else t->wakeup_count++;
}

static u32 g_yield_target = 0;  // loop check: where a yielding thread continues after a restore
void yield_current() {
    Thread* t = g_cur;
    if (!t) return;
    t->resume_pc = g_yield_target;
    t->resume_kind = g_yield_target ? IRK_PLAIN : IRK_NONE;
    t->status = T_READY;
    t->yielded = true;
    switch_out();
}

static Thread* pick() {
    Thread* best = nullptr;
    for (auto& kv : g_threads) {
        Thread* t = kv.second;
        if (t->status != T_READY || t->yielded) continue;
        if (!best || t->prio < best->prio) best = t;
    }
    return best;
}

bool has_ready() {
    double now = kernel::emu_time();
    for (auto& kv : g_threads) {
        Thread* t = kv.second;
        if (t->status == T_READY) return true;
        if (t->status == T_DELAY && now >= t->wake_time) return true;
    }
    return false;
}

void run_ready() {
    if (g_in_run) return;
    g_in_run = true;
    void* prev_caller = g_caller;
    g_caller = GetCurrentFiber();
    double now = kernel::emu_time();
    for (auto& kv : g_threads) {
        Thread* t = kv.second;
        t->yielded = false;
        if (t->status == T_DELAY && now >= t->wake_time) t->status = T_READY;
    }
    for (int guard = 0; guard < 10000; guard++) {
        Thread* t = pick();
        if (!t) break;
        g_cur = t;
        t->status = T_RUN;
        t->run_start = host_time_sec();
        SwitchToFiber(t->fiber);
        g_cur = nullptr;
    }
    g_caller = prev_caller;
    g_in_run = false;
}

void register_vblank_handler(int edge, int prio, u32 handler, u32 arg) {
    LOG(LOG_IOP, "IOP RegisterVblankHandler edge=%d prio=%d func=%08x arg=%08x", edge, prio, handler, arg);
    g_vblank.push_back(VblankHandler{edge, prio, handler, arg});
}

static bool g_in_irq = false;

void vblank(bool start) {
    if (g_in_irq) return;
    g_in_irq = true;
    int edge = start ? 0 : 1;
    for (auto& h : g_vblank) {
        if (h.edge != edge) continue;
        IOPContext* c = g_irq;
        c->r[4] = h.arg;
        c->r[29] = 0x0001FF00;
        c->r[31] = 0;
        call_func(c, h.func);
    }
    g_in_irq = false;
}

// ---------------------------------------------------------------------------
// RPC
// ---------------------------------------------------------------------------
void rpc_register(u32 sd, u32 sid, u32 func, u32 buf, u32 cfunc, u32 cbuf, u32 qd) {
    RpcServer* s = new RpcServer();
    s->sid = sid;
    s->sd_addr = sd;
    s->buf = buf;
    s->cbuf = cbuf;
    s->iop_func = func;
    s->iop_cfunc = cfunc;
    s->queue_thread = (int)qd;
    // fill a few fields of the IOP-side server struct for code that inspects it
    mem::iop_wr32(sd + 0, sid);
    mem::iop_wr32(sd + 4, func);
    mem::iop_wr32(sd + 8, buf);
    iop::register_server(s);
    g_queues[qd].servers.push_back(s);
    log_write(LOG_IOP, "IOP sceSifRegisterRpc sid=%08x func=%08x buf=%08x qd=%08x", sid, func, buf, qd);
}

void rpc_set_queue(u32 qd, int thid) { g_queues[qd].thread = thid; }

void queue_rpc_request(RpcServer* s, u32 fno, u32 send_size, u32 client, u32 rec_id, u32 pkt_addr, u32 rpc_id,
                       u32 receive, u32 recv_size) {
    u32 qd = (u32)s->queue_thread;
    RpcQueue& q = g_queues[qd];
    q.reqs.push_back(RpcRequest{s, fno, send_size, client, rec_id, pkt_addr, rpc_id, receive, recv_size});
    auto it = g_threads.find(q.thread);
    if (it != g_threads.end() && it->second->status == T_RPC) it->second->status = T_READY;
    run_ready();
}

void rpc_loop(IOPContext* c, u32 qd) {
    RpcQueue& q = g_queues[qd];
    q.thread = g_cur ? g_cur->id : 0;
    for (;;) {
        while (q.reqs.empty()) {
            g_cur->status = T_RPC;
            g_cur->resume_kind = IRK_RPC;
            switch_out();
        }
        RpcRequest r = q.reqs.front();
        q.reqs.pop_front();
        u32 sp = c->r[29];
        c->r[4] = r.fno;
        c->r[5] = r.s->buf;
        c->r[6] = r.size;
        c->r[31] = 0;
        Thread* self = g_cur;
        self->in_req = true;
        self->req_sid = r.s->sid; self->req_fno = r.fno; self->req_size = r.size; self->req_client = r.client;
        self->req_rec_id = r.rec_id; self->req_pkt_addr = r.pkt_addr; self->req_rpc_id = r.rpc_id;
        self->req_receive = r.receive; self->req_recv_size = r.recv_size; self->req_sp = sp;
        call_func(c, r.s->iop_func);
        self->in_req = false;
        u32 rdata = c->r[2];
        c->r[29] = sp;
        rpc_complete(r.client, r.rec_id, r.pkt_addr, r.rpc_id, rdata, r.receive, r.recv_size);
    }
}

bool dispatch_user_cmd(u32 cid, u32 pkt_addr) {
    (void)cid;
    (void)pkt_addr;
    return false;
}

// ---------------------------------------------------------------------------
// Savestates. A restored IOP thread continues at the MIPS level like an EE thread (kernel.cpp):
// threads waiting in the RPC loop re-enter it; threads blocked in an import (DelayThread) or at a
// loop check continue in the recompiled module bodies at their resume point. When the outermost
// body returns (ra 0), a request that was being executed is completed and the RPC loop continues.
// ---------------------------------------------------------------------------
static std::vector<const IOPFuncEntry*> g_sorted;  // all module functions by start address
void register_resume_table(const IOPFuncEntry* table, unsigned count) {
    for (unsigned i = 0; i < count; i++) g_sorted.push_back(&table[i]);
    std::sort(g_sorted.begin(), g_sorted.end(), [](const IOPFuncEntry* a, const IOPFuncEntry* b) { return a->start < b->start; });
}

static u32 queue_of(int id) {
    for (auto& kv : g_queues)
        if (kv.second.thread == id) return kv.first;
    return 0;
}

bool can_savestate(std::string* why) {
    if (g_in_run || g_in_irq || g_cur) { if (why) *why = "IOP busy"; return false; }
    for (auto& kv : g_threads) {
        Thread* t = kv.second;
        if (t->status == T_DORMANT) continue;
        bool ok = t->resume_kind != IRK_NONE && t->status != T_RUN;
        if (t->resume_kind == IRK_RPC) ok = ok && t->status == T_RPC && queue_of(t->id) && g_queues[queue_of(t->id)].reqs.empty();
        if (!ok) {
            if (why) *why = "IOP thread " + std::to_string(t->id) + " busy (status " + std::to_string(t->status) + ")";
            return false;
        }
    }
    return true;
}

void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    s.v(g_next_tid);
    u32 n = (u32)g_threads.size();
    s.v(n);
    std::vector<int> ids;
    for (auto& kv : g_threads) ids.push_back(kv.first);
    if (s.loading) ids.assign(n, 0);
    for (u32 i = 0; i < n && s.ok; i++) {
        int id = ids[i];
        s.v(id);
        Thread* t;
        if (s.loading) {
            auto it = g_threads.find(id);
            if (it == g_threads.end()) {
                t = new Thread();
                t->id = id;
                t->ctx = new_ctx();
                g_threads[id] = t;
            } else {
                t = it->second;
            }
        } else {
            t = g_threads[id];
        }
        s.v(t->status); s.v(t->entry); s.v(t->attr); s.v(t->prio); s.v(t->stacksize); s.v(t->stack); s.v(t->option);
        s.v(t->arg); s.v(t->wake_time); s.v(t->wakeup_count); s.v(t->rpc_qd); s.v(t->yielded);
        s.v(t->resume_pc); s.v(t->resume_kind); s.v(t->in_req);
        s.v(t->req_sid); s.v(t->req_fno); s.v(t->req_size); s.v(t->req_client); s.v(t->req_rec_id); s.v(t->req_pkt_addr);
        s.v(t->req_rpc_id); s.v(t->req_receive); s.v(t->req_recv_size); s.v(t->req_sp);
        s.pod(*t->ctx, "IOPContext");
        if (s.loading) t->ctx->thread = t;
    }
    s.vec(g_vblank);
    // which thread serves which RPC queue
    u32 nq = (u32)g_queues.size();
    s.v(nq);
    std::vector<std::pair<u32, int>> q;
    for (auto& kv : g_queues) q.push_back({kv.first, kv.second.thread});
    if (s.loading) q.assign(nq, {0, 0});
    for (auto& e : q) { s.v(e.first); s.v(e.second); }
    if (s.loading)
        for (auto& e : q) {
            g_queues[e.first].thread = e.second;
            g_queues[e.first].reqs.clear();
        }
}

}  // namespace iop::kern
extern "C" uint32_t g_iop_resume = 0;
extern "C" void iop_resume_fail(IOPContext* c, uint32_t func) {
    fatal("Savestate: no IOP resume point in function %08x (ra %08x)", func, c->r[31]);
}
namespace iop::kern {

static void resume_chain(IOPContext* c, u32 pc) {
    for (;;) {
        if (pc == 0) return;
        auto it = std::upper_bound(g_sorted.begin(), g_sorted.end(), pc, [](u32 v, const IOPFuncEntry* e) { return v < e->start; });
        if (it == g_sorted.begin()) fatal("Savestate: no IOP code at %08x", pc);
        const IOPFuncEntry* e = *(it - 1);
        if (e->start == pc) {
            e->fn(c);
        } else {
            if (pc > e->end) fatal("Savestate: no IOP code at %08x", pc);
            g_iop_resume = pc;
            e->fn(c);
        }
        pc = c->r[31];
    }
}

static void WINAPI resume_entry(void* p) {
    Thread* t = (Thread*)p;
    IOPContext* c = t->ctx;
    int kind = t->resume_kind;
    t->resume_kind = IRK_NONE;
    if (kind != IRK_RPC) {
        if (kind == IRK_RET0) c->r[2] = 0;
        resume_chain(c, t->resume_pc);
        if (!t->in_req) {  // the thread function returned
            t->status = T_DORMANT;
            switch_out();
            fatal("dead IOP thread resumed");
        }
        // the request handler returned: complete the request as rpc_loop would have
        t->in_req = false;
        u32 rdata = c->r[2];
        c->r[29] = t->req_sp;
        rpc_complete(t->req_client, t->req_rec_id, t->req_pkt_addr, t->req_rpc_id, rdata, t->req_receive, t->req_recv_size);
    }
    rpc_loop(c, queue_of(t->id));
    fatal("IOP RPC loop returned");
}

void after_state_load() {
    for (auto& kv : g_threads) {
        Thread* t = kv.second;
        if (t->fiber) { DeleteFiber(t->fiber); t->fiber = nullptr; }
        if (t->status == T_DORMANT) continue;
        if (t->resume_kind == IRK_NONE) fatal("IOP thread %d not restorable (status %d)", t->id, t->status);
        t->fiber = CreateFiberEx(64 * 1024, 1024 * 1024, FIBER_FLAG_FLOAT_SWITCH, resume_entry, t);
        if (t->status == T_RUN) t->status = T_READY;
    }
    g_cur = nullptr;
    g_in_run = false;
}

}  // namespace iop::kern

using namespace iop::kern;

extern "C" {

void iop_call(IOPContext* c, uint32_t addr) { call_func(c, addr); }
void iop_syscall(IOPContext* c, uint32_t pc) { log_write(LOG_IOP, "IOP syscall at %08x (v0=%08x)", pc, c->r[2]); }
void iop_break(IOPContext* c, uint32_t pc) { log_write(LOG_IOP, "IOP break at %08x ra=%08x", pc, c->r[31]); }
void iop_invalid(IOPContext* c, uint32_t pc, uint32_t word) {
    fatal("IOP invalid instruction %08x at %08x (ra=%08x)", word, pc, c->r[31]);
}
uint32_t iop_mfc0(IOPContext* c, int reg) { return c->cop0[reg]; }
void iop_mtc0(IOPContext* c, int reg, uint32_t v) { c->cop0[reg] = v; }

void iop_loop_check_at(IOPContext* c, uint32_t target) {
    g_yield_target = target;
    iop_loop_check(c);
    g_yield_target = 0;
}

void iop_loop_check(IOPContext* c) {
    (void)c;
    g_iop_irq_pending = 0;
    // a thread spinning for a long time yields to let the rest of the system progress
    if (g_cur && host_time_sec() - g_cur->run_start > 0.002) yield_current();
}

uint32_t iop_io_rd(uint32_t a, int size) {
    LOG(LOG_IOP, "IOP io read%d %08x", size * 8, a);
    return 0;
}
void iop_io_wr(uint32_t a, uint32_t v, int size) {
    LOG(LOG_IOP, "IOP io write%d %08x = %08x", size * 8, a, v);
}

}
