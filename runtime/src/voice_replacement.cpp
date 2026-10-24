#include "galaxy/voice_replacement.h"

#include "galaxy/runtime_settings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace galaxy::voice {
namespace {

struct Clip {
    std::vector<std::int16_t> samples;
};

// Written by load() before the audio thread starts; read and played on the audio thread only.
std::vector<Clip> s_clips;
std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> s_lines;
std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> s_luigi_lines;
std::uint32_t s_playing = UINT32_MAX;
std::size_t s_position = 0;
std::uint64_t s_last_start = UINT64_MAX;
bool s_rendered = false;  // the carrier channel took samples this subframe
// GALAXY_TRACE_VOICE: samples of the current clip played on the game's channel, after it, and centred.
std::uint64_t s_on_channel = 0, s_after_channel = 0, s_centred = 0;
bool trace_enabled() noexcept {
    static const bool trace = [] {
        char value[8]{};
        std::size_t length = 0;
        return getenv_s(&length, value, sizeof(value), "GALAXY_TRACE_VOICE") == 0 && length > 1u && value[0] == '1';
    }();
    return trace;
}
void report(const char* why) noexcept {
    if (trace_enabled() && s_playing < s_clips.size()) {
        std::cerr << "[voice] clip " << s_playing << " " << why << ": on channel " << s_on_channel
                  << " after " << s_after_channel << " centred " << s_centred << " of "
                  << s_clips[s_playing].samples.size() << '\n';
    }
}
std::minstd_rand s_random{0x6d61726fu};

bool read_wav(const std::filesystem::path& path, Clip& clip) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (data.size() < 12u || std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
        return false;
    }
    const auto u16 = [&data](std::size_t at) {
        return static_cast<std::uint16_t>(static_cast<std::uint8_t>(data[at]) | static_cast<std::uint8_t>(data[at + 1]) << 8);
    };
    const auto u32 = [&data, &u16](std::size_t at) { return static_cast<std::uint32_t>(u16(at) | u16(at + 2) << 16); };
    bool format_ok = false;
    for (std::size_t at = 12; at + 8 <= data.size();) {
        const std::uint32_t size = u32(at + 4);
        if (std::memcmp(data.data() + at, "fmt ", 4) == 0 && at + 8 + 16 <= data.size()) {
            format_ok = u16(at + 8) == 1u && u16(at + 10) == 1u && u32(at + 12) == 32000u && u16(at + 22) == 16u;
        } else if (std::memcmp(data.data() + at, "data", 4) == 0 && format_ok) {
            const std::size_t count = std::min<std::size_t>(size, data.size() - at - 8) / 2u;
            clip.samples.resize(count);
            std::memcpy(clip.samples.data(), data.data() + at + 8, count * 2u);
            return true;
        }
        at += 8u + size + (size & 1u);
    }
    return false;
}

}  // namespace

void load(const std::filesystem::path& content_root) {
    const std::filesystem::path folder = content_root / L"mods" / L"voice";
    std::ifstream list(folder / L"voices.txt");
    if (!list) {
        return;
    }
    std::unordered_map<std::string, std::uint32_t> loaded;
    std::string line;
    while (std::getline(list, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string id_text;
        if (!(fields >> id_text)) continue;
        const bool luigi = id_text == "luigi";
        if (luigi && !(fields >> id_text)) continue;
        const std::uint32_t id = static_cast<std::uint32_t>(std::stoul(id_text, nullptr, 0));
        std::string name;
        while (fields >> name) {
            if (name[0] == '#') break;  // the rest of the line is a comment
            auto found = loaded.find(name);
            if (found == loaded.end()) {
                Clip clip;
                if (!read_wav(folder / name, clip)) {
                    std::cerr << "[voice] cannot read " << (folder / name).string() << '\n';
                    continue;
                }
                found = loaded.emplace(name, static_cast<std::uint32_t>(s_clips.size())).first;
                s_clips.push_back(std::move(clip));
            }
            (luigi ? s_luigi_lines : s_lines)[id].push_back(found->second);
        }
    }
    std::cerr << "[voice] " << s_lines.size() << " voice lines (" << s_luigi_lines.size() << " Luigi's), "
              << s_clips.size() << " clips from " << folder.string() << '\n';
}

Claim claim(std::uint32_t sound_id, std::uint64_t start, bool luigi) noexcept {
    // GALAXY_TRACE_VOICE=1 logs every player voice line the game starts, replaced or not.
    static const bool trace = [] {
        char value[8]{};
        std::size_t length = 0;
        return getenv_s(&length, value, sizeof(value), "GALAXY_TRACE_VOICE") == 0 && length > 1u && value[0] == '1';
    }();
    if (trace && (sound_id >> 16u) == 1u && start != s_last_start) {
        std::cerr << "[voice] line 0x" << std::hex << sound_id << std::dec << (luigi ? " (Luigi)" : "")
                  << ((luigi ? s_luigi_lines : s_lines).count(sound_id) ? " replaced" : " kept") << '\n';
    }
    // Mario's clips never stand in for Luigi: his take keeps the game's voice unless clips of his own are given.
    const auto& lines = luigi ? s_luigi_lines : s_lines;
    const auto line = lines.find(sound_id);
    if (line == lines.end() || line->second.empty() || !get_runtime_settings().custom_voice) {
        return Claim::None;
    }
    if (start == s_last_start) {
        return Claim::Silent;
    }
    static int logged = 0;
    if (logged < 4) {
        ++logged;
        std::cerr << "[voice] replaced 0x" << std::hex << sound_id << std::dec << '\n';
    }
    report("cut by a new line");
    s_last_start = start;
    s_playing = line->second[s_random() % line->second.size()];
    s_position = 0;
    s_on_channel = s_after_channel = s_centred = 0;
    return Claim::Carrier;
}

bool playing() noexcept {
    return s_playing < s_clips.size();
}

void render(std::int16_t* out, std::uint32_t count) noexcept {
    s_rendered = true;
    std::uint32_t i = 0;
    if (s_playing < s_clips.size()) {
        const std::vector<std::int16_t>& samples = s_clips[s_playing].samples;
        // The clips come loudness-normalised with a true-peak ceiling (build_voice_mod.py), and play at that
        // level: the game's channel volume and placement still apply. Matching each clip to the energy of the
        // game's own line made the same clip swing by 8 dB from one play to the next (the game picks among
        // its own takes) and pushed near-full-scale clips into clipping.
        bool game = false;
        for (std::uint32_t k = 0; k < count && !game; ++k) {
            game = out[k] != 0;
        }
        (game ? s_on_channel : s_after_channel) += count;
        for (; i < count && s_position < samples.size(); ++i, ++s_position) {
            out[i] = samples[s_position];
        }
        if (s_position >= samples.size()) {
            report("finished");
            s_playing = UINT32_MAX;
        }
    }
    std::fill(out + i, out + count, std::int16_t{0});
}

void mix_remainder(std::int16_t* left, std::int16_t* right, std::uint32_t count, std::uint32_t gain_q16) noexcept {
    const bool carried = s_rendered;
    s_rendered = false;
    if (carried || s_playing >= s_clips.size()) {
        return;
    }
    // The carrier channel has ended: the rest plays centred, without the game's placement.
    s_centred += count;
    const std::vector<std::int16_t>& samples = s_clips[s_playing].samples;
    for (std::uint32_t i = 0; i < count && s_position < samples.size(); ++i, ++s_position) {
        const std::int32_t v = static_cast<std::int32_t>(
            (static_cast<std::int64_t>(samples[s_position]) * gain_q16) >> 17);  // -6 dB per side
        left[i] = static_cast<std::int16_t>(std::clamp<std::int32_t>(left[i] + v, -32768, 32767));
        right[i] = static_cast<std::int16_t>(std::clamp<std::int32_t>(right[i] + v, -32768, 32767));
    }
    if (s_position >= samples.size()) {
        report("finished centred");
        s_playing = UINT32_MAX;
    }
}

PlaybackState save_playback() noexcept {
    return {s_playing, s_position, s_last_start, s_rendered, s_game_energy, s_clip_energy, s_gain, s_game_peak,
            s_on_channel, s_after_channel, s_centred, s_random};
}

void restore_playback(const PlaybackState& state) noexcept {
    s_playing = state.playing;
    s_position = state.position;
    s_last_start = state.last_start;
    s_rendered = state.rendered;
    s_game_energy = state.game_energy;
    s_clip_energy = state.clip_energy;
    s_gain = state.gain;
    s_game_peak = state.game_peak;
    s_on_channel = state.on_channel;
    s_after_channel = state.after_channel;
    s_centred = state.centred;
    s_random = state.random;
}
}  // namespace galaxy::voice
