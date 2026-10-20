#pragma once

#include "galaxy/coop.h"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace galaxy {
namespace agent {

// Agent play testing (docs/AGENT_TESTING.md). With GALAXY_AGENT_DIR set, a test driver (tools/agent) runs the
// game through that folder: it appends commands to commands.txt, one a line ("<id> <verb> <arguments>"), and
// the game answers each in replies.jsonl as one JSON object a line ({"id":..,"ok":..}); status.json is
// rewritten twice a second with the frame, scene and players. The commands run on the game thread, at the
// first player's controller read, once a frame (agent_guest.cpp).

enum class Verb {
    Unknown,
    Ping,        // ping: answered at once
    State,       // state: scene, stage, mission, players
    File,        // file <1-6> [luigi]: loads that save file and starts it, from the logo or file select (no menus)
    Stage,       // stage <StageName> <mission>: into that galaxy and mission (MR::requestChangeStageInGameMoving)
    Wait,        // wait <frames>: answered after that many frames
    Press,       // press <player 1|2> <buttons A+B+Z..> <frames>: held, then released; answered when released
    Stick,       // stick <player> <x> <y> <frames>: the Nunchuk stick (-1..1, up positive)
    Pointer,     // pointer <x> <y> | pointer off: the first player's Wii Remote pointer, -1..1 across the screen (y down)
    Release,     // release <player>: the player's controller back to the person playing
    PowerUp,     // powerup <player> <normal|rainbow|fire|ice|bee|spring|boo|fly> [force]
    Teleport,    // teleport <player> <x> <y> <z>
    Screenshot,  // screenshot <path.ppm>: the next presented frame
    Quit,        // quit: the game exits
};

struct Command {
    std::uint64_t id = 0u;
    Verb verb = Verb::Unknown;
    std::string name;  // the verb as written
    std::vector<std::string> args;
};

// A controller held by the agent: buttons (coop::button::k*), the Nunchuk stick and a shake.
struct Pad {
    bool active = false;
    std::uint32_t buttons = 0u;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    bool shake = false;
    bool has_pointer = false;  // the first player's only
    float pointer_x = 0.0f;
    float pointer_y = 0.0f;
};

[[nodiscard]] inline Verb verb_of(std::string_view name) noexcept {
    if (name == "ping") return Verb::Ping;
    if (name == "state") return Verb::State;
    if (name == "file") return Verb::File;
    if (name == "stage") return Verb::Stage;
    if (name == "wait") return Verb::Wait;
    if (name == "press") return Verb::Press;
    if (name == "stick") return Verb::Stick;
    if (name == "pointer") return Verb::Pointer;
    if (name == "release") return Verb::Release;
    if (name == "powerup") return Verb::PowerUp;
    if (name == "teleport") return Verb::Teleport;
    if (name == "screenshot") return Verb::Screenshot;
    if (name == "quit") return Verb::Quit;
    return Verb::Unknown;
}

// "<id> <verb> <arguments...>", separated by spaces; empty lines and lines starting with '#' are not commands.
[[nodiscard]] inline std::optional<Command> parse_command(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.remove_suffix(1u);
    std::vector<std::string> words;
    std::size_t at = 0u;
    while (at < line.size()) {
        while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
        std::size_t end = at;
        while (end < line.size() && line[end] != ' ' && line[end] != '\t') ++end;
        if (end > at) words.emplace_back(line.substr(at, end - at));
        at = end;
    }
    if (words.size() < 2u || words[0][0] == '#') return std::nullopt;
    Command command;
    const auto parsed = std::from_chars(words[0].data(), words[0].data() + words[0].size(), command.id);
    if (parsed.ec != std::errc{} || parsed.ptr != words[0].data() + words[0].size()) return std::nullopt;
    command.name = words[1];
    command.verb = verb_of(command.name);
    command.args.assign(words.begin() + 2, words.end());
    return command;
}

[[nodiscard]] inline std::optional<long long> parse_int(std::string_view text) noexcept {
    long long value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

[[nodiscard]] inline std::optional<float> parse_float(std::string_view text) noexcept {
    float value = 0.0f;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !(value == value)) return std::nullopt;
    return value;
}

// "1" or "2": the player's index (0 or 1).
[[nodiscard]] inline std::optional<unsigned> parse_player(std::string_view text) noexcept {
    if (text == "1") return 0u;
    if (text == "2") return 1u;
    return std::nullopt;
}

// Wii Remote and Nunchuk buttons joined with '+' ("A+B", "Z", "none"): coop::button masks.
[[nodiscard]] inline std::optional<std::uint32_t> parse_buttons(std::string_view text) noexcept {
    if (text == "none") return 0u;
    std::uint32_t mask = 0u;
    while (!text.empty()) {
        const std::size_t plus = text.find('+');
        const std::string_view name = text.substr(0u, plus);
        namespace b = coop::button;
        std::uint32_t bit = 0u;
        if (name == "A") bit = b::kA;
        else if (name == "B") bit = b::kB;
        else if (name == "1") bit = b::kOne;
        else if (name == "2") bit = b::kTwo;
        else if (name == "PLUS" || name == "+") bit = b::kPlus;
        else if (name == "MINUS" || name == "-") bit = b::kMinus;
        else if (name == "UP") bit = b::kUp;
        else if (name == "DOWN") bit = b::kDown;
        else if (name == "LEFT") bit = b::kLeft;
        else if (name == "RIGHT") bit = b::kRight;
        else if (name == "C") bit = b::kC;
        else if (name == "Z") bit = b::kZ;
        else if (name == "HOME") bit = b::kHome;
        else return std::nullopt;
        mask |= bit;
        if (plus == std::string_view::npos) break;
        text.remove_prefix(plus + 1u);
    }
    return mask;
}

// A power-up form's MarioActor::setPlayerMode mode (MarioAccess.cpp changeItemStatus).
[[nodiscard]] inline std::optional<std::uint16_t> parse_power_up(std::string_view name) noexcept {
    if (name == "normal") return std::uint16_t{0};
    if (name == "rainbow") return std::uint16_t{1};
    if (name == "fire") return std::uint16_t{2};
    if (name == "ice") return std::uint16_t{3};
    if (name == "bee") return std::uint16_t{4};
    if (name == "spring") return std::uint16_t{5};
    if (name == "boo") return std::uint16_t{6};
    if (name == "fly") return std::uint16_t{7};
    return std::nullopt;
}

[[nodiscard]] inline const char* power_up_name(std::uint32_t mode) noexcept {
    constexpr const char* kNames[] = {"normal", "rainbow", "fire", "ice", "bee", "spring", "boo", "fly"};
    return mode < 8u ? kNames[mode] : "other";
}

// Text as a JSON string's contents.
[[nodiscard]] inline std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (u < 0x20u) {
            constexpr char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out.push_back(kHex[u >> 4u]);
            out.push_back(kHex[u & 15u]);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

}  // namespace agent

// The agent's controller for a player (0 or 1), set on the game thread and read by the input thread
// (native_host.cpp for the first player, coop_partner_input for the second); inactive while the agent has
// not taken that controller.
void agent_set_pad(unsigned player, const agent::Pad& pad) noexcept;
[[nodiscard]] agent::Pad agent_pad(unsigned player) noexcept;

}  // namespace galaxy
