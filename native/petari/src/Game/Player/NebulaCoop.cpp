#include "Game/Player/NebulaCoop.hpp"

#ifdef TARGET_PC
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Camera/CameraContext.hpp"
#include "Game/Map/HitInfo.hpp"
#include "Game/Player/MarioHolder.hpp"
#include "Game/Scene/SceneObjHolder.hpp"
#include "Game/Scene/GameSceneFunction.hpp"
#include "Game/Scene/SceneFunction.hpp"
#include "Game/System/GameSystem.hpp"
#include "Game/System/GameSystemObjHolder.hpp"
#include "Game/System/WPad.hpp"
#include "Game/System/WPadHolder.hpp"
#include "Game/Util/ActorSensorUtil.hpp"
#include "Game/Util/CameraUtil.hpp"
#include "Game/Util/DemoUtil.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/System/GameDataFunction.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include <nebula_coop.h>
#include <nebula_game_api.h>
#include <revolution/os.h>
#include <cstring>

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
        u32 mHeldFrames;       // how long the game last had the first player's controls
        bool mTravelling;      // carried beside the first (a level start, the first's controls held)
        bool mHeldHere;
        TVec3f mHeldAt;
        TVec3f mParked;
        bool mHidden;          // hidden and untouchable (a level start, sitting out, down)
        coop::DownState mDown; // fell while the first plays on: back beside him after a while, for a life
        u32 mTurns;            // his turns in play this scene (GALAXY_TEST_DOWN_SECOND)
        // Split screen: how far split, the line between the halves, the second player's own camera.
        coop::SplitState mSplit;
        coop::SplitLayout mLayout;
        coop::FollowCamera mFollow;
        coop::CameraMatrices mFollowView;
        // While the scene draws split: the pass (0 none, 1 the first player's half, 2 the second's) and the game
        // camera it restores after.
        u32 mPass;
        coop::CameraMatrices mGameCamera;
        coop::Array< float, 16 > mGameProjection;
    };
    State sState;

    // A trip of the first player's (his controls held this long) that ends this far from the second brings the
    // second to him.
    const u32 kCatchUpHeldFrames = 30;
    const f32 kCatchUpDistance = 3000.0f;

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

    CameraContext* cameraContext() {
        return MR::getSceneObj< CameraContext >(SceneObj_CameraContext);
    }

    coop::CameraMatrices readCamera(const CameraContext* pContext) {
        coop::CameraMatrices camera;
        memcpy(&camera.view[0], &pContext->mView, sizeof(float) * 12);
        memcpy(&camera.inverse[0], &pContext->mViewInv, sizeof(float) * 12);
        return camera;
    }

    void writeCamera(CameraContext* pContext, const coop::CameraMatrices& rCamera) {
        memcpy(&pContext->mView, &rCamera.view[0], sizeof(float) * 12);
        memcpy(&pContext->mViewInv, &rCamera.inverse[0], sizeof(float) * 12);
    }

    bool splitShown() {
        return sState.mSplit.amount() > 0.0f && sState.mFollow.started() && sState.mLayout.started();
    }

    // After both players moved: whether the screen splits (a player about to leave the shared picture, or far
    // apart), the line between the halves (square to where the second player is on screen) and his own camera.
    void updateSplit(CameraContext* pContext, const TVec3f& rFirst, const TVec3f& rSecond) {
        const coop::CameraMatrices camera = readCamera(pContext);
        const coop::Vec3 first = toVec(rFirst);
        const coop::Vec3 second = toVec(rSecond);
        const coop::Vec3 middle = (first + second) * 0.5f;
        const float depth = coop::length(coop::pose_of(camera.inverse).eye - middle);
        const float extent = coop::shared_view_extent(camera, first, second, depth, pContext->mFovy,
                                                      pContext->mAspectRatio);
        if (sState.mSplit.update(extent, coop::length(second - first)) == 0.0f) {
            sState.mFollow.reset();
            sState.mLayout.reset();
            return;
        }
        const coop::Vec3 up = toVec(sState.mSecond->mMario->getUpVec());
        if (!sState.mFollow.started()) {
            sState.mFollow.start(coop::pose_of(camera.inverse).eye, first, second, up);
            OSReport("coop: split begins\n");
        }
        // His camera controls (channel 1): the D-pad turns it around him and raises or lowers it, C puts it behind him.
        const coop::SplitTuning tuning;
        if (MR::testSubPadButtonC(1)) {
            sState.mFollow.recentre(second, toVec(sState.mSecond->mMario->getFrontVec()), tuning);
        }
        const float turn = (MR::testCorePadButtonRight(1) ? 0.04f : 0.0f) - (MR::testCorePadButtonLeft(1) ? 0.04f : 0.0f);
        const float raise = (MR::testCorePadButtonUp(1) ? 12.0f : 0.0f) - (MR::testCorePadButtonDown(1) ? 12.0f : 0.0f);
        sState.mFollow.steer(turn, raise, tuning);
        sState.mFollowView = sState.mFollow.update(second, up, tuning);
        // Kept in front of the first wall between the point it looks at and its eye.
        const coop::Vec3 look = sState.mFollow.look();
        const coop::Vec3 eye = sState.mFollow.eye();
        TVec3f hit;
        Triangle triangle;
        if (MR::getFirstPolyOnLineToMap(&hit, &triangle, fromVec(look), fromVec(eye - look))) {
            const coop::Vec3 wall = toVec(hit);
            if (coop::finitef(coop::length(wall))) {
                sState.mFollowView = coop::look_at(coop::camera_before_wall(look, wall, tuning.wall_margin),
                                                   sState.mFollow.up(), look);
            }
        }
        // On the first player's screen (y down): the second player from the first.
        const coop::Vec3 d = second - first;
        sState.mLayout.update(camera.view[0] * d.x + camera.view[1] * d.y + camera.view[2] * d.z,
                              -(camera.view[4] * d.x + camera.view[5] * d.y + camera.view[6] * d.z));
    }

    // A half's projection: the game's, its picture moved to the middle of the half as the split eases in.
    void setHalfProjection(CameraContext* pContext, bool second) {
        coop::Array< float, 16 > projection = sState.mGameProjection;
        const coop::Array< float, 2 > centre = sState.mLayout.centre(second, MR::getFrameBufferWidth(),
                                                                    MR::getFrameBufferHeight());
        coop::split_projection(projection, centre[0], centre[1], sState.mSplit.eased());
        memcpy(&pContext->mProjection, &projection[0], sizeof(float) * 16);
    }

    void sendSplitClip(bool second) {
        const coop::Array< float, 3 > plane = sState.mLayout.plane(second, MR::getFrameBufferWidth(),
                                                                   MR::getFrameBufferHeight());
        __NebulaGXSplitClip(&plane[0], second ? 2 : 1, nullptr);
    }

    // The game's own fall (a miss): the player's game-over nerves, set when he runs out of life, falls into
    // the abyss, burns, sinks or is pulled in.
    bool isGoingDown(MarioActor* pActor) {
        return pActor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOver)) ||
               pActor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverAbyss)) ||
               pActor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverAbyss2)) ||
               pActor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverFire)) ||
               pActor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverBlackHole)) ||
               pActor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverNonStop)) ||
               pActor->isNerve(GET_NERVE(MarioActor, MarioActorNrvGameOverSink));
    }

    // The second player went down while the first plays on: his game-over (which would start the scene's miss
    // for both) never runs; he is out of the picture for a while and comes back beside the first, for a life.
    // With no lives left he sits out until the stage reloads.
    void goDown(MarioActor* pSecond) {
        int lives = GameDataFunction::getPlayerLeft();
        const coop::DownState first{};
        (void)coop::go_down_alone(sState.mDown, first, lives);
        if (lives < GameDataFunction::getPlayerLeft()) {
            GameDataFunction::addPlayerLeft(-1);
        }
        pSecond->setNerve(GET_NERVE(MarioActor, MarioActorNrvWait));
        OSReport("coop: second player down (%s)\n", sState.mDown.out ? "no lives left" : "back soon");
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
        // The first player taken somewhere far while the game had his controls (a launch star, a pipe, a warp):
        // the second joins him there once he has landed and has control again.
        if (sState.mControllableFrames == 0) {
            sState.mHeldFrames++;
        } else if (sState.mControllableFrames == coop::kLevelStartSettleFrames) {
            if (sState.mHeldFrames >= kCatchUpHeldFrames && sState.mPresent && !sState.mDown.down &&
                PSVECDistance(&pFirst->mPosition, &pSecond->mPosition) > kCatchUpDistance) {
                landBeside = true;
                OSReport("coop: second player joins the first after his trip\n");
            }
            sState.mHeldFrames = 0;
        }
        sState.mTravelling = coop::keep_travelling(sState.mTravelling, sState.mLevelStart, sState.mControllableFrames);
        const bool holdHere = !sState.mTravelling && coop::second_player_waits(firstHeld, demo);
        if (holdHere && !sState.mHeldHere) {
            sState.mHeldAt = pSecond->mPosition;
        }
        sState.mHeldHere = holdHere;

        // Down: back beside the first after a while.
        bool comeBack = false;
        if (sState.mDown.down) {
            const coop::DownState first{};
            comeBack = coop::respawn_due(sState.mDown, first);
            if (comeBack) {
                pSecond->mHealth = pSecond->mMaxHealth;
                OSReport("coop: second player back\n");
            }
        }
        const bool down = sState.mDown.down;
        setHidden(!sState.mPresent || sState.mLevelStart || down);

        // His turn.
        MarioHolder* pHolder = MR::getMarioHolder();
        const bool ownIsLuigi = sState.mOwnIsLuigi;
        gIsLuigi = !ownIsLuigi;
        pHolder->setMarioActor(pSecond);
        swapPads();
        if (sState.mPresent && !down) {
            // Split, his stick is relative to his own camera: the player code reads the scene's camera.
            CameraContext* pContext = splitShown() ? cameraContext() : nullptr;
            coop::CameraMatrices gameCamera;
            if (pContext != nullptr) {
                gameCamera = readCamera(pContext);
                writeCamera(pContext, coop::blend_cameras(gameCamera, sState.mFollowView, sState.mSplit.eased()));
            }
            pSecond->movement();
            if (pContext != nullptr) {
                writeCamera(pContext, gameCamera);
            }
            if (!sState.mLevelStart) {
                sState.mTurns++;
                const u32 testDown = __NebulaOption(NEBULA_OPTION_TEST_DOWN_SECOND);
                if (testDown != 0 && sState.mTurns == testDown) {
                    pSecond->forceKill(6);  // any type past 5: the plain game-over fall
                }
            }
            if (isGoingDown(pSecond)) {
                goDown(pSecond);
            }
        }
        // MR::setPlayerPos moves the scene's player: him, in his turn.
        if (comeBack) {
            // Back from a fall: beside the first, his state as at a fresh start.
            MR::setPlayerPosAndWait(besideFirst());
        } else if (!sState.mPresent || sState.mTravelling || landBeside || down) {
            MR::setPlayerPos(sState.mLevelStart ? sState.mParked : besideFirst());
        } else if (holdHere) {
            MR::setPlayerPos(sState.mHeldAt);
        } else {
            sState.mParked = pSecond->mPosition;
        }
        swapPads();
        pHolder->setMarioActor(pFirst);
        gIsLuigi = ownIsLuigi;

        if (!sState.mPresent || sState.mLevelStart || down) {
            sState.mSplit.reset();
            sState.mFollow.reset();
            sState.mLayout.reset();
            return;
        }
        bumpPlayers();
        CameraContext* pContext = cameraContext();
        if (pContext == nullptr || holdHere || sState.mTravelling) {
            // One screen while the game holds the players (a cutscene, a level's opening): the split eases out.
            if (sState.mSplit.update(0.0f, 0.0f) == 0.0f) {
                sState.mFollow.reset();
                sState.mLayout.reset();
            }
        } else {
            updateSplit(pContext, pFirst->mPosition, pSecond->mPosition);
        }
        // The game camera looks at the first player's camera point: moved towards the second so both stay in view,
        // easing back to the first player as the screen splits.
        const TVec3f apart = pSecond->mPosition - pFirst->mPosition;
        pFirst->mCameraTrans += apart * (0.5f * (1.0f - sState.mSplit.eased()));
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
        sState.mHeldFrames = 0;
        sState.mTravelling = true;
        sState.mHeldHere = false;
        sState.mHidden = false;
        sState.mDown = coop::DownState{};
        sState.mTurns = 0;
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

    bool beginSplitDraw() {
        if (sState.mPass != 0 || sState.mSecond == nullptr || !sState.mPresent || !splitShown()) {
            return false;
        }
        CameraContext* pContext = cameraContext();
        if (pContext == nullptr) {
            return false;
        }
        sState.mGameCamera = readCamera(pContext);
        memcpy(&sState.mGameProjection[0], &pContext->mProjection, sizeof(float) * 16);
        sState.mPass = 1;
        setHalfProjection(pContext, false);
        sendSplitClip(false);
        return true;
    }

    void nextSplitPass() {
        CameraContext* pContext = cameraContext();
        sState.mPass = 2;
        // His half: the first player's view panning to his own camera as the split eases in.
        writeCamera(pContext, coop::blend_cameras(sState.mGameCamera, sState.mFollowView, sState.mSplit.eased()));
        setHalfProjection(pContext, true);
        MR::loadViewMtx();
        SceneFunction::executeCalcViewAndEntryList();
        sendSplitClip(true);
    }

    void endSplitDraw() {
        CameraContext* pContext = cameraContext();
        // The whole screen again for the 2D layer, and the line between the halves fading in with the split.
        const coop::Array< float, 3 > line = sState.mLayout.plane(true, MR::getFrameBufferWidth(),
                                                                  MR::getFrameBufferHeight());
        const float none[3] = {0.0f, 0.0f, 0.0f};
        const float separator[4] = {line[0], line[1], line[2], sState.mSplit.eased()};
        __NebulaGXSplitClip(none, 0, separator);
        writeCamera(pContext, sState.mGameCamera);
        memcpy(&pContext->mProjection, &sState.mGameProjection[0], sizeof(float) * 16);
        MR::loadViewMtx();
        sState.mPass = 0;
        MR::setDefaultViewportAndScissor();
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
