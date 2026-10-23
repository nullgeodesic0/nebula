#pragma once

// A CPU rasteriser for menu_kit.h draw lists, with the same shading as the renderer's UI shader, for tests and the
// look-alike checks against screenshots of the game's own menus (tools: galaxy_menu_kit_preview).

#include "galaxy/gx/game_asset_extract.h"
#include "galaxy/gx/menu_kit.h"

#include <array>
#include <cstdint>
#include <vector>

namespace galaxy::gx::menu {

struct Image {
    unsigned width = 0, height = 0;
    std::vector<std::uint8_t> rgba;  // width*height*4
};

// `textures[i]` is Tex value i (nullptr: unbound, samples opaque white).
void rasterize(Image& target, const DrawList& list, const std::array<const DecodedTexture*, kMaxTextures>& textures);

// The pixel shading both the GPU and this rasteriser use: the quad's colour at a texel, before blending.
struct Shaded {
    float r, g, b, a;
    bool additive;
};
[[nodiscard]] Shaded shade(const Quad& quad, const Rgba& vertex, const std::array<float, 4>& texel) noexcept;

// The button cap as the renderer binds it: the 32x32 quarter mirrored into the whole 32x64 round end.
[[nodiscard]] DecodedTexture mirror_cap_vertically(const DecodedTexture& quarter);

}  // namespace galaxy::gx::menu
