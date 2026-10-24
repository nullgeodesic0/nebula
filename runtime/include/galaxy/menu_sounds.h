#pragma once

// The game's own menu sounds for the host-drawn menus, synthesised on the host from the user's disc.
//
// The game's system sound effects are JAudio2 sequences (AudioRes/SMR.szs: the sound table, the SE sequence
// collection, the instrument banks and the wave system tables; the AFC waves in AudioRes/Waves/*.aw). load()
// reads them through GameDisc (and JAudio's envelope curve tables from the game's DOL) and renders each menu
// sound once, the way the game's audio code does: the sequence, track, channel, envelope and mixer-volume
// code of JAudio2 restated on the host, feeding JAudioHostMixer (the runtime's host restatement of the game's
// DSP ucode) on private memory. Nothing here reads or writes guest memory or calls the game's sound
// functions, so online play is unaffected.
//
// The rendered sounds are 32 kHz stereo (the game's DAC rate), as float in [-1, 1]. play() mixes one into a
// private XAudio2 voice, scaled by the user's master and sound-effect volumes.

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace galaxy::gx {
class GameDisc;
}

namespace galaxy::menu_sounds {

enum class Sound : std::uint8_t {
    Cursor,       // SE_SY_BUTTON_CURSOR_ON: the pointer moves onto a button
    Decide,       // SE_SY_FILE_SEL_UPPER_DECIDE
    Cancel,       // SE_SY_GALAXY_DECIDE_CANCEL
    PauseOn,      // SE_SY_PAUSE_ON
    PauseOff,     // SE_SY_PAUSE_OFF
    WindowOpen,   // SE_SY_GALAMAP_WINDOW_OPEN
    WindowClose,  // SE_SY_GALAMAP_WINDOW_CLOSE
    Yes,          // SE_SY_TALK_SELECT_YES
    No,           // SE_SY_TALK_SELECT_NO
    Ok,           // SE_SY_TALK_OK
    TalkFocus,    // SE_SY_TALK_FOCUS_ITEM
    Count,
};

inline constexpr std::uint32_t kSampleRate = 32000u;
inline constexpr std::uint32_t kChannels = 2u;

// Reads AudioRes/SMR.szs and the waves the menu sounds use from game.pak and renders every Sound to PCM once.
// Returns false (and keeps nothing) if anything is missing or malformed. Safe to call again; a later
// successful load replaces the sounds. Not for the render thread: it decodes and renders (tens of ms).
bool load(const galaxy::gx::GameDisc& disc);
[[nodiscard]] bool loaded() noexcept;

// Starts `sound` at `volume` (times the user's master and SFX volumes). Thread-safe and non-blocking: it only
// queues the already rendered buffer on a free voice of the private XAudio2 engine. Does nothing before a
// successful load() or when no audio device is available.
void play(Sound sound, float volume = 1.0f);

// Whether load() found an audio device for the private voices, and how many of them are sounding now.
[[nodiscard]] bool has_output() noexcept;
[[nodiscard]] std::uint32_t voices_playing() noexcept;

// Stops every sound and releases the voices and the engine. load() may be called again afterwards.
void shutdown() noexcept;

// The sound-table name a Sound is rendered from ("SE_SY_BUTTON_CURSOR_ON").
[[nodiscard]] const char* sound_name(Sound sound) noexcept;

// The rendered PCM of a loaded sound: interleaved stereo float at kSampleRate. Empty before load().
[[nodiscard]] std::vector<float> rendered(Sound sound);

// ---- Rendering, for tools and tests -------------------------------------------------------------------

struct RenderOptions {
    // Multiplies the sound-table volume (the JAISe's sound parameters). See menu_sounds.cpp for how the
    // default was measured against the game's own output.
    float sound_volume_scale = -1.0f;  // < 0: the measured default
    // Collects one line per executed sequence command and per channel event.
    bool trace = false;
    // Upper bound on the rendered length.
    float max_seconds = 8.0f;
    // Diagnostics: tracks (bit n: the track the trace calls "trackN", N < 32) whose gate notes are not played.
    std::uint32_t mute_tracks = 0;
    // Diagnostics: DSP channels free for the sound (0: all 64), as when the game's other sounds hold the rest.
    std::uint32_t dsp_channels = 0;
};

struct Rendered {
    bool ok = false;
    std::vector<std::int16_t> pcm;  // interleaved stereo, kSampleRate, exactly as the DSP mixer writes it
    std::uint32_t frames = 0;
    std::string trace;
    std::string error;
};

// The parsed sound archive (sound table and names, sequence collection, banks, wave tables) with the waves
// read from the disc on demand. Not thread-safe.
class SeRenderer {
public:
    [[nodiscard]] static std::unique_ptr<SeRenderer> create(const galaxy::gx::GameDisc& disc, std::string* error);
    virtual ~SeRenderer() = default;

    // A section-0 sound by its sound-table name ("SE_SY_PAUSE_ON") or "s,g,i" id.
    [[nodiscard]] virtual Rendered render(std::string_view name, const RenderOptions& options) = 0;
    // Every name in the sound table's section 0 group 0 (the system sounds), in id order.
    [[nodiscard]] virtual std::vector<std::string> system_sound_names() const = 0;
};

}  // namespace galaxy::menu_sounds
