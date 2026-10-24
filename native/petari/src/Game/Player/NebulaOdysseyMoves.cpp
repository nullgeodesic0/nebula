#include "Game/Player/NebulaOdysseyMoves.hpp"

#ifdef TARGET_PC
#include "Game/Animation/XanimePlayer.hpp"
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioAnimator.hpp"
#include "Game/Player/MarioConst.hpp"
#include "Game/System/WPad.hpp"
#include "Game/System/WPadButton.hpp"
#include "Game/System/WPadHolder.hpp"
#include <cstddef>
#include <nebula_game_api.h>
#include <nebula_motion_style.h>
#include <nebula_odyssey_moveset.h>
#include <revolution/os.h>

namespace moveset = galaxy::moveset;

namespace {
    // The game's names (Shift-JIS, as the decompiled code spells them). With the custom animations
    // (tools/animations) several are Odyssey's moves converted onto rarely used animations:
    const char* const kGroundPoundJump = "\x83\x58\x83\x4A\x83\x43\x83\x89\x83\x75\x83\x57\x83\x83\x83\x93\x83\x76";  // ã‚¹ã‚«ã‚¤ãƒ©ãƒ–ã‚¸ãƒ£ãƒ³ãƒ— <- JumpHipDrop
    const char* const kRoll = "\x8B\xF3\x92\x86\x88\xEA\x89\xF1\x93\x5D";  // ç©ºä¸­ä¸€å›žè»¢ <- Rolling
    const char* const kDive = "\x94\xF2\x82\xD1\x8D\x9E\x82\xDD\x83\x57\x83\x83\x83\x93\x83\x76";  // é£›ã³è¾¼ã¿ã‚¸ãƒ£ãƒ³ãƒ— <- Dive
    const char* const kRollStart = "\x83\x8A\x83\x93\x83\x4F\x83\x5F\x83\x62\x83\x56\x83\x85\x8F\x80\x94\xF5";  // ãƒªãƒ³ã‚°ãƒ€ãƒƒã‚·ãƒ¥æº–å‚™ <- RollingStart
    const char* const kRollJump = "\x95\xA0\x82\xCE\x82\xA2\x83\x57\x83\x83\x83\x93\x83\x76";  // è…¹ã°ã„ã‚¸ãƒ£ãƒ³ãƒ— <- RollingJump
    const char* const kRollLand = "\x83\x58\x83\x50\x81\x5B\x83\x67\x92\x85\x92\x6E";  // ã‚¹ã‚±ãƒ¼ãƒˆç€åœ° <- RollingLand
    const char* const kRollEnd = "\x83\x54\x81\x5B\x83\x74\x83\x42\x83\x93\x92\x85\x92\x6E";  // ã‚µãƒ¼ãƒ•ã‚£ãƒ³ç€åœ° <- RollingEnd
    const char* const kPoundRoll = "\x83\x58\x83\x50\x81\x5B\x83\x67\x90\xC3\x8E\x7E\x92\x85\x92\x6E";  // ã‚¹ã‚±ãƒ¼ãƒˆé™æ­¢ç€åœ° <- LandRolling
    const char* const kDashJump = "\x83\x5F\x83\x62\x83\x56\x83\x85\x83\x57\x83\x83\x83\x93\x83\x76";  // ãƒ€ãƒƒã‚·ãƒ¥ã‚¸ãƒ£ãƒ³ãƒ— <- JumpDashFast
    const char* const kDiveLand = "\x94\xF2\x82\xD1\x8D\x9E\x82\xDD\x8F\x80\x94\xF5";  // é£›ã³è¾¼ã¿æº–å‚™ <- HeadSliding (the belly slide)
    const char* const kDiveTumble = "\x88\xF8\x82\xAB\x96\xDF\x82\xB5\x92\x85\x92\x6E";  // å¼•ãæˆ»ã—ç€åœ° <- RollingStandUp
    const char* const kDashSlow = "\x93\x44\x8D\x82\x91\xAC\x95\xE0\x8D\x73";  // æ³¥é«˜é€Ÿæ­©è¡Œ <- Dash
    const char* const kDash = "\x83\x81\x83\x5E\x83\x8B\x83\x5F\x83\x62\x83\x56\x83\x85";  // ãƒ¡ã‚¿ãƒ«ãƒ€ãƒƒã‚·ãƒ¥ <- DashFast
    const char* const kWallBump = "\x95\xC7\x83\x71\x83\x62\x83\x67";  // å£ãƒ’ãƒƒãƒˆ
    const char* const kFall = "\x97\x8E\x89\xBA";  // è½ä¸‹
    const char* const kBase = "\x8A\xEE\x96\x7B";  // åŸºæœ¬ (the ground blend)
    // Voice lines (with the custom voice, Odyssey's clips).
    const char* const kVoiceRoll = "\x90\xBA\x8F\x52\x82\xE8";  // å£°è¹´ã‚Š
    const char* const kVoiceDive = "\x90\xBA\x83\x70\x83\x93\x83\x60";  // å£°ãƒ‘ãƒ³ãƒ
    const char* const kVoiceHighJump = "\x90\xBA\x91\xE5\x83\x57\x83\x83\x83\x93\x83\x76";  // å£°å¤§ã‚¸ãƒ£ãƒ³ãƒ—
    const char* const kVoiceLand = "\x90\xBA\x92\x85\x92\x6E\x92\xE2\x8E\x7E";  // å£°ç€åœ°åœæ­¢
    // MarioEdit::sounds, by moveset::Sound bit.
    const char* const kSounds[] = {
        "\x91\xAB\x89\xB9\x8D\xB6",  // è¶³éŸ³å·¦
        "\x91\xAB\x89\xB9\x89\x45",  // è¶³éŸ³å³
        "\x8D\xE2\x8A\x8A\x82\xE8",  // å‚æ»‘ã‚Š
        "\x83\x58\x83\x79\x83\x56\x83\x83\x83\x8B\x83\x5F\x83\x62\x83\x56\x83\x85\x8E\xE3",  // ã‚¹ãƒšã‚·ãƒ£ãƒ«ãƒ€ãƒƒã‚·ãƒ¥å¼±
        "\x83\x58\x83\x79\x83\x56\x83\x83\x83\x8B\x83\x5F\x83\x62\x83\x56\x83\x85\x8B\xAD",  // ã‚¹ãƒšã‚·ãƒ£ãƒ«ãƒ€ãƒƒã‚·ãƒ¥å¼·
        "\x95\xC7\x8F\xD5\x93\xCB",  // å£è¡çª
    };
    const char* const kEffectSlideDust = "\x8B\xA4\x92\xCA\x83\x58\x83\x8A\x83\x62\x83\x76\x8D\xE2";  // å…±é€šã‚¹ãƒªãƒƒãƒ—å‚
    const char* const kEffectLandingCloud = "\x8B\xA4\x92\xCA\x92\x85\x92\x6E\x91\xE5";  // å…±é€šç€åœ°å¤§
    const char* const kEffectAirGust = "\x8B\xA4\x92\xCA\x82\xD0\x82\xB1\x82\xA4\x82\xAB\x83\x75\x81\x5B\x83\x58\x83\x67";  // å…±é€šã²ã“ã†ããƒ–ãƒ¼ã‚¹ãƒˆ
    const char* const kEffectBrakePuff = "\x8B\xA4\x92\xCA\x83\x75\x83\x8C\x81\x5B\x83\x4C";  // å…±é€šãƒ–ãƒ¬ãƒ¼ã‚­

    // The dash loops are 60 frames for one stride, played at his speed over Galaxy's full run so the feet stay
    // planted; the dive's roll-out (RollingStandUp, 9 frames) played slower reads as a tumble out of the slide.
    const s16 kDashLoopFrames = 60;
    const s16 kDiveTumbleFrames = 9;
    const f32 kDiveTumbleRate = 0.55f;

    // One moveset per player, kept with the game's own memory (a rollback snapshot carries it).
    struct Slot {
        const Mario* mMario;
        moveset::Moveset mMoveset;
        bool mActive;
    };
    Slot sSlots[2];
    const MarioActor* sSecondPlayer = nullptr;
    u32 sMotionStyle = 0;
    u32 sAppliedMotionStyle = 0;

    moveset::Vec3 toVec(const TVec3f& rVec) {
        moveset::Vec3 v;
        v.x = rVec.x;
        v.y = rVec.y;
        v.z = rVec.z;
        return v;
    }

    TVec3f fromVec(const moveset::Vec3& rVec) {
        return TVec3f(rVec.x, rVec.y, rVec.z);
    }

    // The motion style's fields of Mario's tuning tables (nebula_motion_style.h lists them by offset).
    static_assert(offsetof(MarioConstTable, mTiltRatio) == 0x328 && offsetof(MarioConstTable, mWeightBlendRatio) == 0x354 &&
                      offsetof(MarioConstTable, mInertiaStandardStop) == 0x228 &&
                      offsetof(MarioConstTable, mInertiaStandardMax) == 0x22C &&
                      offsetof(MarioConstTable, mInertiaStop) == 0x238 &&
                      offsetof(MarioConstTable, mTurnAngleSpeed2) == 0x25C &&
                      offsetof(MarioConstTable, mSlowStartTime) == 0x27E,
                  "nebula_motion_style.h's offsets are MarioConstTable's");

    void applyMotionStyle(MarioActor* pActor) {
        sMotionStyle = __NebulaOption(NEBULA_OPTION_MOTION_STYLE);
        if (sMotionStyle > 2) {
            sMotionStyle = 0;
        }
        if (sMotionStyle == sAppliedMotionStyle || pActor->mConst == nullptr) {
            return;
        }
        const galaxy::MotionStyle style = static_cast< galaxy::MotionStyle >(sMotionStyle);
        for (u32 t = 0; t < 2; t++) {
            u8* table = reinterpret_cast< u8* >(pActor->mConst->getTableAt(t));
            if (table == nullptr) {
                continue;
            }
            for (u32 i = 0; i < sizeof(galaxy::kMarioTableFields) / sizeof(galaxy::kMarioTableFields[0]); i++) {
                const galaxy::MarioTableField& rField = galaxy::kMarioTableFields[i];
                f32 value;
                if (rField.halfword) {
                    u16* pField = reinterpret_cast< u16* >(table + rField.offset);
                    if (galaxy::mario_table_value(rField, style, static_cast< f32 >(*pField), value)) {
                        *pField = static_cast< u16 >(value);
                    }
                } else {
                    f32* pField = reinterpret_cast< f32* >(table + rField.offset);
                    if (galaxy::mario_table_value(rField, style, *pField, value)) {
                        *pField = value;
                    }
                }
            }
        }
        sAppliedMotionStyle = sMotionStyle;
    }

    // The current animation's frame control when its length is `frames`, else null.
    XanimeFrameCtrl* frameCtrlOfLength(MarioActor* pActor, s16 frames) {
        if (pActor->mMarioAnim == nullptr || pActor->mMarioAnim->getXanimePlayer() == nullptr) {
            return nullptr;
        }
        XanimeFrameCtrl* pCtrl = pActor->mMarioAnim->getXanimePlayer()->getFrameCtrl();
        return pCtrl != nullptr && pCtrl->getEnd() == frames ? pCtrl : nullptr;
    }
}  // namespace

namespace NebulaOdysseyMoves {
    void setSecondPlayer(const MarioActor* pActor) {
        sSecondPlayer = pActor;
    }

    void shapeInterpole(f32* pRatio, s32* pRemaining) {
        if (sMotionStyle == 0) {
            return;
        }
        const galaxy::AnimationInterpoleStep step =
            galaxy::shape_animation_interpole(static_cast< galaxy::MotionStyle >(sMotionStyle), *pRatio, *pRemaining);
        *pRatio = step.ratio;
        *pRemaining = step.remaining;
    }

    void tick(Mario* pMario) {
        MarioActor* pActor = pMario->mActor;
        if (pActor == nullptr) {
            return;
        }
        applyMotionStyle(pActor);

        const u32 slotIndex = pActor == sSecondPlayer ? 1 : 0;
        Slot& rSlot = sSlots[slotIndex];
        if (rSlot.mMario != pMario) {
            // A new Mario (a new scene): his moves start over.
            rSlot.mMario = pMario;
            rSlot.mMoveset.reset();
            rSlot.mActive = false;
        }
        moveset::Moveset& rMoveset = rSlot.mMoveset;
        const u32 options = __NebulaOption(NEBULA_OPTION_MOVESET);
        const bool enabled = (options & NEBULA_MOVESET_ON) != 0 &&
                             (slotIndex == 0 || (options & NEBULA_MOVESET_SECOND_PLAYER) != 0);
        if (!enabled) {
            if (rSlot.mActive) {
                rMoveset.reset();
                rSlot.mActive = false;
            }
            return;
        }
        rSlot.mActive = true;
        moveset::Options moveOptions;
        moveOptions.roll = (options & NEBULA_MOVESET_ROLL) != 0;
        moveOptions.roll_jump = (options & NEBULA_MOVESET_ROLL_JUMP) != 0;
        moveOptions.ground_pound_jump = (options & NEBULA_MOVESET_GROUND_POUND_JUMP) != 0;
        moveOptions.dive = (options & NEBULA_MOVESET_DIVE) != 0;
        moveOptions.turn_boost = (options & NEBULA_MOVESET_TURN_BOOST) != 0;
        rMoveset.set_options(moveOptions);

        // The game's own answers: input under the player's control (no demo, no event), his mode.
        const u16 mode = pMario->getPlayerMode();
        moveset::MarioView view;
        view.eligible = (mode == 0 || mode == 1) && !pMario->isInputDisable() && pMario->_97C == nullptr;
        view.grounded = pMario->mMovementStates._1;
        view.jumping = pMario->mMovementStates.jumping;
        view.hip_drop = pMario->mMovementStates._B;
        view.squat = pMario->mMovementStates._A;
        view.wall_slide = pMario->mMovementStates._17;
        view.wall_hit = pMario->mMovementStates._8;
        view.braking = pMario->mMovementStates._4 || pMario->_3D0 != 0;
        view.jump_type = pMario->_430;
        view.walk_speed = pMario->mWalkSpeed;
        view.stick_mag = pMario->mStickPos.z;
        view.jump_vec = toVec(pMario->mJumpVec);
        view.front = toVec(pMario->mFrontVec);
        view.stick_dir = toVec(pMario->mWorldPadDir);
        view.down = toVec(pMario->mAirGravityVec);

        if (view.eligible) {
            view.trig_z = pMario->checkTrgZ();
            view.lvl_z = pMario->checkLvlZ();
            // The cap button: Wii 1 pressed this frame. Channel 0 is the player moving (a co-op second player's
            // update runs with his controller there).
            WPad* pPad = MR::getWPad(0);
            view.trig_c = pPad != nullptr && pPad->mButton != nullptr && pPad->mButton->testTrigger1();
            if ((view.jumping && !view.grounded) || rMoveset.rolling()) {
                view.spin_request = pActor->isRequestSpin() || pActor->isRequestRush();
            }
            const MarioConstTable* pTable = pActor->mConst != nullptr ? pActor->mConst->getTable() : nullptr;
            if (pTable != nullptr) {
                view.table.jump_height_high = pTable->mJumpHeight[2];
                view.table.jump_height_small = pTable->mJumpHeight[0];
                view.table.squat_jump_front = pTable->mSquatJumpFrontSpeed;
                view.table.gravity = pTable->mGravityJumping[0];
                view.table.walk_speed = pTable->mWalkSpeed;
            }
            if (rMoveset.diving()) {
                view.dive_anim_running = pMario->isAnimationRun(kDive) || pMario->isAnimationRun(kDiveLand);
            }
            if (rMoveset.rolling()) {
                // Rolling, or still in the roll's start, which hands over to rolling by itself.
                view.roll_anim_running = pMario->isAnimationRun(kRoll) || pMario->isAnimationRun(kRollStart) ||
                                         pMario->isAnimationRun(kPoundRoll);
            }
        }

        const moveset::MarioEdit edit = rMoveset.step(view);

        if (edit.set_walk_speed) {
            pMario->mWalkSpeed = edit.walk_speed;
        }
        if (edit.set_jump_vec) {
            pMario->mJumpVec = fromVec(edit.jump_vec);
        }
        if (edit.set_jump_type) {
            pMario->_430 = edit.jump_type;
        }
        if (edit.end_jump_hold) {
            pMario->mMovementStates._13 = false;
        }
        if (edit.clear_hard_landing) {
            pMario->_10._8 = false;
        }
        if (edit.attack_frames != 0) {
            pActor->setPunchHitTimer(edit.attack_frames);
        }
        if (edit.knock_back) {
            pMario->blown(fromVec(edit.knock_back_dir));
        }
        if (edit.set_front) {
            pMario->setFrontVecKeepUp(fromVec(edit.front));
        }
        if (edit.face_stick) {
            pMario->setFrontVecKeepUp(pMario->mWorldPadDir);
        }
        if (edit.hard_landing) {
            pMario->_10._8 = true;
        }
        if (edit.cancel_squat) {
            pMario->cancelSquatMode();
        }
        if (edit.block_crouch) {
            pMario->mMovementStates._A = false;
            pMario->mMovementStates._21 = true;
        }
        if (edit.block_air_actions) {
            pActor->_935 = true;
        }
        if (edit.end_hip_drop_air) {
            pMario->mMovementStates._B = false;
            pMario->_424 = 0;
        }
        if (edit.end_hip_drop) {
            pMario->mMovementStates.jumping = false;
            pMario->mMovementStates._B = false;
            pMario->mJumpVec.zero();
        }
        if (edit.stop_belly_slide) {
            const char* const names[] = {kDiveLand, kDiveTumble};
            for (u32 i = 0; i < 2; i++) {
                if (pMario->isAnimationRun(names[i])) {
                    pMario->stopAnimation(names[i], kBase);
                }
            }
        }
        for (u32 i = 0; i < sizeof(kSounds) / sizeof(kSounds[0]); i++) {
            if ((edit.sounds & (1u << i)) != 0) {
                pMario->playSound(kSounds[i], -1);
            }
        }
        if (edit.set_trail) {
            if (edit.trail) {
                pMario->playEffect(kEffectSlideDust);
            } else {
                pMario->stopEffect(kEffectSlideDust);
            }
        }
        if (edit.air_gust) {
            pMario->playEffect(kEffectAirGust);
        }
        if (edit.dust_puff) {
            pMario->playEffect(kEffectBrakePuff);
        }
        if (edit.dust_landing) {
            pMario->playEffect(kEffectLandingCloud);
        }
        if (edit.set_run_anim) {
            // Dash and DashFast play as Mario's main animation (the walk blend's run track never shows them);
            // the blend is the game's own walk and run.
            const moveset::RunAnim runAnim = static_cast< moveset::RunAnim >(edit.run_anim);
            const char* want = runAnim == moveset::RunAnim::DashFast ? kDash
                               : runAnim == moveset::RunAnim::Dash   ? kDashSlow
                                                                     : nullptr;
            const char* const dashes[] = {kDashSlow, kDash};
            for (u32 i = 0; i < 2; i++) {
                if (dashes[i] != want && pMario->isAnimationRun(dashes[i])) {
                    pMario->stopAnimation(dashes[i], kBase);
                }
            }
            if (want != nullptr) {
                if (!pMario->isAnimationRun(want)) {
                    pMario->changeAnimation(want, static_cast< const char* >(nullptr));
                }
                if (XanimeFrameCtrl* pCtrl = frameCtrlOfLength(pActor, kDashLoopFrames)) {
                    pCtrl->setRate(moveset::clampf(rMoveset.fast_run_speed() / view.table.walk_speed, 0.5f, 3.0f));
                }
            }
        }
        if (edit.anim != moveset::Anim::None || edit.stop_roll_anim) {
            if (edit.stop_roll_anim) {
                // Every animation of the roll goes, back to the ground blend (Galaxy picks the crouch from there
                // if Z is still held).
                const char* const rolls[] = {kRoll, kRollStart, kPoundRoll};
                for (u32 i = 0; i < 3; i++) {
                    if (pMario->isAnimationRun(rolls[i])) {
                        pMario->stopAnimation(rolls[i], kBase);
                    }
                }
            }
            // The animation, and the one the game plays when it ends (null: the game decides).
            const char* name = nullptr;
            const char* next = nullptr;
            switch (edit.anim) {
            // The lunge ends flat on the belly; the belly-slide loop holds that pose until he lands.
            case moveset::Anim::Dive:
                name = kDive;
                next = kDiveLand;
                break;
            case moveset::Anim::HighJump:
                name = kGroundPoundJump;
                next = kFall;
                break;
            case moveset::Anim::Roll:
                name = kRoll;
                break;
            // The roll's start hands over to the rolling loop through the roll's own check (the loop is started
            // whenever it is not playing), not as the game's "next": that would bring the roll back after
            // anything else that played over it, such as the spin of a roll cancel.
            case moveset::Anim::RollStart:
            case moveset::Anim::RollBoost:
                name = kRollStart;
                break;
            case moveset::Anim::RollJump:
                name = kRollJump;
                next = kFall;
                break;
            case moveset::Anim::DashJump:
                name = kDashJump;
                next = kFall;
                break;
            case moveset::Anim::DiveHold:
                name = kDiveLand;
                break;
            case moveset::Anim::RollBump:
                name = kWallBump;
                next = kBase;
                break;
            case moveset::Anim::DiveLand:
                name = kDiveTumble;
                next = kBase;
                break;
            case moveset::Anim::GroundPoundRoll:
                name = kPoundRoll;
                break;
            case moveset::Anim::RollLand:
                name = kRollLand;
                next = kBase;
                break;
            // Getting up out of a roll is the same tumble up as out of a dive (Odyssey's RollingStandUp).
            case moveset::Anim::RollEnd:
                name = kDiveTumble;
                next = kBase;
                break;
            case moveset::Anim::None:
                break;
            }
            if (name != nullptr) {
                pMario->changeAnimation(name, next);
            }
            if (edit.anim == moveset::Anim::DiveLand && name != nullptr) {
                if (XanimeFrameCtrl* pCtrl = frameCtrlOfLength(pActor, kDiveTumbleFrames)) {
                    pCtrl->setRate(kDiveTumbleRate);
                }
            }
            const char* voice = nullptr;
            switch (edit.anim) {
            case moveset::Anim::RollStart:
            case moveset::Anim::GroundPoundRoll:
                voice = kVoiceRoll;
                break;
            case moveset::Anim::Dive:
                voice = kVoiceDive;
                break;
            case moveset::Anim::HighJump:
                voice = kVoiceHighJump;
                break;
            case moveset::Anim::RollLand:
                voice = kVoiceLand;
                break;
            default:
                break;
            }
            if (voice != nullptr) {
                pMario->playSound(voice, -1);
            }
            // The roll's dust: a puff where it starts.
            if (edit.anim == moveset::Anim::RollStart || edit.anim == moveset::Anim::GroundPoundRoll) {
                pMario->playEffect(kEffectBrakePuff);
            }
        }
    }
};  // namespace NebulaOdysseyMoves
#endif
