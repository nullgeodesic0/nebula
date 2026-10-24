#pragma once

#include "galaxy/motion_style.h"

#include <array>
#include <filesystem>
#include <cstdint>

namespace galaxy {

enum class RuntimeInputMode : std::uint32_t {
    Auto = 0,
    KeyboardMouse = 1,
    Controller = 2,
    RealWiimote = 3,
};

struct RuntimeAudioSettings {
    float master = 1.0f;
    float music = 1.0f;
    float sfx = 1.0f;
    float voice = 1.0f;
    float ambience = 1.0f;
    // In-engine cutscene sounds (the game's DEMO sound category) and the
    // pre-rendered movies' audio.
    float cutscene = 1.0f;
    // What the game plays on the Wii Remote speaker, mixed into the output.
    // Off unless chosen, also for files written before the key existed.
    float wiimote_speaker = 0.0f;
};

// Auto: keyboard, mouse and every controller are live together and the pointer
// follows whichever moved last. KeyboardMouse and Controller are overrides that
// keep only that source.
// The game's language: the three the USA disc carries. Values are the Wii's SC language codes, which
// SCGetLanguage returns.
enum class GameLanguage : std::uint8_t { English = 1, French = 3, Spanish = 4 };

// Which controller the game's button prompts (A, B, Z, +, ...) show: the one in use (Auto: Xbox or
// PlayStation while one is, the Wii Remote's otherwise) or always one family.
enum class ButtonPrompts : std::uint8_t { Auto = 0, Wii = 1, Xbox = 2, DualSense = 3 };

// The Odyssey moveset (odyssey_moveset.h): roll, roll boost, roll jump, ground-pound jump and roll, and dive on top of
// Galaxy's own moves. Off by default; each move can be switched off on its own in runtime_settings.ini.
struct OdysseyMovesetSettings {
    bool enabled = false;
    bool roll = true;
    bool roll_jump = true;
    bool ground_pound_jump = true;
    bool dive = true;
    bool turn_boost = true;
    bool second_player = true;  // co-op: the second player has the moveset too
    friend bool operator==(const OdysseyMovesetSettings&, const OdysseyMovesetSettings&) = default;
};

struct RuntimeSettings {
    RuntimeInputMode input_mode = RuntimeInputMode::Auto;
    RuntimeAudioSettings audio{};
    // Steps of dialogue text revealed per frame: 1 the game's pace, 2 or 3; 4 each page at once (instant).
    unsigned text_speed = 1u;
    // The language to start the game in. A change takes effect at the next start.
    GameLanguage language = GameLanguage::English;
    // Discord shows what the player is doing (galaxy, star, Power Stars) while Discord runs.
    bool discord_status = true;
    ButtonPrompts button_prompts = ButtonPrompts::Auto;
    // Animation smoothing and Mario's movement feel (motion_style.h). Changes live.
    MotionStyle motion_style = MotionStyle::Original;
    // Mario's animations from the user's converted mod (content\mods\animations, built by
    // tools/animations/build_mod.py) instead of the game's own. The archive is part of the disc, so a
    // change takes effect at the next start; without the mod folder this does nothing.
    bool custom_animations = true;
    // Mario's model from the user's converted mod (content\mods\model, built by
    // tools/animations/build_model_mod.py). Like the animations, a change takes effect at the next start.
    bool custom_model = true;
    // Mario's voice lines from the user's clips (content\mods\voice, built by
    // tools/animations/build_voice_mod.py). Changes live.
    bool custom_voice = true;
    // The Odyssey moveset. Changes live.
    OdysseyMovesetSettings moveset{};
    // Mario and Luigi co-op on one screen (docs/MULTIPLAYER.md): a second player with the second controller.
    // The second character's model loads with the first's, so a change takes effect at the next start.
    bool coop = false;
    // Online co-op (docs/NETPLAY.md, F1 > Online): whether another PC may join this game, the UDP port it
    // listens on, and the address this PC last joined.
    bool net_joinable = false;
    unsigned net_port = 7777u;
    std::array<char, 64> net_join_address{};
    // Which device plays which brother on this PC (player_devices.h Router::format_seats), chosen in the Players
    // panel: `xbox.0=second,keyboard.0=first`. Empty: every seat automatic.
    std::array<char, 160> player_devices{};
};

// The language the game runs in this session: the setting when the game first asks (at boot), then
// fixed, so a change made while playing never mixes two languages.
[[nodiscard]] GameLanguage game_language_for_boot() noexcept;
// Whether the game has asked yet, and if so, its language.
[[nodiscard]] bool running_game_language(GameLanguage& language) noexcept;
// "english", "french" or "spanish" (runtime_settings.ini).
[[nodiscard]] const char* game_language_key(GameLanguage language) noexcept;
// "auto", "wii", "xbox" or "dualsense" (runtime_settings.ini `button_prompts`).
[[nodiscard]] const char* button_prompts_key(ButtonPrompts prompts) noexcept;
// "original", "smooth" or "modern" (runtime_settings.ini).
[[nodiscard]] const char* motion_style_key(MotionStyle style) noexcept;

// Whether this session uses the custom animations: the setting when the disc first opens (read from the
// settings file then, since that is before the rest of the settings load), then fixed.
[[nodiscard]] bool custom_animations_for_boot() noexcept;
// Whether the disc has opened yet, and if so, the value it opened with.
[[nodiscard]] bool running_custom_animations(bool& enabled) noexcept;
// The same two for the custom model.
[[nodiscard]] bool custom_model_for_boot() noexcept;
[[nodiscard]] bool running_custom_model(bool& enabled) noexcept;
// The same for co-op: the setting when the game first asks (as the player's archives load), then fixed.
[[nodiscard]] bool coop_for_boot() noexcept;
// Co-op for the game being played: coop_for_boot() until the save-select screen first shows, then the setting as
// it stands while that screen is up (its Multiplayer switch, coop_guest.cpp), fixed again once a save is chosen.
[[nodiscard]] bool coop_enabled() noexcept;
void set_coop_session(bool enabled) noexcept;
// Local hot join: the co-op machinery (the second brother made in every stage, absent until a second controller
// drops in) runs whatever the co-op setting, which then only says whether a save starts with two players.
// GALAXY_COOP_HOT_JOIN=0 turns it off (co-op only with the setting, as before).
[[nodiscard]] bool coop_hot_join() noexcept;
// Whether the co-op machinery runs: co-op on, or hot join.
[[nodiscard]] bool coop_available() noexcept;
// Whether the save-select screen is up, for its Multiplayer / Odyssey moves panel (renderer_d3d12.cpp).
void set_file_select_options_shown(bool shown) noexcept;
[[nodiscard]] bool file_select_options_shown() noexcept;

// A coherent input-source snapshot. The generation changes only when the selected input mode changes,
// so device FIFOs can reject reports acquired under an older source. Unrelated audio-setting writes are
// not treated as an input transition.
struct RuntimeInputModeState {
    RuntimeInputMode mode = RuntimeInputMode::Auto;
    std::uint64_t generation = 1u;
};

[[nodiscard]] RuntimeSettings get_runtime_settings();
[[nodiscard]] RuntimeInputModeState get_runtime_input_mode_state();
void set_runtime_settings(const RuntimeSettings& settings);
// Missing files keep the defaults. Invalid explicit paths and read failures throw before any setting is
// published. Gains use the C numeric locale and complete finite tokens. A malformed value keeps its prior
// or default gain.
void load_runtime_settings_from_file();
// Re-reads only the audio gains when the settings file's last-write time changes, for example when an
// external editor or NebulaSettings.exe writes during play. Read or parse failures keep the current gains.
// Single caller thread.
bool reload_runtime_audio_settings_if_changed() noexcept;
// Invalid explicit paths and formatting/write/replace failures return false;
// a bad override never redirects a save to the normal user settings file.
[[nodiscard]] bool save_runtime_settings_to_file();
// The folder of the settings file (GALAXY_RUNTIME_SETTINGS_PATH's, else Nebula's data folder); empty when unknown.
[[nodiscard]] std::filesystem::path runtime_settings_folder() noexcept;

[[nodiscard]] const char* runtime_input_mode_name(RuntimeInputMode mode);

}  // namespace galaxy
