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
    Glyphs = 3,         // the button prompts (GlyphAtlas)
    // The Star Pointer's hand (DPDPointer.arc), drawn again over the menus when the pointer is on them.
    Hand = 4,           // HandPointerPoint.tpl
    HandShadow = 5,     // HandPointerPointShadow.tpl
    HandRing = 6,       // DPDOutStar.tpl, mirrored to the whole star outline
    HandStar = 7,       // DPDPointer.tpl, mirrored to the whole star
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
// - Text: the vertex colour, alpha times the edge of a distance-field glyph sheet (font_sdf.h), sharp at any size.
// - Image: the texture's colour times the vertex colour (the button prompts).
enum class Mode : std::uint16_t { Solid = 0, Coverage = 1, Material = 2, Additive = 3, Text = 4, Image = 5 };
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

// ---- Button prompts: each controller family's own art (button_prompts.h for Xbox and DualSense, the game's
// picture font for the Wii's), packed in one texture.
enum class GlyphFamily : std::uint8_t { Wii = 0, Xbox = 1, DualSense = 2 };
// What a prompt shows (button_prompts.h Label for the pads; the Wii's equivalents for Wii).
enum class GlyphButton : std::uint8_t { Start, Confirm, Back, Select, Dpad, Controller };
struct GlyphRect {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    float aspect = 1.0f;  // width / height
};
struct GlyphAtlas {
    std::array<std::array<GlyphRect, 6>, 3> rects{};
    std::array<std::array<bool, 6>, 3> present{};
    [[nodiscard]] const GlyphRect* find(GlyphFamily family, GlyphButton button) const noexcept {
        const auto f = static_cast<std::size_t>(family), b = static_cast<std::size_t>(button);
        return f < 3u && b < 6u && present[f][b] ? &rects[f][b] : nullptr;
    }
};

// ---- Widgets. Positions are layout units (the centre of the widget); `alpha` 0..1 multiplies everything.
struct Assets {
    const GlyphAtlas* glyphs = nullptr;
    const GameFont* font = nullptr;  // MessageFont26
    bool button_cap = false;
    bool window_corner = false;
    // The font's sheets are bound as distance fields (Mode::Text) rather than coverage.
    bool font_sdf = false;
    bool pointer_hand = false;
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

// A button prompt `height` layout units tall, centred on (x, y); returns its width (0 if the atlas lacks it).
float glyph(DrawList& list, const Assets& assets, const Space& space, GlyphFamily family, GlyphButton button, float x,
            float y, float height, float alpha);

// The first player's Star Pointer hand (DPDPointer.arc HandPointer) with its tip at (x, y): the game draws its own
// under the picture, so over a host menu this one takes its place.
void pointer_hand(DrawList& list, const Assets& assets, const Space& space, float x, float y, float alpha);

// A plain rectangle (the menu backdrop, dividers), layout units.
void rect(DrawList& list, const Space& space, float x0, float y0, float x1, float y1, const Rgba& top,
          const Rgba& bottom);

}  // namespace galaxy::gx::menu
