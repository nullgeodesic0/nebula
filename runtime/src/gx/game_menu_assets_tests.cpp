// The host menus' disc reader (game_disc.h) and font parser (game_font.h). The disc table is tested on a
// synthetic image; the real font is checked when the user's game.pak is at hand (GALAXY_TEST_GAME_PAK, or
// C:\nb\content-mp\game.pak), and a line of text drawn with it is written to GALAXY_TEST_FONT_PGM if set.

#include "galaxy/gx/game_disc.h"
#include "galaxy/gx/game_font.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

bool expect(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

void put32(std::vector<std::byte>& d, std::size_t o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) d[o + i] = static_cast<std::byte>(v >> (24 - 8 * i));
}

std::string env(const char* name) {
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
    std::string out(value);
    std::free(value);
    return out;
}

}  // namespace

int main() {
    bool ok = true;
    using namespace galaxy::gx;

    {
        // root (3 entries) > dir "LayoutData" > file "Font.arc" (4 bytes "ABCD" at 0x800).
        std::vector<std::byte> image(0x1000);
        const std::size_t table = 0x600;
        put32(image, 0x424, table >> 2);
        put32(image, 0x428, 0x100 >> 2);
        put32(image, table + 0, 0x01000000u);
        put32(image, table + 8, 3u);
        put32(image, table + 12, 0x01000000u | 0u);  // dir, name 0
        put32(image, table + 16, 0u);
        put32(image, table + 20, 3u);
        put32(image, table + 24, 11u);  // file, name 11
        put32(image, table + 28, 0x800 >> 2);
        put32(image, table + 32, 4u);
        const char names[] = "LayoutData\0Font.arc";
        std::memcpy(image.data() + table + 36, names, sizeof(names));
        std::memcpy(image.data() + 0x800, "ABCD", 4);
        auto disc = GameDisc::from_image(std::move(image));
        ok &= expect(disc != nullptr && disc->file_count() == 1u, "the disc table reads");
        const auto file = disc ? disc->read("layoutdata\\FONT.ARC") : std::nullopt;
        ok &= expect(file && file->size() == 4u && std::memcmp(file->data(), "ABCD", 4) == 0,
                     "a file reads by its path, in any case and either slash");
        ok &= expect(disc && !disc->read("LayoutData/Nope.arc"), "a missing file is absent");
    }

    ok &= expect(utf8_to_utf16("Luigi \xC3\xA9") == u"Luigi \u00E9", "UTF-8 to UTF-16");

    std::string pak = env("GALAXY_TEST_GAME_PAK");
    if (pak.empty() && std::filesystem::exists("C:\\nb\\content-mp\\game.pak")) pak = "C:\\nb\\content-mp\\game.pak";
    if (pak.empty()) {
        std::printf("SKIP: no game.pak for the real font\n");
    } else {
        auto disc = GameDisc::open(pak);
        ok &= expect(disc != nullptr && disc->file_count() > 1000u, "game.pak's table reads");
        const auto bytes = disc ? disc->read_arc_entry("LayoutData/Font.arc", "messagefont26.brfnt") : std::nullopt;
        ok &= expect(bytes.has_value(), "MessageFont26 is in Font.arc");
        const auto font = bytes ? parse_brfnt(*bytes) : std::nullopt;
        ok &= expect(font && font->cell_width == 26 && font->cell_height == 32 && font->line_feed == 33 &&
                         font->ascent == 26 && font->sheets.size() == 5u,
                     "MessageFont26: 26x32 cells, line feed 33, ascent 26, 5 sheets");
        ok &= expect(font && font->has(u'A') && font->has(u'g') && font->has(u'\u00E9') && font->glyph(u'A').advance > 8,
                     "it has Latin letters with widths");
        const std::string out = env("GALAXY_TEST_FONT_PGM");
        if (font && !out.empty()) {
            // The coverage of a line drawn at the font's size, to look at.
            const std::u16string text = u"Luigi joins! Start to rejoin. \u00C9t\u00E9 Accept / Deny";
            const int w = font->measure(text) + 8, h = font->line_feed + 8;
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h, 0u);
            int pen = 4;
            for (const char16_t c : text) {
                const FontGlyph g = font->glyph(c);
                const DecodedTexture& sheet = font->sheets[g.sheet];
                for (int y = 0; y < font->cell_height; ++y) {
                    for (int x = 0; x < g.glyph_width; ++x) {
                        const int sx = g.x + x, sy = g.y + y;
                        const int dx = pen + g.left + x, dy = 4 + y;
                        if (dx < 0 || dx >= w || dy >= h) continue;
                        const std::uint8_t a = sheet.rgba8[(static_cast<std::size_t>(sy) * sheet.width + sx) * 4u + 3u];
                        pixels[static_cast<std::size_t>(dy) * w + dx] = std::max(pixels[static_cast<std::size_t>(dy) * w + dx], a);
                    }
                }
                pen += g.advance;
            }
            std::ofstream pgm(out, std::ios::binary);
            pgm << "P5\n" << w << ' ' << h << "\n255\n";
            pgm.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
            std::printf("wrote %s\n", out.c_str());
        }
    }

    std::printf("%s\n", ok ? "game menu assets: all passed" : "game menu assets: FAILED");
    return ok ? 0 : 1;
}
