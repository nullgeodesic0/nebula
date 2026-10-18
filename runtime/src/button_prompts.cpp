#include "galaxy/button_prompts.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

namespace galaxy::prompts {
namespace {

using input::InputSource;
using input::PadAxis;
using input::SourceKind;
using input::WiiTarget;
namespace pad = input::pad;

// ---------------------------------------------------------------- bindings

[[nodiscard]] Label label_for_source(InputSource source) noexcept {
    if (source.kind == SourceKind::Button) {
        switch (source.code) {
        case pad::kSouthBit: return Label::South;
        case pad::kEastBit: return Label::East;
        case pad::kWestBit: return Label::West;
        case pad::kNorthBit: return Label::North;
        case pad::kLeftShoulderBit: return Label::L1;
        case pad::kRightShoulderBit: return Label::R1;
        case pad::kLeftStickBit: return Label::L3;
        case pad::kRightStickBit: return Label::R3;
        case pad::kStartBit: return Label::Start;
        case pad::kBackBit: return Label::Back;
        case pad::kHomeBit: return Label::Home;
        case pad::kCaptureBit: return Label::Capture;
        case pad::kDpadUpBit:
        case pad::kDpadDownBit:
        case pad::kDpadLeftBit:
        case pad::kDpadRightBit: return Label::Dpad;
        default: return Label::None;
        }
    }
    if (source.kind == SourceKind::AxisPositive || source.kind == SourceKind::AxisNegative) {
        switch (static_cast<PadAxis>(source.code)) {
        case PadAxis::LeftTrigger: return Label::L2;
        case PadAxis::RightTrigger: return Label::R2;
        case PadAxis::LeftX:
        case PadAxis::LeftY: return Label::LeftStick;
        case PadAxis::RightX:
        case PadAxis::RightY: return Label::RightStick;
        }
    }
    return Label::None;
}

[[nodiscard]] Label first_label(const input::TargetBindings& bindings) noexcept {
    for (const InputSource& source : bindings) {
        const Label label = label_for_source(source);
        if (label != Label::None) return label;
    }
    return Label::None;
}

// ---------------------------------------------------------------- drawing

struct Rgb {
    float r, g, b;
};
constexpr Rgb rgb8(int r, int g, int b) { return {r / 255.0f, g / 255.0f, b / 255.0f}; }

constexpr float kCx = 17.0f;
constexpr float kCy = 19.0f;

float sd_circle(float x, float y, float cx, float cy, float r) {
    return std::hypot(x - cx, y - cy) - r;
}
float sd_rbox(float x, float y, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::fabs(x - cx) - (hw - r);
    const float qy = std::fabs(y - cy) - (hh - r);
    return std::hypot(std::max(qx, 0.0f), std::max(qy, 0.0f)) + std::min(std::max(qx, qy), 0.0f) - r;
}
float sd_segment(float x, float y, float ax, float ay, float bx, float by) {
    const float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    const float t = std::clamp((px * dx + py * dy) / (dx * dx + dy * dy), 0.0f, 1.0f);
    return std::hypot(px - dx * t, py - dy * t);
}
// Triangle of either winding; exact inside, a lower bound outside (fine for
// strokes this thin).
float sd_triangle(float x, float y, const float (&p)[3][2]) {
    const float area = (p[1][0] - p[0][0]) * (p[2][1] - p[0][1]) - (p[1][1] - p[0][1]) * (p[2][0] - p[0][0]);
    const float orientation = area > 0.0f ? 1.0f : -1.0f;
    float d = -1e9f;
    for (int i = 0; i < 3; ++i) {
        const float ax = p[i][0], ay = p[i][1];
        const float ex = p[(i + 1) % 3][0] - ax, ey = p[(i + 1) % 3][1] - ay;
        d = std::max(d, orientation * ((x - ax) * ey - (y - ay) * ex) / std::hypot(ex, ey));
    }
    return d;
}
float ring(float d, float width) { return std::fabs(d) - width * 0.5f; }

class Canvas {
public:
    template <class Sdf>
    void fill(Sdf&& sdf, Rgb color, float opacity = 1.0f) {
        for (int y = 0; y < kCellH; ++y) {
            for (int x = 0; x < kCellW; ++x) {
                const float d = sdf(x + 0.5f, y + 0.5f);
                const float coverage = std::clamp(0.5f - d, 0.0f, 1.0f) * opacity;
                if (coverage > 0.0f) blend(y * kCellW + x, color, coverage);
            }
        }
    }
    // Rounded outline, rim, then body: the look of every button.
    template <class Sdf>
    void badge(Sdf&& sdf, Rgb body, Rgb rim) {
        fill([&](float x, float y) { return sdf(x, y) - 1.6f; }, rgb8(8, 10, 14));
        fill([&](float x, float y) { return sdf(x, y) - 0.3f; }, rim);
        fill([&](float x, float y) { return sdf(x, y) + 1.0f; }, body);
        // A soft light from above.
        fill([&](float x, float y) { return std::max(sdf(x, y) + 1.0f, y - (kCy - 2.0f)); },
             rgb8(255, 255, 255), 0.07f);
    }
    void text(const wchar_t* s, int pixel_height, float cx, float cy, Rgb color, float opacity = 1.0f);
    void to_rgba(std::uint8_t* out) const {
        for (int i = 0; i < kCellW * kCellH; ++i) {
            const float* q = px_[i];
            out[i * 4 + 0] = static_cast<std::uint8_t>(std::lround(std::clamp(q[0], 0.0f, 1.0f) * 255.0f));
            out[i * 4 + 1] = static_cast<std::uint8_t>(std::lround(std::clamp(q[1], 0.0f, 1.0f) * 255.0f));
            out[i * 4 + 2] = static_cast<std::uint8_t>(std::lround(std::clamp(q[2], 0.0f, 1.0f) * 255.0f));
            out[i * 4 + 3] = static_cast<std::uint8_t>(std::lround(std::clamp(q[3], 0.0f, 1.0f) * 255.0f));
        }
    }
    void blend(int index, Rgb c, float a) {
        float* q = px_[index];
        const float out_a = a + q[3] * (1.0f - a);
        if (out_a <= 0.0f) return;
        const float keep = q[3] * (1.0f - a);
        q[0] = (c.r * a + q[0] * keep) / out_a;
        q[1] = (c.g * a + q[1] * keep) / out_a;
        q[2] = (c.b * a + q[2] * keep) / out_a;
        q[3] = out_a;
    }

private:
    float px_[kCellW * kCellH][4]{};
};

// Antialiased text through GDI, cropped to its ink and centred by it.
struct TextRenderer {
    static constexpr int kSize = 64;
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    std::uint8_t* bits = nullptr;
    std::map<int, HFONT> fonts;

    bool ensure() {
        if (dc != nullptr) return true;
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = kSize;
        info.bmiHeader.biHeight = -kSize;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* data = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &data, nullptr, 0);
        if (bitmap == nullptr || data == nullptr) return false;
        bits = static_cast<std::uint8_t*>(data);
        SelectObject(dc, bitmap);
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, RGB(0, 0, 0));
        SetTextColor(dc, RGB(255, 255, 255));
        return true;
    }
    HFONT font(int pixel_height) {
        auto it = fonts.find(pixel_height);
        if (it != fonts.end()) return it->second;
        HFONT created = CreateFontW(-pixel_height, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                    DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        fonts[pixel_height] = created;
        return created;
    }
};

TextRenderer& text_renderer() {
    static TextRenderer renderer;
    return renderer;
}

void Canvas::text(const wchar_t* s, int pixel_height, float cx, float cy, Rgb color, float opacity) {
    TextRenderer& tr = text_renderer();
    if (!tr.ensure()) return;
    HFONT font = tr.font(pixel_height);
    if (font == nullptr) return;
    std::memset(tr.bits, 0, static_cast<std::size_t>(TextRenderer::kSize) * TextRenderer::kSize * 4u);
    SelectObject(tr.dc, font);
    TextOutW(tr.dc, 4, 4, s, static_cast<int>(std::wcslen(s)));
    GdiFlush();
    int x0 = TextRenderer::kSize, y0 = TextRenderer::kSize, x1 = -1, y1 = -1;
    for (int y = 0; y < TextRenderer::kSize; ++y) {
        for (int x = 0; x < TextRenderer::kSize; ++x) {
            if (tr.bits[(y * TextRenderer::kSize + x) * 4 + 1] > 24) {
                x0 = std::min(x0, x); x1 = std::max(x1, x);
                y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
        }
    }
    if (x1 < x0) return;
    const int w = x1 - x0 + 1, h = y1 - y0 + 1;
    const int left = static_cast<int>(std::lround(cx - w * 0.5f));
    const int top = static_cast<int>(std::lround(cy - h * 0.5f));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int dx = left + x, dy = top + y;
            if (dx < 0 || dy < 0 || dx >= kCellW || dy >= kCellH) continue;
            const float coverage =
                tr.bits[((y0 + y) * TextRenderer::kSize + (x0 + x)) * 4 + 1] / 255.0f * opacity;
            if (coverage > 0.0f) blend(dy * kCellW + dx, color, coverage);
        }
    }
}

constexpr Rgb kBody = rgb8(0x2a, 0x2e, 0x35);
constexpr Rgb kRim = rgb8(0xb9, 0xbf, 0xc8);
constexpr Rgb kInk = rgb8(0xf4, 0xf6, 0xf8);

void draw_bars(Canvas& c, int count, float half_width, float pitch) {
    for (int i = 0; i < count; ++i) {
        const float y = kCy + (i - (count - 1) * 0.5f) * pitch;
        c.fill([&](float px, float py) { return sd_rbox(px, py, kCx, y, half_width, 1.1f, 1.0f); }, kInk);
    }
}

void draw_house(Canvas& c) {
    const float roof[3][2] = {{kCx - 8.5f, kCy - 0.5f}, {kCx, kCy - 8.5f}, {kCx + 8.5f, kCy - 0.5f}};
    c.fill([&](float x, float y) { return sd_triangle(x, y, roof); }, kInk);
    c.fill([&](float x, float y) { return sd_rbox(x, y, kCx, kCy + 4.0f, 5.6f, 4.4f, 0.6f); }, kInk);
    c.fill([&](float x, float y) { return sd_rbox(x, y, kCx, kCy + 6.0f, 1.5f, 2.4f, 0.4f); }, kBody);
}

void draw_dpad(Canvas& c) {
    const auto cross = [](float x, float y, float grow) {
        return std::min(sd_rbox(x, y, kCx, kCy, 13.0f, 4.6f, 1.6f), sd_rbox(x, y, kCx, kCy, 4.6f, 13.0f, 1.6f)) -
               grow;
    };
    c.fill([&](float x, float y) { return cross(x, y, 1.6f); }, rgb8(8, 10, 14));
    c.fill([&](float x, float y) { return cross(x, y, 0.3f); }, kRim);
    c.fill([&](float x, float y) { return cross(x, y, -1.0f); }, kBody);
    const float s = 2.6f;
    const struct { float x, y; int dir; } arrows[4] = {{kCx, kCy - 9.4f, 0}, {kCx, kCy + 9.4f, 1},
                                                      {kCx - 9.4f, kCy, 2}, {kCx + 9.4f, kCy, 3}};
    for (const auto& a : arrows) {
        float t[3][2];
        if (a.dir == 0) { t[0][0] = a.x - s; t[0][1] = a.y + s * 0.8f; t[1][0] = a.x + s; t[1][1] = a.y + s * 0.8f; t[2][0] = a.x; t[2][1] = a.y - s * 0.8f; }
        else if (a.dir == 1) { t[0][0] = a.x - s; t[0][1] = a.y - s * 0.8f; t[1][0] = a.x; t[1][1] = a.y + s * 0.8f; t[2][0] = a.x + s; t[2][1] = a.y - s * 0.8f; }
        else if (a.dir == 2) { t[0][0] = a.x + s * 0.8f; t[0][1] = a.y - s; t[1][0] = a.x - s * 0.8f; t[1][1] = a.y; t[2][0] = a.x + s * 0.8f; t[2][1] = a.y + s; }
        else { t[0][0] = a.x - s * 0.8f; t[0][1] = a.y - s; t[1][0] = a.x - s * 0.8f; t[1][1] = a.y + s; t[2][0] = a.x + s * 0.8f; t[2][1] = a.y; }
        c.fill([&](float x, float y) { return sd_triangle(x, y, t); }, kInk);
    }
}

void draw_controller(Canvas& c) {
    const auto body = [](float x, float y) {
        return std::min({sd_rbox(x, y, kCx, kCy - 2.0f, 13.5f, 8.0f, 6.5f),
                         sd_rbox(x, y, kCx - 9.0f, kCy + 5.0f, 4.4f, 7.0f, 3.8f),
                         sd_rbox(x, y, kCx + 9.0f, kCy + 5.0f, 4.4f, 7.0f, 3.8f)});
    };
    c.badge(body, kBody, kRim);
    c.fill([&](float x, float y) { return sd_circle(x, y, kCx - 6.5f, kCy - 3.0f, 2.6f); }, kInk, 0.9f);
    c.fill([&](float x, float y) { return sd_circle(x, y, kCx + 3.0f, kCy + 2.0f, 2.6f); }, kInk, 0.9f);
    c.fill([&](float x, float y) {
        return std::min(sd_rbox(x, y, kCx - 3.0f, kCy + 2.0f, 2.4f, 0.8f, 0.4f), sd_rbox(x, y, kCx - 3.0f, kCy + 2.0f, 0.8f, 2.4f, 0.4f));
    }, kInk, 0.9f);
    const Rgb dots[4] = {rgb8(0x6d, 0xc3, 0x31), rgb8(0xf0, 0x45, 0x3b), rgb8(0x3d, 0x9b, 0xe9), rgb8(0xf8, 0xc2, 0x1c)};
    const float dx[4] = {0.0f, 2.8f, -2.8f, 0.0f}, dy[4] = {2.8f, 0.0f, 0.0f, -2.8f};
    for (int i = 0; i < 4; ++i) {
        c.fill([&](float x, float y) { return sd_circle(x, y, kCx + 7.6f + dx[i], kCy - 3.4f + dy[i], 1.5f); }, dots[i]);
    }
}

void draw_stick_icon(Canvas& c, const wchar_t* letter) {
    c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, kBody, kRim);
    c.fill([](float x, float y) { return sd_circle(x, y, kCx, kCy, 8.6f); }, rgb8(0x14, 0x17, 0x1b));
    c.fill([](float x, float y) { return sd_circle(x, y, kCx, kCy, 7.6f); }, rgb8(0xc9, 0xce, 0xd6));
    c.text(letter, 12, kCx, kCy, rgb8(0x20, 0x24, 0x2a));
}

void draw_face(Canvas& c, Family family, Label label) {
    c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, kBody, kRim);
    if (family == Family::Xbox) {
        const wchar_t* letter = label == Label::South ? L"A" : label == Label::East ? L"B"
                              : label == Label::West ? L"X" : L"Y";
        const Rgb color = label == Label::South ? rgb8(0x6d, 0xc3, 0x31) : label == Label::East ? rgb8(0xf2, 0x4a, 0x3f)
                        : label == Label::West ? rgb8(0x42, 0xa2, 0xf0) : rgb8(0xfb, 0xc5, 0x1d);
        c.text(letter, 19, kCx, kCy, color);
        return;
    }
    constexpr float w = 2.5f;
    switch (label) {
    case Label::South: {
        const Rgb color = rgb8(0x7f, 0xa9, 0xea);
        c.fill([&](float x, float y) {
            return std::min(sd_segment(x, y, kCx - 5.0f, kCy - 5.0f, kCx + 5.0f, kCy + 5.0f),
                            sd_segment(x, y, kCx - 5.0f, kCy + 5.0f, kCx + 5.0f, kCy - 5.0f)) - w * 0.5f;
        }, color);
        break;
    }
    case Label::East:
        c.fill([&](float x, float y) { return ring(sd_circle(x, y, kCx, kCy, 5.8f), w); }, rgb8(0xff, 0x6f, 0x7d));
        break;
    case Label::West:
        c.fill([&](float x, float y) { return ring(sd_rbox(x, y, kCx, kCy, 5.2f, 5.2f, 0.6f), w); }, rgb8(0xf2, 0x8e, 0xd8));
        break;
    default: {
        const float t[3][2] = {{kCx - 6.4f, kCy + 4.6f}, {kCx + 6.4f, kCy + 4.6f}, {kCx, kCy - 6.6f}};
        c.fill([&](float x, float y) { return ring(sd_triangle(x, y, t), w); }, rgb8(0x57, 0xd8, 0xa4));
        break;
    }
    }
}

void draw_shoulder(Canvas& c, const wchar_t* text) {
    c.badge([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 13.0f, 7.6f, 6.0f); }, kBody, kRim);
    c.text(text, 12, kCx, kCy, kInk);
}

void draw_trigger(Canvas& c, const wchar_t* text) {
    c.badge([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 11.2f, 12.4f, 6.0f); }, kBody, kRim);
    c.text(text, 12, kCx, kCy + 1.0f, kInk);
}

void draw_round_text(Canvas& c, const wchar_t* text, int size) {
    c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, kBody, kRim);
    c.text(text, size, kCx, kCy, kInk);
}

}  // namespace

void render_glyph(Family family, Label label, std::uint8_t* rgba) {
    Canvas c;
    const bool xbox = family == Family::Xbox;
    switch (label) {
    case Label::South: case Label::East: case Label::West: case Label::North:
        draw_face(c, family, label);
        break;
    case Label::L1: draw_shoulder(c, xbox ? L"LB" : L"L1"); break;
    case Label::R1: draw_shoulder(c, xbox ? L"RB" : L"R1"); break;
    case Label::L2: draw_trigger(c, xbox ? L"LT" : L"L2"); break;
    case Label::R2: draw_trigger(c, xbox ? L"RT" : L"R2"); break;
    case Label::L3: draw_round_text(c, xbox ? L"LS" : L"L3", 12); break;
    case Label::R3: draw_round_text(c, xbox ? L"RS" : L"R3", 12); break;
    case Label::Start:
        if (xbox) c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, kBody, kRim);
        else c.badge([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 13.0f, 9.2f, 9.2f); }, kBody, kRim);
        draw_bars(c, 3, 6.4f, 4.4f);
        break;
    case Label::Back:
        if (xbox) {
            c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, kBody, kRim);
            c.fill([](float x, float y) { return ring(sd_rbox(x, y, kCx - 2.2f, kCy - 2.2f, 4.6f, 4.6f, 1.0f), 1.9f); }, kInk);
            c.fill([](float x, float y) { return ring(sd_rbox(x, y, kCx + 2.2f, kCy + 2.2f, 4.6f, 4.6f, 1.0f), 1.9f); }, kInk);
        } else {
            c.badge([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 13.0f, 9.2f, 9.2f); }, kBody, kRim);
            draw_bars(c, 2, 6.4f, 5.4f);
        }
        break;
    case Label::Home:
        if (xbox) {
            c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, kBody, kRim);
            draw_house(c);
        } else {
            draw_round_text(c, L"PS", 13);
        }
        break;
    case Label::Capture:
        c.badge([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 13.4f, 9.4f, 3.4f); }, kBody, kRim);
        c.fill([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 7.0f, 1.0f, 1.0f); }, kInk, 0.8f);
        break;
    case Label::Dpad: draw_dpad(c); break;
    case Label::LeftStick: draw_stick_icon(c, L"L"); break;
    case Label::RightStick: draw_stick_icon(c, L"R"); break;
    case Label::Controller: draw_controller(c); break;
    case Label::Digit1: draw_round_text(c, L"1", 18); break;
    case Label::Digit2: draw_round_text(c, L"2", 18); break;
    case Label::None: break;
    }
    c.to_rgba(rgba);
}

std::uint16_t encode_rgb5a3(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) noexcept {
    if (a >= 248) {
        return static_cast<std::uint16_t>(0x8000u | ((r * 31u + 127u) / 255u) << 10 |
                                          ((g * 31u + 127u) / 255u) << 5 | ((b * 31u + 127u) / 255u));
    }
    const unsigned a3 = (a * 7u + 127u) / 255u;
    if (a3 == 0) return 0x0FFF;
    return static_cast<std::uint16_t>(a3 << 12 | ((r * 15u + 127u) / 255u) << 8 |
                                      ((g * 15u + 127u) / 255u) << 4 | ((b * 15u + 127u) / 255u));
}

std::optional<Family> family_for_scheme(input::ControlScheme scheme) noexcept {
    switch (scheme) {
    case input::ControlScheme::Xbox: return Family::Xbox;
    case input::ControlScheme::DualSense: return Family::DualSense;
    default: return std::nullopt;
    }
}

PromptSet make_prompt_set(Family family, const input::BindingTable& table) noexcept {
    PromptSet set;
    set.family = family;
    if (family == Family::None) return set;
    const auto pick = [&](WiiTarget target, Label fallback) {
        const Label label = first_label(table[target]);
        return label != Label::None ? label : fallback;
    };
    const auto at = [&](Slot slot) -> Label& { return set.labels[static_cast<std::size_t>(slot)]; };
    at(Slot::A) = pick(WiiTarget::A, Label::South);
    // Wii B is back/cancel in menus, which these controllers give the jump/back button.
    const Label jump_back = first_label(table[WiiTarget::JumpBack]);
    at(Slot::B) = jump_back != Label::None ? jump_back : pick(WiiTarget::B, Label::East);
    at(Slot::C) = pick(WiiTarget::C, Label::L3);
    at(Slot::Z) = pick(WiiTarget::Z, Label::L2);
    at(Slot::One) = pick(WiiTarget::One, Label::Digit1);
    at(Slot::Two) = pick(WiiTarget::Two, Label::Digit2);
    at(Slot::Plus) = pick(WiiTarget::Plus, Label::Start);
    at(Slot::Minus) = pick(WiiTarget::Minus, Label::Back);
    at(Slot::Home) = pick(WiiTarget::Home, Label::Home);
    at(Slot::Dpad) = Label::Dpad;
    at(Slot::Remote) = Label::Controller;
    const Label stick = first_label(table[WiiTarget::StickUp]);
    at(Slot::Nunchuk) = (stick == Label::LeftStick || stick == Label::RightStick) ? stick : Label::LeftStick;
    return set;
}

const char* label_name(Family family, Label label) noexcept {
    const bool xbox = family == Family::Xbox;
    switch (label) {
    case Label::South: return xbox ? "A" : "Cross";
    case Label::East: return xbox ? "B" : "Circle";
    case Label::West: return xbox ? "X" : "Square";
    case Label::North: return xbox ? "Y" : "Triangle";
    case Label::L1: return xbox ? "LB" : "L1";
    case Label::R1: return xbox ? "RB" : "R1";
    case Label::L2: return xbox ? "LT" : "L2";
    case Label::R2: return xbox ? "RT" : "R2";
    case Label::L3: return xbox ? "LS" : "L3";
    case Label::R3: return xbox ? "RS" : "R3";
    case Label::Start: return xbox ? "Menu" : "Options";
    case Label::Back: return xbox ? "View" : "Create";
    case Label::Home: return xbox ? "Home" : "PS";
    case Label::Capture: return xbox ? "Capture" : "Touchpad";
    case Label::Dpad: return "D-pad";
    case Label::LeftStick: return "Left stick";
    case Label::RightStick: return "Right stick";
    case Label::Controller: return "Controller";
    case Label::Digit1: return "1";
    case Label::Digit2: return "2";
    case Label::None: break;
    }
    return "none";
}

// ------------------------------------------------------------------ state

namespace {

std::mutex g_publish_mutex;
PromptSet g_published;
std::atomic<std::uint32_t> g_revision{0};

constexpr std::uint32_t kMem1 = 0x80000000u;
constexpr std::uint32_t kMem1Size = 0x01800000u;
constexpr std::uint32_t kMem2 = 0x90000000u;
constexpr std::uint32_t kMem2Size = 0x04000000u;
constexpr std::uint32_t kSliceBytes = 4u << 20;
constexpr std::uint32_t kSheetBytes = 0x8000u;
constexpr std::uint32_t kSheetCount = 6u;
constexpr std::uint32_t kSheetsOffset = 0x60u;
constexpr std::uint32_t kTglpOffset = 48u;
// Compared up to the sheet size: the game turns the last field (the sheet
// offset) into a pointer when it sets the font up.
constexpr std::size_t kTglpMatch = 28;

// The TGLP header of the picture font: cell 34x35, six 128x128 RGB5A3 sheets
// of 3x3 cells, sheet data at 0x60 (structure only, nothing of the art).
constexpr std::uint8_t kTglp[32] = {'T', 'G', 'L', 'P', 0x00, 0x03, 0x00, 0x30, 0x22, 0x23, 0x1A,
                                    0x1E, 0x00, 0x00, 0x80, 0x00, 0x00, 0x06, 0x00, 0x05, 0x00,
                                    0x03, 0x00, 0x03, 0x00, 0x80, 0x00, 0x80, 0x00, 0x00, 0x00, 0x60};

struct FontCopy {
    std::uint32_t base = 0;                // guest address the TGLP header sits 48 bytes after
    std::uint32_t sheets = 0;              // guest address of the first sheet
    std::vector<std::uint8_t> original;    // the six sheets as found
};

// The HUD A button (the "talk" prompt, layout IconAButton): IA4 textures found by
// a hash of 16 bytes of their pixels and then of all of them (the game's bytes
// are never stored). 48x48 and 40x40, 8x4-pixel tiles.
struct IconTarget {
    int size;
    std::uint32_t window_offset;  // 32-aligned offset of the 16 bytes that are hashed first
    std::uint64_t window_hash;
    std::uint64_t full_hash;
};
constexpr IconTarget kIconTargets[2] = {
    {48, 1504u, 0x0027A459775D4E12ull, 0x8B364481CA20AEABull},
    {40, 960u, 0x00469AB469D6E02Full, 0x4531CBD39A3F26AAull},
};

std::uint64_t fnv64(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}

struct IconCopy {
    std::uint32_t address = 0;
    int target = 0;
    std::vector<std::uint8_t> original;
    std::uint64_t current_hash = 0;  // of the bytes as last drawn (or found)
};

struct State {
    std::vector<IconCopy> icons;
    std::map<std::uint32_t, std::vector<std::uint8_t>> icon_pixels;
    std::vector<FontCopy> fonts;
    std::uint32_t seen_revision = 0;
    bool announced = false;
    bool dirty = false;  // the fonts need drawing with `applied`
    PromptSet applied;
    std::uint32_t cursor = 0;  // scan position over MEM1 then MEM2
    ULONGLONG next_scan = 0;
    ULONGLONG next_verify = 0;
    ULONGLONG next_icon_verify = 0;
    std::map<std::uint32_t, std::vector<std::uint16_t>> cells;
    int debug = -1;
};

State& state() {
    static State s;
    return s;
}

void log_line(const NativeServicesV1* services, State& s, const char* message) {
    if (s.debug < 0) {
        char value[8]{};
        std::size_t length = 0;
        s.debug = (getenv_s(&length, value, sizeof(value), "NEBULA_PROMPTS_DEBUG") == 0 && length > 1) ? 1 : 0;
    }
    if (s.debug == 1) std::fprintf(stderr, "[prompts] %s\n", message);
    if (s.debug == 1 && services != nullptr && services->log != nullptr) {
        services->log(services->user, LogLevelV1::Info, message);
    }
}

const std::vector<std::uint16_t>& cell_pixels(State& s, Family family, Label label) {
    const std::uint32_t key = static_cast<std::uint32_t>(family) << 8 | static_cast<std::uint32_t>(label);
    auto it = s.cells.find(key);
    if (it != s.cells.end()) return it->second;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kCellW) * kCellH * 4u);
    render_glyph(family, label, rgba.data());
    std::vector<std::uint16_t> out(static_cast<std::size_t>(kCellW) * kCellH);
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = encode_rgb5a3(rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]);
    }
    return s.cells.emplace(key, std::move(out)).first->second;
}

// Where the sheets are: the TGLP sheet field is an offset from the font start in
// the file and a guest pointer once the game has set the font up.
std::uint32_t sheets_address(GuestMemoryV1* memory, std::uint32_t base) {
    const std::byte* p = resolve_guest_fast(memory, base + kTglpOffset + 28u, 4u);
    if (p == nullptr) return 0;
    std::uint32_t value = 0;
    std::memcpy(&value, p, 4);
    value = byte_swap_u32(value);
    return value >= kMem1 ? value : base + value;
}

bool header_ok(GuestMemoryV1* memory, std::uint32_t base) {
    const std::byte* p = resolve_guest_fast(memory, base, kTglpOffset + 32u);
    if (p == nullptr) return false;
    return std::memcmp(p + kTglpOffset, kTglp, kTglpMatch) == 0;
}

void scan_slice(GuestMemoryV1* memory, State& s, const NativeServicesV1* services) {
    constexpr std::uint32_t total = kMem1Size + kMem2Size;
    const std::uint32_t offset = s.cursor;
    const bool in_mem1 = offset < kMem1Size;
    const std::uint32_t region_base = in_mem1 ? kMem1 : kMem2;
    const std::uint32_t region_size = in_mem1 ? kMem1Size : kMem2Size;
    const std::uint32_t local = in_mem1 ? offset : offset - kMem1Size;
    const std::uint32_t address = region_base + local;
    std::uint32_t length = std::min(kSliceBytes, region_size - local);
    std::uint32_t read = std::min(length + 31u, region_size - local);
    s.cursor = offset + length >= total ? 0u : offset + length;
    if (s.cursor == 0u) {
        char cycle[96];
        std::snprintf(cycle, sizeof(cycle), "scan cycle done, fonts=%zu", s.fonts.size());
        log_line(services, s, cycle);
    }
    const std::byte* p = resolve_guest_fast(memory, address, read);
    if (p == nullptr || read < 32u) return;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(p);
    std::uint32_t i = 0;
    while (i + 32u <= read) {
        const void* hit = std::memchr(bytes + i, 'T', read - 32u - i + 1u);
        if (hit == nullptr) break;
        i = static_cast<std::uint32_t>(static_cast<const std::uint8_t*>(hit) - bytes);
        if (i >= length) break;
        if (std::memcmp(bytes + i, kTglp, kTglpMatch) == 0 && address + i >= kTglpOffset) {
            const std::uint32_t base = address + i - kTglpOffset;
            const bool known = std::any_of(s.fonts.begin(), s.fonts.end(),
                                           [&](const FontCopy& f) { return f.base == base; });
            if (!known && header_ok(memory, base)) {
                const std::byte* sheets =
                    resolve_guest_fast(memory, sheets_address(memory, base), kSheetBytes * kSheetCount);
                if (sheets != nullptr) {
                    FontCopy copy;
                    copy.base = base;
                    copy.sheets = sheets_address(memory, base);
                    copy.original.assign(reinterpret_cast<const std::uint8_t*>(sheets),
                                         reinterpret_cast<const std::uint8_t*>(sheets) + kSheetBytes * kSheetCount);
                    s.fonts.push_back(std::move(copy));
                    s.dirty = true;  // draw into the new copy
                    char message[96];
                    std::snprintf(message, sizeof(message), "button prompts: picture font at 0x%08X", base);
                    log_line(services, s, message);
                }
            }
        }
        ++i;
    }
    // The HUD A icon: 32-aligned windows.
    const std::uint32_t first = (address + 31u) & ~31u;
    for (std::uint32_t a = first; a + 16u <= address + length && a + 16u - address <= read; a += 32u) {
        const std::uint64_t window = fnv64(bytes + (a - address), 16);
        for (int t = 0; t < 2; ++t) {
            const IconTarget& target = kIconTargets[t];
            if (window != target.window_hash || a < target.window_offset) continue;
            const std::uint32_t start = a - target.window_offset;
            const std::size_t size = static_cast<std::size_t>(target.size) * target.size;
            const bool known = std::any_of(s.icons.begin(), s.icons.end(),
                                           [&](const IconCopy& c) { return c.address == start; });
            const std::byte* data = known ? nullptr : resolve_guest_fast(memory, start, static_cast<std::uint32_t>(size));
            if (data == nullptr) continue;
            const auto* raw = reinterpret_cast<const std::uint8_t*>(data);
            if (fnv64(raw, size) != target.full_hash) continue;
            IconCopy copy;
            copy.address = start;
            copy.target = t;
            copy.original.assign(raw, raw + size);
            copy.current_hash = target.full_hash;
            s.icons.push_back(std::move(copy));
            s.dirty = true;
            char message[96];
            std::snprintf(message, sizeof(message), "button prompts: A icon %dx%d at 0x%08X", target.size, target.size, start);
            log_line(services, s, message);
        }
    }
}

// The glyph of `label` as an IA4 icon of size x size (bilinear, premultiplied).
const std::vector<std::uint8_t>& icon_pixels(State& s, Family family, Label label, int size) {
    const std::uint32_t key = static_cast<std::uint32_t>(family) << 16 | static_cast<std::uint32_t>(label) << 8 |
                              static_cast<std::uint32_t>(size);
    auto it = s.icon_pixels.find(key);
    if (it != s.icon_pixels.end()) return it->second;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kCellW) * kCellH * 4u);
    render_glyph(family, label, rgba.data());
    const auto texel = [&](int x, int y, float out[4]) {
        if (x < 0 || y < 0 || x >= kCellW || y >= kCellH) { out[0] = out[1] = out[2] = out[3] = 0.0f; return; }
        const std::uint8_t* q = &rgba[(static_cast<std::size_t>(y) * kCellW + x) * 4];
        const float a = q[3] / 255.0f;
        out[0] = q[0] / 255.0f * a; out[1] = q[1] / 255.0f * a; out[2] = q[2] / 255.0f * a; out[3] = a;
    };
    const float scale = size / 30.0f;  // the 28 px of art fill the icon
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size) * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float sx = 17.0f + (x + 0.5f - size * 0.5f) / scale - 0.5f;
            const float sy = 19.0f + (y + 0.5f - size * 0.5f) / scale - 0.5f;
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            const float fx = sx - x0, fy = sy - y0;
            float acc[4] = {0, 0, 0, 0}, t[4];
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    texel(x0 + i, y0 + j, t);
                    const float w = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
                    for (int c = 0; c < 4; ++c) acc[c] += t[c] * w;
                }
            }
            const float a = acc[3];
            const float luma = a > 0.0f ? (0.299f * acc[0] + 0.587f * acc[1] + 0.114f * acc[2]) / a : 0.0f;
            const int a4 = std::clamp(static_cast<int>(std::lround(a * 15.0f)), 0, 15);
            const int i4 = std::clamp(static_cast<int>(std::lround(luma * 15.0f)), 0, 15);
            const std::size_t off = (static_cast<std::size_t>(y / 4) * (size / 8) + x / 8) * 32u +
                                    static_cast<std::size_t>(y % 4) * 8u + x % 8;
            data[off] = static_cast<std::uint8_t>(a4 << 4 | i4);
        }
    }
    return s.icon_pixels.emplace(key, std::move(data)).first->second;
}

void draw_icon(GuestMemoryV1* memory, const NativeServicesV1* services, std::uint32_t pc, State& s,
               IconCopy& icon, const PromptSet& want) {
    const int size = kIconTargets[icon.target].size;
    const std::size_t bytes = static_cast<std::size_t>(size) * size;
    std::byte* data = resolve_guest_fast(memory, icon.address, static_cast<std::uint32_t>(bytes));
    if (data == nullptr) return;
    const Label label = want.labels[static_cast<std::size_t>(Slot::A)];
    const std::vector<std::uint8_t>& target =
        want.family != Family::None && label != Label::None ? icon_pixels(s, want.family, label, size)
                                                            : icon.original;
    auto* current = reinterpret_cast<std::uint8_t*>(data);
    for (std::size_t i = 0; i < bytes; ++i) {
        if (current[i] != target[i]) guest_store_u8(memory, icon.address + static_cast<std::uint32_t>(i), target[i], services, pc);
    }
    icon.current_hash = fnv64(current, bytes);
}

void draw_font(GuestMemoryV1* memory, const NativeServicesV1* services, std::uint32_t pc, State& s,
               const FontCopy& font, const PromptSet& want) {
    std::byte* sheets = resolve_guest_fast(memory, font.sheets, kSheetBytes * kSheetCount);
    if (sheets == nullptr) return;
    for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
        const std::uint32_t glyph = kSlotGlyph[slot];
        const std::uint32_t sheet = glyph / 9u, cell = glyph % 9u;
        const std::uint32_t ox = (cell % 3u) * 35u, oy = (cell / 3u) * 36u;
        const std::vector<std::uint16_t>* target = nullptr;
        if (want.family != Family::None && want.labels[slot] != Label::None) {
            target = &cell_pixels(s, want.family, want.labels[slot]);
        }
        for (std::uint32_t y = 0; y < static_cast<std::uint32_t>(kCellH); ++y) {
            for (std::uint32_t x = 0; x < static_cast<std::uint32_t>(kCellW); ++x) {
                const std::uint32_t px = ox + x, py = oy + y;
                const std::uint32_t off = sheet * kSheetBytes + ((py >> 2) * 32u + (px >> 2)) * 32u +
                                          ((py & 3u) * 4u + (px & 3u)) * 2u;
                std::uint16_t value;
                if (target != nullptr) {
                    value = (*target)[y * kCellW + x];
                } else {
                    value = static_cast<std::uint16_t>(font.original[off] << 8 | font.original[off + 1]);
                }
                const auto* current = reinterpret_cast<const std::uint8_t*>(sheets) + off;
                if ((current[0] << 8 | current[1]) != value) {
                    guest_store_u16(memory, font.sheets + off, value, services, pc);
                }
            }
        }
    }
}

}  // namespace

namespace {
// NEBULA_PROMPTS_FORCE=xbox|dualsense shows that family's default prompts
// whatever controller is connected (testing without the pad).
const std::optional<Family>& forced_family() {
    static const std::optional<Family> forced = [] () -> std::optional<Family> {
        char value[16]{};
        std::size_t length = 0;
        if (getenv_s(&length, value, sizeof(value), "NEBULA_PROMPTS_FORCE") != 0 || length < 2) return std::nullopt;
        if (std::strcmp(value, "xbox") == 0) return Family::Xbox;
        if (std::strcmp(value, "dualsense") == 0) return Family::DualSense;
        return std::nullopt;
    }();
    return forced;
}
}  // namespace

void publish(const PromptSet& requested) noexcept {
    PromptSet set = requested;
    if (const auto& forced = forced_family()) {
        set = make_prompt_set(*forced, input::default_binding_table(
            *forced == Family::Xbox ? input::ControlScheme::Xbox : input::ControlScheme::DualSense));
    }
    std::lock_guard lock(g_publish_mutex);
    if (set == g_published) return;
    g_published = set;
    g_revision.fetch_add(1, std::memory_order_release);
}

PromptSet published() noexcept {
    std::lock_guard lock(g_publish_mutex);
    return g_published;
}

void tick(GuestMemoryV1* memory, const NativeServicesV1* services, std::uint32_t pc) noexcept {
    try {
        State& s = state();
        const std::uint32_t revision = g_revision.load(std::memory_order_acquire);
        if (revision != s.seen_revision) {
            s.applied = published();
            s.seen_revision = revision;
            s.dirty = true;
        }
        if (s.applied.family == Family::None && !s.dirty) return;
        if (!s.announced) { s.announced = true; log_line(services, s, "tick active"); }
        const ULONGLONG now = GetTickCount64();
        // Look for the font (and for more copies of it) while a controller is in use.
        if (s.applied.family != Family::None && now >= s.next_scan) {
            scan_slice(memory, s, services);
            s.next_scan = now + (s.fonts.empty() && s.icons.empty() ? 40u : 100u);
        }
        if (!s.fonts.empty() && now >= s.next_verify) {
            s.next_verify = now + 3000u;
            const std::size_t before = s.fonts.size();
            std::erase_if(s.fonts, [&](const FontCopy& f) { return !header_ok(memory, f.base); });
            if (s.fonts.size() != before) log_line(services, s, "button prompts: a picture font went away");
        }
        if (!s.icons.empty() && now >= s.next_icon_verify) {
            s.next_icon_verify = now + 3000u;
            std::erase_if(s.icons, [&](const IconCopy& c) {
                const int size = kIconTargets[c.target].size;
                const std::byte* data = resolve_guest_fast(memory, c.address, static_cast<std::uint32_t>(size * size));
                if (data == nullptr) return true;
                const std::uint64_t hash = fnv64(reinterpret_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size) * size);
                return hash != c.current_hash && hash != kIconTargets[c.target].full_hash;
            });
        }
        if (s.dirty) {
            for (IconCopy& icon : s.icons) draw_icon(memory, services, pc, s, icon, s.applied);
            for (const FontCopy& font : s.fonts) draw_font(memory, services, pc, s, font, s.applied);
            s.dirty = false;
        }
    } catch (...) {
    }
}

}  // namespace galaxy::prompts
