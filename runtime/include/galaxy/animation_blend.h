#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace galaxy {

// How a character's skeleton moves from one animation to the next. The game's XanimePlayer
// cross-fades from a frozen copy of the old pose to the new animation with a linear ratio, which
// starts and stops the blend at full speed; the joints visibly snap at both ends.
//   Original: the game's linear cross-fade.
//   Smooth:   the same length, eased in and out (smoothstep), so a blend starts and lands at rest.
//   Smoother: eased, and each cross-fade lasts kSmootherBlendStretch times as long.
// Only the drawn pose changes: animation frames, timers and Mario's physics stay the game's.
enum class AnimationBlend : std::uint8_t { Original = 0, Smooth = 1, Smoother = 2 };

inline constexpr float kSmootherBlendStretch = 1.5f;
// XanimeFrameCtrl's interpolation counts come from the game's animation tables (a few to a few
// dozen frames); anything outside this is not a blend this code understands and is left alone.
inline constexpr std::int32_t kMaxInterpoleFrames = 600;

[[nodiscard]] inline float animation_blend_ease(float progress) noexcept {
    const float t = std::clamp(progress, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The progress whose eased ratio is `ratio` (the inverse of smoothstep on [0, 1]).
[[nodiscard]] inline float animation_blend_progress(float ratio) noexcept {
    const float y = std::clamp(ratio, 0.0f, 1.0f);
    return std::clamp(0.5f - std::sin(std::asin(1.0f - 2.0f * y) / 3.0f), 0.0f, 1.0f);
}

// XanimePlayer::updateInterpoleRatio, while frames remain, does
//     ratio += (1 - ratio) / remaining;  remaining -= 1;
// and the result is the weight of the new animation against the frozen pose. Given the player's
// ratio and remaining frames just before that call, returns the values to store instead so that
// the game's own step lands on the eased curve. The ratio the game keeps between frames is the eased
// value, so the linear progress is recovered from it each frame and nothing is remembered here.
// A ratio of exactly 0 is the first frame of a new cross-fade (every change of animation resets it),
// which is when Smoother lengthens the count.
struct AnimationInterpoleStep {
    float ratio = 1.0f;
    std::int32_t remaining = 0;
};

[[nodiscard]] inline AnimationInterpoleStep shape_animation_interpole(
    AnimationBlend blend, float ratio, std::int32_t remaining) noexcept {
    AnimationInterpoleStep step{ratio, remaining};
    if (blend == AnimationBlend::Original || remaining <= 0 || remaining > kMaxInterpoleFrames ||
        !(ratio >= 0.0f && ratio < 1.0f)) {
        return step;
    }
    if (blend == AnimationBlend::Smoother && ratio == 0.0f) {
        const auto stretched = static_cast<std::int32_t>(
            std::lround(static_cast<float>(remaining) * kSmootherBlendStretch));
        step.remaining = std::clamp(stretched, remaining, kMaxInterpoleFrames);
    }
    const std::int32_t n = step.remaining;
    if (n == 1) {
        return step;  // The game's step lands on 1 from any ratio.
    }
    const float progress = ratio == 0.0f ? 0.0f : animation_blend_progress(ratio);
    const float target = animation_blend_ease(progress + (1.0f - progress) / static_cast<float>(n));
    // ratio + (1 - ratio) / n == target.
    step.ratio = (static_cast<float>(n) * target - 1.0f) / static_cast<float>(n - 1);
    return step;
}

}  // namespace galaxy
