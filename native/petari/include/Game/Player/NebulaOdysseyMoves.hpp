#pragma once

#include <revolution/types.h>

class Mario;
class MarioActor;

// Nebula's player options on the native game (TARGET_PC): the Odyssey moves (nebula_odyssey_moveset.h) and the
// motion style (nebula_motion_style.h), as the host's settings ask (NEBULA_OPTION_MOVESET, _MOTION_STYLE).
namespace NebulaOdysseyMoves {
    // Once a frame for each Mario, before Mario::actionMain: the moves read what he is doing and what the player
    // pressed, and change the few values his action then uses. Also applies the motion style to his tuning tables.
    void tick(Mario* pMario);

    // A co-op second player's Mario (nullptr: none): his moves are his own, and need the second-player setting.
    void setSecondPlayer(const MarioActor* pActor);

    // XanimePlayer::updateInterpoleRatio, before its step: the cross-fade's ratio and frames left, eased and (in
    // the modern style) lengthened.
    void shapeInterpole(f32* pRatio, s32* pRemaining);
};  // namespace NebulaOdysseyMoves
