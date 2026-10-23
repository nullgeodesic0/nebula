#include "galaxy/coop_guest.h"

#include "guest_access.h"

#include "galaxy/coop.h"
#include "galaxy/netplay.h"
#include "galaxy/runtime_settings.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace galaxy {
namespace {

using guest_access::Guest;
using guest_access::plausible;
using guest_access::SavedContext;

// Mario and Luigi co-op on one screen, in one world (docs/MULTIPLAYER.md).
//
// The game has one player: a MarioActor, which the game reaches through the scene's MarioHolder, and whose
// character (gIsLuigi) and controller (Wii Remote channel 0) are global. A second MarioActor is created right
// after the first, as the other character; it is kept out of the game's own movement list and moved by the
// runtime straight after the first player, with the MarioHolder, gIsLuigi and the channel 0 and 1 pads
// swapped to it for that call, so the game's player code runs for it as it does for the first. Its
// controller is channel 1, which the runtime fills from the second controller (KpadSynth).
//
// RMGE01 addresses and layouts are from Petari (symbols.txt, MarioActor.hpp, WPad.hpp and the
// stationed-archive loader); call ids are translate.rs coop_call_id.

namespace call {
constexpr std::uint32_t kKpadRead = 1u;
constexpr std::uint32_t kGameDataHolderIsDataMario = 2u;
constexpr std::uint32_t kGameDataFunctionIsDataMario = 3u;
constexpr std::uint32_t kMarioActorInit2 = 4u;
constexpr std::uint32_t kCategoryListExecuteMovement = 5u;
constexpr std::uint32_t kMountArchive = 6u;
constexpr std::uint32_t kCreateAndAddPlayerArchives = 7u;
constexpr std::uint32_t kPlayerHeapCreateHeap = 8u;
constexpr std::uint32_t kGameSceneDraw3D = 9u;
constexpr std::uint32_t kGxSetViewport = 10u;  // hooked but left alone: viewports stay full-screen
constexpr std::uint32_t kGxSetScissor = 11u;   // likewise: the renderer keeps each half (SplitClip)
constexpr std::uint32_t kClipFrustum = 12u;
constexpr std::uint32_t kShadowFarClipping = 13u;
constexpr std::uint32_t kSetPlayerMode = 14u;
constexpr std::uint32_t kNameObjHolderCtor = 15u;
constexpr std::uint32_t kNameObjHolderAdd = 16u;
constexpr std::uint32_t kSolidHeapAllocHead = 17u;  // JKRSolidHeap::allocFromHead(u32 size, int align)
constexpr std::uint32_t kSolidHeapAllocTail = 18u;  // JKRSolidHeap::allocFromTail(u32 size, int align)
// A player down (the calls MarioActor::exeGameOver* make; skippable).
constexpr std::uint32_t kStartPlayerEvent = 19u;
constexpr std::uint32_t kStartDeathCamera = 20u;
constexpr std::uint32_t kStartPointerDeathDemo = 21u;
constexpr std::uint32_t kSetCubeBgmChangeInvalid = 22u;
constexpr std::uint32_t kClearBgmQueue = 23u;
// HUD.
constexpr std::uint32_t kMarioInit2D = 24u;    // skippable
constexpr std::uint32_t kMarioUpdate2D = 25u;  // skippable
constexpr std::uint32_t kMeterRequestFirst = 26u;  // 26-30: MeterLayout::request*
constexpr std::uint32_t kMeterRequestLast = 30u;
constexpr std::uint32_t kPlayerLeftFirst = 31u;    // 31-33: PlayerLeft::appear, forceAppear, disappear
constexpr std::uint32_t kPlayerLeftLast = 33u;
constexpr std::uint32_t kGetPlayerLeft = 34u;
constexpr std::uint32_t kIncPlayerLife = 35u;
// Launch stars: SuperSpinDriver then SpinDriver exeCapture, exeShootStart, exeShoot.
constexpr std::uint32_t kStarStepFirst = 36u;
constexpr std::uint32_t kStarStepLast = 41u;
constexpr std::uint32_t kAddArchive = 42u;          // NameObjArchiveListCollector::addArchive(const char*)
constexpr std::uint32_t kAttackFirst = 43u;         // 43-45: MarioActor::attackOrPushSensor*(HitSensor*, f32)
constexpr std::uint32_t kAttackLast = 45u;
// EarthenPipe exeReady, exePlayerIn (skippable), exeTargetPipeShowUp, exePlayerOut.
constexpr std::uint32_t kPipeStepFirst = 46u;
constexpr std::uint32_t kPipePlayerIn = 47u;
constexpr std::uint32_t kPipePlayerOut = 49u;
constexpr std::uint32_t kPipeStepLast = 49u;
constexpr std::uint32_t kIsNearPlayer = 50u;         // MR::isNearPlayer(const LiveActor*, f32)
constexpr std::uint32_t kIsNearPlayerAnyTime = 51u;  // MR::isNearPlayerAnyTime(const LiveActor*, f32)
constexpr std::uint32_t kGetPlayerPos = 52u;         // MR::getPlayerPos()
constexpr std::uint32_t kNearerPlayerCode = 53u;     // any function of the star bunnies (TrickRabbit, RunawayRabbit)
// Multiplayer talks (talk_begin).
constexpr std::uint32_t kTryTalk = 54u;              // MR::tryTalk*(TalkMessageCtrl*, ...)
constexpr std::uint32_t kTryTalkNpc = 55u;           // MR::tryTalk*AndStartTalkAction(NPCActor*, ...)
constexpr std::uint32_t kTalkDirectorStep = 56u;     // TalkDirector::movement's updateMessage and LayoutActor::movement
constexpr std::uint32_t kEndRemoteDemo = 57u;        // MarioAccess::endRemoteDemo(const RushEndInfo*)
constexpr std::uint32_t kReadyRemoteDemo = 58u;      // MarioAccess::readyRemoteDemo()
constexpr std::uint32_t kPadButtonA = 59u;           // MR::testCorePadButtonA(s32)
constexpr std::uint32_t kPadTriggerA = 60u;          // MR::testCorePadTriggerA(s32)
constexpr std::uint32_t kPointingPane = 61u;         // MR::isStarPointerPointingPane(layout, pane, s32 chan, bool, name)
constexpr std::uint32_t kMenuDecide = 62u;           // MR::testDPDMenuPadDecideTrigger()
// Talks that do not freeze the world (63-68 skippable, by call site in TalkDirector).
constexpr std::uint32_t kTalkDemoStart = 63u;        // MR::tryStartDemoMarioPuppetable (prepTalk)
constexpr std::uint32_t kStartPlayerTalk = 64u;      // MR::startPlayerTalk (prepTalk)
constexpr std::uint32_t kEndPlayerTalk = 65u;        // MR::endPlayerTalk (termTalk)
constexpr std::uint32_t kEndDemo = 66u;              // MR::endDemo (termTalk)
constexpr std::uint32_t kTalkCamera = 67u;           // TalkMessageCtrl::startCamera (TalkStateEvent::open)
constexpr std::uint32_t kTalkCameraEnd = 68u;        // MR::endNPCTalkCamera, MR::endMultiActorCamera (termTalk)
constexpr std::uint32_t kCameraNearlyEnd = 69u;      // MR::isCameraInterpolatingNearlyEnd (TalkBalloonEvent::exeWait)
constexpr std::uint32_t kCanStartDemo = 70u;         // MR::canStartDemo()
constexpr std::uint32_t kCheckButton = 71u;          // MarioActor::checkButtonType(u16 button, bool)
constexpr std::uint32_t kLodDistance = 72u;          // LodCtrl::calcDistanceToCamera() const
constexpr std::uint32_t kBalloonPos = 73u;           // TalkMessageCtrl::updateBalloonPos() (TalkBalloon::updateBalloon)
// Star Pointers (docs/MULTIPLAYER.md "Star Pointers").
constexpr std::uint32_t kPointingCore = 74u;      // isStarPointerPointingCore(info, actor, s32 channel, ...)
constexpr std::uint32_t kScreenPosition = 75u;    // MR::calcScreenPosition(TVec2f*, const TVec3f&)
constexpr std::uint32_t kTractTrigger = 76u;      // MR::isOnTractTrigger()
constexpr std::uint32_t kCaptureCode = 77u;       // calls into GCapture and GCaptureTarget
constexpr std::uint32_t kPointing = 78u;          // MR::isStarPointerPointing(const LiveActor*, s32, bool, const char*)
constexpr std::uint32_t kPointerScreenPos = 79u;  // MR::getStarPointerScreenPosition(s32)
constexpr std::uint32_t kStrokeMoment = 80u;      // MR::calcStarPointerStrokeRotateMoment(TVec3f*, const TVec3f&, f32, s32)
constexpr std::uint32_t kCoStar = 81u;            // the second pointer's Co-Star actions (MarioActor::isRequestJump2P, ...)
constexpr std::uint32_t kSpineUpdate = 82u;       // Spine::update(): an actor's nerve step
}  // namespace call

namespace fn {
constexpr std::uint32_t kCreateMarioActor = 0x80263C34u;     // createNameObj<MarioActor>(const char*)
constexpr std::uint32_t kMarioActorInit2 = 0x802AED64u;      // MarioActor::init2(const TVec3f&, const TVec3f&, s32)
constexpr std::uint32_t kMarioActorMovement = 0x802AFD80u;   // MarioActor::movement()
constexpr std::uint32_t kRequestSuspend = 0x80261278u;       // NameObj::requestSuspend(): out of the movement list
constexpr std::uint32_t kGetMarioHolder = 0x802E1528u;       // MR::getMarioHolder()
constexpr std::uint32_t kSetMarioActor = 0x802E1518u;        // MarioHolder::setMarioActor(MarioActor*)
constexpr std::uint32_t kGetMarioActor = 0x802E1520u;        // MarioHolder::getMarioActor() const
constexpr std::uint32_t kSetCameraTargetToPlayer = 0x803C8ACCu;  // MR::setGameCameraTargetToPlayer()
constexpr std::uint32_t kGetWPadHolder = 0x803AB558u;        // (WPadHolder.cpp) getWPadHolder(): +0 WPad* mPad[2]
constexpr std::uint32_t kSetPlayerPos = 0x803F1F80u;         // MR::setPlayerPos(const TVec3f&)
constexpr std::uint32_t kSetPlayerMode = 0x802C42C0u;        // MarioActor::setPlayerMode(u16, bool)
constexpr std::uint32_t kMountArchive = 0x803CDDD4u;         // MR::mountArchive(const char*, JKRHeap*)
constexpr std::uint32_t kCreateAndAddStationed = 0x803A6A70u;  // ResourceHolderManager::createAndAddStationed(const char*)
// Mario::push(const TVec3f&): added to the velocity on Mario's next move (Mario::tryPushToVelocity), which
// keeps walls and the ground in charge, as when objects shove the player.
constexpr std::uint32_t kMarioPush = 0x8030C330u;
// Split screen (docs/MULTIPLAYER.md): the 3D scene is drawn a second time with the second player's camera.
constexpr std::uint32_t kGameSceneDraw3D = 0x8033EE8Cu;     // GameScene::draw3D() const
constexpr std::uint32_t kCalcViewAndEntry = 0x80343DCCu;    // SceneFunction::executeCalcViewAndEntryList()
constexpr std::uint32_t kLoadViewMtx = 0x803C7394u;         // MR::loadViewMtx(): CameraContext view -> j3dSys
constexpr std::uint32_t kSetDefaultViewportAndScissor = 0x803CA5F4u;  // MR::setDefaultViewportAndScissor()
constexpr std::uint32_t kGetCameraDirector = 0x8009B08Cu;   // MR::getCameraDirector()
constexpr std::uint32_t kIsEventCameraActive = 0x803C799Cu;  // MR::isEventCameraActive()
constexpr std::uint32_t kIsOpeningCamera = 0x8033F9F4u;  // GameSceneFunction::isExecScenarioOpeningCamera()

constexpr std::uint32_t kGetFrameBufferWidth = 0x802B5140u;   // MR::getFrameBufferWidth()
constexpr std::uint32_t kGetFrameBufferHeight = 0x8034AFB0u;  // MR::getFrameBufferHeight()
// MR::getFirstPolyOnLineToMap(TVec3f* hit, Triangle*, const TVec3f& start, const TVec3f& direction): the
// second player's camera collision.
constexpr std::uint32_t kFirstPolyOnLineToMap = 0x803E0874u;
constexpr std::uint32_t kShadowGetDropPos = 0x8016C1B4u;  // ShadowController::getDropPos(TVec3f*) const
constexpr std::uint32_t kIsInAreaObj = 0x803FEA64u;  // MR::isInAreaObj(const char* name, const TVec3f& position)
constexpr std::uint32_t kIsDemoActive = 0x803C9E84u;     // MR::isDemoActive()
constexpr std::uint32_t kIsTimeKeepDemoActive = 0x803C9F80u;  // MR::isTimeKeepDemoActive()
constexpr std::uint32_t kIsInputDisable = 0x802E98B8u;   // MarioModule::isInputDisable() const (on a Mario)
// Lives and respawn.
constexpr std::uint32_t kGetPlayerLeft = 0x803CC3CCu;    // MR::getPlayerLeft()
constexpr std::uint32_t kIncPlayerLeft = 0x803CC3D0u;    // MR::incPlayerLeft()
constexpr std::uint32_t kDecPlayerLeft = 0x803CC3D8u;    // MR::decPlayerLeft()
constexpr std::uint32_t kSetNerve = 0x80165824u;         // LiveActor::setNerve(const Nerve*)
constexpr std::uint32_t kSetPlayerPosAndWait = 0x803F2528u;  // MR::setPlayerPosAndWait(const TVec3f&)
constexpr std::uint32_t kShowModel = 0x803DD758u;        // MR::showModel(LiveActor*)
constexpr std::uint32_t kHideModel = 0x803DD794u;        // MR::hideModel(LiveActor*)
constexpr std::uint32_t kValidateHitSensors = 0x803C27B8u;    // MR::validateHitSensors(LiveActor*)
constexpr std::uint32_t kInvalidateHitSensors = 0x803C27CCu;  // MR::invalidateHitSensors(LiveActor*)
constexpr std::uint32_t kMarioIncLife = 0x802B1CE4u;     // MarioActor::incLife(u32)
constexpr std::uint32_t kChangeGameOverAnimation = 0x802AF7BCu;  // MarioActor::changeGameOverAnimation()
constexpr std::uint32_t kChangeAnimationNonStop = 0x802AF660u;  // MarioActor::changeAnimationNonStop(const char*)
// Pipes (EarthenPipe.cpp).
constexpr std::uint32_t kIsBckStopped = 0x803DB2B0u;     // MR::isBckStopped(const LiveActor*)
constexpr std::uint32_t kPipeNrvWait = 0x806A06D0u;      // NrvEarthenPipe::EarthenPipeNrvWait::sInstance
constexpr std::uint32_t kAreaIsInVolume = 0x8001E1A0u;   // AreaObj::isInVolume(const TVec3f&) const
constexpr std::uint32_t kSetPaneScale = 0x803D6A24u;     // MR::setPaneScale(const LayoutActor*, f32, f32, const char* pane)
constexpr std::uint32_t kTalkBalloonEventVtable = 0x805A21A0u;  // __vt__16TalkBalloonEvent
// Talks.
constexpr std::uint32_t kIsSystemTalking = 0x803CA334u;  // MR::isSystemTalking()
constexpr std::uint32_t kReadyRemoteDemo = 0x80303B5Cu;  // MarioAccess::readyRemoteDemo(): the game's player held
constexpr std::uint32_t kEndRemoteDemo = 0x80304538u;    // MarioAccess::endRemoteDemo(const RushEndInfo*)
constexpr std::uint32_t kTestPadButtonA = 0x803D2244u;   // MR::testCorePadButtonA(s32 channel)
constexpr std::uint32_t kTestPadTriggerA = 0x803D25CCu;  // MR::testCorePadTriggerA(s32 channel)
constexpr std::uint32_t kPointingPane = 0x803FA9ECu;     // MR::isStarPointerPointingPane(...)
constexpr std::uint32_t kMenuDecide = 0x803D2B6Cu;       // MR::testDPDMenuPadDecideTrigger(): channel 0's A
// HUD.
constexpr std::uint32_t kGetLayoutHolder = 0x80361738u;  // MR::getGameSceneLayoutHolder()
constexpr std::uint32_t kAddArchive = 0x8026158Cu;       // NameObjArchiveListCollector::addArchive(const char*)
constexpr std::uint32_t kNewOnHeap = 0x80409B0Cu;        // operator new(u32 size, JKRHeap*, int align)
constexpr std::uint32_t kInitWithoutIter = 0x8026120Cu;  // NameObj::initWithoutIter()
constexpr std::uint32_t kMeterLayoutCtor = 0x8036C2BCu;  // MeterLayout::MeterLayout(const char*, const char*)
constexpr std::uint32_t kMeterInitCount = 0x8036C80Cu;   // MeterLayout::initCount(s32)
constexpr std::uint32_t kMeterSetCount = 0x8036C814u;    // MeterLayout::setCount(s32)
constexpr std::uint32_t kPlayerLeftCtor = 0x8037867Cu;   // PlayerLeft::PlayerLeft(const char*)
constexpr std::uint32_t kLayoutSetTrans = 0x803662CCu;   // LayoutActor::setTrans(const TVec2f&): a screen position
constexpr std::uint32_t kGetPane = 0x803D82F4u;          // MR::getPane(const LayoutActor*, const char*)
constexpr std::uint32_t kIsLayoutDead = 0x803D813Cu;     // MR::isDead(const LayoutActor*)
constexpr std::uint32_t kShowLayout = 0x803D814Cu;       // MR::showLayout(LayoutActor*)
constexpr std::uint32_t kLayoutKill = 0x803661F8u;       // LayoutActor::kill()
// nw4r::lyt::Picture::SetVtxColorElement(u32 element, u8): element 4 * corner + R, G, B, A.
constexpr std::uint32_t kPictureSetVtxColorElement = 0x8000DE94u;
constexpr std::uint32_t kPictureVtable = 0x80550130u;    // nw4r::lyt::Picture's vtable
// MR::convertScreenPosToLayoutPos(TVec2f* layout, const TVec2f& screen)
constexpr std::uint32_t kScreenToLayout = 0x80366658u;
// The MeterLayout requests MarioMeter makes (ids 26-30) and the PlayerLeft calls the counters make (31-33),
// made on the second player's meter and counter as well.
constexpr std::array<std::uint32_t, 5> kMeterRequests = {0x8036C59Cu, 0x8036C664u, 0x8036C6E8u, 0x8036C72Cu,
                                                         0x8036C79Cu};
constexpr std::array<std::uint32_t, 3> kPlayerLeftCalls = {0x803787CCu, 0x80378828u, 0x80378888u};
// Star Pointers.
constexpr std::uint32_t kStarPointerViewMtx = 0x803FB51Cu;  // MR::getStarPointerViewMtx(): StarPointerTransformHolder's
constexpr std::uint32_t kStarPointerProjMtx = 0x803FB540u;  // MR::getStarPointerProjMtx()
constexpr std::uint32_t kIsStarPointerPointing = 0x803FA9A8u;  // MR::isStarPointerPointing(actor, s32, bool, const char*)
constexpr std::uint32_t kIsNerve = 0x8016582Cu;              // LiveActor::isNerve(const Nerve*) const
constexpr std::uint32_t kGetStarPointerDirector = 0x80386000u;  // StarPointerFunction::getStarPointerDirector()
constexpr std::uint32_t kGetScreenWidth = 0x803F6B44u;          // MR::getScreenWidth(): 832 wide, 608 at 4:3
}  // namespace fn
// StarPointerDirector +4: StarPointerController[2], each kStarPointerControllerSize; mPastInfo.mPos (the position
// target tests use, last frame's) at kPointerPastPos.
constexpr std::uint32_t kStarPointerControllerSize = 0x74u;
constexpr std::uint32_t kPointerPastPos = 0x04u;

// Pull Stars (GCapture.cpp): the one GCapture, its idle nerves, and its and GCaptureTarget's code.
constexpr std::uint32_t kGCaptureVtable = 0x80580814u;
constexpr std::uint32_t kGCaptureTargetVtable = 0x80580AB0u;
constexpr std::uint32_t kGCaptureNrvWait = 0x806A07E0u;      // NrvGCapture::GCaptureNrvWait::sInstance
constexpr std::uint32_t kGCaptureNrvCoolDown = 0x806A07F8u;  // NrvGCapture::GCaptureNrvCoolDown::sInstance
constexpr bool in_capture_target(std::uint32_t pc) { return pc >= 0x801E1630u && pc < 0x801E2408u; }
constexpr bool in_capture_code(std::uint32_t pc) {
    return (pc >= 0x801DEBA4u && pc < 0x801E0CF8u) || in_capture_target(pc);
}
constexpr bool in_star_pointer_layout(std::uint32_t pc) { return pc >= 0x803861D4u && pc < 0x803893A4u; }
constexpr bool in_capture_receive(std::uint32_t pc) { return pc >= 0x801DF0ECu && pc < 0x801DF164u; }  // receiveOtherMsg

// CameraContext (scene object 0x17), reached as MR::getCameraViewMtx does: *(r13 - 0x3A78) -> +0x24 -> +0xAC ->
// +0x10 -> [0x17].
namespace camera {
constexpr std::uint32_t kView = 0x0Cu;        // 3x4
constexpr std::uint32_t kInverse = 0x3Cu;     // 3x4
constexpr std::uint32_t kProjection = 0x6Cu;  // 4x4; [0][0] is the x scale
constexpr std::uint32_t kAspect = 0xACu;
constexpr std::uint32_t kFarZ = 0xB4u;
constexpr std::uint32_t kFovy = 0xB8u;  // degrees
}  // namespace camera

constexpr std::uint32_t kIsLuigi = 0x806A18C8u;               // gIsLuigi (bool)
constexpr std::uint32_t kResourceHolderManager = 0x806A2480u;  // SingletonHolder<ResourceHolderManager>::sInstance
constexpr std::uint32_t kMovementTypePlayer = 0x25u;          // MR::MovementType_Player
constexpr std::uint32_t kPlayerHeapSize = 0x500000u;          // PlayerHeapHolder's heaps, one character's set
// Room for the second character's model, face and shadow archives next to the first's.
constexpr std::uint32_t kPartnerHeapExtra = 0x200000u;
// SingletonHolder<HeapMemoryWatcher>::sInstance; HeapMemoryWatcher::mSceneHeapGDDR (the stage's MEM2 heap).
constexpr std::uint32_t kHeapMemoryWatcher = 0x806A245Cu;
// SingletonHolder<GameSystem>::sInstance: +0x24 GameSystemSceneController, whose current stage name is at +0x20.
constexpr std::uint32_t kGameSystem = 0x806A1228u;
constexpr std::uint32_t kSceneHeapNapa = 0x14u;
constexpr std::uint32_t kSceneHeapGddr = 0x18u;
constexpr std::uint32_t kSolidHeapFreeSize = 0x6Cu;  // JKRSolidHeap::mFreeSize
// MarioActor::_482: set, MarioActor::draw leaves the player's model out, as in a black hole (MR::hideModel does
// not; _A61 would also darken the screen, MarioActor::drawPreWipe).
constexpr std::uint32_t kMarioActorHidden = 0x482u;
// NameObj::mFlags (u16 at +8), its low byte: bit 0 suspended (NameObj::requestSuspend asks with bit 1, requestResume
// with bit 2; NameObj::syncWithFlags applies them).
constexpr std::uint32_t kNameObjFlagsLow = 0x9u;
constexpr std::uint8_t kNameObjSuspended = 0x01u;
constexpr std::uint8_t kNameObjRequests = 0x06u;
// MarioActor's own nerve for ordinary play (NrvMarioActor::MarioActorNrvWait::sInstance).
constexpr std::uint32_t kMarioActorNrvWait = 0x8069F038u;
// MarioActor::exeGameOver* (the player down: exeGameOver to exeTimeWait, and exeGameOverBlackHole2).
constexpr bool in_game_over_step(std::uint32_t pc) {
    return (pc >= 0x802B8A38u && pc < 0x802B8FE0u) || (pc >= 0x802CA324u && pc < 0x802CA6E0u);
}
// PlayerLeft's code (its MR::getPlayerLeft calls).
constexpr bool in_player_left(std::uint32_t pc) { return pc >= 0x803786D0u && pc < 0x80378B10u; }
// GameSceneLayoutHolder: +0x14 MarioMeter (+0x0C its HitPointMeter), +0x34 CounterLayoutController (+0x30 its
// PlayerLeft).
constexpr std::uint32_t kHolderMarioMeter = 0x14u;
constexpr std::uint32_t kMarioMeterHitPoint = 0x0Cu;
constexpr std::uint32_t kHolderCounters = 0x34u;
constexpr std::uint32_t kCountersPlayerLeft = 0x30u;
constexpr std::uint32_t kPlayerLeftCount = 0x20u;  // PlayerLeft::mPlayerLeft
// A player riding a launch star: MarioActor::_924 (the HitSensor they rush with), HitSensor::mHost, and the
// launchers' vtables (SuperSpinDriver, SpinDriver).
constexpr std::uint32_t kMarioRushSensor = 0x924u;
constexpr std::uint32_t kSensorHost = 0x24u;
constexpr std::uint32_t kSuperSpinDriverVtable = 0x8058FF8Cu;
constexpr std::uint32_t kSpinDriverVtable = 0x8058C5ACu;
// Star-shaped launchers (SuperSpinDriver, SpinDriver): the actor riding them.
constexpr std::uint32_t kStarBindActor = 0x8Cu;
// EarthenPipe (Petari EarthenPipe.hpp): its up vector, the top joint's matrix (the mouth at column 3) and the
// player riding it.
constexpr std::uint32_t kPipeUp = 0x98u;
constexpr std::uint32_t kPipeTopMtx = 0xB4u;
constexpr std::uint32_t kPipeRider = 0xECu;

namespace actor {
constexpr std::uint32_t kPosition = 0x00Cu;     // LiveActor::mPosition
constexpr std::uint32_t kRotation = 0x018u;     // LiveActor::mRotation
constexpr std::uint32_t kMario = 0x230u;        // MarioActor::mMario
constexpr std::uint32_t kCameraTrans = 0x2F4u;  // MarioActor::mCameraTrans: what the game camera looks at
}  // namespace actor
constexpr std::uint32_t kMarioSideVec = 0x310u;  // Mario::mSideVec
constexpr std::uint32_t kMarioUpVec = 0x1FCu;    // Mario::_1FC, Mario::getUpVec()
constexpr std::uint32_t kMarioFrontVec = 0x208u;  // Mario::mFrontVec
constexpr std::uint32_t kMarioState = 0x97Cu;     // Mario: MarioState* of a special state, 0 in ordinary play
// MarioActor::mModels[6] (J3DModelX*): the power-up models, created when their archive is loaded
// (MarioActorDraw.cpp init*Mario).
constexpr std::uint32_t kActorModels = 0xA28u;
constexpr std::uint32_t kActorTeresaParts = 0x9A4u;  // MarioActor::_9A4: the Boo form's parts
constexpr std::uint32_t kActorPlayerMode = 0x3D4u;  // MarioActor::mPlayerMode (u16)
// ShadowController::_67 (u8): the shadow is past the far clip from the camera (updateFarClipping).
constexpr std::uint32_t kShadowFarClipped = 0x67u;

bool trace_enabled() {
    static const bool enabled = [] {
        char* value = nullptr;
        std::size_t length = 0u;
        const bool set = _dupenv_s(&value, &length, "GALAXY_TRACE_COOP") == 0 && value != nullptr &&
                         value[0] != '\0' && value[0] != '0';
        std::free(value);
        return set;
    }();
    return enabled;
}

void trace(const char* format, ...) {
    if (!trace_enabled()) return;
    std::va_list args;
    va_start(args, format);
    std::fprintf(stderr, "[coop] ");
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
    va_end(args);
}

// Guest addresses of the strings the archive calls take, found once.
struct ArchiveNames {
    bool searched = false;
    std::array<std::uint32_t, 3> mario{};  // model, face, shadow paths
    std::array<std::uint32_t, 3> luigi{};
    const std::array<std::uint32_t, 3>& of(coop::Character character) const {
        return character == coop::Character::Luigi ? luigi : mario;
    }
};

struct State {
    // Arguments of the calls in flight, kept from before the call for after it. KPADRead and the archive
    // calls do not nest; the movement categories and init2 calls are not re-entered by the runtime's own
    // calls (guarded below).
    std::uint32_t kpad_channel = 0xFFFFFFFFu;
    std::uint32_t kpad_buffer = 0u;
    std::uint32_t mount_path = 0u;
    std::uint32_t mount_heap = 0u;
    bool mounting_partner = false;
    std::uint32_t player_archives_is_mario = 1u;
    std::array<std::uint32_t, 8> init2_actors{};
    unsigned init2_depth = 0u;
    std::array<std::uint32_t, 8> categories{};
    unsigned category_depth = 0u;

    // The players. `own` is the character of the game's save (the first player); the second player is
    // the other one.
    coop::Character own = coop::Character::Mario;
    std::uint32_t first = 0u;
    std::uint32_t second = 0u;
    std::uint32_t second_mario = 0u;
    bool creating_second = false;
    bool moving_second = false;
    // During the second player's creation the save is asked which character the player is; the answer
    // is the second player's.
    bool answer_partner = false;

    ArchiveNames names;
    // The Observatory's doors and domes (AstroChangeStageCube areas) test the game's player, the first: when
    // the second player walks into one the players swap places, so the game's own check takes them through.
    // `entrance_name` is the area class's name in the game's data; `entrance_wait` frames between swaps.
    coop::Vec3 parked_position{};
    // The scene's NameObjHolder (every NameObj in the scene, a fixed-size array without a bounds check): the
    // second player's MarioActor and its parts add their own, so it is made larger (kNameObjHolderExtra);
    // `holder_count` counts what is added, for the trace.
    std::uint32_t scene_holder = 0u;
    std::uint32_t holder_capacity = 0u;
    std::uint32_t holder_count = 0u;
    bool second_waiting_input = false;
    bool second_waiting_demo = false;
    std::uint32_t entrance_name = 0u;
    bool entrance_searched = false;
    unsigned entrance_wait = 0u;
    coop::KpadSynth kpad;
    coop::LeashTimer leash;
    unsigned frames = 0u;

    // Split screen. `follow_view` is the second player's own camera (follow, steered and kept out of walls),
    // `camera2` what their half draws with (blended in from the first player's as the split eases in);
    // `layout` the line between the halves. `pass` is 1 or 2 while GameScene::draw3D draws the first or
    // second half, else 0.
    coop::SplitState split;
    coop::FollowCamera follow;
    coop::CameraMatrices follow_view;
    coop::CameraMatrices camera2;
    coop::SplitLayout layout;
    std::uint32_t steer_buttons = 0u;
    float screen_width = 640.0f;
    float screen_height = 456.0f;
    unsigned pass = 0u;
    // A camera event the second player set off (a warp, launch star, pipe): the game camera is theirs while
    // it plays, so their half shows it, and the first player's half holds `last_camera1` (the game camera
    // before the event), moved with them, until the game camera is back after it (CameraHold).
    bool event_second = false;
    unsigned event_inputs = 0u;  // for the trace: what camera_event_is_second last saw
    // When the game camera changes hands (a second player's event begins or ends), the half it moves to
    // blends from that half's own camera into it, so the game camera's swing over from the other player is
    // not seen.
    coop::CameraHandover handover;
    coop::CameraHold hold;
    // The game camera's last view that was the first player's, and where they were then.
    coop::CameraMatrices last_camera1;
    std::array<float, 16> last_projection{};
    coop::Vec3 last_origin{};
    // The second player's half, eased from the event's camera into their own when their event ends;
    // `second_view` is what their half drew last frame.
    coop::CameraHandover second_handover;
    // At the start of the second player's event the game camera travels over from the first player: their
    // half keeps its own camera until the game camera has arrived, then eases into it.
    bool second_awaits_event = false;
    // The second player travels with the first (second_player_waits, the first player parked at the origin, a
    // level start until the first has had control a moment); `controllable_frames` counts frames the first
    // player has been in ordinary play.
    bool travelling = false;
    bool sticky_travel = false;
    bool level_start = false;
    unsigned controllable_frames = 0u;
    coop::CameraMatrices second_view;
    // Trace: frames of per-frame camera detail left after a camera change of hands.
    unsigned camera_trace_frames = 0u;
    std::uint32_t shadow_controller = 0u;
    float shadow_clip = 0.0f;
    std::uint32_t draw_scene = 0u;
    std::uint32_t clip_position = 0u;
    float clip_radius = 0.0f;
    float clip_far = 0.0f;
    // The agent's requests for the second player (coop_request_second_player_*), done in their turn.
    bool agent_teleport = false;
    coop::Vec3 agent_position{};
    int agent_mode = -1;
    // Co-op lives (docs/MULTIPLAYER.md, coop::Lives): each player's down state, the second player's lives (the
    // first's are the game's own), and whether the game's own miss is under way.
    std::array<coop::DownState, 2> down{};
    int second_lives = -1;
    bool game_miss = false;
    bool skip_death_effects = false;  // the rest of this exeGameOver's calls, for a player down alone
    std::uint32_t mario_down_name = 0u;  // "マリオダウン" in the game's data
    bool mario_down_searched = false;
    // HUD: the second player's life meter and lives counter, and the game's own they follow.
    std::uint32_t second_meter = 0u;
    std::uint32_t second_left = 0u;
    std::uint32_t first_meter = 0u;
    std::uint32_t first_left = 0u;
    std::uint32_t hud_call_this = 0u;  // the MeterLayout or PlayerLeft a mirrored call was made on
    std::uint32_t heal_amount = 0u;
    bool heal_second_turn = false;
    std::uint32_t star_swapped = 0u;  // a launch star whose step runs with the second player as the game's
    std::uint32_t pipe_swapped = 0u;  // the same for a pipe the second player goes through
    std::uint32_t pipe_out = 0u;      // the pipe of the exePlayerOut in flight
    std::uint32_t pipe_traced = 0u;
    std::uint32_t pipe_traced_step = 0u;
    // NSMBW-style pipes (pipe_holds): the pipe whose first rider waits inside for the other, and the pipe re-armed
    // for the other to follow through.
    struct Pipe {
        std::uint32_t waiting = 0u;
        std::uint32_t rider = 0u;
        std::uint32_t frames = 0u;
        bool partner_in = false;
        std::uint32_t follow_pipe = 0u;
        std::uint32_t follower = 0u;
        std::uint32_t follow_frames = 0u;
    } pipe;
    // Star bunny code run with the brother nearer the bunny as the game's player (nearer_player_code): the
    // nesting depth of calls into it, whether the outermost one swapped, and to whom it gives back.
    std::uint32_t nearer_depth = 0u;
    bool nearer_swapped = false;
    bool nearer_back_to_second = false;
    // Frames left of a scene (a talk, a bunny's catch) a brother other than the first alone is in: the second is
    // not taken along with the first, who the scene holds.
    std::uint32_t second_scene = 0u;
    // Multiplayer talks (talk_begin): the brother nearest an NPC talks to it; the other waits in place.
    struct Talk {
        std::uint32_t depth = 0u;      // talk calls (54-56) in flight
        bool swapped = false;          // the outermost made the talker the game's player
        bool back_to_second = false;
        bool talker_second = false;    // who the outermost runs as
        bool locked = false;           // a talk under way, and who has it
        bool locked_second = false;
        std::uint32_t candidate_frames = 0u;  // the brother an NPC last asked about, for the talk director
        bool candidate_second = false;
        bool held = false;             // a brother held by the game (readyRemoteDemo), and which
        bool held_second = false;
        bool other_held = false;       // the other brother held (readyRemoteDemo) with the talker
        bool held_talker_second = false;
        bool end_swapped = false;      // endRemoteDemo in flight run as the talker
        bool end_back_to_second = false;
        std::uint32_t end_info = 0u;
        bool calm = false;             // a talk under way without the world frozen (talk_calm)
        bool answering = false;        // answer_partner set for the second player's talk call
        std::uint32_t button = 0u;     // MarioActor::checkButtonType's button, for after it
        std::uint32_t lod = 0u;        // LodCtrl::calcDistanceToCamera's LodCtrl, for after it
        std::uint32_t pad_channel = 0u;   // testCorePad*A's channel, for after it
        std::array<std::uint32_t, 5> pane_args{};  // isStarPointerPointingPane's arguments
    } talk;
    // MR::isNearPlayer* in flight: its actor and distance.
    std::uint32_t near_actor = 0u;
    float near_distance = 0.0f;
    bool miss_counted = false;
    std::string last_stage;  // the previous scene's stage, to see a save loaded (on_stage_start)
    // NameObjArchiveListCollector::addArchive in flight, and the partner archives a collector already has.
    std::uint32_t archive_collector = 0u;
    std::uint32_t archive_name = 0u;
    std::uint32_t partner_archives_collector = 0u;
    std::vector<std::string> partner_archives;
    bool second_riding = false;  // the second player on a launch star: the first flies beside them
    bool second_hidden_for_start = false;
    std::uint32_t star_traced = 0u;
    bool hud_tried = false;
    std::uint32_t hit_point_name = 0u;  // "HitPointMeter" in the game's data
    std::uint32_t second_meter_count = 0u;
    std::uint32_t ring_names = 0u;  // "PicBase", "PicBaseW" in guest memory
    float hud_scale_x = 0.0f;
    float hud_scale_y = 0.0f;
    float hud_offset_x = 0.0f;
    float hud_offset_y = 0.0f;
    // The first player's camera and projection as the game made them, restored after the second half.
    coop::CameraMatrices camera1;
    std::array<float, 16> projection{};
    // The camera's lens, for the second player's clipping.
    float fovy = 45.0f;
    float aspect = 16.0f / 9.0f;
    float far_z = 100000.0f;
    // Star Pointers (docs/MULTIPLAYER.md "Star Pointers"). `camera`, `projection`: what each player's half was
    // last drawn with (index 0 the first player's), while `split`; the game's pointer camera
    // (StarPointerTransformHolder) and CameraContext are given a pointer's half for its target tests.
    struct Pointers {
        bool split = false;
        std::array<coop::CameraMatrices, 2> camera{};
        std::array<std::array<float, 16>, 2> projection{};
        bool holder_swapped = false;  // isStarPointerPointingCore in flight with a half's camera
        std::array<float, 12> holder_view{};
        std::array<float, 16> holder_projection{};
        bool context_swapped = false;  // GCapture's MR::calcScreenPosition in flight with a half's camera
        coop::CameraMatrices context_camera{};
        std::array<float, 16> context_projection{};
        // Pull Stars: the brother whose pointer has them (GCapture runs with him as the game's player).
        bool capture_second = false;
        std::uint32_t capture_depth = 0u;
        bool capture_swapped = false;
        bool capture_back_to_second = false;
        bool bind_refused = false;  // the other brother's bind skipped, for after it
        bool spine_swapped = false;  // a Pull Star nerve step in flight run as the capturing brother
        bool spine_back_to_second = false;
        std::uint32_t gcapture = 0u;
        std::array<std::uint32_t, 4> pointing_args{};  // MR::isStarPointerPointing's, for after it
        bool pointing_target = false;  // ... called from GCaptureTarget
    } pointers;
};

bool split_screen_enabled() {
    static const bool enabled = [] {
        char* value = nullptr;
        std::size_t length = 0u;
        const bool off = _dupenv_s(&value, &length, "GALAXY_COOP_SPLIT") == 0 && value != nullptr && value[0] == '0';
        std::free(value);
        return !off;
    }();
    return enabled;
}

State& state() {
    static State s;
    return s;
}

// native_runtime.cpp puts split screen's line in the GX stream (coop_set_split_clip_sink).
struct SplitClipSinkSlot {
    SplitClipSink sink = nullptr;
    void* user = nullptr;
};
SplitClipSinkSlot& split_clip_sink() {
    static SplitClipSinkSlot slot;
    return slot;
}
void send_split_clip(const std::array<float, 3>& plane, unsigned pass,
                     const std::array<float, 4>& separator = {0.0f, 0.0f, 0.0f, 0.0f}) {
    const SplitClipSinkSlot& slot = split_clip_sink();
    if (slot.sink != nullptr) slot.sink(slot.user, plane.data(), pass, separator.data());
}

std::uint32_t camera_context(const Guest& guest) {
    std::uint32_t object = guest.u32(guest.context()->gpr[13] - 0x3A78u);
    for (const std::uint32_t offset : {0x24u, 0xACu, 0x10u, 0x17u * 4u}) {
        if (!plausible(object)) return 0u;
        object = guest.u32(object + offset);
    }
    return plausible(object) ? object : 0u;
}

template <std::size_t N>
std::array<float, N> read_floats(const Guest& guest, std::uint32_t address) {
    std::array<float, N> values{};
    for (std::size_t i = 0; i < N; ++i) values[i] = guest.f32(address + static_cast<std::uint32_t>(4u * i));
    return values;
}

template <std::size_t N>
void write_floats(const Guest& guest, std::uint32_t address, const std::array<float, N>& values) {
    for (std::size_t i = 0; i < N; ++i) guest.set_f32(address + static_cast<std::uint32_t>(4u * i), values[i]);
}

void write_camera(const Guest& guest, std::uint32_t context, const coop::CameraMatrices& matrices) {
    write_floats(guest, context + camera::kView, matrices.view);
    write_floats(guest, context + camera::kInverse, matrices.inverse);
}

coop::CameraMatrices read_camera(const Guest& guest, std::uint32_t context) {
    return {read_floats<12>(guest, context + camera::kView), read_floats<12>(guest, context + camera::kInverse)};
}

bool finite(const std::array<float, 12>& values) {
    for (const float value : values) {
        if (!std::isfinite(value)) return false;
    }
    return true;
}


coop::Vec3 player_up(const Guest& guest, std::uint32_t mario) {
    return coop::normalized(guest.vec(mario + kMarioUpVec), {0.0f, 1.0f, 0.0f});
}

std::uint8_t character_byte(coop::Character character) {
    return character == coop::Character::Luigi ? 1u : 0u;
}

void ensure_archive_names(const Guest& guest, State& s) {
    if (s.names.searched) return;
    s.names.searched = true;
    for (const coop::Character character : {coop::Character::Mario, coop::Character::Luigi}) {
        const coop::CharacterArchives archives = coop::archives_of(character);
        auto& names = character == coop::Character::Luigi ? s.names.luigi : s.names.mario;
        names = {guest.find_string(archives.model_path), guest.find_string(archives.face_path),
                 guest.find_string(archives.shadow_path)};
    }
    trace("archive names: Mario %08X %08X %08X, Luigi %08X %08X %08X", s.names.mario[0], s.names.mario[1],
          s.names.mario[2], s.names.luigi[0], s.names.luigi[1], s.names.luigi[2]);
}

// After the game mounts its player's model archive, the other character's model, face and shadow are
// mounted into the same heap (the stationed-archive loader's thread).
void mount_partner_archives(const Guest& guest, State& s) {
    coop::Character own_model = coop::Character::Mario;
    if (!coop::own_model_archive(guest.string(s.mount_path), own_model)) return;
    ensure_archive_names(guest, s);
    const coop::Character partner = coop::partner_of(own_model);
    SavedContext saved(guest.context());
    s.mounting_partner = true;
    for (const std::uint32_t path : s.names.of(partner)) {
        if (path == 0u) continue;
        const std::uint32_t mounted = guest.call(fn::kMountArchive, path, s.mount_heap);
        trace("mounted %s into heap %08X: %08X", guest.string(path).c_str(), s.mount_heap, mounted);
    }
    s.mounting_partner = false;
}

// After the game makes the resource holders of its player's archives, the same for the other character.
void add_partner_resources(const Guest& guest, State& s) {
    ensure_archive_names(guest, s);
    const coop::Character own = s.player_archives_is_mario != 0u ? coop::Character::Mario : coop::Character::Luigi;
    const std::uint32_t manager = guest.u32(kResourceHolderManager);
    if (!plausible(manager)) return;
    SavedContext saved(guest.context());
    for (const std::uint32_t path : s.names.of(coop::partner_of(own))) {
        if (path == 0u) continue;
        const std::uint32_t holder = guest.call(fn::kCreateAndAddStationed, manager, path);
        trace("resource holder for %s: %08X", guest.string(path).c_str(), holder);
    }
}

// Right after the game sets up its player, the second player: the same MarioActor as the other character,
// beside the first, out of the movement list.
void create_hud(const Guest& guest, State& s);
void on_stage_start(const Guest& guest, State& s);
void tint_second_meter(const Guest& guest, const State& s);

void create_second_player(const Guest& guest, State& s, std::uint32_t first) {
    SavedContext saved(guest.context());
    s.first = first;
    s.second = 0u;
    s.second_mario = 0u;
    s.kpad.reset();
    s.leash.reset();
    s.split.reset();
    // Until the first player has landed and has control, the second waits where they were placed.
    s.level_start = true;
    s.travelling = true;
    s.sticky_travel = false;
    s.controllable_frames = 0u;
    s.follow.reset();
    // A new stage: both players in play (one out of lives comes back), its own HUD.
    s.down = {};
    s.game_miss = false;
    s.miss_counted = false;
    s.second_meter = s.second_left = s.first_meter = s.first_left = 0u;
    s.hud_tried = false;
    s.star_swapped = 0u;
    s.pipe_swapped = 0u;
    s.pipe = {};
    s.nearer_depth = 0u;
    s.nearer_swapped = false;
    s.second_scene = 0u;
    s.talk = {};
    s.pointers = {};
    s.second_hidden_for_start = false;
    const std::uint32_t first_mario = guest.u32(first + actor::kMario);
    if (!plausible(first_mario)) return;
    const coop::Vec3 position = guest.vec(first + actor::kPosition);
    const coop::Vec3 rotation = guest.vec(first + actor::kRotation);
    const coop::Vec3 side = guest.vec(first_mario + kMarioSideVec);
    const coop::Vec3 beside = position + side * 120.0f;
    s.parked_position = beside;

    // The actor's name: the debug name the game gives its own player is unknown, so the archive path does.
    ensure_archive_names(guest, s);
    const std::uint32_t name = s.names.of(coop::partner_of(s.own))[0];
    s.creating_second = true;
    // The stage heaps' room before and after (a second player takes about 600 KB of MEM1).
    const std::uint32_t watcher = guest.u32(kHeapMemoryWatcher);
    const auto free_kb = [&](std::uint32_t field) {
        const std::uint32_t heap = plausible(watcher) ? guest.u32(watcher + field) : 0u;
        return heap != 0u ? guest.u32(heap + kSolidHeapFreeSize) / 1024u : 0u;
    };
    trace("stage heaps before the second player: MEM1 %u KB free, MEM2 %u KB free", free_kb(kSceneHeapNapa),
          free_kb(kSceneHeapGddr));
    const std::uint32_t second = guest.call(fn::kCreateMarioActor, name);
    if (plausible(second)) {
        Guest::Frame frame(guest);
        s.answer_partner = true;
        guest.call(fn::kMarioActorInit2, second, frame.vec(0, beside), frame.vec(1, rotation), 1u);  // 1: 基本
        s.answer_partner = false;
    }
    trace("stage heaps after the second player: MEM1 %u KB free, MEM2 %u KB free", free_kb(kSceneHeapNapa),
          free_kb(kSceneHeapGddr));
    s.creating_second = false;
    // init2 made the new actor the scene's player and set gIsLuigi to its character: back to the first.
    guest.set_u8(kIsLuigi, character_byte(s.own));
    const std::uint32_t holder = guest.call(fn::kGetMarioHolder);
    if (plausible(holder)) guest.call(fn::kSetMarioActor, holder, first);
    guest.call(fn::kSetCameraTargetToPlayer);
    if (!plausible(second)) {
        trace("second player not created (%08X)", second);
        return;
    }
    guest.call(fn::kRequestSuspend, second);
    s.second = second;
    s.second_mario = guest.u32(second + actor::kMario);
    on_stage_start(guest, s);
    // The second player's HUD is made with the stage's own objects: layouts made once the stage runs are not
    // moved or drawn.
    create_hud(guest, s);
    trace("players: first %08X (%s), second %08X (%s)", first, s.own == coop::Character::Luigi ? "Luigi" : "Mario",
          second, s.own == coop::Character::Luigi ? "Mario" : "Luigi");
}

// Swaps WPadHolder's WPads of channels 0 and 1, so the game's player code (which asks MR::getWPad for
// channel 0) reads the second player's controller. Swapping twice restores them.
void swap_pads(const Guest& guest, std::uint32_t pad_holder) {
    const std::uint32_t pad0 = guest.u32(pad_holder);
    guest.set_u32(pad_holder, guest.u32(pad_holder + 4u));
    guest.set_u32(pad_holder + 4u, pad0);
}

// The players do not pass through each other: when they overlap, each is pushed half the way apart.
void bump_players(const Guest& guest, const State& s, coop::Vec3 first_position, coop::Vec3 second_position) {
    const std::uint32_t first_mario = guest.u32(s.first + actor::kMario);
    if (!plausible(first_mario) || !plausible(s.second_mario)) return;
    coop::Vec3 up = guest.vec(first_mario + kMarioUpVec);
    const float up_length = coop::length(up);
    if (!std::isfinite(up_length) || up_length < 0.5f) return;
    up = up * (1.0f / up_length);
    const coop::Vec3 push = coop::body_push(first_position, second_position, up);
    if (push.x == 0.0f && push.y == 0.0f && push.z == 0.0f) return;
    Guest::Frame frame(guest);
    guest.call(fn::kMarioPush, s.second_mario, frame.vec(0, push));
    guest.call(fn::kMarioPush, first_mario, frame.vec(1, push * -1.0f));
}

// The game camera's player: CameraDirector +0x34 CameraTargetHolder, +0xC its CameraTargetPlayer, whose
// MarioActor is at +0x10 (Petari CameraDirector.hpp, CameraTargetHolder.hpp, CameraTargetObj.hpp).
// While the second player moves, the game's player is them, so a warp, launch star, pipe or event camera
// they set off (MR::setCameraTargetToPlayer, CameraTargetArg::setTarget) points the camera at them. While
// that event camera plays it stays theirs (event_second: their half shows it); after, the camera is the
// first player's again.
void keep_camera_on_first_player(const Guest& guest, State& s) {
    const std::uint32_t director = guest.call(fn::kGetCameraDirector);
    if (!plausible(director)) return;
    const std::uint32_t holder = guest.u32(director + 0x34u);
    if (!plausible(holder)) return;
    const std::uint32_t target = guest.u32(holder + 0xCu);
    if (!plausible(target)) return;
    const std::uint32_t actor = guest.u32(target + 0x10u);
    const bool event = (guest.call(fn::kIsEventCameraActive) & 0xFFu) != 0u;
    const bool was = s.event_second;
    // An event set off by the second player's own move points the camera at them; one an object starts for
    // them (a launch star, a warp pad) points it at the first player, but then only the second player is
    // in a special state (flying, warping).
    const std::uint32_t first_mario = guest.u32(s.first + actor::kMario);
    const bool first_special = plausible(first_mario) && guest.u32(first_mario + kMarioState) != 0u;
    const bool second_special = plausible(s.second_mario) && guest.u32(s.second_mario + kMarioState) != 0u;
    // At a level start every camera is the first player's (the intro ends where the second player waits).
    s.event_second = !s.level_start && coop::camera_event_is_second(actor, s.second, event, second_special, first_special);
    const unsigned inputs = (event ? 1u : 0u) | (actor == s.second ? 2u : actor == s.first ? 4u : 0u) |
                            (second_special ? 8u : 0u) | (first_special ? 16u : 0u);
    if (inputs != s.event_inputs) {
        trace("camera event %d, camera on %s, special: second %d first %d (split %.2f)", event ? 1 : 0,
              actor == s.second ? "second" : actor == s.first ? "first" : "other", second_special ? 1 : 0,
              first_special ? 1 : 0, s.split.amount());
        s.event_inputs = inputs;
        s.camera_trace_frames = 120u;
    }
    if (s.event_second != was) {
        trace("second player's camera event %s", s.event_second ? "begins" : "ends");
        s.camera_trace_frames = 120u;
        if (s.event_second && s.split.amount() > 0.0f) {
            // Their half blends from their own camera into the event's; the first player's half holds the
            // game camera's view from before the event.
            // The event camera aims at the first player for its first frames (until it follows the second),
            // so their half keeps its own view a moment before easing in.
            s.second_awaits_event = true;
            s.hold.start(s.last_camera1, s.last_origin, s.last_projection);
        } else if (!s.event_second && s.split.amount() > 0.0f) {
            // Their half eases from the event's camera back into their own.
            s.second_handover.start(s.second_view, true);
        }
    }
    // The first player's half holds on past the event while the second player is still in flight (a launch
    // star's flight outlasts its event camera, and the game camera follows the flight); once they land it
    // waits for the game camera to be back.
    if (!s.event_second && s.hold.active() && !s.hold.returning() && !second_special) {
        s.hold.release();
        s.camera_trace_frames = 120u;
        trace("second player landed: the first player's half waits for the game camera");
    }
    if (s.event_second) {
        // The event's camera follows the second player while it plays.
        if (actor != s.second) guest.set_u32(target + 0x10u, s.second);
        return;
    }
    const std::uint32_t kept = coop::camera_player(actor, s.first, s.second);
    if (kept == actor) return;
    guest.set_u32(target + 0x10u, kept);
    trace("camera target was the second player (%08X): back to the first (%08X)", actor, kept);
}

// While split, after both players moved: the second player's own camera (it trails them, turned and raised
// by their controller, recentred by C, and kept in front of walls), and where the line between the halves
// runs: square to where the second player is from the first on the first player's screen.
void update_second_camera(const Guest& guest, State& s, coop::Vec3 first_position, coop::Vec3 second_position) {
    const std::uint32_t context = camera_context(guest);
    if (context == 0u || !plausible(s.second_mario)) return;
    const coop::CameraMatrices first_camera = read_camera(guest, context);
    if (!finite(first_camera.view) || !finite(first_camera.inverse)) return;
    const coop::SplitTuning tuning{};
    const coop::Vec3 up = player_up(guest, s.second_mario);
    if (!s.follow.started()) {
        // The split begins: the second player's camera starts as the shared one, moved onto them.
        const coop::Vec3 midpoint = first_position + (second_position - first_position) * 0.5f;
        s.follow.start(coop::pose_of(first_camera.inverse).eye, midpoint, second_position, up, tuning);
        trace("split begins");
    }
    const coop::PartnerInput input = coop_partner_input();
    const coop::CameraSteer steer = coop::camera_steer(input, s.steer_buttons);
    s.steer_buttons = input.connected ? input.buttons : 0u;
    if (steer.recentre) s.follow.recentre(second_position, guest.vec(s.second_mario + kMarioFrontVec), tuning);
    s.follow.steer(steer.turn, steer.raise, tuning);
    s.follow_view = s.follow.update(second_position, up, tuning);

    // Camera collision: the first wall between the point the camera looks at and its eye.
    const coop::Vec3 look = s.follow.look();
    const coop::Vec3 eye = s.follow.eye();
    {
        Guest::Frame frame(guest);
        const std::uint32_t hit = frame.vec(0, eye);
        const std::uint32_t found =
            guest.call(fn::kFirstPolyOnLineToMap, hit, frame.scratch(), frame.vec(1, look), frame.vec(2, eye - look));
        if ((found & 0xFFu) != 0u) {
            const coop::Vec3 wall = guest.vec(hit);
            if (std::isfinite(coop::length(wall))) {
                s.follow_view = coop::look_at(coop::camera_before_wall(look, wall, tuning.wall_margin), s.follow.up(), look);
            }
        }
    }

    // The line follows the first player's view as their half shows it: while it is held (the game camera is
    // the second player's event or warp camera) the held view, so the line does not swing with that camera.
    const coop::CameraMatrices layout_camera = s.hold.active() ? s.hold.view(first_position) : first_camera;
    const coop::Vec3 a = coop::transform_point(layout_camera.view, first_position);
    const coop::Vec3 b = coop::transform_point(layout_camera.view, second_position);
    s.layout.update(b.x - a.x, a.y - b.y);  // screen y runs down
}

// How far towards the edge of a shared picture the players would be, with the game camera's turn and
// distance (from the midpoint it looks at while the screen is one, from the first player once split).
float shared_extent(const Guest& guest, const State& s, coop::Vec3 first_position, coop::Vec3 second_position) {
    const std::uint32_t context = camera_context(guest);
    if (context == 0u) return 0.0f;
    const coop::CameraMatrices camera = read_camera(guest, context);
    if (!finite(camera.view) || !finite(camera.inverse)) return 0.0f;
    float fovy = guest.f32(context + camera::kFovy);
    float aspect = guest.f32(context + camera::kAspect);
    if (!std::isfinite(fovy) || fovy <= 1.0f || fovy >= 170.0f) fovy = s.fovy;
    if (!std::isfinite(aspect) || aspect <= 0.1f) aspect = s.aspect;
    const coop::CameraPose pose = coop::pose_of(camera.inverse);
    const coop::Vec3 looked_at =
        s.split.split() ? first_position : first_position + (second_position - first_position) * 0.5f;
    const float depth = coop::dot(pose.forward, looked_at - pose.eye);
    return coop::shared_view_extent(camera, first_position, second_position, depth, fovy, aspect);
}

// One screen again: nothing held.
void remember_first_camera(const Guest& guest, State& s) {
    (void)guest;
    if (s.split.amount() == 0.0f) s.hold.reset();
}

// After the game moves its players, the second player: one MarioActor::movement with the game's player,
// character and controller switched to it. Then the shared camera, and the warp back to the first player.
// Whether a player rides a launch star (the launcher they rush with is a SuperSpinDriver or SpinDriver).
bool rides_star(const Guest& guest, std::uint32_t actor_address) {
    const std::uint32_t sensor = guest.u32(actor_address + kMarioRushSensor);
    if (!plausible(sensor)) return false;
    const std::uint32_t host = guest.u32(sensor + kSensorHost);
    if (!plausible(host)) return false;
    const std::uint32_t vtable = guest.u32(host);
    return vtable == kSuperSpinDriverVtable || vtable == kSpinDriverVtable;
}

// A power-up archive of the first player's character added to a stage's archive list: the second player's
// as well, so their power-up models are made (MarioActor::init*Mario, which want the archive loaded) and they
// can take the power-up.
constexpr std::array<std::pair<std::string_view, std::string_view>, 7> kPowerUpArchives = {{
    {"BeeMario", "BeeLuigi"},
    {"BoneMario", "BoneLuigi"},
    {"HopperMario", "HopperLuigi"},
    {"IceMario", "IceLuigi"},
    {"InvincibleMario", "InvincibleLuigi"},
    {"TeresaMario", "TeresaLuigi"},
    {"MarioTornado", "LuigiTornado"},
}};

void add_partner_archive(const Guest& guest, State& s) {
    const std::uint32_t collector = s.archive_collector;
    const std::uint32_t name_address = s.archive_name;
    s.archive_collector = s.archive_name = 0u;
    if (!plausible(collector) || !plausible(name_address & ~3u)) return;
    const std::string name = guest.string(name_address, 64u);
    const bool own_mario = s.own == coop::Character::Mario;
    for (const auto& [mario, luigi] : kPowerUpArchives) {
        const std::string_view own = own_mario ? mario : luigi;
        const std::string_view partner = own_mario ? luigi : mario;
        if (name.size() < own.size() || std::string_view(name).substr(0u, own.size()) != own) continue;
        const std::string wanted = std::string(partner) + name.substr(own.size());  // keeps a ".arc"
        if (s.partner_archives_collector != collector) {
            s.partner_archives_collector = collector;
            s.partner_archives.clear();
        }
        if (std::find(s.partner_archives.begin(), s.partner_archives.end(), wanted) != s.partner_archives.end()) return;
        s.partner_archives.push_back(wanted);
        const std::uint32_t partner_name = guest.find_string(wanted);
        if (partner_name == 0u) {
            trace("partner power-up archive %s: name not in the game's data", wanted.c_str());
            return;
        }
        SavedContext saved(guest.context());
        guest.call(fn::kAddArchive, collector, partner_name);
        trace("stage archives: %s with %s", wanted.c_str(), name.c_str());
        return;
    }
}

// Makes the second player (or the first again) the game's player: MarioHolder, gIsLuigi and the channel 0 and 1
// pads, as move_second_player does for the second player's turn.
void set_game_player(const Guest& guest, State& s, bool second) {
    const std::uint32_t holder = guest.call(fn::kGetMarioHolder);
    if (!plausible(holder)) return;
    const std::uint32_t pad_holder = guest.call(fn::kGetWPadHolder);
    const bool pads = plausible(pad_holder) && plausible(guest.u32(pad_holder)) && plausible(guest.u32(pad_holder + 4u));
    guest.set_u8(kIsLuigi, character_byte(second ? coop::partner_of(s.own) : s.own));
    guest.call(fn::kSetMarioActor, holder, second ? s.second : s.first);
    if (pads) swap_pads(guest, pad_holder);
    s.moving_second = second;
}

// Puts a player (0 the first, 1 the second) at a position, made the game's player for MR::setPlayerPos.
void place_player(const Guest& guest, State& s, unsigned who, coop::Vec3 at) {
    SavedContext saved(guest.context());
    const bool was_second = s.moving_second;
    const bool second = who == 1u;
    if (second != was_second) set_game_player(guest, s, second);
    {
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, at));
    }
    if (second != was_second) set_game_player(guest, s, was_second);
}

// ---- NSMBW-style pipes (docs/MULTIPLAYER.md "Pipes") ----
// An EarthenPipe takes one rider through: Ready, PlayerIn (in, then over to its partner pipe), PlayerOut (out).
// The first brother in waits inside, his PlayerIn held, until the other reaches the mouth or kPipeWaitFrames
// pass; once he is out the pipe takes riders again with the other on its mouth, who goes through right behind.

coop::Vec3 pipe_mouth(const Guest& guest, std::uint32_t pipe) {
    const std::uint32_t mtx = guest.u32(pipe + kPipeTopMtx);
    if (!plausible(mtx)) return guest.vec(pipe + actor::kPosition);
    return {guest.f32(mtx + 0x0Cu), guest.f32(mtx + 0x1Cu), guest.f32(mtx + 0x2Cu)};
}

// Before exePlayerIn: whether its rider, in and still (the enter animation done), waits for the other.
bool pipe_holds(const Guest& guest, State& s, std::uint32_t pipe, std::uint32_t rider) {
    if (rider != s.first && rider != s.second) return false;
    if (s.pipe.follow_pipe == pipe && rider == s.pipe.follower) return false;  // the other's own way through
    const unsigned other = rider == s.second ? 0u : 1u;
    if (s.down[other].down) return false;
    bool inside = false;
    {
        SavedContext saved(guest.context());  // the step's own arguments stay
        inside = (guest.call(fn::kIsBckStopped, rider) & 0xFFu) != 0u;
    }
    if (!inside) return false;
    if (s.pipe.waiting != pipe) {
        s.pipe.waiting = pipe;
        s.pipe.rider = rider;
        s.pipe.frames = 0u;
        s.pipe.partner_in = false;
        trace("pipe %08X: the %s player waits inside for the other", pipe, other == 1u ? "first" : "second");
    }
    if (s.pipe.partner_in) return false;
    const coop::Vec3 at = guest.vec((other == 1u ? s.second : s.first) + actor::kPosition);
    const bool arrived = coop::length(at - pipe_mouth(guest, pipe)) < coop::kPipeJoinDistance;
    if (!arrived && ++s.pipe.frames < coop::kPipeWaitFrames) return true;
    s.pipe.partner_in = true;  // either way the other follows through
    trace("pipe %08X: %s; through", pipe, arrived ? "the other is at the mouth" : "waited long enough");
    return false;
}

// After exePlayerOut: once the rider is out, the other goes in after him.
void pipe_after_exit(const Guest& guest, State& s, std::uint32_t pipe) {
    if (guest.u32(pipe + kPipeRider) != 0u) return;
    if (s.pipe.follow_pipe == pipe) {
        s.pipe.follow_pipe = s.pipe.follower = 0u;
        trace("pipe %08X: both through", pipe);
        return;
    }
    if (s.pipe.waiting != pipe) return;
    const unsigned other = s.pipe.rider == s.second ? 0u : 1u;
    s.pipe.waiting = 0u;
    const std::uint32_t follower = other == 1u ? s.second : s.first;
    if (s.down[other].down || !plausible(follower)) return;
    SavedContext saved(guest.context());
    guest.call(fn::kSetNerve, pipe, fn::kPipeNrvWait);
    guest.call(fn::kValidateHitSensors, pipe);
    place_player(guest, s, other, pipe_mouth(guest, pipe) + guest.vec(pipe + kPipeUp) * 20.0f);
    s.pipe.follow_pipe = pipe;
    s.pipe.follower = follower;
    s.pipe.follow_frames = 0u;
    trace("pipe %08X: the %s player follows", pipe, other == 1u ? "second" : "first");
}

// After MR::isNearPlayer*: the other brother near counts too.
bool near_other_player(const Guest& guest, const State& s) {
    if (!plausible(s.near_actor) || !plausible(s.first) || !plausible(s.second)) return false;
    const unsigned other = s.moving_second ? 0u : 1u;
    if (s.down[other].down) return false;
    const coop::Vec3 at = guest.vec(s.near_actor + actor::kPosition);
    const coop::Vec3 player = guest.vec((other == 1u ? s.second : s.first) + actor::kPosition);
    return std::isfinite(s.near_distance) && coop::length(player - at) < s.near_distance;
}

// A call into a star bunny's code from outside it (its nerve steps, sensor answers, catch): the code runs with the
// brother nearer the bunny as the game's player, so the one who comes up to it is the one it talks to, runs
// from and is caught by (the caught scene binds him), and the other is left where he is.
void nearer_player_code(const Guest& guest, State& s) {
    const std::uint32_t actor = guest.context()->gpr[3];
    if (!plausible(actor) || !plausible(s.first) || !plausible(s.second) || s.down[1].down) return;
    const std::uint32_t vtable = guest.u32(actor);
    if (vtable < 0x8059E000u || vtable >= 0x805A2000u) return;  // the rabbits' vtables (not a constructor)
    const coop::Vec3 at = guest.vec(actor + actor::kPosition);
    const bool second = s.down[0].down ||
        coop::length(guest.vec(s.second + actor::kPosition) - at) < coop::length(guest.vec(s.first + actor::kPosition) - at);
    if (second) s.second_scene = 30u;
    if (second == s.moving_second) return;
    SavedContext saved(guest.context());
    s.nearer_back_to_second = s.moving_second;
    set_game_player(guest, s, second);
    s.nearer_swapped = true;
}

// ---- Multiplayer talks (docs/MULTIPLAYER.md "Talks") ----
// The brother nearest an NPC is the one it offers a talk to (the A prompt is his, and his A starts it); the talk
// then plays on him: the NPC turns to him, he takes the talk pose, the camera frames him, and lines that ask
// whether the player is Luigi answer for him. Either brother's A pages the text, and either chooses yes or no.
// The other brother is held where he stands for the talk (readyRemoteDemo, as the game holds its player) and
// let go with the talker.

bool second_nearer(const Guest& guest, const State& s, std::uint32_t actor) {
    if (s.down[1].down) return false;
    if (s.down[0].down) return true;
    const coop::Vec3 at = guest.vec(actor + actor::kPosition);
    return coop::length(guest.vec(s.second + actor::kPosition) - at) <
           coop::length(guest.vec(s.first + actor::kPosition) - at);
}

// Before an outermost talk call: run it as `second` (or the first), recording how to give the game back.
void talk_swap(const Guest& guest, State& s, bool second) {
    s.talk.talker_second = second;
    // The text's player name and its Luigi-only lines ask the save's character (MR::isPlayerLuigi asks
    // GameDataFunction::isDataMario): they answer for the talker.
    if (second && !s.answer_partner) {
        s.answer_partner = true;
        s.talk.answering = true;
    }
    if (second) s.second_scene = 30u;
    if (second == s.moving_second) return;
    SavedContext saved(guest.context());
    s.talk.back_to_second = s.moving_second;
    set_game_player(guest, s, second);
    s.talk.swapped = true;
}

// Before MR::tryTalk* (the NPC's own talk calls; `host` the NPC): the talker under way, or the nearer brother.
void talk_begin(const Guest& guest, State& s, std::uint32_t host) {
    if (!plausible(s.first) || !plausible(s.second)) return;
    bool second = false;
    if (s.talk.locked) {
        second = s.talk.locked_second;
    } else if (plausible(host)) {
        second = second_nearer(guest, s, host);
        s.talk.candidate_second = second;
        s.talk.candidate_frames = 2u;
    } else {
        return;
    }
    talk_swap(guest, s, second);
}

// Before TalkDirector::movement's steps (the A prompt, the talk's start, paging, its end; `director` its r3): its
// talker, or else the brother nearer the NPC the director has in hand (its talk state's, else its request's). Not
// the last NPC that asked: in split screen NPCs near each brother ask every frame.
void talk_director_begin(const Guest& guest, State& s, std::uint32_t director) {
    if (!plausible(s.first) || !plausible(s.second)) return;
    if (s.talk.locked) {
        s.second_scene = 30u;  // nobody is taken along while a talk is under way
        talk_swap(guest, s, s.talk.locked_second);
        return;
    }
    std::uint32_t ctrl = 0u;
    if (plausible(director)) {
        const std::uint32_t state = guest.u32(director + 0x48u);  // TalkDirector::mTalkState
        ctrl = plausible(state) ? guest.u32(state + 4u) : 0u;    // TalkState::_04, its TalkMessageCtrl
        if (!plausible(ctrl)) ctrl = guest.u32(director + 0x3Cu);  // the request in hand
        if (!plausible(ctrl)) ctrl = guest.u32(director + 0x38u);  // TalkDirector::mMsgCtrl
    }
    const std::uint32_t host = plausible(ctrl) ? guest.u32(ctrl + 0xCu) : 0u;  // TalkMessageCtrl::mHostActor
    if (plausible(host)) {
        talk_swap(guest, s, second_nearer(guest, s, host));
    } else if (s.talk.candidate_frames != 0u) {
        talk_swap(guest, s, s.talk.candidate_second);
    }
}

// After the outermost talk call: the game back as it was; a talk that has started is locked to its talker, one
// that has ended is let go.
void talk_end(const Guest& guest, State& s) {
    SavedContext saved(guest.context());
    if (s.talk.answering) {
        s.answer_partner = false;
        s.talk.answering = false;
    }
    if (s.talk.swapped) {
        set_game_player(guest, s, s.talk.back_to_second);
        s.talk.swapped = false;
    }
    const bool talking = (guest.call(fn::kIsSystemTalking) & 0xFFu) != 0u;
    if (talking && !s.talk.locked) {
        s.talk.locked = true;
        s.talk.locked_second = s.talk.talker_second;
        trace("talk: the %s player talks", s.talk.locked_second ? "second" : "first");
    } else if (!talking && s.talk.locked) {
        s.talk.locked = false;
        trace("talk over");
    }
}

// After MarioAccess::readyRemoteDemo inside a talk call: the other brother is held too.
void talk_hold_other(const Guest& guest, State& s) {
    if (s.talk.depth == 0u || s.talk.other_held || !plausible(s.first) || !plausible(s.second)) return;
    SavedContext saved(guest.context());
    const bool talker = s.moving_second;
    set_game_player(guest, s, !talker);
    guest.call(fn::kReadyRemoteDemo);
    set_game_player(guest, s, talker);
    s.talk.other_held = true;
    s.talk.held_talker_second = talker;
}

// After MarioAccess::endRemoteDemo (run as the talker, see coop_before_call): the other brother let go too.
void talk_release_other(const Guest& guest, State& s) {
    SavedContext saved(guest.context());
    if (s.talk.end_swapped) {
        set_game_player(guest, s, s.talk.end_back_to_second);
        s.talk.end_swapped = false;
    }
    if (!s.talk.other_held) return;
    s.talk.other_held = false;
    const bool was = s.moving_second;
    set_game_player(guest, s, !s.talk.held_talker_second);
    guest.call(fn::kEndRemoteDemo, s.talk.end_info);
    set_game_player(guest, s, was);
}

// After MR::testCorePadButtonA/TriggerA(0) from TalkStateEvent::talk (paging): the other brother's A too.
// Channel 1 is the other brother's however the pads are swapped.
void talk_either_a(const Guest& guest, const State& s, PpcContext* context, std::uint32_t function,
                   std::uint32_t guest_pc) {
    if (guest_pc < 0x802A25A0u || guest_pc >= 0x802A2700u || s.talk.pad_channel != 0u) return;
    if ((context->gpr[3] & 0xFFu) != 0u) return;
    std::uint32_t other = 0u;
    {
        SavedContext saved(context);
        other = guest.call(function, 1u) & 0xFFu;
    }
    context->gpr[3] = other;
}

// After the yes/no choice's pointer and A tests (ButtonPaneController), during a talk: the other brother's too.
void talk_either_choice(const Guest& guest, const State& s, PpcContext* context, bool pane, std::uint32_t guest_pc) {
    if (guest_pc < 0x8034BE84u || guest_pc >= 0x8034C770u || (context->gpr[3] & 0xFFu) != 0u) return;
    if (pane && s.talk.pane_args[2] != 0u) return;
    std::uint32_t other = 0u;
    {
        SavedContext saved(context);
        if ((guest.call(fn::kIsSystemTalking) & 0xFFu) == 0u) return;
        if (pane) {
            const auto& a = s.talk.pane_args;
            other = guest.call(fn::kPointingPane, a[0], a[1], 1u, a[3], a[4]) & 0xFFu;
        } else {
            const std::uint32_t pad_holder = guest.call(fn::kGetWPadHolder);
            if (!plausible(pad_holder) || !plausible(guest.u32(pad_holder)) || !plausible(guest.u32(pad_holder + 4u))) return;
            swap_pads(guest, pad_holder);
            other = guest.call(fn::kMenuDecide) & 0xFFu;
            swap_pads(guest, pad_holder);
        }
    }
    context->gpr[3] = other;
}

// Co-op talks do not freeze the world (docs/MULTIPLAYER.md "Talks"). A normal talk's freeze is its demo, started
// in TalkDirector::prepTalk (the scene stopped but for the player, the HUD away, the player held), the talker's
// talk pose and the talk camera; the talk window, its pages, choices and the NPC's event callbacks need none of
// them. So, by call site: no demo, pose or camera (and none of their ends), the window opens at once, demos the
// talk sets off (a Power Star appearing, a Luma turning into a launch star) wait for its end as they would have,
// and A pages instead of jumping. Talks inside a cutscene (pauseTimeKeepDemo) and short balloons are left alone.
bool talk_calm_skips(State& s, std::uint32_t id, std::uint32_t guest_pc) {
    switch (id) {
    case call::kTalkDemoStart:
        if (guest_pc != 0x8029EF08u) return false;
        if (!s.talk.calm) trace("talk: the world keeps going");
        s.talk.calm = true;
        return true;
    case call::kStartPlayerTalk: return s.talk.calm && guest_pc == 0x8029EF34u;
    case call::kEndPlayerTalk: return s.talk.calm && guest_pc == 0x8029EFE4u;
    case call::kEndDemo:
        if (!s.talk.calm || guest_pc != 0x8029F01Cu) return false;
        s.talk.calm = false;  // termTalk's last step: the talk is over
        return true;
    case call::kTalkCamera: return s.talk.calm && guest_pc == 0x802A2544u;
    case call::kTalkCameraEnd: return s.talk.calm && (guest_pc == 0x8029EF90u || guest_pc == 0x8029EFC0u);
    default: return false;
    }
}

// After MR::canStartDemo, from the demo request paths (DemoDirector::tryStartDemoRequested, requestStartDemo,
// requestStartTimeKeepDemo): not while a calm talk is open, so its demos come after it.
bool demo_request_site(std::uint32_t guest_pc) {
    switch (guest_pc) {
    case 0x800BA61Cu: case 0x800BE10Cu: case 0x800BE23Cu: case 0x800BE370u:
    case 0x800BE48Cu: case 0x800BE5A8u: case 0x800BE6BCu:
        return true;
    default:
        return false;
    }
}

// After LodCtrl::calcDistanceToCamera (its LodCtrl in s.talk.lod; the distance in f1): the distance to the
// nearer brother, so an object shows its high and middle models around either of them and goes low only where
// neither is. It is the distance to the player for most objects (MR::calcDistanceToPlayer), the camera depth for
// the rest, which the other brother's distance stands in for (his camera follows him).
void lod_distance(const Guest& guest, const State& s, PpcContext* context) {
    if (!plausible(s.first) || !plausible(s.second) || !plausible(s.talk.lod)) return;
    const unsigned other = s.moving_second ? 0u : 1u;
    if (s.down[other].down) return;
    const std::uint32_t actor = guest.u32(s.talk.lod + 0xCu);  // LodCtrl::mActor
    if (!plausible(actor)) return;
    const float near_other = coop::length(guest.vec(actor + actor::kPosition) -
                                          guest.vec((other == 1u ? s.second : s.first) + actor::kPosition));
    const double game = std::bit_cast<double>(context->fpr_bits[1]);
    if (std::isfinite(near_other) && near_other < game) {
        context->fpr_bits[1] = std::bit_cast<std::uint64_t>(double(near_other));
    }
}

// A co-op talk's window stands in the world over its NPC, like the walk-by balloons, and smaller, rather than
// across the top of the screen (the walk-by balloon itself cannot page or choose). Offset from the NPC's screen
// point and scale: GALAXY_TALK_WINDOW="dx,dy,scale" overrides them while they are tuned.
struct TalkWindowTuning {
    float dx = 60.0f;   // the window's beak lands on the NPC's balloon point
    float dy = 60.0f;
    float scale = 0.7f;
};

TalkWindowTuning talk_window_tuning() {
    TalkWindowTuning t;
    char* text = nullptr;
    std::size_t length = 0u;
    if (_dupenv_s(&text, &length, "GALAXY_TALK_WINDOW") == 0 && text != nullptr) {
        float dx = 0.0f, dy = 0.0f, scale = 1.0f;
        if (sscanf_s(text, "%f,%f,%f", &dx, &dy, &scale) == 3 && std::isfinite(dx) && std::isfinite(dy) &&
            std::isfinite(scale) && scale > 0.1f && scale <= 2.0f) {
            t = {dx, dy, scale};
        }
        std::free(text);
    }
    return t;
}

// Moves a talk window's anchor (its NPC's balloon point, screen units) so the whole window stays on screen and,
// in split screen, inside its talker's half. At 0.7 of its size the window spans 208 pixels either side of the
// point and 5 to 90 above it on an 854 x 480 picture (measured), scaled to the game's screen units.
void keep_talk_window_in_view(const State& s, bool second, float scale, float& x, float& y) {
    const float k = scale / 0.7f;
    const float half_w = 208.0f * k * s.screen_width / 854.0f;
    const float top = 90.0f * k * s.screen_height / 480.0f;
    const float bottom = 5.0f * k * s.screen_height / 480.0f;
    constexpr float kMargin = 8.0f;
    const bool split = s.split.amount() > 0.5f && s.layout.started();
    const std::array<float, 3> side = s.layout.plane(second, s.screen_width, s.screen_height);
    for (int pass = 0; pass < 3; ++pass) {
        if (split) {
            // The corner deepest across the line pushes the window back along the line's normal.
            float least = std::numeric_limits<float>::max();
            for (const float cx : {x - half_w, x + half_w}) {
                for (const float cy : {y - top, y - bottom}) {
                    least = std::min(least, side[0] * cx + side[1] * cy + side[2]);
                }
            }
            if (least < kMargin) {
                x += side[0] * (kMargin - least);
                y += side[1] * (kMargin - least);
            }
        }
        x = std::clamp(x, half_w + kMargin, std::max(half_w + kMargin, s.screen_width - half_w - kMargin));
        y = std::clamp(y, top + kMargin, std::max(top + kMargin, s.screen_height - kMargin));
    }
}

// After TalkMessageCtrl::updateBalloonPos from TalkBalloon::updateBalloon (the balloon in r30; the NPC's screen
// point, at its message control's +0x1C, just computed): a co-op talk's TalkBalloonEvent moves there. Its beak
// is aimed from the new place right after.
void talk_window_in_world(const Guest& guest, State& s, PpcContext* context) {
    if (!s.talk.calm) return;
    const std::uint32_t balloon = context->gpr[30];
    if (!plausible(balloon) || guest.u32(balloon) != fn::kTalkBalloonEventVtable) return;
    const std::uint32_t ctrl = guest.u32(balloon + 0x20u);  // TalkBalloon::mMessageCtrl
    if (!plausible(ctrl)) return;
    static const TalkWindowTuning tuning = talk_window_tuning();
    coop::Vec3 npc = guest.vec(ctrl + 0x1Cu);  // TalkMessageCtrl::_1C: screen x, y, and z (> 0 behind)
    // In split screen the second player's talk shows over his half: the NPC's balloon point (above its head)
    // through the camera his half drew with, rather than the game's camera (the first player's).
    const bool second_talks = s.talk.locked ? s.talk.locked_second : s.talk.talker_second;
    if (second_talks && s.split.amount() > 0.5f) {
        const std::uint32_t host = guest.u32(ctrl + 0xCu);
        if (plausible(host)) {
            const coop::Vec3 up = coop::pose_of(s.second_view.inverse).up;
            const coop::Vec3 point = guest.vec(host + actor::kPosition) + up * 180.0f;
            const coop::Vec3 eye = coop::transform_point(s.second_view.view, point);
            const float ty = std::tan(s.fovy * 0.5f * 3.14159265f / 180.0f);
            if (eye.z < -1.0f && ty > 0.0f && s.aspect > 0.0f) {
                const float nx = (eye.x / -eye.z) / (ty * s.aspect);
                const float ny = (eye.y / -eye.z) / ty;
                npc.x = (nx + 1.0f) * 0.5f * s.screen_width;
                npc.y = (1.0f - ny) * 0.5f * s.screen_height;
            }
        }
    }
    if (!std::isfinite(npc.x) || !std::isfinite(npc.y)) return;
    // A half of the screen is narrower: the window is smaller there (0.55 for the default 0.7).
    const bool split = s.split.amount() > 0.5f && s.layout.started();
    const float scale = split ? tuning.scale * (0.55f / 0.7f) : tuning.scale;
    const float shrink = scale / tuning.scale;
    keep_talk_window_in_view(s, second_talks, scale, npc.x, npc.y);
    SavedContext saved(context);
    Guest::Frame frame(guest);
    // The window scales about its origin, so the beak's offset from it scales too.
    guest.call(fn::kLayoutSetTrans, balloon,
               frame.vec(0, {npc.x + tuning.dx * shrink, npc.y + tuning.dy * shrink, 0.0f}));
    if (scale != 1.0f) {
        // The root pane (a null name, as MR::setLayoutScaleAtPaneScale passes): the window's animations move its
        // panes below it.
        context->fpr_bits[1] = std::bit_cast<std::uint64_t>(double(scale));
        context->fpr_bits[2] = std::bit_cast<std::uint64_t>(double(scale));
        guest.call(fn::kSetPaneScale, balloon, 0u);
    }
}

// After MR::getPlayerPos in SwitchArea::movement (its `this` in r31), which turns its switch on with the player
// inside it: the second player inside, the first not, answers with the second's position. Areas that let star
// bunnies out of hiding, start events and so on then see either brother.
void switch_area_player_pos(const Guest& guest, const State& s, PpcContext* context) {
    const std::uint32_t area = context->gpr[31];
    if (!plausible(area) || !plausible(s.first) || !plausible(s.second) || s.down[1].down) return;
    bool first_in = false;
    bool second_in = false;
    {
        SavedContext saved(context);
        Guest::Frame frame(guest);
        first_in = (guest.call(fn::kAreaIsInVolume, area, frame.vec(0, guest.vec(s.first + actor::kPosition))) & 0xFFu) != 0u;
        second_in = !first_in &&
            (guest.call(fn::kAreaIsInVolume, area, frame.vec(1, guest.vec(s.second + actor::kPosition))) & 0xFFu) != 0u;
    }
    if (second_in) context->gpr[3] = s.second + actor::kPosition;
}

// After MR::getPlayerPos from a star bunny (TrickRabbit, RunawayRabbit and their kin, whose `this` is in
// r29-r31): the nearer brother's position, so it runs from either.
void bunny_player_pos(const Guest& guest, const State& s, PpcContext* context, std::uint32_t guest_pc) {
    const bool trick = guest_pc >= 0x802938E0u && guest_pc < 0x80299824u;
    const bool runaway = guest_pc >= 0x80284A58u && guest_pc < 0x80286A54u;
    if (!trick && !runaway) return;
    if (!plausible(s.first) || !plausible(s.second) || s.down[0].down || s.down[1].down) return;
    std::uint32_t bunny = 0u;
    for (unsigned r = 31u; r >= 29u && bunny == 0u; --r) {
        const std::uint32_t candidate = context->gpr[r];
        const std::uint32_t vtable = plausible(candidate) ? guest.u32(candidate) : 0u;
        if (vtable >= 0x8059E000u && vtable < 0x805A2000u) bunny = candidate;  // the rabbits' vtables
    }
    if (bunny == 0u) return;
    const coop::Vec3 at = guest.vec(bunny + actor::kPosition);
    const float first = coop::length(guest.vec(s.first + actor::kPosition) - at);
    const float second = coop::length(guest.vec(s.second + actor::kPosition) - at);
    context->gpr[3] = (second < first ? s.second : s.first) + actor::kPosition;
}

// ---- Star Pointers (docs/MULTIPLAYER.md "Star Pointers") ----
// Mario's pointer is channel 0's and the second player's (the orange one) channel 1's, however the WPads are swapped.

// The half a pointer is over (1 the second player's), from where its target tests look
// (StarPointerController::mPastInfo.mPos, in the game's 2D screen: MR::getScreenWidth by the frame buffer's height).
std::size_t half_under_pointer(const Guest& guest, const State& s, std::uint32_t channel) {
    SavedContext saved(guest.context());
    const std::uint32_t director = guest.call(fn::kGetStarPointerDirector);
    const std::uint32_t controllers = plausible(director) ? guest.u32(director + 4u) : 0u;
    if (!plausible(controllers)) return channel;
    const std::uint32_t controller = controllers + kStarPointerControllerSize * channel;
    const auto width = static_cast<float>(guest.call(fn::kGetScreenWidth));
    const auto height = static_cast<float>(guest.call(fn::kGetFrameBufferHeight) & 0xFFFFu);
    const float x = guest.f32(controller + kPointerPastPos) / width * s.screen_width;
    const float y = guest.f32(controller + kPointerPastPos + 4u) / height * s.screen_height;
    if (!std::isfinite(x) || !std::isfinite(y)) return channel;
    const std::array<float, 3> line = s.layout.plane(true, s.screen_width, s.screen_height);
    return line[0] * x + line[1] * y + line[2] >= 0.0f ? 1u : 0u;
}

// Before isStarPointerPointingCore (one pointer's gates and target test, `channel`): while split, the game's pointer
// camera (StarPointerTransformHolder, the game camera's copy) is the camera of the half the pointer is over, as last
// drawn, so a target counts where it is seen under the cursor, depth test included. After it, the game's again.
void pointer_half_begin(const Guest& guest, State& s, std::uint32_t channel) {
    auto& p = s.pointers;
    if (!p.split || channel > 1u || p.holder_swapped) return;
    const std::size_t half = half_under_pointer(guest, s, channel);
    SavedContext saved(guest.context());
    const std::uint32_t view = guest.call(fn::kStarPointerViewMtx);
    const std::uint32_t projection = guest.call(fn::kStarPointerProjMtx);
    if (!plausible(view) || !plausible(projection)) return;
    p.holder_view = read_floats<12>(guest, view);
    p.holder_projection = read_floats<16>(guest, projection);
    write_floats(guest, view, p.camera[half].view);
    write_floats(guest, projection, p.projection[half]);
    p.holder_swapped = true;
}

void pointer_half_end(const Guest& guest, State& s) {
    auto& p = s.pointers;
    if (!p.holder_swapped) return;
    p.holder_swapped = false;
    SavedContext saved(guest.context());
    write_floats(guest, guest.call(fn::kStarPointerViewMtx), p.holder_view);
    write_floats(guest, guest.call(fn::kStarPointerProjMtx), p.holder_projection);
}

// Around GCapture's MR::calcScreenPosition (which Pull Star is nearest its pointer, from CameraContext): the camera
// of the half the capturing brother's pointer is over, while split.
void capture_screen_begin(const Guest& guest, State& s) {
    auto& p = s.pointers;
    if (!p.split || p.context_swapped) return;
    const std::size_t half = half_under_pointer(guest, s, p.capture_second ? 1u : 0u);
    SavedContext saved(guest.context());
    const std::uint32_t context = camera_context(guest);
    if (context == 0u) return;
    p.context_camera = read_camera(guest, context);
    p.context_projection = read_floats<16>(guest, context + camera::kProjection);
    write_camera(guest, context, p.camera[half]);
    write_floats(guest, context + camera::kProjection, p.projection[half]);
    p.context_swapped = true;
}

void capture_screen_end(const Guest& guest, State& s) {
    auto& p = s.pointers;
    if (!p.context_swapped) return;
    p.context_swapped = false;
    SavedContext saved(guest.context());
    const std::uint32_t context = camera_context(guest);
    if (context == 0u) return;
    write_camera(guest, context, p.context_camera);
    write_floats(guest, context + camera::kProjection, p.context_projection);
}

// The Pull Stars change hands only while GCapture holds nobody (its Wait or CoolDown).
bool capture_idle(const Guest& guest, const State& s) {
    const std::uint32_t capture = s.pointers.gcapture;
    if (!plausible(capture) || guest.u32(capture) != kGCaptureVtable) return true;
    SavedContext saved(guest.context());
    return (guest.call(fn::kIsNerve, capture, kGCaptureNrvWait) & 0xFFu) != 0u ||
           (guest.call(fn::kIsNerve, capture, kGCaptureNrvCoolDown) & 0xFFu) != 0u;
}

// A call into GCapture's or GCaptureTarget's code from their own (nerve steps, control, sensor answers): it runs
// with the capturing brother as the game's player (his position, his bind, his A as channel 0's), as star bunnies
// run with the nearer one. Calls from elsewhere (the player's and the camera's checks) are left as they are.
void capture_code_begin(const Guest& guest, State& s, std::uint32_t guest_pc) {
    auto& p = s.pointers;
    PpcContext* context = guest.context();
    const std::uint32_t self = context->gpr[3];
    if (plausible(self) && guest.u32(self) == kGCaptureVtable) p.gcapture = self;
    ++p.capture_depth;
    // GCapture::receiveOtherMsg's requestBind(sender): the Pull Star binds whoever's body touches it first. The
    // brother not capturing (standing next to the other) is refused; requestBind's false tells him so.
    if (in_capture_receive(guest_pc) && plausible(s.first) && plausible(s.second)) {
        const std::uint32_t sensor = context->gpr[4];
        const std::uint32_t host = plausible(sensor) ? guest.u32(sensor + kSensorHost) : 0u;
        const std::uint32_t capturer = p.capture_second ? s.second : s.first;
        if ((host == s.first || host == s.second) && host != capturer) {
            p.bind_refused = true;
            context->pc = kCoopSkipCall;
            return;
        }
    }
    if (p.capture_depth != 1u) return;
    if (!in_capture_code(guest_pc) || p.capture_second == s.moving_second || !plausible(s.first) ||
        !plausible(s.second) || s.down[p.capture_second ? 1u : 0u].down) {
        return;
    }
    SavedContext saved(guest.context());
    p.capture_back_to_second = s.moving_second;
    set_game_player(guest, s, p.capture_second);
    p.capture_swapped = true;
}

// Spine::update of GCapture or a GCaptureTarget: its nerve step (exeCapture and the rest, which their nerves reach
// by a tail branch, unseen by capture_code_begin) runs as the capturing brother too.
void capture_spine_begin(const Guest& guest, State& s) {
    auto& p = s.pointers;
    const std::uint32_t spine = guest.context()->gpr[3];
    const std::uint32_t actor = plausible(spine) ? guest.u32(spine) : 0u;  // Spine::mExecutor
    const std::uint32_t vtable = plausible(actor) ? guest.u32(actor) : 0u;
    if ((vtable != kGCaptureVtable && vtable != kGCaptureTargetVtable) || p.spine_swapped ||
        p.capture_second == s.moving_second || !plausible(s.first) || !plausible(s.second) ||
        s.down[p.capture_second ? 1u : 0u].down) {
        return;
    }
    SavedContext saved(guest.context());
    p.spine_back_to_second = s.moving_second;
    set_game_player(guest, s, p.capture_second);
    p.spine_swapped = true;
}

void capture_spine_end(const Guest& guest, State& s) {
    auto& p = s.pointers;
    if (!p.spine_swapped) return;
    p.spine_swapped = false;
    SavedContext saved(guest.context());
    set_game_player(guest, s, p.spine_back_to_second);
}

void capture_code_end(const Guest& guest, State& s) {
    auto& p = s.pointers;
    if (p.bind_refused) {
        p.bind_refused = false;
        guest.context()->gpr[3] = 0u;
    }
    if (p.capture_depth == 0u || --p.capture_depth != 0u || !p.capture_swapped) return;
    SavedContext saved(guest.context());
    set_game_player(guest, s, p.capture_back_to_second);
    p.capture_swapped = false;
}

// After a Pull Star's own pointing test (GCaptureTarget, on the capturing brother's pointer): if it missed and the
// other brother's pointer is on it while GCapture is free, the Pull Stars are his.
void capture_pointing_after(const Guest& guest, State& s, PpcContext* context) {
    auto& p = s.pointers;
    if ((context->gpr[3] & 0xFFu) != 0u || !plausible(s.first) || !plausible(s.second)) return;
    const bool other_second = !p.capture_second;
    if (s.down[other_second ? 1u : 0u].down || !capture_idle(guest, s)) return;
    std::uint32_t pointed = 0u;
    {
        SavedContext saved(context);
        const auto& a = p.pointing_args;
        pointed = guest.call(fn::kIsStarPointerPointing, a[0], other_second ? 1u : 0u, a[2], a[3]) & 0xFFu;
    }
    if (pointed == 0u) return;
    p.capture_second = other_second;
    trace("pull stars: the %s player's pointer", other_second ? "second" : "first");
    context->gpr[3] = 1u;
}

void pointers_before(std::uint32_t id, const Guest& guest, State& s, PpcContext* context, std::uint32_t guest_pc) {
    auto& p = s.pointers;
    // Channel 0 in the Pull Stars' code is the capturing brother's pointer.
    const bool remap = in_capture_code(guest_pc) && p.capture_second;
    switch (id) {
    case call::kPointingCore: pointer_half_begin(guest, s, context->gpr[5]); return;
    case call::kScreenPosition:
        if (in_capture_code(guest_pc)) capture_screen_begin(guest, s);
        return;
    case call::kCaptureCode: capture_code_begin(guest, s, guest_pc); return;
    case call::kPointing:
        for (unsigned i = 0; i < 4u; ++i) p.pointing_args[i] = context->gpr[3 + i];
        p.pointing_target = in_capture_target(guest_pc);
        if (remap && context->gpr[4] == 0u) context->gpr[4] = 1u;
        return;
    case call::kPointerScreenPos:
        if (remap && context->gpr[3] == 0u) context->gpr[3] = 1u;
        return;
    case call::kStrokeMoment:
        if (remap && context->gpr[5] == 0u) context->gpr[5] = 1u;
        return;
    case call::kSpineUpdate: capture_spine_begin(guest, s); return;
    default: return;
    }
}

void pointers_after(std::uint32_t id, const Guest& guest, State& s, PpcContext* context) {
    auto& p = s.pointers;
    switch (id) {
    case call::kPointingCore: pointer_half_end(guest, s); return;
    case call::kScreenPosition: capture_screen_end(guest, s); return;
    case call::kTractTrigger:
        // A on a Pull Star is the capturing brother's: the other's jump is not taken for it.
        if (p.capture_second != s.moving_second) context->gpr[3] = 0u;
        return;
    case call::kCaptureCode: capture_code_end(guest, s); return;
    case call::kPointing:
        if (p.pointing_target) {
            p.pointing_target = false;
            capture_pointing_after(guest, s, context);
        }
        return;
    case call::kCoStar:
        // The second pointer's A is the second player's jump, not Co-Star help: no jump assist for the other
        // brother, no enemy held, no launch star fired.
        context->gpr[3] = 0u;
        return;
    case call::kSpineUpdate: capture_spine_end(guest, s); return;
    default: return;
    }
}

// ---- Co-op lives, respawn and HUD (docs/MULTIPLAYER.md "Lives") ----

// The second player's lives are kept only while the game runs, as the game keeps the first player's: loading a
// save starts the first player at 4 (GameDataPlayerStatus::deserialize), and the second with as many.
void ensure_second_lives(const Guest& guest, State& s) {
    if (s.second_lives >= 0) return;
    s.second_lives = std::clamp(static_cast<int>(guest.call(fn::kGetPlayerLeft)), 0, 99);
    trace("second player's lives: %d (as the first player's)", s.second_lives);
}

// A save just loaded (the first stage after the file select): the second player's lives start over with the
// first player's.
void on_stage_start(const Guest& guest, State& s) {
    const std::uint32_t system = guest.u32(kGameSystem);
    const std::uint32_t controller = plausible(system) ? guest.u32(system + 0x24u) : 0u;
    const std::string stage = plausible(controller) ? guest.string(controller + 0x20u, 31u) : std::string{};
    if (s.last_stage == "FileSelect" && stage != "FileSelect") s.second_lives = -1;
    s.last_stage = stage;
}

// A MarioActor::exeGameOver* step's first frame makes these calls (the BGM lock and queue, the player event, the
// death camera, the pointer demo). For a player going down while the other plays on, none of them happen: the
// first decides, and the player's down state skips the rest that frame.
void on_game_over_call(std::uint32_t id, const Guest& guest, State& s, PpcContext* context, std::uint32_t guest_pc) {
    if (!in_game_over_step(guest_pc) || !plausible(s.first) || !plausible(s.second)) return;
    // The MarioActor going down: exeGameOver* keeps itself in r30 or r31.
    const std::uint32_t r30 = context->gpr[30];
    const std::uint32_t r31 = context->gpr[31];
    const unsigned who = (r30 == s.second || r31 == s.second) ? 1u
                         : (r30 == s.first || r31 == s.first) ? 0u
                                                               : (s.moving_second ? 1u : 0u);
    if (id == call::kSetCubeBgmChangeInvalid || id == call::kStartDeathCamera) {
        trace("down: call %u from %08X, r30 %08X r31 %08X, second's turn %d: %s", id, guest_pc, r30, r31,
              s.moving_second ? 1 : 0, who == 0u ? "first" : "second");
    }
    coop::DownState& self = s.down[who];
    const coop::DownState& partner = s.down[1u - who];
    if (!(self.down && self.frames == 0u) && !s.game_miss) {
        // A race lost is the game's to handle (MarioActor::_39D: 1, 2 and 5 are race outcomes).
        const std::uint32_t actor_address = who == 0u ? s.first : s.second;
        const std::uint8_t kind = guest.u8(actor_address + 0x39Du);
        if (kind == 1u || kind == 2u || kind == 5u) return;
        SavedContext saved(context);
        ensure_second_lives(guest, s);
        if (who == 0u) {
            int lives = static_cast<int>(guest.call(fn::kGetPlayerLeft));
            const int before = lives;
            if (coop::go_down_alone(self, partner, lives) && lives < before) guest.call(fn::kDecPlayerLeft);
        } else {
            (void)coop::go_down_alone(self, partner, s.second_lives);
        }
        if (self.down) {
            // Lives counters count up by themselves but never down (a miss always reloaded the stage): show it.
            const std::uint32_t counter = who == 0u ? s.first_left : s.second_left;
            if (plausible(counter)) {
                const int lives = who == 0u ? static_cast<int>(guest.call(fn::kGetPlayerLeft)) : s.second_lives;
                guest.set_u32(counter + kPlayerLeftCount, static_cast<std::uint32_t>(std::max(lives, 0)));
                guest.call(fn::kPlayerLeftCalls[1], counter);  // PlayerLeft::forceAppear
            }
            // The down animation the game's player event would have started (MarioActor::exeGameOver; a fall
            // into the abyss starts its own).
            if (guest_pc >= 0x802B8A38u && guest_pc < 0x802B8B64u) guest.call(fn::kChangeGameOverAnimation, actor_address);
            trace("%s down alone: %s", who == 0u ? "first player" : "second player",
                  self.out ? "no lives left, out until the stage reloads" : "back beside the other soon");
        } else {
            s.game_miss = true;
            trace("both players down: the game's miss");
        }
    }
    if (self.down && self.frames == 0u) {
        context->pc = kCoopSkipCall;
        return;
    }
    if (id != call::kStartPlayerEvent || !s.game_miss || s.miss_counted) return;
    // The game's own miss with both down: each player's life is spent once. The first player's comes off with the
    // miss (the event's decLeft), so one goes back if they spent theirs going down alone; the second player's
    // here. Game over only when both are out.
    s.miss_counted = true;
    std::uint32_t name = context->gpr[3];
    {
        SavedContext saved(context);
        const int mario = static_cast<int>(guest.call(fn::kGetPlayerLeft));
        const bool mario_out = who == 0u ? mario == 0 : s.down[0].out;
        bool luigi_out = s.down[1].out;
        if (who == 1u) {
            luigi_out = s.second_lives == 0;
            if (s.second_lives > 0) --s.second_lives;
        }
        if (who == 1u && s.down[0].down && !s.down[0].out) guest.call(fn::kIncPlayerLeft);  // spent alone already
        if (!(mario_out && luigi_out) && static_cast<int>(guest.call(fn::kGetPlayerLeft)) == 0) {
            // The game chose its game over from the first player's lives alone: a miss instead.
            guest.call(fn::kIncPlayerLeft);
            if (!s.mario_down_searched) {
                s.mario_down_searched = true;
                s.mario_down_name = guest.find_string("\x83\x7D\x83\x8A\x83\x49\x83\x5F\x83\x45\x83\x93");  // マリオダウン
            }
            if (s.mario_down_name != 0u) name = s.mario_down_name;
        }
        trace("the game's miss: lives %d and %d", static_cast<int>(guest.call(fn::kGetPlayerLeft)), s.second_lives);
    }
    context->gpr[3] = name;
}

// Puts a player who was down back in play at `there`: ordinary nerve, full health, visible, touchable. Called in
// that player's turn (the game's player is them), so MR::setPlayerPosAndWait places them.
void respawn_player(const Guest& guest, std::uint32_t actor_address, coop::Vec3 there) {
    guest.call(fn::kSetNerve, actor_address, kMarioActorNrvWait);
    guest.set_u32(actor_address + 0x380u, 3u);  // MarioActor::mHealth
    guest.set_u32(actor_address + 0x3E0u, 3u);  // MarioActor::mMaxHealth
    guest.set_u8(actor_address + 0x39Du, 0u);   // the down's kind
    // MarioActor::_B90, set by changeGameOverAnimation: while set the player stays in the down pose (the game
    // never needed to clear it: a miss reloads the stage).
    guest.set_u8(actor_address + 0xB90u, 0u);
    guest.call(fn::kShowModel, actor_address);
    guest.set_u8(actor_address + kMarioActorHidden, 0u);
    guest.call(fn::kValidateHitSensors, actor_address);
    Guest::Frame frame(guest);
    guest.call(fn::kSetPlayerPosAndWait, frame.vec(0, there));
    // Out of the down animation into the ordinary stand ("待機").
    static std::uint32_t idle = 0u;
    if (idle == 0u) idle = guest.find_string("\x91\xD2\x8B\x40");
    if (idle != 0u) guest.call(fn::kChangeAnimationNonStop, actor_address, idle);  // as the down was started
}

// Once a frame, in a player's own turn: the fallen player vanishes after their fall and comes back beside the
// other in time; one out of lives is carried along hidden beside the other (the game camera follows the first).
void update_down_player(const Guest& guest, State& s, unsigned who, coop::Vec3 partner_position,
                        coop::Vec3 partner_side) {
    coop::DownState& self = s.down[who];
    if (!self.down) return;
    const std::uint32_t actor_address = who == 0u ? s.first : s.second;
    if (self.frames + 1u == coop::kDownHideFrames) {
        guest.call(fn::kHideModel, actor_address);
        guest.set_u8(actor_address + kMarioActorHidden, 1u);
        guest.call(fn::kInvalidateHitSensors, actor_address);
    }
    const coop::Vec3 beside = partner_position + partner_side * 120.0f;
    if (s.game_miss) return;  // the game's own miss reloads the stage: nobody comes back meanwhile
    if (coop::respawn_due(self, s.down[1u - who])) {
        respawn_player(guest, actor_address, beside);
        // Their life meter broke and hid itself at zero (MeterLayout::exeZeroMeterBreak): back with the player.
        const std::uint32_t meter = who == 0u ? s.first_meter : s.second_meter;
        if (plausible(meter)) guest.call(fn::kShowLayout, meter);
        if (who == 1u) tint_second_meter(guest, s);
        trace("%s back beside the other", who == 0u ? "first player" : "second player");
        return;
    }
    if (self.out && self.frames > coop::kDownHideFrames) {
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, beside));
    }
}

// The second player's life meter in their colour, set before each draw (its animations set the colours): the
// HitPointMeter pictures tinted, the ring and the health pieces on land and in water. Their names are in guest
// memory (State::ring_names) for MR::getPane.
constexpr const char* kTintPanes[] = {"PicBase",    "PicBaseW",   "PicPoint1",  "PicPoint2",
                                      "PicPoint3",  "PicPoint1W", "PicPoint2W", "PicPoint3W"};
constexpr std::uint32_t kTintPaneCount = 8u;
constexpr std::uint32_t kTintNameSize = 12u;
void tint_second_meter(const Guest& guest, const State& s) {
    if (!plausible(s.second_meter) || !plausible(s.ring_names)) return;
    for (std::uint32_t i = 0; i < kTintPaneCount; ++i) {
        const std::uint32_t pane = guest.call(fn::kGetPane, s.second_meter, s.ring_names + i * kTintNameSize);
        if (!plausible(pane) || guest.u32(pane) != fn::kPictureVtable) continue;
        for (std::uint32_t corner = 0; corner < 4u; ++corner) {
            for (std::uint32_t channel = 0; channel < 3u; ++channel) {
                guest.call(fn::kPictureSetVtxColorElement, pane, corner * 4u + channel, coop::kSecondMeterTint[channel]);
            }
        }
    }
}

// The second player's life meter and lives counter, made once a stage beside the game's own: the same layouts
// (HitPointMeter, and PlayerLeft in its Luigi look), fed with the second player's health and lives.
void create_hud(const Guest& guest, State& s) {
    if (s.second_meter != 0u || s.hud_tried) return;
    s.hud_tried = true;  // one try a stage
    const std::uint32_t holder = guest.call(fn::kGetLayoutHolder);
    if (!plausible(holder)) return trace("second player's HUD not made: no layout holder");
    const std::uint32_t mario_meter = guest.u32(holder + kHolderMarioMeter);
    const std::uint32_t counters = guest.u32(holder + kHolderCounters);
    if (!plausible(mario_meter) || !plausible(counters)) return trace("second player's HUD not made: no meter or counters");
    s.first_meter = guest.u32(mario_meter + kMarioMeterHitPoint);
    s.first_left = guest.u32(counters + kCountersPlayerLeft);
    if (!plausible(s.first_meter) || !plausible(s.first_left)) return trace("second player's HUD not made: no game meter or lives counter");
    if (s.hit_point_name == 0u) s.hit_point_name = guest.find_string("HitPointMeter");
    const std::uint32_t watcher = guest.u32(kHeapMemoryWatcher);
    const std::uint32_t heap = plausible(watcher) ? guest.u32(watcher + kSceneHeapNapa) : 0u;
    if (s.hit_point_name == 0u || !plausible(heap)) return trace("second player's HUD not made: name %08X heap %08X", s.hit_point_name, heap);
    ensure_second_lives(guest, s);
    const std::uint32_t meter = guest.call(fn::kNewOnHeap, 0x50u, heap, 4u);
    if (!plausible(meter)) return trace("second player's HUD not made: no memory");
    for (std::uint32_t at = 0u; at < 0x50u; at += 4u) guest.set_u32(meter + at, 0u);
    // Screen to layout positions, per axis (layout = scale * screen + offset), from two points: setTrans takes a
    // screen position, and the game's HUD layouts stand at the layout origin.
    {
        Guest::Frame frame(guest);
        guest.call(fn::kScreenToLayout, frame.scratch(), frame.vec(0, coop::Vec3{0.0f, 0.0f, 0.0f}));
        const float x0 = guest.f32(frame.scratch());
        const float y0 = guest.f32(frame.scratch() + 4u);
        guest.call(fn::kScreenToLayout, frame.scratch(), frame.vec(1, coop::Vec3{100.0f, 100.0f, 0.0f}));
        s.hud_scale_x = (guest.f32(frame.scratch()) - x0) / 100.0f;
        s.hud_scale_y = (guest.f32(frame.scratch() + 4u) - y0) / 100.0f;
        s.hud_offset_x = x0;
        s.hud_offset_y = y0;
        trace("HUD layout space: x = %.3f * screen %+.1f, y = %.3f * screen %+.1f", s.hud_scale_x, x0, s.hud_scale_y, y0);
    }
    guest.call(fn::kMeterLayoutCtor, meter, s.hit_point_name, s.hit_point_name);
    guest.call(fn::kInitWithoutIter, meter);
    guest.call(fn::kMeterInitCount, meter, 3u);
    // MarioMeter activated the first player's meter as the stage began, before this one was made.
    guest.call(fn::kMeterRequests[0], meter);  // MeterLayout::requestActive
    // The second player's meter in their colour (tint_second_meter).
    s.ring_names = guest.call(fn::kNewOnHeap, kTintPaneCount * kTintNameSize, heap, 4u);
    if (plausible(s.ring_names)) {
        for (std::uint32_t i = 0; i < kTintPaneCount; ++i) {
            const std::string_view name = kTintPanes[i];
            for (std::uint32_t c = 0; c < kTintNameSize; ++c) {
                guest.set_u8(s.ring_names + i * kTintNameSize + c, c < name.size() ? static_cast<std::uint8_t>(name[c]) : 0u);
            }
        }
    }
    s.second_meter = meter;
    s.second_meter_count = 3u;
    const std::uint32_t left = guest.call(fn::kNewOnHeap, 0x30u, heap, 4u);
    if (plausible(left)) {
        for (std::uint32_t at = 0u; at < 0x30u; at += 4u) guest.set_u32(left + at, 0u);
        guest.call(fn::kPlayerLeftCtor, left, s.hit_point_name);
        // In the second player's character, so the counter shows their face; its MR::getPlayerLeft calls get the
        // second player's lives (call::kGetPlayerLeft).
        s.second_left = left;
        guest.set_u8(kIsLuigi, character_byte(coop::partner_of(s.own)));
        s.answer_partner = true;  // MR::isPlayerLuigi asks GameDataFunction::isDataMario
        guest.call(fn::kInitWithoutIter, left);
        s.answer_partner = false;
        guest.set_u8(kIsLuigi, character_byte(s.own));
    }
    tint_second_meter(guest, s);
    trace("second player's HUD: life meter %08X, lives counter %08X", s.second_meter, s.second_left);
}

// Once a frame: the second player's life meter shows their health, and their layouts are live while the
// game's are (the game kills its HUD for a miss or a cutscene with a virtual call no hook sees).
void update_hud_count(const Guest& guest, State& s) {
    if (s.second_meter == 0u || !plausible(s.second)) return;
    const auto mirror_alive = [&](std::uint32_t mine, std::uint32_t theirs, std::uint32_t revive) {
        if (!plausible(mine) || !plausible(theirs)) return;
        const bool their_dead = (guest.call(fn::kIsLayoutDead, theirs) & 0xFFu) != 0u;
        const bool my_dead = (guest.call(fn::kIsLayoutDead, mine) & 0xFFu) != 0u;
        if (their_dead && !my_dead) guest.call(fn::kLayoutKill, mine);
        if (!their_dead && my_dead) guest.call(revive, mine);
    };
    mirror_alive(s.second_meter, s.first_meter, fn::kMeterRequests[0]);    // MeterLayout::requestActive
    mirror_alive(s.second_left, s.first_left, fn::kPlayerLeftCalls[0]);    // PlayerLeft::appear

    const std::uint32_t health = s.down[1].down ? 0u : guest.u32(s.second + 0x380u);
    if (health == s.second_meter_count || health > 8u) return;
    s.second_meter_count = health;
    guest.call(fn::kMeterSetCount, s.second_meter, health);
}

// Before the 2D draw: the second player's layouts stand beside the first's, wherever the game has moved those.
void place_hud(const Guest& guest, const State& s) {
    const auto follow = [&](std::uint32_t mine, std::uint32_t theirs, float dx, float dy) {
        if (!plausible(mine) || !plausible(theirs)) return;
        // The layout position (dx, dy) as the screen position setTrans takes.
        if (std::fabs(s.hud_scale_x) < 1e-4f || std::fabs(s.hud_scale_y) < 1e-4f) return;
        const coop::Vec3 at{(dx - s.hud_offset_x) / s.hud_scale_x, (dy - s.hud_offset_y) / s.hud_scale_y, 0.0f};
        if (!std::isfinite(at.x) || !std::isfinite(at.y)) return;
        Guest::Frame frame(guest);
        guest.call(fn::kLayoutSetTrans, mine, frame.vec(0, at));
    };
    follow(s.second_meter, s.first_meter, coop::kSecondMeterOffset[0], coop::kSecondMeterOffset[1]);
    if (plausible(s.second_meter)) {
        follow(s.first_meter, s.first_meter, coop::kFirstMeterOffset[0], coop::kFirstMeterOffset[1]);
    }
    // The meter's own animations (damage, recovery, breaking) set its colours as it moves: its tint after them.
    tint_second_meter(guest, s);
    follow(s.second_left, s.first_left, coop::kSecondLivesOffset[0], coop::kSecondLivesOffset[1]);
    if (plausible(s.second_left)) {
        follow(s.first_left, s.first_left, coop::kFirstLivesOffset[0], coop::kFirstLivesOffset[1]);
    }
}

void move_second_player(const Guest& guest, State& s) {
    SavedContext saved(guest.context());
    const std::uint32_t holder = guest.call(fn::kGetMarioHolder);
    const std::uint32_t player = plausible(holder) ? guest.call(fn::kGetMarioActor, holder) : 0u;
    if (player != s.first) {
        // A different scene's player: this second player went with its scene.
        trace("player changed (%08X): second player dropped", player);
        s.first = s.second = s.second_mario = 0u;
        s.split.reset();
        s.follow.reset();
        return;
    }
    // The second player moves only here, in their turn: suspended in the game's movement list (NameObj::mFlags
    // bit 0). A galaxy's start resumes the scene's suspended objects, him with them, and the game then moved him a
    // second time each frame with the first player's controller (Luigi crept, tied to the keyboard, until the
    // stage reloaded).
    if (const std::uint8_t flags = guest.u8(s.second + kNameObjFlagsLow); (flags & kNameObjSuspended) == 0u) {
        guest.set_u8(s.second + kNameObjFlagsLow, static_cast<std::uint8_t>((flags & ~kNameObjRequests) | kNameObjSuspended));
        trace("second player was resumed in the game's movement list: suspended again");
    }
    const std::uint32_t pad_holder = guest.call(fn::kGetWPadHolder);
    const bool pads = plausible(pad_holder) && plausible(guest.u32(pad_holder)) && plausible(guest.u32(pad_holder + 4u));

    // While the game has the first player's controls (a dome's Pull Star, the flight into a galaxy) or a
    // cutscene plays, the second player goes with them, beside them: those sequences change the map
    // (collision, gravity) around the first player and dropped the second through the floor.
    const std::uint32_t first_mario = guest.u32(s.first + actor::kMario);
    // (A first player down has no control either, but the second plays on alone then.)
    const bool first_held = !s.down[0].down && plausible(first_mario) &&
                            (guest.call(fn::kIsInputDisable, first_mario) & 0xFFu) != 0u;
    // A dome's galaxy selector puppets the first player in a time-keep demo (not MR::isDemoActive).
    const bool demo = (guest.call(fn::kIsDemoActive) & 0xFFu) != 0u ||
                      (guest.call(fn::kIsTimeKeepDemoActive) & 0xFFu) != 0u;
    if (first_held != s.second_waiting_input || demo != s.second_waiting_demo) {
        trace("second player %s (first player's controls held %d, cutscene %d)",
              first_held || demo ? "goes beside the first" : "is free again", first_held ? 1 : 0, demo ? 1 : 0);
        s.second_waiting_input = first_held;
        s.second_waiting_demo = demo;
    }
    const coop::Vec3 first_now = guest.vec(s.first + actor::kPosition);
    // A puppeted first player's actor sits at the origin (a dome's galaxy selector).
    const bool first_parked = !(coop::length(first_now) >= 1.0f);
    // Once a dome's galaxy selector has puppeted the first player, the rest of the scene is the flight into the
    // chosen galaxy (the dome floor no longer holds the second player): travel with them until the next scene.
    if (first_parked && !s.level_start) s.sticky_travel = true;
    // A brother waiting inside a pipe, or a pipe taking the other through after him (pipe_holds): the other
    // stays free to walk to it and go through himself, rather than wait and land beside the first.
    if (s.pipe.follow_pipe != 0u && ++s.pipe.follow_frames > coop::kPipeFollowFrames) {
        trace("pipe %08X: the other did not go in; given up", s.pipe.follow_pipe);
        s.pipe.follow_pipe = s.pipe.follower = 0u;
    }
    if (s.second_scene != 0u) --s.second_scene;
    if (s.talk.candidate_frames != 0u) --s.talk.candidate_frames;
    const bool piping = s.pipe.waiting != 0u || s.pipe.follow_pipe != 0u || s.second_scene != 0u;
    const bool first_busy =
        !piping && (coop::second_player_waits(first_held, demo) || first_parked || s.sticky_travel);
    // A level start lasts through the mission's opening camera and any event camera of the first player's
    // arrival, not just until they look controllable.
    const bool arriving = s.level_start && ((guest.call(fn::kIsOpeningCamera) & 0xFFu) != 0u ||
                                            (guest.call(fn::kIsEventCameraActive) & 0xFFu) != 0u);
    s.controllable_frames = first_busy || arriving ? 0u : s.controllable_frames + 1u;
    bool land_beside = false;
    if (s.level_start && s.controllable_frames >= coop::kLevelStartSettleFrames) {
        // The first player has landed and has control: the second joins them.
        s.level_start = false;
        land_beside = true;
        trace("level start over: the second player joins the first");
    }
    s.travelling = !piping && coop::keep_travelling(s.travelling, s.level_start || first_busy, s.controllable_frames);
    const bool beside_first = s.travelling;

    const coop::Character partner = coop::partner_of(s.own);
    s.moving_second = true;
    guest.set_u8(kIsLuigi, character_byte(partner));
    guest.call(fn::kSetMarioActor, holder, s.second);
    if (pads) swap_pads(guest, pad_holder);
    // While split, the second player's stick is relative to their own camera: the player code reads the
    // camera's directions from CameraContext.
    const std::uint32_t camera_ctx =
        s.split.amount() > 0.0f && s.follow.started() && !s.event_second ? camera_context(guest) : 0u;
    const coop::CameraMatrices first_camera = camera_ctx != 0u ? read_camera(guest, camera_ctx) : coop::CameraMatrices{};
    if (camera_ctx != 0u) write_camera(guest, camera_ctx, s.camera2);
    guest.call(fn::kMarioActorMovement, s.second);
    if (camera_ctx != 0u) write_camera(guest, camera_ctx, first_camera);
    if (s.agent_mode >= 0) {
        guest.call(fn::kSetPlayerMode, s.second, static_cast<std::uint32_t>(s.agent_mode), 1u);
        trace("agent: second player's power-up %d", s.agent_mode);
        s.agent_mode = -1;
    }
    if (s.agent_teleport) {
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, s.agent_position));
        s.agent_teleport = false;
    }
    // The second player down: they vanish, come back beside the first, or (out of lives) go along hidden.
    if (plausible(first_mario)) update_down_player(guest, s, 1u, first_now, guest.vec(first_mario + kMarioSideVec));
    // While the first player flies in at a level start, the second waits unseen and untouchable, out of the
    // camera's picture; they appear beside the first as they land.
    if (s.level_start != s.second_hidden_for_start && !s.down[1].down) {
        s.second_hidden_for_start = s.level_start;
        guest.call(s.level_start ? fn::kHideModel : fn::kShowModel, s.second);
        guest.set_u8(s.second + kMarioActorHidden, s.level_start ? 1u : 0u);
        guest.call(s.level_start ? fn::kInvalidateHitSensors : fn::kValidateHitSensors, s.second);
    }
    // Out of the picture: down, or waiting for the first player at a level start (still parked meanwhile).
    const bool second_down = s.down[1].down || s.level_start;
    if ((beside_first || land_beside) && plausible(first_mario) && !s.down[1].down) {
        // Still the second player's turn: MR::setPlayerPos moves the scene's player. A puppeted first player's
        // actor sits at the origin, and at a level start the first is still flying in: the second player then
        // stays where they were.
        const coop::Vec3 there = s.level_start ? s.parked_position
                                               : coop::travel_position(first_now, guest.vec(first_mario + kMarioSideVec),
                                                                       s.parked_position);
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, there));
    }

    const coop::Vec3 first_position = guest.vec(s.first + actor::kPosition);
    const coop::Vec3 second_actual = guest.vec(s.second + actor::kPosition);
    // A player down is not in the picture: the camera and the split follow the one still playing.
    const coop::Vec3 second_position = second_down ? first_position : second_actual;
    if (!beside_first) s.parked_position = second_actual;
    coop::CameraShare share{};
    bool too_far = false;
    if (split_screen_enabled()) {
        // The camera looks at the midpoint until the split and eases to the first player as it splits.
        const coop::SplitTuning tuning{};
        const float distance = coop::length(second_position - first_position);
        // While the second player's camera event plays the game camera is theirs, so it cannot tell whether
        // both fit one picture: the screen stays as it is until the event ends.
        if (s.level_start) {
            // The level's intro and the first player's flight in: one screen, the game's camera.
            s.split.reset();
        } else if (!s.event_second) {
            s.split.update(shared_extent(guest, s, first_position, second_position), distance, tuning);
        }
        if (s.split.amount() == 0.0f) {
            s.follow.reset();
            s.layout.reset();
        }
        if (std::isfinite(distance)) share.offset = (second_position - first_position) * (0.5f * (1.0f - s.split.eased()));
        too_far = !std::isfinite(distance) || distance >= tuning.split_warp;
    } else {
        share = coop::shared_camera(first_position, second_position);
        too_far = share.out_of_range;
    }
    // The second player at a door or dome the first player is not at: swap places (MR::setPlayerPos moves the
    // scene's player, the second now and the first below).
    bool swap_places = false;
    if (beside_first || second_down || s.down[0].down) {
        // Travelling with the first player: no doors, and no bringing back (the first may be parked); nor for
        // a player down.
        s.leash.reset();
    } else if (s.entrance_wait > 0u) {
        --s.entrance_wait;
    } else {
        if (!s.entrance_searched) {
            s.entrance_searched = true;
            s.entrance_name = guest.find_string("AstroChangeStageCube");
            trace("entrance area name at %08X", s.entrance_name);
        }
        if (s.entrance_name != 0u) {
            Guest::Frame frame(guest);
            const bool second_in = (guest.call(fn::kIsInAreaObj, s.entrance_name, frame.vec(0, second_position)) & 0xFFu) != 0u;
            const bool first_in = second_in &&
                (guest.call(fn::kIsInAreaObj, s.entrance_name, frame.vec(1, first_position)) & 0xFFu) != 0u;
            swap_places = coop::swap_for_entrance(second_in, first_in);
        }
    }
    if (swap_places) {
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, first_position));
        s.entrance_wait = 120u;
        trace("second player at an entrance: the players swap places");
    } else if (!piping && !beside_first && s.leash.update(too_far, coop::CameraTuning{}.warp_frames)) {
        // Still the second player's turn: MR::setPlayerPos moves the scene's player.
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, first_position));
        trace("second player brought back to the first");
    }

    if (pads) swap_pads(guest, pad_holder);
    guest.call(fn::kSetMarioActor, holder, s.first);
    guest.set_u8(kIsLuigi, character_byte(s.own));
    s.moving_second = false;
    if (swap_places) {
        Guest::Frame frame(guest);
        guest.call(fn::kSetPlayerPos, frame.vec(0, second_position));
    }
    // The second player on a launch star: the first flies with them, beside them, as the second does with the
    // first (MR::setPlayerPos moves the first player now).
    const bool second_rides = !second_down && !s.down[0].down && rides_star(guest, s.second);
    if (second_rides != s.second_riding) {
        s.second_riding = second_rides;
        trace(second_rides ? "second player on a launch star: the first flies beside them" : "launch star flight over");
    }
    if (second_rides && plausible(s.second_mario)) {
        Guest::Frame frame(guest);
        const coop::Vec3 there =
            coop::travel_position(second_actual, guest.vec(s.second_mario + kMarioSideVec), first_position);
        guest.call(fn::kSetPlayerPos, frame.vec(0, there));
    }
    // The first player down, in their turn (the game's player is theirs again).
    if (plausible(s.second_mario)) {
        update_down_player(guest, s, 0u, second_actual, guest.vec(s.second_mario + kMarioSideVec));
    }
    create_hud(guest, s);
    update_hud_count(guest, s);

    keep_camera_on_first_player(guest, s);
    if (!s.down[0].down && !second_down) bump_players(guest, s, first_position, second_position);
    if (s.split.amount() > 0.0f) update_second_camera(guest, s, first_position, second_position);
    remember_first_camera(guest, s);

    // The game camera looks at the first player's camera point; move it towards the second player.
    const coop::Vec3 look = guest.vec(s.first + actor::kCameraTrans);
    guest.set_vec(s.first + actor::kCameraTrans, look + share.offset);

    if (trace_enabled() && (++s.frames % 120u) == 0u) {
        trace("first (%.0f %.0f %.0f) second (%.0f %.0f %.0f) camera offset (%.0f %.0f %.0f)%s", first_position.x,
              first_position.y, first_position.z, second_position.x, second_position.y, second_position.z,
              share.offset.x, share.offset.y, share.offset.z, too_far ? " out of range" : "");
        if (s.split.amount() > 0.0f) {
            trace("split %.2f, line at %.0f degrees", s.split.amount(), s.layout.angle() * 57.2958f);
        }
    }
}

// The lens of a player's half: the game's, except the first player's while their view is held (the game's
// lens is the other player's event camera's then) and as it eases back, and the second player's while their
// half waits for the event camera.
std::array<float, 16> half_lens(const State& s, bool second) {
    if (!second && s.hold.active()) return s.hold.projection();
    if (second && s.second_awaits_event && s.hold.active()) return s.hold.projection();
    return s.handover.projection(s.projection, second);
}

// The projection of a player's half: centred on the middle of their side of the line as the split eases in.
std::array<float, 16> half_projection(const State& s, bool second) {
    std::array<float, 16> projection = half_lens(s, second);
    const std::array<float, 2> centre = s.layout.centre(second, s.screen_width, s.screen_height);
    coop::split_projection(projection, centre[0], centre[1], s.split.eased());
    return projection;
}

// While the second player's camera event plays, the halves swap cameras: the game camera (theirs) draws
// first, in their half, and the first player's held view second.
bool events_swapped(const State& s) { return s.event_second && s.hold.active(); }
// Whether the pass being drawn is the second player's half.
bool second_half(const State& s) { return (s.pass == 2u) != events_swapped(s); }

// Before GameScene::draw3D: when split, the first pass, with the game camera: the first player's half (or
// the second's while their camera event plays). The other camera is blended in from the game's as the
// split eases in; the renderer keeps each pass to its side of the line.
void begin_split_draw(const Guest& guest, State& s) {
    s.pass = 0u;
    s.pointers.split = false;
    if (!split_screen_enabled() || s.split.amount() == 0.0f || !plausible(s.second) || !plausible(s.second_mario) ||
        !plausible(s.first) || !s.follow.started() || !s.layout.started()) {
        return;
    }
    const std::uint32_t context = camera_context(guest);
    if (context == 0u) return;
    s.camera1 = read_camera(guest, context);
    s.projection = read_floats<16>(guest, context + camera::kProjection);
    if (!finite(s.camera1.view) || !finite(s.camera1.inverse)) return;
    const float fovy = guest.f32(context + camera::kFovy);
    const float aspect = guest.f32(context + camera::kAspect);
    const float far_z = guest.f32(context + camera::kFarZ);
    if (std::isfinite(fovy) && fovy > 1.0f && fovy < 170.0f) s.fovy = fovy;
    if (std::isfinite(aspect) && aspect > 0.1f) s.aspect = aspect;
    if (std::isfinite(far_z) && far_z > 1.0f) s.far_z = far_z;
    {
        SavedContext saved(guest.context());
        const auto width = static_cast<float>(guest.call(fn::kGetFrameBufferWidth) & 0xFFFFu);
        const auto height = static_cast<float>(guest.call(fn::kGetFrameBufferHeight) & 0xFFFFu);
        if (width >= 320.0f && width <= 1024.0f) s.screen_width = width;
        if (height >= 240.0f && height <= 640.0f) s.screen_height = height;
    }
    const coop::Vec3 first_position = guest.vec(s.first + actor::kPosition);
    const coop::Vec3 second_position = guest.vec(s.second + actor::kPosition);
    if (!events_swapped(s)) {
        // A game camera set up at the second player's warp while aiming at the first player is not the first
        // player's: their half holds its last good view until the game camera is back with them.
        if (!s.level_start &&
            coop::camera_is_second_players(coop::pose_of(s.camera1.inverse).eye, first_position, second_position)) {
            if (!s.hold.active()) {
                s.hold.start(s.last_camera1, s.last_origin, s.last_projection);
                trace("the game camera is at the second player: the first player's half holds");
            } else {
                s.hold.rearm();
            }
        } else if (!s.hold.active() && !s.handover.active()) {
            s.last_camera1 = s.camera1;
            s.last_projection = s.projection;
            s.last_origin = first_position;
        }
    }
    // Pans from the first player's view at the split to the second player's own.
    s.camera2 = events_swapped(s) ? s.hold.view(first_position)
                                  : coop::blend_cameras(s.camera1, s.follow_view, s.split.eased());
    if (!events_swapped(s) && s.second_handover.active()) {
        s.camera2 = s.second_handover.camera(s.camera2, true);
        s.second_handover.advance();
    }

    s.pass = 1u;
    s.draw_scene = guest.context()->gpr[3];
    // The game camera's half draws with it, except while it changes hands: after the second player's event
    // the first player's half holds its view until the game camera is back, then eases into it; at the start
    // of the event the second player's half eases from its own camera into the event's.
    coop::CameraMatrices game_half = s.camera1;
    if (events_swapped(s) && s.second_awaits_event) {
        const coop::Vec3 eye = coop::pose_of(s.camera1.inverse).eye;
        if (coop::length(eye - second_position) < coop::length(eye - first_position)) {
            const std::array<float, 16> lens = s.hold.projection();
            s.handover.start(s.follow_view, true, &lens);
            s.second_awaits_event = false;
            trace("the event camera has reached the second player");
        } else {
            game_half = s.follow_view;
        }
    }
    if (s.hold.active() && !events_swapped(s)) {
        // The first player's half, held through the second player's flight and until the game camera is back.
        const coop::CameraMatrices held = s.hold.view(first_position);
        if (s.hold.returning() && s.hold.back(s.camera1, first_position)) {
            const std::array<float, 16> held_lens = s.hold.projection();
            s.handover.start(held, false, &held_lens);
            trace("the game camera is back with the first player");
        } else {
            game_half = held;
        }
    }
    if (s.handover.active()) {
        game_half = s.handover.camera(game_half, second_half(s));
        s.handover.advance();
    }
    if (game_half.view != s.camera1.view) {
        SavedContext saved(guest.context());
        write_camera(guest, context, game_half);
        guest.call(fn::kLoadViewMtx);
        guest.call(fn::kCalcViewAndEntry);
    }
    s.second_view = second_half(s) ? game_half : s.camera2;
    if (s.camera_trace_frames > 0u && trace_enabled()) {
        --s.camera_trace_frames;
        const coop::Vec3 game_eye = coop::pose_of(s.camera1.inverse).eye;
        const coop::Vec3 drawn_eye = coop::pose_of(game_half.inverse).eye;
        const coop::Vec3 other_eye = coop::pose_of(s.camera2.inverse).eye;
        const coop::Vec3 drawn_fwd = coop::pose_of(game_half.inverse).forward;
        const coop::Vec3 other_fwd = coop::pose_of(s.camera2.inverse).forward;
        trace("cam f%u pass1=%s ev=%d hold=%d/%d ho=%d ho2=%d line=%.0f game(%.0f %.0f %.0f) "
              "pass1(%.0f %.0f %.0f >%.2f %.2f %.2f) pass2(%.0f %.0f %.0f >%.2f %.2f %.2f) fov00=%.3f",
              120u - s.camera_trace_frames, second_half(s) ? "second" : "first", s.event_second ? 1 : 0,
              s.hold.active() ? 1 : 0, s.hold.returning() ? 1 : 0, s.handover.active() ? 1 : 0,
              s.second_handover.active() ? 1 : 0, s.layout.angle() * 57.2958f, game_eye.x, game_eye.y, game_eye.z,
              drawn_eye.x, drawn_eye.y, drawn_eye.z, drawn_fwd.x, drawn_fwd.y, drawn_fwd.z, other_eye.x, other_eye.y,
              other_eye.z, other_fwd.x, other_fwd.y, other_fwd.z, s.projection[0]);
    }
    write_floats(guest, context + camera::kProjection, half_projection(s, second_half(s)));
    s.pointers.camera[second_half(s) ? 1u : 0u] = game_half;
    s.pointers.projection[second_half(s) ? 1u : 0u] = half_projection(s, second_half(s));
    // The renderer keeps EFB copies per view: per player, so each half's effects see only its own picture
    // whichever pass draws it.
    send_split_clip(s.layout.plane(second_half(s), s.screen_width, s.screen_height), second_half(s) ? 2u : 1u);
}

// After the first half: the second, with the second player's camera, then the first player's camera back for
// the 2D layer and the next frame.
void finish_split_draw(const Guest& guest, State& s) {
    if (s.pass != 1u) return;
    SavedContext saved(guest.context());
    const std::uint32_t context = camera_context(guest);
    if (context == 0u) {
        s.pass = 0u;
        return;
    }
    s.pass = 2u;
    write_camera(guest, context, s.camera2);
    write_floats(guest, context + camera::kProjection, half_projection(s, second_half(s)));
    s.pointers.camera[second_half(s) ? 1u : 0u] = s.camera2;
    s.pointers.projection[second_half(s) ? 1u : 0u] = half_projection(s, second_half(s));
    s.pointers.split = true;
    guest.call(fn::kLoadViewMtx);
    guest.call(fn::kCalcViewAndEntry);
    send_split_clip(s.layout.plane(second_half(s), s.screen_width, s.screen_height), second_half(s) ? 2u : 1u);
    guest.call(fn::kGameSceneDraw3D, s.draw_scene);
    // The whole screen again for the 2D layer (HUD), and the black line between the halves, fading in with
    // the split.
    const std::array<float, 3> line = s.layout.plane(true, s.screen_width, s.screen_height);
    send_split_clip({0.0f, 0.0f, 0.0f}, 0u, {line[0], line[1], line[2], s.split.eased()});
    write_camera(guest, context, s.camera1);
    write_floats(guest, context + camera::kProjection, s.projection);
    guest.call(fn::kLoadViewMtx);
    s.pass = 0u;
    guest.call(fn::kSetDefaultViewportAndScissor);
}

double fpr(const PpcContext* context, unsigned index) { return std::bit_cast<double>(context->fpr_bits[index]); }

// After ShadowController::updateFarClipping: a shadow past the far clip from the first player's camera is
// drawn again if it is near enough to the second player's, so their side of a split screen has shadows.
void keep_second_player_shadow(const Guest& guest, State& s) {
    const std::uint32_t controller = s.shadow_controller;
    s.shadow_controller = 0u;
    if (!plausible(controller) || guest.u8(controller + kShadowFarClipped) == 0u) return;
    SavedContext saved(guest.context());
    Guest::Frame frame(guest);
    const std::uint32_t drop = frame.vec(0, {});
    guest.call(fn::kShadowGetDropPos, controller, drop);
    const coop::Vec3 position = guest.vec(drop);
    const float depth = -coop::transform_point(s.camera2.view, position).z;
    if (std::isfinite(depth) && depth < s.shadow_clip) guest.set_u8(controller + kShadowFarClipped, 0u);
}

void fill_partner_kpad(const Guest& guest, State& s) {
    if (!plausible(s.kpad_buffer)) return;
    const coop::KpadStatusBytes bytes = s.kpad.next(coop_partner_input());
    for (std::size_t at = 0; at < bytes.size(); at += 4u) {
        guest.set_u32(s.kpad_buffer + static_cast<std::uint32_t>(at),
                      (static_cast<std::uint32_t>(bytes[at]) << 24u) | (static_cast<std::uint32_t>(bytes[at + 1u]) << 16u) |
                          (static_cast<std::uint32_t>(bytes[at + 2u]) << 8u) | bytes[at + 3u]);
    }
    // The orange Star Pointer (port 1) moves, shows and collects only on a connected Remote: WPad::mIsConnected
    // (+0x35, MR::isConnectedWPad), which only WPAD's connect callback sets. Both of WPadHolder's pads, as
    // swap_pads may have them either way round.
    const std::uint32_t holder = guest.call(fn::kGetWPadHolder);
    for (std::uint32_t at = 0u; plausible(holder) && at < 8u; at += 4u) {
        const std::uint32_t pad = guest.u32(holder + at);
        if (plausible(pad)) guest.set_u8(pad + 0x35u, 1u);
    }
    guest.context()->gpr[3] = 1u;  // one status read (after the call above, which returns in r3)
}

}  // namespace

// JKRSolidHeap::allocFromHead/allocFromTail(this, size, align): an allocation the stage's MEM1 heap cannot hold
// comes from its MEM2 heap (coop::scene_heap_overflows). Solid heaps are freed whole with the stage, so the
// borrowed block goes when the MEM2 heap does. With or without co-op: it only replaces a crash.
void borrow_from_mem2(PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                      std::uint32_t guest_pc) {
    const Guest guest(context, memory, services, guest_pc);
    const std::uint32_t heap = context->gpr[3];
    const std::uint32_t watcher = guest.u32(kHeapMemoryWatcher);
    if (heap == 0u || !plausible(watcher) || heap != guest.u32(watcher + kSceneHeapNapa)) return;
    const std::uint32_t mem2 = guest.u32(watcher + kSceneHeapGddr);
    if (mem2 < 0x90000000u || mem2 >= 0x94000000u) return;
    const std::uint32_t size = context->gpr[4];
    const auto align = static_cast<std::uint32_t>(std::abs(static_cast<std::int32_t>(context->gpr[5])));
    const std::uint32_t mem1_free = guest.u32(heap + kSolidHeapFreeSize);
    const std::uint32_t mem2_free = guest.u32(mem2 + kSolidHeapFreeSize);
    if (!coop::scene_heap_overflows(mem1_free, mem2_free, size, align)) return;
    context->gpr[3] = mem2;
    static unsigned logged = 0u;
    if (logged++ < 8u) {
        std::fprintf(stderr, "[heap] the stage's MEM1 heap is full (%u bytes left): %u bytes from its MEM2 heap (%u KB left)\n",
                     mem1_free, size, mem2_free / 1024u);
    }
}

// The save-select screen's switches (docs/MULTIPLAYER.md), read from the first player's controller: Wii 1 turns
// co-op on or off for the save about to be chosen, + the Odyssey moveset (on release, and only pressed alone: - and
// + together open the settings). The renderer draws the panel; co-op stays as chosen once a save loads.
struct FileSelectMenu {
    std::uint32_t kpad_buffer = 0u;  // KPADRead's status buffer for channel 0, between its before and after hooks
    std::uint32_t hold = 0u;
    bool plus_alone = false;
    bool shown = false;
};
FileSelectMenu s_menu;

void file_select_frame(const Guest& guest, std::uint32_t samples) {
    const std::uint32_t system = guest.u32(kGameSystem);
    const std::uint32_t controller = plausible(system) ? guest.u32(system + 0x24u) : 0u;
    const bool shown = plausible(controller) && guest.string(controller + 0x00u, 31u) == "Game" &&
                       guest.string(controller + 0x20u, 31u) == "FileSelect";
    if (shown != s_menu.shown) {
        s_menu.shown = shown;
        set_file_select_options_shown(shown);
        s_menu.hold = 0u;
        s_menu.plus_alone = false;
    }
    if (!shown) return;
    RuntimeSettings settings = get_runtime_settings();
    if (!coop_enabled() != !settings.coop) set_coop_session(settings.coop);
    if (samples == 0u || samples > 0x10u) return;
    const std::uint32_t hold = guest.u32(s_menu.kpad_buffer);  // KPADStatus::hold, the newest sample
    const std::uint32_t pressed = hold & ~s_menu.hold;
    const std::uint32_t released = s_menu.hold & ~hold;
    s_menu.hold = hold;
    namespace b = coop::button;
    bool changed = false;
    if ((pressed & b::kOne) != 0u) {
        settings.coop = !settings.coop;
        set_coop_session(settings.coop);
        changed = true;
        trace("save select: multiplayer %s", settings.coop ? "on" : "off");
    }
    if ((pressed & b::kPlus) != 0u) s_menu.plus_alone = (hold & b::kMinus) == 0u;
    if ((hold & b::kMinus) != 0u) s_menu.plus_alone = false;
    if ((released & b::kPlus) != 0u && s_menu.plus_alone) {
        s_menu.plus_alone = false;
        settings.moveset.enabled = !settings.moveset.enabled;
        changed = true;
        trace("save select: odyssey moves %s", settings.moveset.enabled ? "on" : "off");
    }
    if (!changed) return;
    set_runtime_settings(settings);
    if (!save_runtime_settings_to_file()) trace("save select: the settings file could not be written");
}

void coop_before_call(std::uint32_t id, PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                      std::uint32_t guest_pc) {
    if (id == call::kSolidHeapAllocHead || id == call::kSolidHeapAllocTail) {
        if (services != nullptr) borrow_from_mem2(context, memory, services, guest_pc);
        return;
    }
    if (id == call::kKpadRead) netplay_kpad_before(context, memory, guest_pc);
    // KPADRead(chan, ...): the agent's frame, at the first player's controller read.
    if (id == call::kKpadRead && context->gpr[3] == 0u && services != nullptr && services->call_guest != nullptr) {
        agent_frame(context, memory, services, guest_pc);
        s_menu.kpad_buffer = context->gpr[4];
    }
    if (services == nullptr || services->call_guest == nullptr || !coop_enabled()) return;
    State& s = state();
    (void)memory;
    (void)guest_pc;
    if (id >= call::kStartPlayerEvent && id <= call::kClearBgmQueue) {
        on_game_over_call(id, Guest(context, memory, services, guest_pc), s, context, guest_pc);
        return;
    }
    if (id == call::kMarioInit2D || id == call::kMarioUpdate2D) {
        // The second player's 2D updates write the one life meter (theirs is fed by update_hud_count).
        if (plausible(s.second) && context->gpr[3] == s.second) context->pc = kCoopSkipCall;
        return;
    }
    if (id >= call::kMeterRequestFirst && id <= call::kPlayerLeftLast) {
        s.hud_call_this = context->gpr[3];
        return;
    }
    if (id == call::kIncPlayerLife) {
        s.heal_amount = context->gpr[3];
        s.heal_second_turn = s.moving_second;
        return;
    }
    if (id >= call::kStarStepFirst && id <= call::kStarStepLast) {
        // A launch star the second player rides: its step runs with them as the game's player (its animations,
        // its end of the ride and its jump off are the rider's).
        const Guest guest(context, memory, services, guest_pc);
        const std::uint32_t star = context->gpr[3];
        if (!s.moving_second && plausible(s.second) && plausible(star) && guest.u32(star + kStarBindActor) == s.second) {
            SavedContext saved(context);
            set_game_player(guest, s, true);
            if (s.star_swapped == 0u && s.star_traced != star) {
                s.star_traced = star;
                trace("launch star %08X: the second player rides it", star);
            }
            s.star_swapped = star;
        }
        return;
    }
    if (id >= call::kPipeStepFirst && id <= call::kPipeStepLast) {
        // A pipe step: the first brother in waits inside for the other (exePlayerIn skipped); the second
        // player's run with him as the game's player, as launch stars do.
        const Guest guest(context, memory, services, guest_pc);
        const std::uint32_t pipe = context->gpr[3];
        if (!plausible(pipe) || !plausible(s.first) || !plausible(s.second)) return;
        const std::uint32_t rider = guest.u32(pipe + kPipeRider);
        if (pipe != s.pipe_traced || id != s.pipe_traced_step) {
            s.pipe_traced = pipe;
            s.pipe_traced_step = id;
            trace("pipe %08X step %u, rider %s", pipe, id - call::kPipeStepFirst,
                  rider == s.first ? "first" : rider == s.second ? "second" : "none");
        }
        if (id == call::kPipePlayerOut) s.pipe_out = pipe;  // for after it (its r3 is gone by then)
        if (id == call::kPipePlayerIn && pipe_holds(guest, s, pipe, rider)) {
            context->pc = kCoopSkipCall;
            return;
        }
        if (!s.moving_second && rider == s.second) {
            SavedContext saved(context);
            set_game_player(guest, s, true);
            s.pipe_swapped = pipe;
        }
        return;
    }
    if (id == call::kIsNearPlayer || id == call::kIsNearPlayerAnyTime) {
        s.near_actor = context->gpr[3];
        s.near_distance = static_cast<float>(fpr(context, 1));
        return;
    }
    if (id == call::kNearerPlayerCode) {
        if (s.nearer_depth++ == 0u) nearer_player_code(Guest(context, memory, services, guest_pc), s);
        return;
    }
    if (id == call::kTryTalk || id == call::kTryTalkNpc ||
        (id == call::kTalkDirectorStep && guest_pc >= 0x8029E8ECu && guest_pc < 0x8029E920u)) {
        if (s.talk.depth++ != 0u) return;
        const Guest guest(context, memory, services, guest_pc);
        if (id == call::kTalkDirectorStep) {
            talk_director_begin(guest, s, context->gpr[3]);
        } else {
            const std::uint32_t arg = context->gpr[3];  // TalkMessageCtrl (its NPC at +0xC), or the NPC
            talk_begin(guest, s, id == call::kTryTalkNpc ? arg : plausible(arg) ? guest.u32(arg + 0xCu) : 0u);
        }
        return;
    }
    if (id == call::kTalkDirectorStep) return;
    if (id == call::kEndRemoteDemo) {
        // A talk's hold may end later (DemoDirector), with the first player as the game's: it ends on the talker.
        s.talk.end_info = context->gpr[3];
        // Any hold ends on the brother it was put on (a demo the second player started may end with the first
        // as the game's player), and a talk's on its talker.
        if (s.talk.held && s.talk.held_second != s.moving_second && plausible(s.first) && plausible(s.second)) {
            const Guest guest(context, memory, services, guest_pc);
            SavedContext saved(context);
            s.talk.end_back_to_second = s.moving_second;
            set_game_player(guest, s, s.talk.held_second);
            s.talk.end_swapped = true;
        }
        s.talk.held = false;
        return;
    }
    if (id == call::kReadyRemoteDemo) return;
    if (id == call::kPadButtonA || id == call::kPadTriggerA) {
        s.talk.pad_channel = context->gpr[3];
        return;
    }
    if (id == call::kPointingPane) {
        for (unsigned i = 0; i < 5u; ++i) s.talk.pane_args[i] = context->gpr[3 + i];
        return;
    }
    if (id == call::kMenuDecide) return;
    if (id >= call::kTalkDemoStart && id <= call::kTalkCameraEnd) {
        if (talk_calm_skips(s, id, guest_pc)) context->pc = kCoopSkipCall;
        return;
    }
    if (id == call::kCheckButton) {
        s.talk.button = context->gpr[4];
        return;
    }
    if (id == call::kCameraNearlyEnd || id == call::kCanStartDemo) return;
    if (id == call::kLodDistance) {
        s.talk.lod = context->gpr[3];
        return;
    }
    if (id == call::kBalloonPos) return;
    if (id >= call::kPointingCore && id <= call::kSpineUpdate) {
        pointers_before(id, Guest(context, memory, services, guest_pc), s, context, guest_pc);
        return;
    }
    if (id == call::kGetPlayerPos) return;
    if (id == call::kAddArchive) {
        s.archive_collector = context->gpr[3];
        s.archive_name = context->gpr[4];
        return;
    }
    if (id >= call::kAttackFirst && id <= call::kAttackLast) {
        // MarioActor::attackSensor's attacks and pushes on another player: a landing on the partner was a
        // stomp, and its bounce landed on them again, forever. The players push each other aside instead
        // (bump_players).
        const Guest guest(context, memory, services, guest_pc);
        const std::uint32_t sensor = context->gpr[4];
        const std::uint32_t host = plausible(sensor) ? guest.u32(sensor + kSensorHost) : 0u;
        if (plausible(s.second) && host != context->gpr[3] && (host == s.first || host == s.second)) {
            context->pc = kCoopSkipCall;
        }
        return;
    }
    if (id == call::kGetPlayerLeft) return;
    switch (id) {
    case call::kKpadRead:
        s.kpad_channel = context->gpr[3];
        s.kpad_buffer = context->gpr[4];
        return;
    case call::kMarioActorInit2:
        if (s.init2_depth < s.init2_actors.size()) s.init2_actors[s.init2_depth] = context->gpr[3];
        ++s.init2_depth;
        return;
    case call::kCategoryListExecuteMovement:
        if (s.category_depth < s.categories.size()) s.categories[s.category_depth] = context->gpr[3];
        ++s.category_depth;
        return;
    case call::kMountArchive:
        if (!s.mounting_partner) {
            s.mount_path = context->gpr[3];
            s.mount_heap = context->gpr[4];
        }
        return;
    case call::kCreateAndAddPlayerArchives:
        s.player_archives_is_mario = context->gpr[4] & 0xFFu;
        return;
    case call::kPlayerHeapCreateHeap:
        // Both characters' player archives share PlayerHeapHolder's heaps (the stationed-archive loader
        // shrinks them to fit once loaded).
        if (context->gpr[3] == kPlayerHeapSize) context->gpr[3] = kPlayerHeapSize + kPartnerHeapExtra;
        return;
    case call::kGameSceneDraw3D: {
        const Guest guest(context, memory, services, guest_pc);
        {
            SavedContext saved(context);
            place_hud(guest, s);
        }
        begin_split_draw(guest, s);
        return;
    }
    case call::kClipFrustum:
        // ClippingJudge::isJudgedToClipFrustum(this, const TVec3f& position, f32 radius, s32 level).
        s.clip_position = s.split.amount() > 0.0f && s.follow.started() ? context->gpr[4] : 0u;
        s.clip_radius = static_cast<float>(fpr(context, 1));
        s.clip_far = coop::clip_level_distance(context->gpr[5], s.far_z);
        return;
    case call::kNameObjHolderCtor:
        // NameObjHolder::NameObjHolder(this, int num): the scene's (num 0x1300) gets room for the second player.
        if (context->gpr[4] >= coop::kSceneNameObjHolderMin) {
            s.scene_holder = context->gpr[3];
            s.holder_capacity = context->gpr[4];
            s.holder_count = 0u;
            context->gpr[4] = coop::scene_name_obj_capacity(context->gpr[4]);
            trace("scene object table: %u entries (game's %u)", context->gpr[4], s.holder_capacity);
        }
        return;
    case call::kNameObjHolderAdd:
        // NameObjHolder::add(this, NameObj*).
        if (context->gpr[3] == s.scene_holder && s.scene_holder != 0u && ++s.holder_count == s.holder_capacity + 1u) {
            trace("scene object table past the game's %u entries (the extra room is in use)", s.holder_capacity);
        }
        return;
    case call::kSetPlayerMode:
        // MarioActor::setPlayerMode(this, u16 mode, bool): the second player stays as they are rather than
        // take a power-up whose model was not loaded for their character (the game loads only the first
        // player's character's power-up archives; the empty model crashed).
        // Nor do both players become Boos: the second's Boo parts draw in the first's Boo draw buffer, which
        // holds one (DrawBuffer::add overran it). Either way the player keeps the form they have.
        if (plausible(s.second) && (context->gpr[3] == s.second || context->gpr[3] == s.first)) {
            const std::uint32_t player = context->gpr[3];
            const std::uint32_t other = player == s.second ? s.first : s.second;
            const auto mode = static_cast<std::uint16_t>(context->gpr[4] & 0xFFFFu);
            const int slot = coop::power_up_model_slot(mode);
            const Guest guest(context, memory, services, guest_pc);
            const bool missing = player == s.second &&
                                 (slot == coop::kTeresaParts ? guest.u32(s.second + kActorTeresaParts) == 0u
                                  : slot >= 0 ? guest.u32(s.second + kActorModels + 4u * static_cast<std::uint32_t>(slot)) == 0u
                                              : false);
            const bool both_boo = slot == coop::kTeresaParts && (guest.u32(other + kActorPlayerMode) >> 16u) == mode;
            if (missing || both_boo) {
                const std::uint32_t current = guest.u32(player + kActorPlayerMode) >> 16u;
                trace("%s player's power-up %u not taken (%s)", player == s.second ? "second" : "first", mode,
                      missing ? "no model" : "the other is a Boo");
                context->gpr[4] = current;
            }
        }
        return;
    case call::kShadowFarClipping:
        // ShadowController::updateFarClipping(this, f32 clip).
        s.shadow_controller = s.split.amount() > 0.0f && s.follow.started() ? context->gpr[3] : 0u;
        s.shadow_clip = static_cast<float>(fpr(context, 1));
        return;
    default:
        return;
    }
}

void coop_after_call(std::uint32_t id, PpcContext* context, GuestMemoryV1* memory, const NativeServicesV1* services,
                     std::uint32_t guest_pc) {
    // Netplay's recording or replay of the frame's controller data (netplay.h), once a channel's is final: the
    // second player's after co-op has written it (below).
    if (id == call::kKpadRead && !(coop_enabled() && netplay_pending_channel() == 1u)) {
        netplay_kpad_after(context, memory, services, guest_pc);
    }
    if (id == call::kKpadRead && s_menu.kpad_buffer != 0u && services != nullptr && services->call_guest != nullptr) {
        file_select_frame(Guest(context, memory, services, guest_pc), context->gpr[3]);
        s_menu.kpad_buffer = 0u;
    }
    if (services == nullptr || services->call_guest == nullptr || !coop_enabled()) return;
    State& s = state();
    const Guest guest(context, memory, services, guest_pc);
    if (id >= call::kMeterRequestFirst && id <= call::kPlayerLeftLast) {
        // The game's own life meter or lives counter: the second player's does the same.
        const bool meter = id <= call::kMeterRequestLast;
        const std::uint32_t theirs = meter ? s.first_meter : s.first_left;
        const std::uint32_t mine = meter ? s.second_meter : s.second_left;
        if (theirs != 0u && mine != 0u && s.hud_call_this == theirs) {
            SavedContext saved(context);
            guest.call(meter ? fn::kMeterRequests[id - call::kMeterRequestFirst] : fn::kPlayerLeftCalls[id - call::kPlayerLeftFirst],
                       mine);
        }
        s.hud_call_this = 0u;
        return;
    }
    if (id == call::kGetPlayerLeft) {
        // The second player's lives counter asks for their lives (PlayerLeft keeps itself in r30 or r31).
        if (s.second_left != 0u && s.second_lives >= 0 && in_player_left(guest_pc) &&
            (context->gpr[30] == s.second_left || context->gpr[31] == s.second_left)) {
            context->gpr[3] = static_cast<std::uint32_t>(s.second_lives);
        }
        return;
    }
    if (id == call::kIncPlayerLife) {
        // A coin heals both players.
        const std::uint32_t other = s.heal_second_turn ? s.first : s.second;
        const unsigned other_index = s.heal_second_turn ? 0u : 1u;
        if (plausible(s.second) && plausible(other) && !s.down[other_index].down && s.heal_amount > 0u &&
            s.heal_amount < 16u) {
            SavedContext saved(context);
            guest.call(fn::kMarioIncLife, other, s.heal_amount);
        }
        return;
    }
    if (id == call::kAddArchive) {
        add_partner_archive(guest, s);
        return;
    }
    if (id >= call::kStarStepFirst && id <= call::kStarStepLast) {
        if (s.star_swapped != 0u) {
            SavedContext saved(context);
            set_game_player(guest, s, false);
            s.star_swapped = 0u;
        }
        return;
    }
    if (id >= call::kPipeStepFirst && id <= call::kPipeStepLast) {
        if (s.pipe_swapped != 0u) {
            SavedContext saved(context);
            set_game_player(guest, s, false);
            s.pipe_swapped = 0u;
        }
        if (id == call::kPipePlayerOut && s.pipe_out != 0u) {
            pipe_after_exit(guest, s, s.pipe_out);
            s.pipe_out = 0u;
        }
        return;
    }
    if (id == call::kTryTalk || id == call::kTryTalkNpc ||
        (id == call::kTalkDirectorStep && guest_pc >= 0x8029E8ECu && guest_pc < 0x8029E920u)) {
        if (s.talk.depth == 0u || --s.talk.depth != 0u) return;
        talk_end(guest, s);
        return;
    }
    if (id == call::kTalkDirectorStep) return;
    if (id == call::kReadyRemoteDemo) {
        s.talk.held = true;
        s.talk.held_second = s.moving_second;
        talk_hold_other(guest, s);
        return;
    }
    if (id == call::kEndRemoteDemo) {
        talk_release_other(guest, s);
        return;
    }
    if (id == call::kPadButtonA || id == call::kPadTriggerA) {
        // The second pointer's Co-Star grip (StarPointerLayout's Grip and Hold, with their sound) is not shown: its
        // A is the second player's jump.
        if (in_star_pointer_layout(guest_pc) && s.talk.pad_channel == 1u) {
            context->gpr[3] = 0u;
            return;
        }
        talk_either_a(guest, s, context, id == call::kPadButtonA ? fn::kTestPadButtonA : fn::kTestPadTriggerA, guest_pc);
        return;
    }
    if (id == call::kPointingPane || id == call::kMenuDecide) {
        talk_either_choice(guest, s, context, id == call::kPointingPane, guest_pc);
        return;
    }
    if (id >= call::kTalkDemoStart && id <= call::kTalkCameraEnd) return;
    if (id == call::kCameraNearlyEnd) {
        if (s.talk.calm && guest_pc == 0x8029D968u) context->gpr[3] = 1u;  // no talk camera to wait for
        return;
    }
    if (id == call::kCanStartDemo) {
        if (s.talk.calm && demo_request_site(guest_pc)) context->gpr[3] = 0u;
        return;
    }
    if (id == call::kCheckButton) {
        if (s.talk.calm && s.talk.button == 3u) context->gpr[3] = 0u;  // A (MarioModule::checkTrgA's 3) pages
        return;
    }
    if (id == call::kLodDistance) {
        lod_distance(guest, s, context);
        return;
    }
    if (id == call::kBalloonPos) {
        if (guest_pc == 0x8029CFA0u) talk_window_in_world(guest, s, context);
        return;
    }
    if (id >= call::kPointingCore && id <= call::kSpineUpdate) {
        pointers_after(id, guest, s, context);
        return;
    }
    if (id == call::kNearerPlayerCode) {
        if (s.nearer_depth == 0u || --s.nearer_depth != 0u || !s.nearer_swapped) return;
        SavedContext saved(context);
        set_game_player(guest, s, s.nearer_back_to_second);
        s.nearer_swapped = false;
        return;
    }
    if (id == call::kIsNearPlayer || id == call::kIsNearPlayerAnyTime) {
        if ((context->gpr[3] & 0xFFu) == 0u && near_other_player(guest, s)) context->gpr[3] = 1u;
        s.near_actor = 0u;
        return;
    }
    if (id == call::kGetPlayerPos) {
        if (guest_pc >= 0x8002574Cu && guest_pc < 0x80025814u) {
            switch_area_player_pos(guest, s, context);  // SwitchArea::movement
        } else {
            bunny_player_pos(guest, s, context, guest_pc);
        }
        return;
    }
    switch (id) {
    case call::kKpadRead:
        if (s.kpad_channel == 1u) fill_partner_kpad(guest, s);
        if (netplay_pending_channel() == 1u) netplay_kpad_after(context, memory, services, guest_pc);
        s.kpad_channel = 0xFFFFFFFFu;
        return;
    case call::kGameDataHolderIsDataMario:
    case call::kGameDataFunctionIsDataMario:
        if (s.answer_partner) {
            context->gpr[3] = coop::partner_of(s.own) == coop::Character::Mario ? 1u : 0u;
        } else {
            s.own = (context->gpr[3] & 0xFFu) != 0u ? coop::Character::Mario : coop::Character::Luigi;
        }
        return;
    case call::kMarioActorInit2: {
        if (s.init2_depth == 0u) return;
        --s.init2_depth;
        const std::uint32_t actor = s.init2_depth < s.init2_actors.size() ? s.init2_actors[s.init2_depth] : 0u;
        if (s.creating_second || !plausible(actor)) return;
        create_second_player(guest, s, actor);
        return;
    }
    case call::kCategoryListExecuteMovement: {
        if (s.category_depth == 0u) return;
        --s.category_depth;
        const std::uint32_t category = s.category_depth < s.categories.size() ? s.categories[s.category_depth] : 0u;
        if (category != kMovementTypePlayer || s.moving_second || !plausible(s.first) || !plausible(s.second)) return;
        move_second_player(guest, s);
        return;
    }
    case call::kMountArchive:
        if (!s.mounting_partner) mount_partner_archives(guest, s);
        return;
    case call::kCreateAndAddPlayerArchives:
        add_partner_resources(guest, s);
        return;
    case call::kGameSceneDraw3D:
        finish_split_draw(guest, s);
        return;
    case call::kClipFrustum:
        // Out of the first player's view but in the second's: not clipped, so it moves and is drawn.
        if (s.clip_position != 0u && (context->gpr[3] & 0xFFu) != 0u && plausible(s.clip_position)) {
            const coop::Vec3 position = guest.vec(s.clip_position);
            if (coop::sphere_in_view(s.camera2.view, position, s.clip_radius, s.fovy, s.aspect, s.clip_far)) {
                context->gpr[3] = 0u;
            }
        }
        s.clip_position = 0u;
        return;
    case call::kShadowFarClipping:
        if (s.shadow_controller != 0u) keep_second_player_shadow(guest, s);
        return;
    default:
        return;
    }
}

unsigned coop_split_pass() noexcept { return state().pass; }

void coop_set_split_clip_sink(SplitClipSink sink, void* user) noexcept {
    split_clip_sink() = {sink, user};
}

bool coop_is_second_player_mario(std::uint32_t mario) noexcept {
    const State& s = state();
    return mario != 0u && mario == s.second_mario;
}


std::uint32_t coop_second_player(std::uint32_t first) noexcept {
    const State& s = state();
    return first != 0u && s.first == first ? s.second : 0u;
}

void coop_request_second_player_teleport(coop::Vec3 position) noexcept {
    State& s = state();
    s.agent_position = position;
    s.agent_teleport = true;
}

void coop_request_second_player_mode(std::uint16_t mode) noexcept {
    state().agent_mode = mode;
}

int coop_second_lives() noexcept {
    return state().second_lives;
}

coop::DownState coop_down_state(unsigned player) noexcept {
    return player < 2u ? state().down[player] : coop::DownState{};
}

struct CoopRollbackState {
    State state;
    FileSelectMenu menu;
};

std::shared_ptr<CoopRollbackState> coop_rollback_save() {
    auto saved = std::make_shared<CoopRollbackState>();
    saved->state = state();
    saved->menu = s_menu;
    return saved;
}

void coop_rollback_restore(const CoopRollbackState& saved) {
    state() = saved.state;
    s_menu = saved.menu;
}

}  // namespace galaxy
