#pragma once
// Subtitles for voiced cutscenes. Their voice tracks are entries of MUSIC\GR3_STR.STZ that the IOP
// streams through a ring buffer in sound RAM; the module follows that data path (disc read -> IOP
// buffer -> sceSdVoiceTrans -> SPU2 RAM) so the read position of the playing voice gives the exact
// time within the track. Lines come from lang\sub_<language>.tsv: tag (hex) TAB start TAB end TAB text,
// times in seconds from the start of the track.
#include "../common.h"
#include <string>
#include <vector>

namespace subs {

void init();                                                  // after the disc is open
void on_cd_read(u32 lsn, u32 count, u32 iop_addr);            // IOP sceCdRead
void on_voice_trans(u32 iop_addr, u32 spu_addr, u32 bytes);   // sceSdVoiceTrans, IOP -> sound RAM
void on_key_on(int core, int voice, u32 spu_addr);           // --log=spu: logs which disc file a voice plays
// Current line drawn for a picture of pic_w x pic_h pixels; false when no line is due.
bool pixels(int pic_w, int pic_h, std::vector<u32>& out, int& w, int& h, u64& version);

}  // namespace subs
