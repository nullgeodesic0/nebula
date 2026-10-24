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
    // `scale` multiplies every coordinate the drawing code uses (cell units).
    explicit Canvas(float scale = 1.0f, float cell_w = kCellW, float cell_h = kCellH)
        : scale_(scale),
          w_(static_cast<int>(std::ceil(cell_w * scale))),
          h_(static_cast<int>(std::ceil(cell_h * scale))),
          px_(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_) * 4u, 0.0f) {}
    // Button colours (the DualSense family is the pale one).
    struct Palette {
        Rgb body, rim, ink;
    } pal{rgb8(0x2a, 0x2e, 0x35), rgb8(0xb9, 0xbf, 0xc8), rgb8(0xf4, 0xf6, 0xf8)};
    [[nodiscard]] int width() const { return w_; }
    [[nodiscard]] int height() const { return h_; }

    template <class Sdf>
    void fill(Sdf&& sdf, Rgb color, float opacity = 1.0f) {
        for (int y = 0; y < h_; ++y) {
            for (int x = 0; x < w_; ++x) {
                const float d = sdf((x + 0.5f) / scale_, (y + 0.5f) / scale_);
                const float coverage = std::clamp(0.5f - d * scale_, 0.0f, 1.0f) * opacity;
                if (coverage > 0.0f) blend(y * w_ + x, color, coverage);
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
    void sky(Rgb top, Rgb bottom) {
        for (int y = 0; y < h_; ++y) {
            const float k = h_ > 1 ? static_cast<float>(y) / static_cast<float>(h_ - 1) : 0.0f;
            const Rgb c{top.r + (bottom.r - top.r) * k, top.g + (bottom.g - top.g) * k, top.b + (bottom.b - top.b) * k};
            for (int x = 0; x < w_; ++x) blend(y * w_ + x, c, 1.0f);
        }
    }
    void text(const wchar_t* s, int pixel_height, float cx, float cy, Rgb color, float opacity = 1.0f);
    void to_rgba(std::uint8_t* out) const {
        for (std::size_t i = 0; i < static_cast<std::size_t>(w_) * h_; ++i) {
            const float* q = &px_[i * 4];
            for (int c = 0; c < 4; ++c) {
                out[i * 4 + c] = static_cast<std::uint8_t>(std::lround(std::clamp(q[c], 0.0f, 1.0f) * 255.0f));
            }
        }
    }
    void blend(int index, Rgb c, float a) {
        float* q = &px_[static_cast<std::size_t>(index) * 4];
        const float out_a = a + q[3] * (1.0f - a);
        if (out_a <= 0.0f) return;
        const float keep = q[3] * (1.0f - a);
        q[0] = (c.r * a + q[0] * keep) / out_a;
        q[1] = (c.g * a + q[1] * keep) / out_a;
        q[2] = (c.b * a + q[2] * keep) / out_a;
        q[3] = out_a;
    }

private:
    float scale_;
    int w_, h_;
    std::vector<float> px_;
};
// Antialiased text through GDI, cropped to its ink and centred by it.
struct TextRenderer {
    static constexpr int kSize = 256;
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

// One per thread: the simulation thread draws the in-memory prompts while the render thread draws the HD ones.
TextRenderer& text_renderer() {
    thread_local TextRenderer renderer;
    return renderer;
}

void Canvas::text(const wchar_t* s, int pixel_height, float cx, float cy, Rgb color, float opacity) {
    TextRenderer& tr = text_renderer();
    if (!tr.ensure()) return;
    HFONT font = tr.font(std::max(1, static_cast<int>(std::lround(pixel_height * scale_))));
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
    const int left = static_cast<int>(std::lround(cx * scale_ - w * 0.5f));
    const int top = static_cast<int>(std::lround(cy * scale_ - h * 0.5f));
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const int dx = left + x, dy = top + y;
            if (dx < 0 || dy < 0 || dx >= w_ || dy >= h_) continue;
            const float coverage =
                tr.bits[((y0 + y) * TextRenderer::kSize + (x0 + x)) * 4 + 1] / 255.0f * opacity;
            if (coverage > 0.0f) blend(dy * w_ + dx, color, coverage);
        }
    }
}
constexpr Rgb kBody = rgb8(0x2a, 0x2e, 0x35);
constexpr Rgb kRim = rgb8(0xb9, 0xbf, 0xc8);
constexpr Rgb kInk = rgb8(0xf4, 0xf6, 0xf8);




void draw_bars(Canvas& c, int count, float half_width, float pitch) {
    for (int i = 0; i < count; ++i) {
        const float y = kCy + (i - (count - 1) * 0.5f) * pitch;
        c.fill([&](float px, float py) { return sd_rbox(px, py, kCx, y, half_width, 1.1f, 1.0f); }, c.pal.ink);
    }
}

void draw_house(Canvas& c) {
    const float roof[3][2] = {{kCx - 8.5f, kCy - 0.5f}, {kCx, kCy - 8.5f}, {kCx + 8.5f, kCy - 0.5f}};
    c.fill([&](float x, float y) { return sd_triangle(x, y, roof); }, c.pal.ink);
    c.fill([&](float x, float y) { return sd_rbox(x, y, kCx, kCy + 4.0f, 5.6f, 4.4f, 0.6f); }, c.pal.ink);
    c.fill([&](float x, float y) { return sd_rbox(x, y, kCx, kCy + 6.0f, 1.5f, 2.4f, 0.4f); }, c.pal.body);
}

void draw_dpad(Canvas& c) {
    const auto cross = [](float x, float y, float grow) {
        return std::min(sd_rbox(x, y, kCx, kCy, 13.0f, 4.6f, 1.6f), sd_rbox(x, y, kCx, kCy, 4.6f, 13.0f, 1.6f)) -
               grow;
    };
    c.fill([&](float x, float y) { return cross(x, y, 1.6f); }, rgb8(8, 10, 14));
    c.fill([&](float x, float y) { return cross(x, y, 0.3f); }, c.pal.rim);
    c.fill([&](float x, float y) { return cross(x, y, -1.0f); }, c.pal.body);
    const float s = 2.6f;
    const struct { float x, y; int dir; } arrows[4] = {{kCx, kCy - 9.4f, 0}, {kCx, kCy + 9.4f, 1},
                                                      {kCx - 9.4f, kCy, 2}, {kCx + 9.4f, kCy, 3}};
    for (const auto& a : arrows) {
        float t[3][2];
        if (a.dir == 0) { t[0][0] = a.x - s; t[0][1] = a.y + s * 0.8f; t[1][0] = a.x + s; t[1][1] = a.y + s * 0.8f; t[2][0] = a.x; t[2][1] = a.y - s * 0.8f; }
        else if (a.dir == 1) { t[0][0] = a.x - s; t[0][1] = a.y - s * 0.8f; t[1][0] = a.x; t[1][1] = a.y + s * 0.8f; t[2][0] = a.x + s; t[2][1] = a.y - s * 0.8f; }
        else if (a.dir == 2) { t[0][0] = a.x + s * 0.8f; t[0][1] = a.y - s; t[1][0] = a.x - s * 0.8f; t[1][1] = a.y; t[2][0] = a.x + s * 0.8f; t[2][1] = a.y + s; }
        else { t[0][0] = a.x - s * 0.8f; t[0][1] = a.y - s; t[1][0] = a.x - s * 0.8f; t[1][1] = a.y + s; t[2][0] = a.x + s * 0.8f; t[2][1] = a.y; }
        c.fill([&](float x, float y) { return sd_triangle(x, y, t); }, c.pal.ink);
    }
}

void draw_controller(Canvas& c) {
    const auto body = [](float x, float y) {
        return std::min({sd_rbox(x, y, kCx, kCy - 2.0f, 13.5f, 8.0f, 6.5f),
                         sd_rbox(x, y, kCx - 9.0f, kCy + 5.0f, 4.4f, 7.0f, 3.8f),
                         sd_rbox(x, y, kCx + 9.0f, kCy + 5.0f, 4.4f, 7.0f, 3.8f)});
    };
    c.badge(body, c.pal.body, c.pal.rim);
    c.fill([&](float x, float y) { return sd_circle(x, y, kCx - 6.5f, kCy - 3.0f, 2.6f); }, c.pal.ink, 0.9f);
    c.fill([&](float x, float y) { return sd_circle(x, y, kCx + 3.0f, kCy + 2.0f, 2.6f); }, c.pal.ink, 0.9f);
    c.fill([&](float x, float y) {
        return std::min(sd_rbox(x, y, kCx - 3.0f, kCy + 2.0f, 2.4f, 0.8f, 0.4f), sd_rbox(x, y, kCx - 3.0f, kCy + 2.0f, 0.8f, 2.4f, 0.4f));
    }, c.pal.ink, 0.9f);
    const Rgb dots[4] = {rgb8(0x6d, 0xc3, 0x31), rgb8(0xf0, 0x45, 0x3b), rgb8(0x3d, 0x9b, 0xe9), rgb8(0xf8, 0xc2, 0x1c)};
    const float dx[4] = {0.0f, 2.8f, -2.8f, 0.0f}, dy[4] = {2.8f, 0.0f, 0.0f, -2.8f};
    for (int i = 0; i < 4; ++i) {
        c.fill([&](float x, float y) { return sd_circle(x, y, kCx + 7.6f + dx[i], kCy - 3.4f + dy[i], 1.5f); }, dots[i]);
    }
}

void draw_stick_icon(Canvas& c, const wchar_t* letter) {
    c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, c.pal.body, c.pal.rim);
    c.fill([](float x, float y) { return sd_circle(x, y, kCx, kCy, 8.6f); }, rgb8(0x14, 0x17, 0x1b));
    c.fill([](float x, float y) { return sd_circle(x, y, kCx, kCy, 7.6f); }, rgb8(0xc9, 0xce, 0xd6));
    c.text(letter, 12, kCx, kCy, rgb8(0x20, 0x24, 0x2a));
}

void draw_face(Canvas& c, Family family, Label label) {
    c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, c.pal.body, c.pal.rim);
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
        const Rgb color = rgb8(0x3d, 0x6f, 0xd6);
        c.fill([&](float x, float y) {
            return std::min(sd_segment(x, y, kCx - 5.0f, kCy - 5.0f, kCx + 5.0f, kCy + 5.0f),
                            sd_segment(x, y, kCx - 5.0f, kCy + 5.0f, kCx + 5.0f, kCy - 5.0f)) - w * 0.5f;
        }, color);
        break;
    }
    case Label::East:
        c.fill([&](float x, float y) { return ring(sd_circle(x, y, kCx, kCy, 5.8f), w); }, rgb8(0xe0, 0x41, 0x4f));
        break;
    case Label::West:
        c.fill([&](float x, float y) { return ring(sd_rbox(x, y, kCx, kCy, 5.2f, 5.2f, 0.6f), w); }, rgb8(0xd4, 0x50, 0x9f));
        break;
    default: {
        const float t[3][2] = {{kCx - 6.4f, kCy + 4.6f}, {kCx + 6.4f, kCy + 4.6f}, {kCx, kCy - 6.6f}};
        c.fill([&](float x, float y) { return ring(sd_triangle(x, y, t), w); }, rgb8(0x1f, 0xae, 0x7e));
        break;
    }
    }
}

void draw_shoulder(Canvas& c, const wchar_t* text) {
    c.badge([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 13.0f, 7.6f, 6.0f); }, c.pal.body, c.pal.rim);
    c.text(text, 12, kCx, kCy, c.pal.ink);
}

void draw_trigger(Canvas& c, const wchar_t* text) {
    c.badge([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 11.2f, 12.4f, 6.0f); }, c.pal.body, c.pal.rim);
    c.text(text, 12, kCx, kCy + 1.0f, c.pal.ink);
}

void draw_round_text(Canvas& c, const wchar_t* text, int size) {
    c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, c.pal.body, c.pal.rim);
    c.text(text, size, kCx, kCy, c.pal.ink);
}

// ------------------------------------------------------------ guidance art

float smooth_min(float a, float b, float k) {
    const float h = std::clamp(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);
    return b + (a - b) * h - k * h * (1.0f - h);
}

// A whole controller seen from the front, `s` cell units per controller unit
// (the body is 25 units wide), turned by `degrees`. Drawn for the tutorial
// pictures (tilt, spin, ball).
void draw_controller_art(Canvas& c, Family family, float cx, float cy, float s, float degrees) {
    const float angle = degrees * 3.14159265f / 180.0f;
    const float ca = std::cos(angle), sa = std::sin(angle);
    const auto local = [&](float x, float y, float& u, float& v) {
        const float dx = x - cx, dy = y - cy;
        u = (dx * ca + dy * sa) / s;
        v = (-dx * sa + dy * ca) / s;
    };
    const auto body = [&](float x, float y) {
        float u, v;
        local(x, y, u, v);
        const float center = sd_rbox(u, v, 0.0f, -1.0f, 11.8f, 6.2f, 5.0f);
        const float left = sd_rbox(u, v, -8.6f, 5.0f, 3.9f, 7.2f, 3.7f);
        const float right = sd_rbox(u, v, 8.6f, 5.0f, 3.9f, 7.2f, 3.7f);
        return smooth_min(smooth_min(center, left, 3.0f), right, 3.0f) * s;
    };
    const bool xbox = family == Family::Xbox;
    const Rgb outline = rgb8(8, 10, 14);
    const Rgb shell = xbox ? rgb8(0x2b, 0x2f, 0x36) : rgb8(0xee, 0xf1, 0xf5);
    // Bumpers behind the body.
    for (const float side : {-1.0f, 1.0f}) {
        c.fill([&](float x, float y) {
            float u, v;
            local(x, y, u, v);
            return (sd_rbox(u, v, side * 8.4f, -7.4f, 4.2f, 1.5f, 1.2f) - 0.5f) * s;
        }, outline);
        c.fill([&](float x, float y) {
            float u, v;
            local(x, y, u, v);
            return sd_rbox(u, v, side * 8.4f, -7.4f, 4.2f, 1.5f, 1.2f) * s;
        }, xbox ? rgb8(0x4a, 0x4f, 0x58) : rgb8(0xdd, 0xe1, 0xe7));
    }
    c.fill([&](float x, float y) { return body(x, y) - 0.45f * s; }, outline);
    c.fill(body, shell);
    c.fill([&](float x, float y) {  // light from above
        float u, v;
        local(x, y, u, v);
        return std::max(body(x, y) + 0.6f * s, v + 0.5f * 6.0f - 3.0f);
    }, rgb8(255, 255, 255), xbox ? 0.07f : 0.35f);
    const auto disc = [&](float u0, float v0, float r, Rgb color, float opacity = 1.0f) {
        c.fill([&](float x, float y) {
            float u, v;
            local(x, y, u, v);
            return (std::hypot(u - u0, v - v0) - r) * s;
        }, color, opacity);
    };
    const auto bar = [&](float u0, float v0, float hw, float hh, float r, Rgb color, float opacity = 1.0f) {
        c.fill([&](float x, float y) {
            float u, v;
            local(x, y, u, v);
            return sd_rbox(u, v, u0, v0, hw, hh, r) * s;
        }, color, opacity);
    };
    const Rgb green = rgb8(0x6d, 0xc3, 0x31), red = rgb8(0xf2, 0x4a, 0x3f);
    const Rgb blue = rgb8(0x42, 0xa2, 0xf0), yellow = rgb8(0xfb, 0xc5, 0x1d);
    if (xbox) {
        disc(0.0f, -3.4f, 1.9f, rgb8(0xe9, 0xec, 0xf0));   // the Xbox button
        disc(0.0f, -3.4f, 1.2f, rgb8(0x2b, 0x2f, 0x36));
        disc(-6.4f, -2.2f, 2.9f, rgb8(0x14, 0x17, 0x1b));  // left stick
        disc(-6.4f, -2.2f, 2.0f, rgb8(0x56, 0x5c, 0x66));
        disc(3.6f, 2.8f, 2.9f, rgb8(0x14, 0x17, 0x1b));    // right stick
        disc(3.6f, 2.8f, 2.0f, rgb8(0x56, 0x5c, 0x66));
        bar(-3.4f, 2.8f, 2.6f, 0.9f, 0.4f, rgb8(0xc9, 0xce, 0xd6));  // d-pad
        bar(-3.4f, 2.8f, 0.9f, 2.6f, 0.4f, rgb8(0xc9, 0xce, 0xd6));
        disc(6.4f, -4.1f, 1.1f, yellow);                    // Y
        disc(6.4f, -0.3f, 1.1f, green);                     // A
        disc(4.5f, -2.2f, 1.1f, blue);                      // X
        disc(8.3f, -2.2f, 1.1f, red);                       // B
        bar(-1.5f, -1.0f, 0.9f, 0.5f, 0.3f, rgb8(0xc9, 0xce, 0xd6), 0.9f);
        bar(1.5f, -1.0f, 0.9f, 0.5f, 0.3f, rgb8(0xc9, 0xce, 0xd6), 0.9f);
    } else {
        bar(0.0f, -1.2f, 6.6f, 5.6f, 2.6f, rgb8(0x17, 0x19, 0x1d));  // the dark middle
        bar(0.0f, -3.4f, 4.4f, 2.0f, 1.2f, rgb8(0x3d, 0x41, 0x4a));  // touch pad
        bar(-5.2f, -1.4f, 0.35f, 3.4f, 0.3f, rgb8(0x4d, 0x8d, 0xff), 0.95f);  // light bar
        bar(5.2f, -1.4f, 0.35f, 3.4f, 0.3f, rgb8(0x4d, 0x8d, 0xff), 0.95f);
        disc(0.0f, 2.6f, 1.0f, rgb8(0xee, 0xf1, 0xf5), 0.9f);        // PS button
        disc(-3.4f, 2.8f, 2.5f, rgb8(0x08, 0x09, 0x0c));             // sticks
        disc(-3.4f, 2.8f, 1.6f, rgb8(0x3b, 0x3f, 0x47));
        disc(3.4f, 2.8f, 2.5f, rgb8(0x08, 0x09, 0x0c));
        disc(3.4f, 2.8f, 1.6f, rgb8(0x3b, 0x3f, 0x47));
        bar(-8.4f, -2.2f, 2.2f, 0.8f, 0.3f, rgb8(0x17, 0x19, 0x1d));  // d-pad
        bar(-8.4f, -2.2f, 0.8f, 2.2f, 0.3f, rgb8(0x17, 0x19, 0x1d));
        disc(8.4f, -4.0f, 1.0f, rgb8(0x1f, 0xae, 0x7e));              // triangle
        disc(8.4f, -0.4f, 1.0f, rgb8(0x7f, 0xa9, 0xea));              // cross
        disc(6.6f, -2.2f, 1.0f, rgb8(0xd4, 0x50, 0x9f));              // square
        disc(10.2f, -2.2f, 1.0f, rgb8(0xe0, 0x41, 0x4f));             // circle
    }
}

struct GuidanceFrame {
    float roll;
    bool swoosh;
};

// How the tutorial frames of the game's tilt, spin and ball pictures hold the
// controller (kind 0 tilt, 1 spin, 2 ball; the frame numbers of the layouts).
GuidanceFrame guidance_frame(int kind, int frame) {
    switch (kind) {
    case 0:
        switch (frame) { case 1: return {0.0f, false}; case 2: return {-30.0f, false};
                         case 3: return {-80.0f, false}; default: return {35.0f, false}; }
    case 1:
        switch (frame) { case 7: return {0.0f, false}; case 8: return {0.0f, true}; default: return {-55.0f, false}; }
    default:
        switch (frame) { case 1: return {25.0f, false}; case 2: return {-30.0f, false}; case 3: return {-65.0f, false};
                         case 6: return {10.0f, false}; case 7: return {-80.0f, false}; default: return {0.0f, false}; }
    }
}

}  // namespace

// The tutorial picture of a controller held at the pose of the game's frame:
// 32 * scale pixels square (scale 4 is the game's 128x128).
void render_guidance(Family family, int kind, int frame, float scale, int& size, std::vector<std::uint8_t>& rgba) {
    Canvas c(scale, 32.0f, 32.0f);
    c.sky(rgb8(0x9c, 0xcf, 0xff), rgb8(0xc6, 0xe7, 0xff));
    const GuidanceFrame pose = guidance_frame(kind, frame);
    if (pose.swoosh) {
        // A white arc over the shoulder: the shake.
        c.fill([](float x, float y) {
            const float dx = x - 16.0f, dy = y - 23.0f;
            const float r = std::hypot(dx, dy);
            const float a = std::atan2(dy, dx);
            float d = std::fabs(r - 17.5f) - 0.8f;
            if (a > -0.55f || a < -2.6f) d = 1e3f;
            return d;
        }, rgb8(255, 255, 255), 0.95f);
    }
    draw_controller_art(c, family, 16.0f, 17.0f, 1.0f, pose.roll);
    size = c.width();
    rgba.resize(static_cast<std::size_t>(c.width()) * c.height() * 4u);
    c.to_rgba(rgba.data());
}

void render_glyph_scaled(Family family, Label label, float scale, int& width, int& height,
                         std::vector<std::uint8_t>& rgba) {
    Canvas c(scale);
    const bool xbox = family == Family::Xbox;
    if (!xbox) c.pal = {rgb8(0xf2, 0xf4, 0xf8), rgb8(0x7d, 0x85, 0x91), rgb8(0x1f, 0x23, 0x29)};
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
        if (xbox) c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, c.pal.body, c.pal.rim);
        else c.badge([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 13.0f, 9.2f, 9.2f); }, c.pal.body, c.pal.rim);
        draw_bars(c, 3, 6.4f, 4.4f);
        break;
    case Label::Back:
        if (xbox) {
            c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, c.pal.body, c.pal.rim);
            c.fill([](float x, float y) { return ring(sd_rbox(x, y, kCx - 2.2f, kCy - 2.2f, 4.6f, 4.6f, 1.0f), 1.9f); }, c.pal.ink);
            c.fill([](float x, float y) { return ring(sd_rbox(x, y, kCx + 2.2f, kCy + 2.2f, 4.6f, 4.6f, 1.0f), 1.9f); }, c.pal.ink);
        } else {
            c.badge([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 13.0f, 9.2f, 9.2f); }, c.pal.body, c.pal.rim);
            draw_bars(c, 2, 6.4f, 5.4f);
        }
        break;
    case Label::Home:
        if (xbox) {
            c.badge([](float x, float y) { return sd_circle(x, y, kCx, kCy, 13.0f); }, c.pal.body, c.pal.rim);
            draw_house(c);
        } else {
            draw_round_text(c, L"PS", 13);
        }
        break;
    case Label::Capture:
        c.badge([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 13.4f, 9.4f, 3.4f); }, c.pal.body, c.pal.rim);
        c.fill([](float x, float y) { return sd_rbox(x, y, kCx, kCy, 7.0f, 1.0f, 1.0f); }, c.pal.ink, 0.8f);
        break;
    case Label::Dpad: draw_dpad(c); break;
    case Label::LeftStick: draw_stick_icon(c, L"L"); break;
    case Label::RightStick: draw_stick_icon(c, L"R"); break;
    case Label::Controller: draw_controller(c); break;
    case Label::Digit1: draw_round_text(c, L"1", 18); break;
    case Label::Digit2: draw_round_text(c, L"2", 18); break;
    case Label::None: break;
    }
    width = c.width();
    height = c.height();
    rgba.resize(static_cast<std::size_t>(width) * height * 4u);
    c.to_rgba(rgba.data());
}

void render_glyph(Family family, Label label, std::uint8_t* rgba) {
    int w = 0, h = 0;
    std::vector<std::uint8_t> pixels;
    render_glyph_scaled(family, label, 1.0f, w, h, pixels);
    std::memcpy(rgba, pixels.data(), pixels.size());
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
    // Upstream's PlayStation scheme (DualShock 4, DualSense, Edge) shows the DualSense art; the Steam Controller's
    // A/B/X/Y sit where an Xbox pad's do.
    case input::ControlScheme::PlayStation: return Family::DualSense;
    case input::ControlScheme::Steam: return Family::Xbox;
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

// Guest textures the HD replacement may redraw (texture cache thread reads, the
// simulation thread rewrites after every scan change).
struct HdRegion {
    std::uint32_t address = 0;
    bool icon = false;
    bool guidance = false;
    int index = 0;  // font sheet number, icon target or guidance target
};
std::mutex g_region_mutex;
std::vector<HdRegion> g_regions;
std::atomic<std::uint32_t> g_revision{0};

constexpr std::uint32_t kSliceBytes = 4u << 20;
constexpr std::uint32_t kSheetBytes = 0x8000u;
constexpr std::uint32_t kSheetCount = 6u;
constexpr std::uint32_t kSheetsOffset = 0x60u;
constexpr std::uint32_t kTglpOffset = 48u;
// Compared up to the sheet size: the game turns the last field (the sheet
// offset) into a self-relative pointer when it sets the font up.
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

// The tutorial pictures of tilting, spinning and balancing (layouts
// Tilt/Spin/BallGuidanceReplace): 128x128 RGB565 frames of a hand holding a Wii
// Remote, found like the A icon. kind 0 tilt, 1 spin, 2 ball.
struct GuidanceTarget {
    int kind;
    int frame;
    std::uint32_t window_offset;
    std::uint64_t window_hash;
    std::uint64_t full_hash;
};
constexpr GuidanceTarget kGuidanceTargets[] = {
    {0, 1, 8608u, 0xDEF120EB82048E1Full, 0xD817B605EA5CEB19ull},
    {0, 2, 10752u, 0x47DAF43816535ED2ull, 0x7E61AAE8602C6852ull},
    {0, 3, 12736u, 0xCE9DE58933469E22ull, 0x9EECE82A16E5EA80ull},
    {0, 4, 8608u, 0x49B3842D45A8519Dull, 0x8A03E2BDC0F901C1ull},
    {1, 7, 8864u, 0x423D39A6555C9CD2ull, 0x379FD58FC8B081D9ull},
    {1, 8, 8480u, 0x8D3BBC3005CE15EAull, 0x49812AC66CB604D3ull},
    {1, 9, 9440u, 0x25A7F7DD0582E8E4ull, 0x041CD0F3FCA13F45ull},
    {2, 1, 8320u, 0x7899270BA8BEA1ECull, 0x821F1D079727516Cull},
    {2, 2, 8864u, 0xD545D656CE1072D6ull, 0xA21E7FEEE7B6A042ull},
    {2, 3, 8672u, 0x6F41B7AA30BAC8F5ull, 0x3CE61E6489CE642Bull},
    {2, 4, 8608u, 0xD32A15629AFF9996ull, 0x7D4181A9CF808673ull},  // also frames 5 and 8
    {2, 6, 8608u, 0x06F7E514560F4E43ull, 0xBE4B432E8193D3D0ull},
    {2, 7, 14784u, 0xB73D3F7E3D60E6A3ull, 0x3F7153A1864BD26Cull},
};
constexpr std::uint32_t kGuidanceBytes = 128u * 128u * 2u;

struct GuidanceCopy {
    std::uint32_t address = 0;
    int target = 0;
    std::vector<std::uint8_t> original;
    std::uint64_t current_hash = 0;
};

struct State {
    std::vector<GuidanceCopy> guidance;
    std::map<std::uint32_t, std::vector<std::uint8_t>> guidance_pixels;
    std::vector<IconCopy> icons;
    std::map<std::uint32_t, std::vector<std::uint8_t>> icon_pixels;
    std::vector<FontCopy> fonts;
    std::uint32_t seen_revision = 0;
    bool announced = false;
    bool dirty = false;  // the fonts need drawing with `applied`
    PromptSet applied;
    std::uint32_t cursor = 0;  // scan position over the view's regions, one after another
    ULONGLONG next_scan = 0;
    ULONGLONG next_verify = 0;
    ULONGLONG next_icon_verify = 0;
    ULONGLONG next_guidance_verify = 0;
    std::map<std::uint32_t, std::vector<std::uint16_t>> cells;
    int debug = -1;
};

State& state() {
    static State s;
    return s;
}

void log_line(const GuestView& view, State& s, const char* message) {
    if (s.debug < 0) {
        char value[8]{};
        std::size_t length = 0;
        s.debug = (getenv_s(&length, value, sizeof(value), "NEBULA_PROMPTS_DEBUG") == 0 && length > 1) ? 1 : 0;
    }
    if (s.debug == 1) std::fprintf(stderr, "[prompts] %s\n", message);
    if (s.debug == 1 && view.log != nullptr) view.log(view.user, message);
}

std::byte* resolve(const GuestView& view, std::uint32_t physical, std::uint32_t size) {
    return view.resolve != nullptr ? view.resolve(view.user, physical, size) : nullptr;
}

// Writes `count` bytes of `target` at `physical` where they differ, and reports the range changed.
void store_changed(const GuestView& view, std::uint32_t physical, std::uint8_t* current, const std::uint8_t* target,
                   std::size_t count) {
    std::size_t first = count, last = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (current[i] != target[i]) {
            current[i] = target[i];
            first = std::min(first, i);
            last = i + 1;
        }
    }
    if (first < last && view.written != nullptr) {
        view.written(view.user, physical + static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(last - first));
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

// Where the sheets are: the TGLP sheet field is the file's big-endian offset from
// the font's start (kSheetsOffset), and once the game has set the font up
// (ResFont) a host-order offset from the field itself (SelfRel).
std::uint32_t sheets_address(const GuestView& view, std::uint32_t base) {
    const std::uint32_t field = base + kTglpOffset + 28u;
    const std::byte* p = resolve(view, field, 4u);
    if (p == nullptr) return 0;
    std::uint32_t value = 0;
    std::memcpy(&value, p, 4);
    if (_byteswap_ulong(value) == kSheetsOffset) return base + kSheetsOffset;
    return field + value;
}

bool header_ok(const GuestView& view, std::uint32_t base) {
    const std::byte* p = resolve(view, base, kTglpOffset + 32u);
    if (p == nullptr) return false;
    return std::memcmp(p + kTglpOffset, kTglp, kTglpMatch) == 0;
}

void scan_slice(const GuestView& view, State& s) {
    std::uint32_t total = 0;
    for (std::uint32_t r = 0; r < view.region_count; ++r) total += view.regions[r].size;
    if (total == 0u) return;
    if (s.cursor >= total) s.cursor = 0u;
    const std::uint32_t offset = s.cursor;
    std::uint32_t region_base = 0, region_size = 0, local = offset;
    for (std::uint32_t r = 0; r < view.region_count; ++r) {
        if (local < view.regions[r].size) {
            region_base = view.regions[r].physical;
            region_size = view.regions[r].size;
            break;
        }
        local -= view.regions[r].size;
    }
    const std::uint32_t address = region_base + local;
    std::uint32_t length = std::min(kSliceBytes, region_size - local);
    std::uint32_t read = std::min(length + 31u, region_size - local);
    s.cursor = offset + length >= total ? 0u : offset + length;
    if (s.cursor == 0u) {
        char cycle[96];
        std::snprintf(cycle, sizeof(cycle), "scan cycle done, fonts=%zu", s.fonts.size());
        log_line(view, s, cycle);
    }
    const std::byte* p = resolve(view, address, read);
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
            if (!known && header_ok(view, base)) {
                const std::byte* sheets =
                    resolve(view, sheets_address(view, base), kSheetBytes * kSheetCount);
                if (sheets != nullptr) {
                    FontCopy copy;
                    copy.base = base;
                    copy.sheets = sheets_address(view, base);
                    copy.original.assign(reinterpret_cast<const std::uint8_t*>(sheets),
                                         reinterpret_cast<const std::uint8_t*>(sheets) + kSheetBytes * kSheetCount);
                    s.fonts.push_back(std::move(copy));
                    s.dirty = true;  // draw into the new copy
                    char message[96];
                    std::snprintf(message, sizeof(message), "button prompts: picture font at 0x%08X", base);
                    log_line(view, s, message);
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
            const std::byte* data = known ? nullptr : resolve(view, start, static_cast<std::uint32_t>(size));
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
            log_line(view, s, message);
        }
        for (int g = 0; g < static_cast<int>(std::size(kGuidanceTargets)); ++g) {
            const GuidanceTarget& target = kGuidanceTargets[g];
            if (window != target.window_hash || a < target.window_offset) continue;
            const std::uint32_t start = a - target.window_offset;
            const bool known = std::any_of(s.guidance.begin(), s.guidance.end(),
                                           [&](const GuidanceCopy& c) { return c.address == start; });
            const std::byte* data = known ? nullptr : resolve(view, start, kGuidanceBytes);
            if (data == nullptr) continue;
            const auto* raw = reinterpret_cast<const std::uint8_t*>(data);
            if (fnv64(raw, kGuidanceBytes) != target.full_hash) continue;
            GuidanceCopy copy;
            copy.address = start;
            copy.target = g;
            copy.original.assign(raw, raw + kGuidanceBytes);
            copy.current_hash = target.full_hash;
            s.guidance.push_back(std::move(copy));
            s.dirty = true;
            char message[96];
            std::snprintf(message, sizeof(message), "button prompts: tutorial picture %d/%d at 0x%08X", target.kind,
                          target.frame, start);
            log_line(view, s, message);
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

void publish_regions(const State& s) {
    std::vector<HdRegion> regions;
    for (const FontCopy& f : s.fonts) {
        for (int sheet : {0, 2}) {
            regions.push_back({f.sheets + static_cast<std::uint32_t>(sheet) * kSheetBytes, false, false, sheet});
        }
    }
    for (const IconCopy& c : s.icons) regions.push_back({c.address, true, false, c.target});
    for (const GuidanceCopy& c : s.guidance) regions.push_back({c.address, false, true, c.target});
    std::lock_guard lock(g_region_mutex);
    g_regions = std::move(regions);
}

// The tutorial picture at 128x128 as RGB565 4x4-pixel tiles.
const std::vector<std::uint8_t>& guidance_pixels(State& s, Family family, int target) {
    const std::uint32_t key = static_cast<std::uint32_t>(family) << 8 | static_cast<std::uint32_t>(target);
    auto it = s.guidance_pixels.find(key);
    if (it != s.guidance_pixels.end()) return it->second;
    int size = 0;
    std::vector<std::uint8_t> rgba;
    render_guidance(family, kGuidanceTargets[target].kind, kGuidanceTargets[target].frame, 4.0f, size, rgba);
    std::vector<std::uint8_t> data(kGuidanceBytes, 0);
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            const std::uint8_t* q = &rgba[(static_cast<std::size_t>(y) * 128 + x) * 4];
            const unsigned v = ((q[0] * 31u + 127u) / 255u) << 11 | ((q[1] * 63u + 127u) / 255u) << 5 |
                               ((q[2] * 31u + 127u) / 255u);
            const std::size_t off = (static_cast<std::size_t>(y >> 2) * 32u + (x >> 2)) * 32u +
                                    (static_cast<std::size_t>(y & 3) * 4u + (x & 3)) * 2u;
            data[off] = static_cast<std::uint8_t>(v >> 8);
            data[off + 1] = static_cast<std::uint8_t>(v & 0xFF);
        }
    }
    return s.guidance_pixels.emplace(key, std::move(data)).first->second;
}

void draw_guidance(const GuestView& view, State& s, GuidanceCopy& copy, const PromptSet& want) {
    std::byte* data = resolve(view, copy.address, kGuidanceBytes);
    if (data == nullptr) return;
    const std::vector<std::uint8_t>& target = want.family != Family::None
        ? guidance_pixels(s, want.family, copy.target) : copy.original;
    auto* current = reinterpret_cast<std::uint8_t*>(data);
    store_changed(view, copy.address, current, target.data(), kGuidanceBytes);
    copy.current_hash = fnv64(current, kGuidanceBytes);
}

void draw_icon(const GuestView& view, State& s, IconCopy& icon, const PromptSet& want) {
    const int size = kIconTargets[icon.target].size;
    const std::size_t bytes = static_cast<std::size_t>(size) * size;
    std::byte* data = resolve(view, icon.address, static_cast<std::uint32_t>(bytes));
    if (data == nullptr) return;
    const Label label = want.labels[static_cast<std::size_t>(Slot::A)];
    const std::vector<std::uint8_t>& target =
        want.family != Family::None && label != Label::None ? icon_pixels(s, want.family, label, size)
                                                            : icon.original;
    auto* current = reinterpret_cast<std::uint8_t*>(data);
    store_changed(view, icon.address, current, target.data(), bytes);
    icon.current_hash = fnv64(current, bytes);
}

void draw_font(const GuestView& view, State& s, const FontCopy& font, const PromptSet& want) {
    std::byte* sheets = resolve(view, font.sheets, kSheetBytes * kSheetCount);
    if (sheets == nullptr) return;
    std::uint32_t first = kSheetBytes * kSheetCount, last = 0;
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
                auto* current = reinterpret_cast<std::uint8_t*>(sheets) + off;
                if ((current[0] << 8 | current[1]) != value) {
                    current[0] = static_cast<std::uint8_t>(value >> 8);
                    current[1] = static_cast<std::uint8_t>(value & 0xFF);
                    first = std::min(first, off);
                    last = std::max(last, off + 2u);
                }
            }
        }
    }
    if (first < last && view.written != nullptr) view.written(view.user, font.sheets + first, last - first);
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
    static const bool debug = [] {
        char value[8]{};
        std::size_t length = 0;
        return getenv_s(&length, value, sizeof(value), "NEBULA_PROMPTS_DEBUG") == 0 && length > 1;
    }();
    if (const auto& forced = forced_family()) {
        set = make_prompt_set(*forced, input::default_binding_table(
            *forced == Family::Xbox ? input::ControlScheme::Xbox : input::ControlScheme::PlayStation));
    }
    std::lock_guard lock(g_publish_mutex);
    if (set == g_published) return;
    if (debug) std::fprintf(stderr, "[prompts] now showing family %d (A=%s B=%s)\n", static_cast<int>(set.family),
                            label_name(set.family, set.labels[static_cast<std::size_t>(Slot::A)]),
                            label_name(set.family, set.labels[static_cast<std::size_t>(Slot::B)]));
    g_published = set;
    g_revision.fetch_add(1, std::memory_order_release);
}

PromptSet published() noexcept {
    std::lock_guard lock(g_publish_mutex);
    return g_published;
}

void tick(const GuestView& view) noexcept {
    try {
        State& s = state();
        const std::uint32_t revision = g_revision.load(std::memory_order_acquire);
        if (revision != s.seen_revision) {
            s.applied = published();
            s.seen_revision = revision;
            s.dirty = true;
        }
        if (s.applied.family == Family::None && !s.dirty) return;
        if (!s.announced) { s.announced = true; log_line(view, s, "tick active"); }
        const ULONGLONG now = GetTickCount64();
        // Look for the font (and for more copies of it) while a controller is in use.
        if (s.applied.family != Family::None && now >= s.next_scan) {
            const std::size_t known = s.fonts.size() + s.icons.size() + s.guidance.size();
            scan_slice(view, s);
            if (s.fonts.size() + s.icons.size() + s.guidance.size() != known) publish_regions(s);
            s.next_scan = now + (s.fonts.empty() && s.icons.empty() && s.guidance.empty() ? 40u : 100u);
        }
        if (!s.fonts.empty() && now >= s.next_verify) {
            s.next_verify = now + 3000u;
            const std::size_t before = s.fonts.size();
            std::erase_if(s.fonts, [&](const FontCopy& f) { return !header_ok(view, f.base); });
            if (s.fonts.size() != before) {
                log_line(view, s, "button prompts: a picture font went away");
                publish_regions(s);
            }
        }
        if (!s.icons.empty() && now >= s.next_icon_verify) {
            s.next_icon_verify = now + 3000u;
            std::erase_if(s.icons, [&](const IconCopy& c) {
                const int size = kIconTargets[c.target].size;
                const std::byte* data = resolve(view, c.address, static_cast<std::uint32_t>(size * size));
                if (data == nullptr) return true;
                const std::uint64_t hash = fnv64(reinterpret_cast<const std::uint8_t*>(data), static_cast<std::size_t>(size) * size);
                return hash != c.current_hash && hash != kIconTargets[c.target].full_hash;
            });
            publish_regions(s);
        }
        if (!s.guidance.empty() && now >= s.next_guidance_verify) {
            s.next_guidance_verify = now + 3000u;
            std::erase_if(s.guidance, [&](const GuidanceCopy& c) {
                const std::byte* data = resolve(view, c.address, kGuidanceBytes);
                if (data == nullptr) return true;
                const std::uint64_t hash = fnv64(reinterpret_cast<const std::uint8_t*>(data), kGuidanceBytes);
                return hash != c.current_hash && hash != kGuidanceTargets[c.target].full_hash;
            });
            publish_regions(s);
        }
        if (s.dirty) {
            for (GuidanceCopy& copy : s.guidance) draw_guidance(view, s, copy, s.applied);
            for (IconCopy& icon : s.icons) draw_icon(view, s, icon, s.applied);
            for (const FontCopy& font : s.fonts) draw_font(view, s, font, s.applied);
            s.dirty = false;
        }
    } catch (...) {
    }
}

// ------------------------------------------------------------ HD replacement

namespace {

constexpr float kHd = static_cast<float>(kHdScale);

struct HdGlyph {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

std::mutex g_hd_mutex;
std::map<std::uint32_t, HdGlyph> g_hd_glyphs;
std::map<std::uint32_t, HdGlyph> g_hd_icons;

const HdGlyph& hd_glyph(Family family, Label label) {
    const std::uint32_t key = static_cast<std::uint32_t>(family) << 8 | static_cast<std::uint32_t>(label);
    auto it = g_hd_glyphs.find(key);
    if (it != g_hd_glyphs.end()) return it->second;
    HdGlyph glyph;
    render_glyph_scaled(family, label, kHd, glyph.width, glyph.height, glyph.rgba);
    return g_hd_glyphs.emplace(key, std::move(glyph)).first->second;
}

// The glyph drawn to fill a size x size icon at kHdScale: a square RGBA image.
const HdGlyph& hd_icon(Family family, Label label, int size) {
    const std::uint32_t key = static_cast<std::uint32_t>(family) << 16 | static_cast<std::uint32_t>(label) << 8 |
                              static_cast<std::uint32_t>(size);
    auto it = g_hd_icons.find(key);
    if (it != g_hd_icons.end()) return it->second;
    const int n = size * static_cast<int>(kHdScale);
    const float scale = n / 30.0f;  // the 28 cell units of art fill the icon
    int w = 0, h = 0;
    std::vector<std::uint8_t> canvas;
    render_glyph_scaled(family, label, scale, w, h, canvas);
    HdGlyph icon;
    icon.width = icon.height = n;
    icon.rgba.assign(static_cast<std::size_t>(n) * n * 4u, 0);
    const int ox = static_cast<int>(std::lround(17.0f * scale - n * 0.5f));
    const int oy = static_cast<int>(std::lround(19.0f * scale - n * 0.5f));
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const int sx = x + ox, sy = y + oy;
            if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
            std::memcpy(&icon.rgba[(static_cast<std::size_t>(y) * n + x) * 4u],
                        &canvas[(static_cast<std::size_t>(sy) * w + sx) * 4u], 4u);
        }
    }
    return g_hd_icons.emplace(key, std::move(icon)).first->second;
}

// RGB5A3 tiles to straight-alpha RGBA, then k times larger by bilinear filtering
// of premultiplied colour.
void upscale_rgb5a3(const std::uint8_t* source, int size, int k, std::vector<std::uint8_t>& out) {
    std::vector<float> premultiplied(static_cast<std::size_t>(size) * size * 4u);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t at = (static_cast<std::size_t>(y >> 2) * (size / 4) + (x >> 2)) * 32u +
                                   (static_cast<std::size_t>(y & 3) * 4u + (x & 3)) * 2u;
            const unsigned v = source[at] << 8 | source[at + 1];
            float r, g, b, a;
            if (v & 0x8000u) {
                a = 1.0f;
                r = ((v >> 10) & 31u) / 31.0f; g = ((v >> 5) & 31u) / 31.0f; b = (v & 31u) / 31.0f;
            } else {
                a = ((v >> 12) & 7u) / 7.0f;
                r = ((v >> 8) & 15u) / 15.0f; g = ((v >> 4) & 15u) / 15.0f; b = (v & 15u) / 15.0f;
            }
            float* q = &premultiplied[(static_cast<std::size_t>(y) * size + x) * 4u];
            q[0] = r * a; q[1] = g * a; q[2] = b * a; q[3] = a;
        }
    }
    const int n = size * k;
    out.assign(static_cast<std::size_t>(n) * n * 4u, 0);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const float sx = (x + 0.5f) / k - 0.5f, sy = (y + 0.5f) / k - 0.5f;
            const int x0 = static_cast<int>(std::floor(sx)), y0 = static_cast<int>(std::floor(sy));
            const float fx = sx - x0, fy = sy - y0;
            float acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const int px = std::clamp(x0 + i, 0, size - 1), py = std::clamp(y0 + j, 0, size - 1);
                    const float w = (i ? fx : 1.0f - fx) * (j ? fy : 1.0f - fy);
                    const float* q = &premultiplied[(static_cast<std::size_t>(py) * size + px) * 4u];
                    for (int c = 0; c < 4; ++c) acc[c] += q[c] * w;
                }
            }
            std::uint8_t* o = &out[(static_cast<std::size_t>(y) * n + x) * 4u];
            const float a = acc[3];
            for (int c = 0; c < 3; ++c) {
                o[c] = static_cast<std::uint8_t>(std::lround(std::clamp(a > 0.0f ? acc[c] / a : 0.0f, 0.0f, 1.0f) * 255.0f));
            }
            o[3] = static_cast<std::uint8_t>(std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f));
        }
    }
}

}  // namespace

static bool hd_replacement_impl(std::uint32_t guest_address, std::uint32_t width, std::uint32_t height,
                               std::uint8_t gx_format, const std::uint8_t* source, std::uint32_t source_bytes,
                               HdImage& out);

bool hd_replacement(std::uint32_t guest_address, std::uint32_t width, std::uint32_t height, std::uint8_t gx_format,
                    const std::uint8_t* source, std::uint32_t source_bytes, HdImage& out) {
    const bool hit = hd_replacement_impl(guest_address, width, height, gx_format, source, source_bytes, out);
    static const bool debug = [] {
        char value[8]{};
        std::size_t length = 0;
        return getenv_s(&length, value, sizeof(value), "NEBULA_PROMPTS_DEBUG") == 0 && length > 1;
    }();
    if (hit && debug) {
        static std::mutex seen_mutex;
        static std::map<std::string, int> seen;
        std::lock_guard lock(seen_mutex);
        if (seen.emplace(out.key, 1).second) std::fprintf(stderr, "[prompts] HD texture %s (%ux%u)\n", out.key.c_str(), out.width, out.height);
    }
    return hit;
}

static bool hd_replacement_impl(std::uint32_t guest_address, std::uint32_t width, std::uint32_t height,
                                std::uint8_t gx_format, const std::uint8_t* source, std::uint32_t source_bytes,
                                HdImage& out) {
    try {
        HdRegion region;
        {
            std::lock_guard lock(g_region_mutex);
            const auto it = std::find_if(g_regions.begin(), g_regions.end(),
                                         [&](const HdRegion& r) {
                                             // The texture cache holds physical addresses (0x11EA8EC0), the scan virtual ones.
                                             return (r.address & 0x1FFFFFFFu) == (guest_address & 0x1FFFFFFFu);
                                         });
            if (it == g_regions.end()) {
                static const bool debug = [] {
                    char value[8]{};
                    std::size_t length = 0;
                    return getenv_s(&length, value, sizeof(value), "NEBULA_PROMPTS_DEBUG") == 0 && length > 1;
                }();
                if (debug && width == 128u && height == 128u && gx_format == 5u) {
                    static std::map<std::uint32_t, int> seen;
                    if (seen.emplace(guest_address, 1).second && seen.size() < 40) {
                        std::fprintf(stderr, "[prompts] 128x128 RGB5A3 texture at 0x%08X not a prompt sheet (regions:", guest_address);
                        for (const HdRegion& r : g_regions) std::fprintf(stderr, " 0x%08X", r.address);
                        std::fprintf(stderr, ")\n");
                    }
                }
                return false;
            }
            region = *it;
        }
        const PromptSet set = published();
        if (set.family == Family::None) return false;
        std::lock_guard lock(g_hd_mutex);
        char key[96];
        if (region.guidance) {
            if (width != 128u || height != 128u || gx_format != 4u) return false;
            const GuidanceTarget& target = kGuidanceTargets[region.index];
            int size = 0;
            render_guidance(set.family, target.kind, target.frame, 4.0f * kHd, size, out.rgba);
            out.width = out.height = static_cast<std::uint32_t>(size);
            std::snprintf(key, sizeof(key), "prompts_guidance%d_%d_%d_%d", target.kind, target.frame,
                          static_cast<int>(set.family), static_cast<int>(kHdScale));
            out.key = key;
            return true;
        }
        if (region.icon) {
            const int size = kIconTargets[region.index].size;
            if (width != static_cast<std::uint32_t>(size) || height != width || gx_format != 2u) return false;
            const Label label = set.labels[static_cast<std::size_t>(Slot::A)];
            if (label == Label::None) return false;
            const HdGlyph& icon = hd_icon(set.family, label, size);
            out.width = static_cast<std::uint32_t>(icon.width);
            out.height = static_cast<std::uint32_t>(icon.height);
            out.rgba = icon.rgba;
            std::snprintf(key, sizeof(key), "prompts_icon%d_%d_%d_%d", size, static_cast<int>(set.family),
                          static_cast<int>(label), static_cast<int>(kHdScale));
            out.key = key;
            return true;
        }
        if (width != 128u || height != 128u || gx_format != 5u || source_bytes < 128u * 128u * 2u) return false;
        upscale_rgb5a3(source, 128, static_cast<int>(kHdScale), out.rgba);
        out.width = out.height = 128u * kHdScale;
        std::uint64_t identity = 14695981039346656037ull;
        const auto mix = [&](std::uint64_t value) { identity = (identity ^ value) * 1099511628211ull; };
        mix(static_cast<std::uint64_t>(set.family));
        mix(static_cast<std::uint64_t>(region.index));
        for (std::size_t slot = 0; slot < kSlotCount; ++slot) {
            const std::uint32_t glyph = kSlotGlyph[slot];
            if (static_cast<int>(glyph / 9u) != region.index) continue;
            const Label label = set.labels[slot];
            if (label == Label::None) continue;
            mix(slot << 8 | static_cast<std::uint64_t>(label));
            const HdGlyph& cell = hd_glyph(set.family, label);
            const int cell_index = static_cast<int>(glyph % 9u);
            const int ox = (cell_index % 3) * 35 * static_cast<int>(kHdScale);
            const int oy = (cell_index / 3) * 36 * static_cast<int>(kHdScale);
            const int stride = static_cast<int>(out.width);
            for (int y = 0; y < cell.height; ++y) {
                for (int x = 0; x < cell.width; ++x) {
                    const int dx = ox + x, dy = oy + y;
                    if (dx >= stride || dy >= stride) continue;
                    std::memcpy(&out.rgba[(static_cast<std::size_t>(dy) * stride + dx) * 4u],
                                &cell.rgba[(static_cast<std::size_t>(y) * cell.width + x) * 4u], 4u);
                }
            }
        }
        std::snprintf(key, sizeof(key), "prompts_font%d_%016llx", region.index,
                      static_cast<unsigned long long>(identity));
        out.key = key;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace galaxy::prompts