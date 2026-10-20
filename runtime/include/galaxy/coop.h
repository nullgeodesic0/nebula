#pragma once

// Mario and Luigi co-op: plain logic for the second player (coop_guest.cpp drives the game with it).
// Nothing here touches the guest; galaxy_coop_tests covers it.

#include "galaxy/input_bindings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>

namespace galaxy::coop {

enum class Character : std::uint8_t { Mario, Luigi };

[[nodiscard]] constexpr Character partner_of(Character own) noexcept {
    return own == Character::Mario ? Character::Luigi : Character::Mario;
}

// The player archives the game keeps resident for one character (StationedFileInfo.cpp in Petari):
// the second player needs the other character's model, face and shadow next to them. MarioAnime.arc
// (both characters' animations) is shared.
struct CharacterArchives {
    std::string_view model_path;   // mounted from the disc
    std::string_view face_path;
    std::string_view shadow_path;
    std::string_view model_holder;  // ResourceHolder name of the model archive
    std::string_view model_name;    // the J3D model inside it
};

[[nodiscard]] constexpr CharacterArchives archives_of(Character character) noexcept {
    if (character == Character::Luigi) {
        return {"/ObjectData/Luigi.arc", "/ObjectData/LuigiFace.arc", "/ObjectData/LuigiShadow.arc", "Luigi.arc",
                "luigi"};
    }
    return {"/ObjectData/Mario.arc", "/ObjectData/MarioFace.arc", "/ObjectData/MarioShadow.arc", "Mario.arc", "mario"};
}

// The model archive the game mounts for its own player, which says which character's set is loading.
[[nodiscard]] constexpr bool own_model_archive(std::string_view path, Character& character) noexcept {
    for (const Character c : {Character::Mario, Character::Luigi}) {
        if (path == archives_of(c).model_path) {
            character = c;
            return true;
        }
    }
    return false;
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
constexpr Vec3 operator*(Vec3 a, float k) noexcept { return {a.x * k, a.y * k, a.z * k}; }
inline float length(Vec3 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
[[nodiscard]] inline float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

// KPAD button bits (revolution/kpad.h, the Wii Remote's core buttons plus the Nunchuk's Z and C).
namespace button {
inline constexpr std::uint32_t kLeft = 0x0001u;
inline constexpr std::uint32_t kRight = 0x0002u;
inline constexpr std::uint32_t kDown = 0x0004u;
inline constexpr std::uint32_t kUp = 0x0008u;
inline constexpr std::uint32_t kPlus = 0x0010u;
inline constexpr std::uint32_t kTwo = 0x0100u;
inline constexpr std::uint32_t kOne = 0x0200u;
inline constexpr std::uint32_t kB = 0x0400u;
inline constexpr std::uint32_t kA = 0x0800u;
inline constexpr std::uint32_t kMinus = 0x1000u;
inline constexpr std::uint32_t kZ = 0x2000u;
inline constexpr std::uint32_t kC = 0x4000u;
inline constexpr std::uint32_t kHome = 0x8000u;
}  // namespace button

// The second player's controller, as a Wii Remote and Nunchuk: buttons (button::k*), the Nunchuk stick
// (-1..1, up positive) and a shake (the spin). `camera_x`/`camera_y` are the stick bound to the pointer
// (usually the right stick): the second player does not aim, so it turns their split-screen camera.
struct PartnerInput {
    bool connected = false;
    std::uint32_t buttons = 0u;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    bool shake = false;
    float camera_x = 0.0f;
    float camera_y = 0.0f;
};

// The second player's Wii Remote and Nunchuk from their controller's bindings (the same tables as the first
// player's). JumpBack is A: the second player is always in play.
[[nodiscard]] inline PartnerInput partner_input_from(const input::LogicalInput& logic, bool shaking) noexcept {
    using input::WiiTarget;
    PartnerInput input;
    input.connected = true;
    const auto bit = [&](WiiTarget target, std::uint32_t mask) { return logic.held(target) ? mask : 0u; };
    input.buttons = bit(WiiTarget::A, button::kA) | bit(WiiTarget::JumpBack, button::kA) |
                    bit(WiiTarget::B, button::kB) | bit(WiiTarget::One, button::kOne) |
                    bit(WiiTarget::Two, button::kTwo) | bit(WiiTarget::Plus, button::kPlus) |
                    bit(WiiTarget::Minus, button::kMinus) | bit(WiiTarget::DpadUp, button::kUp) |
                    bit(WiiTarget::DpadDown, button::kDown) | bit(WiiTarget::DpadLeft, button::kLeft) |
                    bit(WiiTarget::DpadRight, button::kRight) | bit(WiiTarget::C, button::kC) |
                    bit(WiiTarget::Z, button::kZ);
    input.stick_x = logic.stick_x;
    input.stick_y = logic.stick_y;
    input.shake = shaking || logic.held(WiiTarget::Shake) || logic.held(WiiTarget::NunchukShake);
    input.camera_x = logic.pointer_x;
    input.camera_y = logic.pointer_y;
    return input;
}

// KPADStatus (revolution/kpad.h): 0x84 bytes, big-endian.
inline constexpr std::size_t kKpadStatusSize = 0x84u;
using KpadStatusBytes = std::array<std::uint8_t, kKpadStatusSize>;

namespace kpad {
inline constexpr std::size_t kHold = 0x00u;
inline constexpr std::size_t kTrig = 0x04u;
inline constexpr std::size_t kRelease = 0x08u;
inline constexpr std::size_t kAcc = 0x0Cu;
inline constexpr std::size_t kAccValue = 0x18u;
inline constexpr std::size_t kAccSpeed = 0x1Cu;
inline constexpr std::size_t kDist = 0x48u;
inline constexpr std::size_t kDevType = 0x5Cu;
inline constexpr std::size_t kWpadErr = 0x5Du;
inline constexpr std::size_t kDpdValid = 0x5Eu;
inline constexpr std::size_t kDataFormat = 0x5Fu;
inline constexpr std::size_t kFsStick = 0x60u;
inline constexpr std::size_t kFsAcc = 0x68u;
inline constexpr std::size_t kFsAccValue = 0x74u;
inline constexpr std::uint8_t kDevFreestyle = 1u;    // WPAD_DEV_FREESTYLE: Remote and Nunchuk
inline constexpr std::uint8_t kFormatFreestyleAcc = 3u;  // WPAD_FMT_FREESTYLE_ACC
}  // namespace kpad

inline void put_u32(KpadStatusBytes& bytes, std::size_t at, std::uint32_t value) noexcept {
    bytes[at] = static_cast<std::uint8_t>(value >> 24u);
    bytes[at + 1u] = static_cast<std::uint8_t>(value >> 16u);
    bytes[at + 2u] = static_cast<std::uint8_t>(value >> 8u);
    bytes[at + 3u] = static_cast<std::uint8_t>(value);
}
inline void put_f32(KpadStatusBytes& bytes, std::size_t at, float value) noexcept {
    put_u32(bytes, at, std::bit_cast<std::uint32_t>(value));
}
[[nodiscard]] inline std::uint32_t get_u32(const KpadStatusBytes& bytes, std::size_t at) noexcept {
    return (static_cast<std::uint32_t>(bytes[at]) << 24u) | (static_cast<std::uint32_t>(bytes[at + 1u]) << 16u) |
           (static_cast<std::uint32_t>(bytes[at + 2u]) << 8u) | bytes[at + 3u];
}
[[nodiscard]] inline float get_f32(const KpadStatusBytes& bytes, std::size_t at) noexcept {
    return std::bit_cast<float>(get_u32(bytes, at));
}

// Builds the second player's KPADStatus once a game frame, so the game's own pad code (button edges, the
// Nunchuk stick, the swing that makes Mario spin) reads channel 1 as a Remote with a Nunchuk.
class KpadSynth {
public:
    // The shake: the same twelve frames of alternating acceleration the runtime gives the first player's
    // Remote (native_input.cpp advance_wiimote_shake_accel), in KPAD's units (g; at rest (0, -1, 0)).
    static constexpr std::uint8_t kShakeFrames = 12u;

    [[nodiscard]] KpadStatusBytes next(const PartnerInput& input) noexcept {
        KpadStatusBytes bytes{};
        const std::uint32_t hold = input.connected ? input.buttons : 0u;
        put_u32(bytes, kpad::kHold, hold);
        put_u32(bytes, kpad::kTrig, hold & ~previous_hold_);
        put_u32(bytes, kpad::kRelease, previous_hold_ & ~hold);
        previous_hold_ = hold;

        if (input.connected && input.shake && !previous_shake_) shake_left_ = kShakeFrames;
        previous_shake_ = input.connected && input.shake;
        Vec3 acc{0.0f, -1.0f, 0.0f};
        if (shake_left_ != 0u) {
            const bool phase = (shake_left_ & 1u) != 0u;
            acc = phase ? Vec3{-2.24f, 2.08f, 0.0f} : Vec3{2.24f, -2.08f, 0.0f};
            --shake_left_;
        }
        put_f32(bytes, kpad::kAcc, acc.x);
        put_f32(bytes, kpad::kAcc + 4u, acc.y);
        put_f32(bytes, kpad::kAcc + 8u, acc.z);
        put_f32(bytes, kpad::kAccValue, length(acc));
        put_f32(bytes, kpad::kAccSpeed, length(acc - previous_acc_));
        previous_acc_ = acc;
        put_f32(bytes, kpad::kDist, 0.0f);
        bytes[kpad::kDevType] = kpad::kDevFreestyle;
        bytes[kpad::kWpadErr] = 0u;
        bytes[kpad::kDpdValid] = 0u;  // no pointer: the second player does not aim
        bytes[kpad::kDataFormat] = kpad::kFormatFreestyleAcc;
        put_f32(bytes, kpad::kFsStick, input.connected ? std::clamp(input.stick_x, -1.0f, 1.0f) : 0.0f);
        put_f32(bytes, kpad::kFsStick + 4u, input.connected ? std::clamp(input.stick_y, -1.0f, 1.0f) : 0.0f);
        put_f32(bytes, kpad::kFsAcc, 0.0f);
        put_f32(bytes, kpad::kFsAcc + 4u, -1.0f);
        put_f32(bytes, kpad::kFsAcc + 8u, 0.0f);
        put_f32(bytes, kpad::kFsAccValue, 1.0f);
        return bytes;
    }

    void reset() noexcept { *this = KpadSynth{}; }

private:
    std::uint32_t previous_hold_ = 0u;
    bool previous_shake_ = false;
    std::uint8_t shake_left_ = 0u;
    Vec3 previous_acc_{0.0f, -1.0f, 0.0f};
};

// The shared camera. The game's camera follows the first player's camera point; it is moved towards the
// second player so both stay in view. Up to `share` units apart the camera looks at the midpoint; it hands
// back to the first player by `leash`. Farther than `leash` for `warp_frames` frames, the second player is
// brought back to the first. Units are about a centimetre (Mario is about 160 tall).
// (With split screen, SplitTuning instead.)
struct CameraTuning {
    float share = 1200.0f;
    float leash = 2600.0f;
    unsigned warp_frames = 120u;
};

// With split screen the camera looks at the midpoint until the split, then eases to the first player as
// the screen splits (SplitState).
struct SplitTuning {
    // The screen splits when a player is about to leave the shared picture: past `split_extent` of the way
    // from its middle to its edge (shared_view_extent), and joins again below `merge_extent`. Farther apart
    // than `split_at` it splits whatever the picture.
    float split_extent = 0.85f;
    float merge_extent = 0.6f;
    float split_at = 6000.0f;
    float split_warp = 15000.0f;
    // The second player's camera: `distance` behind and `height` above the point it looks at, which is
    // `look_height` above the player; it trails the player (a lazy follow) and eases by `follow` a frame.
    float distance = 1000.0f;
    float height = 380.0f;
    float look_height = 110.0f;
    float follow = 0.2f;
    float up_follow = 0.1f;
    // The range the second player can raise or lower their camera to (as `height`).
    float min_height = -150.0f;
    float max_height = 1100.0f;
    // Camera collision: the camera stops this far short of a wall between it and the player.
    float wall_margin = 40.0f;
};

// Splits past `split_at` and joins below `merge_at`, so the screen does not flicker at one distance, and
// moves between the two over `transition_frames` (Lego style, seamless):
//
// - At amount 0 there is one screen; the camera looks at the players' midpoint.
// - As the amount rises, the screen is drawn twice, each half first showing exactly its part of the shared
//   picture (an off-centre projection, split_projection), so nothing moves at the split. Each half then slides
//   to centre on its own player as the first player's camera eases from the midpoint to them and the second
//   player's camera pans from the first player's view to its own.
// - Joining runs the same backwards: the halves meet in one picture before the second draw stops.
class SplitState {
public:
    static constexpr unsigned kTransitionFrames = 75u;  // 1.25 s

    // The split amount after this frame, 0..1. `extent`: how far towards the edge of the shared picture the
    // players are (shared_view_extent); `distance` between them.
    float update(float extent, float distance, const SplitTuning& tuning = {}) noexcept {
        const bool far = std::isfinite(distance) && distance >= tuning.split_at;
        if (std::isfinite(extent)) {
            if (!split_ && (extent >= tuning.split_extent || far)) split_ = true;
            else if (split_ && extent < tuning.merge_extent && !far) split_ = false;
        } else if (far) {
            split_ = true;
        }
        const float step = 1.0f / static_cast<float>(kTransitionFrames);
        amount_ = split_ ? std::min(1.0f, amount_ + step) : std::max(0.0f, amount_ - step);
        return amount_;
    }
    [[nodiscard]] bool split() const noexcept { return split_; }
    // 0: one screen; 1: fully split.
    [[nodiscard]] float amount() const noexcept { return amount_; }
    // The amount eased (smootherstep: still at both ends, so the halves leave and meet the shared picture
    // without a jolt), for the cameras and projections.
    [[nodiscard]] float eased() const noexcept {
        const float t = amount_;
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }
    void reset() noexcept { *this = SplitState{}; }

private:
    bool split_ = false;
    float amount_ = 0.0f;
};

// The projection of one half. Viewports stay full-screen (the game's screen-space passes, such as the
// shadow and bloom composites, draw textures of the whole screen and must not be squeezed), the renderer
// keeps each half's pixels on its side of the line (SplitLayout::plane), and the camera's picture is moved
// into its half instead: its centre shifts towards the middle of the half (`centre_x`, `centre_y` in
// normalized device coordinates, y up) as the split eases in (`eased` 0..1). At the split the shift is 0,
// so each half shows exactly its part of the shared picture; split, each half is centred on its own camera
// at the same scale. A GameCube perspective projection's last row is (0, 0, -1, 0): rows 0 and 1 become
// row + shift * row3.
inline void split_projection(std::array<float, 16>& projection, float centre_x, float centre_y, float eased) noexcept {
    for (unsigned column = 0; column < 4u; ++column) {
        projection[column] += centre_x * eased * projection[12u + column];
        projection[4u + column] += centre_y * eased * projection[12u + column];
    }
}

// Lego Indiana Jones 2 style: the line between the halves runs through the middle of the screen, square to
// where the second player is from the first on the first player's screen, so each takes the side of the
// screen they are on and the line turns as they move around each other. Screen pixels, y down.
class SplitLayout {
public:
    static constexpr float kTurnPerFrame = 0.06f;  // radians, the most the line turns in a frame
    static constexpr float kTurnEase = 0.15f;

    // `dx`, `dy`: the second player from the first, on screen. The first call sets the line; later ones
    // turn it towards the new direction, smoothly. A direction too short to tell keeps the line.
    void update(float dx, float dy) noexcept {
        if (!std::isfinite(dx) || !std::isfinite(dy) || dx * dx + dy * dy < 1e-6f) return;
        const float target = std::atan2(dy, dx);
        if (!started_) {
            angle_ = target;
            started_ = true;
            return;
        }
        constexpr float kPi = 3.14159265f;
        float turn = target - angle_;
        while (turn > kPi) turn -= 2.0f * kPi;
        while (turn < -kPi) turn += 2.0f * kPi;
        angle_ += std::clamp(turn * kTurnEase, -kTurnPerFrame, kTurnPerFrame);
        if (angle_ > kPi) angle_ -= 2.0f * kPi;
        if (angle_ < -kPi) angle_ += 2.0f * kPi;
    }
    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] float angle() const noexcept { return angle_; }
    void reset() noexcept { *this = SplitLayout{}; }

    // The half a player keeps: a * x + b * y + c >= 0 in screen pixels (the renderer's SplitClip line).
    [[nodiscard]] std::array<float, 3> plane(bool second, float width, float height) const noexcept {
        const float sign = second ? 1.0f : -1.0f;
        const float a = std::cos(angle_) * sign;
        const float b = std::sin(angle_) * sign;
        return {a, b, -(a * width * 0.5f + b * height * 0.5f)};
    }

    // The middle (centroid) of a player's half, in normalized device coordinates (y up).
    [[nodiscard]] std::array<float, 2> centre(bool second, float width, float height) const noexcept {
        const std::array<float, 3> p = plane(second, width, height);
        // The screen rectangle cut by the line (Sutherland-Hodgman), then its centroid.
        const float corners[4][2] = {{0.0f, 0.0f}, {width, 0.0f}, {width, height}, {0.0f, height}};
        float poly[8][2]{};
        unsigned n = 0u;
        for (unsigned i = 0; i < 4u; ++i) {
            const float* a = corners[i];
            const float* b = corners[(i + 1u) % 4u];
            const float da = p[0] * a[0] + p[1] * a[1] + p[2];
            const float db = p[0] * b[0] + p[1] * b[1] + p[2];
            if (da >= 0.0f) {
                poly[n][0] = a[0];
                poly[n][1] = a[1];
                ++n;
            }
            if ((da >= 0.0f) != (db >= 0.0f)) {
                const float t = da / (da - db);
                poly[n][0] = a[0] + (b[0] - a[0]) * t;
                poly[n][1] = a[1] + (b[1] - a[1]) * t;
                ++n;
            }
        }
        float area = 0.0f;
        float cx = 0.0f;
        float cy = 0.0f;
        for (unsigned i = 0; i < n; ++i) {
            const float* a = poly[i];
            const float* b = poly[(i + 1u) % n];
            const float cross_ab = a[0] * b[1] - b[0] * a[1];
            area += cross_ab;
            cx += (a[0] + b[0]) * cross_ab;
            cy += (a[1] + b[1]) * cross_ab;
        }
        if (n < 3u || std::fabs(area) < 1e-3f) return {0.0f, 0.0f};
        cx /= 3.0f * area;
        cy /= 3.0f * area;
        return {cx / width * 2.0f - 1.0f, 1.0f - cy / height * 2.0f};
    }

private:
    float angle_ = 0.0f;
    bool started_ = false;
};

[[nodiscard]] inline Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline Vec3 normalized(Vec3 v, Vec3 fallback) noexcept {
    const float l = length(v);
    return std::isfinite(l) && l > 1e-4f ? v * (1.0f / l) : fallback;
}

// A camera as the GameCube's MTXLookAt makes it: `view` maps the world to the camera (looking down -z), 3x4
// row-major; `inverse` maps back, with the eye in its last column.
struct CameraMatrices {
    std::array<float, 12> view{};
    std::array<float, 12> inverse{};
};

[[nodiscard]] inline CameraMatrices look_at(Vec3 eye, Vec3 up, Vec3 target) noexcept {
    const Vec3 z = normalized(eye - target, {0.0f, 0.0f, 1.0f});
    const Vec3 x = normalized(cross(up, z), std::fabs(z.y) < 0.9f ? normalized(cross({0.0f, 1.0f, 0.0f}, z), {1.0f, 0.0f, 0.0f})
                                                                  : Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 y = cross(z, x);
    CameraMatrices m;
    m.view = {x.x, x.y, x.z, -dot(x, eye), y.x, y.y, y.z, -dot(y, eye), z.x, z.y, z.z, -dot(z, eye)};
    m.inverse = {x.x, y.x, z.x, eye.x, x.y, y.y, z.y, eye.y, x.z, y.z, z.z, eye.z};
    return m;
}

// A camera's eye, forward (where it looks) and up, from its inverse view matrix.
struct CameraPose {
    Vec3 eye{};
    Vec3 forward{0.0f, 0.0f, -1.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
};
[[nodiscard]] inline CameraPose pose_of(const std::array<float, 12>& inverse) noexcept {
    return {{inverse[3], inverse[7], inverse[11]},
            {-inverse[2], -inverse[6], -inverse[10]},
            {inverse[1], inverse[5], inverse[9]}};
}
// Between two cameras: `t` 0 is `a`, 1 is `b`.
[[nodiscard]] inline CameraMatrices blend_cameras(const CameraMatrices& a, const CameraMatrices& b, float t) noexcept {
    const CameraPose pa = pose_of(a.inverse);
    const CameraPose pb = pose_of(b.inverse);
    const Vec3 eye = pa.eye + (pb.eye - pa.eye) * t;
    const Vec3 forward = normalized(pa.forward + (pb.forward - pa.forward) * t, pb.forward);
    const Vec3 up = normalized(pa.up + (pb.up - pa.up) * t, pb.up);
    return look_at(eye, up, eye + forward * 1000.0f);
}

// When the game camera changes hands in split screen (a camera event the second player sets off begins or
// ends), the half it moves to blends from its own camera into the game's over `kFrames`, eased: the game
// camera's own swing over from the other player is not seen, and the half starts where it was.
class CameraHandover {
public:
    static constexpr unsigned kFrames = 60u;  // 1 s
    // An event camera aims at the game's player (the first) for its first frames, before it follows the
    // second player: their half waits this long before easing in.
    static constexpr unsigned kEventDelayFrames = 10u;

    // `from`: the camera the half had; `second`: whether it is the second player's half; `projection`, the
    // lens it had (eased into the game's too; none keeps the game's).
    void start(const CameraMatrices& from, bool second, const std::array<float, 16>* projection = nullptr,
               unsigned delay_frames = 0u) noexcept {
        bool usable = false;
        for (const float value : from.view) {
            if (!std::isfinite(value)) return;
            usable = usable || value != 0.0f;
        }
        if (!usable) return;
        from_ = from;
        second_ = second;
        has_projection_ = projection != nullptr;
        if (projection != nullptr) from_projection_ = *projection;
        frame_ = 0u;
        delay_ = delay_frames;
        active_ = true;
    }
    [[nodiscard]] bool active() const noexcept { return active_; }
    // The camera for a half drawn with the game camera `game`.
    [[nodiscard]] CameraMatrices camera(const CameraMatrices& game, bool half_is_second) const noexcept {
        if (!active_ || half_is_second != second_) return game;
        return blend_cameras(from_, game, eased());
    }
    // The lens for that half, from its own to the game's.
    [[nodiscard]] std::array<float, 16> projection(const std::array<float, 16>& game, bool half_is_second) const noexcept {
        if (!active_ || !has_projection_ || half_is_second != second_) return game;
        const float t = eased();
        std::array<float, 16> blended{};
        for (std::size_t i = 0; i < blended.size(); ++i) blended[i] = from_projection_[i] + (game[i] - from_projection_[i]) * t;
        return blended;
    }
    void advance() noexcept {
        if (active_ && ++frame_ >= kFrames + delay_) active_ = false;
    }

private:
    [[nodiscard]] float eased() const noexcept {
        const float waited = frame_ > delay_ ? static_cast<float>(frame_ - delay_) : 0.0f;
        const float t = std::clamp(waited / static_cast<float>(kFrames), 0.0f, 1.0f);
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    unsigned delay_ = 0u;
    CameraMatrices from_{};
    std::array<float, 16> from_projection_{};
    bool has_projection_ = false;
    bool second_ = false;
    unsigned frame_ = 0u;
    bool active_ = false;
};

// The first player's view while the second player's camera event has the game camera: their half keeps the
// game camera's last view from before the event, moved with them as they walk. After the event it holds on
// until the game camera is back near that view (it travels back from the second player first), at most
// `kReturnFrames`, and then hands over (CameraHandover).
class CameraHold {
public:
    static constexpr unsigned kReturnFrames = 150u;  // 2.5 s at most
    // Back: the game camera's eye within `kBackDistance` of the first player and moving less than `kSettled`
    // a frame (it has finished travelling back).
    static constexpr float kBackDistance = 2500.0f;
    static constexpr float kSettled = 25.0f;

    // `projection`: the lens the view had (the game's lens changes with the other player's event camera).
    void start(const CameraMatrices& camera, Vec3 player, const std::array<float, 16>& projection) noexcept {
        for (const float value : camera.inverse) {
            if (!std::isfinite(value)) return;
        }
        camera_ = camera;
        projection_ = projection;
        origin_ = player;
        returning_ = false;
        frames_ = 0u;
        active_ = true;
    }
    [[nodiscard]] bool active() const noexcept { return active_; }
    [[nodiscard]] const std::array<float, 16>& projection() const noexcept { return projection_; }
    // The held view, moved with the player.
    [[nodiscard]] CameraMatrices view(Vec3 player) const noexcept {
        const CameraPose pose = pose_of(camera_.inverse);
        Vec3 moved = player - origin_;
        if (!std::isfinite(length(moved))) moved = {};
        return look_at(pose.eye + moved, pose.up, pose.eye + moved + pose.forward * 1000.0f);
    }
    // The event is over: hold until the game camera is back.
    void release() noexcept {
        if (active_) returning_ = true;
    }
    // Still not the first player's camera: hold on (and wait afresh once released again).
    void rearm() noexcept {
        returning_ = false;
        frames_ = 0u;
    }
    [[nodiscard]] bool returning() const noexcept { return active_ && returning_; }
    // While returning, once a frame with the game camera: true when it is back with the player and has
    // stopped travelling, or has taken too long; the hold then ends (and the half eases into the game camera).
    bool back(const CameraMatrices& game, Vec3 player) noexcept {
        if (!returning()) return false;
        const Vec3 eye = pose_of(game.inverse).eye;
        const bool near_player = length(eye - player) <= kBackDistance;
        const bool settled = frames_ > 0u && length(eye - last_eye_) <= kSettled;
        last_eye_ = eye;
        if ((near_player && settled) || ++frames_ >= kReturnFrames) {
            active_ = false;
            return true;
        }
        if (frames_ == 0u) frames_ = 1u;
        return false;
    }
    void reset() noexcept { *this = CameraHold{}; }

private:
    CameraMatrices camera_{};
    std::array<float, 16> projection_{};
    Vec3 origin_{};
    Vec3 last_eye_{};
    bool returning_ = false;
    unsigned frames_ = 0u;
    bool active_ = false;
};

[[nodiscard]] inline Vec3 transform_point(const std::array<float, 12>& m, Vec3 p) noexcept {
    return {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
            m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
}

// The second player's own camera while the screen is split: it trails the player at `distance`, keeps
// `height` above them along their up vector (their gravity's opposite) and looks just above their head.
class FollowCamera {
public:
    // Starts where the first player's camera sits relative to the first player, moved to the second.
    void start(Vec3 first_eye, Vec3 first_player, Vec3 second_player, Vec3 up, const SplitTuning& tuning = {}) noexcept {
        up_ = normalized(up, {0.0f, 1.0f, 0.0f});
        const Vec3 first_look = first_player + up_ * tuning.look_height;
        eye_ = second_player + up_ * tuning.look_height + (first_eye - first_look);
        if (!std::isfinite(length(eye_))) eye_ = second_player + up_ * tuning.height + Vec3{0.0f, 0.0f, tuning.distance};
        started_ = true;
    }

    // The player's own camera controls, before update: `turn` orbits the camera around them (radians; +
    // to the camera's right), `raise` moves it up or down (units, kept within the tuning's range).
    void steer(float turn, float raise, const SplitTuning& tuning = {}) noexcept {
        if (std::isfinite(raise)) {
            height_offset_ = std::clamp(height_offset_ + raise, tuning.min_height - tuning.height,
                                        tuning.max_height - tuning.height);
        }
        if (!std::isfinite(turn) || turn == 0.0f || !started_) return;
        // Rodrigues' rotation of the eye about the up axis through the look point.
        const Vec3 offset = eye_ - look_;
        const float c = std::cos(turn);
        const float s = std::sin(turn);
        const Vec3 rotated = offset * c + cross(up_, offset) * s + up_ * (dot(up_, offset) * (1.0f - c));
        eye_ = look_ + rotated;
    }

    // Behind the player again, facing where they face (`front`), as the game's C button does.
    void recentre(Vec3 player, Vec3 front, const SplitTuning& tuning = {}) noexcept {
        Vec3 flat = front - up_ * dot(front, up_);
        flat = normalized(flat, {0.0f, 0.0f, -1.0f});
        look_ = player + up_ * tuning.look_height;
        eye_ = look_ - flat * tuning.distance + up_ * (tuning.height + height_offset_);
    }

    CameraMatrices update(Vec3 player, Vec3 up, const SplitTuning& tuning = {}) noexcept {
        const Vec3 target_up = normalized(up, up_);
        up_ = normalized(up_ + (target_up - up_) * tuning.up_follow, target_up);
        look_ = player + up_ * tuning.look_height;
        Vec3 behind = eye_ - look_;
        behind = behind - up_ * dot(behind, up_);
        const Vec3 fallback = normalized(cross(up_, std::fabs(up_.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f}),
                                         {0.0f, 0.0f, 1.0f});
        const Vec3 desired = look_ + normalized(behind, fallback) * tuning.distance + up_ * (tuning.height + height_offset_);
        eye_ = eye_ + (desired - eye_) * tuning.follow;
        if (!std::isfinite(length(eye_))) eye_ = desired;
        return look_at(eye_, up_, look_);
    }

    [[nodiscard]] bool started() const noexcept { return started_; }
    [[nodiscard]] Vec3 eye() const noexcept { return eye_; }
    [[nodiscard]] Vec3 look() const noexcept { return look_; }
    [[nodiscard]] Vec3 up() const noexcept { return up_; }
    void reset() noexcept { *this = FollowCamera{}; }

private:
    Vec3 eye_{};
    Vec3 look_{};
    Vec3 up_{0.0f, 1.0f, 0.0f};
    float height_offset_ = 0.0f;
    bool started_ = false;
};

// The second player's camera controls from their controller: the stick bound to the pointer turns and
// raises the camera; the D-pad does the same (left and right turn, up and down raise), and C recentres it.
struct CameraSteer {
    float turn = 0.0f;   // radians this frame
    float raise = 0.0f;  // units this frame
    bool recentre = false;
};
inline constexpr float kSteerTurnPerFrame = 0.045f;  // about 2.6 degrees, 155 a second at full tilt
inline constexpr float kSteerRaisePerFrame = 9.0f;
inline constexpr float kSteerDeadZone = 0.2f;
[[nodiscard]] inline CameraSteer camera_steer(const PartnerInput& input, std::uint32_t previous_buttons) noexcept {
    CameraSteer steer;
    if (!input.connected) return steer;
    const auto axis = [](float value) {
        if (!std::isfinite(value) || std::fabs(value) < kSteerDeadZone) return 0.0f;
        return std::clamp((std::fabs(value) - kSteerDeadZone) / (1.0f - kSteerDeadZone), 0.0f, 1.0f) *
               (value < 0.0f ? -1.0f : 1.0f);
    };
    float x = axis(input.camera_x);
    float y = axis(input.camera_y);
    if ((input.buttons & button::kLeft) != 0u) x -= 1.0f;
    if ((input.buttons & button::kRight) != 0u) x += 1.0f;
    if ((input.buttons & button::kUp) != 0u) y += 1.0f;
    if ((input.buttons & button::kDown) != 0u) y -= 1.0f;
    steer.turn = std::clamp(x, -1.0f, 1.0f) * kSteerTurnPerFrame;
    steer.raise = std::clamp(y, -1.0f, 1.0f) * kSteerRaisePerFrame;
    steer.recentre = (input.buttons & button::kC) != 0u && (previous_buttons & button::kC) == 0u;
    return steer;
}

// How far towards the edge of a shared picture the players would be: the camera turned as `camera` is,
// looking at their midpoint from `depth` away, with vertical field of view `fovy_degrees` and width:height
// `aspect`. 0 at the middle, 1 at the edge (the larger of across and up); infinity when it cannot frame them.
[[nodiscard]] inline float shared_view_extent(const CameraMatrices& camera, Vec3 first, Vec3 second, float depth,
                                              float fovy_degrees, float aspect) noexcept {
    const Vec3 half = (second - first) * 0.5f;
    // The camera's rotation (the view matrix without its translation).
    const float x = camera.view[0] * half.x + camera.view[1] * half.y + camera.view[2] * half.z;
    const float y = camera.view[4] * half.x + camera.view[5] * half.y + camera.view[6] * half.z;
    const float z = camera.view[8] * half.x + camera.view[9] * half.y + camera.view[10] * half.z;
    const float ty = std::tan(fovy_degrees * 0.5f * 3.14159265f / 180.0f);
    const float tx = ty * aspect;
    // The nearer player is |z| closer to the camera.
    const float near_depth = depth - std::fabs(z);
    if (!std::isfinite(near_depth) || near_depth <= 1.0f || !(tx > 0.0f) || !(ty > 0.0f)) {
        return std::numeric_limits<float>::infinity();
    }
    return std::max(std::fabs(x) / (near_depth * tx), std::fabs(y) / (near_depth * ty));
}

// Whether a sphere can be seen from a camera (view as look_at's) with vertical field of view `fovy_degrees`,
// width:height `aspect` and far plane `far_z`. Conservative: the sphere is tested against widened planes.
[[nodiscard]] inline bool sphere_in_view(const std::array<float, 12>& view, Vec3 centre, float radius, float fovy_degrees,
                                         float aspect, float far_z) noexcept {
    const Vec3 v = transform_point(view, centre);
    const float depth = -v.z;
    if (!std::isfinite(depth) || depth + radius < 0.0f || depth - radius > far_z) return false;
    const float ty = std::tan(fovy_degrees * 0.5f * 3.14159265f / 180.0f);
    const float tx = ty * aspect;
    if (!std::isfinite(tx) || !std::isfinite(ty)) return false;
    return std::fabs(v.x) <= depth * tx + radius * std::sqrt(1.0f + tx * tx) &&
           std::fabs(v.y) <= depth * ty + radius * std::sqrt(1.0f + ty * ty);
}

// ClippingJudge::isJudgedToClipFrustum's far plane for a clipping level: level 0 is the camera's far plane,
// levels 1-7 the judge's fixed distances (ClippingJudge's constructor). An object past it stays clipped for
// the second player as well; Space Junk's spider-web coins sit 70000 units out, never meant to move unless
// Tarantox has made the web.
[[nodiscard]] inline float clip_level_distance(std::uint32_t level, float far_z) noexcept {
    constexpr float kDistances[8] = {0.0f, 60000.0f, 50000.0f, 40000.0f, 30000.0f, 20000.0f, 10000.0f, 5000.0f};
    return level >= 1u && level < 8u ? kDistances[level] : far_z;
}

// Whether an allocation of `size` bytes aligned to `align` from the stage's full MEM1 heap (JKRSolidHeap,
// `mem1_free` bytes left) goes to its MEM2 heap (`mem2_free` left) instead: only when MEM1 cannot hold it and MEM2
// can, so it changes nothing but an allocation that would have stopped the game (HeapMemoryWatcher's error
// handler panics). The second player and their split screen take MEM1 the Wii's 24 MB did not leave room for.
[[nodiscard]] inline bool scene_heap_overflows(std::uint32_t mem1_free, std::uint32_t mem2_free, std::uint32_t size,
                                               std::uint32_t align) noexcept {
    const std::uint64_t need = static_cast<std::uint64_t>(size) + (align > 4u ? align : 4u);
    return mem1_free < need && mem2_free >= need;
}

// Camera collision: where the camera goes when the map is in the way between the point it looks at and the
// eye (`hit`, the first wall on that line): `margin` short of the wall, towards the player.
[[nodiscard]] inline Vec3 camera_before_wall(Vec3 look, Vec3 hit, float margin) noexcept {
    const Vec3 back = look - hit;
    const float distance = length(back);
    if (!std::isfinite(distance) || distance <= margin) return look + (hit - look) * 0.1f;
    return hit + back * (margin / distance);
}

struct CameraShare {
    Vec3 offset{};          // add to the first player's camera point
    bool out_of_range = false;
};

[[nodiscard]] inline CameraShare shared_camera(Vec3 first, Vec3 second, const CameraTuning& tuning = {}) noexcept {
    const Vec3 apart = second - first;
    const float distance = length(apart);
    if (!std::isfinite(distance)) return {};
    if (distance >= tuning.leash) return {{}, true};
    float weight = 1.0f;
    if (distance > tuning.share) {
        const float t = (distance - tuning.share) / (tuning.leash - tuning.share);
        weight = 1.0f - t * t * (3.0f - 2.0f * t);  // smoothstep down to the first player
    }
    return {apart * (0.5f * weight), false};
}

// The players bump into each other: each is a cylinder `radius` wide and `height` tall along the up vector
// (gravity's opposite). When they overlap, each is pushed half the overlap apart, across the up vector, at
// most `max_push` a frame, so walking into the other shoves them along rather than passing through.
struct BodyTuning {
    float radius = 50.0f;
    float height = 150.0f;
    float max_push = 12.0f;
};

// The push for the second player (the first gets its opposite), or zero when they do not touch. `up` is a
// unit vector.
[[nodiscard]] inline Vec3 body_push(Vec3 first, Vec3 second, Vec3 up, const BodyTuning& tuning = {}) noexcept {
    const Vec3 apart = second - first;
    const float along_up = dot(apart, up);
    if (!std::isfinite(along_up) || std::fabs(along_up) >= tuning.height) return {};
    Vec3 across = apart - up * along_up;
    float distance = length(across);
    const float touching = tuning.radius * 2.0f;
    if (!std::isfinite(distance) || distance >= touching) return {};
    if (distance < 0.01f) {
        // On top of each other: any direction across the up vector.
        across = std::fabs(up.x) < 0.9f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
        across = across - up * dot(across, up);
        distance = length(across);
    }
    const float amount = std::min((touching - length(apart - up * along_up)) * 0.5f, tuning.max_push);
    return across * (amount / distance);
}

// The player the game camera follows. Camera events and "target the player" calls made while the second
// player moves catch the second player's MarioActor; the camera is the first player's, so it goes back to
// them. Any other target (an object, an event's own actor) is left alone.
[[nodiscard]] constexpr std::uint32_t camera_player(std::uint32_t current, std::uint32_t first,
                                                    std::uint32_t second) noexcept {
    return second != 0u && current == second ? first : current;
}

// A camera event the second player set off, while an event camera plays: their own move pointed the camera
// at them, or an object started it for them (a launch star, a warp pad: the camera then points at the first
// player, but only the second is in a special state, flying or warping). Their half shows it (split screen)
// and the camera follows them until it ends; after it, camera_player puts it back on the first player.
[[nodiscard]] constexpr bool camera_event_is_second(std::uint32_t current, std::uint32_t second,
                                                    bool event_camera_active, bool second_special,
                                                    bool first_special) noexcept {
    return event_camera_active && second != 0u && (current == second || (second_special && !first_special));
}

// Whether the game camera is about the second player rather than the first: its eye nearer the second, and
// well away from the first (a warp or launch star camera the game sets up at the second player's warp while
// still aiming at the first player, its player).
inline constexpr float kOtherPlayerCameraDistance = 2000.0f;
[[nodiscard]] inline bool camera_is_second_players(Vec3 eye, Vec3 first, Vec3 second) noexcept {
    const float to_first = length(eye - first);
    const float to_second = length(eye - second);
    return std::isfinite(to_first) && std::isfinite(to_second) && to_second < to_first &&
           to_first > kOtherPlayerCameraDistance;
}

// Doors and domes (the Observatory's AstroChangeStageCube areas) test the game's player, the first: the
// second player at one the first is not at swaps places with them, so the game takes them through.
[[nodiscard]] constexpr bool swap_for_entrance(bool second_in_entrance, bool first_in_entrance) noexcept {
    return second_in_entrance && !first_in_entrance;
}

// The second player waits (does not move) while the game has the first player's controls (a Pull Star, a
// launch into a galaxy) or a cutscene plays: those sequences change the map around the first player.
[[nodiscard]] constexpr bool second_player_waits(bool first_player_controls_held, bool cutscene) noexcept {
    return first_player_controls_held || cutscene;
}

// Travelling with the first player (second_player_waits, a parked first player, a level start) ends only
// after the first player has been in ordinary play `kTravelReleaseFrames` in a row, so a flicker of their
// controls does not pull the second player back and forth. A level start waits `kLevelStartSettleFrames`
// (the first player flies in, then lands and gets control).
inline constexpr unsigned kTravelReleaseFrames = 10u;
inline constexpr unsigned kLevelStartSettleFrames = 20u;
[[nodiscard]] constexpr bool keep_travelling(bool travelling, bool first_busy, unsigned controllable_frames) noexcept {
    return first_busy || (travelling && controllable_frames < kTravelReleaseFrames);
}
// Where the second player goes while they travel with the first (second_player_waits): `kBesideDistance`
// to the first player's side (`side`, Mario::mSideVec).
inline constexpr float kBesideDistance = 150.0f;
[[nodiscard]] inline Vec3 beside(Vec3 first, Vec3 side) noexcept {
    return first + normalized(side, {1.0f, 0.0f, 0.0f}) * kBesideDistance;
}

// Where the second player goes while they travel with the first: beside them, or, while the game puppets the
// first player and leaves their actor at the origin (a dome's galaxy selector), where they were (`parked`).
[[nodiscard]] inline Vec3 travel_position(Vec3 first, Vec3 side, Vec3 parked) noexcept {
    const float from_origin = length(first);
    if (!std::isfinite(from_origin) || from_origin < 1.0f) return parked;
    return beside(first, side);
}

// The scene's NameObjHolder holds every NameObj of a scene in a fixed-size array the game fills without a
// bounds check (0x1300 entries); the second player's MarioActor, its models and parts add their own, which in
// big galaxies (Space Junk) ran past the end and corrupted what followed. With two players it is made larger.
inline constexpr std::uint32_t kSceneNameObjHolderMin = 0x1000u;  // the system's holder is 0x100
inline constexpr std::uint32_t kNameObjHolderExtra = 0x800u;
[[nodiscard]] constexpr std::uint32_t scene_name_obj_capacity(std::uint32_t game_capacity) noexcept {
    return game_capacity >= kSceneNameObjHolderMin ? game_capacity + kNameObjHolderExtra : game_capacity;
}

// The MarioActor::mModels slot of a power-up form's model (MarioActorDraw.cpp init*Mario), kTeresaParts
// for the Boo form (its own parts), -1 for forms drawn with the normal model (fire, flying).
inline constexpr int kTeresaParts = -2;
[[nodiscard]] constexpr int power_up_model_slot(std::uint16_t mode) noexcept {
    switch (mode) {
    case 1: return 4;  // rainbow (invincible)
    case 3: return 3;  // ice
    case 4: return 2;  // bee
    case 5: return 5;  // spring (hopper)
    case 6: return kTeresaParts;  // boo
    case 9: return 1;  // bone
    default: return -1;
    }
}

// Counts frames out of range; true on the frame the second player should be brought back.
class LeashTimer {
public:
    [[nodiscard]] bool update(bool out_of_range, unsigned warp_frames) noexcept {
        frames_ = out_of_range ? frames_ + 1u : 0u;
        if (frames_ < warp_frames) return false;
        frames_ = 0u;
        return true;
    }
    void reset() noexcept { frames_ = 0u; }

private:
    unsigned frames_ = 0u;
};

}  // namespace galaxy::coop
