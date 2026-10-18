#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace galaxy::moveset {

// Super Mario Odyssey's moves for Mario (the ones that need no Cappy), as a layer over Galaxy's own
// movement.
//
// The game keeps running its own state machine (Mario::actionMain). Once per frame, before it runs,
// this module reads what Mario is doing and what the player pressed, and changes the few values the
// game then uses: his ground speed, his jump velocity, his jump type and an animation. A move is the
// game's own crouch slide, long jump, jump or dive-into-water landing with Odyssey's numbers. What
// Galaxy already does the way Odyssey does (ground pound, long jump, backflip, side flip, triple jump,
// wall jump and wall slide, running, skids) is left to Galaxy.
//
// Odyssey's cap button (Y) is the Wii Remote's 1 button here, which Galaxy leaves unused in play (bound
// to the left stick click and the C key; the camera reset, Nunchuk C, moves to the right stick click
// and the V key). Z is Odyssey's crouch and ground pound (ZL). Speeds are Odyssey's, in units per frame:
// Odyssey's and Galaxy's units are both about a centimetre (Galaxy's full run is 13, Odyssey's 14).
// Odyssey's values are from the Super Mario Odyssey Wiki (smo.wiki/Roll, /Dive, /Ground_pound).
//
//   Roll              Crouch (hold Z) and press C. At least 20 u/fr; it keeps going while Z is held,
//                     losing 0.2% a frame, up to 35. Below 17 he is back in a crouch; letting go of Z
//                     gets him up.
//   Roll boost        C again during a roll, at most every 16 frames (32 after a ground-pound roll):
//                     23 u/fr after a crouch roll, 26 after a ground-pound, dive or long-jump roll or an
//                     earlier boost.
//   Ground-pound roll Z held and C as a ground pound lands: a roll at 30 u/fr.
//   Ground-pound jump A as a ground pound lands: the second highest jump, after the triple jump.
//   Dive              C during a ground pound (Z then C in the air), or C in any jump: forward at a
//                     fixed speed, rising 182 units. Landing with Z held rolls on; otherwise a belly slide.
//                     The dive hits enemies as the spin does (stunning what a spin stuns).
//   Long-jump roll    Landing from a long jump with Z held rolls on.
//   Run after a turn  A skid into a run the other way runs at 17 for a second (Odyssey's RunAfterTurn),
//                     with the fast dash animation; its first jump has the dash jump's.
//   Roll cancel       The spin during a roll (Odyssey's cap throw): out of the roll at its speed.
//   Roll jump         A during a roll: a jump that keeps the roll's speed. Z then A quickly is Galaxy's
//                     long jump, as in Odyssey.
//
// Everything is pure arithmetic on a snapshot (MarioView) and returns a MarioEdit, so the rules are
// testable without a game. odyssey_moveset.cpp reads and writes the guest.

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

[[nodiscard]] inline Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] inline Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] inline Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] inline float dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] inline float length(Vec3 a) noexcept { return std::sqrt(dot(a, a)); }
[[nodiscard]] inline Vec3 normalized_or_zero(Vec3 a) noexcept {
    const float n = length(a);
    return n > 1e-6f ? a * (1.0f / n) : Vec3{};
}
[[nodiscard]] inline bool finite(Vec3 a) noexcept {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}

struct Options {
    bool roll = true;
    bool roll_jump = true;
    bool ground_pound_jump = true;
    bool dive = true;
    bool turn_boost = true;  // the faster run out of a turn-around
};

// The game's own numbers, read from Mario's constant table (MarioConstTable).
struct Constants {
    float jump_height_high = 36.0f;      // mJumpHeight[2], the triple jump
    float jump_height_small = 22.0f;     // mJumpHeight[0]
    float squat_jump_front = 16.5f;      // mSquatJumpFrontSpeed, the long jump
    float walk_speed = 13.0f;            // mWalkSpeed: ground speed in units per frame at a full run
    float gravity = 1.8f;                // mGravityJumping[0]
};

// Roll* are the roll's moments: starting (then rolling), rolling, a boost, the roll jump, landing from
// it, and getting up when it ends on the ground.
enum class Anim : std::uint8_t {
    None, HighJump, Dive, Roll, RollStart, RollBoost, RollJump, RollLand, RollEnd, GroundPoundRoll, DashJump, DiveLand, RollBump
};

struct MarioView {
    bool eligible = false;      // normal (or star) Mario under the player's control, in no special state
    bool grounded = false;      // MovementStates._1
    bool jumping = false;       // MovementStates.jumping (also true through a ground pound)
    bool hip_drop = false;      // MovementStates._B, the ground pound
    bool squat = false;         // MovementStates._A
    bool wall_slide = false;    // MovementStates._17
    bool wall_hit = false;      // MovementStates._8: he is against a wall
    bool trig_z = false;        // pressed this frame
    bool lvl_z = false;         // held
    bool trig_c = false;        // the cap button (Wii 1) pressed this frame
    bool spin_request = false;  // the game would spin this frame (MarioActor::isRequestSpin/Rush): in the
                                // air, and during a roll
    bool roll_anim_running = true;  // the roll's animation is playing
    bool braking = false;       // Galaxy's turn-around skid (Mario::_3D0, MovementStates._4): it zeroes
                                // his speed every frame, so forcing a speed there loops its sound
    std::uint32_t jump_type = 0;  // Mario::_430
    float walk_speed = 0.0f;      // Mario::mWalkSpeed, 1.0 a full run
    float stick_mag = 0.0f;       // Mario::mStickPos.z
    Vec3 jump_vec{};              // Mario::mJumpVec, the air velocity of the frame
    Vec3 front{};                 // Mario::mFrontVec
    Vec3 stick_dir{};             // Mario::mWorldPadDir: where the stick points, in the world
    Vec3 down{};                  // Mario::mAirGravityVec, unit, towards the ground
    Constants table{};
};

struct MarioEdit {
    bool set_walk_speed = false;
    float walk_speed = 0.0f;
    bool set_jump_vec = false;
    Vec3 jump_vec{};
    bool set_jump_type = false;
    std::uint32_t jump_type = 0;
    bool end_jump_hold = false;   // MovementStates._13 off: the jump no longer rises while A is held
    bool hard_landing = false;    // Mario::_10._8: land as a dive does (belly slide)
    bool knock_back = false;      // Mario::blown(knock_back_dir): bounced off a wall as Galaxy's long jump is
    Vec3 knock_back_dir{};
    bool set_front = false;       // turn him to face `front` (a roll's limited turning)
    Vec3 front{};
    bool face_stick = false;      // turn him to face the stick (Mario::setFrontVecKeepUp(mWorldPadDir))
    Anim anim = Anim::None;
    bool stop_roll_anim = false;  // the roll is over: stop its looping animation
    bool cancel_squat = false;    // Mario::cancelSquatMode: leave the crouch
    bool block_crouch = false;    // MovementStates._21 on, _A off: the latch Galaxy sets for a jump so a
                                  // held Z does not crouch (it clears itself when Z is let go). Without
                                  // it a held Z keeps the crouch-walk, its voice and footsteps, going
                                  // under the roll
    bool end_hip_drop = false;    // leave the ground pound's landing (MovementStates.jumping and _B off)
    bool end_hip_drop_air = false;  // leave the ground pound in the air (_B off, its stall timer Mario::_424
                                    // cleared; still jumping): a dive
    bool block_air_actions = false;  // MarioActor::_935 for this frame: no ground pound and no spin (the
                                     // game clears it at the end of every frame)
    bool clear_hard_landing = false;  // Mario::_10._8 off: a spin cancelled the dive, land on the feet
    bool stop_belly_slide = false;  // the dive's belly slide is over: stop its loop, back to the ground blend
    std::uint8_t attack_frames = 0;  // MarioActor::setPunchHitTimer: the spin's hit on enemies for this
                                     // many frames (the dive tackles what a spin stuns)
    bool air_gust = false;        // a gust of air blowing out behind him (a roll boost)
    bool dust_puff = false;       // a puff of dust at his feet (the roll, every few frames, a boost)
    bool dust_landing = false;    // a landing's cloud of dust (a dive or a roll jump landing)
    // The running animation for his speed above the run (RunAnim), when it changes or must be restarted.
    std::uint32_t sounds = 0;     // Sound bits
    bool set_run_anim = false;
    std::uint8_t run_anim = 0;
};

// Sounds the moves play (Mario's own sound table, Mario::playSoundJ names), as bits of MarioEdit::sounds.
enum Sound : std::uint32_t {
    kSoundStepLeft = 1u << 0,   // 足音左: a footstep (the dash loops have no sound track of their own)
    kSoundStepRight = 1u << 1,  // 足音右
    kSoundRolling = 1u << 2,    // 坂滑り: the slide along the ground, every frame of a roll on the ground
    kSoundRollStart = 1u << 3,  // スペシャルダッシュ弱: a roll starting
    kSoundRollBoost = 1u << 4,  // スペシャルダッシュ強: a boost
    kSoundBump = 1u << 5,       // 壁衝突: rolling into a wall
};

// Running animations by speed: Galaxy's walk and run blend (Odyssey's Walk and Run with the custom
// animations) up to the run, then Odyssey's Dash, then DashFast.
enum class RunAnim : std::uint8_t { Blend = 0, Dash = 1, DashFast = 2 };
inline constexpr float kDashAnimSpeed = 15.0f;         // units per frame
inline constexpr float kDashFastAnimSpeed = 18.0f;
inline constexpr float kRunAnimHysteresis = 0.5f;
// Above the run's cap Odyssey slows by 7/60 units per frame each frame (smo.wiki/Running).
inline constexpr float kOverCapDecel = 7.0f / 60.0f;
inline constexpr float kRunCap = 14.0f;

// The roll (Odyssey's values, units per frame).
inline constexpr float kRollCrouchSpeed = 20.0f;       // a crouch roll starts at least this fast
inline constexpr float kRollPoundSpeed = 30.0f;        // a ground-pound roll starts at this
inline constexpr float kRollMinSpeed = 17.0f;          // below this he is back in a crouch
inline constexpr float kRollMaxSpeed = 35.0f;          // the ground cap
inline constexpr float kRollDecay = 0.997f;            // per frame on flat ground (Odyssey's 0.998, a little faster)
inline constexpr float kRollBoostWhite = 23.0f;        // a boost after a crouch roll
inline constexpr float kRollBoostYellow = 26.0f;       // after a ground-pound, dive or long-jump roll, or a boost
inline constexpr int kRollBoostWait = 16;              // frames from a roll's start or a boost to the next boost
inline constexpr int kRollBoostWaitPound = 32;         // after a ground-pound roll
inline constexpr float kBumpBounce = 0.2f;            // of the roll's speed, back off the wall
inline constexpr float kRollTurnDegrees = 5.0f;       // a roll turns at most this much a frame (Odyssey)
inline constexpr int kRollCancelWait = 9;             // frames after a start or boost before a roll cancel
inline constexpr int kRollStopGrace = 6;              // frames a new roll ignores a stopped speed
inline constexpr int kRollAirFrames = 8;               // frames off the ground (no jump) a roll rides out
inline constexpr int kRollDustEvery = 8;               // frames between the roll's dust puffs
inline constexpr float kRollStopWalkSpeed = 0.6f;      // ran into something (Galaxy's speed fell under this)
inline constexpr int kRollJumpGrace = 3;               // ticks after a roll in which a jump is a roll jump
inline constexpr float kRollJumpHeightScale = 1.1f;    // of the first jump's height
inline constexpr int kRollJumpMinFrames = 12;          // a jump sooner than this into a roll is a long jump
// Ground-pound jump, dive.
inline constexpr float kGroundPoundJumpScale = 1.14f;  // of the triple jump's height: under it in Odyssey
                                                       // terms once Galaxy's held-A float is counted
inline constexpr float kDiveSpeed = 20.0f;             // fixed forward speed
inline constexpr float kDiveRise = 182.0f;             // units above where the dive starts
inline constexpr float kDiveStick = 0.3f;             // the stick pushed this far steers the dive
inline constexpr std::uint8_t kDiveAttackFrames = 4;  // the hit is renewed each frame of the dive
inline constexpr int kDiveSpinGuard = 12;             // frames a new dive ignores the spin input
inline constexpr float kPoundDiveRise = 40.0f;         // out of a ground pound's stall: a lift, not a jump
inline constexpr int kBellySlideFrames = 30;          // the belly slide after a dive (Galaxy slides 25)
// How long a roll goes on before letting go of Z stands him up (Odyssey's standing-up delay).
inline constexpr int kRollHoldCrouch = 47;            // a crouch roll
inline constexpr int kRollHoldPound = 33;             // a ground-pound, dive or long-jump roll
inline constexpr int kRollHoldBoost = 46;             // after a boost
// The run after a turn-around (Odyssey's RunAfterTurn): braking and running the other way raises the run's
// cap from 14 to 17 for a moment; above the cap Odyssey slows by 7/60 a frame (Galaxy's inertia does that
// part here). The Wiki gives no length for "a moment"; a second is used.
inline constexpr float kTurnBoostSpeed = 17.0f;        // units per frame
inline constexpr float kTurnBoostAccel = 0.5f;         // units per frame, per frame, up to the cap
inline constexpr int kTurnBoostFrames = 60;
inline constexpr float kTurnBoostStick = 0.5f;         // the stick held at least this far starts it
inline constexpr float kTurnBoostKeepStick = 0.7f;     // and at least this far keeps it
inline constexpr int kFastRunPause = 60;              // frames a carried roll speed waits for a spin to end
inline constexpr int kFastRunGrace = 8;               // frames a new fast run ignores a stop or a skid flag
inline constexpr int kTurnBoostAirFrames = 120;        // a jump keeps it for the landing
inline constexpr std::uint32_t kNormalJumpType = 0u;
inline constexpr std::uint32_t kSquatJumpType = 5u;
inline constexpr std::uint32_t kHighJumpType = 2u;
inline constexpr std::uint32_t kSpinJumpType = 8u;

class Moveset {
public:
    void set_options(const Options& options) noexcept { options_ = options; }
    [[nodiscard]] const Options& options() const noexcept { return options_; }

    void reset() noexcept {
        const Options options = options_;
        *this = Moveset{};
        options_ = options;
    }

    [[nodiscard]] bool rolling() const noexcept { return roll_frame_ >= 0; }
    [[nodiscard]] bool diving() const noexcept { return dive_; }
    // The roll's speed in units per frame.
    [[nodiscard]] float roll_speed() const noexcept { return roll_speed_; }
    // Why the last roll ended (for traces).
    [[nodiscard]] const char* roll_end_reason() const noexcept { return end_reason_; }
    [[nodiscard]] bool turn_boosting() const noexcept { return dash_; }
    [[nodiscard]] float fast_run_speed() const noexcept { return dash_ ? dash_speed_ : 0.0f; }

    // Call once per frame, before Mario::actionMain. The edit says what to change in the guest.
    [[nodiscard]] MarioEdit step(const MarioView& v) noexcept {
        MarioEdit edit;
        if (!v.eligible || !finite(v.jump_vec) || !finite(v.front) || !finite(v.down) ||
            length(v.down) < 0.5f || v.table.walk_speed <= 0.0f) {
            edit.stop_roll_anim = rolling();
            if (rolling()) end_reason_ = "lost control";
            // The spin out of a roll cancel is a state of its own (not under the moveset's control); the
            // speed carried out of the roll waits for it to end.
            if (dash_ && !(carry_ && ++paused_ <= kFastRunPause)) end_dash(edit);
            if (dash_) fast_age_ = 0;
            reset_tracking();
            remember(v, {});
            return edit;
        }
        const Vec3 up = v.down * -1.0f;
        const bool jump_started = v.jumping && !prev_jumping_;
        const Vec3 air_sideways = v.jump_vec - up * dot(v.jump_vec, up);

        // --- Ground-pound jump: the ground pound's landing ended into a jump (Galaxy calls tryJump when
        // A is pressed as it lands), so the jump it made is the one to raise. Only from the landing: a
        // dive out of the pound in the air also ends it, and must not be raised.
        if (options_.ground_pound_jump && prev_hip_drop_ && prev_grounded_ && !v.hip_drop && v.jumping &&
            !v.grounded &&
            dot(v.jump_vec, up) > 0.0f) {
            edit.set_jump_vec = true;
            edit.jump_vec = air_sideways + up * (v.table.jump_height_high * kGroundPoundJumpScale);
            edit.set_jump_type = true;
            edit.jump_type = kHighJumpType;
            edit.anim = Anim::HighJump;
            roll_recent_ = 0;
        }

        // --- Roll jump: A during a roll: a jump a little higher than the first jump that keeps the
        // roll's speed. Z then A quickly (within kRollJumpMinFrames, while Galaxy still crouches) stays
        // Galaxy's long jump.
        if (options_.roll_jump && jump_started && !v.hip_drop && roll_recent_ > 0 &&
            last_roll_frames_ >= kRollJumpMinFrames) {
            const Vec3 flat = v.front - up * dot(v.front, up);
            Vec3 heading = normalized_or_zero(air_sideways);
            if (length(heading) < 0.5f) heading = normalized_or_zero(flat);
            if (length(heading) > 0.5f) {
                edit.set_jump_vec = true;
                edit.jump_vec = heading * last_roll_speed_ + up * (v.table.jump_height_small * kRollJumpHeightScale);
                edit.set_jump_type = true;
                edit.jump_type = kNormalJumpType;
                edit.cancel_squat = true;  // the long jump keeps the crouch; an ordinary jump must not
                edit.anim = Anim::RollJump;
                roll_jump_air_ = true;
            }
            roll_recent_ = 0;
        }

        // --- Dive: C during a ground pound in the air, or in any jump, a spin jump too: a spin cancels a
        // dive, and a dive can follow the spin.
        // The ground resets the air spin and the dive; a spin jump spends the air spin.
        if (v.grounded && !v.jumping) {
            air_spin_used_ = false;
            dive_spent_ = false;
        }
        if (jump_started && v.jump_type == kSpinJumpType) air_spin_used_ = true;
        if (options_.dive && v.trig_c && v.jumping && !v.grounded && !v.wall_slide && !dive_ && !dive_spent_) {
            // Towards the stick when it is pushed, else straight ahead.
            const Vec3 to_stick = normalized_or_zero(v.stick_dir - up * dot(v.stick_dir, up));
            const bool steer = v.stick_mag >= kDiveStick && length(to_stick) > 0.5f;
            const Vec3 heading = steer ? to_stick : normalized_or_zero(v.front - up * dot(v.front, up));
            if (length(heading) > 0.5f) {
                edit.face_stick = steer;
                // Rising kDiveRise under the jump's gravity (A held or not: the hold is ended).
                const float height = v.hip_drop ? kPoundDiveRise : kDiveRise;
                const float rise = std::sqrt(2.0f * std::max(v.table.gravity, 0.1f) * height);
                edit.set_jump_vec = true;
                edit.jump_vec = heading * kDiveSpeed + up * rise;
                edit.end_jump_hold = true;
                edit.hard_landing = true;
                edit.end_hip_drop_air = v.hip_drop;
                edit.set_jump_type = true;  // the first jump's gravity, whatever jump he dived out of
                edit.jump_type = kNormalJumpType;
                edit.anim = Anim::Dive;
                dive_ = true;
                dive_age_ = 0;
                dive_spent_ = true;
                long_jump_air_ = false;
            }
        }

        // A dive lasts until it lands; only the spin cancels it (no ground pound). Only a fresh air spin
        // (the first since the ground: the one that lifts him) earns another dive; a false spin (the
        // animation alone) does not.
        // A spin input still held over from the spin just before the dive (Galaxy's shake and spin
        // inputs last some frames) must not cancel it straight away: the dive holds for a moment first.
        if (dive_) ++dive_age_;
        const bool spin_counts = v.spin_request && !(dive_ && dive_age_ <= kDiveSpinGuard);
        if (spin_counts && v.jumping && !v.grounded && edit.anim != Anim::Dive) {
            if (dive_) {
                dive_ = false;
                edit.clear_hard_landing = true;
            }
            if (!air_spin_used_) dive_spent_ = false;
            air_spin_used_ = true;
        } else if (dive_ && v.jumping && !v.grounded) {  // the dive's own frame too: a spin input
            // still held from before it would otherwise start a spin on top of it
            edit.block_air_actions = true;
        }

        // The dive tackles: the spin's hit on enemies, renewed every frame of it.
        if (dive_ && v.jumping && !v.grounded) edit.attack_frames = kDiveAttackFrames;

        // Air moves that can land into a roll.
        if (jump_started && v.jump_type == kSquatJumpType && !edit.set_jump_vec) long_jump_air_ = true;
        if (v.hip_drop) long_jump_air_ = false;
        // The first frame on the ground (the game may still count him as jumping on it).
        const bool landed = v.grounded && !prev_grounded_;
        bool land_roll = false;
        float land_speed = 0.0f;
        if (landed && (dive_ || long_jump_air_)) {
            land_speed = length(prev_air_sideways_);
            land_roll = options_.roll && v.lvl_z;
        }
        if (landed || v.grounded) {
            if (landed && dive_) {
                edit.dust_landing = true;
                if (!land_roll) {
                    edit.anim = Anim::DiveLand;  // the belly slide
                    belly_frames_ = kBellySlideFrames;
                    belly_speed_ = land_speed;
                }
            }
            dive_ = false;
            long_jump_air_ = false;
        }

        // --- The belly slide ends after its time, or as soon as he jumps. The cap button rolls out of it.
        bool belly_roll = false;
        if (belly_frames_ > 0 && edit.anim != Anim::DiveLand) {
            belly_roll = options_.roll && v.trig_c && v.grounded && !v.jumping;
            if (--belly_frames_ == 0 || v.jumping || belly_roll) {
                belly_frames_ = 0;
                edit.stop_belly_slide = true;
            }
        }

        // --- Landing from a roll jump.
        if (roll_jump_air_ && (v.hip_drop || dive_)) roll_jump_air_ = false;
        if (roll_jump_air_ && v.grounded && !v.jumping) {
            roll_jump_air_ = false;
            edit.dust_landing = true;
            if (edit.anim == Anim::None) edit.anim = Anim::RollLand;
        }

        // --- Ground-pound roll: Z held and C as a ground pound lands rolls straight out of it.
        bool started = false;
        if (options_.roll && roll_frame_ < 0 && v.hip_drop && v.grounded && v.trig_c && v.lvl_z) {
            start_roll(kRollPoundSpeed, kRollBoostWaitPound, true, v.table.walk_speed, edit);
            edit.end_hip_drop = true;
            edit.anim = Anim::GroundPoundRoll;
            started = true;
        } else if (belly_roll && roll_frame_ < 0) {
            // The cap button during the dive's belly slide rolls out of it.
            start_roll(std::clamp(belly_speed_, kRollCrouchSpeed, kRollMaxSpeed), kRollBoostWait, true,
                       v.table.walk_speed, edit);
            edit.anim = Anim::RollStart;
            started = true;
        } else if (land_roll && roll_frame_ < 0) {
            // A dive or a long jump landing with Z held rolls on at his speed.
            start_roll(std::clamp(land_speed, kRollCrouchSpeed, kRollMaxSpeed), kRollBoostWait, true,
                       v.table.walk_speed, edit);
            edit.anim = Anim::RollStart;
            started = true;
        } else if (options_.roll && roll_frame_ < 0 && v.grounded && !v.jumping && !v.braking && v.squat &&
                   v.trig_c) {
            // A crouch roll: from Galaxy's crouch (or crouch slide), at least kRollCrouchSpeed.
            const float speed = v.walk_speed * v.table.walk_speed;
            start_roll(std::clamp(speed, kRollCrouchSpeed, kRollMaxSpeed), kRollBoostWait, false,
                       v.table.walk_speed, edit);
            edit.anim = Anim::RollStart;
            started = true;
        }

        // --- Roll.
        if (!started && roll_frame_ >= 0) {
            // A bump or a slope can lift him off the ground for a few frames without a jump; the roll
            // carries on through that.
            air_frames_ = v.grounded ? 0 : air_frames_ + 1;
            const char* reason = nullptr;
            if (jump_started) reason = "jump";
            // The roll cancel: Odyssey's cap throw out of a roll, here the spin, no sooner than 9 frames
            // after the start or a boost. He keeps the roll's speed and spins out of it.
            else if (v.spin_request && v.grounded && boost_wait_ >= kRollCancelWait) reason = "roll cancel";
            else if (!v.lvl_z && boost_wait_ >= hold_frames_) reason = "z released";
            else if (v.braking) reason = "skid";
            else if (air_frames_ > kRollAirFrames) reason = "left the ground";
            else if (v.grounded && v.wall_hit) reason = "bumped";
            // Galaxy's own landing (a dive's belly landing, a long jump's) zeroes his speed on the frame
            // the roll starts; only a roll that has been going a few frames can be stopped that way.
            else if (v.grounded && v.walk_speed < kRollStopWalkSpeed && roll_age_ > kRollStopGrace) reason = "stopped";
            else if (roll_speed_ < kRollMinSpeed) reason = "slowed to a crouch";
            if (reason != nullptr) {
                end_reason_ = reason;
                roll_frame_ = -1;
                edit.stop_roll_anim = true;
                // Standing up out of a roll keeps its speed: he runs on, slowing as Odyssey does above the
                // run's cap.
                const bool cancel = std::strcmp(reason, "roll cancel") == 0;
                if (!v.jumping && (!v.lvl_z || cancel) && v.grounded && !v.braking &&
                    v.stick_mag >= kTurnBoostKeepStick && roll_speed_ > kRunCap) {
                    carry_ = true;
                    dash_ = true;
                    dash_air_ = 0;
                    fast_age_ = 0;
                    paused_ = 0;
                    boost_frames_ = 0;
                    dash_speed_ = roll_speed_;
                }
                // Getting up out of the roll; slowing down leaves him in Galaxy's crouch instead.
                if (v.grounded && !v.jumping && !v.lvl_z && edit.anim == Anim::None) edit.anim = Anim::RollEnd;
                // Rolling into a wall bumps him off it and stops him.
                // Rolling into a wall bounces him off it the way Galaxy's long jump bounces off one
                // (Mario::checkWallJumpHit: blown with a fifth of the speed, its own animation and landing).
                if (std::strcmp(reason, "bumped") == 0) {
                    edit.anim = Anim::None;
                    edit.sounds |= kSoundBump;
                    edit.dust_puff = true;
                    edit.knock_back = true;
                    edit.knock_back_dir = normalized_or_zero(v.front - up * dot(v.front, up)) *
                                          (-roll_speed_ * kBumpBounce);
                }
                if (!v.jumping) roll_recent_ = 0;
            } else {
                ++boost_wait_;
                ++roll_age_;
                if (v.trig_c && boost_wait_ >= boost_due_) {
                    // A boost: a speed tier and a fresh wait.
                    roll_speed_ = std::max(roll_speed_, boosted_ ? kRollBoostYellow : kRollBoostWhite);
                    boosted_ = true;
                    boost_wait_ = 0;
                    boost_due_ = kRollBoostWait;
                    hold_frames_ = kRollHoldBoost;
                    // The roll's loop goes on (no new start); a gust of air blows out behind him.
                    edit.sounds |= kSoundRollBoost;
                    edit.dust_puff = true;
                    edit.air_gust = true;
                } else {
                    roll_speed_ *= kRollDecay;
                }
                roll_speed_ = std::min(roll_speed_, kRollMaxSpeed);
                last_roll_speed_ = roll_speed_;
                edit.set_walk_speed = true;
                edit.walk_speed = roll_speed_ / v.table.walk_speed;
                edit.block_crouch = roll_frame_ >= kRollJumpMinFrames;
                if (!v.roll_anim_running && edit.anim == Anim::None) edit.anim = Anim::Roll;  // loop it
                if (v.grounded && roll_frame_ % kRollDustEvery == 0) edit.dust_puff = true;
                if (v.grounded) edit.sounds |= kSoundRolling;
                // A roll turns wide: Galaxy turns him as fast as a run, so his facing is held to
                // kRollTurnDegrees a frame from the roll's heading.
                const Vec3 facing = normalized_or_zero(v.front - up * dot(v.front, up));
                Vec3 heading = normalized_or_zero(roll_heading_ - up * dot(roll_heading_, up));
                if (length(facing) > 0.5f && length(heading) > 0.5f) {
                    const float cos_limit = std::cos(kRollTurnDegrees * 3.14159265f / 180.0f);
                    const float c = std::clamp(dot(facing, heading), -1.0f, 1.0f);
                    if (c < cos_limit) {
                        // Rotate the heading towards his facing by the limit, in the ground's plane.
                        const Vec3 across = normalized_or_zero(facing - heading * c);
                        const float s = std::sqrt(1.0f - cos_limit * cos_limit);
                        heading = normalized_or_zero(heading * cos_limit + across * s);
                        edit.set_front = true;
                        edit.front = heading;
                    } else {
                        heading = facing;
                    }
                    roll_heading_ = heading;
                } else if (length(facing) > 0.5f) {
                    roll_heading_ = facing;
                }
                last_roll_frames_ = roll_frame_;
                ++roll_frame_;
                roll_recent_ = kRollJumpGrace;
            }
        } else if (!started && roll_recent_ > 0) {
            --roll_recent_;
        }

        // --- Running above the run's cap: after a turn-around, or out of a roll.
        if (options_.turn_boost || carry_) {
            step_fast_run(v, edit);
        } else if (dash_) {
            end_dash(edit);
        }

        // --- The running animation for his speed (left to the game in the air).
        const bool on_ground = v.grounded && !v.jumping;
        if (on_ground || !dash_) {
            RunAnim want = RunAnim::Blend;
            if (dash_) {
                const bool fast_now = run_anim_ == RunAnim::DashFast;
                const bool dash_now = run_anim_ != RunAnim::Blend;
                if (dash_speed_ >= kDashFastAnimSpeed - (fast_now ? kRunAnimHysteresis : 0.0f)) {
                    want = RunAnim::DashFast;
                } else if (dash_speed_ >= kDashAnimSpeed - (dash_now ? kRunAnimHysteresis : 0.0f)) {
                    want = RunAnim::Dash;
                }
            }
            // Footsteps for the dashes: a step at each half of the stride (one stride per 60-frame loop,
            // played at his speed over the full run).
            if (want != RunAnim::Blend && v.table.walk_speed > 0.0f) {
                const float before = stride_;
                stride_ += dash_speed_ / v.table.walk_speed / 60.0f;
                if (before < 0.5f && stride_ >= 0.5f) edit.sounds |= kSoundStepRight;
                if (stride_ >= 1.0f) {
                    stride_ -= 1.0f;
                    edit.sounds |= kSoundStepLeft;
                }
            } else {
                stride_ = 0.0f;
            }
            // A dash is asked for every frame (the guest side starts it only if it is not playing: a
            // landing or anything else may have replaced it); the blend only when it changes.
            if (want != run_anim_ || want != RunAnim::Blend) {
                edit.set_run_anim = true;
                edit.run_anim = static_cast<std::uint8_t>(want);
            }
            run_anim_ = want;
        }

        remember(v, air_sideways);
        return edit;
    }

private:
    // `boosted`: the roll counts as boosted already (ground pound, dive, long jump), so its first boost
    // is the yellow one.
    void start_roll(float speed, int boost_due, bool boosted, float unit, MarioEdit& edit) noexcept {
        roll_speed_ = speed;
        last_roll_speed_ = speed;
        boosted_ = boosted;
        boost_wait_ = 0;
        roll_age_ = 0;
        boost_due_ = boost_due;
        hold_frames_ = boosted ? kRollHoldPound : kRollHoldCrouch;
        air_frames_ = 0;
        roll_recent_ = kRollJumpGrace;
        // Out of a ground pound, dive or long jump, A is a roll jump at once.
        roll_frame_ = boosted ? kRollJumpMinFrames : 1;
        last_roll_frames_ = boosted ? kRollJumpMinFrames : 0;
        edit.set_walk_speed = true;
        edit.walk_speed = speed / unit;
        roll_heading_ = {};  // taken from his facing on the next frame
        edit.block_crouch = boosted;
        edit.dust_puff = true;
        edit.sounds |= kSoundRollStart;
    }

    void end_dash(MarioEdit& /*edit*/) noexcept {
        dash_ = false;
        carry_ = false;
        boost_frames_ = 0;
        dash_air_ = 0;
    }

    // Faster than the run: Odyssey's run after a turn-around (up to 17 for a moment), or the speed a
    // roll had when he stood up out of it; both slow back to the run as Odyssey does above its cap.
    void step_fast_run(const MarioView& v, MarioEdit& edit) noexcept {
        const bool on_ground = v.grounded && !v.jumping;
        // (a roll cancel with Z held can leave the crouch flag up for its first frames)
        if (rolling() || (v.squat && fast_age_ > kFastRunGrace) || v.hip_drop) {
            if (dash_) end_dash(edit);
            return;
        }
        if (!dash_) {
            // A skid just ended into a run the other way.
            if (options_.turn_boost && prev_braking_ && !v.braking && on_ground && v.stick_mag >= kTurnBoostStick) {
                dash_ = true;
                dash_air_ = 0;
                fast_age_ = 0;
                boost_frames_ = kTurnBoostFrames;
                dash_speed_ = v.walk_speed * v.table.walk_speed;
            }
            if (!dash_) return;
        }
        ++fast_age_;
        // Standing up out of a roll or coming out of a skid, Galaxy zeroes his speed and may still flag
        // the skid for a few frames; the fast run rides that out.
        const bool settling = fast_age_ <= kFastRunGrace;
        if ((v.braking && !settling) || v.stick_mag < kTurnBoostKeepStick) {
            end_dash(edit);
            return;
        }
        if (!on_ground) {
            // The first jump at a dash has its own animation (the double and triple jumps, long jump,
            // side flip and backflip keep theirs).
            if (v.jumping && !prev_jumping_ && v.jump_type == kNormalJumpType && edit.anim == Anim::None &&
                dash_speed_ >= kDashAnimSpeed) {
                edit.anim = Anim::DashJump;
            }
            if (++dash_air_ > kTurnBoostAirFrames) end_dash(edit);
            return;
        }
        dash_air_ = 0;
        if (v.walk_speed < kRollStopWalkSpeed && !settling) {  // ran into something
            end_dash(edit);
            return;
        }
        if (boost_frames_ > 0) {
            --boost_frames_;
            dash_speed_ = std::min(std::max(dash_speed_, v.walk_speed * v.table.walk_speed) + kTurnBoostAccel,
                                   kTurnBoostSpeed);
        } else {
            dash_speed_ -= kOverCapDecel;
        }
        // Slowed back to the run's own speed: Galaxy's run takes over.
        if (boost_frames_ == 0 && dash_speed_ <= kRunCap) {
            end_dash(edit);
            return;
        }
        edit.set_walk_speed = true;
        edit.walk_speed = dash_speed_ / v.table.walk_speed;
    }

    void reset_tracking() noexcept {
        roll_frame_ = -1;
        roll_recent_ = 0;
        dive_ = false;
        dive_spent_ = false;
        air_spin_used_ = false;
        roll_jump_air_ = false;
        long_jump_air_ = false;
        belly_frames_ = 0;
    }
    void remember(const MarioView& v, Vec3 air_sideways) noexcept {
        prev_jumping_ = v.jumping;
        prev_grounded_ = v.grounded;
        prev_hip_drop_ = v.hip_drop && v.eligible;
        prev_braking_ = v.braking && v.eligible;
        if (v.jumping) prev_air_sideways_ = air_sideways;
    }

    Options options_{};
    int roll_frame_ = -1;       // -1 not rolling, else frames rolled + 1
    int roll_recent_ = 0;
    int air_frames_ = 0;
    int last_roll_frames_ = 0;  // frames the current or last roll lasted
    int boost_wait_ = 0;
    int roll_age_ = 0;  // frames since the roll started
    Vec3 roll_heading_{};  // the roll's facing, turned at most kRollTurnDegrees a frame
    int boost_due_ = kRollBoostWait;
    bool boosted_ = false;
    bool roll_jump_air_ = false;  // in the air after a roll jump
    bool long_jump_air_ = false;  // in the air in a long jump
    int belly_frames_ = 0;  // frames left of the belly slide
    float belly_speed_ = 0.0f;  // the dive's speed as it landed
    int hold_frames_ = kRollHoldCrouch;  // frames from the start or a boost before Z can be let go
    Vec3 prev_air_sideways_{};
    int boost_frames_ = 0;
    int fast_age_ = 0;  // frames of the fast run
    int paused_ = 0;    // frames it has waited out a state (the roll cancel's spin)
    bool carry_ = false;  // the fast run is a roll's speed carried on
    RunAnim run_anim_ = RunAnim::Blend;
    float stride_ = 0.0f;  // the dash's stride, 0..1
    int dash_air_ = 0;
    bool dash_ = false;
    float dash_speed_ = 0.0f;
    const char* end_reason_ = "";
    float roll_speed_ = 0.0f;
    float last_roll_speed_ = 0.0f;
    bool dive_ = false;
    int dive_age_ = 0;            // frames since the dive started
    bool dive_spent_ = false;     // dived since the ground or the last fresh air spin
    bool air_spin_used_ = false;  // the air spin is spent until the ground
    bool prev_jumping_ = false;
    bool prev_grounded_ = true;
    bool prev_hip_drop_ = false;
    bool prev_braking_ = false;
};

}  // namespace galaxy::moveset
