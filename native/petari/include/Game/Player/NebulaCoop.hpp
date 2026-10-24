#pragma once

#include <revolution/types.h>

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

    // The second player's MarioActor (null: none in this scene).
    MarioActor* getSecond();
};  // namespace NebulaCoop
