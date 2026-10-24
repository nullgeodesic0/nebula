// galaxy::menu_sounds against the user's disc (GALAXY_TEST_GAME_PAK, or C:\nb\content-mp-nomods\game.pak or
// C:\nb\content-mp\game.pak). Skips when no game.pak is at hand.
//
// The expected numbers are the game's own: its output captured from the runtime (the native DSP ucode,
// GALAXY_AUDIO_WAV_DUMP) while MR::startSystemSE played each sound with every other category muted. For each
// sound the span from the first to the last sample above 8 and the peak sample are exactly the capture's.

#include "galaxy/gx/game_disc.h"
#include "galaxy/menu_sounds.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

bool expect(bool ok, const std::string& what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what.c_str());
    return ok;
}

std::string env(const char* name) {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
    std::string out(value);
    std::free(value);
    return out;
}

struct Expected {
    galaxy::menu_sounds::Sound sound;
    std::uint32_t active_frames;  // first to last frame with a sample above 8 in magnitude
    int peak;
};

// From the capture (32 kHz stereo; the game's DSP output for the sound alone).
constexpr Expected kExpected[] = {
    {galaxy::menu_sounds::Sound::Cursor, 19037u, 6847},
    {galaxy::menu_sounds::Sound::Decide, 45276u, 13864},
    {galaxy::menu_sounds::Sound::Cancel, 36201u, 5962},
    {galaxy::menu_sounds::Sound::PauseOn, 41988u, 17438},
    {galaxy::menu_sounds::Sound::PauseOff, 42425u, 19418},
    {galaxy::menu_sounds::Sound::WindowOpen, 27806u, 4582},
    {galaxy::menu_sounds::Sound::WindowClose, 25191u, 4356},
    {galaxy::menu_sounds::Sound::Yes, 31502u, 14004},
    {galaxy::menu_sounds::Sound::No, 30588u, 14285},
    {galaxy::menu_sounds::Sound::Ok, 27509u, 11376},
    {galaxy::menu_sounds::Sound::TalkFocus, 19037u, 6847},
};

}  // namespace

int main() {
    using namespace galaxy::menu_sounds;
    bool ok = true;
    std::string pak = env("GALAXY_TEST_GAME_PAK");
    for (const char* candidate : {"C:\\nb\\content-mp-nomods\\game.pak", "C:\\nb\\content-mp\\game.pak"}) {
        if (pak.empty() && std::filesystem::exists(candidate)) pak = candidate;
    }

    ok &= expect(!loaded(), "nothing is loaded before load()");
    play(Sound::Cursor);  // before load(): a no-op
    ok &= expect(rendered(Sound::Cursor).empty(), "no PCM before load()");

    if (pak.empty()) {
        std::printf("SKIP: no game.pak for the sounds\n");
        return ok ? 0 : 1;
    }
    auto disc = galaxy::gx::GameDisc::open(pak);
    ok &= expect(disc != nullptr, "game.pak opens");
    if (disc == nullptr) return 1;

    std::string error;
    auto renderer = SeRenderer::create(*disc, &error);
    ok &= expect(renderer != nullptr, "the sound archive, banks, wave tables and curve tables read " + error);
    if (renderer == nullptr) return 1;
    ok &= expect(renderer->system_sound_names().size() > 200u &&
                     renderer->system_sound_names()[7] == "SE_SY_CURSOR_1",
                 "the system sound names read in id order");

    for (const Expected& e : kExpected) {
        const std::string name = sound_name(e.sound);
        const Rendered r = renderer->render(name, RenderOptions{});
        ok &= expect(r.ok && r.error.empty(), name + " renders without random, oscillator or filter-table data " +
                                                 r.error);
        std::uint32_t first = r.frames, last = 0;
        int peak = 0;
        for (std::uint32_t i = 0; i < r.frames; ++i) {
            const int m = std::max(std::abs(static_cast<int>(r.pcm[i * 2u])), std::abs(static_cast<int>(r.pcm[i * 2u + 1u])));
            peak = std::max(peak, m);
            if (m > 8) {
                first = std::min(first, i);
                last = i;
            }
        }
        const std::uint32_t active = first <= last ? last - first + 1u : 0u;
        ok &= expect(active == e.active_frames && peak == e.peak,
                     name + " matches the game's length and peak (" + std::to_string(active) + " frames, peak " +
                         std::to_string(peak) + "; game " + std::to_string(e.active_frames) + ", " +
                         std::to_string(e.peak) + ")");
        const Rendered again = renderer->render(name, RenderOptions{});
        ok &= expect(again.pcm == r.pcm, name + " renders the same every time");
    }

    {
        // The trace names what ran; the menu sounds use the DSP's auto mixer (bus 0 config 0xFFFF).
        RenderOptions options;
        options.trace = true;
        const Rendered r = renderer->render(sound_name(Sound::Cursor), options);
        ok &= expect(r.trace.find("cmd EA 0 FFFF") != std::string::npos && r.trace.find("auto 3F00") != std::string::npos,
                     "the cursor sound mixes centred through the auto mixer");
    }
    ok &= expect(!renderer->render("SE_SY_NO_SUCH_SOUND", RenderOptions{}).ok, "an unknown name is refused");

    ok &= expect(load(*disc), "load() renders every menu sound");
    ok &= expect(loaded(), "loaded() after load()");
    for (const Expected& e : kExpected) {
        const std::vector<float> pcm = rendered(e.sound);
        float peak = 0.0f;
        for (const float s : pcm) peak = std::max(peak, std::fabs(s));
        ok &= expect(pcm.size() % kChannels == 0u && std::fabs(peak - static_cast<float>(e.peak) / 32768.0f) < 1e-6f,
                     std::string(sound_name(e.sound)) + " is loaded as float stereo");
    }
    // play() only queues a buffer: it must return at once, from any thread, many times over.
    double longest_ms = 0.0;
    for (int i = 0; i < 40; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        play(static_cast<Sound>(i % static_cast<int>(Sound::Count)), 0.0f);
        longest_ms = std::max(longest_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
    }
    ok &= expect(longest_ms < 20.0, "play() does not block (longest " + std::to_string(longest_ms) + " ms)");
    if (has_output()) {
        // Played at volume 0 above: every voice is busy (the oldest are reused), and they finish on their own.
        ok &= expect(voices_playing() == 8u, "the plays are queued on the private voices");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (voices_playing() != 0u && std::chrono::steady_clock::now() < deadline) {
        }
        ok &= expect(voices_playing() == 0u, "the voices end with their sounds");
    } else {
        std::printf("SKIP: no audio device for the playback checks\n");
    }
    shutdown();
    ok &= expect(!loaded() && rendered(Sound::Cursor).empty(), "shutdown() releases the sounds");
    play(Sound::Cursor);
    return ok ? 0 : 1;
}
