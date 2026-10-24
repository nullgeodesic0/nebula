#include "Game/Player/NebulaCoop.hpp"

#ifdef TARGET_PC
#include "Game/Player/Mario.hpp"
#include "Game/Player/MarioActor.hpp"
#include "Game/Camera/CameraContext.hpp"
#include "Game/Map/HitInfo.hpp"
#include "Game/NameObj/NameObjHolder.hpp"
#include "Game/System/GameSystemSceneController.hpp"
#include "Game/LiveActor/HitSensor.hpp"
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
#include "Game/Util/ActorShadowUtil.hpp"
#include "Game/Screen/GameSceneLayoutHolder.hpp"
#include "Game/Screen/MarioMeter.hpp"
#include "Game/Screen/MeterLayout.hpp"
#include "Game/Util/DrawUtil.hpp"
#include "Game/Util/LayoutUtil.hpp"
#include "Game/Util/ObjUtil.hpp"
#include <nw4r/lyt/pane.h>
#include "Game/Util/EventUtil.hpp"
#include "Game/Util/EffectUtil.hpp"
#include "Game/Util/GamePadUtil.hpp"
#include "Game/Util/LiveActorUtil.hpp"
#include "Game/Util/MapUtil.hpp"
#include "Game/Util/PlayerUtil.hpp"
#include "Game/Util/ScreenUtil.hpp"
#include "Game/System/GameDataFunction.hpp"
#include "Game/Util/SceneUtil.hpp"
#include "Game/Util/SingletonHolder.hpp"
#include "Game/Util/StringUtil.hpp"
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
        // The second player's own event (his launch star, pipe, warp): the game camera is his, and the first
        // player's half follows him with this camera meanwhile.
        bool mSecondEvent;
        coop::FollowCamera mFirstFollow;
        coop::CameraMatrices mFirstFollowView;
        // Where the second player stood when the first player's trip began (the catch-up after it).
        TVec3f mTripStart;
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
    const f32 kCatchUpStillDistance = 300.0f;

    // Tests: the virtual tables of the kinds of objects the tests put players on (NEBULA_TEST_PUT_*), noted as they
    // are made.
    const void* sTestClasses[NEBULA_TEST_PUT_KINDS];
    const char* const sTestKindNames[NEBULA_TEST_PUT_KINDS] = {"launch star", "pole", "Star Ball", "manta ray"};

    struct NearestStar {
        TVec3f mFrom;
        LiveActor* mStar;
        f32 mDistance;
        u32 mKind;
        u32 mCount;
    };

    void considerStar(NameObj* pObj, void* pData) {
        NearestStar* pNearest = static_cast< NearestStar* >(pData);
        if (pObj == nullptr || sTestClasses[pNearest->mKind] == nullptr ||
            *reinterpret_cast< const void* const* >(pObj) != sTestClasses[pNearest->mKind]) {
            return;
        }
        pNearest->mCount++;
        LiveActor* pStar = static_cast< LiveActor* >(pObj);
        const f32 distance = PSVECDistance(&pStar->mPosition, &pNearest->mFrom);
        if (!MR::isDead(pStar) && !MR::isHiddenModel(pStar) && distance < pNearest->mDistance) {
            pNearest->mStar = pStar;
            pNearest->mDistance = distance;
        }
    }

    // The scene's nearest object in play of a kind (NEBULA_TEST_PUT_*) to a point; null when it has none.
    LiveActor* findNearest(u32 kind, const TVec3f& rFrom) {
        NameObjHolder* pHolder = SingletonHolder< GameSystem >::get()->mSceneController->mObjHolder;
        NearestStar nearest = {rFrom, nullptr, 1.0e9f, kind, 0};
        if (pHolder != nullptr) {
            pHolder->forEach(considerStar, &nearest);
        }
        OSReport("coop test: %u %s(s) in this scene\n", nearest.mCount, sTestKindNames[kind]);
        return nearest.mStar;
    }

    // GALAXY_TEST_LAUNCH_STAR: the first player put on the scene's nearest launch star in play (a shake fires it).
    void testLaunchStar() {
        LiveActor* pStar = findNearest(NEBULA_TEST_PUT_LAUNCH_STAR, sState.mFirst->mPosition);
        if (pStar == nullptr) {
            return;
        }
        OSReport("coop test: the first player to the launch star at %.0f %.0f %.0f\n", pStar->mPosition.x,
                 pStar->mPosition.y, pStar->mPosition.z);
        MR::setPlayerPos(pStar->mPosition);
    }

    // GALAXY_TEST_PUT: a player put at the scene's nearest object of a kind (call outside the second's turn; the
    // second's own placement runs in his turn). Returns the object.
    LiveActor* testPutTarget(u32 kind, MarioActor* pPlayer) {
        LiveActor* pObj = findNearest(kind, pPlayer->mPosition);
        if (pObj != nullptr) {
            OSReport("coop test: the %s player to the %s at %.0f %.0f %.0f\n", pPlayer == sState.mFirst ? "first" : "second",
                     sTestKindNames[kind], pObj->mPosition.x, pObj->mPosition.y, pObj->mPosition.z);
        }
        return pObj;
    }

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
    void updateSplit(CameraContext* pContext, const TVec3f& rFirst, const TVec3f& rSecond, bool soloEvent) {
        const coop::CameraMatrices camera = readCamera(pContext);
        const coop::Vec3 first = toVec(rFirst);
        const coop::Vec3 second = toVec(rSecond);
        const coop::Vec3 middle = (first + second) * 0.5f;
        const float depth = coop::length(coop::pose_of(camera.inverse).eye - middle);
        const float extent = coop::shared_view_extent(camera, first, second, depth, pContext->mFovy,
                                                      pContext->mAspectRatio);
        // One brother's own event (a launch star, a pipe, a warp) splits the screen even with the two together, so
        // the other keeps his own view.
        if (sState.mSplit.update(soloEvent ? HUGE_VALF : extent, coop::length(second - first)) == 0.0f) {
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

    // The game's player is the second for a while (his turn, an object he is bound to): his character, his
    // MarioActor as the MarioHolder's and his controller as channel 0's.
    void enterSecond() {
        gIsLuigi = !sState.mOwnIsLuigi;
        MR::getMarioHolder()->setMarioActor(sState.mSecond);
        swapPads();
    }

    void leaveSecond() {
        swapPads();
        MR::getMarioHolder()->setMarioActor(sState.mFirst);
        gIsLuigi = sState.mOwnIsLuigi;
    }

    // The object a player rides or is bound to (his rush partner), or null.
    const LiveActor* riddenBy(const MarioActor* pActor) {
        return pActor != nullptr && pActor->_934 && pActor->_924 != nullptr ? pActor->_924->mHost : nullptr;
    }

    // A vehicle (the Star Ball, the manta ray), told by its class (Tamakoro::init and SurfRay::init note them; the
    // objects' names are their placement's, in Japanese).
    bool isVehicle(const LiveActor* pActor) {
        if (pActor == nullptr) {
            return false;
        }
        const void* const pClass = *reinterpret_cast< const void* const* >(pActor);
        return (sTestClasses[NEBULA_TEST_PUT_STAR_BALL] != nullptr && pClass == sTestClasses[NEBULA_TEST_PUT_STAR_BALL]) ||
               (sTestClasses[NEBULA_TEST_PUT_RAY] != nullptr && pClass == sTestClasses[NEBULA_TEST_PUT_RAY]);
    }

    // The first player rides a vehicle: the second goes along unseen at his side.
    bool firstOnVehicle() {
        return isVehicle(riddenBy(sState.mFirst));
    }

    void setHidden(bool hidden) {
        if (hidden == sState.mHidden) {
            return;
        }
        sState.mHidden = hidden;
        MarioActor* pSecond = sState.mSecond;
        // Hidden, he is inert: no model, shadow, effects or sensors (nothing hits him, he collects nothing).
        if (hidden) {
            MR::hideModel(pSecond);
            MR::invalidateHitSensors(pSecond);
            if (pSecond->mShadowControllerList != nullptr) {
                MR::invalidateShadowAll(pSecond);
            }
            MR::forceDeleteEffectAll(pSecond);
        } else {
            MR::showModel(pSecond);
            MR::validateHitSensors(pSecond);
            if (pSecond->mShadowControllerList != nullptr) {
                MR::validateShadowAll(pSecond);
            }
        }
        pSecond->_482 = hidden;
    }

    // The second player's life meter (GameSceneLayoutHolder::mSecondMeter, the game's HitPointMeter made again): his
    // health, his colour, where the first player's stands alone (the first's moved left beside it), shown while he
    // plays and the game's own meter shows.
    const char* const sMeterTintPanes[] = {"PicBase", "PicBaseW", "PicPoint1", "PicPoint2", "PicPoint3",
                                           "PicPoint1W", "PicPoint2W", "PicPoint3W"};
    const f32 kFirstMeterShift = -85.0f;

    void updateSecondMeter(bool shown) {
        if (!MR::isExistSceneObj(SceneObj_GameSceneLayoutHolder)) {
            return;
        }
        GameSceneLayoutHolder* pHolder = MR::getGameSceneLayoutHolder();
        MeterLayout* pMeter = pHolder->mSecondMeter;
        MeterLayout* pFirst = pHolder->mMarioMeter != nullptr ? pHolder->mMarioMeter->mHitPointMeter : nullptr;
        if (pMeter == nullptr || pFirst == nullptr) {
            return;
        }
        // The first player's meter moved left by its root pane, in layout units (the HUD's widescreen fitting
        // works on the panes below it).
        static MeterLayout* spPlaced;
        nw4r::lyt::Pane* pFirstRoot = MR::getRootPane(pFirst);
        shown = shown && !MR::isDead(pFirst) && !MR::isHiddenLayout(pFirst) && !pHolder->mMarioMeter->mUseSuddenDeath;
        if (!shown) {
            if (spPlaced == pFirst) {
                pFirstRoot->mTranslate.x -= kFirstMeterShift;
                spPlaced = nullptr;
            }
            if (!MR::isHiddenLayout(pMeter)) {
                MR::hideLayout(pMeter);
            }
            return;
        }
        if (spPlaced != pFirst) {
            pFirstRoot->mTranslate.x += kFirstMeterShift;
            spPlaced = pFirst;
            pMeter->initCount(sState.mSecond->mHealth);
            pMeter->requestActive();
        }
        if (MR::isHiddenLayout(pMeter)) {
            MR::showLayout(pMeter);
        }
        pMeter->setCount(sState.mSecond->mHealth);
        // His colour, set each frame (the meter's animations set the colours).
        for (u32 i = 0; i < ARRAY_SIZE(sMeterTintPanes); i++) {
            nw4r::lyt::Pane* pPane = MR::getPane(pMeter, sMeterTintPanes[i]);
            if (pPane == nullptr) {
                continue;
            }
            for (u32 corner = 0; corner < 4; corner++) {
                pPane->SetVtxColorElement(corner * 4 + 0, coop::kSecondMeterTint[0]);
                pPane->SetVtxColorElement(corner * 4 + 1, coop::kSecondMeterTint[1]);
                pPane->SetVtxColorElement(corner * 4 + 2, coop::kSecondMeterTint[2]);
            }
        }
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
            if (const u32 starBits = __NebulaOption(NEBULA_OPTION_TEST_STAR_BITS)) {
                MR::addStarPiece(starBits);
            }
        }
        // The first player taken somewhere far while the game had his controls (a launch star, a pipe, a warp):
        // the second joins him there once he has landed and has control again.
        // The second plays on meanwhile; the catch-up is for a second player who stood still through it.
        if (sState.mControllableFrames == 0) {
            if (sState.mHeldFrames == 0) {
                sState.mTripStart = pSecond->mPosition;
            }
            sState.mHeldFrames++;
        } else if (sState.mControllableFrames == coop::kLevelStartSettleFrames) {
            if (sState.mHeldFrames >= kCatchUpHeldFrames && sState.mPresent && !sState.mDown.down &&
                PSVECDistance(&pFirst->mPosition, &pSecond->mPosition) > kCatchUpDistance &&
                PSVECDistance(&sState.mTripStart, &pSecond->mPosition) < kCatchUpStillDistance) {
                landBeside = true;
                OSReport("coop: second player joins the first after his trip\n");
            }
            sState.mHeldFrames = 0;
        }
        sState.mTravelling = coop::keep_travelling(sState.mTravelling, sState.mLevelStart, sState.mControllableFrames);
        // A cutscene holds him where he stands; the first player's own trips (his controls held) do not.
        const bool holdHere = !sState.mTravelling && demo;
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
        // The first player's vehicle: the second rides along unseen at his side, and gets off beside him.
        const bool riding = sState.mPresent && firstOnVehicle();
        setHidden(!sState.mPresent || sState.mLevelStart || down || riding);

        if (!sState.mLevelStart && sState.mTurns + 1 == __NebulaOption(NEBULA_OPTION_TEST_LAUNCH_STAR)) {
            testLaunchStar();
        }
        // GALAXY_TEST_PUT: the first player now, the second in his turn below.
        LiveActor* pPutSecondAt = nullptr;
        {
            const u32 put = __NebulaOption(NEBULA_OPTION_TEST_PUT);
            if (put != 0 && !sState.mLevelStart && sState.mTurns + 1 == (put & 0xFFFFF)) {
                const u32 kind = (put >> 20) & 0xF;
                const bool second = (put >> 28) & 1;
                LiveActor* pObj = kind < NEBULA_TEST_PUT_KINDS ? testPutTarget(kind, second ? pSecond : pFirst) : nullptr;
                if (pObj != nullptr && !second) {
                    // Onto a Star Ball from above (its centre is inside it).
                    MR::setPlayerPos(kind == NEBULA_TEST_PUT_STAR_BALL ? pObj->mPosition + TVec3f(0.0f, 300.0f, 0.0f)
                                                                       : pObj->mPosition);
                } else {
                    pPutSecondAt = pObj;
                }
            }
        }
        // Tests: what the second player rides, every two seconds.
        if (sState.mTurns % 120 == 1 && __NebulaOption(NEBULA_OPTION_TEST_PUT) != 0) {
            const LiveActor* pRide = riddenBy(pSecond);
            const LiveActor* pFirstRide = riddenBy(pFirst);
            OSReport("coop test: second at %.0f %.0f %.0f rides %s, hidden %d; first rides %s\n", pSecond->mPosition.x,
                     pSecond->mPosition.y, pSecond->mPosition.z, pRide != nullptr ? pRide->mName : "nothing",
                     sState.mHidden, pFirstRide != nullptr ? pFirstRide->mName : "nothing");
        }

        // His turn.
        enterSecond();
        if (sState.mPresent && !down && !riding) {
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
        if (pPutSecondAt != nullptr) {
            MR::setPlayerPos(pPutSecondAt->mPosition);
        } else if (comeBack) {
            // Back from a fall: beside the first, his state as at a fresh start.
            MR::setPlayerPosAndWait(besideFirst());
        } else if (!sState.mPresent || sState.mTravelling || landBeside || down || riding) {
            MR::setPlayerPos(sState.mLevelStart ? sState.mParked : besideFirst());
        } else if (holdHere) {
            MR::setPlayerPos(sState.mHeldAt);
        } else {
            sState.mParked = pSecond->mPosition;
        }
        leaveSecond();

        updateSecondMeter(sState.mPresent && !sState.mLevelStart && !down && !riding);
        if (!sState.mPresent || sState.mLevelStart || down || riding) {
            sState.mSplit.reset();
            sState.mFollow.reset();
            sState.mLayout.reset();
            return;
        }
        bumpPlayers();
        CameraContext* pContext = cameraContext();
        // His own event: his controls held, or a launch star (or another non-vehicle object) carrying him.
        const LiveActor* pSecondRide = riddenBy(pSecond);
        const bool secondEvent =
            !demo && (pSecond->mMario->isInputDisable() ||
                      (pSecondRide != nullptr && !isVehicle(pSecondRide) && MR::isEventCameraActive()));
        if (pContext == nullptr || holdHere || sState.mTravelling) {
            // One screen while the game holds the players (a cutscene, a level's opening): the split eases out.
            if (sState.mSplit.update(0.0f, 0.0f) == 0.0f) {
                sState.mFollow.reset();
                sState.mLayout.reset();
            }
        } else {
            updateSplit(pContext, pFirst->mPosition, pSecond->mPosition, (firstHeld && !demo) || secondEvent);
        }
        // His own event: if the game camera went with him, the first player's half follows the first meanwhile.
        const coop::CameraMatrices gameCamera = pContext != nullptr ? readCamera(pContext) : coop::CameraMatrices{};
        // The game camera went with him when an event camera runs that the first player did not start, or it is
        // nearer him.
        sState.mSecondEvent = secondEvent && pContext != nullptr &&
                              ((MR::isEventCameraActive() && !firstHeld) ||
                               coop::camera_is_second_players(coop::pose_of(gameCamera.inverse).eye,
                                                              toVec(pFirst->mPosition), toVec(pSecond->mPosition)));
        if (sState.mSecondEvent) {
            const coop::Vec3 up = toVec(pFirst->mMario->getUpVec());
            if (!sState.mFirstFollow.started()) {
                sState.mFirstFollow.start(coop::pose_of(sState.mGameCamera.inverse).eye, toVec(pFirst->mPosition),
                                          toVec(pFirst->mPosition), up);
            }
            sState.mFirstFollowView = sState.mFirstFollow.update(toVec(pFirst->mPosition), up);
        } else {
            sState.mFirstFollow.reset();
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

    ShooterScope::ShooterScope(s32 channel) : mPlayer(false), mCamera(false) {
        if (channel != 1 || sState.mSecond == nullptr || !sState.mPresent || sState.mHidden ||
            MR::getMarioHolder()->getMarioActor() != sState.mFirst) {
            return;
        }
        gIsLuigi = !sState.mOwnIsLuigi;
        MR::getMarioHolder()->setMarioActor(sState.mSecond);
        mPlayer = true;
        CameraContext* pContext = cameraContext();
        if (pContext != nullptr && splitShown() && !sState.mSecondEvent) {
            sState.mGameCamera = readCamera(pContext);
            memcpy(&sState.mGameProjection[0], &pContext->mProjection, sizeof(float) * 16);
            writeCamera(pContext, coop::blend_cameras(sState.mGameCamera, sState.mFollowView, sState.mSplit.eased()));
            setHalfProjection(pContext, true);
            mCamera = true;
        }
    }

    ShooterScope::~ShooterScope() {
        if (mCamera) {
            CameraContext* pContext = cameraContext();
            writeCamera(pContext, sState.mGameCamera);
            memcpy(&pContext->mProjection, &sState.mGameProjection[0], sizeof(float) * 16);
        }
        if (mPlayer) {
            MR::getMarioHolder()->setMarioActor(sState.mFirst);
            gIsLuigi = sState.mOwnIsLuigi;
        }
    }

    bool isSecondInPlay() {
        return sState.mSecond != nullptr && sState.mPresent;
    }

    bool isOtherBrother(const LiveActor* pActor, const LiveActor* pOther) {
        return sState.mSecond != nullptr && pActor != pOther &&
               (pActor == sState.mFirst || pActor == sState.mSecond) &&
               (pOther == sState.mFirst || pOther == sState.mSecond);
    }

    void noteTestObject(u32 kind, const LiveActor* pObj) {
        if (kind < NEBULA_TEST_PUT_KINDS) {
            sTestClasses[kind] = *reinterpret_cast< const void* const* >(pObj);
        }
    }

    BoundObjectScope::BoundObjectScope(const LiveActor* pActor) : mSwapped(false) {
        if (sState.mSecond == nullptr || !sState.mPresent || pActor == sState.mSecond || pActor == sState.mFirst ||
            riddenBy(sState.mSecond) != pActor || MR::getMarioHolder()->getMarioActor() != sState.mFirst) {
            return;
        }
        enterSecond();
        mSwapped = true;
    }

    BoundObjectScope::~BoundObjectScope() {
        if (mSwapped) {
            leaveSecond();
        }
    }

    bool mayRush(const MarioActor* pActor, const HitSensor* pSensor) {
        if (sState.mSecond == nullptr || pSensor == nullptr) {
            return true;
        }
        const MarioActor* pOther = pActor == sState.mSecond ? sState.mFirst : sState.mSecond;
        if (riddenBy(pOther) == pSensor->mHost) {
            return false;
        }
        if (pActor == sState.mSecond && (pSensor->isType(ATYPE_SPHERE_PLAYER_BIND) || isVehicle(pSensor->mHost))) {
            return false;
        }
        return true;
    }

    const TVec3f* nearerPlayerCenter(const TVec3f& rPosition) {
        MarioActor* pSecond = sState.mSecond;
        if (pSecond != nullptr && sState.mPresent && !sState.mHidden && riddenBy(sState.mFirst) == nullptr &&
            rPosition.squared(pSecond->mPosition) <
                rPosition.squared(sState.mFirst->mPosition)) {
            return &pSecond->_2A0;
        }
        return MR::getPlayerCenterPos();
    }

    bool getOtherHalfView(TPos3f* pView) {
        if (sState.mSecond == nullptr || !sState.mPresent || !splitShown()) {
            return false;
        }
        CameraContext* pContext = cameraContext();
        if (pContext == nullptr) {
            return false;
        }
        const coop::CameraMatrices other =
            sState.mSecondEvent ? sState.mFirstFollowView
                                : coop::blend_cameras(readCamera(pContext), sState.mFollowView, sState.mSplit.eased());
        memcpy(pView, &other.view[0], sizeof(float) * 12);
        return true;
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
        if (sState.mSecondEvent) {
            // The game camera is the second player's event's: the first player's half draws with his own.
            writeCamera(pContext, sState.mFirstFollowView);
            MR::loadViewMtx();
            SceneFunction::executeCalcViewAndEntryList();
        }
        setHalfProjection(pContext, false);
        sendSplitClip(false);
        return true;
    }

    void nextSplitPass() {
        CameraContext* pContext = cameraContext();
        sState.mPass = 2;
        // His half: the first player's view panning to his own camera as the split eases in; the game camera while
        // it is his event's.
        writeCamera(pContext, sState.mSecondEvent ? sState.mGameCamera
                                                  : coop::blend_cameras(sState.mGameCamera, sState.mFollowView,
                                                                        sState.mSplit.eased()));
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
