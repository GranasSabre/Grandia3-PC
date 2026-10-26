// Disc image access and HLE of the cdvdfsv RPC servers used by libcdvd.
#include "cdvd.h"
#include "iop.h"
#include "modules.h"
#include "../memory.h"
#include "../hw/sif.h"
#include <windows.h>
#include <algorithm>
#include <map>
#include <atomic>
#include <mutex>

namespace cdvd {

static HANDLE g_iso = INVALID_HANDLE_VALUE;
static u64 g_iso_size = 0;
static std::mutex g_iso_mutex;

struct Entry {
    u32 lsn, size;
    bool dir;
};
static std::map<std::string, Entry> g_files;  // upper-case path without version, e.g. "\\DATA\\FOO.DAT"

static std::string norm(std::string p) {
    std::replace(p.begin(), p.end(), '/', '\\');
    auto sc = p.find(';');
    if (sc != std::string::npos) p = p.substr(0, sc);
    std::transform(p.begin(), p.end(), p.begin(), ::toupper);
    if (p.empty() || p[0] != '\\') p = "\\" + p;
    return p;
}

static void scan_dir(u32 lsn, u32 size, const std::string& prefix, int depth) {
    if (depth > 8) return;
    std::vector<u8> d((size + 2047) & ~2047u);
    if (!read_sectors(lsn, (u32)d.size() / 2048, d.data())) return;
    u32 off = 0;
    while (off < size) {
        u8 len = d[off];
        if (len == 0) {
            off = (off / 2048 + 1) * 2048;
            continue;
        }
        u32 elsn, esize;
        memcpy(&elsn, &d[off + 2], 4);
        memcpy(&esize, &d[off + 10], 4);
        u8 flags = d[off + 25];
        u8 nlen = d[off + 32];
        std::string name((char*)&d[off + 33], nlen);
        off += len;
        if (nlen == 1 && (name[0] == 0 || name[0] == 1)) continue;
        std::string full = norm(prefix + "\\" + name);
        g_files[full] = Entry{elsn, esize, (flags & 2) != 0};
        if (flags & 2) scan_dir(elsn, esize, full, depth + 1);
    }
}

// Data directory mode (tools/make_data_dir.py): the disc's files unpacked into a folder plus disc.idx
// with their sector positions and the sectors outside files. Sector reads are served from those.
struct DataFile {
    u32 lsn, size;
    std::string path;
    HANDLE h = INVALID_HANDLE_VALUE;
};
static bool g_dmode = false;
static std::string g_ddir;
static std::vector<DataFile> g_dfiles;      // sorted by lsn
static std::vector<u32> g_dsys_lsn;         // sorted
static std::vector<u8> g_dsys;              // their contents, 2048 bytes each

static bool scan_volume(const std::string& what) {
    u8 pvd[2048];
    if (!read_sectors(16, 1, pvd) || memcmp(pvd + 1, "CD001", 5) != 0) return false;
    u32 rlsn, rsize;
    memcpy(&rlsn, pvd + 156 + 2, 4);
    memcpy(&rsize, pvd + 156 + 10, 4);
    scan_dir(rlsn, rsize, "", 0);
    log_write(LOG_CDVD, "Disc opened: %s (%llu MB, %zu files)", what.c_str(), (unsigned long long)(g_iso_size >> 20), g_files.size());
    return true;
}

bool open_image(const std::string& path) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    g_iso = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (g_iso == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(g_iso, &sz);
    g_iso_size = sz.QuadPart;
    return scan_volume(path);
}

// a wrong image was chosen: forget it
void close_image() {
    if (g_iso != INVALID_HANDLE_VALUE) CloseHandle(g_iso);
    g_iso = INVALID_HANDLE_VALUE;
    g_iso_size = 0;
    g_files.clear();
}

bool open_data_dir(const std::string& dir, const std::string& index) {
    FILE* f = fopen((dir + "\\" + index).c_str(), "rb");
    if (!f) return false;
    char magic[8];
    u32 hdr[3];
    bool ok = fread(magic, 1, 8, f) == 8 && memcmp(magic, "G3DISC1", 8) == 0 && fread(hdr, 4, 3, f) == 3;
    for (u32 i = 0; ok && i < hdr[1]; i++) {
        u32 ls[2];
        u16 n;
        DataFile d;
        ok = fread(ls, 4, 2, f) == 2 && fread(&n, 2, 1, f) == 1;
        if (!ok) break;
        d.lsn = ls[0];
        d.size = ls[1];
        d.path.resize(n);
        ok = fread(&d.path[0], 1, n, f) == n;
        std::replace(d.path.begin(), d.path.end(), '/', '\\');
        g_dfiles.push_back(std::move(d));
    }
    if (ok) {
        g_dsys_lsn.resize(hdr[2]);
        g_dsys.resize((size_t)hdr[2] * 2048);
        ok = fread(g_dsys_lsn.data(), 4, hdr[2], f) == hdr[2] && fread(g_dsys.data(), 1, g_dsys.size(), f) == g_dsys.size();
    }
    fclose(f);
    if (!ok) {
        g_dfiles.clear();
        return false;
    }
    std::sort(g_dfiles.begin(), g_dfiles.end(), [](const DataFile& a, const DataFile& b) { return a.lsn < b.lsn; });
    g_ddir = dir;
    g_dmode = true;
    g_iso_size = (u64)hdr[0] * 2048;
    return scan_volume(dir + "\\" + index);
}

// one run of sectors from the data directory; returns the number of sectors handled
static u32 read_data_run(u32 lsn, u32 count, u8* dst) {
    auto it = std::upper_bound(g_dfiles.begin(), g_dfiles.end(), lsn, [](u32 l, const DataFile& d) { return l < d.lsn; });
    if (it != g_dfiles.begin()) {
        DataFile& d = *(it - 1);
        u32 nsec = (d.size + 2047) / 2048;
        if (lsn < d.lsn + nsec) {
            u32 take = std::min(count, d.lsn + nsec - lsn);
            if (d.h == INVALID_HANDLE_VALUE) {
                // a file of the same name in override\ (next to data\) replaces the disc file
                std::string rel = d.path;
                if (rel.rfind("disc2\\", 0) == 0) rel = rel.substr(6);
                std::string ov = g_ddir + "\\..\\override\\" + rel;
                d.h = CreateFileA(ov.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
                if (d.h != INVALID_HANDLE_VALUE) log_write(LOG_CDVD, "override: %s", rel.c_str());
                else
                    d.h = CreateFileA((g_ddir + "\\" + d.path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
            }
            u64 off = (u64)(lsn - d.lsn) * 2048;
            // up to the end of the last sector: an override file may be a little larger
            u64 len = std::min<u64>((u64)take * 2048, (u64)nsec * 2048 - off);
            memset(dst, 0, (size_t)take * 2048);
            if (d.h != INVALID_HANDLE_VALUE && len) {
                OVERLAPPED ov{};
                ov.Offset = (DWORD)off;
                ov.OffsetHigh = (DWORD)(off >> 32);
                DWORD got = 0;
                ReadFile(d.h, dst, (DWORD)len, &got, &ov);
            }
            return take;
        }
    }
    auto s = std::lower_bound(g_dsys_lsn.begin(), g_dsys_lsn.end(), lsn);
    if (s != g_dsys_lsn.end() && *s == lsn) memcpy(dst, &g_dsys[(size_t)(s - g_dsys_lsn.begin()) * 2048], 2048);
    else memset(dst, 0, 2048);
    return 1;
}

u32 total_sectors() { return (u32)(g_iso_size / 2048); }

static std::atomic<u64> g_sectors_read{0};
u64 sectors_read() { return g_sectors_read.load(std::memory_order_relaxed); }

bool read_sectors(u32 lsn, u32 count, void* dst) {
    g_sectors_read += count;
    std::lock_guard<std::mutex> lk(g_iso_mutex);
    if (g_dmode) {
        u8* p = (u8*)dst;
        while (count) {
            u32 n = read_data_run(lsn, count, p);
            lsn += n;
            count -= n;
            p += (size_t)n * 2048;
        }
        return true;
    }
    u64 off = (u64)lsn * 2048;
    u64 len = (u64)count * 2048;
    if (off >= g_iso_size) {
        memset(dst, 0, len);
        return false;
    }
    if (off + len > g_iso_size) {
        memset(dst, 0, len);
        len = g_iso_size - off;
    }
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    if (!ReadFile(g_iso, dst, (DWORD)len, &got, &ov)) return false;
    return got == len;
}

std::string file_at(u32 lsn, u32* file_lsn) {
    for (const auto& [name, e] : g_files)
        if (!e.dir && lsn >= e.lsn && lsn < e.lsn + (e.size + 2047) / 2048) {
            *file_lsn = e.lsn;
            return name;
        }
    return "";
}

bool find_file(const std::string& path, u32* lsn, u32* size) {
    auto it = g_files.find(norm(path));
    if (it == g_files.end()) return false;
    *lsn = it->second.lsn;
    *size = it->second.size;
    return true;
}

// ---------------------------------------------------------------------------
// RPC servers
// ---------------------------------------------------------------------------
static void dump(const char* what, u32 fno, u32 buf, u32 size) {
    char t[96];
    snprintf(t, sizeof(t), "%s fno=%u", what, fno);
    iop::modules::hexdump(LOG_CDVD, t, mem::iop_ptr(buf), std::min<u32>(size, 64));
}

void register_servers() {
    using namespace iop;
    // sceCdInit
    register_native_server(0x80000592, 0x40, [](u32 fno, u32 buf, u32 size) -> u32 {
        dump("cdInit", fno, buf, size);
        u32* b = (u32*)mem::iop_ptr(buf);
        b[0] = 1;
        return buf;
    });
    // S-commands
    register_native_server(0x80000593, 0x400, [](u32 fno, u32 buf, u32 size) -> u32 {
        dump("cdSCmd", fno, buf, size);
        u32* b = (u32*)mem::iop_ptr(buf);
        u8* b8 = (u8*)b;
        switch (fno) {
        case 0x01: {  // ReadClock -> result + sceCdCLOCK
            SYSTEMTIME st;
            GetLocalTime(&st);
            auto bcd = [](int v) { return (u8)(((v / 10) << 4) | (v % 10)); };
            memset(b8, 0, 16);
            b[0] = 1;
            b8[4] = 0;
            b8[5] = bcd(st.wSecond);
            b8[6] = bcd(st.wMinute);
            b8[7] = bcd(st.wHour);
            b8[8] = 0;
            b8[9] = bcd(st.wDay);
            b8[10] = bcd(st.wMonth);
            b8[11] = bcd(st.wYear % 100);
            break;
        }
        case 0x03:  // GetDiskType
            b[0] = 0x14;  // SCECdPS2DVD
            break;
        case 0x04:  // GetError
            b[0] = 0;
            break;
        case 0x05:  // TrayReq
            b[0] = 1;
            b[1] = 0;
            break;
        default:
            memset(b, 0, 16);
            b[0] = 1;
            break;
        }
        return buf;
    });
    // N-commands
    register_native_server(0x80000595, 0x400, [](u32 fno, u32 buf, u32 size) -> u32 {
        u32* b = (u32*)mem::iop_ptr(buf);
        switch (fno) {
        case 0x01:    // Read
        case 0x03: {  // DvdRead
            u32 lsn = b[0], n = b[1], dst = b[2];
            LOG(LOG_CDVD, "cdRead lsn=%u n=%u -> %08x", lsn, n, dst);
            std::vector<u8> tmp((size_t)n * 2048);
            read_sectors(lsn, n, tmp.data());
            sif::ee_write(dst & 0x1FFFFFFF, tmp.data(), (u32)tmp.size());
            b[0] = 1;
            break;
        }
        case 0x0D: {  // ReadIOPMem
            u32 lsn = b[0], n = b[1], dst = b[2];
            LOG(LOG_CDVD, "cdReadIOPm lsn=%u n=%u -> iop %08x", lsn, n, dst);
            read_sectors(lsn, n, mem::iop_ptr(dst));
            b[0] = 1;
            break;
        }
        default:
            dump("cdNCmd", fno, buf, size);
            b[0] = 1;
            break;
        }
        return buf;
    });
    // search file
    register_native_server(0x80000597, 0x200, [](u32 fno, u32 buf, u32 size) -> u32 {
        dump("cdSearchFile", fno, buf, size);
        u8* b = mem::iop_ptr(buf);
        // layout: sceCdlFILE (32 bytes), name[256], EE dest ptr
        std::string name((char*)b + 32, strnlen((char*)b + 32, 256));
        u32 dest;
        memcpy(&dest, b + 32 + 256, 4);
        u32 lsn = 0, fsize = 0;
        bool ok = find_file(name, &lsn, &fsize);
        log_write(LOG_CDVD, "sceCdSearchFile('%s') -> %s lsn=%u size=%u (dest %08x)", name.c_str(), ok ? "found" : "NOT FOUND", lsn, fsize, dest);
        u8 info[32] = {};
        memcpy(info, &lsn, 4);
        memcpy(info + 4, &fsize, 4);
        std::string base = name.substr(name.find_last_of('\\') + 1);
        memcpy(info + 8, base.c_str(), std::min<size_t>(base.size(), 15));
        if (ok && dest) sif::ee_write(dest & 0x1FFFFFFF, info, 32);
        u32 r = ok ? 1 : 0;
        memcpy(b, &r, 4);
        return buf;
    });
    // disk ready
    register_native_server(0x8000059A, 0x40, [](u32 fno, u32 buf, u32 size) -> u32 {
        (void)fno;
        (void)size;
        u32* b = (u32*)mem::iop_ptr(buf);
        b[0] = 6;  // SCECdComplete
        return buf;
    });
    for (u32 sid : {0x80000594u, 0x80000596u, 0x80000598u, 0x80000599u, 0x8000059Bu, 0x8000059Cu, 0x8000059Du}) {
        register_native_server(sid, 0x200, [sid](u32 fno, u32 buf, u32 size) -> u32 {
            char t[64];
            snprintf(t, sizeof(t), "cd server %08x", sid);
            dump(t, fno, buf, size);
            u32* b = (u32*)mem::iop_ptr(buf);
            b[0] = 1;
            return buf;
        });
    }
}

}  // namespace cdvd
