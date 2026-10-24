#pragma once

// The menus' button prompts in one texture (menu_kit.h GlyphAtlas): Xbox and DualSense drawn by button_prompts.h at
// HD size, the Wii's (also shown for the Switch controllers and the keyboard's +) taken from the game's picture font.

#include "galaxy/gx/game_asset_extract.h"
#include "galaxy/gx/game_font.h"
#include "galaxy/gx/menu_kit.h"

namespace galaxy::gx::menu {

struct GlyphAtlasImage {
    DecodedTexture texture;
    GlyphAtlas atlas;
};

// `picture_font`: picturefont.brfnt, or nullptr (no Wii prompts). `scale`: the pads' prompts' size over the game's
// 34x35 cell.
[[nodiscard]] GlyphAtlasImage build_glyph_atlas(const GameFont* picture_font, float scale = 3.0f);

}  // namespace galaxy::gx::menu
