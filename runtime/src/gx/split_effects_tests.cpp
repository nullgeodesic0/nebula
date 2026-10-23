// Co-op split screen and the scene effects (split_effects.h): when depth of field and motion blur run per half,
// when the camera history restarts, and where each half's focus is taken.

#include "galaxy/gx/split_effects.h"

#include <cmath>
#include <cstdio>

namespace {

bool expect(bool ok, const char* what) {
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    return ok;
}

using galaxy::gx::SplitEffectTracker;
using galaxy::gx::SplitScene;

// The halves left and right of a vertical line through x = 320 (the first player on the left).
SplitScene left_right(float opacity) {
    SplitScene scene;
    scene.halves[0] = scene.halves[1] = true;
    scene.plane[0] = {-1.0f, 0.0f, 320.0f};
    scene.plane[1] = {1.0f, 0.0f, -320.0f};
    scene.opacity = opacity;
    return scene;
}

}  // namespace

int main() {
    bool ok = true;
    SplitEffectTracker tracker;

    // One view: as single player, nothing to restart.
    SplitScene one;
    auto plan = tracker.next(one);
    ok &= expect(!plan.per_half && !plan.reset_history, "one view: whole-screen effects, history kept");

    // The split opening (the separator fading in): still as single player.
    plan = tracker.next(left_right(0.4f));
    ok &= expect(!plan.per_half && !plan.reset_history, "while the split opens: whole-screen effects");

    // Fully open: per half, and the history restarts once (the second camera's view appeared).
    plan = tracker.next(left_right(1.0f));
    ok &= expect(plan.per_half && plan.reset_history, "the split opens: per-half effects, history restarts");
    plan = tracker.next(left_right(1.0f));
    ok &= expect(plan.per_half && !plan.reset_history, "the split stays: per half, history kept");

    // The line turning a little as the players move: no restart.
    SplitScene turned = left_right(1.0f);
    turned.plane[0] = {-0.9f, 0.3f, 300.0f};
    turned.plane[1] = {0.9f, -0.3f, -300.0f};
    plan = tracker.next(turned);
    ok &= expect(plan.per_half && !plan.reset_history, "the line turns smoothly: history kept");

    // The cameras change hands (each half's side flips): the history restarts.
    SplitScene swapped = left_right(1.0f);
    swapped.plane[0] = {1.0f, 0.0f, -320.0f};
    swapped.plane[1] = {-1.0f, 0.0f, 320.0f};
    plan = tracker.next(swapped);
    ok &= expect(plan.per_half && plan.reset_history, "a camera hand-over: history restarts");

    // One half missing (a scene drew only one): whole screen, and the history restarts.
    SplitScene half_only = left_right(1.0f);
    half_only.halves[1] = false;
    plan = tracker.next(half_only);
    ok &= expect(!plan.per_half && plan.reset_history, "back to one view: whole screen, history restarts");

    // A degenerate line (no normal) never runs per half.
    SplitScene degenerate = left_right(1.0f);
    degenerate.plane[0] = {0.0f, 0.0f, 0.0f};
    tracker.reset();
    plan = tracker.next(degenerate);
    ok &= expect(!plan.per_half, "a half with no line: whole-screen effects");

    // Each half's focus point: the middle of its own side, never on the line.
    const float viewport[4] = {0.0f, 0.0f, 640.0f, 456.0f};
    const auto left = galaxy::gx::split_focus_point({-1.0f, 0.0f, 320.0f}, viewport);
    const auto right = galaxy::gx::split_focus_point({1.0f, 0.0f, -320.0f}, viewport);
    ok &= expect(std::fabs(left[0] - 160.0f) < 0.5f && std::fabs(left[1] - 228.0f) < 0.5f,
                 "the left half focuses at the middle of the left half");
    ok &= expect(std::fabs(right[0] - 480.0f) < 0.5f, "the right half focuses at the middle of the right half");
    const auto top = galaxy::gx::split_focus_point({0.0f, -1.0f, 228.0f}, viewport);
    ok &= expect(std::fabs(top[1] - 114.0f) < 0.5f, "a top half focuses at the middle of the top half");
    ok &= expect(-1.0f * left[0] + 320.0f > 0.0f && 1.0f * right[0] - 320.0f > 0.0f,
                 "each focus point is on its own side of the line");

    // Logical EFB pixels to render pixels at 3x: the same line, scaled.
    const auto render = galaxy::gx::split_plane_render({-1.0f, 0.0f, 320.0f}, 3.0f);
    ok &= expect(-1.0f * 960.0f + render[2] == 0.0f, "a scaled plane passes through the scaled line");

    std::printf("%s\n", ok ? "split effects: all passed" : "split effects: FAILED");
    return ok ? 0 : 1;
}
