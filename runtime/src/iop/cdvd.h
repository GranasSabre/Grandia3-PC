#pragma once
#include "../common.h"

namespace cdvd {

bool open_image(const std::string& path);  // UTF-8 path
void close_image();
bool open_data_dir(const std::string& dir, const std::string& index);  // unpacked disc (tools/make_data_dir.py)
bool read_sectors(u32 lsn, u32 count, void* dst);
// ISO9660 lookup; path like "\\DATA\\FOO.DAT;1"
bool find_file(const std::string& path, u32* lsn, u32* size);
std::string file_at(u32 lsn, u32* file_lsn);  // file containing a sector ("" if none), for diagnostics
u32 total_sectors();
u64 sectors_read();  // all sectors read so far (load detection)
void register_servers();

}  // namespace cdvd

namespace iop::cdvd {
using ::cdvd::register_servers;
}
