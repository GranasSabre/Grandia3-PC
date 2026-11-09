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
#include <array>
#include <functional>

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

using SectorReader = std::function<bool(u32 lsn, u32 count, u8* dst)>;
static u32 g_root_lsn = 0, g_root_size = 0;

static void scan_dir(const SectorReader& rd, std::map<std::string, Entry>& out, u32 lsn, u32 size,
                     const std::string& prefix, int depth) {
    if (depth > 8) return;
    std::vector<u8> d((size + 2047) & ~2047u);
    if (!rd(lsn, (u32)d.size() / 2048, d.data())) return;
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
        out[full] = Entry{elsn, esize, (flags & 2) != 0};
        if (flags & 2) scan_dir(rd, out, elsn, esize, full, depth + 1);
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
    g_root_lsn = rlsn;
    g_root_size = rsize;
    scan_dir([](u32 l, u32 c, u8* d) { return read_sectors(l, c, d); }, g_files, rlsn, rsize, "", 0);
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

// the game reads the ISO directory itself: change the record of the file at lsn in the directory sectors
static void patch_dir_record(u32 lsn, u32 new_lsn, u32 size) {
    auto put = [](u8* p, u32 v) {
        memcpy(p, &v, 4);
        for (int i = 0; i < 4; i++) p[4 + i] = (u8)(v >> (24 - 8 * i));
    };
    for (size_t s = 0; s < g_dsys_lsn.size(); s++) {
        u8* sec = &g_dsys[s * 2048];
        for (u32 off = 0; off + 33 < 2048 && sec[off];) {
            u8 len = sec[off];
            u32 elsn;
            memcpy(&elsn, sec + off + 2, 4);
            if (elsn == lsn && !(sec[off + 25] & 2)) {
                put(sec + off + 2, new_lsn);
                put(sec + off + 10, size);
            }
            off += len;
        }
    }
}

// A file in override\ (next to data\) replaces the disc file of the same name. One larger than the
// sectors of the disc file gets its own sectors after the end of the disc, and the file table points there.
static void place_overrides() {
    u32 next = (u32)((g_iso_size + 2047) / 2048) + 16;
    std::vector<DataFile> moved;
    for (const DataFile& d : g_dfiles) {
        std::string rel = d.path;
        if (rel.rfind("disc2\\", 0) == 0) rel = rel.substr(6);
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExA((g_ddir + "\\..\\override\\" + rel).c_str(), GetFileExInfoStandard, &fa)) continue;
        u32 size = fa.nFileSizeLow;
        auto e = std::find_if(g_files.begin(), g_files.end(), [&](const auto& f) { return !f.second.dir && f.second.lsn == d.lsn; });
        if (e == g_files.end()) continue;
        e->second.size = size;
        if (size <= (d.size + 2047) / 2048 * 2048) {
            patch_dir_record(d.lsn, d.lsn, size);
            continue;
        }
        DataFile n;
        n.lsn = next;
        n.size = size;
        n.path = d.path;
        e->second.lsn = next;
        patch_dir_record(d.lsn, next, size);
        next += (size + 2047) / 2048;
        log_write(LOG_CDVD, "override: %s (%u bytes) moved to lsn %u", rel.c_str(), size, n.lsn);
        moved.push_back(std::move(n));
    }
    if (moved.empty()) return;
    for (DataFile& n : moved) g_dfiles.push_back(std::move(n));
    std::sort(g_dfiles.begin(), g_dfiles.end(), [](const DataFile& a, const DataFile& b) { return a.lsn < b.lsn; });
    g_iso_size = (u64)next * 2048;
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
    if (!scan_volume(dir + "\\" + index)) return false;
    place_overrides();
    return true;
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

// Files replaced by files of another disc image (option jp_voices): their sectors are read from that
// image; a file larger than its sectors on this disc moves behind the end of the disc. The game reads
// the ISO directory itself, so the changed records are kept as patched directory sectors.
struct Redirect {
    u32 lsn, nsec;
    HANDLE h;
    u64 base, size;  // file position and length in the other image
    std::string name;
};
static std::vector<Redirect> g_redirects;                  // sorted by lsn
static std::map<u32, std::array<u8, 2048>> g_dir_patches;  // lsn -> directory sector with changed records
static HANDLE g_iso2 = INVALID_HANDLE_VALUE;

static bool read_plain(u32 lsn, u32 count, void* dst) {
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

bool read_sectors(u32 lsn, u32 count, void* dst) {
    g_sectors_read += count;
    std::lock_guard<std::mutex> lk(g_iso_mutex);
    if (g_redirects.empty() && g_dir_patches.empty()) return read_plain(lsn, count, dst);
    u8* p = (u8*)dst;
    bool ok = true;
    while (count) {
        auto it = std::upper_bound(g_redirects.begin(), g_redirects.end(), lsn, [](u32 l, const Redirect& r) { return l < r.lsn; });
        u32 n = count;
        if (it != g_redirects.begin() && lsn < (it - 1)->lsn + (it - 1)->nsec) {
            const Redirect& r = *(it - 1);
            n = std::min(count, r.lsn + r.nsec - lsn);
            u64 off = (u64)(lsn - r.lsn) * 2048;
            memset(p, 0, (size_t)n * 2048);
            u64 len = off < r.size ? std::min<u64>((u64)n * 2048, r.size - off) : 0;
            if (len) {
                OVERLAPPED ov{};
                u64 at = r.base + off;
                ov.Offset = (DWORD)at;
                ov.OffsetHigh = (DWORD)(at >> 32);
                DWORD got = 0;
                ok &= ReadFile(r.h, p, (DWORD)len, &got, &ov) && got == len;
            }
        } else {
            if (it != g_redirects.end()) n = std::min(count, it->lsn - lsn);
            ok &= read_plain(lsn, n, p);
        }
        for (u32 i = 0; i < n; i++) {
            auto pt = g_dir_patches.find(lsn + i);
            if (pt != g_dir_patches.end()) memcpy(p + (size_t)i * 2048, pt->second.data(), 2048);
        }
        lsn += n;
        count -= n;
        p += (size_t)n * 2048;
    }
    return ok;
}

// the directory record of a file (path as in g_files): new position and size
static bool patch_record(const std::string& path, u32 new_lsn, u32 new_size) {
    size_t cut = path.find_last_of('\\');
    std::string parent = path.substr(0, cut), name = path.substr(cut);
    u32 dl = g_root_lsn, ds = g_root_size;
    if (!parent.empty()) {
        auto d = g_files.find(parent);
        if (d == g_files.end() || !d->second.dir) return false;
        dl = d->second.lsn;
        ds = d->second.size;
    }
    auto put = [](u8* q, u32 v) {
        memcpy(q, &v, 4);
        for (int i = 0; i < 4; i++) q[4 + i] = (u8)(v >> (24 - 8 * i));
    };
    for (u32 s = 0; s < (ds + 2047) / 2048; s++) {
        std::array<u8, 2048> sec;
        auto pt = g_dir_patches.find(dl + s);
        if (pt != g_dir_patches.end()) sec = pt->second;
        else if (!read_plain(dl + s, 1, sec.data())) return false;
        for (u32 off = 0; off + 33 < 2048 && sec[off];) {
            u8 len = sec[off], nlen = sec[off + 32];
            if (off + 33 + nlen > 2048) break;
            if (!(sec[off + 25] & 2) && norm(std::string((char*)&sec[off + 33], nlen)) == name) {
                put(&sec[off + 2], new_lsn);
                put(&sec[off + 10], new_size);
                g_dir_patches[dl + s] = sec;
                return true;
            }
            off += len;
        }
    }
    return false;
}

// The voice and sound bank index tables are read once at the start and stay in memory. Both discs'
// versions are kept here (this disc's and the other image's), so a savestate of a session with the
// other voice set can be adapted after loading (refresh_tables_in_ram).
static const char* const k_tables[] = {"\\MUSIC\\GR3_STR.IDX", "\\MUSIC\\GR3.IDX"};
static std::map<std::string, std::vector<std::vector<u8>>> g_table_versions;

static void add_table_version(const std::string& name, std::vector<u8> v) {
    auto& vs = g_table_versions[name];
    for (const auto& x : vs)
        if (x == v) return;
    vs.push_back(std::move(v));
}

int add_image_files(const std::string& path, const char* check, const std::function<bool(const std::string&)>& want) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    auto rd = [h](u32 l, u32 c, u8* d) {
        OVERLAPPED ov{};
        u64 at = (u64)l * 2048;
        ov.Offset = (DWORD)at;
        ov.OffsetHigh = (DWORD)(at >> 32);
        DWORD got = 0;
        return ReadFile(h, d, c * 2048, &got, &ov) && got == c * 2048;
    };
    u8 pvd[2048];
    std::map<std::string, Entry> other;
    if (rd(16, 1, pvd) && memcmp(pvd + 1, "CD001", 5) == 0) {
        u32 rlsn, rsize;
        memcpy(&rlsn, pvd + 156 + 2, 4);
        memcpy(&rsize, pvd + 156 + 10, 4);
        scan_dir(rd, other, rlsn, rsize, "", 0);
    }
    if (!other.count(norm(check))) {
        CloseHandle(h);
        log_write(LOG_CDVD, "%s: not the expected disc (no %s)", path.c_str(), check);
        return -2;
    }
    std::lock_guard<std::mutex> lk(g_iso_mutex);
    for (const char* t : k_tables) {
        auto e = g_files.find(norm(t));
        auto o = other.find(norm(t));
        if (e == g_files.end() || o == other.end() || e->second.size != o->second.size || e->second.size > 256 * 1024) continue;
        u32 ns = (e->second.size + 2047) / 2048;
        std::vector<u8> a((size_t)ns * 2048), b((size_t)ns * 2048);
        if (!read_plain(e->second.lsn, ns, a.data()) || !rd(o->second.lsn, ns, b.data())) continue;
        a.resize(e->second.size);
        b.resize(e->second.size);
        add_table_version(e->first, std::move(a));
        add_table_version(e->first, std::move(b));
    }
    u32 next = (u32)((g_iso_size + 2047) / 2048) + 16;
    for (const auto& r : g_redirects) next = std::max(next, r.lsn + r.nsec + 16);
    int count = 0;
    for (auto& [name, e] : g_files) {
        auto o = other.find(name);
        if (e.dir || o == other.end() || o->second.dir || !want(name)) continue;
        u32 nsec_old = (e.size + 2047) / 2048, nsec_new = (o->second.size + 2047) / 2048;
        Redirect r{e.lsn, nsec_old, h, (u64)o->second.lsn * 2048, o->second.size, name};
        if (nsec_new > nsec_old) {
            r.lsn = next;
            r.nsec = nsec_new;
            next += nsec_new + 16;
        }
        if (!patch_record(name, r.lsn, o->second.size)) {
            log_write(LOG_CDVD, "%s: directory record not found", name.c_str());
            continue;
        }
        e.lsn = r.lsn;
        e.size = o->second.size;
        g_redirects.push_back(r);
        count++;
    }
    std::sort(g_redirects.begin(), g_redirects.end(), [](const Redirect& a, const Redirect& b) { return a.lsn < b.lsn; });
    if (count) g_iso2 = h;
    else CloseHandle(h);
    log_write(LOG_CDVD, "%s: %d files read from it", path.c_str(), count);
    return count;
}

// Map loads (automatic savestate): the game read the start of a map file \DATA\xxxxxxxx.MDZ
static std::atomic<u32> g_map_loads{0};
u32 map_loads() { return g_map_loads.load(std::memory_order_relaxed); }

void on_game_read(u32 lsn) {
    static std::map<u32, bool> starts;  // first sector of every map file
    static bool built = false;
    if (!built) {
        built = true;
        for (const auto& [name, e] : g_files)
            if (!e.dir && name.size() == 18 && name.rfind("\\DATA\\", 0) == 0 && name.compare(14, 4, ".MDZ") == 0) starts[e.lsn] = true;
    }
    if (starts.count(lsn)) g_map_loads.fetch_add(1, std::memory_order_relaxed);
}

// --verify-redirects: the directory as the game sees it (read through the patched sectors) points to
// every replaced file, and its sectors hold exactly the other image's file. Returns the number of errors.
int verify_redirects() {
    std::map<std::string, Entry> seen;
    scan_dir([](u32 l, u32 c, u8* d) { return read_sectors(l, c, d); }, seen, g_root_lsn, g_root_size, "", 0);
    int bad = 0;
    u64 bytes = 0;
    std::vector<u8> a(256 * 2048), b(256 * 2048);
    for (const Redirect& r : g_redirects) {
        auto s = seen.find(r.name);
        if (s == seen.end() || s->second.lsn != r.lsn || s->second.size != r.size) {
            log_write(LOG_CDVD, "verify: %s: directory record wrong", r.name.c_str());
            bad++;
            continue;
        }
        for (u32 done = 0; done < r.nsec;) {
            u32 n = std::min<u32>(256, r.nsec - done);
            u64 off = (u64)done * 2048, len = off < r.size ? std::min<u64>((u64)n * 2048, r.size - off) : 0;
            std::fill(b.begin(), b.end(), 0);
            OVERLAPPED ov{};
            ov.Offset = (DWORD)(r.base + off);
            ov.OffsetHigh = (DWORD)((r.base + off) >> 32);
            DWORD got = 0;
            bool ok = read_sectors(r.lsn + done, n, a.data()) && (!len || (ReadFile(r.h, b.data(), (DWORD)len, &got, &ov) && got == len)) &&
                      memcmp(a.data(), b.data(), (size_t)n * 2048) == 0;
            if (!ok) {
                log_write(LOG_CDVD, "verify: %s: data differs at sector %u", r.name.c_str(), done);
                bad++;
                break;
            }
            done += n;
            bytes += len;
        }
    }
    log_write(LOG_CDVD, "verify: %zu replaced files, %llu MB compared, %d errors", g_redirects.size(), (unsigned long long)(bytes >> 20), bad);
    return bad;
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

// After a savestate load: copies of the index tables in memory that belong to the other voice set get
// this session's values. Only the bytes where the two versions differ change (the game changes some
// entries of GR3.IDX in memory itself), and only in copies that clearly hold the other version there.
void refresh_tables_in_ram() {
    for (const auto& [name, versions] : g_table_versions) {
        auto it = g_files.find(name);
        if (it == g_files.end() || versions.size() < 2) continue;
        u32 n = it->second.size;
        std::vector<u8> cur(((size_t)n + 2047) & ~(size_t)2047);
        if (!read_sectors(it->second.lsn, (u32)cur.size() / 2048, cur.data())) continue;
        cur.resize(n);
        for (const auto& alt : versions) {
            if (alt.size() != n || alt == cur) continue;
            std::vector<u32> diff;
            for (u32 i = 0; i < n; i++)
                if (alt[i] != cur[i]) diff.push_back(i);
            size_t pre = std::min<size_t>(64, diff.front());  // both versions begin alike
            if (pre < 16) continue;
            u32 first;
            memcpy(&first, cur.data(), 4);
            auto scan = [&](u8* ram, u32 size, const char* where) {
                for (u32 a = 0; a + n <= size; a += 4) {
                    u32 v;
                    memcpy(&v, ram + a, 4);
                    if (v != first || memcmp(ram + a, cur.data(), pre) != 0) continue;
                    size_t m_alt = 0, m_cur = 0;
                    for (u32 i : diff) {
                        m_alt += ram[a + i] == alt[i];
                        m_cur += ram[a + i] == cur[i];
                    }
                    // (GR3.IDX: the game keeps state in its copy, which matches neither file; it stays)
                    if (m_alt > m_cur && m_alt * 10 >= diff.size() * 9) {
                        for (u32 i : diff)
                            if (ram[a + i] == alt[i]) ram[a + i] = cur[i];
                        log_write(LOG_CDVD, "%s: %s copy at %08x set to this voice set (%zu of %zu bytes)", name.c_str(), where, a, m_alt, diff.size());
                    }
                    a += (n - 4) & ~3u;
                }
            };
            scan(mem::ram, mem::RAM_SIZE, "EE");
            scan(mem::iop_ram, mem::IOP_RAM_SIZE, "IOP");
        }
    }
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
