#pragma once

// The host-side menus' look and feel (docs/MULTIPLAYER_UX.md "Where each menu lives, and how it looks"): the
// game's own button pills, info windows and MessageFont26 text, laid out in the game's 608x456 layout units and
// animated with the game's own keyframes (PauseMenu.arc and SysInfoWindow.arc BRLANs). Widgets append quads to a
// DrawList, which the renderer draws over the presented picture (renderer_d3d12.cpp) and the tests rasterise on
// the CPU (menu_kit_raster.h). Nothing here touches the game's memory, so a menu shown on one PC of an online
// session cannot desync the other.

#include "galaxy/gx/game_font.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

namespace galaxy::gx::menu {

struct Rgba {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};
[[nodiscard]] constexpr Rgba rgb255(int r, int g, int b, int a = 255) noexcept {
    return {r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
}
[[nodiscard]] Rgba mix(const Rgba& a, const Rgba& b, float t) noexcept;

// The textures a quad samples (the renderer binds them in this order).
enum class Tex : std::uint16_t {
    None = 0,
    ButtonCap = 1,      // MyButtonATypeSide.tpl, mirrored vertically to the whole 32x64 round end
    WindowCorner = 2,   // MySysInfoWindow.tpl, the info window's 24x24 corner
    Font = 8,           // MessageFont26 sheet 0; sheet n is Font + n
};
inline constexpr std::size_t kMaxTextures = 16u;

// How a quad's colour comes out (the game's TEV setups these menus use):
// - Solid: the vertex colour.
// - Coverage: the vertex colour, alpha times the texture's alpha (text, shadows cut by a texture).
// - Material: lerp(c0, c1 times the vertex colour times `scale`, texture intensity), alpha the texture's alpha
//   times the vertex alpha (the pills' and windows' materials: the rim keeps c0 whatever the gradient does).
// - Additive: as Material, added to what is under it (the decide shine).
// Every mode's colour is then multiplied by kPictureGain, the brightness the game's picture reaches on screen.
enum class Mode : std::uint16_t { Solid = 0, Coverage = 1, Material = 2, Additive = 3 };
inline constexpr float kPictureGain = 1.24f;

struct Vertex {
    float x = 0.0f, y = 0.0f;  // screen pixels
    float u = 0.0f, v = 0.0f;
    Rgba color;
};
struct Quad {
    std::array<Vertex, 4> v;  // top-left, top-right, bottom-left, bottom-right
    Rgba c0, c1;
    Tex tex = Tex::None;
    Mode mode = Mode::Solid;
    float scale = 1.0f;
};
using DrawList = std::vector<Quad>;

// Layout units to screen pixels: the game draws its 608x456 layouts centred on the picture, one unit per
// picture-height/456 pixels, y up.
struct Space {
    float cx = 0.0f, cy = 0.0f, unit = 1.0f;
    [[nodiscard]] static Space for_picture(float x, float y, float width, float height) noexcept;
    [[nodiscard]] float sx(float lx) const noexcept { return cx + lx * unit; }
    [[nodiscard]] float sy(float ly) const noexcept { return cy - ly * unit; }
    // Back from pixels (the mouse) to layout units.
    [[nodiscard]] float lx(float px) const noexcept { return (px - cx) / unit; }
    [[nodiscard]] float ly(float py) const noexcept { return (cy - py) / unit; }
};

// ---- Animation: the BRLAN Hermite curves, in the game's frames (60 per second).
struct Key {
    float frame, value, slope;
};
// A curve's value at `frame` (keys in order; a repeated frame is a step).
[[nodiscard]] float curve(std::span<const Key> keys, float frame) noexcept;
[[nodiscard]] inline float curve(std::initializer_list<Key> keys, float frame) noexcept {
    return curve(std::span<const Key>(keys.begin(), keys.size()), frame);
}

// A layout's own hover keys: how far the button grows and bounces, and the face gradient it turns to.
struct HoverStyle {
    float peak, in_slope;                             // ButtonSelectIn scale [0:1 s in_slope] [6:peak s0]
    float out_slope, bounce, bounce_slope;            // ButtonSelectOut [6:1 s out_slope] ... [8:bounce s bounce_slope]
    Rgba top, bottom;                                 // the face gradient once hovered
};
// PauseMenu.arc and SysInfoWindow.arc.
inline constexpr HoverStyle kPauseHover{1.15f, 0.069f, -0.07f, 1.05f, 0.0f, rgb255(50, 60, 70), rgb255(130, 170, 210)};
inline constexpr HoverStyle kDialogHover{1.1f, 0.041f, -0.045f, 1.069f, -0.035f, rgb255(60, 70, 80), rgb255(140, 180, 220)};

// A button's hover (ButtonSelectIn / ButtonSelectOut) and decide (ButtonDecide) state, stepped in frames.
struct ButtonAnim {
    enum class Phase : std::uint8_t { Idle, In, Hovered, Out, Decide };
    Phase phase = Phase::Idle;
    float frame = 0.0f;
    void hover(bool on) noexcept;
    void decide() noexcept;
    void step(float frames) noexcept;
    [[nodiscard]] bool decided() const noexcept { return phase == Phase::Decide && frame >= 15.0f; }
    // The pane's scale, the gradient (0 normal, 1 hovered), the text's grey, the hover shadow's alpha and
    // offset, the decide shine's alpha.
    struct Look {
        float scale = 1.0f, gradient = 0.0f, text_grey = 80.0f, shadow_alpha = 0.0f, shadow_y = 0.0f, shine = 0.0f;
    };
    [[nodiscard]] Look look(const HoverStyle& style = kPauseHover) const noexcept;
};

// A window or a pause-menu button row coming in (Appear) or going out (End).
struct WindowAnim {
    enum class Phase : std::uint8_t { Hidden, Appear, Shown, End };
    Phase phase = Phase::Hidden;
    float frame = 0.0f;
    void open() noexcept;
    void close() noexcept;
    void step(float frames) noexcept;
    [[nodiscard]] bool visible() const noexcept { return phase != Phase::Hidden; }
    [[nodiscard]] bool settled() const noexcept { return phase == Phase::Shown || phase == Phase::Hidden; }
    // SysInfoWindow ButtonAppear/ButtonEnd: slide by +100 to 0 to -100 with the alpha.
    [[nodiscard]] float slide_x() const noexcept;
    [[nodiscard]] float alpha() const noexcept;
};

// ---- Widgets. Positions are layout units (the centre of the widget); `alpha` 0..1 multiplies everything.
struct Assets {
    const GameFont* font = nullptr;  // MessageFont26
    bool button_cap = false;
    bool window_corner = false;
};

// The pause menu's pill button (PauseMenu.arc NBackUp): centre width and height of the face, the pane scale
// (1.1 in the pause menu, 1.234 for the dialogs' Yes/No), the label, and its animation look.
struct PillStyle {
    float width = 240.0f;
    float height = 64.0f;
    float cap = 32.0f;
    float pane_scale = 1.1f;
    float font_w = 27.0f, font_h = 33.0f, char_space = 2.0f;
    // The label's own pane: its scale, its offset and its hover shadow's offset (the pause menu's text sits in the
    // scaled button pane; the dialogs' beside it).
    float label_scale = 1.1f, label_y = 2.0f, label_shadow_y = -1.0f;
    // The pointer's hit box (bnd1) and the pane scale around it.
    float hit_w = 304.0f, hit_h = 64.0f, hit_scale = 1.1f;
    const HoverStyle* hover = &kPauseHover;
};
// The dialogs' Yes/No (SysInfoWindow.arc LeftButton / RightButton).
[[nodiscard]] PillStyle dialog_pill_style() noexcept;
void pill(DrawList& list, const Assets& assets, const Space& space, const PillStyle& style, float x, float y,
          std::u16string_view label, const ButtonAnim::Look& look, float alpha);
// The pill's hit box (bnd1) in layout units, around its centre.
[[nodiscard]] bool pill_hit(const PillStyle& style, float x, float y, float px, float py) noexcept;

// The info window (SysInfoWindow.arc WinInfo): rounded frame, pale blue body and its shadow.
void window(DrawList& list, const Assets& assets, const Space& space, float x, float y, float width, float height,
            float alpha);

// Text in MessageFont26 at a layout font size, centred on (x, y) line by line ('\n' breaks lines).
struct TextStyle {
    float font_w = 27.0f, font_h = 33.0f, char_space = 2.0f, line_space = -4.0f;
    Rgba color = rgb255(80, 80, 80);
    // The enclosing panes' scale already in font_w/font_h (the line pitch leaves it out).
    float pane_scale = 1.0f;
    enum class Align : std::uint8_t { Left, Center, Right } align = Align::Center;
};
void text(DrawList& list, const Assets& assets, const Space& space, const TextStyle& style, float x, float y,
          std::u16string_view str, float alpha);
// A line's width in layout units.
[[nodiscard]] float text_width(const Assets& assets, const TextStyle& style, std::u16string_view str) noexcept;

// A plain rectangle (the menu backdrop, dividers), layout units.
void rect(DrawList& list, const Space& space, float x0, float y0, float x1, float y1, const Rgba& top,
          const Rgba& bottom);

}  // namespace galaxy::gx::menu
