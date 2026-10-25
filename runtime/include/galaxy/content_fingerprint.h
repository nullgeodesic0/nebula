#pragma once

// The game content this PC plays, as one number (native_host.cpp computes it once the disc and its mods are loaded):
// game.pak's boot block and file table, and every disc mod override in force (the custom model and animations,
// animation packs). Two PCs whose numbers differ load different data into the game and cannot play online in step,
// so the lobby refuses the join (net_lobby.cpp). Voice replacement is mixed outside the game and is not part of it.

#include <cstdint>
#include <string>

namespace galaxy {

void set_content_fingerprint(std::uint64_t value, std::string mods) noexcept;
// 0 until the disc has been read.
[[nodiscard]] std::uint64_t content_fingerprint() noexcept;
// The mods in force, for messages ("model, animations"; empty with none).
[[nodiscard]] std::string content_mods();

}  // namespace galaxy
