#pragma once

// Controller button prompts. Super Mario Galaxy writes its on-screen button
// icons (A, B, C, Z, 1, 2, +, -, D-pad, Remote, Nunchuk) as characters of the
// picture font in LayoutData/Font.arc (picturefont.brfnt), one RGB5A3 cell
// each. While an Xbox or DualSense controller is the one in use, this module
// redraws those cells in guest memory with prompts drawn here (nothing from the
// game is copied), following the player's bindings: the icon the game calls
// "A" shows whichever button is bound to Wii A, and so on. Keyboard and the
// other schemes keep the game's own art, restored from a copy taken before the
// first change.

#include "galaxy/input_bindings.h"
#include "galaxy/native_api.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace galaxy::prompts {

enum class Family : std::uint8_t { None = 0, Xbox, DualSense };

// What a prompt shows. South/East/West/North are the face buttons by position
// (Xbox A/B/X/Y, DualSense cross/circle/square/triangle).
enum class Label : std::uint8_t {
    None = 0,
    South, East, West, North,
    L1, R1, L2, R2, L3, R3,
    Start, Back, Home, Capture,
    Dpad, LeftStick, RightStick, Controller, Digit1, Digit2,
};

// The game's icons that are redrawn.
enum class Slot : std::uint8_t {
    A, B, C, Z, One, Two, Plus, Minus, Home, Dpad, Remote, Nunchuk,
};
inline constexpr std::size_t kSlotCount = 12;
// Glyph (cell) index of each slot in picturefont.brfnt.
inline constexpr std::array<std::uint8_t, kSlotCount> kSlotGlyph{
    1, 2, 3, 26, 6, 7, 25, 24, 23, 20, 4, 5};

struct PromptSet {
    Family family = Family::None;
    std::array<Label, kSlotCount> labels{};
    friend bool operator==(const PromptSet&, const PromptSet&) = default;
};

[[nodiscard]] std::optional<Family> family_for_scheme(input::ControlScheme scheme) noexcept;
// The label of every slot for `family` under `table`: a slot takes its Wii
// target's first controller binding (the B icon follows the jump/back button),
// and the family's default when that target has none.
[[nodiscard]] PromptSet make_prompt_set(Family family, const input::BindingTable& table) noexcept;
[[nodiscard]] const char* label_name(Family family, Label label) noexcept;

// Thread-safe. Family::None restores the game's art.
void publish(const PromptSet& set) noexcept;
[[nodiscard]] PromptSet published() noexcept;

// Glyph cell geometry of the picture font: 34x35 pixels at a 35x36 pitch in
// 128x128 RGB5A3 sheets of 3x3 cells.
inline constexpr int kCellW = 34;
inline constexpr int kCellH = 35;
// Straight-alpha RGBA, kCellW * kCellH * 4 bytes. Needs GDI for the letters.
void render_glyph(Family family, Label label, std::uint8_t* rgba);
// The same prompt drawn at `scale` times the cell (width/height are the result).
void render_glyph_scaled(Family family, Label label, float scale, int& width, int& height,
                         std::vector<std::uint8_t>& rgba);
// A tutorial picture (kind 0 tilt, 1 spin, 2 ball; frame as numbered in the game's
// layouts): a controller held at that pose on a sky, 32 * scale pixels square.
void render_guidance(Family family, int kind, int frame, float scale, int& size, std::vector<std::uint8_t>& rgba);
[[nodiscard]] std::uint16_t encode_rgb5a3(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                          std::uint8_t a) noexcept;

// Simulation thread (called from the site hook dispatcher): finds the picture
// font in guest memory, in small slices, and draws the published prompts into
// it. Cheap when nothing changed.
void tick(GuestMemoryV1* memory, const NativeServicesV1* services, std::uint32_t pc) noexcept;

// HD prompts. The texture cache asks for a replacement whenever it decodes a
// texture: the picture-font sheets that carry the icons and the HUD A icon come
// back kHdScale times larger with the prompts drawn at full resolution (the
// other cells of a sheet are the game's own, enlarged). Render thread; `source`
// is the texture's guest bytes. `key` names the image for the cache and changes
// with the prompts shown.
inline constexpr unsigned kHdScale = 4;
struct HdImage {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> rgba;  // straight alpha
    std::string key;
};
[[nodiscard]] bool hd_replacement(std::uint32_t guest_address, std::uint32_t width, std::uint32_t height,
                                  std::uint8_t gx_format, const std::uint8_t* source, std::uint32_t source_bytes,
                                  HdImage& out);

}  // namespace galaxy::prompts
