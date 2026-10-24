#include "galaxy/gx/menu_glyphs.h"

#include "galaxy/button_prompts.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace galaxy::gx::menu {

namespace {

struct Piece {
    GlyphRef ref;
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

}  // namespace

GlyphAtlasImage build_glyph_atlas(const GameFont* picture_font, float scale) {
    std::vector<Piece> pieces;
    // Every pad prompt (any button can be bound to a Wii target), and every Wii button the game draws.
    for (const GlyphFamily family : {GlyphFamily::Xbox, GlyphFamily::DualSense}) {
        const auto prompt_family = family == GlyphFamily::Xbox ? prompts::Family::Xbox : prompts::Family::DualSense;
        for (int label = static_cast<int>(prompts::Label::South); label <= static_cast<int>(prompts::Label::Digit2);
             ++label) {
            Piece piece{{family, static_cast<std::uint16_t>(label)}};
            prompts::render_glyph_scaled(prompt_family, static_cast<prompts::Label>(label), scale, piece.width,
                                         piece.height, piece.rgba);
            if (piece.width > 0 && piece.height > 0) pieces.push_back(std::move(piece));
        }
    }
    if (picture_font != nullptr) {
        for (const std::uint8_t index : prompts::kSlotGlyph) {
            if (index >= picture_font->glyphs.size()) continue;
            const FontGlyph& g = picture_font->glyphs[index];
            if (g.sheet >= picture_font->sheets.size()) continue;
            const DecodedTexture& sheet = picture_font->sheets[g.sheet];
            Piece piece{{GlyphFamily::Wii, index}, picture_font->cell_width, picture_font->cell_height};
            piece.rgba.resize(static_cast<std::size_t>(piece.width) * piece.height * 4u);
            for (int y = 0; y < piece.height; ++y) {
                for (int x = 0; x < piece.width; ++x) {
                    const unsigned sx = g.x + static_cast<unsigned>(x), sy = g.y + static_cast<unsigned>(y);
                    if (sx >= sheet.width || sy >= sheet.height) continue;
                    std::memcpy(&piece.rgba[(static_cast<std::size_t>(y) * piece.width + x) * 4u],
                                &sheet.rgba8[(static_cast<std::size_t>(sy) * sheet.width + sx) * 4u], 4u);
                }
            }
            pieces.push_back(std::move(piece));
        }
    }
    // Shelves 1024 wide, a texel of clear border around each.
    GlyphAtlasImage out;
    constexpr int kWidth = 1024;
    int x = 1, y = 1, shelf = 0;
    struct Place {
        int x, y;
    };
    std::vector<Place> places;
    for (const Piece& piece : pieces) {
        if (x + piece.width + 1 > kWidth) {
            x = 1;
            y += shelf + 2;
            shelf = 0;
        }
        places.push_back({x, y});
        x += piece.width + 2;
        shelf = std::max(shelf, piece.height);
    }
    const int height = std::max(1, y + shelf + 1);
    out.texture.width = kWidth;
    out.texture.height = static_cast<unsigned>(height);
    out.texture.rgba8.assign(static_cast<std::size_t>(kWidth) * height * 4u, 0u);
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const Piece& piece = pieces[i];
        for (int row = 0; row < piece.height; ++row) {
            std::memcpy(&out.texture.rgba8[(static_cast<std::size_t>(places[i].y + row) * kWidth + places[i].x) * 4u],
                        &piece.rgba[static_cast<std::size_t>(row) * piece.width * 4u],
                        static_cast<std::size_t>(piece.width) * 4u);
        }
        out.atlas.rects[GlyphAtlas::key(piece.ref)] = {
            static_cast<float>(places[i].x) / kWidth, static_cast<float>(places[i].y) / height,
            static_cast<float>(places[i].x + piece.width) / kWidth, static_cast<float>(places[i].y + piece.height) / height,
            static_cast<float>(piece.width) / static_cast<float>(piece.height)};
    }
    return out;
}

}  // namespace galaxy::gx::menu
