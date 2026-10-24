#pragma once

// The game's own fonts (NW4R BRFNT, LayoutData/Font.arc on the disc) for the host-side menus: the glyph sheets
// decoded to RGBA8 and every character's sheet cell and widths, so our renderer draws text exactly as the game's
// layouts do (MessageFont26 for the pause menu, the file select and the dialogs).

#include "galaxy/gx/game_asset_extract.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace galaxy::gx {

struct FontGlyph {
    std::uint16_t sheet = 0;
    // The cell's top-left in its sheet, in texels.
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    // NW4R CharWidths: where the glyph's ink starts in the cell, its ink width, and the pen advance.
    std::int8_t left = 0;
    std::uint8_t glyph_width = 0;
    std::int8_t advance = 0;
};

struct GameFont {
    // FINF
    int line_feed = 0;
    int ascent = 0;
    int height = 0;
    int width = 0;
    // TGLP
    int cell_width = 0;
    int cell_height = 0;
    int baseline = 0;
    std::uint32_t sheet_format = 0;
    std::vector<DecodedTexture> sheets;
    std::uint16_t alternate_index = 0;

    // The glyph for a UTF-16 code unit (the alternate glyph, normally '?', when the font lacks it).
    [[nodiscard]] FontGlyph glyph(char16_t code) const noexcept;
    [[nodiscard]] bool has(char16_t code) const noexcept { return map.contains(code); }
    // A line's pen advance at the font's own size.
    [[nodiscard]] int measure(std::u16string_view text) const noexcept;

    std::unordered_map<char16_t, std::uint16_t> map;   // code -> glyph index
    std::vector<FontGlyph> glyphs;                     // by glyph index
};

// Parses a BRFNT file (version 0x0104, uncompressed sheets of formats I4, IA4 or RGB5A3).
[[nodiscard]] std::optional<GameFont> parse_brfnt(std::span<const std::byte> data) noexcept;

// UTF-8 to the UTF-16 the fonts are keyed by.
[[nodiscard]] std::u16string utf8_to_utf16(std::string_view text);

}  // namespace galaxy::gx
