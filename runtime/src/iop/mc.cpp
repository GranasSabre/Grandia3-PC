// HLE of mcserv (libmc 2.5 protocol) backed by a host directory ("folder memory card").
#include "mc.h"
#include "../savestate.h"
#include "iop.h"
#include "modules.h"
#include "../memory.h"
#include "../hw/sif.h"
#include "../platform/platform.h"
#include <windows.h>
#include <algorithm>
#include <map>

namespace iop::mc {

static std::string g_root[2];
static std::string g_cwd[2] = {"/", "/"};
static bool g_reported[2] = {false, false};

struct McFile {
    bool used = false;
    int port = 0;
    std::string host;
    FILE* f = nullptr;
};
static McFile g_fd[32];

enum { RES_OK = 0, RES_CHANGED = -1, RES_NOFORMAT = -2, RES_FULL = -3, RES_NOENTRY = -4, RES_DENIED = -5,
       RES_NOTEMPTY = -6, RES_UPLIMIT = -7, RES_NOCARD = -10 };

static std::string exe_dir() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p);
    return s.substr(0, s.find_last_of("\\/"));
}

static void ensure_dir(const std::string& p) {
    std::string acc;
    for (size_t i = 0; i < p.size(); i++) {
        acc.push_back(p[i]);
        if ((p[i] == '\\' || p[i] == '/') && acc.size() > 3) CreateDirectoryA(acc.c_str(), nullptr);
    }
    CreateDirectoryA(p.c_str(), nullptr);
}

static bool card_present(int port) { return port == 0; }

// resolve a PS2 memcard path (absolute or relative to cwd) into a normalized card path "/a/b"
static std::string resolve(int port, const std::string& name) {
    std::string p = name;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.empty() || p[0] != '/') p = g_cwd[port] + (g_cwd[port].back() == '/' ? "" : "/") + p;
    std::vector<std::string> parts;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string s = p.substr(i, j - i);
        if (s == "..") { if (!parts.empty()) parts.pop_back(); }
        else if (!s.empty() && s != ".") parts.push_back(s);
        i = j + 1;
    }
    std::string out;
    for (auto& s : parts) out += "/" + s;
    return out.empty() ? "/" : out;
}

static std::string host_path(int port, const std::string& cardpath) {
    std::string h = g_root[port];
    for (char c : cardpath) h.push_back(c == '/' ? '\\' : c);
    return h;
}

static bool is_dir(const std::string& h) {
    DWORD a = GetFileAttributesA(h.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool exists(const std::string& h) { return GetFileAttributesA(h.c_str()) != INVALID_FILE_ATTRIBUTES; }

static u64 dir_size(const std::string& h) {
    u64 total = 0;
    WIN32_FIND_DATAA fd;
    HANDLE hf = FindFirstFileA((h + "\\*").c_str(), &fd);
    if (hf == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) total += 1024 + dir_size(h + "\\" + fd.cFileName);
        else total += ((((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow) + 1023) & ~1023ull;
    } while (FindNextFileA(hf, &fd));
    FindClose(hf);
    return total;
}

static u32 free_clusters(int port) {
    u64 used = dir_size(g_root[port]) / 1024;
    const u32 total = 8000;
    return used >= total ? 0 : (u32)(total - used);
}

static void fill_datetime(u8* d, const FILETIME& ft) {
    SYSTEMTIME st;
    FILETIME local;
    FileTimeToLocalFileTime(&ft, &local);
    FileTimeToSystemTime(&local, &st);
    d[0] = 0;
    d[1] = (u8)st.wSecond;
    d[2] = (u8)st.wMinute;
    d[3] = (u8)st.wHour;
    d[4] = (u8)st.wDay;
    d[5] = (u8)st.wMonth;
    d[6] = (u8)(st.wYear & 0xFF);
    d[7] = (u8)(st.wYear >> 8);
}

static bool wild_match(const char* pat, const char* s) {
    if (*pat == 0) return *s == 0;
    if (*pat == '*') return wild_match(pat + 1, s) || (*s && wild_match(pat, s + 1));
    if (*pat == '?') return *s && wild_match(pat + 1, s + 1);
    return toupper((u8)*pat) == toupper((u8)*s) && wild_match(pat + 1, s + 1);
}

// GetDir state (mode 1 continues the previous listing)
static std::vector<std::vector<u8>> g_dir_entries;
static size_t g_dir_pos = 0;

static int do_getdir(int port, const std::string& name, u32 mode, u32 maxent, u32 table) {
    if (mode == 0) {
        g_dir_entries.clear();
        g_dir_pos = 0;
        std::string full = resolve(port, name);
        size_t slash = full.find_last_of('/');
        std::string dir = full.substr(0, slash);
        std::string pat = full.substr(slash + 1);
        if (dir.empty()) dir = "/";
        std::string hdir = host_path(port, dir);
        if (!is_dir(hdir)) return RES_NOENTRY;
        auto add = [&](const std::string& n, bool isdir, u32 size, const FILETIME& ct, const FILETIME& mt) {
            std::vector<u8> e(64, 0);
            fill_datetime(&e[0], ct);
            fill_datetime(&e[8], mt);
            memcpy(&e[16], &size, 4);
            u16 attr = isdir ? 0x8427 : 0x8497;
            memcpy(&e[20], &attr, 2);
            memcpy(&e[32], n.c_str(), std::min<size_t>(n.size(), 31));
            g_dir_entries.push_back(std::move(e));
        };
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        if (dir != "/" && (pat == "*" || pat == "." || pat == "..")) {
            if (wild_match(pat.c_str(), ".")) add(".", true, 0, now, now);
            if (wild_match(pat.c_str(), "..")) add("..", true, 0, now, now);
        }
        WIN32_FIND_DATAA fd;
        HANDLE hf = FindFirstFileA((hdir + "\\*").c_str(), &fd);
        if (hf != INVALID_HANDLE_VALUE) {
            do {
                if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
                if (!wild_match(pat.c_str(), fd.cFileName)) continue;
                bool isdir = fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
                u32 size = isdir ? 0 : fd.nFileSizeLow;
                if (isdir) {
                    // directory size field = number of entries (incl. . and ..)
                    u32 n = 2;
                    WIN32_FIND_DATAA f2;
                    HANDLE h2 = FindFirstFileA((hdir + "\\" + fd.cFileName + "\\*").c_str(), &f2);
                    if (h2 != INVALID_HANDLE_VALUE) {
                        do { if (strcmp(f2.cFileName, ".") && strcmp(f2.cFileName, "..")) n++; } while (FindNextFileA(h2, &f2));
                        FindClose(h2);
                    }
                    size = n;
                }
                add(fd.cFileName, isdir, size, fd.ftCreationTime, fd.ftLastWriteTime);
            } while (FindNextFileA(hf, &fd));
            FindClose(hf);
        }
    }
    int n = 0;
    while (g_dir_pos < g_dir_entries.size() && (u32)n < maxent) {
        sif::ee_write(table + n * 64, g_dir_entries[g_dir_pos].data(), 64);
        n++;
        g_dir_pos++;
    }
    return n;
}

static int alloc_fd() {
    for (int i = 0; i < 32; i++)
        if (!g_fd[i].used) return i;
    return -1;
}

static u32 rpc(u32 fno, u32 buf, u32 size) {
    u8* b8 = mem::iop_ptr(buf);
    u32* b = (u32*)b8;
    s32 res = 0;
    switch (fno) {
    case 0xFE:  // init
        b[0] = 0;
        b[1] = 0x240;
        b[2] = 0x240;
        return buf;
    case 0x01: {  // GetInfo(port, slot, type?, free?, format?) -> result; info into EE rdata
        int port = (int)(b[1] & 1);
        u32 rdata = b[7];
        u32 info[64] = {};
        if (!card_present(port)) {
            res = RES_NOCARD;
        } else {
            info[0] = 2;                      // type: PS2 memory card
            info[1] = free_clusters(port);    // free space (KB)
            info[0x90 / 4] = 1;               // formatted
            res = g_reported[port] ? RES_OK : RES_CHANGED;
            g_reported[port] = true;
        }
        if (rdata) sif::ee_write(rdata, info, 0xC0);
        LOG(LOG_MC, "sceMcGetInfo(%d) -> %d", port, res);
        break;
    }
    case 0x02: {  // Open(port, slot, mode, name)
        int port = (int)(b[0] & 1);
        u32 mode = b[2];
        std::string name((char*)b8 + 20, strnlen((char*)b8 + 20, 1023));
        std::string cp = resolve(port, name);
        std::string hp = host_path(port, cp);
        LOG(LOG_MC, "sceMcOpen(%d, '%s' -> %s, mode %x)", port, name.c_str(), cp.c_str(), mode);
        if (!card_present(port)) { res = RES_NOCARD; break; }
        if (mode & 0x40) {  // mkdir
            if (exists(hp)) res = RES_NOENTRY;   // already exists
            else { ensure_dir(hp); res = RES_OK; }
            break;
        }
        if (is_dir(hp)) { res = RES_NOENTRY; break; }
        bool create = mode & 0x200;
        bool write = (mode & 3) != 1;
        if (!exists(hp) && !create) { res = RES_NOENTRY; break; }
        int fd = alloc_fd();
        if (fd < 0) { res = RES_UPLIMIT; break; }
        FILE* f = nullptr;
        if (!exists(hp)) f = fopen(hp.c_str(), "w+b");
        else f = fopen(hp.c_str(), write ? "r+b" : "rb");
        if (!f) { res = RES_DENIED; break; }
        g_fd[fd] = McFile{true, port, hp, f};
        res = fd;
        break;
    }
    case 0x03: {  // Close(fd)
        u32 fd = b[0];
        if (fd < 32 && g_fd[fd].used) { fclose(g_fd[fd].f); g_fd[fd] = McFile(); res = 0; }
        else res = RES_NOENTRY;
        break;
    }
    case 0x04: {  // Seek(fd, offset, origin)
        u32 fd = b[0];
        if (fd >= 32 || !g_fd[fd].used) { res = RES_NOENTRY; break; }
        fseek(g_fd[fd].f, (s32)b[4], b[5] == 0 ? SEEK_SET : b[5] == 1 ? SEEK_CUR : SEEK_END);
        res = (s32)ftell(g_fd[fd].f);
        break;
    }
    case 0x05: {  // Read(fd, size, buf, rdata)
        u32 fd = b[0], n = b[3], dst = b[6], rdata = b[7];
        if (fd >= 32 || !g_fd[fd].used) { res = RES_NOENTRY; break; }
        std::vector<u8> tmp(n);
        size_t got = fread(tmp.data(), 1, n, g_fd[fd].f);
        if (got) sif::ee_write(dst, tmp.data(), (u32)got);
        u32 hdr[4] = {0, 0, 0, 0};
        if (rdata) sif::ee_write(rdata, hdr, 16);
        res = (s32)got;
        LOG(LOG_MC, "sceMcRead(%u, %u) -> %d", fd, n, res);
        break;
    }
    case 0x06: {  // Write(fd, head bytes in packet, aligned part from EE)
        u32 fd = b[0];
        if (fd >= 32 || !g_fd[fd].used) { res = RES_NOENTRY; break; }
        u32 head = b[5], aligned_addr = b[6], aligned_size = b[3];
        std::vector<u8> data(b8 + 32, b8 + 32 + std::min<u32>(head, 16));
        if (aligned_size) {
            size_t off = data.size();
            data.resize(off + aligned_size);
            mem::copy_from_ee(&data[off], aligned_addr & 0x1FFFFFFF, aligned_size);
        }
        size_t w = fwrite(data.data(), 1, data.size(), g_fd[fd].f);
        res = (s32)w;
        LOG(LOG_MC, "sceMcWrite(%u, %zu) -> %d", fd, data.size(), res);
        break;
    }
    case 0x0A: {  // Flush(fd)
        u32 fd = b[0];
        if (fd < 32 && g_fd[fd].used) fflush(g_fd[fd].f);
        res = 0;
        break;
    }
    case 0x0C: {  // ChDir(port, slot, curdir_out, name)
        int port = (int)(b[0] & 1);
        std::string name((char*)b8 + 20, strnlen((char*)b8 + 20, 1023));
        u32 out = b[4];
        if (out) {
            char cur[1024] = {};
            strncpy(cur, g_cwd[port].c_str(), 1023);
            sif::ee_write(out, cur, 1024);
        }
        if (name.empty()) { res = 0; break; }
        std::string cp = resolve(port, name);
        if (is_dir(host_path(port, cp))) { g_cwd[port] = cp; res = 0; }
        else res = RES_NOENTRY;
        break;
    }
    case 0x0D: {  // GetDir(port, slot, mode, maxent, table, name)
        int port = (int)(b[0] & 1);
        std::string name((char*)b8 + 20, strnlen((char*)b8 + 20, 1023));
        if (!card_present(port)) { res = RES_NOCARD; break; }
        res = do_getdir(port, name, b[2], b[3], b[4]);
        LOG(LOG_MC, "sceMcGetDir(%d, '%s', mode %u, max %u) -> %d", port, name.c_str(), b[2], b[3], res);
        break;
    }
    case 0x0E: {  // SetFileInfo(port, slot, flags, info, name)
        res = 0;
        break;
    }
    case 0x0F: {  // Delete(port, slot, flags, name)
        int port = (int)(b[0] & 1);
        std::string name((char*)b8 + 20, strnlen((char*)b8 + 20, 1023));
        std::string hp = host_path(port, resolve(port, name));
        if (is_dir(hp)) res = RemoveDirectoryA(hp.c_str()) ? 0 : RES_NOTEMPTY;
        else if (exists(hp)) res = DeleteFileA(hp.c_str()) ? 0 : RES_DENIED;
        else res = RES_NOENTRY;
        LOG(LOG_MC, "sceMcDelete('%s') -> %d", name.c_str(), res);
        break;
    }
    case 0x10:  // Format
    case 0x11:  // Unformat
        res = 0;
        break;
    case 0x12:  // GetEntSpace
        res = 1024;
        break;
    case 0x14:  // change thread priority
        res = 0;
        break;
    case 0x15:  // slot max
        res = 1;
        break;
    case 0x35:  // UdCheckNewCard
        res = 0;
        break;
    default: {
        char t[64];
        snprintf(t, sizeof(t), "mc: unknown fno=0x%x", fno);
        modules::hexdump(LOG_MC, t, b8, size < 64 ? size : 64);
        res = 0;
        break;
    }
    }
    b[0] = (u32)res;
    return buf;
}

void register_servers() {
    std::string dir = platform::config().save_dir;
    if (dir.empty()) dir = exe_dir() + "\\memcards";
    g_root[0] = dir + "\\slot1";
    g_root[1] = dir + "\\slot2";
    ensure_dir(g_root[0]);
    register_native_server(0x80000400, 0x500, rpc);
}

void start(const std::string&) {}

}  // namespace iop::mc

namespace iop::mc {
bool files_open() {
    for (auto& f : g_fd)
        if (f.used) return true;
    return false;
}
void state(ss::Stream& s) {
    u32 ver = 1;
    s.v(ver);
    s.str(g_cwd[0]); s.str(g_cwd[1]);
    u32 n = (u32)g_dir_entries.size();
    s.v(n);
    if (s.loading) g_dir_entries.assign(n, {});
    for (auto& e : g_dir_entries) s.vec(e);
    u64 pos = g_dir_pos;
    s.v(pos);
    g_dir_pos = (size_t)pos;
    if (s.loading)  // no card file is open in a savestate (checked when saving)
        for (auto& f : g_fd)
            if (f.used) { if (f.f) fclose(f.f); f = McFile(); }
}
}  // namespace iop::mc
