#pragma once

// Which of the game's own menus are on screen, for the host menus that sit beside them (host_menus.cpp): the
// layout hook (site_hooks.cpp) notes each layout it sees drawn. Read-only towards the game, so it is safe online.

#include <string_view>

namespace galaxy::game_menus {

enum class Menu : unsigned { Pause = 0, Count };

// From the simulation thread, for every layout drawn (by its LayoutManager name), unless hidden.
void note_layout_drawn(std::string_view layout_name) noexcept;
// Whether the menu was drawn within the last tenth of a second (any thread).
[[nodiscard]] bool shown(Menu menu) noexcept;

}  // namespace galaxy::game_menus
