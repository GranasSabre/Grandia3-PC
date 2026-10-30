#pragma once
#include "../common.h"

namespace gs {
void raster_represent();   // GPU renderer: present the last picture again (paused game, in-game menu)

void init();
void set_crt(u32 interlace, u32 mode, u32 field);
u32 get_imr();
void put_imr(u32 v);
u64 read_csr();
void reraise_events();  // after loading a savestate: SIGNAL / FINISH set without its interrupt
u64 flip_count();  // game frame buffer flips so far
extern int g_test_frame_delay_ms;  // --frame-delay: sleep per flip (test of the 60 fps fallback)
void on_vblank_start();
void on_vblank_end();
u64 priv_read64(u32 addr);
void priv_write64(u32 addr, u64 v);

// GIF interface
void gif_tag_start();
void write_reg(u32 reg, u64 v);
void write_packed(u32 reg, const u8* qw);
void image_write(const u8* data, u32 qwc);
// local -> host transfer (VIF1 readback)
void image_read(u8* dst, u32 qwc);

}  // namespace gs
