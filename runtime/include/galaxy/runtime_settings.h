// Player settings in runtime_settings.ini: input mode, audio gains, text speed, language, Discord status.
#pragma once

#include <filesystem>
#include <cstdint>

namespace galaxy {

// Auto: keyboard, mouse and every controller are live together and the pointer follows whichever moved
// last. KeyboardMouse and Controller keep only that source.
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
    // In-engine cutscene sounds (the game's DEMO sound category) and the movies' audio.
    float cutscene = 1.0f;
    // What the game plays on the Wii Remote speaker, mixed into the output; off unless chosen.
    float wiimote_speaker = 0.0f;
};

// The three languages of the USA disc, as the Wii's SC language codes SCGetLanguage returns.
enum class GameLanguage : std::uint8_t { English = 1, French = 3, Spanish = 4 };

// Which controller the game's button prompts (A, B, Z, +, ...) show: the one in use (Auto: Xbox or
// PlayStation while one is, the Wii Remote's otherwise) or always one family.
enum class ButtonPrompts : std::uint8_t { Auto = 0, Wii = 1, Xbox = 2, DualSense = 3 };

// The Odyssey moveset (nebula_odyssey_moveset.h): roll, roll boost, roll jump, ground-pound jump and roll, and dive
// on top of Galaxy's own moves. Off by default; each move can be switched off on its own in runtime_settings.ini.
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
    // Takes effect at the next start.
    GameLanguage language = GameLanguage::English;
    // Discord shows what the player is doing (galaxy, star, Power Stars) while Discord runs.
    bool discord_status = true;
    ButtonPrompts button_prompts = ButtonPrompts::Auto;
    // Mario's voice lines from the user's clips (the mods folder's voice mod, built by
    // tools/animations/build_voice_mod.py). Changes live.
    bool custom_voice = true;
    // The Odyssey moves. Changes live.
    OdysseyMovesetSettings moveset{};
    // Animation smoothing and Mario's movement feel (nebula_motion_style.h): 0 original, 1 smooth, 2 modern.
    // Changes live.
    unsigned motion_style = 0u;
};

// The language the game runs in this session: the setting when the game first asks (at boot), then
// fixed, so a change made while playing never mixes two languages.
[[nodiscard]] GameLanguage game_language_for_boot() noexcept;
// Whether the game has asked yet, and if so, its language.
[[nodiscard]] bool running_game_language(GameLanguage& language) noexcept;
// "english", "french" or "spanish" (runtime_settings.ini).
[[nodiscard]] const char* game_language_key(GameLanguage language) noexcept;
// "original", "smooth" or "modern" (runtime_settings.ini `motion_style`).
[[nodiscard]] const char* motion_style_key(unsigned style) noexcept;
// "auto", "wii", "xbox" or "dualsense" (runtime_settings.ini `button_prompts`).
[[nodiscard]] const char* button_prompts_key(ButtonPrompts prompts) noexcept;

// The generation changes only when the input mode changes, so device FIFOs can reject reports acquired
// under an older source.
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
// Returns false on an invalid explicit path or a failed write; a bad override never redirects the save to
// the normal settings file.
[[nodiscard]] bool save_runtime_settings_to_file();
// The folder of the settings file (GALAXY_RUNTIME_SETTINGS_PATH's, else Nebula's data folder); empty when unknown.
[[nodiscard]] std::filesystem::path runtime_settings_folder() noexcept;

[[nodiscard]] const char* runtime_input_mode_name(RuntimeInputMode mode);

}  // namespace galaxy
