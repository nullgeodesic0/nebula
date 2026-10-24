#pragma once

// The host-side menus drawn over the game with the menu kit (menu_kit.h, docs/MULTIPLAYER_UX.md): the Players
// panel, the join request, the status cards. present() calls draw_host_menus once a frame after the XFB blit.

#include "galaxy/gx/menu_kit.h"

namespace galaxy::gx {

class RendererD3D12;

// The game picture's rectangle in backbuffer pixels (the letterboxed XFB blit), which the menus' layout space
// is centred on.
struct PictureRect {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

// Builds this frame's menus into one draw list and draws it. `seconds` is a steady clock for the animations;
// `settings_open`: F1 is up (the save select's options give way to it).
void draw_host_menus(RendererD3D12& renderer, const PictureRect& picture, double seconds, bool settings_open);

}  // namespace galaxy::gx
