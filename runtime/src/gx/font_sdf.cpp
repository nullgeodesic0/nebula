#include "galaxy/gx/font_sdf.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace galaxy::gx {

namespace {

constexpr double kFar = 1e20;

// Felzenszwalb and Huttenlocher's squared distance transform of one row or column.
void edt_1d(const double* f, double* d, int n, std::vector<int>& v, std::vector<double>& z) {
    v.assign(static_cast<std::size_t>(n), 0);
    z.assign(static_cast<std::size_t>(n) + 1u, 0.0);
    std::size_t k = 0;
    z[0] = -kFar;
    z[1] = kFar;
    const auto meet = [&](int q, int p) {
        return ((f[q] + static_cast<double>(q) * q) - (f[p] + static_cast<double>(p) * p)) / (2.0 * (q - p));
    };
    for (int q = 1; q < n; ++q) {
        double s = meet(q, v[k]);
        while (s <= z[k]) {
            --k;
            s = meet(q, v[k]);
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1u] = kFar;
    }
    k = 0;
    for (int q = 0; q < n; ++q) {
        while (z[k + 1u] < static_cast<double>(q)) ++k;
        const int p = v[k];
        d[q] = static_cast<double>(q - p) * (q - p) + f[p];
    }
}

// Squared distance from every texel to the nearest texel where `inside` is set.
std::vector<double> edt(const std::vector<std::uint8_t>& inside, int w, int h) {
    std::vector<double> grid(static_cast<std::size_t>(w) * h);
    for (std::size_t i = 0; i < grid.size(); ++i) grid[i] = inside[i] ? 0.0 : kFar;
    std::vector<int> v;
    std::vector<double> z;
    std::vector<double> f(static_cast<std::size_t>(std::max(w, h))), d(f.size());
    for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) f[static_cast<std::size_t>(y)] = grid[static_cast<std::size_t>(y) * w + x];
        edt_1d(f.data(), d.data(), h, v, z);
        for (int y = 0; y < h; ++y) grid[static_cast<std::size_t>(y) * w + x] = d[static_cast<std::size_t>(y)];
    }
    for (int y = 0; y < h; ++y) {
        double* row = &grid[static_cast<std::size_t>(y) * w];
        std::copy(row, row + w, f.begin());
        edt_1d(f.data(), d.data(), w, v, z);
        std::copy(d.begin(), d.begin() + w, row);
    }
    return grid;
}
}  // namespace

DecodedTexture coverage_to_sdf(const DecodedTexture& sheet, unsigned out_scale, float spread) {
    // The edge is found on a grid 2x finer than the output (bilinear coverage, thresholded at 0.5), then each
    // output texel takes the signed distance at its centre.
    constexpr int kFine = 2;
    const int sw = static_cast<int>(sheet.width), sh = static_cast<int>(sheet.height);
    const int scale = static_cast<int>(out_scale) * kFine;
    const int fw = sw * scale, fh = sh * scale;
    const auto coverage = [&](int x, int y) {
        x = std::clamp(x, 0, sw - 1);
        y = std::clamp(y, 0, sh - 1);
        return static_cast<float>(sheet.rgba8[(static_cast<std::size_t>(y) * sheet.width + x) * 4u + 3u]) / 255.0f;
    };
    std::vector<std::uint8_t> inside(static_cast<std::size_t>(fw) * fh), outside(inside.size());
    for (int y = 0; y < fh; ++y) {
        const float sy = (static_cast<float>(y) + 0.5f) / static_cast<float>(scale) - 0.5f;
        const int y0 = static_cast<int>(std::floor(sy));
        const float ty = sy - static_cast<float>(y0);
        for (int x = 0; x < fw; ++x) {
            const float sx = (static_cast<float>(x) + 0.5f) / static_cast<float>(scale) - 0.5f;
            const int x0 = static_cast<int>(std::floor(sx));
            const float tx = sx - static_cast<float>(x0);
            const float top = coverage(x0, y0) + (coverage(x0 + 1, y0) - coverage(x0, y0)) * tx;
            const float bottom = coverage(x0, y0 + 1) + (coverage(x0 + 1, y0 + 1) - coverage(x0, y0 + 1)) * tx;
            const bool in = top + (bottom - top) * ty >= 0.5f;
            inside[static_cast<std::size_t>(y) * fw + x] = in ? 1u : 0u;
            outside[static_cast<std::size_t>(y) * fw + x] = in ? 0u : 1u;
        }
    }
    const std::vector<double> to_inside = edt(inside, fw, fh);
    const std::vector<double> to_outside = edt(outside, fw, fh);

    DecodedTexture out;
    out.width = sheet.width * out_scale;
    out.height = sheet.height * out_scale;
    out.rgba8.assign(static_cast<std::size_t>(out.width) * out.height * 4u, 255u);
    for (unsigned y = 0; y < out.height; ++y) {
        for (unsigned x = 0; x < out.width; ++x) {
            // The fine texel at this output texel's centre.
            const std::size_t fine = static_cast<std::size_t>(y * kFine + kFine / 2) * fw + (x * kFine + kFine / 2);
            // Texel centres sit half a fine texel from the edge between them.
            const float dist = static_cast<float>(inside[fine] ? std::sqrt(to_outside[fine]) - 0.5 : 0.5 - std::sqrt(to_inside[fine]));
            const float value = 0.5f + dist / static_cast<float>(kFine) / spread * 0.5f;
            out.rgba8[(static_cast<std::size_t>(y) * out.width + x) * 4u + 3u] =
                static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
        }
    }
    return out;
}

}  // namespace galaxy::gx
