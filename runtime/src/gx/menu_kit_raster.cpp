#include "galaxy/gx/menu_kit_raster.h"

#include <algorithm>
#include <cmath>

namespace galaxy::gx::menu {

namespace {

std::array<float, 4> sample(const DecodedTexture* tex, float u, float v) {
    if (tex == nullptr || tex->width == 0u || tex->height == 0u) return {1.0f, 1.0f, 1.0f, 1.0f};
    // Bilinear, clamped (the renderer's sampler).
    const float fx = std::clamp(u * static_cast<float>(tex->width) - 0.5f, 0.0f, static_cast<float>(tex->width - 1u));
    const float fy = std::clamp(v * static_cast<float>(tex->height) - 0.5f, 0.0f, static_cast<float>(tex->height - 1u));
    const unsigned x0 = static_cast<unsigned>(fx), y0 = static_cast<unsigned>(fy);
    const unsigned x1 = std::min(x0 + 1u, tex->width - 1u), y1 = std::min(y0 + 1u, tex->height - 1u);
    const float tx = fx - static_cast<float>(x0), ty = fy - static_cast<float>(y0);
    std::array<float, 4> out{};
    for (int c = 0; c < 4; ++c) {
        const auto at = [&](unsigned x, unsigned y) {
            return static_cast<float>(tex->rgba8[(static_cast<std::size_t>(y) * tex->width + x) * 4u + c]) / 255.0f;
        };
        const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
        const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
        out[c] = top + (bottom - top) * ty;
    }
    return out;
}

}  // namespace

Shaded shade(const Quad& quad, const Rgba& vertex, const std::array<float, 4>& texel) noexcept {
    Shaded s{vertex.r, vertex.g, vertex.b, vertex.a, quad.mode == Mode::Additive};
    switch (quad.mode) {
    case Mode::Solid:
        break;
    case Mode::Coverage:
        s.a = vertex.a * texel[3];
        break;
    case Mode::Text: {
        // The GPU widens the edge by the screen-space rate of change; a fixed width stands in for it here.
        const float t = std::clamp((texel[3] - 0.42f) / 0.16f, 0.0f, 1.0f);
        s.a = vertex.a * t * t * (3.0f - 2.0f * t);
        break;
    }
    case Mode::Material:
    case Mode::Additive: {
        const float t = texel[0];
        const float fr = std::min(quad.c1.r * vertex.r * quad.scale, 1.0f);
        const float fg = std::min(quad.c1.g * vertex.g * quad.scale, 1.0f);
        const float fb = std::min(quad.c1.b * vertex.b * quad.scale, 1.0f);
        s.r = quad.c0.r + (fr - quad.c0.r) * t;
        s.g = quad.c0.g + (fg - quad.c0.g) * t;
        s.b = quad.c0.b + (fb - quad.c0.b) * t;
        s.a = vertex.a * texel[3];
        break;
    }
    }
    s.r = std::clamp(std::clamp(s.r, 0.0f, 1.0f) * kPictureGain, 0.0f, 1.0f);
    s.g = std::clamp(std::clamp(s.g, 0.0f, 1.0f) * kPictureGain, 0.0f, 1.0f);
    s.b = std::clamp(std::clamp(s.b, 0.0f, 1.0f) * kPictureGain, 0.0f, 1.0f);
    s.a = std::clamp(s.a, 0.0f, 1.0f);
    return s;
}

void rasterize(Image& target, const DrawList& list, const std::array<const DecodedTexture*, kMaxTextures>& textures) {
    for (const Quad& q : list) {
        const float x0 = q.v[0].x, x1 = q.v[1].x, y0 = q.v[0].y, y1 = q.v[2].y;
        const float left = std::min(x0, x1), right = std::max(x0, x1);
        const float top = std::min(y0, y1), bottom = std::max(y0, y1);
        const int px0 = std::max(0, static_cast<int>(std::floor(left)));
        const int px1 = std::min(static_cast<int>(target.width), static_cast<int>(std::ceil(right)));
        const int py0 = std::max(0, static_cast<int>(std::floor(top)));
        const int py1 = std::min(static_cast<int>(target.height), static_cast<int>(std::ceil(bottom)));
        const auto index = static_cast<std::size_t>(q.tex);
        const DecodedTexture* tex = q.tex == Tex::None || index >= textures.size() ? nullptr : textures[index];
        for (int py = py0; py < py1; ++py) {
            const float cy = static_cast<float>(py) + 0.5f;
            if (cy < top || cy > bottom) continue;
            const float ty = (cy - y0) / (y1 - y0);
            const Rgba vertex = mix(q.v[0].color, q.v[2].color, ty);
            const float v = q.v[0].v + (q.v[2].v - q.v[0].v) * ty;
            for (int px = px0; px < px1; ++px) {
                const float cx = static_cast<float>(px) + 0.5f;
                if (cx < left || cx > right) continue;
                const float tx = (cx - x0) / (x1 - x0);
                const float u = q.v[0].u + (q.v[1].u - q.v[0].u) * tx;
                const Shaded s = shade(q, vertex, q.tex == Tex::None ? std::array<float, 4>{1, 1, 1, 1} : sample(tex, u, v));
                std::uint8_t* d = &target.rgba[(static_cast<std::size_t>(py) * target.width + px) * 4u];
                for (int c = 0; c < 3; ++c) {
                    const float src = (c == 0 ? s.r : c == 1 ? s.g : s.b) * s.a;
                    const float dst = static_cast<float>(d[c]) / 255.0f;
                    const float out = s.additive ? dst + src : src + dst * (1.0f - s.a);
                    d[c] = static_cast<std::uint8_t>(std::lround(std::clamp(out, 0.0f, 1.0f) * 255.0f));
                }
                d[3] = 255u;
            }
        }
    }
}

DecodedTexture mirror_cap_vertically(const DecodedTexture& quarter) {
    DecodedTexture out;
    out.width = quarter.width;
    out.height = quarter.height * 2u;
    out.rgba8.resize(static_cast<std::size_t>(out.width) * out.height * 4u);
    const std::size_t row = static_cast<std::size_t>(quarter.width) * 4u;
    for (unsigned y = 0; y < quarter.height; ++y) {
        std::copy_n(&quarter.rgba8[y * row], row, &out.rgba8[y * row]);
        std::copy_n(&quarter.rgba8[y * row], row, &out.rgba8[(out.height - 1u - y) * row]);
    }
    return out;
}

}  // namespace galaxy::gx::menu
