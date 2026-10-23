#pragma once

#include <array>
#include <cmath>

namespace galaxy::gx {

// Co-op split screen and the scene effects (depth of field, motion blur). Both halves draw into one scene and the
// effects run once it ends (GxBackend's SceneEnd), so each needs to know the halves: where each one's side of the
// line is, and its camera planes. SplitScene is what one scene showed; SplitEffectTracker decides what the effects
// do with it.
struct SplitScene {
    bool halves[2]{};                     // the first (pass 1) and second (pass 2) halves were drawn
    std::array<float, 3> plane[2]{};      // each half's side, a*x + b*y + c >= 0 in logical EFB pixels
    float near_plane[2]{};                // each half's camera planes, 0 when not seen
    float far_plane[2]{};
    float opacity = 0.0f;                 // the separator's: 1 once the split has fully opened
};

struct SplitEffectPlan {
    bool per_half = false;       // the effects run once per half, each inside its side with its own state
    bool reset_history = false;  // the camera history restarts (the split opened or closed, or a half changed side)
};

class SplitEffectTracker {
public:
    // One scene. While the split opens or closes (opacity under 1) and with one view, the effects are as in single
    // player. A half whose side of the line turns by more than 90 degrees from the last scene (the cameras changed
    // hands) or a split that opens or closes restarts the motion history: a camera jump must not become a smear.
    SplitEffectPlan next(const SplitScene& scene) noexcept {
        SplitEffectPlan plan;
        bool split = scene.halves[0] && scene.halves[1] && scene.opacity >= 0.999f;
        std::array<float, 2> normal[2]{};
        for (unsigned h = 0; split && h < 2u; ++h) {
            const float length = std::hypot(scene.plane[h][0], scene.plane[h][1]);
            if (!(length > 1e-6f) || !std::isfinite(length)) {
                split = false;
                break;
            }
            normal[h] = {scene.plane[h][0] / length, scene.plane[h][1] / length};
        }
        plan.per_half = split;
        if (split != was_split_) plan.reset_history = true;
        if (split && was_split_) {
            for (unsigned h = 0; h < 2u; ++h) {
                if (normal[h][0] * last_normal_[h][0] + normal[h][1] * last_normal_[h][1] < 0.0f) {
                    plan.reset_history = true;
                }
            }
        }
        was_split_ = split;
        if (split) {
            last_normal_[0] = normal[0];
            last_normal_[1] = normal[1];
        }
        return plan;
    }

    void reset() noexcept { *this = SplitEffectTracker{}; }

private:
    bool was_split_ = false;
    std::array<float, 2> last_normal_[2]{};
};

// A logical-EFB-pixel half plane in render pixels (EFB scale `scale`): (a, b, c * scale).
[[nodiscard]] inline std::array<float, 3> split_plane_render(const std::array<float, 3>& plane, float scale) noexcept {
    return {plane[0], plane[1], plane[2] * scale};
}

// Where a half's depth of field takes its focus: the middle of its side of the viewport, a quarter of the viewport's
// extent along the line's normal from the centre (the line runs through the centre, and each player is kept near
// the middle of their half). `viewport` is x, y, w, h in the plane's pixels.
[[nodiscard]] inline std::array<float, 2> split_focus_point(const std::array<float, 3>& plane,
                                                            const float viewport[4]) noexcept {
    const float cx = viewport[0] + viewport[2] * 0.5f;
    const float cy = viewport[1] + viewport[3] * 0.5f;
    const float length = std::hypot(plane[0], plane[1]);
    if (!(length > 1e-6f)) return {cx, cy};
    const float nx = plane[0] / length;
    const float ny = plane[1] / length;
    const float reach = 0.25f * (std::fabs(nx) * viewport[2] + std::fabs(ny) * viewport[3]);
    return {cx + nx * reach, cy + ny * reach};
}

}  // namespace galaxy::gx
