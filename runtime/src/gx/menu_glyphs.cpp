#include "galaxy/gx/menu_glyphs.h"

#include "galaxy/button_prompts.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace galaxy::gx::menu {

namespace {

struct Piece {
    GlyphFamily family;
    GlyphButton button;
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

prompts::Label pad_label(GlyphButton button) {
    switch (button) {
    case GlyphButton::Start: return prompts::Label::Start;
    case GlyphButton::Confirm: return prompts::Label::South;
    case GlyphButton::Back: return prompts::Label::East;
    case GlyphButton::Select: return prompts::Label::Back;
    case GlyphButton::Dpad: return prompts::Label::Dpad;
    case GlyphButton::Controller: return prompts::Label::Controller;
    }
    return prompts::Label::None;
}

// The picture font's glyph index of the Wii button each prompt stands for (button_prompts.h kSlotGlyph).
int wii_glyph(GlyphButton button) {
    switch (button) {
    case GlyphButton::Start: return 25;       // +
    case GlyphButton::Confirm: return 1;      // A
    case GlyphButton::Back: return 2;         // B
    case GlyphButton::Select: return 24;      // -
    case GlyphButton::Dpad: return 20;
    case GlyphButton::Controller: return 4;   // the Wii Remote
    }
    return -1;
}

}  // namespace

GlyphAtlasImage build_glyph_atlas(const GameFont* picture_font, float scale) {
    constexpr std::array kButtons{GlyphButton::Start, GlyphButton::Confirm, GlyphButton::Back,
                                  GlyphButton::Select, GlyphButton::Dpad, GlyphButton::Controller};
    std::vector<Piece> pieces;
    for (const GlyphFamily family : {GlyphFamily::Xbox, GlyphFamily::DualSense}) {
        const auto prompt_family = family == GlyphFamily::Xbox ? prompts::Family::Xbox : prompts::Family::DualSense;
        for (const GlyphButton button : kButtons) {
            Piece piece{family, button};
            prompts::render_glyph_scaled(prompt_family, pad_label(button), scale, piece.width, piece.height, piece.rgba);
            if (piece.width > 0 && piece.height > 0) pieces.push_back(std::move(piece));
        }
    }
    if (picture_font != nullptr) {
        for (const GlyphButton button : kButtons) {
            const int index = wii_glyph(button);
            if (index < 0 || static_cast<std::size_t>(index) >= picture_font->glyphs.size()) continue;
            const FontGlyph& g = picture_font->glyphs[static_cast<std::size_t>(index)];
            if (g.sheet >= picture_font->sheets.size()) continue;
            const DecodedTexture& sheet = picture_font->sheets[g.sheet];
            Piece piece{GlyphFamily::Wii, button, picture_font->cell_width, picture_font->cell_height};
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
        const auto f = static_cast<std::size_t>(piece.family), b = static_cast<std::size_t>(piece.button);
        out.atlas.rects[f][b] = {static_cast<float>(places[i].x) / kWidth, static_cast<float>(places[i].y) / height,
                                 static_cast<float>(places[i].x + piece.width) / kWidth,
                                 static_cast<float>(places[i].y + piece.height) / height,
                                 static_cast<float>(piece.width) / static_cast<float>(piece.height)};
        out.atlas.present[f][b] = true;
    }
    return out;
}

}  // namespace galaxy::gx::menu
