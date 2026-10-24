// Not a test: renders the game's system sounds with galaxy::menu_sounds and writes each to a 16-bit stereo
// 32 kHz WAV (and, with --trace, the executed sequence and channel events to a .txt beside it), for listening
// and for comparison with the game's own output. The WAVs come from the user's disc: write them outside the
// repository.
//
//   galaxy_menu_sounds_dump <game.pak> <out-dir> [--trace] [--scale <volume-scale>] [--mute <track mask>] [--all | NAME...]
//
// With no names it renders the sounds menu_sounds::Sound maps plus the other menu candidates.

#include "galaxy/gx/game_disc.h"
#include "galaxy/menu_sounds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

void write_wav(const std::filesystem::path& path, const std::vector<std::int16_t>& pcm) {
    std::ofstream f(path, std::ios::binary);
    const auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    const auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const std::uint32_t bytes = static_cast<std::uint32_t>(pcm.size() * 2u);
    f.write("RIFF", 4);
    u32(36u + bytes);
    f.write("WAVEfmt ", 8);
    u32(16u);
    u16(1u);
    u16(2u);
    u32(galaxy::menu_sounds::kSampleRate);
    u32(galaxy::menu_sounds::kSampleRate * 4u);
    u16(4u);
    u16(16u);
    f.write("data", 4);
    u32(bytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(bytes));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <game.pak> <out-dir> [--trace] [--scale s] [--all | NAME...]\n", argv[0]);
        return 2;
    }
    auto disc = galaxy::gx::GameDisc::open(argv[1]);
    if (disc == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::string error;
    auto renderer = galaxy::menu_sounds::SeRenderer::create(*disc, &error);
    if (renderer == nullptr) {
        std::fprintf(stderr, "cannot read the sound archive: %s\n", error.c_str());
        return 1;
    }
    const std::filesystem::path out = argv[2];
    std::filesystem::create_directories(out);
    galaxy::menu_sounds::RenderOptions options;
    std::vector<std::string> names;
    for (int i = 3; i < argc; ++i) {
        if (std::strcmp(argv[i], "--trace") == 0) {
            options.trace = true;
        } else if (std::strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            options.sound_volume_scale = static_cast<float>(std::atof(argv[++i]));
        } else if (std::strcmp(argv[i], "--mute") == 0 && i + 1 < argc) {
            options.mute_tracks = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--dsp-channels") == 0 && i + 1 < argc) {
            options.dsp_channels = static_cast<std::uint32_t>(std::strtoul(argv[++i], nullptr, 0));
        } else if (std::strcmp(argv[i], "--all") == 0) {
            names = renderer->system_sound_names();
        } else {
            names.emplace_back(argv[i]);
        }
    }
    if (names.empty()) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(galaxy::menu_sounds::Sound::Count); ++i) {
            names.emplace_back(galaxy::menu_sounds::sound_name(static_cast<galaxy::menu_sounds::Sound>(i)));
        }
        for (const char* extra : {"SE_SY_CURSOR_1", "SE_SY_DECIDE_1", "SE_SY_CANCEL_1", "SE_SY_SELECT_PAUSE_ITEM"}) {
            names.emplace_back(extra);
        }
    }
    int failures = 0;
    for (const std::string& name : names) {
        const galaxy::menu_sounds::Rendered r = renderer->render(name, options);
        int peak = 0;
        double energy = 0.0;
        for (const std::int16_t s : r.pcm) {
            peak = std::max(peak, std::abs(static_cast<int>(s)));
            energy += static_cast<double>(s) * s;
        }
        const double rms = r.pcm.empty() ? 0.0 : std::sqrt(energy / static_cast<double>(r.pcm.size()));
        std::printf("%-32s %s frames=%6u (%.3f s) peak=%5d rms=%7.1f %s\n", name.c_str(), r.ok ? "ok  " : "FAIL",
                    r.frames, r.frames / static_cast<double>(galaxy::menu_sounds::kSampleRate), peak, rms,
                    r.error.c_str());
        if (!r.ok) {
            ++failures;
            continue;
        }
        write_wav(out / (name + ".wav"), r.pcm);
        if (options.trace) std::ofstream(out / (name + ".txt")) << r.trace;
    }
    return failures == 0 ? 0 : 1;
}
