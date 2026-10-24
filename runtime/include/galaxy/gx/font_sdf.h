#pragma once

// Glyph sheets as signed distance fields, so the game's 26-pixel font draws with sharp edges at any HD size
// (menu_kit.h Mode::Text). The edge is the coverage's 0.5 line, so letters keep the weight they have in the game.

#include "galaxy/gx/game_asset_extract.h"

namespace galaxy::gx {

// `sheet`'s alpha is coverage. The result is `out_scale` times larger; its alpha is 0.5 on the edge, rising
// inside and falling outside by 0.5 per `spread` output texels (RGB white).
[[nodiscard]] DecodedTexture coverage_to_sdf(const DecodedTexture& sheet, unsigned out_scale = 2u, float spread = 4.0f);

}  // namespace galaxy::gx
