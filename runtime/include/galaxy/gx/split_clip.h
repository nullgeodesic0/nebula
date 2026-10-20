#pragma once

// Split screen (coop_guest.cpp): the line between the halves, for the vertex shader's clip distance.

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace galaxy::gx {

// The line a * x + b * y + c >= 0 (`plane`, logical EFB pixels, y down) as the clip distance
// cos(t) * x + sin(t) * y + d * w >= 0 in clip space, for a viewport of half extents `half_width` x
// `half_height` at `left`, `top` (logical pixels): x_px = (x_ndc + 1) * half_width + left and
// y_px = (1 - y_ndc) * half_height + top. Returns the two GxVsConstants words the vertex shaders read
// (pad0 = t + 16, never 0; pad1 = d); {0, 0} is no line.
[[nodiscard]] inline std::array<std::uint32_t, 2> split_clip_vs_words(
    const std::array<float, 3>& plane, float half_width, float half_height, float left, float top) noexcept {
    const float a = plane[0] * half_width;
    const float b = -plane[1] * half_height;
    const float c = plane[0] * (half_width + left) + plane[1] * (half_height + top) + plane[2];
    const float length = std::sqrt(a * a + b * b);
    if (!(length > 1e-6f) || !std::isfinite(length) || !std::isfinite(c)) return {0u, 0u};
    const float angle = std::atan2(b, a) + 16.0f;
    return {std::bit_cast<std::uint32_t>(angle), std::bit_cast<std::uint32_t>(c / length)};
}

}  // namespace galaxy::gx
