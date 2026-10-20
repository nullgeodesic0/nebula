#include "galaxy/coop.h"
#include "galaxy/gx/split_clip.h"

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
    ok &= expect(split.update(tuning.split_extent - 0.05f, 1000.0f) == 0.0f, "both well in the picture: one screen");
    float amount = 0.0f;
    for (unsigned frame = 0; frame < SplitState::kTransitionFrames; ++frame) {
        amount = split.update(tuning.split_extent + 0.01f, 1000.0f);
    }
    ok &= expect(near(amount, 1.0f) && split.split(), "a player about to leave the picture: fully split after the transition");
    ok &= expect(near(split.update(tuning.merge_extent + 0.05f, 1000.0f), 1.0f), "between the thresholds: stays split");
    (void)split.update(tuning.merge_extent - 0.05f, 1000.0f);
    ok &= expect(!split.split() && split.amount() < 1.0f && split.amount() > 0.0f, "back in the picture: joining, gradually");
    split.reset();
    (void)split.update(0.1f, tuning.split_at + 1.0f);
    ok &= expect(split.split(), "very far apart: split whatever the picture");
    ok &= expect(split.amount() > 0.0f && split.eased() < split.amount(), "the transition starts gently");
    SplitState ends;
    for (unsigned frame = 0; frame + 1u < SplitState::kTransitionFrames; ++frame) (void)ends.update(1.0f, 0.0f);
    ok &= expect(ends.eased() > ends.amount() && ends.eased() < 1.0f, "and ends gently");

    // How far towards the edge of a shared picture: players across the screen of a camera looking down -z.
    const CameraMatrices shared = look_at({0.0f, 0.0f, 1000.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 0.0f});
    const float tan_half = std::tan(45.0f * 0.5f * 3.14159265f / 180.0f);
    const float edge = shared_view_extent(shared, {-1000.0f * tan_half, 0.0f, 0.0f}, {1000.0f * tan_half, 0.0f, 0.0f},
                                          1000.0f, 45.0f, 1.0f);
    ok &= expect(near(edge, 1.0f, 0.01f), "players at the picture's edges: extent 1");
    ok &= expect(shared_view_extent(shared, {-100.0f, 0.0f, 0.0f}, {100.0f, 0.0f, 0.0f}, 1000.0f, 45.0f, 2.0f) < 0.2f,
                 "close together: near the middle");
    ok &= expect(shared_view_extent(shared, {0.0f, 0.0f, -1500.0f}, {0.0f, 0.0f, 1500.0f}, 1000.0f, 45.0f, 1.0f) > 1.0f,
                 "one behind the camera: cannot be framed");

    // Viewports stay full-screen; the camera's picture moves into its half. At the split it does not move,
    // so each half shows its own side of the shared picture; split, the camera's centre is the half's.
    std::array<float, 16> projection{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.5f, 0.0f, 0.0f,
                                     0.0f, 0.0f, -1.0f, -2.0f, 0.0f, 0.0f, -1.0f, 0.0f};
    const auto ndc_x = [](const std::array<float, 16>& m, Vec3 v) {
        const float x = m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3];
        const float w = m[12] * v.x + m[13] * v.y + m[14] * v.z + m[15];
        return x / w;
    };
    const auto ndc_y = [](const std::array<float, 16>& m, Vec3 v) {
        const float y = m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7];
        const float w = m[12] * v.x + m[13] * v.y + m[14] * v.z + m[15];
        return y / w;
    };
    std::array<float, 16> at_split = projection;
    split_projection(at_split, 0.5f, 0.0f, 0.0f);
    ok &= expect(at_split == projection, "at the split: the shared picture, unmoved");
    std::array<float, 16> right = projection;
    split_projection(right, 0.5f, 0.0f, 1.0f);
    ok &= expect(near(ndc_x(right, {0.0f, 0.0f, -10.0f}), 0.5f) && near(ndc_x(right, {5.0f, 0.0f, -10.0f}), 1.0f),
                 "split: the camera's centre moves to its half's, at the same scale");
    std::array<float, 16> diagonal = projection;
    split_projection(diagonal, -0.3f, 0.4f, 1.0f);
    ok &= expect(near(ndc_x(diagonal, {0.0f, 0.0f, -10.0f}), -0.3f) && near(ndc_y(diagonal, {0.0f, 0.0f, -10.0f}), 0.4f),
                 "an angled half: the centre moves in both directions");
    std::array<float, 16> halfway = projection;
    split_projection(halfway, 0.5f, 0.0f, 0.5f);
    ok &= expect(near(ndc_x(halfway, {0.0f, 0.0f, -10.0f}), 0.25f), "halfway through, halfway there");

    // The layout: the second player to the right of the first on screen splits left and right, the second
    // player taking the right; below, top and bottom; the line turns smoothly as they move around.
    SplitLayout layout;
    layout.update(100.0f, 0.0f);
    std::array<float, 3> plane = layout.plane(true, 640.0f, 456.0f);
    ok &= expect(plane[0] * 600.0f + plane[1] * 200.0f + plane[2] > 0.0f &&
                     plane[0] * 40.0f + plane[1] * 200.0f + plane[2] < 0.0f,
                 "second player to the right: they keep the right of the screen");
    std::array<float, 3> first_plane = layout.plane(false, 640.0f, 456.0f);
    ok &= expect(first_plane[0] * 40.0f + first_plane[1] * 200.0f + first_plane[2] > 0.0f, "and the first the left");
    std::array<float, 2> centre = layout.centre(true, 640.0f, 456.0f);
    ok &= expect(near(centre[0], 0.5f, 0.01f) && near(centre[1], 0.0f, 0.01f), "the right half's middle");
    SplitLayout below;
    below.update(0.0f, 100.0f);
    centre = below.centre(true, 640.0f, 456.0f);
    ok &= expect(near(centre[0], 0.0f, 0.01f) && near(centre[1], -0.5f, 0.01f), "second player below: the bottom half");
    SplitLayout turning;
    turning.update(100.0f, 0.0f);
    turning.update(0.0f, 100.0f);
    ok &= expect(turning.angle() > 0.0f && turning.angle() <= SplitLayout::kTurnPerFrame + 1e-5f,
                 "the line turns towards the new direction, a little a frame");
    for (unsigned frame = 0; frame < 200u; ++frame) turning.update(0.0f, 100.0f);
    ok &= expect(near(turning.angle(), 1.5708f, 0.01f), "and settles on it");
    SplitLayout diagonal_layout;
    diagonal_layout.update(100.0f, 100.0f);
    centre = diagonal_layout.centre(true, 640.0f, 456.0f);
    ok &= expect(centre[0] > 0.1f && centre[1] < -0.1f, "a diagonal split: the lower right");
    turning.update(0.0f, 0.0f);
    ok &= expect(near(turning.angle(), 1.5708f, 0.01f), "no direction: the line stays");

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

    // The second player's controls: turning orbits around them at the same distance; raising lifts it.
    const Vec3 before = follow.eye();
    follow.steer(0.5f, 0.0f);
    const Vec3 after = follow.eye();
    const Vec3 look = follow.look();
    ok &= expect(near(length(after - look), length(before - look), 0.5f) && !near(after.x, before.x, 1.0f),
                 "turning orbits the camera around the player");
    for (unsigned frame = 0; frame < 200u; ++frame) {
        follow.steer(0.0f, 50.0f);
        view = follow.update({2000.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    }
    ok &= expect(near(pose_of(view.inverse).eye.y - tuning.look_height, tuning.max_height, 1.0f),
                 "raising stops at the top of its range");
    follow.recentre({2000.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
    ok &= expect(follow.eye().x < 2000.0f - tuning.distance * 0.9f, "recentring puts the camera behind where they face");

    PartnerInput input;
    input.connected = true;
    input.camera_x = 1.0f;
    CameraSteer steer = camera_steer(input, 0u);
    ok &= expect(near(steer.turn, kSteerTurnPerFrame) && near(steer.raise, 0.0f), "the right stick turns the camera");
    input.camera_x = 0.1f;
    input.buttons = button::kUp | button::kC;
    steer = camera_steer(input, 0u);
    ok &= expect(near(steer.turn, 0.0f) && near(steer.raise, kSteerRaisePerFrame) && steer.recentre,
                 "a resting stick does nothing; D-pad up raises; C recentres");
    ok &= expect(!camera_steer(input, button::kC).recentre, "a held C recentres once");

    // Camera collision: short of the wall, towards the player.
    const Vec3 stop = camera_before_wall({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 500.0f}, 40.0f);
    ok &= expect(near(stop.z, 460.0f), "the camera stops short of the wall");
    ok &= expect(camera_before_wall({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 20.0f}, 40.0f).z < 20.0f,
                 "a wall right behind the player: the camera stays in front of it");
    return ok;
}

bool test_camera_player() {
    bool ok = true;
    constexpr std::uint32_t first = 0x80A00000u;
    constexpr std::uint32_t second = 0x80B00000u;
    ok &= expect(camera_player(second, first, second) == first, "an event caught the second player: back to the first");
    ok &= expect(camera_player(first, first, second) == first, "the first player stays");
    ok &= expect(camera_player(0x80C00000u, first, second) == 0x80C00000u, "an event's own target is left alone");
    ok &= expect(camera_player(0u, first, 0u) == 0u, "no second player: nothing changes");
    ok &= expect(camera_event_is_second(second, second, true, false, false),
                 "the second player's warp camera stays theirs while it plays");
    ok &= expect(!camera_event_is_second(second, second, false, true, false),
                 "and the camera is the first player's once it ends");
    ok &= expect(!camera_event_is_second(first, second, true, false, false), "an event on the first player is theirs");
    ok &= expect(camera_event_is_second(first, second, true, true, false),
                 "a launch star started for the second player (only they are flying) is theirs");
    ok &= expect(!camera_event_is_second(first, second, true, true, true), "both in special states: the first player's");
    ok &= expect(!camera_event_is_second(0u, 0u, true, true, false), "no second player: no swap");
    return ok;
}

// The renderer's form of the line: the vertex shader's clip distance in clip space.
bool test_split_clip_vs_words() {
    bool ok = true;
    const auto distance = [](const std::array<std::uint32_t, 2>& words, float x_ndc, float y_ndc) {
        const float angle = std::bit_cast<float>(words[0]) - 16.0f;
        const float w = 2.0f;  // any positive w: clip space is NDC times w
        return std::cos(angle) * x_ndc * w + std::sin(angle) * y_ndc * w + std::bit_cast<float>(words[1]) * w;
    };
    // Keep x >= 320 of a 640 x 456 screen.
    const auto right = galaxy::gx::split_clip_vs_words({1.0f, 0.0f, -320.0f}, 320.0f, 228.0f, 0.0f, 0.0f);
    ok &= expect(right[0] != 0u, "a line has words");
    ok &= expect(distance(right, 0.5f, 0.0f) > 0.0f && distance(right, -0.5f, 0.0f) < 0.0f,
                 "right of the middle kept, left clipped");
    ok &= expect(std::fabs(distance(right, 0.0f, 0.3f)) < 1e-4f, "the line runs through the middle");
    // Keep y >= 300 (pixels run down; NDC y up): the bottom.
    const auto bottom = galaxy::gx::split_clip_vs_words({0.0f, 1.0f, -300.0f}, 320.0f, 228.0f, 0.0f, 0.0f);
    const float y_ndc_300 = 1.0f - 300.0f / 228.0f;
    ok &= expect(distance(bottom, 0.0f, y_ndc_300 - 0.1f) > 0.0f && distance(bottom, 0.0f, y_ndc_300 + 0.1f) < 0.0f,
                 "below the line kept, above clipped");
    const auto none = galaxy::gx::split_clip_vs_words({0.0f, 0.0f, 0.0f}, 320.0f, 228.0f, 0.0f, 0.0f);
    ok &= expect(none[0] == 0u && none[1] == 0u, "no line: no words (nothing clipped)");
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
    passed &= test_camera_player();
    passed &= test_split_clip_vs_words();
    passed &= test_leash();
    if (!passed) {
        return 1;
    }
    std::cout << "coop_tests: passed\n";
    return 0;
}
