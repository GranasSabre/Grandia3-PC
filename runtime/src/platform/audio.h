// Audio output: lock-free ring buffer fed by the emulation thread (48 kHz stereo s16),
// consumed by a WASAPI shared-mode (event driven) render thread with drift compensation.
#pragma once
#include "../common.h"

namespace audio {

// Start the output device thread (if enabled in the config). Safe to call once at startup.
void init();
// Producer side (emulation thread only): n stereo frames of 48 kHz s16 audio. Never blocks.
void push(const s16* frames, int n);
// Write the mixed output (48 kHz s16 stereo, before the volume setting) to a WAV file.
// Call before the emulation starts.
void set_wav_path(const std::string& path);

}  // namespace audio
