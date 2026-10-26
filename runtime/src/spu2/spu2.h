// SPU2 emulation core: 2 MB sound RAM, 2 cores x 24 ADPCM voices, ADSR, gaussian interpolation,
// noise, pitch modulation, core mixing / chaining, AutoDMA (ADMA) input, reverb.
// All state is owned by the emulation thread (libsd HLE calls and sample generation both run there).
#pragma once
#include "../common.h"

namespace spu2 {

constexpr int RATE = 48000;
constexpr u32 RAM_HW = 1024 * 1024;   // sound RAM size in halfwords (2 MB)
constexpr u32 RAM_MASK = RAM_HW - 1;

extern u16 g_ram[RAM_HW];

// Volume register with sweep (voice VOLL/VOLR, core MVOLL/MVOLR).
struct Volume {
    u16 reg = 0;        // last written register value
    s32 level = 0;      // current volume (-0x8000..0x7FFF) = VOLX
    u32 counter = 0;
    void write(u16 v);
    void tick();
};

enum AdsrPhase { ADSR_OFF = 0, ADSR_ATTACK, ADSR_DECAY, ADSR_SUSTAIN, ADSR_RELEASE };

struct Voice {
    Volume vol_l, vol_r;
    u16 pitch = 0;
    u16 adsr1 = 0, adsr2 = 0;
    s32 envx = 0;               // current envelope level 0..0x7FFF
    int phase = ADSR_OFF;
    u32 env_counter = 0;
    u32 ssa = 0, lsa = 0;       // halfword addresses
    u32 cur = 0;                // halfword address of the block being played (NAX block)
    u32 counter = 0;            // pitch counter (bit 12+ = sample index in block)
    s32 prev1 = 0, prev2 = 0;   // ADPCM decoder history
    s16 dec[31] = {};           // [0..2] = last 3 samples of previous block, [3..30] = current block
    u8 flags = 0;               // loop flags of current block
    s32 outx = 0;               // last output after envelope (for pitch modulation of the next voice)
};

struct ReverbRegs {
    // address offsets (halfwords, relative to the current buffer position), PS1 register order
    u32 apf1_size = 0, apf2_size = 0;
    u32 same_l_dst = 0, same_r_dst = 0, comb1_l = 0, comb1_r = 0, comb2_l = 0, comb2_r = 0;
    u32 same_l_src = 0, same_r_src = 0, diff_l_dst = 0, diff_r_dst = 0, comb3_l = 0, comb3_r = 0;
    u32 comb4_l = 0, comb4_r = 0, diff_l_src = 0, diff_r_src = 0, apf1_l = 0, apf1_r = 0, apf2_l = 0, apf2_r = 0;
    // volumes
    s16 iir_vol = 0, comb1_vol = 0, comb2_vol = 0, comb3_vol = 0, comb4_vol = 0, wall_vol = 0;
    s16 apf1_vol = 0, apf2_vol = 0, in_l = 0, in_r = 0;
};

struct Adma {
    bool active = false;    // register 0x1B0 != 0
    bool loop = false;
    bool read = false;      // SPU -> IOP (memout capture)
    u32 base = 0;           // IOP address of buffer
    u32 half = 0;           // bytes per (half) transfer
    u32 buff = 0;           // which half is being transferred
    u32 madr = 0;           // current IOP DMA address
    u32 read_area = 0;      // halfword address of the SPU area for read mode
    u32 read_count = 1;     // number of consecutive 0x200-halfword areas (read mode)
};

struct Core {
    Voice v[24];
    u16 mmix = 0;
    u16 attr = 0;
    u32 pmon = 0, non = 0, vmixl = 0, vmixr = 0, vmixel = 0, vmixer = 0, endx = 0;
    u32 kon_last = 0, koff_last = 0;
    Volume mvol_l, mvol_r;
    s16 evol_l = 0, evol_r = 0, avol_l = 0, avol_r = 0, bvol_l = 0, bvol_r = 0;
    u32 esa = 0, eea_hi = 0, tsa = 0, irqa = 0;   // ESA/TSA/IRQA halfword addresses; EEA = (eea_hi << 16) | 0xFFFF
    ReverbRegs rv;
    // runtime state
    u32 rv_pos = 0;         // reverb buffer position (relative)
    s32 rv_down[2][128] = {};
    s32 rv_up[2][128] = {};
    u32 rv_bufpos = 0;
    u32 noise_level = 1;
    s32 noise_timer = 0;
    Adma adma;
};

extern Core g_core[2];

// separate volumes: what each voice plays (Grandia III's sound driver uses fixed voices for its
// two streams), gain 0..256 per category
enum Category { CAT_MUSIC = 0, CAT_VOICE, CAT_SFX, CAT_MOVIE, CAT_COUNT };
extern int g_cat_gain[CAT_COUNT];

void init();
// Generate samples up to emulated time t (seconds). Called on the emulation thread.
void advance_to(double t);

// register-level helpers used by the libsd HLE
void key_on(int core, u32 mask);
void key_off(int core, u32 mask);
void write_esa(int core, u32 hw_addr);
void set_attr(int core, u16 v);
inline u32 eea(int core) { return (g_core[core].eea_hi << 16) | 0xFFFF; }
u32 voice_nax(int core, int voice);   // halfword address of current playback position
bool voice_playing(int core, int voice);

// AutoDMA (block transfer)
int adma_start_write(int core, u32 iop_addr, u32 size, bool loop, u32 start_offset, u32 start_buff);
int adma_start_read(int core, u32 iop_addr, u32 size, bool loop, u32 area_hw, u32 count);
u32 adma_status(int core);
u32 adma_stop(int core);

}  // namespace spu2
