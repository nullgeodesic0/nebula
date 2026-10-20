#include "galaxy/odyssey_moveset_guest.h"

#include "galaxy/coop_guest.h"
#include "galaxy/odyssey_moveset.h"
#include "galaxy/runtime_settings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace galaxy {
namespace {

// RMGE01. Layouts from Petari's Mario.hpp, MarioActor.hpp and MarioConst.hpp (Mario is a MarioModule:
// +0 vtable, +4 its MarioActor).
namespace mario {
constexpr std::uint32_t kActor = 0x004u;
constexpr std::uint32_t kStates = 0x008u;        // MovementStates, two big-endian words (bit 0 is the top bit)
constexpr std::uint32_t kStatesB = 0x010u;       // the second MovementStates (_10)
constexpr std::uint32_t kStickPos = 0x1C0u;      // x, y, magnitude
constexpr std::uint32_t kAirGravity = 0x1D8u;
constexpr std::uint32_t kFront = 0x208u;
constexpr std::uint32_t kWorldPadDir = 0x1CCu;
constexpr std::uint32_t kWalkSpeed = 0x278u;
constexpr std::uint32_t kJumpVec = 0x2D4u;
constexpr std::uint32_t kJumpType = 0x430u;
constexpr std::uint32_t kTurnSlipTimer = 0x3D0u;  // u16, the turn-around skid
constexpr std::uint32_t kZeroGravityTimer = 0x424u;  // u16: frames the ground pound hangs in the air
constexpr std::uint32_t kState = 0x97Cu;         // MarioState* of a special state, 0 in ordinary play

// MovementStates bit n is mask 0x80000000 >> (n % 32) of word n / 32.
constexpr std::uint32_t bit(unsigned n) { return 0x80000000u >> (n % 32u); }
constexpr std::uint32_t kJumping = bit(0x00);
constexpr std::uint32_t kGrounded = bit(0x01);
constexpr std::uint32_t kTurning = bit(0x04);
constexpr std::uint32_t kSquat = bit(0x0A);
constexpr std::uint32_t kHipDrop = bit(0x0B);
constexpr std::uint32_t kKeepJump = bit(0x13);
constexpr std::uint32_t kWallHit = bit(0x08);
constexpr std::uint32_t kNoCrouch = bit(0x21);  // in the second word (+4): MovementStates._21
constexpr std::uint32_t kWallSlip = bit(0x17);
constexpr std::uint32_t kHardLanding = bit(0x08);  // in the second table: Mario::_10._8
}  // namespace mario

constexpr std::uint32_t kActorNoAirActions = 0x935u;  // MarioActor::_935 (bool): blocks the ground pound and spin
constexpr std::uint32_t kIsRequestSpin = 0x802BB498u;  // MarioActor::isRequestSpin() const
constexpr std::uint32_t kIsRequestRush = 0x802BB338u;  // MarioActor::isRequestRush() const
constexpr std::uint32_t kSetFrontVecKeepUp = 0x802AB5B8u;  // Mario::setFrontVecKeepUp(const TVec3f&)
constexpr std::uint32_t kBlown = 0x802DB4F4u;  // Mario::blown(const TVec3f&): the long jump's bounce off a wall
constexpr std::uint32_t kSetPunchHitTimer = 0x802B5150u;  // MarioActor::setPunchHitTimer(u8): the spin's hit on enemies
constexpr std::uint32_t kActorConst = 0x23Cu;  // MarioActor::mConst -> { MarioConstTable* mTable[2]; u32 mCurrent }

// Game functions (Petari symbols.txt, RMGE01).
constexpr std::uint32_t kGetPlayerMode = 0x802E8894u;          // MarioModule::getPlayerMode() const
constexpr std::uint32_t kIsInputDisable = 0x802E98B8u;         // MarioModule::isInputDisable() const
constexpr std::uint32_t kCheckTrgZ = 0x802E93F0u;              // MarioModule::checkTrgZ() const
constexpr std::uint32_t kCheckLvlZ = 0x802E9400u;              // MarioModule::checkLvlZ() const
// Odyssey's cap button is the Wii Remote's 1 button, which Galaxy does not use in play (C stays the
// camera reset). MR::getWPad(0) -> WPad::mButton (+0x08) -> WPadButton::mTrigger (+0x08), WPAD_BUTTON_1.
constexpr std::uint32_t kGetWPad = 0x803AB568u;                 // MR::getWPad(long)
constexpr std::uint32_t kWPadButton = 0x08u;
constexpr std::uint32_t kWPadButtonTrigger = 0x08u;
constexpr std::uint32_t kWPadButton1 = 0x0200u;
constexpr std::uint32_t kChangeAnimation = 0x802E8930u;        // MarioModule::changeAnimation(const char*, const char*)
constexpr std::uint32_t kStopAnimation = 0x802E8A74u;          // MarioModule::stopAnimation(const char*, const char*)
constexpr std::uint32_t kIsAnimationRun = 0x802E8B18u;         // MarioModule::isAnimationRun(const char*) const
constexpr std::uint32_t kCancelSquatMode = 0x80306654u;        // Mario::cancelSquatMode()
constexpr std::uint32_t kPlayEffect = 0x802E926Cu;             // MarioModule::playEffect(const char*)
constexpr std::uint32_t kStopEffect = 0x802E929Cu;             // MarioModule::stopEffect(const char*)

// The game's animation and effect names are Shift-JIS strings in its data. They are found there, not
// copied in. With the custom animations on (content\mods\animations, tools/animations), the animations
// below are the ones converted from the Odyssey .blend:
//   ã‚¹ã‚«ã‚¤ãƒ©ãƒ–ã‚¸ãƒ£ãƒ³ãƒ— (JumpTwin)  Odyssey JumpHipDrop: the ground-pound jump
//   ç©ºä¸­ä¸€å›žè»¢ (AirControl)        Odyssey Rolling, a 30-frame loop: the roll
//   é£›ã³è¾¼ã¿ã‚¸ãƒ£ãƒ³ãƒ— (Dive)        Odyssey Dive: the dive
//   ã‚¸ãƒ£ãƒ³ãƒ— (Jump)                Odyssey Jump: the roll jump
constexpr std::array<std::uint8_t, 18> kNameGroundPoundJump = {0x83, 0x58, 0x83, 0x4a, 0x83, 0x43, 0x83, 0x89, 0x83,
                                                              0x75, 0x83, 0x57, 0x83, 0x83, 0x83, 0x93, 0x83, 0x76};
constexpr std::array<std::uint8_t, 10> kNameRoll = {0x8b, 0xf3, 0x92, 0x86, 0x88, 0xea, 0x89, 0xf1, 0x93, 0x5d};
constexpr std::array<std::uint8_t, 16> kNameDive = {0x94, 0xf2, 0x82, 0xd1, 0x8d, 0x9e, 0x82, 0xdd,
                                                   0x83, 0x57, 0x83, 0x83, 0x83, 0x93, 0x83, 0x76};
// The roll's other moments, on rare swim, slide, skate and surf animations that the custom animations
// replace with Odyssey's: リングダッシュ準備 <- RollingStart, 腹ばいジャンプ <- RollingJump,
// スケート着地 <- RollingLand, サーフィン着地 <- RollingEnd. 落下 (falling) and 基本 (the ground blend)
// follow them.
constexpr std::array<std::uint8_t, 18> kNameRollStart = {0x83, 0x8a, 0x83, 0x93, 0x83, 0x4f, 0x83, 0x5f, 0x83,
                                                        0x62, 0x83, 0x56, 0x83, 0x85, 0x8f, 0x80, 0x94, 0xf5};
constexpr std::array<std::uint8_t, 14> kNameRollJump = {0x95, 0xa0, 0x82, 0xce, 0x82, 0xa2, 0x83,
                                                       0x57, 0x83, 0x83, 0x83, 0x93, 0x83, 0x76};
constexpr std::array<std::uint8_t, 12> kNameRollLand = {0x83, 0x58, 0x83, 0x50, 0x81, 0x5b,
                                                       0x83, 0x67, 0x92, 0x85, 0x92, 0x6e};
constexpr std::array<std::uint8_t, 14> kNameRollEnd = {0x83, 0x54, 0x81, 0x5b, 0x83, 0x74, 0x83,
                                                      0x42, 0x83, 0x93, 0x92, 0x85, 0x92, 0x6e};
// Mario's voice lines (Mario::playSoundJ names). With the custom voice mod they are Odyssey's clips:
// 声蹴り SE_PV_KICK (roll start), 声パンチ SE_PV_PUNCH (dive), 声大ジャンプ SE_PV_JUMP_L
// (ground-pound jump), 声着地停止 SE_PV_LAND (landing from a roll jump). The roll jump keeps the long
// jump's own 声幅ジャンプ (SE_PV_JUMP_LONG), which Galaxy plays as it starts it.
constexpr std::array<std::uint8_t, 6> kVoiceRoll = {0x90, 0xba, 0x8f, 0x52, 0x82, 0xe8};
constexpr std::array<std::uint8_t, 8> kVoiceDive = {0x90, 0xba, 0x83, 0x70, 0x83, 0x93, 0x83, 0x60};
constexpr std::array<std::uint8_t, 12> kVoiceHighJump = {0x90, 0xba, 0x91, 0xe5, 0x83, 0x57,
                                                        0x83, 0x83, 0x83, 0x93, 0x83, 0x76};
constexpr std::array<std::uint8_t, 10> kVoiceLand = {0x90, 0xba, 0x92, 0x85, 0x92, 0x6e, 0x92, 0xe2, 0x8e, 0x7e};
constexpr std::uint32_t kPlaySound = 0x802E92ACu;              // MarioModule::playSound(const char*, s32)
// スケート静止着地 <- LandRolling: the ground-pound roll.
constexpr std::array<std::uint8_t, 16> kNamePoundRoll = {0x83, 0x58, 0x83, 0x50, 0x81, 0x5b, 0x83, 0x67,
                                                        0x90, 0xc3, 0x8e, 0x7e, 0x92, 0x85, 0x92, 0x6e};
// ダッシュジャンプ <- JumpDashFast: the first jump of the run after a turn-around.
constexpr std::array<std::uint8_t, 16> kNameDashJump = {0x83, 0x5f, 0x83, 0x62, 0x83, 0x56, 0x83, 0x85,
                                                       0x83, 0x57, 0x83, 0x83, 0x83, 0x93, 0x83, 0x76};
// 飛び込み準備 <- HeadSliding: the belly-slide loop after a dive (the dive itself, HeadSlidingStart,
// ends on the belly).
constexpr std::array<std::uint8_t, 12> kNameDiveLand = {0x94, 0xf2, 0x82, 0xd1, 0x8d, 0x9e, 0x82, 0xdd, 0x8f, 0x80, 0x94, 0xf5};
// 泥高速歩行 (WalkBury, the fast wade through mud) <- Odyssey Dash: the run between Run and DashFast.
constexpr std::array<std::uint8_t, 10> kNameDashSlow = {0x93, 0x44, 0x8d, 0x82, 0x91, 0xac, 0x95, 0xe0, 0x8d, 0x73};
// The dash loops are 60 frames for one stride; played at his speed over Galaxy's full run so the feet
// stay planted. MarioActor::mMarioAnim (+0x234) -> MarioAnimator::mXanimePlayer (+0x0C) -> the current
// XanimeFrameCtrl (+0x20), a J3DFrameCtrl: +8 mEnd, +0xC mRate.
constexpr std::uint32_t kActorAnimator = 0x234u;
constexpr std::uint32_t kAnimatorPlayer = 0x0Cu;
constexpr std::uint32_t kPlayerFrameCtrl = 0x20u;
constexpr std::int16_t kDashLoopFrames = 60;
// 壁ヒット (bumping into a wall): a roll that runs into a wall.
constexpr std::array<std::uint8_t, 8> kNameWallBump = {0x95, 0xc7, 0x83, 0x71, 0x83, 0x62, 0x83, 0x67};
// Sound names for MarioEdit::sounds (moveset::Sound bits, same order).
constexpr std::array<std::uint8_t, 6> kSoundStepLeft = {0x91, 0xab, 0x89, 0xb9, 0x8d, 0xb6};  // 足音左
constexpr std::array<std::uint8_t, 6> kSoundStepRight = {0x91, 0xab, 0x89, 0xb9, 0x89, 0x45};  // 足音右
constexpr std::array<std::uint8_t, 6> kSoundRolling = {0x8d, 0xe2, 0x8a, 0x8a, 0x82, 0xe8};  // 坂滑り
constexpr std::array<std::uint8_t, 20> kSoundRollStart = {0x83, 0x58, 0x83, 0x79, 0x83, 0x56, 0x83, 0x83, 0x83, 0x8b, 0x83, 0x5f, 0x83, 0x62, 0x83, 0x56, 0x83, 0x85, 0x8e, 0xe3};  // スペシャルダッシュ弱
constexpr std::array<std::uint8_t, 20> kSoundRollBoost = {0x83, 0x58, 0x83, 0x79, 0x83, 0x56, 0x83, 0x83, 0x83, 0x8b, 0x83, 0x5f, 0x83, 0x62, 0x83, 0x56, 0x83, 0x85, 0x8b, 0xad};  // スペシャルダッシュ強
constexpr std::array<std::uint8_t, 6> kSoundBump = {0x95, 0xc7, 0x8f, 0xd5, 0x93, 0xcb};  // 壁衝突
// 引き戻し着地 (PullBackLand) <- Odyssey RollingStandUp: the dive's roll-out, a forward tumble up to standing.
constexpr std::array<std::uint8_t, 12> kNameDiveTumble = {0x88, 0xf8, 0x82, 0xab, 0x96, 0xdf, 0x82, 0xb5, 0x92, 0x85, 0x92, 0x6e};
constexpr std::int16_t kDiveTumbleFrames = 9;  // its length with the custom animations (RollingStandUp)
constexpr float kDiveTumbleRate = 0.55f;       // about 16 frames
constexpr std::array<std::uint8_t, 4> kNameFall = {0x97, 0x8e, 0x89, 0xba};
constexpr std::array<std::uint8_t, 4> kNameBase = {0x8a, 0xee, 0x96, 0x7b};
// The run track's two animations: ãƒ©ãƒ³ (Run) and ãƒ¡ã‚¿ãƒ«ãƒ€ãƒƒã‚·ãƒ¥ (RunDash, Odyssey Dash when converted).
constexpr std::array<std::uint8_t, 4> kNameRun = {0x83, 0x89, 0x83, 0x93};
constexpr std::array<std::uint8_t, 14> kNameDash = {0x83, 0x81, 0x83, 0x5e, 0x83, 0x8b, 0x83,
                                                   0x5f, 0x83, 0x62, 0x83, 0x56, 0x83, 0x85};
// Rolling dust: å…±é€šã‚¹ãƒªãƒƒãƒ—å‚ (the slope-slide dust, a trail that runs until stopped) for the roll and
// å…±é€šãƒ–ãƒ¬ãƒ¼ã‚­ (the brake puff) as it starts.
constexpr std::array<std::uint8_t, 14> kEffectSlideDust = {0x8b, 0xa4, 0x92, 0xca, 0x83, 0x58, 0x83,
                                                          0x8a, 0x83, 0x62, 0x83, 0x76, 0x8d, 0xe2};
// 共通着地大: the big landing cloud (a dive's or a roll jump's landing).
constexpr std::array<std::uint8_t, 10> kEffectLandingCloud = {0x8b, 0xa4, 0x92, 0xca, 0x92,
                                                             0x85, 0x92, 0x6e, 0x91, 0xe5};
// 共通ひこうきブースト: the boost burst of Galaxy's flying Mario, a gust behind him (a roll boost).
constexpr std::array<std::uint8_t, 20> kEffectAirGust = {0x8b, 0xa4, 0x92, 0xca, 0x82, 0xd0, 0x82, 0xb1, 0x82, 0xa4, 0x82, 0xab, 0x83, 0x75, 0x81, 0x5b, 0x83, 0x58, 0x83, 0x67};
constexpr std::array<std::uint8_t, 12> kEffectBrakePuff = {0x8b, 0xa4, 0x92, 0xca, 0x83, 0x75,
                                                          0x83, 0x8c, 0x81, 0x5b, 0x83, 0x4c};

struct GuestNames {
    bool searched = false;
    std::uint32_t ground_pound_jump = 0;
    std::uint32_t roll = 0;
    std::uint32_t dive = 0;
    std::uint32_t roll_start = 0;
    std::uint32_t roll_jump = 0;
    std::uint32_t roll_land = 0;
    std::uint32_t roll_end = 0;
    std::uint32_t fall = 0;
    std::uint32_t base = 0;
    std::uint32_t pound_roll = 0;
    std::uint32_t dash_jump = 0;
    std::uint32_t dive_land = 0;
    std::uint32_t dive_tumble = 0;
    std::uint32_t wall_bump = 0;
    std::array<std::uint32_t, 6> sounds{};
    std::uint32_t voice_roll = 0;
    std::uint32_t voice_dive = 0;
    std::uint32_t voice_high_jump = 0;
    std::uint32_t voice_land = 0;
    std::uint32_t slide_dust = 0;
    std::uint32_t brake_puff = 0;
    std::uint32_t landing_cloud = 0;
    std::uint32_t air_gust = 0;
    std::uint32_t run = 0;
    std::uint32_t dash = 0;  // メタルダッシュ: DashFast
    std::uint32_t dash_slow = 0;  // 泥高速歩行: Odyssey's Dash
};

bool trace_enabled() {
    static const bool enabled = [] {
        char* value = nullptr;
        std::size_t length = 0u;
        const bool set = _dupenv_s(&value, &length, "GALAXY_TRACE_MOVESET") == 0 && value != nullptr &&
                         value[0] != '\0' && value[0] != '0';
        std::free(value);
        return set;
    }();
    return enabled;
}

class Guest {
public:
    Guest(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services, std::uint32_t pc)
        : context_(context), memory_(memory), services_(services), pc_(pc) {}

    std::uint32_t u32(std::uint32_t address) const { return guest_load_u32(memory_, address, services_, pc_); }
    std::uint16_t u16(std::uint32_t address) const { return guest_load_u16(memory_, address, services_, pc_); }
    std::uint8_t u8(std::uint32_t address) const { return guest_load_u8(memory_, address, services_, pc_); }
    float f32(std::uint32_t address) const { return std::bit_cast<float>(u32(address)); }
    moveset::Vec3 vec(std::uint32_t address) const { return {f32(address), f32(address + 4u), f32(address + 8u)}; }
    void set_u32(std::uint32_t address, std::uint32_t value) const {
        guest_store_u32(memory_, address, value, services_, pc_);
    }
    void set_u16(std::uint32_t address, std::uint16_t value) const {
        guest_store_u16(memory_, address, value, services_, pc_);
    }
    void set_u8(std::uint32_t address, std::uint8_t value) const {
        const std::uint32_t word = address & ~3u;
        const unsigned shift = (3u - (address & 3u)) * 8u;
        set_u32(word, (u32(word) & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(value) << shift));
    }
    void set_f32(std::uint32_t address, float value) const { set_u32(address, std::bit_cast<std::uint32_t>(value)); }
    void set_vec(std::uint32_t address, moveset::Vec3 v) const {
        set_f32(address, v.x);
        set_f32(address + 4u, v.y);
        set_f32(address + 8u, v.z);
    }

    // Calls a game function with up to three integer arguments and returns r3. The caller's registers
    // are restored by the owner of this object (see tick_odyssey_moveset).
    std::uint32_t call(std::uint32_t function, std::uint32_t r3 = 0u, std::uint32_t r4 = 0u,
                       std::uint32_t r5 = 0u) const {
        context_->gpr[3] = r3;
        context_->gpr[4] = r4;
        context_->gpr[5] = r5;
        services_->call_guest(services_->user, function, context_, memory_);
        return context_->gpr[3];
    }

    // Calls a game function taking (this, const TVec3f&): the vector goes on the guest stack below the
    // caller's frame, and r1 is moved under it for the call (restored with the other registers).
    std::uint32_t call_with_vec(std::uint32_t function, std::uint32_t self, moveset::Vec3 v) const {
        const std::uint32_t sp = context_->gpr[1];
        const std::uint32_t frame = (sp - 0x40u) & ~0xFu;
        set_vec(frame + 0x10u, v);
        context_->gpr[1] = frame;
        set_u32(frame, sp);  // back chain
        const std::uint32_t result = call(function, self, frame + 0x10u);
        context_->gpr[1] = sp;
        return result;
    }

    // The address of a NUL-terminated byte string in the game's data, or 0.
    template <std::size_t N>
    std::uint32_t find_string(const std::array<std::uint8_t, N>& text) const {
        constexpr std::uint32_t kBegin = 0x80004000u;
        constexpr std::uint32_t kEnd = 0x80700000u;
        for (std::uint32_t at = kBegin + 1u; at + N + 1u < kEnd; ++at) {
            if (u8(at) != text[0] || u8(at - 1u) != 0u) continue;
            bool same = true;
            for (std::size_t i = 1; i < N && same; ++i) same = u8(at + static_cast<std::uint32_t>(i)) == text[i];
            if (same && u8(at + static_cast<std::uint32_t>(N)) == 0u) return at;
        }
        return 0u;
    }

private:
    PpcContext* context_;
    GuestMemoryV1* memory_;
    const NativeServicesV1* services_;
    std::uint32_t pc_;
};

moveset::Options options_from(const OdysseyMovesetSettings& settings) {
    moveset::Options options;
    options.roll = settings.roll;
    options.roll_jump = settings.roll_jump;
    options.ground_pound_jump = settings.ground_pound_jump;
    options.dive = settings.dive;
    options.turn_boost = settings.turn_boost;
    return options;
}

bool plausible_mario(std::uint32_t address) {
    return address >= 0x80000000u && address < 0x81800000u && (address & 3u) == 0u;
}

}  // namespace

void tick_odyssey_moveset(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                          std::uint32_t guest_pc) {
    static moveset::Moveset s_moveset;
    static GuestNames s_names;
    static bool s_active = false;
    static bool s_in_tick = false;
    static std::uint32_t s_frame = 0u;
    static std::uint8_t s_last_run_anim = 0u;
    if (s_in_tick || services == nullptr || services->call_guest == nullptr) return;

    const OdysseyMovesetSettings settings = get_runtime_settings().moveset;
    if (!settings.enabled) {
        if (s_active) {
            s_moveset.reset();
            s_active = false;
        }
        return;
    }
    const std::uint32_t self = context->gpr[3];
    if (!plausible_mario(self)) return;
    // One moveset state follows one Mario: the co-op second player keeps Galaxy's moves.
    if (coop_is_second_player_mario(self)) return;

    s_in_tick = true;
    const PpcContext saved = *context;
    Guest guest(context, memory, services, guest_pc);
    do {
        const std::uint32_t actor = guest.u32(self + mario::kActor);
        if (!plausible_mario(actor)) break;
        s_active = true;
        s_moveset.set_options(options_from(settings));
        ++s_frame;

        // The game's own answers: input under the player's control (no demo, no event), his mode.
        const std::uint32_t mode = guest.call(kGetPlayerMode, self) & 0xFFFFu;
        const bool input_disabled = (guest.call(kIsInputDisable, self) & 0xFFu) != 0u;
        const bool special_state = guest.u32(self + mario::kState) != 0u;

        moveset::MarioView view;
        view.eligible = (mode == 0u || mode == 1u) && !input_disabled && !special_state;
        const std::uint32_t states = guest.u32(self + mario::kStates);
        view.grounded = (states & mario::kGrounded) != 0u;
        view.jumping = (states & mario::kJumping) != 0u;
        view.hip_drop = (states & mario::kHipDrop) != 0u;
        view.squat = (states & mario::kSquat) != 0u;
        view.wall_slide = (states & mario::kWallSlip) != 0u;
        view.wall_hit = (states & mario::kWallHit) != 0u;
        view.braking = (states & mario::kTurning) != 0u || guest.u16(self + mario::kTurnSlipTimer) != 0u;
        view.jump_type = guest.u32(self + mario::kJumpType);
        view.walk_speed = guest.f32(self + mario::kWalkSpeed);
        view.stick_mag = guest.f32(self + mario::kStickPos + 8u);
        view.jump_vec = guest.vec(self + mario::kJumpVec);
        view.front = guest.vec(self + mario::kFront);
        view.stick_dir = guest.vec(self + mario::kWorldPadDir);
        view.down = guest.vec(self + mario::kAirGravity);

        if (view.eligible) {
            view.trig_z = (guest.call(kCheckTrgZ, self) & 0xFFu) != 0u;
            view.lvl_z = (guest.call(kCheckLvlZ, self) & 0xFFu) != 0u;
            // The cap button: Wii 1 pressed this frame (bound to the left stick click and the C key in
            // runtime_settings.ini; F1 > Controls > 1 rebinds it).
            const std::uint32_t wpad = guest.call(kGetWPad, 0u);
            const std::uint32_t button = plausible_mario(wpad) ? guest.u32(wpad + kWPadButton) : 0u;
            if ((view.jumping && !view.grounded) || s_moveset.rolling()) {
                view.spin_request = ((guest.call(kIsRequestSpin, actor) | guest.call(kIsRequestRush, actor)) & 0xFFu) != 0u;
            }
            view.trig_c = plausible_mario(button) &&
                          (guest.u32(button + kWPadButtonTrigger) & kWPadButton1) != 0u;

            // Mario's constant table: MarioActor::mConst -> mTable[mCurrentTable].
            const std::uint32_t constants = guest.u32(actor + kActorConst);
            if (plausible_mario(constants)) {
                const std::uint32_t index = guest.u32(constants + 8u) & 1u;
                const std::uint32_t table = guest.u32(constants + index * 4u);
                if (plausible_mario(table)) {
                    const float high = guest.f32(table + 0x08u);
                    const float small = guest.f32(table + 0x00u);
                    const float squat_front = guest.f32(table + 0x70u);
                    if (high > 1.0f && high < 200.0f) view.table.jump_height_high = high;
                    if (small > 1.0f && small < 200.0f) view.table.jump_height_small = small;
                    if (squat_front > 1.0f && squat_front < 200.0f) view.table.squat_jump_front = squat_front;
                    const float gravity = guest.f32(table + 0x24u);  // mGravityJumping[0]
                    if (gravity > 0.05f && gravity < 20.0f) view.table.gravity = gravity;
                    const float walk = guest.f32(table + 0x224u);
                    if (walk > 1.0f && walk < 200.0f) view.table.walk_speed = walk;
                }
            }
        }

        const auto ensure_names = [&] {
            if (s_names.searched) return;
            s_names.searched = true;
            s_names.ground_pound_jump = guest.find_string(kNameGroundPoundJump);
            s_names.roll = guest.find_string(kNameRoll);
            s_names.dive = guest.find_string(kNameDive);
            s_names.roll_start = guest.find_string(kNameRollStart);
            s_names.roll_jump = guest.find_string(kNameRollJump);
            s_names.roll_land = guest.find_string(kNameRollLand);
            s_names.roll_end = guest.find_string(kNameRollEnd);
            s_names.fall = guest.find_string(kNameFall);
            s_names.base = guest.find_string(kNameBase);
            s_names.pound_roll = guest.find_string(kNamePoundRoll);
            s_names.dash_jump = guest.find_string(kNameDashJump);
            s_names.dash_slow = guest.find_string(kNameDashSlow);
            s_names.dive_land = guest.find_string(kNameDiveLand);
            s_names.dive_tumble = guest.find_string(kNameDiveTumble);
            s_names.wall_bump = guest.find_string(kNameWallBump);
            s_names.sounds = {guest.find_string(kSoundStepLeft), guest.find_string(kSoundStepRight),
                              guest.find_string(kSoundRolling), guest.find_string(kSoundRollStart),
                              guest.find_string(kSoundRollBoost), guest.find_string(kSoundBump)};
            s_names.voice_roll = guest.find_string(kVoiceRoll);
            s_names.voice_dive = guest.find_string(kVoiceDive);
            s_names.voice_high_jump = guest.find_string(kVoiceHighJump);
            s_names.voice_land = guest.find_string(kVoiceLand);
            s_names.slide_dust = guest.find_string(kEffectSlideDust);
            s_names.brake_puff = guest.find_string(kEffectBrakePuff);
            s_names.landing_cloud = guest.find_string(kEffectLandingCloud);
            s_names.air_gust = guest.find_string(kEffectAirGust);
            s_names.run = guest.find_string(kNameRun);
            s_names.dash = guest.find_string(kNameDash);
            if (trace_enabled()) {
                std::fprintf(stderr,
                             "[moveset] names: ground-pound jump %08X roll %08X dive %08X roll jump %08X dust %08X "
                             "puff %08X\n",
                             s_names.ground_pound_jump, s_names.roll, s_names.dive, s_names.roll_jump,
                             s_names.slide_dust, s_names.brake_puff);
            }
        };
        if (s_moveset.diving() && view.eligible) {
            ensure_names();
            const auto running = [&](std::uint32_t name) {
                return name != 0u && (guest.call(kIsAnimationRun, self, name) & 0xFFu) != 0u;
            };
            view.dive_anim_running = s_names.dive == 0u || running(s_names.dive) || running(s_names.dive_land);
        }
        if (s_moveset.rolling() && view.eligible) {
            ensure_names();
            // Rolling, or still in the roll's start, which hands over to rolling by itself.
            view.roll_anim_running =
                s_names.roll == 0u || (guest.call(kIsAnimationRun, self, s_names.roll) & 0xFFu) != 0u ||
                (s_names.roll_start != 0u && (guest.call(kIsAnimationRun, self, s_names.roll_start) & 0xFFu) != 0u) ||
                (s_names.pound_roll != 0u && (guest.call(kIsAnimationRun, self, s_names.pound_roll) & 0xFFu) != 0u);
        }

        const bool was_rolling = s_moveset.rolling();
        const bool was_diving = s_moveset.diving();
        const moveset::MarioEdit edit = s_moveset.step(view);

        if (edit.set_walk_speed) guest.set_f32(self + mario::kWalkSpeed, edit.walk_speed);
        if (edit.set_jump_vec) guest.set_vec(self + mario::kJumpVec, edit.jump_vec);
        if (edit.set_jump_type) guest.set_u32(self + mario::kJumpType, edit.jump_type);
        if (edit.end_jump_hold) {
            guest.set_u32(self + mario::kStates, guest.u32(self + mario::kStates) & ~mario::kKeepJump);
        }
        if (edit.clear_hard_landing) {
            guest.set_u32(self + mario::kStatesB, guest.u32(self + mario::kStatesB) & ~mario::kHardLanding);
        }
        if (edit.attack_frames != 0u) guest.call(kSetPunchHitTimer, actor, edit.attack_frames);
        if (edit.knock_back) guest.call_with_vec(kBlown, self, edit.knock_back_dir);
        if (edit.set_front) guest.call_with_vec(kSetFrontVecKeepUp, self, edit.front);
        if (edit.face_stick) guest.call(kSetFrontVecKeepUp, self, self + mario::kWorldPadDir);
        if (edit.hard_landing) {
            guest.set_u32(self + mario::kStatesB, guest.u32(self + mario::kStatesB) | mario::kHardLanding);
        }
        if (edit.cancel_squat) guest.call(kCancelSquatMode, self);
        if (edit.block_crouch) {
            guest.set_u32(self + mario::kStates, guest.u32(self + mario::kStates) & ~mario::kSquat);
            guest.set_u32(self + mario::kStates + 4u, guest.u32(self + mario::kStates + 4u) | mario::kNoCrouch);
        }
        if (edit.block_air_actions) guest.set_u8(actor + kActorNoAirActions, 1u);
        if (edit.end_hip_drop_air) {
            guest.set_u32(self + mario::kStates, guest.u32(self + mario::kStates) & ~mario::kHipDrop);
            guest.set_u16(self + mario::kZeroGravityTimer, 0u);
        }
        if (edit.end_hip_drop) {
            guest.set_u32(self + mario::kStates,
                          guest.u32(self + mario::kStates) & ~(mario::kJumping | mario::kHipDrop));
            guest.set_vec(self + mario::kJumpVec, {});
        }
        if (edit.stop_belly_slide) {
            ensure_names();
            for (const std::uint32_t name : {s_names.dive_land, s_names.dive_tumble}) {
                if (name != 0u && (guest.call(kIsAnimationRun, self, name) & 0xFFu) != 0u) {
                    guest.call(kStopAnimation, self, name, s_names.base);
                }
            }
        }
        if (edit.sounds != 0u) {
            ensure_names();
            for (std::size_t i = 0; i < s_names.sounds.size(); ++i) {
                if ((edit.sounds & (1u << i)) != 0u && s_names.sounds[i] != 0u) {
                    guest.call(kPlaySound, self, s_names.sounds[i], 0xFFFFFFFFu);
                }
            }
        }
        if (edit.set_trail) {
            ensure_names();
            if (s_names.slide_dust != 0u) guest.call(edit.trail ? kPlayEffect : kStopEffect, self, s_names.slide_dust);
        }
        if (edit.air_gust) {
            ensure_names();
            if (s_names.air_gust != 0u) guest.call(kPlayEffect, self, s_names.air_gust);
        }
        if (edit.dust_puff || edit.dust_landing) {
            ensure_names();
            if (edit.dust_puff && s_names.brake_puff != 0u) guest.call(kPlayEffect, self, s_names.brake_puff);
            if (edit.dust_landing && s_names.landing_cloud != 0u) guest.call(kPlayEffect, self, s_names.landing_cloud);
        }
        if (edit.set_run_anim) {
            ensure_names();
            // Dash and DashFast are played as Mario's main animation (the walk blend's run track never
            // shows them); the blend is the game's own walk and run. 0: no such animation in this game.
            const std::uint32_t dash = s_names.dash_slow;
            const std::uint32_t dash_fast = s_names.dash;
            const auto running = [&](std::uint32_t name) {
                return name != 0u && (guest.call(kIsAnimationRun, self, name) & 0xFFu) != 0u;
            };
            const auto run_anim = static_cast<moveset::RunAnim>(edit.run_anim);
            const std::uint32_t want = run_anim == moveset::RunAnim::DashFast ? dash_fast
                                       : run_anim == moveset::RunAnim::Dash   ? dash
                                                                              : 0u;
            for (const std::uint32_t other : {dash, dash_fast}) {
                if (other != want && running(other)) guest.call(kStopAnimation, self, other, s_names.base);
            }
            if (want != 0u && !running(want)) guest.call(kChangeAnimation, self, want, 0u);
            if (want != 0u) {
                const std::uint32_t animator = guest.u32(actor + kActorAnimator);
                const std::uint32_t player = plausible_mario(animator) ? guest.u32(animator + kAnimatorPlayer) : 0u;
                const std::uint32_t ctrl = plausible_mario(player) ? guest.u32(player + kPlayerFrameCtrl) : 0u;
                if (plausible_mario(ctrl) && static_cast<std::int16_t>(guest.u16(ctrl + 8u)) == kDashLoopFrames) {
                    const float rate = std::clamp(s_moveset.fast_run_speed() / view.table.walk_speed, 0.5f, 3.0f);
                    guest.set_f32(ctrl + 0xCu, rate);
                }
            }
            if (trace_enabled() && static_cast<std::uint8_t>(run_anim) != s_last_run_anim) {
                std::fprintf(stderr, "[moveset] f%u run animation %u (fast run %.1f u/fr)\n", s_frame,
                             static_cast<unsigned>(edit.run_anim), static_cast<double>(s_moveset.fast_run_speed()));
            }
            s_last_run_anim = static_cast<std::uint8_t>(run_anim);
        }
        if (edit.anim != moveset::Anim::None || edit.stop_roll_anim) {
            ensure_names();
            if (edit.stop_roll_anim) {
                // Every animation of the roll goes, back to the ground blend (Galaxy picks the crouch from
                // there if Z is still held).
                for (const std::uint32_t name : {s_names.roll, s_names.roll_start, s_names.pound_roll}) {
                    if (name != 0u && (guest.call(kIsAnimationRun, self, name) & 0xFFu) != 0u) {
                        guest.call(kStopAnimation, self, name, s_names.base);
                    }
                }
            }
            // The animation, and the one the game plays when it ends (0: the game decides).
            std::uint32_t name = 0u;
            std::uint32_t next = 0u;
            switch (edit.anim) {
            // The lunge ends flat on the belly; the belly-slide loop holds that pose until he lands.
            case moveset::Anim::Dive: name = s_names.dive; next = s_names.dive_land; break;
            case moveset::Anim::HighJump: name = s_names.ground_pound_jump; next = s_names.fall; break;
            case moveset::Anim::Roll: name = s_names.roll; break;
            // The roll's start hands over to the rolling loop through the roll's own check (the loop is
            // started whenever it is not playing), not as the game's "next" animation: that would bring
            // the roll back after anything else that played over it, such as the spin of a roll cancel.
            case moveset::Anim::RollStart:
            case moveset::Anim::RollBoost:
                name = s_names.roll_start != 0u ? s_names.roll_start : s_names.roll;
                break;
            case moveset::Anim::RollJump: name = s_names.roll_jump; next = s_names.fall; break;
            case moveset::Anim::DashJump: name = s_names.dash_jump; next = s_names.fall; break;
            case moveset::Anim::DiveHold: name = s_names.dive_land; break;
            case moveset::Anim::RollBump: name = s_names.wall_bump; next = s_names.base; break;
            case moveset::Anim::DiveLand: name = s_names.dive_tumble; next = s_names.base; break;
            case moveset::Anim::GroundPoundRoll:
                name = s_names.pound_roll != 0u ? s_names.pound_roll : s_names.roll;
                break;
            case moveset::Anim::RollLand: name = s_names.roll_land; next = s_names.base; break;
            // Getting up out of a roll is the same tumble up as out of a dive (Odyssey's RollingStandUp).
            case moveset::Anim::RollEnd: name = s_names.dive_tumble != 0u ? s_names.dive_tumble : s_names.roll_end; next = s_names.base; break;
            case moveset::Anim::None: break;
            }
            if (name != 0u) guest.call(kChangeAnimation, self, name, next);
            if (edit.anim == moveset::Anim::DiveLand && name != 0u) {
                // Odyssey's roll-out is 9 frames; played slower it reads as a tumble out of the slide.
                const std::uint32_t animator = guest.u32(actor + kActorAnimator);
                const std::uint32_t player = plausible_mario(animator) ? guest.u32(animator + kAnimatorPlayer) : 0u;
                const std::uint32_t ctrl = plausible_mario(player) ? guest.u32(player + kPlayerFrameCtrl) : 0u;
                if (plausible_mario(ctrl) && static_cast<std::int16_t>(guest.u16(ctrl + 8u)) == kDiveTumbleFrames) {
                    guest.set_f32(ctrl + 0xCu, kDiveTumbleRate);
                }
            }
            std::uint32_t voice = 0u;
            switch (edit.anim) {
            case moveset::Anim::RollStart:
            case moveset::Anim::GroundPoundRoll: voice = s_names.voice_roll; break;
            case moveset::Anim::Dive: voice = s_names.voice_dive; break;
            case moveset::Anim::HighJump: voice = s_names.voice_high_jump; break;
            case moveset::Anim::RollLand: voice = s_names.voice_land; break;
            default: break;
            }
            if (voice != 0u) guest.call(kPlaySound, self, voice, 0xFFFFFFFFu);
            // The roll's dust: a puff where it starts and a trail behind him while it lasts.
            if (edit.anim == moveset::Anim::RollStart || edit.anim == moveset::Anim::GroundPoundRoll) {
                if (s_names.brake_puff != 0u) guest.call(kPlayEffect, self, s_names.brake_puff);
            }
        }

        if (trace_enabled()) {
            if (s_moveset.rolling() != was_rolling || s_moveset.diving() != was_diving || edit.set_jump_vec) {
                std::fprintf(stderr,
                             "[moveset] f%u %s roll %d->%d dive %d->%d jt %u walk %.2f jump (%.1f %.1f %.1f) "
                             "stick %.2f grounded %d jumping %d hipdrop %d squat %d%s%s\n",
                             s_frame, edit.set_jump_vec ? "set-jump" : "event", was_rolling, s_moveset.rolling(),
                             was_diving, s_moveset.diving(), view.jump_type, static_cast<double>(view.walk_speed),
                             static_cast<double>(view.jump_vec.x), static_cast<double>(view.jump_vec.y),
                             static_cast<double>(view.jump_vec.z), static_cast<double>(view.stick_mag),
                             view.grounded, view.jumping, view.hip_drop, view.squat,
                             was_rolling && !s_moveset.rolling() ? " ended: " : "",
                             was_rolling && !s_moveset.rolling() ? s_moveset.roll_end_reason() : "");
            } else if (s_frame % 60u == 0u) {
                std::fprintf(stderr,
                             "[moveset] f%u elig %d mode %u walk %.2f jt %u g%d j%d hd%d sq%d stick %.2f "
                             "jv (%.1f %.1f %.1f) front (%.2f %.2f %.2f) down (%.2f %.2f %.2f)\n",
                             s_frame, view.eligible, mode, static_cast<double>(view.walk_speed), view.jump_type,
                             view.grounded, view.jumping, view.hip_drop, view.squat,
                             static_cast<double>(view.stick_mag), static_cast<double>(view.jump_vec.x),
                             static_cast<double>(view.jump_vec.y), static_cast<double>(view.jump_vec.z),
                             static_cast<double>(view.front.x), static_cast<double>(view.front.y),
                             static_cast<double>(view.front.z), static_cast<double>(view.down.x),
                             static_cast<double>(view.down.y), static_cast<double>(view.down.z));
            }
        }
    } while (false);
    // Only the registers the guest calls may have changed. The timer state (decrementer, time base) and
    // the SPRs move on with the calls and must not be rolled back.
    std::memcpy(context->gpr, saved.gpr, sizeof(context->gpr));
    std::memcpy(context->fpr_bits, saved.fpr_bits, sizeof(context->fpr_bits));
    std::memcpy(context->ps1_bits, saved.ps1_bits, sizeof(context->ps1_bits));
    context->cr = saved.cr;
    context->lr = saved.lr;
    context->ctr = saved.ctr;
    context->xer = saved.xer;
    context->pc = saved.pc;
    s_in_tick = false;
}

}  // namespace galaxy
