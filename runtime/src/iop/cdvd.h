#pragma once
#include "../common.h"
#include <functional>

namespace cdvd {

bool open_image(const std::string& path);  // UTF-8 path
void close_image();
bool open_data_dir(const std::string& dir, const std::string& index);  // unpacked disc (tools/make_data_dir.py)
bool read_sectors(u32 lsn, u32 count, void* dst);
// ISO9660 lookup; path like "\\DATA\\FOO.DAT;1"
bool find_file(const std::string& path, u32* lsn, u32* size);
std::string file_at(u32 lsn, u32* file_lsn);  // file containing a sector ("" if none), for diagnostics
u32 total_sectors();
// Files of another disc image replace this disc's files of the same path (want picks them; check: a
// file that image must have). Returns the number of files, -1: cannot open, -2: not that disc.
int add_image_files(const std::string& path, const char* check, const std::function<bool(const std::string&)>& want);
void refresh_tables_in_ram();  // after a savestate load: index tables in memory as on this session's disc
int verify_redirects();        // test: replaced files as the game reads them == the other image's files (errors)
void on_game_read(u32 lsn);    // every disc read of the game (IOP sceCdRead)
u32 map_loads();               // map files (\DATA\xxxxxxxx.MDZ) read so far
u64 sectors_read();  // all sectors read so far (load detection)
void register_servers();

}  // namespace cdvd

namespace iop::cdvd {
using ::cdvd::register_servers;
}
