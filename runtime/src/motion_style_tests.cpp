#include "nebula_motion_style.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using galaxy::MotionStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "motion_style_tests: " << message << '\n';
    }
    return condition;
}

// The game's XanimePlayer::updateInterpoleRatio while the animation plays, after the site hook.
std::vector<float> run_cross_fade(MotionStyle blend, std::int32_t frames) {
    float ratio = 0.0f;
    std::int32_t remaining = frames;
    std::vector<float> ratios;
    for (int frame = 0; frame < 1000 && ratio < 1.0f; ++frame) {
        const auto shaped = galaxy::shape_animation_interpole(blend, ratio, remaining);
        ratio = shaped.ratio;
        remaining = shaped.remaining;
        if (remaining != 0) {
            ratio += (1.0f - ratio) / static_cast<float>(remaining);
            remaining -= 1;
        } else {
            ratio = 1.0f;
        }
        ratios.push_back(ratio);
    }
    return ratios;
}

bool near(float a, float b, float tolerance = 1e-4f) {
    return std::fabs(a - b) <= tolerance;
}

bool test_ease_and_inverse() {
    bool passed = true;
    passed &= expect(galaxy::animation_blend_ease(0.0f) == 0.0f, "ease starts at 0");
    passed &= expect(galaxy::animation_blend_ease(1.0f) == 1.0f, "ease ends at 1");
    passed &= expect(near(galaxy::animation_blend_ease(0.5f), 0.5f), "ease is symmetric");
    for (int i = 0; i <= 100; ++i) {
        const float p = static_cast<float>(i) / 100.0f;
        if (!near(galaxy::animation_blend_progress(galaxy::animation_blend_ease(p)), p, 2e-3f)) {
            return expect(false, "progress inverts the ease");
        }
    }
    return passed;
}

bool test_original_is_untouched() {
    const auto ratios = run_cross_fade(MotionStyle::Original, 8);
    bool passed = expect(ratios.size() == 8u, "original blend keeps its eight frames");
    for (std::size_t i = 0; i < ratios.size(); ++i) {
        passed &= expect(near(ratios[i], static_cast<float>(i + 1) / 8.0f), "original blend is linear");
    }
    return passed;
}

bool test_smooth_follows_the_eased_curve() {
    const auto ratios = run_cross_fade(MotionStyle::Smooth, 10);
    bool passed = expect(ratios.size() == 10u, "smooth blend keeps its length");
    for (std::size_t i = 0; i < ratios.size(); ++i) {
        const float expected = galaxy::animation_blend_ease(static_cast<float>(i + 1) / 10.0f);
        passed &= expect(near(ratios[i], expected, 1e-3f), "smooth blend lands on smoothstep");
        passed &= expect(ratios[i] >= 0.0f && ratios[i] <= 1.0f, "smooth ratio stays in range");
        if (i > 0) passed &= expect(ratios[i] >= ratios[i - 1], "smooth ratio never goes back");
    }
    passed &= expect(ratios.back() == 1.0f, "smooth blend finishes exactly");
    passed &= expect(ratios.front() < 1.0f / 10.0f, "smooth blend starts slower than linear");
    return passed;
}

bool test_smoother_stretches_once() {
    const auto ratios = run_cross_fade(MotionStyle::Modern, 8);
    bool passed = expect(ratios.size() == 12u, "smoother blend lasts 1.5 times as long");
    passed &= expect(ratios.back() == 1.0f, "smoother blend finishes exactly");
    for (std::size_t i = 1; i < ratios.size(); ++i) {
        passed &= expect(ratios[i] >= ratios[i - 1], "smoother ratio never goes back");
    }
    return passed;
}

bool test_out_of_range_is_left_alone() {
    bool passed = true;
    const auto finished = galaxy::shape_animation_interpole(MotionStyle::Smooth, 1.0f, 3);
    passed &= expect(finished.ratio == 1.0f && finished.remaining == 3, "a finished blend is untouched");
    const auto idle = galaxy::shape_animation_interpole(MotionStyle::Modern, 0.0f, 0);
    passed &= expect(idle.ratio == 0.0f && idle.remaining == 0, "no remaining frames is untouched");
    const auto huge = galaxy::shape_animation_interpole(MotionStyle::Modern, 0.0f, 100000);
    passed &= expect(huge.remaining == 100000, "an implausible count is untouched");
    const auto nan = galaxy::shape_animation_interpole(MotionStyle::Smooth, std::nanf(""), 4);
    passed &= expect(std::isnan(nan.ratio) && nan.remaining == 4, "a NaN ratio is untouched");
    const auto last = galaxy::shape_animation_interpole(MotionStyle::Smooth, 0.7f, 1);
    passed &= expect(last.ratio == 0.7f && last.remaining == 1, "the last frame is the game's");
    return passed;
}

bool test_switching_mid_blend() {
    // A blend begun as Original and continued as Smooth still finishes on time.
    float ratio = 0.0f;
    std::int32_t remaining = 10;
    for (int i = 0; i < 4; ++i) {
        ratio += (1.0f - ratio) / static_cast<float>(remaining);
        remaining -= 1;
    }
    int frames = 0;
    while (ratio < 1.0f && frames < 100) {
        const auto shaped = galaxy::shape_animation_interpole(MotionStyle::Smooth, ratio, remaining);
        const float before = ratio;
        ratio = shaped.ratio + (1.0f - shaped.ratio) / static_cast<float>(shaped.remaining);
        remaining = shaped.remaining - 1;
        if (ratio < before - 1e-4f) return expect(false, "switching mid-blend never goes back");
        ++frames;
    }
    return expect(frames == 6 && ratio == 1.0f, "switching mid-blend finishes on time");
}

bool test_mario_table_values() {
    bool passed = true;
    float value = 0.0f;
    const auto& tilt = galaxy::kMarioTableFields[0];
    passed &= expect(galaxy::mario_table_value(tilt, MotionStyle::Smooth, 40.0f, value) && value == 52.0f,
                     "smooth leans further into turns");
    passed &= expect(galaxy::mario_table_value(tilt, MotionStyle::Original, 60.0f, value) && value == 40.0f,
                     "original restores the game's lean");
    passed &= expect(!galaxy::mario_table_value(tilt, MotionStyle::Smooth, 41.0f, value),
                     "an unexpected value is not a table this changes");
    for (const auto& field : galaxy::kMarioTableFields) {
        // 0x7A4: MarioConstTable's size (the game module checks each field's offset by name).
        passed &= expect(field.offset < 0x7A4u &&
                             (field.offset & (field.halfword ? 1u : 3u)) == 0u,
                         "fields lie aligned inside the table");
        passed &= expect(!field.halfword || (field.values[0] == std::floor(field.values[0]) &&
                                             field.values[2] == std::floor(field.values[2])),
                         "halfword fields hold whole numbers");
    }
    // Movement changes only in Modern; Smooth is looks only.
    for (const auto& field : galaxy::kMarioTableFields) {
        if (field.offset != 0x328u && field.offset != 0x354u) {
            passed &= expect(field.values[1] == field.values[0], "smooth keeps Mario's movement");
        }
    }
    // The spin's own fields (Petari's MarioConstTable: mJumpTornadoSpeed, mHipDropLimitTimerAfterTornado,
    // mTornadoTime through mTornadoReflectSpeed, mTornadoHoldTime) are never among them.
    for (const auto& field : galaxy::kMarioTableFields) {
        passed &= expect(field.offset != 0x060u && field.offset != 0x0B6u && field.offset != 0x75Au &&
                             !(field.offset >= 0x3A0u && field.offset < 0x414u),
                         "no spin field is changed");
    }
    return passed;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= test_ease_and_inverse();
    passed &= test_original_is_untouched();
    passed &= test_smooth_follows_the_eased_curve();
    passed &= test_smoother_stretches_once();
    passed &= test_out_of_range_is_left_alone();
    passed &= test_switching_mid_blend();
    passed &= test_mario_table_values();
    if (!passed) {
        return 1;
    }
    std::cout << "motion_style_tests: passed\n";
    return 0;
}
