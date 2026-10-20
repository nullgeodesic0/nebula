#include "galaxy/coop.h"

#include <cmath>
#include <iostream>

namespace {

using namespace galaxy::coop;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "coop_tests: " << message << '\n';
    }
    return condition;
}

bool near(float a, float b, float tolerance = 1e-3f) {
    return std::fabs(a - b) <= tolerance;
}

bool test_characters_and_archives() {
    bool ok = true;
    ok &= expect(partner_of(Character::Mario) == Character::Luigi, "Mario's partner is Luigi");
    ok &= expect(partner_of(Character::Luigi) == Character::Mario, "Luigi's partner is Mario");
    ok &= expect(archives_of(Character::Luigi).model_path == "/ObjectData/Luigi.arc", "Luigi's model archive");
    ok &= expect(archives_of(Character::Luigi).face_path == "/ObjectData/LuigiFace.arc", "Luigi's face archive");
    ok &= expect(archives_of(Character::Mario).shadow_path == "/ObjectData/MarioShadow.arc", "Mario's shadow");
    Character character = Character::Luigi;
    ok &= expect(own_model_archive("/ObjectData/Mario.arc", character) && character == Character::Mario,
                 "Mario.arc is Mario's set loading");
    ok &= expect(own_model_archive("/ObjectData/Luigi.arc", character) && character == Character::Luigi,
                 "Luigi.arc is Luigi's set loading");
    ok &= expect(!own_model_archive("/ObjectData/MarioAnime.arc", character), "the shared animations are neither");
    ok &= expect(!own_model_archive("/ObjectData/MarioFace.arc", character), "a face archive is not the model");
    return ok;
}

bool test_kpad_layout_and_edges() {
    bool ok = true;
    KpadSynth synth;
    PartnerInput input;
    input.connected = true;
    input.buttons = button::kA | button::kZ;
    input.stick_x = 0.5f;
    input.stick_y = -2.0f;
    KpadStatusBytes first = synth.next(input);
    ok &= expect(get_u32(first, kpad::kHold) == (button::kA | button::kZ), "hold carries the buttons");
    ok &= expect(get_u32(first, kpad::kTrig) == (button::kA | button::kZ), "first press triggers");
    ok &= expect(get_u32(first, kpad::kRelease) == 0u, "nothing released yet");
    ok &= expect(first[kpad::kDevType] == kpad::kDevFreestyle, "a Remote with a Nunchuk");
    ok &= expect(first[kpad::kDpdValid] == 0u, "no pointer");
    ok &= expect(near(get_f32(first, kpad::kFsStick), 0.5f), "stick x");
    ok &= expect(near(get_f32(first, kpad::kFsStick + 4u), -1.0f), "stick y is clamped");
    ok &= expect(near(get_f32(first, kpad::kAcc + 4u), -1.0f), "the Remote rests at (0, -1, 0)");
    ok &= expect(near(get_f32(first, kpad::kAccValue), 1.0f), "1 g at rest");

    input.buttons = button::kA;
    KpadStatusBytes second = synth.next(input);
    ok &= expect(get_u32(second, kpad::kTrig) == 0u, "a held button triggers once");
    ok &= expect(get_u32(second, kpad::kRelease) == button::kZ, "Z released");

    input.connected = false;
    KpadStatusBytes gone = synth.next(input);
    ok &= expect(get_u32(gone, kpad::kHold) == 0u, "no controller holds nothing");
    ok &= expect(get_u32(gone, kpad::kRelease) == button::kA, "and releases what was held");
    ok &= expect(near(get_f32(gone, kpad::kFsStick), 0.0f), "a centred stick");
    return ok;
}

bool test_shake_is_twelve_frames_per_press() {
    bool ok = true;
    KpadSynth synth;
    PartnerInput input;
    input.connected = true;
    input.shake = true;
    unsigned shaking = 0u;
    for (int frame = 0; frame < 30; ++frame) {
        const KpadStatusBytes bytes = synth.next(input);
        if (get_f32(bytes, kpad::kAccValue) > 2.0f) ++shaking;
        if (frame == 1) ok &= expect(get_f32(bytes, kpad::kAccSpeed) > 4.0f, "the shake swings each frame");
    }
    ok &= expect(shaking == KpadSynth::kShakeFrames, "one shake per press, however long it is held");
    input.shake = false;
    (void)synth.next(input);
    input.shake = true;
    ok &= expect(get_f32(synth.next(input), kpad::kAccValue) > 2.0f, "a new press shakes again");
    return ok;
}

bool test_partner_input_from_bindings() {
    bool ok = true;
    galaxy::input::LogicalInput logic;
    logic.pressed = galaxy::input::wii_target_bit(galaxy::input::WiiTarget::JumpBack) |
                    galaxy::input::wii_target_bit(galaxy::input::WiiTarget::Z) |
                    galaxy::input::wii_target_bit(galaxy::input::WiiTarget::Plus);
    logic.stick_x = -0.25f;
    logic.stick_y = 0.75f;
    const PartnerInput input = partner_input_from(logic, false);
    ok &= expect(input.connected, "a bound controller is connected");
    ok &= expect(input.buttons == (button::kA | button::kZ | button::kPlus), "JumpBack is A for the second player");
    ok &= expect(near(input.stick_x, -0.25f) && near(input.stick_y, 0.75f), "the stick passes through");
    ok &= expect(!input.shake, "no shake");
    logic.pressed = galaxy::input::wii_target_bit(galaxy::input::WiiTarget::Shake);
    ok &= expect(partner_input_from(logic, false).shake, "the shake binding spins");
    ok &= expect(partner_input_from({}, true).shake, "a motion shake spins");
    return ok;
}

bool test_shared_camera() {
    bool ok = true;
    const Vec3 first{100.0f, 0.0f, 0.0f};
    CameraShare close = shared_camera(first, {500.0f, 0.0f, 0.0f});
    ok &= expect(near(close.offset.x, 200.0f) && !close.out_of_range, "close together: the midpoint");
    CameraShare edge = shared_camera(first, {100.0f + CameraTuning{}.share, 0.0f, 0.0f});
    ok &= expect(near(edge.offset.x, CameraTuning{}.share * 0.5f), "full share up to the share distance");
    const float halfway = (CameraTuning{}.share + CameraTuning{}.leash) * 0.5f;
    CameraShare middle = shared_camera(first, {100.0f + halfway, 0.0f, 0.0f});
    ok &= expect(near(middle.offset.x, halfway * 0.25f, 1.0f), "half weight halfway to the leash");
    CameraShare far = shared_camera(first, {100.0f + CameraTuning{}.leash + 1.0f, 0.0f, 0.0f});
    ok &= expect(far.out_of_range && near(far.offset.x, 0.0f), "past the leash: the first player only");
    CameraShare broken = shared_camera(first, {NAN, 0.0f, 0.0f});
    ok &= expect(!broken.out_of_range && near(broken.offset.x, 0.0f), "a bad position changes nothing");
    return ok;
}

bool test_body_push() {
    bool ok = true;
    const Vec3 up{0.0f, 1.0f, 0.0f};
    const BodyTuning tuning{};
    Vec3 push = body_push({0.0f, 0.0f, 0.0f}, {80.0f, 0.0f, 0.0f}, up);
    ok &= expect(near(push.x, 10.0f) && near(push.y, 0.0f), "overlapping by 20: each moves 10 apart");
    push = body_push({0.0f, 0.0f, 0.0f}, {20.0f, 30.0f, 0.0f}, up);
    ok &= expect(near(push.x, tuning.max_push) && near(push.y, 0.0f), "capped, and only across the up vector");
    push = body_push({0.0f, 0.0f, 0.0f}, {120.0f, 0.0f, 0.0f}, up);
    ok &= expect(near(push.x, 0.0f), "not touching: no push");
    push = body_push({0.0f, 0.0f, 0.0f}, {10.0f, tuning.height + 1.0f, 0.0f}, up);
    ok &= expect(near(push.x, 0.0f), "one above the other's head: no push");
    push = body_push({0.0f, 0.0f, 0.0f}, {0.0f, 40.0f, 0.0f}, up);
    ok &= expect(length(push) > 1.0f && near(push.y, 0.0f), "exactly on top: pushed sideways");
    push = body_push({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 80.0f}, {1.0f, 0.0f, 0.0f});
    ok &= expect(near(push.z, 10.0f) && near(push.x, 0.0f), "follows the up vector of other gravity");
    push = body_push({0.0f, 0.0f, 0.0f}, {NAN, 0.0f, 0.0f}, up);
    ok &= expect(near(push.x, 0.0f), "a bad position changes nothing");
    return ok;
}

bool test_split_screen() {
    bool ok = true;
    const SplitTuning tuning{};
    SplitState split;
    ok &= expect(split.update(tuning.split_at - 1.0f) == 0.0f, "close: one screen");
    float amount = 0.0f;
    for (unsigned frame = 0; frame < SplitState::kTransitionFrames; ++frame) amount = split.update(tuning.split_at + 10.0f);
    ok &= expect(near(amount, 1.0f) && split.split(), "far apart: fully split after the transition");
    ok &= expect(near(split.update(tuning.merge_at + 10.0f), 1.0f), "between the thresholds: stays split");
    (void)split.update(tuning.merge_at - 10.0f);
    ok &= expect(!split.split() && split.amount() < 1.0f && split.amount() > 0.0f, "close again: joining, gradually");
    split.reset();
    (void)split.update(tuning.split_at + 1.0f);
    ok &= expect(split.amount() > 0.0f && split.eased() < split.amount(), "the transition starts gently");

    // Viewports stay full-screen; the camera's picture moves into its half. At the split it does not move,
    // so each half shows its own side of the shared picture; split, the camera's centre is the half's.
    std::array<float, 16> projection{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.5f, 0.0f, 0.0f,
                                     0.0f, 0.0f, -1.0f, -2.0f, 0.0f, 0.0f, -1.0f, 0.0f};
    const auto ndc_x = [](const std::array<float, 16>& m, Vec3 v) {
        const float x = m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3];
        const float w = m[12] * v.x + m[13] * v.y + m[14] * v.z + m[15];
        return x / w;
    };
    std::array<float, 16> at_split = projection;
    split_projection(at_split, true, 0.0f);
    ok &= expect(at_split == projection, "at the split: the shared picture, unmoved");
    std::array<float, 16> right = projection;
    split_projection(right, true, 1.0f);
    ok &= expect(near(ndc_x(right, {0.0f, 0.0f, -10.0f}), 0.5f) && near(ndc_x(right, {5.0f, 0.0f, -10.0f}), 1.0f),
                 "split: the right camera's centre is the right half's, at the same scale");
    std::array<float, 16> left = projection;
    split_projection(left, false, 1.0f);
    ok &= expect(near(ndc_x(left, {0.0f, 0.0f, -10.0f}), -0.5f) && near(ndc_x(left, {-5.0f, 0.0f, -10.0f}), -1.0f),
                 "split: the left camera's centre is the left half's");
    std::array<float, 16> halfway = projection;
    split_projection(halfway, true, 0.5f);
    ok &= expect(near(ndc_x(halfway, {0.0f, 0.0f, -10.0f}), 0.25f), "halfway through, halfway there");

    const CameraMatrices camera = look_at({0.0f, 0.0f, 10.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
    const Vec3 seen = transform_point(camera.view, {0.0f, 0.0f, 0.0f});
    ok &= expect(near(seen.x, 0.0f) && near(seen.z, -10.0f), "look_at looks down -z at the target");
    ok &= expect(near(pose_of(camera.inverse).eye.z, 10.0f) && near(pose_of(camera.inverse).forward.z, -1.0f),
                 "the inverse holds the eye and the forward direction");
    const CameraMatrices other = look_at({10.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
    ok &= expect(near(pose_of(blend_cameras(camera, other, 0.0f).inverse).eye.z, 10.0f) &&
                     near(pose_of(blend_cameras(camera, other, 1.0f).inverse).eye.x, 10.0f),
                 "blending runs from one camera to the other");
    ok &= expect(sphere_in_view(camera.view, {0.0f, 0.0f, 0.0f}, 1.0f, 45.0f, 1.0f, 1000.0f), "in front: seen");
    ok &= expect(!sphere_in_view(camera.view, {0.0f, 0.0f, 20.0f}, 1.0f, 45.0f, 1.0f, 1000.0f), "behind: not seen");
    ok &= expect(!sphere_in_view(camera.view, {50.0f, 0.0f, 0.0f}, 1.0f, 45.0f, 1.0f, 1000.0f), "far to the side: not seen");
    ok &= expect(sphere_in_view(camera.view, {50.0f, 0.0f, 0.0f}, 60.0f, 45.0f, 1.0f, 1000.0f), "a big sphere reaching in: seen");

    FollowCamera follow;
    follow.start({0.0f, 300.0f, 1000.0f}, {0.0f, 0.0f, 0.0f}, {2000.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    CameraMatrices view{};
    for (unsigned frame = 0; frame < 200u; ++frame) view = follow.update({2000.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const Vec3 eye = pose_of(view.inverse).eye;
    const Vec3 behind = eye - Vec3{2000.0f, tuning.look_height, 0.0f};
    ok &= expect(near(std::sqrt(behind.x * behind.x + behind.z * behind.z), tuning.distance, 1.0f) &&
                     near(behind.y, tuning.height, 1.0f),
                 "the follow camera settles behind and above its player");
    ok &= expect(near(transform_point(view.view, {2000.0f, tuning.look_height, 0.0f}).x, 0.0f, 0.01f),
                 "and looks at them");

    ScreenRect full{0.0f, 0.0f, 640.0f, 456.0f};
    ok &= expect(half_of(full, true) && near(full.left, 320.0f) && near(full.width, 320.0f), "full screen: the right half");
    ScreenRect corner{0.0f, 0.0f, 256.0f, 256.0f};
    ok &= expect(!half_of(corner, false) && near(corner.width, 256.0f), "an effect's corner scissor is left alone");
    return ok;
}

bool test_leash() {
    bool ok = true;
    LeashTimer leash;
    bool warped = false;
    for (unsigned frame = 0; frame + 1u < 120u; ++frame) warped = warped || leash.update(true, 120u);
    ok &= expect(!warped, "not before the warp delay");
    ok &= expect(leash.update(true, 120u), "warp on the delay's last frame");
    ok &= expect(!leash.update(true, 120u), "and the count starts again");
    for (unsigned frame = 0; frame < 100u; ++frame) (void)leash.update(true, 120u);
    (void)leash.update(false, 120u);
    for (unsigned frame = 0; frame + 1u < 120u; ++frame) warped = warped || leash.update(true, 120u);
    ok &= expect(!warped, "coming back in range resets the count");
    return ok;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= test_characters_and_archives();
    passed &= test_kpad_layout_and_edges();
    passed &= test_shake_is_twelve_frames_per_press();
    passed &= test_partner_input_from_bindings();
    passed &= test_shared_camera();
    passed &= test_body_push();
    passed &= test_split_screen();
    passed &= test_leash();
    if (!passed) {
        return 1;
    }
    std::cout << "coop_tests: passed\n";
    return 0;
}
