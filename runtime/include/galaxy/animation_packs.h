#pragma once

// Custom animation packs: a folder of exported animations (build_animation_mod.py --pack) that Nebula loads
// at boot, with no build step and no Blender. <content>/mods/animation-packs/<any folder>/<bck name>.ank1 is
// the ANK1 section of one of MarioAnime.arc's animations; the game's own archive, from the player's disc, is
// rebuilt with each such animation replaced (its BCK header is the game's), and served in place of the
// original through the disc's file table.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace galaxy::animation_packs {

struct Override {
    std::uint64_t disc_offset{};
    std::vector<std::byte> bytes;
};

struct Result {
    std::vector<Override> overrides;  // the rebuilt archive and the patched file-table entry
    std::size_t replaced{};           // animations taken from packs
    std::size_t packs{};              // folders that had any
    std::string note;                 // why nothing was built, or what was
};

// The pack folder (<content>/mods/animation-packs) holds any .ank1 file.
[[nodiscard]] bool present(const std::filesystem::path& content_root);

// `pak` is game.pak (the disc's data partition); `taken` the disc ranges other overrides use. With
// `custom_model`, a pack's model/ subfolder (the custom model's variant) wins over its plain files.
[[nodiscard]] Result build(const std::filesystem::path& content_root, std::span<const std::byte> pak,
                           std::span<const std::pair<std::uint64_t, std::uint64_t>> taken, bool custom_model);

}  // namespace galaxy::animation_packs
