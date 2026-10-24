#include "galaxy/gx/game_font.h"

#include <cstring>

namespace galaxy::gx {

namespace {

std::uint8_t u8(std::span<const std::byte> d, std::size_t o) { return static_cast<std::uint8_t>(d[o]); }
std::int8_t s8(std::span<const std::byte> d, std::size_t o) { return static_cast<std::int8_t>(d[o]); }
std::uint16_t u16(std::span<const std::byte> d, std::size_t o) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(d[o]) << 8u) | static_cast<unsigned>(d[o + 1u]));
}
std::uint32_t u32(std::span<const std::byte> d, std::size_t o) {
    return (static_cast<std::uint32_t>(u16(d, o)) << 16u) | u16(d, o + 2u);
}

constexpr std::uint16_t kNoGlyph = 0xFFFFu;

}  // namespace

FontGlyph GameFont::glyph(char16_t code) const noexcept {
    const auto it = map.find(code);
    const std::uint16_t index = it != map.end() ? it->second : alternate_index;
    return index < glyphs.size() ? glyphs[index] : FontGlyph{};
}

int GameFont::measure(std::u16string_view text) const noexcept {
    int pen = 0;
    for (const char16_t c : text) pen += glyph(c).advance;
    return pen;
}

std::optional<GameFont> parse_brfnt(std::span<const std::byte> d) noexcept try {
    // Sections are read with their 8-byte headers; the FINF pointers point past those headers.
    if (d.size() < 0x30u || std::memcmp(d.data(), "RFNT", 4u) != 0) return std::nullopt;
    const std::size_t finf = u16(d, 0x0Cu);
    if (finf + 0x20u > d.size() || std::memcmp(d.data() + finf, "FINF", 4u) != 0) return std::nullopt;
    GameFont font;
    font.line_feed = s8(d, finf + 9u);
    font.alternate_index = u16(d, finf + 10u);
    const FontGlyph default_width{0, 0, 0, s8(d, finf + 12u), u8(d, finf + 13u), s8(d, finf + 14u)};
    font.height = u8(d, finf + 28u);
    font.width = u8(d, finf + 29u);
    font.ascent = u8(d, finf + 30u);

    const auto section = [&](std::uint32_t pointer, const char* magic) -> std::optional<std::size_t> {
        if (pointer < 8u || pointer - 8u + 16u > d.size()) return std::nullopt;
        const std::size_t at = pointer - 8u;
        if (std::memcmp(d.data() + at, magic, 4u) != 0) return std::nullopt;
        return at;
    };

    const auto tglp = section(u32(d, finf + 16u), "TGLP");
    if (!tglp || *tglp + 32u > d.size()) return std::nullopt;
    font.cell_width = u8(d, *tglp + 8u);
    font.cell_height = u8(d, *tglp + 9u);
    font.baseline = s8(d, *tglp + 10u);
    const std::uint32_t sheet_size = u32(d, *tglp + 12u);
    const unsigned sheet_count = u16(d, *tglp + 16u);
    const std::uint16_t format = u16(d, *tglp + 18u);
    const unsigned per_row = u16(d, *tglp + 20u);
    const unsigned rows = u16(d, *tglp + 22u);
    const unsigned sheet_width = u16(d, *tglp + 24u);
    const unsigned sheet_height = u16(d, *tglp + 26u);
    const std::size_t image = u32(d, *tglp + 28u);
    if ((format & 0x8000u) != 0u || sheet_count == 0u || sheet_count > 256u || per_row == 0u || rows == 0u) {
        return std::nullopt;  // compressed sheets: not used by this game's fonts
    }
    font.sheet_format = format;
    for (unsigned sheet = 0; sheet < sheet_count; ++sheet) {
        auto decoded = decode_gx_texels(format, d, image + static_cast<std::size_t>(sheet) * sheet_size, sheet_width,
                                        sheet_height);
        if (!decoded) return std::nullopt;
        font.sheets.push_back(std::move(*decoded));
    }

    // Every glyph index's cell: cells are laid out row by row with a 1-texel gap.
    const unsigned per_sheet = per_row * rows;
    const unsigned glyph_count = per_sheet * sheet_count;
    font.glyphs.resize(glyph_count);
    for (unsigned index = 0; index < glyph_count; ++index) {
        FontGlyph& g = font.glyphs[index];
        g = default_width;
        g.sheet = static_cast<std::uint16_t>(index / per_sheet);
        const unsigned cell = index % per_sheet;
        g.x = static_cast<std::uint16_t>((cell % per_row) * (font.cell_width + 1u));
        g.y = static_cast<std::uint16_t>((cell / per_row) * (font.cell_height + 1u));
    }

    for (auto cwdh = section(u32(d, finf + 20u), "CWDH"); cwdh;) {
        const unsigned begin = u16(d, *cwdh + 8u);
        const unsigned end = u16(d, *cwdh + 10u);
        for (unsigned index = begin; index <= end && index < glyph_count; ++index) {
            const std::size_t at = *cwdh + 16u + static_cast<std::size_t>(index - begin) * 3u;
            if (at + 3u > d.size()) break;
            FontGlyph& g = font.glyphs[index];
            g.left = s8(d, at);
            g.glyph_width = u8(d, at + 1u);
            g.advance = s8(d, at + 2u);
        }
        const std::uint32_t next = u32(d, *cwdh + 12u);
        cwdh = next != 0u ? section(next, "CWDH") : std::nullopt;
    }

    for (auto cmap = section(u32(d, finf + 24u), "CMAP"); cmap;) {
        const unsigned first = u16(d, *cmap + 8u);
        const unsigned last = u16(d, *cmap + 10u);
        const unsigned method = u16(d, *cmap + 12u);
        const std::size_t info = *cmap + 20u;
        if (method == 0u) {
            const unsigned offset = u16(d, info);
            for (unsigned code = first; code <= last; ++code) {
                font.map[static_cast<char16_t>(code)] = static_cast<std::uint16_t>(code - first + offset);
            }
        } else if (method == 1u) {
            for (unsigned code = first; code <= last; ++code) {
                const std::size_t at = info + static_cast<std::size_t>(code - first) * 2u;
                if (at + 2u > d.size()) break;
                const std::uint16_t index = u16(d, at);
                if (index != kNoGlyph) font.map[static_cast<char16_t>(code)] = index;
            }
        } else if (method == 2u) {
            const unsigned count = u16(d, info);
            for (unsigned i = 0; i < count; ++i) {
                const std::size_t at = info + 2u + static_cast<std::size_t>(i) * 4u;
                if (at + 4u > d.size()) break;
                font.map[static_cast<char16_t>(u16(d, at))] = u16(d, at + 2u);
            }
        }
        const std::uint32_t next = u32(d, *cmap + 16u);
        cmap = next != 0u ? section(next, "CMAP") : std::nullopt;
    }
    std::erase_if(font.map, [&](const auto& entry) { return entry.second >= glyph_count; });
    if (font.map.empty()) return std::nullopt;
    return font;
} catch (...) {
    return std::nullopt;
}

std::u16string utf8_to_utf16(std::string_view text) {
    std::u16string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        char32_t code = c;
        std::size_t length = 1u;
        if (c >= 0xF0u) { code = c & 0x07u; length = 4u; }
        else if (c >= 0xE0u) { code = c & 0x0Fu; length = 3u; }
        else if (c >= 0xC0u) { code = c & 0x1Fu; length = 2u; }
        if (i + length > text.size()) break;
        for (std::size_t k = 1; k < length; ++k) code = (code << 6u) | (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
        i += length;
        if (code >= 0x10000u) {
            code -= 0x10000u;
            out.push_back(static_cast<char16_t>(0xD800u + (code >> 10u)));
            out.push_back(static_cast<char16_t>(0xDC00u + (code & 0x3FFu)));
        } else {
            out.push_back(static_cast<char16_t>(code));
        }
    }
    return out;
}

}  // namespace galaxy::gx
