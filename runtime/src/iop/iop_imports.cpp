// HLE implementations of IOP library imports used by recompiled IRX modules.
#include "iop_kernel.h"
#include "../savestate.h"
#include "cdvd.h"
#include "../memory.h"
#include "../hw/sif.h"
#include "../ee/kernel.h"
#include "../platform/subtitles.h"
#include <string>

using namespace iop;

static inline u32 A(IOPContext* c, int i) {
    if (i < 4) return c->r[4 + i];
    return mem::iop_rd32(c->r[29] + 16 + (i - 4) * 4);
}
static inline void RET(IOPContext* c, u32 v) { c->r[2] = v; }

// printf-style formatting with IOP varargs
static std::string iop_format(IOPContext* c, u32 fmt_addr, int first_arg) {
    std::string fmt = mem::iop_cstr(fmt_addr, 512);
    std::string out;
    int ai = first_arg;
    for (size_t i = 0; i < fmt.size(); i++) {
        char ch = fmt[i];
        if (ch != '%') { out.push_back(ch); continue; }
        std::string spec = "%";
        i++;
        while (i < fmt.size() && strchr("-+ #0123456789.lh", fmt[i])) {
            if (fmt[i] != 'l' && fmt[i] != 'h') spec.push_back(fmt[i]);
            i++;
        }
        if (i >= fmt.size()) break;
        char conv = fmt[i];
        char buf[256];
        switch (conv) {
        case '%': out.push_back('%'); continue;
        case 's': {
            std::string s = mem::iop_cstr(A(c, ai++), 256);
            spec += 's';
            snprintf(buf, sizeof(buf), spec.c_str(), s.c_str());
            break;
        }
        case 'c':
            spec += 'c';
            snprintf(buf, sizeof(buf), spec.c_str(), (int)A(c, ai++));
            break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            spec += conv;
            snprintf(buf, sizeof(buf), spec.c_str(), A(c, ai++));
            break;
        case 'p':
            snprintf(buf, sizeof(buf), "%08x", A(c, ai++));
            break;
        default:
            buf[0] = conv;
            buf[1] = 0;
            break;
        }
        out += buf;
    }
    return out;
}

extern "C" {

// ---------------- loadcore ----------------
void iop_imp_loadcore_5(IOPContext* c) { RET(c, 0); }  // FlushDcache

// ---------------- sysmem ----------------
void iop_imp_sysmem_4(IOPContext* c) {  // AllocSysMemory(mode, size, addr)
    u32 size = A(c, 1);
    u32 a = iop::alloc(size, 256);
    LOG(LOG_IOP, "IOP AllocSysMemory(%u, %x) -> %08x", A(c, 0), size, a);
    RET(c, a);
}

// ---------------- intrman ----------------
void iop_imp_intrman_6(IOPContext* c) { RET(c, 0); }  // EnableIntr

// ---------------- stdio / sysclib ----------------
void iop_imp_stdio_4(IOPContext* c) {
    std::string s = iop_format(c, A(c, 0), 1);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    log_write(LOG_IOP, "[iop printf] %s", s.c_str());
    RET(c, (u32)s.size());
}
void iop_imp_sysclib_19(IOPContext* c) {  // sprintf
    std::string s = iop_format(c, A(c, 1), 2);
    memcpy(mem::iop_ptr(A(c, 0)), s.c_str(), s.size() + 1);
    RET(c, (u32)s.size());
}
void iop_imp_sysclib_23(IOPContext* c) {  // strcpy
    std::string s = mem::iop_cstr(A(c, 1), 4096);
    memcpy(mem::iop_ptr(A(c, 0)), s.c_str(), s.size() + 1);
    RET(c, A(c, 0));
}

// ---------------- thbase ----------------
void iop_imp_thbase_4(IOPContext* c) {  // CreateThread(param)
    u32 p = A(c, 0);
    u32 attr = mem::iop_rd32(p + 0), option = mem::iop_rd32(p + 4), entry = mem::iop_rd32(p + 8);
    u32 stacksize = mem::iop_rd32(p + 12), prio = mem::iop_rd32(p + 16);
    RET(c, (u32)kern::create_thread(entry, attr, prio, stacksize, option));
}
void iop_imp_thbase_6(IOPContext* c) { RET(c, (u32)kern::start_thread((int)A(c, 0), A(c, 1))); }
void iop_imp_thbase_20(IOPContext* c) { RET(c, (u32)kern::current_thread()); }
void iop_imp_thbase_33(IOPContext* c) {  // DelayThread(usec)
    kern::delay_current(A(c, 0) / 1e6);
    RET(c, 0);
}

// ---------------- vblank ----------------
void iop_imp_vblank_8(IOPContext* c) {  // RegisterVblankHandler(edge, prio, handler, arg)
    kern::register_vblank_handler((int)A(c, 0), (int)A(c, 1), A(c, 2), A(c, 3));
    RET(c, 0);
}

// ---------------- sifcmd ----------------
void iop_imp_sifcmd_14(IOPContext* c) { RET(c, 0); }  // sceSifInitRpc
void iop_imp_sifcmd_17(IOPContext* c) {               // sceSifRegisterRpc(sd, sid, func, buf, cfunc, cbuf, qd)
    kern::rpc_register(A(c, 0), A(c, 1), A(c, 2), A(c, 3), A(c, 4), A(c, 5), A(c, 6));
    RET(c, 0);
}
void iop_imp_sifcmd_19(IOPContext* c) {  // sceSifSetRpcQueue(qd, thid)
    kern::rpc_set_queue(A(c, 0), (int)A(c, 1));
    RET(c, 0);
}
void iop_imp_sifcmd_22(IOPContext* c) {  // sceSifRpcLoop(qd)
    kern::rpc_loop(c, A(c, 0));
}

// ---------------- sifman ----------------
static int g_sif_dma_id = 1;
void iop_imp_sifman_7(IOPContext* c) {  // sceSifSetDma(transfers, count)
    u32 t = A(c, 0);
    int n = (int)A(c, 1);
    for (int i = 0; i < n; i++) {
        u32 src = mem::iop_rd32(t + i * 16), dst = mem::iop_rd32(t + i * 16 + 4);
        u32 size = mem::iop_rd32(t + i * 16 + 8);
        LOG(LOG_SIF, "IOP->EE dma src=%08x dst=%08x size=%x", src, dst, size);
        sif::ee_write(dst & 0x1FFFFFFF, mem::iop_ptr(src), (size + 3) & ~3u);
    }
    RET(c, (u32)g_sif_dma_id++);
}
void iop_imp_sifman_8(IOPContext* c) { RET(c, (u32)-1); }  // sceSifDmaStat: done

// ---------------- cdvdman ----------------
static int g_cd_error = 0;
void iop_imp_cdvdman_5(IOPContext* c) { RET(c, 1); }  // Standby
void iop_imp_cdvdman_6(IOPContext* c) {               // Read(lsn, sectors, buf, mode)
    u32 lsn = A(c, 0), n = A(c, 1), buf = A(c, 2);
    LOG(LOG_CDVD, "IOP sceCdRead lsn=%u n=%u -> %08x", lsn, n, buf);
    if (n) ::cdvd::read_sectors(lsn, n, mem::iop_ptr(buf));
    if (n) subs::on_cd_read(lsn, n, buf);
    RET(c, 1);
}
void iop_imp_cdvdman_7(IOPContext* c) { RET(c, 1); }   // Seek
void iop_imp_cdvdman_8(IOPContext* c) { RET(c, (u32)g_cd_error); }  // GetError
void iop_imp_cdvdman_11(IOPContext* c) { RET(c, 0); }  // Sync: complete
void iop_imp_cdvdman_12(IOPContext* c) { RET(c, 0x14); }  // GetDiskType: PS2 DVD
void iop_imp_cdvdman_13(IOPContext* c) { RET(c, 2); }  // DiskReady: SCECdComplete(2)
void iop_imp_cdvdman_24(IOPContext* c) { RET(c, 1); }
void iop_imp_cdvdman_28(IOPContext* c) { RET(c, 0x0A); }  // Status: spinning/paused
void iop_imp_cdvdman_39(IOPContext* c) { RET(c, 1); }  // Break

// ---------------- ioman ----------------
struct IopFile { u32 lsn, size, pos; bool used; };
static IopFile g_files[32];
void iop_imp_ioman_4(IOPContext* c) {  // open(name, flags)
    std::string name = mem::iop_cstr(A(c, 0));
    LOG(LOG_IOP, "IOP open('%s', %x)", name.c_str(), A(c, 1));
    if (name.rfind("cdrom0:", 0) == 0) {
        u32 lsn, size;
        if (::cdvd::find_file(name.substr(7), &lsn, &size)) {
            for (int i = 1; i < 32; i++)
                if (!g_files[i].used) { g_files[i] = IopFile{lsn, size, 0, true}; RET(c, i); return; }
        }
    }
    RET(c, (u32)-1);
}
void iop_imp_ioman_5(IOPContext* c) {
    u32 fd = A(c, 0);
    if (fd < 32) g_files[fd].used = false;
    RET(c, 0);
}
void iop_imp_ioman_6(IOPContext* c) {  // read(fd, buf, n)
    u32 fd = A(c, 0), buf = A(c, 1), n = A(c, 2);
    if (fd >= 32 || !g_files[fd].used) { RET(c, (u32)-1); return; }
    IopFile& f = g_files[fd];
    if (f.pos + n > f.size) n = f.size - f.pos;
    std::vector<u8> tmp(((f.pos % 2048) + n + 2047) / 2048 * 2048);
    ::cdvd::read_sectors(f.lsn + f.pos / 2048, (u32)tmp.size() / 2048, tmp.data());
    memcpy(mem::iop_ptr(buf), tmp.data() + f.pos % 2048, n);
    f.pos += n;
    RET(c, n);
}
void iop_imp_ioman_7(IOPContext* c) { RET(c, (u32)-1); }  // write
void iop_imp_ioman_8(IOPContext* c) {                     // lseek(fd, off, whence)
    u32 fd = A(c, 0);
    s32 off = (s32)A(c, 1);
    u32 wh = A(c, 2);
    if (fd >= 32 || !g_files[fd].used) { RET(c, (u32)-1); return; }
    IopFile& f = g_files[fd];
    if (wh == 0) f.pos = off;
    else if (wh == 1) f.pos += off;
    else f.pos = f.size + off;
    RET(c, f.pos);
}
void iop_imp_ioman_10(IOPContext* c) { RET(c, (u32)-1); }
void iop_imp_ioman_13(IOPContext* c) { RET(c, (u32)-1); }
void iop_imp_ioman_15(IOPContext* c) { RET(c, (u32)-1); }
void iop_imp_ioman_18(IOPContext* c) { RET(c, (u32)-1); }
void iop_imp_ioman_27(IOPContext* c) { RET(c, (u32)-1); }
void iop_imp_ioman_28(IOPContext* c) { RET(c, (u32)-1); }
void iop_imp_ioman_29(IOPContext* c) { RET(c, (u32)-1); }
void iop_imp_ioman_31(IOPContext* c) { RET(c, (u32)-1); }

}

void ioman_state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    for (auto& f : g_files) s.pod(f, "IopFile");
    s.v(g_sif_dma_id); s.v(g_cd_error);
}
