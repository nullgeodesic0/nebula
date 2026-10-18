#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace galaxy {

// How characters move: the game's own, or closer to a modern 3D Mario while staying Galaxy.
//   Original: the game's animation blending and Mario's own movement.
//   Smooth:   looks only. Every character's animation cross-fades are eased in and out, and Mario
//             leans further into turns and blends wait, walk and run more gently.
//   Modern:   Smooth's looks with cross-fades kModernBlendStretch times as long, and Mario's
//             movement is more responsive: he gets going and stops sooner and turns faster at a run.
// The spin (the game's "tornado") keeps every one of its own values in every style; its speed,
// turning and inertia come from separate fields this never changes.
enum class MotionStyle : std::uint8_t { Original = 0, Smooth = 1, Modern = 2 };

// --- Animation cross-fades (every character) ---
//
// The game's XanimePlayer cross-fades from a frozen copy of the old pose to the new animation with a
// linear ratio, which starts and stops the blend at full speed; the joints visibly snap at both ends.
// Only the drawn pose changes: animation frames, timers and game logic stay the game's.

inline constexpr float kModernBlendStretch = 1.5f;
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
// which is when Modern lengthens the count.
struct AnimationInterpoleStep {
    float ratio = 1.0f;
    std::int32_t remaining = 0;
};

[[nodiscard]] inline AnimationInterpoleStep shape_animation_interpole(
    MotionStyle style, float ratio, std::int32_t remaining) noexcept {
    AnimationInterpoleStep step{ratio, remaining};
    if (style == MotionStyle::Original || remaining <= 0 || remaining > kMaxInterpoleFrames ||
        !(ratio >= 0.0f && ratio < 1.0f)) {
        return step;
    }
    if (style == MotionStyle::Modern && ratio == 0.0f) {
        const auto stretched = static_cast<std::int32_t>(
            std::lround(static_cast<float>(remaining) * kModernBlendStretch));
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

// --- Mario's tuning table ---
//
// MarioConstTable fields (offsets and the game's values from Petari's MarioConst), each with its value
// in Original, Smooth and Modern. Looks (read only by MarioAnimator, for the drawn pose):
//   mTiltRatio         chest lean into a turn for his speed (MarioAnimator::setTilt)
//   mWeightBlendRatio  how fast wait, walk and run cross-blend as speed changes (setWalkWeight)
// Movement (Mario::decideInertia, updateWalkSpeed and the turn in MarioMove), Modern only:
//   mInertiaStandardStop / Max  how much speed carries over each frame while moving, at a walk and
//                               at full speed; lower reaches the stick's speed sooner
//   mInertiaStop               the same with the stick released; lower stops sooner
//   mTurnAngleSpeed2           how fast he turns while running
//   mSlowStartTime (u16)       frames of the slow first steps from a standstill
// While spinning the game swaps in mInertiaTornadoAccel/Brake and mTurnAngleSpeedTornado, and scales
// speed by mTornadoMultiply; none of those, nor jumps, gravity or top speed, are touched.
struct MarioTableField {
    std::uint32_t offset;
    bool halfword;
    float values[3];  // Original, Smooth, Modern
};

inline constexpr MarioTableField kMarioTableFields[] = {
    {0x328u, false, {40.0f, 52.0f, 60.0f}},   // mTiltRatio
    {0x354u, false, {0.2f, 0.15f, 0.12f}},    // mWeightBlendRatio
    {0x228u, false, {0.94f, 0.94f, 0.91f}},   // mInertiaStandardStop
    {0x22Cu, false, {0.9f, 0.9f, 0.87f}},     // mInertiaStandardMax
    {0x238u, false, {0.85f, 0.85f, 0.8f}},    // mInertiaStop
    {0x25Cu, false, {0.15f, 0.15f, 0.2f}},    // mTurnAngleSpeed2
    {0x27Eu, true, {10.0f, 10.0f, 6.0f}},     // mSlowStartTime
};

// RMGE01's two tables, MarioConst::mTable[0] and [1].
inline constexpr std::uint32_t kMarioConstTables[] = {0x805B0670u, 0x805B0E14u};
inline constexpr std::uint32_t kMarioConstTableSize = 0x7A4u;

// The value to store over `current`, or false when `current` is none of the field's known values (not
// the table this expects, so it is left alone).
[[nodiscard]] inline bool mario_table_value(
    const MarioTableField& field, MotionStyle style, float current, float& value) noexcept {
    bool known = false;
    for (const float candidate : field.values) known = known || current == candidate;
    if (!known) return false;
    value = field.values[static_cast<unsigned>(style) < 3u ? static_cast<unsigned>(style) : 0u];
    return true;
}

}  // namespace galaxy
