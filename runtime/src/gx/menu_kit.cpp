#include "galaxy/gx/menu_kit.h"

#include <algorithm>
#include <cmath>

namespace galaxy::gx::menu {

Rgba mix(const Rgba& a, const Rgba& b, float t) noexcept {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

Space Space::for_picture(float x, float y, float width, float height) noexcept {
    return {x + width * 0.5f, y + height * 0.5f, height / 456.0f};
}

float curve(std::span<const Key> keys, float frame) noexcept {
    if (keys.empty()) return 0.0f;
    if (frame <= keys.front().frame) return keys.front().value;
    for (std::size_t i = 0; i + 1u < keys.size(); ++i) {
        const Key& k0 = keys[i];
        const Key& k1 = keys[i + 1u];
        if (frame >= k1.frame) continue;
        const float span = k1.frame - k0.frame;
        if (span <= 0.0f) return k1.value;
        // NW4R's Hermite: slopes are per frame.
        const float t = (frame - k0.frame) / span;
        const float t2 = t * t, t3 = t2 * t;
        return k0.value * (2.0f * t3 - 3.0f * t2 + 1.0f) + k1.value * (-2.0f * t3 + 3.0f * t2) +
               k0.slope * span * (t3 - 2.0f * t2 + t) + k1.slope * span * (t3 - t2);
    }
    return keys.back().value;
}

// ---- ButtonAnim: PauseMenu.arc buttonselectin (8 frames), buttonselectout (11), buttondecide (15).
namespace {
constexpr float kInFrames = 8.0f;
constexpr float kOutFrames = 11.0f;
constexpr float kDecideFrames = 15.0f;
constexpr float kInHalf = 6.0f;  // where the in and out curves meet (scale 1.15, text 20)
}  // namespace

void ButtonAnim::hover(bool on) noexcept {
    if (phase == Phase::Decide) return;
    if (on && (phase == Phase::Idle || phase == Phase::Out)) {
        // Picks up from the matching point of the way out, as the game's controller reverses a running animation.
        frame = phase == Phase::Out && frame < kInHalf ? kInHalf - frame : 0.0f;
        phase = Phase::In;
    } else if (!on && (phase == Phase::In || phase == Phase::Hovered)) {
        frame = phase == Phase::In && frame < kInHalf ? kInHalf - frame : 0.0f;
        phase = Phase::Out;
    }
}

void ButtonAnim::decide() noexcept {
    phase = Phase::Decide;
    frame = 0.0f;
}

void ButtonAnim::step(float frames) noexcept {
    frame += frames;
    if (phase == Phase::In && frame >= kInFrames) {
        phase = Phase::Hovered;
        frame = kInFrames;
    } else if (phase == Phase::Out && frame >= kOutFrames) {
        phase = Phase::Idle;
        frame = 0.0f;
    } else if (phase == Phase::Decide) {
        frame = std::min(frame, kDecideFrames);
    } else if (phase == Phase::Idle || phase == Phase::Hovered) {
        frame = 0.0f;
    }
}

ButtonAnim::Look ButtonAnim::look(const HoverStyle& h) const noexcept {
    Look look;
    switch (phase) {
    case Phase::Idle:
        break;
    case Phase::In:
        look.scale = curve({{0, 1.0f, h.in_slope}, {6, h.peak, 0}}, frame);
        look.gradient = curve({{0, 0, 0}, {7, 1, 0}}, frame);
        look.text_grey = curve({{0, 80, -10}, {6, 20, -10}, {6, 20, 0}}, frame);
        look.shadow_alpha = curve({{0, 0, 0}, {6, 255, 64.37f}, {6, 255, 0}}, frame) / 255.0f;
        look.shadow_y = curve({{0, 0, 1.995f}, {6, 4, 0}}, frame);
        break;
    case Phase::Hovered:
    case Phase::Decide:
        look.scale = h.peak;
        look.gradient = 1.0f;
        look.text_grey = 20.0f;
        look.shadow_alpha = 1.0f;
        look.shadow_y = 4.0f;
        if (phase == Phase::Decide) look.shine = curve({{0, 0, 127.5f}, {2, 255, 0}, {14, 0, 0}}, frame) / 255.0f;
        break;
    case Phase::Out:
        look.scale = curve({{0, h.peak, 0}, {6, 1.0f, h.out_slope}, {6, 1.0f, 0.067f}, {8, h.bounce, h.bounce_slope}, {10, 1.0f, -0.072f}}, frame);
        look.gradient = curve({{0, 1, 0}, {7, 0, 0}}, frame);
        look.text_grey = curve({{0, 20, 10}, {6, 80, 10}}, frame);
        look.shadow_alpha = curve({{0, 255, -68.289f}, {6, 0, 0}}, frame) / 255.0f;
        look.shadow_y = curve({{0, 4, 0}, {6, 0, -2}}, frame);
        break;
    }
    look.gradient = std::clamp(look.gradient, 0.0f, 1.0f);
    look.shadow_alpha = std::clamp(look.shadow_alpha, 0.0f, 1.0f);
    look.shine = std::clamp(look.shine, 0.0f, 1.0f);
    return look;
}

// ---- WindowAnim: SysInfoWindow.arc buttonappear / buttonend (21 frames each).
namespace {
constexpr float kWindowFrames = 21.0f;
}

void WindowAnim::open() noexcept {
    if (phase == Phase::Appear || phase == Phase::Shown) return;
    // Interrupting the way out comes back from where it is.
    frame = phase == Phase::End ? std::max(0.0f, 20.0f - frame) : 0.0f;
    phase = Phase::Appear;
}

void WindowAnim::close() noexcept {
    if (phase == Phase::Hidden || phase == Phase::End) return;
    frame = phase == Phase::Appear ? std::max(0.0f, 20.0f - frame) : 0.0f;
    phase = Phase::End;
}

void WindowAnim::step(float frames) noexcept {
    if (phase == Phase::Hidden || phase == Phase::Shown) return;
    frame += frames;
    if (frame >= kWindowFrames) {
        phase = phase == Phase::Appear ? Phase::Shown : Phase::Hidden;
        frame = 0.0f;
    }
}

float WindowAnim::slide_x() const noexcept {
    switch (phase) {
    case Phase::Appear: return curve({{0, 100, -8.128f}, {20, 0, 0}}, frame);
    case Phase::End: return curve({{0, 0, 0}, {20, -100, -6.556f}}, frame);
    default: return 0.0f;
    }
}

float WindowAnim::alpha() const noexcept {
    switch (phase) {
    case Phase::Hidden: return 0.0f;
    case Phase::Shown: return 1.0f;
    case Phase::Appear: return std::clamp(curve({{0, 0, 25.517f}, {20, 255, 0}}, frame) / 255.0f, 0.0f, 1.0f);
    case Phase::End: return std::clamp(curve({{0, 255, 0}, {20, 0, -21.324f}}, frame) / 255.0f, 0.0f, 1.0f);
    }
    return 0.0f;
}

// ---- Drawing.
namespace {

Rgba fade(Rgba c, float alpha) noexcept {
    c.a *= alpha;
    return c;
}

// A quad from layout-space corners, with a vertical colour gradient.
void push(DrawList& list, const Space& space, float x0, float y0, float x1, float y1, float u0, float v0, float u1,
          float v1, const Rgba& top, const Rgba& bottom, Tex tex, Mode mode, const Rgba& c0 = {}, const Rgba& c1 = {},
          float scale = 1.0f) {
    Quad q;
    q.v[0] = {space.sx(x0), space.sy(y0), u0, v0, top};
    q.v[1] = {space.sx(x1), space.sy(y0), u1, v0, top};
    q.v[2] = {space.sx(x0), space.sy(y1), u0, v1, bottom};
    q.v[3] = {space.sx(x1), space.sy(y1), u1, v1, bottom};
    q.c0 = c0;
    q.c1 = c1;
    q.tex = tex;
    q.mode = mode;
    q.scale = scale;
    list.push_back(q);
}

// The texture's last column, where the round end meets the straight middle: the middle's whole width samples it.
constexpr float kCapEdgeU = 31.5f / 32.0f;

// One pill-shaped layer (three pics: the middle and the two round ends) around (x, y): `scale` is the pane's,
// `middle_y` the middle's own vertical scale and `cap_scale` the ends' (as in the layout).
struct PillLayer {
    float width, height, cap, scale, middle_y, cap_scale, offset_y;
};
void pill_layer(DrawList& list, const Space& space, float x, float y, const PillLayer& l, const Rgba& top,
                const Rgba& bottom, Mode mode, const Rgba& c0, const Rgba& c1, float tev_scale) {
    const float half = l.width * 0.5f * l.scale;
    const float oy = y + l.offset_y * l.scale;
    const float mh = l.height * l.middle_y * l.scale * 0.5f;
    const float ch = l.height * l.cap_scale * l.scale * 0.5f;
    const float cw = l.cap * l.cap_scale * l.scale;
    push(list, space, x - half, oy + mh, x + half, oy - mh, kCapEdgeU, 0, kCapEdgeU, 1, top, bottom, Tex::ButtonCap,
         mode, c0, c1, tev_scale);
    push(list, space, x - half - cw, oy + ch, x - half, oy - ch, 0, 0, 1, 1, top, bottom, Tex::ButtonCap, mode, c0, c1,
         tev_scale);
    push(list, space, x + half, oy + ch, x + half + cw, oy - ch, 1, 0, 0, 1, top, bottom, Tex::ButtonCap, mode, c0, c1,
         tev_scale);
}

float font_scale_x(const GameFont& font, const TextStyle& style) {
    return style.font_w / static_cast<float>(font.width > 0 ? font.width : font.cell_width);
}
float font_scale_y(const GameFont& font, const TextStyle& style) {
    return style.font_h / static_cast<float>(font.height > 0 ? font.height : font.cell_height);
}

}  // namespace

void rect(DrawList& list, const Space& space, float x0, float y0, float x1, float y1, const Rgba& top,
          const Rgba& bottom) {
    push(list, space, x0, y0, x1, y1, 0, 0, 1, 1, top, bottom, Tex::None, Mode::Solid);
}

float text_width(const Assets& assets, const TextStyle& style, std::u16string_view str) noexcept {
    if (assets.font == nullptr || str.empty()) return 0.0f;
    const float sx = font_scale_x(*assets.font, style);
    float width = 0.0f;
    for (const char16_t c : str) width += static_cast<float>(assets.font->glyph(c).advance) * sx + style.char_space;
    return width - style.char_space;
}

void text(DrawList& list, const Assets& assets, const Space& space, const TextStyle& style, float x, float y,
          std::u16string_view str, float alpha) {
    if (assets.font == nullptr || str.empty() || alpha <= 0.0f) return;
    const GameFont& font = *assets.font;
    const float sx = font_scale_x(font, style);
    const float sy = font_scale_y(font, style);
    // The line pitch is the font's line feed plus the line space at the font size alone (the game's screens show
    // it unscaled by the enclosing panes).
    const float line_height = static_cast<float>(font.line_feed) * sy / style.pane_scale + style.line_space;
    std::size_t lines = 1u;
    for (const char16_t c : str) lines += c == u'\n' ? 1u : 0u;
    // The block is centred on y: its first line's top sits half the block above.
    const float block = style.font_h + static_cast<float>(lines - 1u) * line_height;
    float top = y + block * 0.5f;
    const Rgba color = fade(style.color, alpha);
    std::size_t start = 0u;
    while (start <= str.size()) {
        std::size_t end = str.find(u'\n', start);
        if (end == std::u16string_view::npos) end = str.size();
        const std::u16string_view line = str.substr(start, end - start);
        const float width = text_width(assets, style, line);
        float pen = style.align == TextStyle::Align::Left ? x
                    : style.align == TextStyle::Align::Right ? x - width
                                                             : x - width * 0.5f;
        for (const char16_t c : line) {
            const FontGlyph g = font.glyph(c);
            if (g.glyph_width > 0 && g.sheet < font.sheets.size()) {
                const DecodedTexture& sheet = font.sheets[g.sheet];
                const float gx = pen + static_cast<float>(g.left) * sx;
                const float gw = static_cast<float>(g.glyph_width) * sx;
                const float gh = static_cast<float>(font.cell_height) * sy;
                const float u0 = static_cast<float>(g.x) / static_cast<float>(sheet.width);
                const float v0 = static_cast<float>(g.y) / static_cast<float>(sheet.height);
                const float u1 = static_cast<float>(g.x + g.glyph_width) / static_cast<float>(sheet.width);
                const float v1 = static_cast<float>(g.y + font.cell_height) / static_cast<float>(sheet.height);
                push(list, space, gx, top, gx + gw, top - gh, u0, v0, u1, v1, color, color,
                     static_cast<Tex>(static_cast<std::uint16_t>(Tex::Font) + g.sheet),
                     assets.font_sdf ? Mode::Text : Mode::Coverage);
            }
            pen += static_cast<float>(g.advance) * sx + style.char_space;
        }
        top -= line_height;
        start = end + 1u;
    }
}

void pill(DrawList& list, const Assets& assets, const Space& space, const PillStyle& style, float x, float y,
          std::u16string_view label, const ButtonAnim::Look& look, float alpha) {
    if (alpha <= 0.0f) return;
    const float s = look.scale;
    // The material: the texture's intensity lerps from the cyan rim (C0) to white (C1), times the vertex gradient
    // (doubled, TEV scale 2), which the hover inverts.
    const Rgba c0 = rgb255(0, 160, 255, 0);
    const Rgba c1 = rgb255(255, 255, 255);
    // The face's gradient inverts on hover; the rim's (the Frame pics) never changes.
    const Rgba top = fade(mix(rgb255(150, 200, 255), style.hover->top, look.gradient), alpha);
    const Rgba bottom = fade(mix(rgb255(60, 65, 70), style.hover->bottom, look.gradient), alpha);
    const Rgba rim_top = fade(rgb255(150, 200, 255), alpha);
    const Rgba rim_bottom = fade(rgb255(60, 65, 70), alpha);
    if (assets.button_cap) {
        // The drop shadow (ShaButton1, pane alpha 128, black at 80): below, sliding with the hover.
        const Rgba shadow = fade(rgb255(0, 0, 0, 80), alpha * 128.0f / 255.0f);
        pill_layer(list, space, x, y + look.shadow_y * s,
                   {style.width, style.height, style.cap, s * 1.07f, 1.1f, 1.1f, -6.0f}, shadow, shadow,
                   Mode::Coverage, {}, {}, 1.0f);
        // The rim (the Frame pics, 10 % taller) under the face.
        const float pane = s * style.pane_scale;
        pill_layer(list, space, x, y, {style.width, style.height, style.cap, pane, 1.1f, 1.1f, 0.0f}, rim_top,
                   rim_bottom, Mode::Material, c0, c1, 2.0f);
        pill_layer(list, space, x, y, {style.width, style.height, style.cap, pane, 1.0f, 1.0f, 0.0f}, top, bottom,
                   Mode::Material, c0, c1, 2.0f);
        if (look.shine > 0.0f) {
            const Rgba cyan = rgb255(0, 255, 255);
            const Rgba white = fade(rgb255(255, 255, 255), alpha * look.shine);
            pill_layer(list, space, x, y, {style.width, style.height, style.cap, s * 1.1f, 1.1f, 1.1f, 0.0f}, white,
                       white, Mode::Additive, cyan, cyan, 1.0f);
        }
    }
    if (assets.font != nullptr && !label.empty()) {
        const float pane = s * style.label_scale;
        TextStyle shadow_style;
        shadow_style.font_w = style.font_w * pane * 0.95f;
        shadow_style.font_h = style.font_h * pane;
        shadow_style.char_space = style.char_space * pane * 0.95f;
        shadow_style.color = rgb255(0, 0, 0, 50);
        shadow_style.pane_scale = pane;
        if (look.shadow_alpha > 0.0f) {
            text(list, assets, space, shadow_style, x, y + style.label_shadow_y * pane, label, alpha * look.shadow_alpha);
        }
        TextStyle label_style;
        label_style.font_w = style.font_w * pane;
        label_style.font_h = style.font_h * pane;
        label_style.char_space = style.char_space * pane;
        label_style.pane_scale = pane;
        const int grey = static_cast<int>(std::lround(look.text_grey));
        label_style.color = rgb255(grey, grey, grey);
        text(list, assets, space, label_style, x, y + style.label_y * pane, label, alpha);
    }
}

PillStyle dialog_pill_style() noexcept {
    PillStyle style;
    style.width = 110.0f;
    style.height = 50.0f;
    style.cap = 25.0f;
    style.pane_scale = 1.234f;
    style.font_w = 20.25f;
    style.font_h = 28.05f;
    style.char_space = 1.0f;
    style.label_scale = 1.0f;
    style.label_y = 0.0f;
    style.label_shadow_y = -4.0f;
    style.hit_w = 160.0f;
    style.hit_h = 50.0f;
    style.hit_scale = 1.0f;
    style.hover = &kDialogHover;
    return style;
}

bool pill_hit(const PillStyle& style, float x, float y, float px, float py) noexcept {
    // The layout's bnd1 box.
    const float half_w = style.hit_w * 0.5f * style.hit_scale;
    const float half_h = style.hit_h * 0.5f * style.hit_scale;
    return std::fabs(px - x) <= half_w && std::fabs(py - y) <= half_h;
}

void window(DrawList& list, const Assets& assets, const Space& space, float x, float y, float width, float height,
            float alpha) {
    if (alpha <= 0.0f) return;
    const float x0 = x - width * 0.5f, x1 = x + width * 0.5f;
    const float y0 = y + height * 0.5f, y1 = y - height * 0.5f;
    const float corner = 24.0f;
    // The frame's material lerps from white (the ring) to the body's pale blue (MySysInfoWindow WinInfoLT).
    const Rgba c0 = rgb255(255, 255, 255);
    const Rgba c1 = rgb255(200, 220, 245);
    const Rgba body = fade(c1, alpha);
    const Rgba white = fade(rgb255(255, 255, 255), alpha);
    const auto frame = [&](float ox, float oy, Mode mode, const Rgba& color, const Rgba& fill) {
        const float l = x0 + ox, r = x1 + ox, t = y0 + oy, b = y1 + oy;
        if (!assets.window_corner) {
            push(list, space, l, t, r, b, 0, 0, 1, 1, fill, fill, Tex::None, Mode::Solid);
            return;
        }
        const float il = l + corner, ir = r - corner, it = t - corner, ib = b + corner;
        // Corners: the texture, mirrored to each.
        push(list, space, l, t, il, it, 0, 0, 1, 1, color, color, Tex::WindowCorner, mode, c0, c1);
        push(list, space, ir, t, r, it, 1, 0, 0, 1, color, color, Tex::WindowCorner, mode, c0, c1);
        push(list, space, l, ib, il, b, 0, 1, 1, 0, color, color, Tex::WindowCorner, mode, c0, c1);
        push(list, space, ir, ib, r, b, 1, 1, 0, 0, color, color, Tex::WindowCorner, mode, c0, c1);
        // Edges: the corner's inner column or row, stretched.
        constexpr float kEdge = 23.5f / 24.0f;
        push(list, space, il, t, ir, it, kEdge, 0, kEdge, 1, color, color, Tex::WindowCorner, mode, c0, c1);
        push(list, space, il, ib, ir, b, kEdge, 1, kEdge, 0, color, color, Tex::WindowCorner, mode, c0, c1);
        push(list, space, l, it, il, ib, 0, kEdge, 1, kEdge, color, color, Tex::WindowCorner, mode, c0, c1);
        push(list, space, ir, it, r, ib, 1, kEdge, 0, kEdge, color, color, Tex::WindowCorner, mode, c0, c1);
        push(list, space, il, it, ir, ib, 0, 0, 1, 1, fill, fill, Tex::None, Mode::Solid);
    };
    // The shadow (ShaInfo): 6 right and 6 down, black at 100.
    const Rgba shadow = fade(rgb255(0, 0, 0, 100), alpha);
    frame(6.0f, -6.0f, Mode::Coverage, shadow, shadow);
    frame(0.0f, 0.0f, Mode::Material, white, body);
}

}  // namespace galaxy::gx::menu
