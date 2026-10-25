// SPDX-License-Identifier: GPL-3.0-only
// Animation packs: a mod of Mario's animations with no build step. A pack is a folder of <bck name>.ank1 files, each
// the ANK1 section of one of MarioAnime.arc's animations (tools/animations/build_animation_mod.py --pack writes
// them); as a mod (mods.h) it is the mod's animations/ folder, or a mod of .ank1 files only. When the mods are
// applied, MarioAnime.arc (the game's, or a mod's) is rebuilt with each animation a pack has, and served in its place.
#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace galaxy::mods {

// `arc` (a RARC archive, Yaz0-compressed or not) with each named animation replaced: the archive's own BCK header,
// then the pack's ANK1 section. `ank1_by_bck` is keyed by the animation's file name, lower case ("walk.bck").
// Empty when `arc` is not an archive; `replaced` counts the animations taken from the packs.
[[nodiscard]] std::vector<std::uint8_t> replace_animations(
    std::span<const std::uint8_t> arc, const std::map<std::string, std::vector<std::uint8_t>>& ank1_by_bck,
    unsigned* replaced);

}  // namespace galaxy::mods
