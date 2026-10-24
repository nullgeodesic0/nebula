#include "Game/Player/NebulaCoop.hpp"

#ifdef TARGET_PC
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Player/MarioHolder.hpp"
#include "Game/Scene/GameSceneFunction.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/WPad.hpp"
#include "Game/System/WPadHolder.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <nebula_coop.h>
#include <nebula_game_api.h>
#include <revolution/os.h>

namespace coop = galaxy::coop;

extern bool gIsLuigi;

namespace {
    // The scene's two players and the second player's frame state. Kept with the game's memory (a rollback
    // snapshot carries it).
    struct State {
        MarioActor* mFirst;
        MarioActor* mSecond;
        bool mOwnIsLuigi;      // the save's character, the first player's
        bool mMaking;          // the second player's init2 runs
        bool mPresent;         // in play (his controller connected), else sitting out
        bool mPresentKnown;
        bool mLevelStart;      // until the first player has landed and has control
        u32 mControllableFrames;
        bool mTravelling;      // carried beside the first (a level start, the first's controls held)
        bool mHeldHere;
        TVec3f mHeldAt;
        TVec3f mParked;
        bool mHidden;          // hidden and untouchable (a level start, sitting out)
        coop::LeashTimer mLeash;
    };
    State sState;

    WPadHolder* padHolder() {
        GameSystem* pSystem = SingletonHolder< GameSystem >::get();
        return pSystem != nullptr && pSystem->mObjHolder != nullptr ? pSystem->mObjHolder->mWPadHolder : nullptr;
    }

    void swapPads() {
        WPadHolder* pHolder = padHolder();
        if (pHolder != nullptr) {
            WPad* pPad = pHolder->mPad[0];
            pHolder->mPad[0] = pHolder->mPad[1];
            pHolder->mPad[1] = pPad;
        }
    }

    // The second player's controller is in: the host connects channel 1 while a second player plays.
    bool secondConnected() {
        WPadHolder* pHolder = padHolder();
        return pHolder != nullptr && pHolder->mPad[1] != nullptr && pHolder->mPad[1]->mIsConnected;
    }

    coop::Vec3 toVec(const TVec3f& rVec) {
        coop::Vec3 v;
        v.x = rVec.x;
        v.y = rVec.y;
        v.z = rVec.z;
        return v;
    }

    TVec3f fromVec(const coop::Vec3& rVec) {
        return TVec3f(rVec.x, rVec.y, rVec.z);
    }

    // Where he stands beside the first player.
    TVec3f besideFirst() {
        Mario* pFirstMario = sState.mFirst->mMario;
        return fromVec(coop::travel_position(toVec(sState.mFirst->mPosition), toVec(pFirstMario->mSideVec),
                                             toVec(sState.mParked)));
    }

    void setHidden(bool hidden) {
        if (hidden == sState.mHidden) {
            return;
        }
        sState.mHidden = hidden;
        MarioActor* pSecond = sState.mSecond;
        if (hidden) {
            MR::hideModel(pSecond);
            MR::invalidateHitSensors(pSecond);
        } else {
            MR::showModel(pSecond);
            MR::validateHitSensors(pSecond);
        }
        pSecond->_482 = hidden;
    }

    // The players do not pass through each other: each is pushed half the overlap apart (Mario::push keeps walls
    // and the ground in charge).
    void bumpPlayers() {
        const coop::Vec3 up = coop::normalized(toVec(sState.mFirst->mMario->_1FC), coop::Vec3{0.0f, 1.0f, 0.0f});
        const coop::Vec3 push = coop::body_push(toVec(sState.mFirst->mPosition), toVec(sState.mSecond->mPosition), up);
        if (push.x == 0.0f && push.y == 0.0f && push.z == 0.0f) {
            return;
        }
        sState.mSecond->mMario->push(fromVec(push));
        sState.mFirst->mMario->push(fromVec(push * -1.0f));
    }

    // The second player's turn: the game's player code runs for him, with his character and controller.
    void secondTurn() {
        MarioActor* pFirst = sState.mFirst;
        MarioActor* pSecond = sState.mSecond;
        // Kept out of the game's movement list (a galaxy's start resumes the scene's suspended objects, him with
        // them, and the game would move him a second time with the first player's controller).
        pSecond->requestSuspend();
        pSecond->syncWithFlags();

        const bool present = secondConnected();
        if (!sState.mPresentKnown || present != sState.mPresent) {
            sState.mPresentKnown = true;
            sState.mPresent = present;
            OSReport("coop: second player %s\n", present ? "in" : "out");
            if (present) {
                // Drop in: beside the first player, full health.
                sState.mTravelling = true;
                pSecond->mHealth = pSecond->mMaxHealth;
            }
        }

        // While the game has the first player's controls or a cutscene plays, the second player waits where he
        // stands; at a level start he waits unseen until the first has landed and has control.
        const bool firstHeld = pFirst->mMario->isInputDisable();
        const bool demo = MR::isDemoActive() || MR::isTimeKeepDemoActive();
        const bool arriving = sState.mLevelStart && (GameSceneFunction::isExecScenarioOpeningCamera() || MR::isEventCameraActive());
        sState.mControllableFrames = firstHeld || demo || arriving ? 0 : sState.mControllableFrames + 1;
        bool landBeside = false;
        if (sState.mLevelStart && sState.mControllableFrames >= coop::kLevelStartSettleFrames) {
            sState.mLevelStart = false;
            landBeside = true;
        }
        sState.mTravelling = coop::keep_travelling(sState.mTravelling, sState.mLevelStart, sState.mControllableFrames);
        const bool holdHere = !sState.mTravelling && coop::second_player_waits(firstHeld, demo);
        if (holdHere && !sState.mHeldHere) {
            sState.mHeldAt = pSecond->mPosition;
        }
        sState.mHeldHere = holdHere;

        setHidden(!sState.mPresent || sState.mLevelStart);

        // His turn.
        MarioHolder* pHolder = MR::getMarioHolder();
        const bool ownIsLuigi = sState.mOwnIsLuigi;
        gIsLuigi = !ownIsLuigi;
        pHolder->setMarioActor(pSecond);
        swapPads();
        if (sState.mPresent) {
            pSecond->movement();
        }
        // MR::setPlayerPos moves the scene's player: him, in his turn.
        if (!sState.mPresent || sState.mTravelling || landBeside) {
            MR::setPlayerPos(sState.mLevelStart ? sState.mParked : besideFirst());
        } else if (holdHere) {
            MR::setPlayerPos(sState.mHeldAt);
        } else {
            sState.mParked = pSecond->mPosition;
            // Too far for one screen for two seconds: brought back (until split screen).
            const coop::CameraShare share = coop::shared_camera(toVec(pFirst->mPosition), toVec(pSecond->mPosition));
            if (sState.mLeash.update(share.out_of_range, coop::CameraTuning().warp_frames)) {
                MR::setPlayerPos(besideFirst());
                OSReport("coop: second player brought back to the first\n");
            }
        }
        swapPads();
        pHolder->setMarioActor(pFirst);
        gIsLuigi = ownIsLuigi;

        if (!sState.mPresent || sState.mLevelStart) {
            sState.mLeash.reset();
            return;
        }
        bumpPlayers();
        // The game camera looks at the first player's camera point: moved towards the second so both stay in view.
        const coop::CameraShare share = coop::shared_camera(toVec(pFirst->mPosition), toVec(pSecond->mPosition));
        pFirst->mCameraTrans += fromVec(share.offset);
    }
}  // namespace

namespace NebulaCoop {
    bool isAvailable() {
        return __NebulaOption(NEBULA_OPTION_COOP) != 0;
    }

    bool isMakingSecond(bool* pIsLuigi) {
        if (!sState.mMaking) {
            return false;
        }
        *pIsLuigi = !sState.mOwnIsLuigi;
        return true;
    }

    MarioActor* getSecond() {
        return sState.mSecond;
    }

    void afterPlayerInit(MarioActor* pActor) {
        if (sState.mMaking || !isAvailable()) {
            return;
        }
        // A new scene's player: its second player is made beside him.
        sState.mFirst = pActor;
        sState.mSecond = nullptr;
        sState.mOwnIsLuigi = gIsLuigi;
        sState.mLevelStart = true;
        sState.mControllableFrames = 0;
        sState.mTravelling = true;
        sState.mHeldHere = false;
        sState.mHidden = false;
        sState.mLeash.reset();
        sState.mPresentKnown = false;
        const TVec3f side = pActor->mMario->mSideVec;
        sState.mParked = pActor->mPosition + side * 120.0f;

        sState.mMaking = true;
        MarioActor* pSecond = new MarioActor(sState.mOwnIsLuigi ? "Mario" : "Luigi");
        pSecond->init2(sState.mParked, pActor->mRotation, 1);  // 1: 基本
        sState.mMaking = false;
        // init2 made the new actor the scene's player, with his character: back to the first.
        gIsLuigi = sState.mOwnIsLuigi;
        MR::getMarioHolder()->setMarioActor(pActor);
        MR::setGameCameraTargetToPlayer();
        pSecond->requestSuspend();
        sState.mSecond = pSecond;
        OSReport("coop: players: first %s, second %s\n", sState.mOwnIsLuigi ? "Luigi" : "Mario",
                 sState.mOwnIsLuigi ? "Mario" : "Luigi");
    }

    void afterMovement(int category) {
        if (category != MR::MovementType_Player || sState.mSecond == nullptr) {
            return;
        }
        if (MR::getMarioHolder()->getMarioActor() != sState.mFirst) {
            // Another scene's player: this second player went with his scene.
            sState.mFirst = nullptr;
            sState.mSecond = nullptr;
            return;
        }
        secondTurn();
    }
};  // namespace NebulaCoop
#endif
