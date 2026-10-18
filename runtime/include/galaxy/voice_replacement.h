#pragma once

#include <cstdint>
#include <filesystem>

namespace galaxy::voice {

// Mario's voice lines from the user's own clips (content\mods\voice, built by
// tools/animations/build_voice_mod.py). voices.txt lists, per JAudio sound ID, the clips to pick from:
//     0x00010000 jump_1.wav jump_2.wav ...
// Clips are 16-bit mono WAV at the DSP's 32000 Hz. The clip takes the place of the game's samples on the
// channel that plays the line, so the game's pan, distance volume, filters and effect sends (reverb, echo)
// apply to it as to the original (jaudio_host_mixer.h).

// Reads the folder once; without it nothing is replaced.
void load(const std::filesystem::path& content_root);

enum class Claim : std::uint8_t {
    None,     // not replaced: the channel plays the game's line
    Carrier,  // this channel carries the clip
    Silent,   // another channel of the same line carries it; this one is silenced
};

// A channel starts the sound `sound_id`. The first channel of a line (its `start` stamp) carries a newly
// picked clip; a new line cuts the clip before it, as the voice of one character does.
Claim claim(std::uint32_t sound_id, std::uint64_t start) noexcept;

// The carrier's next `count` samples of the clip (silence once it has ended), in place of the channel's.
void render(std::int16_t* out, std::uint32_t count) noexcept;

// Whether a clip is still playing (the mixer finishes it through its channel's last state when the channel
// ends first).
bool playing() noexcept;

// Once per subframe after the channels: a clip that nothing rendered this subframe goes to the front buses
// here, centred, at `gain_q16` (65536 = unity), so a line is never cut short.
void mix_remainder(std::int16_t* left, std::int16_t* right, std::uint32_t count, std::uint32_t gain_q16) noexcept;

}  // namespace galaxy::voice
