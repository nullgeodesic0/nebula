#pragma once

#include <JSystem/JGeometry.hpp>
#include <revolution/types.h>

class HitSensor;
class LiveActor;
class MarioActor;

// Nebula's Mario and Luigi co-op on the native game (TARGET_PC; docs/MULTIPLAYER.md). The game has one player: a
// MarioActor the scene reaches through its MarioHolder, whose character (gIsLuigi) and controller (Wii Remote
// channel 0) are global. A second MarioActor, the other character, is made right after the first; it is kept out
// of the game's movement list and moved straight after the player's movement category, with the MarioHolder,
// gIsLuigi and the channel 0 and 1 pads swapped to him for that call, so the game's own player code runs for him.
// His controller is channel 1, which the host connects while a second player is in (the host's router decides who:
// a free controller's +); while it is not connected he sits out, hidden and untouchable beside the first.
namespace NebulaCoop {
    // The co-op machinery runs (the host's NEBULA_OPTION_COOP): both characters' archives load, and every stage
    // makes the second player, in play or sitting out.
    bool isAvailable();

    // MarioActor::init2's end: the first player is placed; the second is made beside him.
    void afterPlayerInit(MarioActor* pActor);

    // While the second player is being made: MR::isPlayerLuigi answers for his character.
    bool isMakingSecond(bool* pIsLuigi);

    // NameObjListExecutor::executeMovement after a category: the player's is followed by the second player's turn.
    void afterMovement(int category);

    // Split screen (GameScene::draw3D): when the screen is split, sets up the first player's half and returns true;
    // the scene is then drawn, nextSplitPass sets up the second player's half, the scene is drawn again, and
    // endSplitDraw puts the game camera back for the 2D layer.
    bool beginSplitDraw();
    void nextSplitPass();
    void endSplitDraw();

    // LiveActor::movement: an object the second player is bound to (a pole, swing, rope, plant or cannon he jumped
    // into; his rush partner) runs with him as the game's player, so its steps read his position, stick and A.
    class BoundObjectScope {
    public:
        explicit BoundObjectScope(const LiveActor* pActor);
        ~BoundObjectScope();

    private:
        bool mSwapped;
    };

    // MarioActor::addRushSensor: whether a player may take this sensor's object. One rider per object (not the
    // object the other brother rides), and the vehicles (the Star Ball, the manta ray) are the first player's: the
    // second rides along unseen.
    bool mayRush(const MarioActor* pActor, const HitSensor* pSensor);

    // A free pole follows the nearer brother (Pole::exeFree): his centre.
    const TVec3f* nearerPlayerCenter(const TVec3f& rPosition);

    // While the screen is split: the camera of the half the game camera does not draw (world-to-eye), for clipping.
    bool getOtherHalfView(TPos3f* pView);

    // Tests (GALAXY_TEST_LAUNCH_STAR): a launch star's class, to tell launch stars among the scene's objects.
    void noteLaunchStar(const LiveActor* pStar);

    // The second player's MarioActor (null: none in this scene).
    MarioActor* getSecond();
};  // namespace NebulaCoop
