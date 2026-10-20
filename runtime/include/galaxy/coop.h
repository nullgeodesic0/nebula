#pragma once

// Mario and Luigi co-op: plain logic for the second player (coop_guest.cpp drives the game with it).
// Nothing here touches the guest; galaxy_coop_tests covers it.

#include "galaxy/input_bindings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
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
// (-1..1, up positive) and a shake (the spin).
struct PartnerInput {
    bool connected = false;
    std::uint32_t buttons = 0u;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
    bool shake = false;
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
struct CameraTuning {
    float share = 1200.0f;
    float leash = 2600.0f;
    unsigned warp_frames = 120u;
};

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

[[nodiscard]] inline float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

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
