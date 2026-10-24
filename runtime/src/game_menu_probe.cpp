#include "galaxy/game_menu_probe.h"

#include <array>
#include <atomic>
#include <chrono>

namespace galaxy::game_menus {

namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(Menu::Count)> kLayouts{"PauseMenu"};
std::array<std::atomic<long long>, static_cast<std::size_t>(Menu::Count)> s_seen{};

long long now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

void note_layout_drawn(std::string_view layout_name) noexcept {
    for (std::size_t i = 0; i < kLayouts.size(); ++i) {
        if (layout_name == kLayouts[i]) s_seen[i].store(now_ms(), std::memory_order_relaxed);
    }
}

bool shown(Menu menu) noexcept {
    const long long seen = s_seen[static_cast<std::size_t>(menu)].load(std::memory_order_relaxed);
    return seen != 0 && now_ms() - seen < 100;
}

}  // namespace galaxy::game_menus
