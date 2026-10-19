# Odyssey moveset

Nebula can give Mario Super Mario Odyssey's moves that need no Cappy, on top of Galaxy's own. It is
runtime code only (`runtime/include/galaxy/odyssey_moveset.h`, `runtime/src/odyssey_moveset.cpp`): no
game data or Odyssey assets are part of it. With the custom animations (docs/CUSTOM_ANIMATIONS.md) the
moves play Odyssey's animations converted from the player's own `.blend`; without them they use the
closest Galaxy animations.

## Switching it on

F1 > General > ODYSSEY MOVES, or `odyssey_moveset=on` in `runtime_settings.ini`
(`GALAXY_ODYSSEY_MOVESET`). It changes live. Each move can be switched off on its own:
`moveset_roll`, `moveset_roll_jump`, `moveset_ground_pound_jump`, `moveset_dive`, `moveset_turn_boost`
(`on`/`off`).

## Buttons

Odyssey's cap button (Y) is the Wii Remote's 1 button, which Galaxy does not use in play. On
controllers it is the west face button, where Odyssey has it (Xbox X, DualSense Square, Switch Pro and
Pro Controller 2 Y), which no longer spins; the camera reset (Wii C) is the left stick click. On the
keyboard it is `1`. F1 > Controls > 1 rebinds it. Z is Odyssey's crouch and ground pound, A jumps, the spin is Galaxy's.

## Moves

Speeds are Odyssey's, in units per frame (both games' units are about a centimetre; Galaxy's full run is
13, Odyssey's 14). Odyssey's values come from the Super Mario Odyssey Wiki (smo.wiki/Roll, /Dive,
/Ground_pound, /Running).

| Move | Input | What happens |
|---|---|---|
| Roll | Z (crouch), then 1 | At least 20, up to 35; loses 0.3% a frame (Odyssey 0.2%); turns at most 5 degrees a frame. Holding Z keeps it going; after its standing-up delay (47 frames, 33 out of a pound or dive, 46 after a boost) letting go stands him up at the roll's speed. Below 17 he is back in a crouch. Dust, a slide sound and a whoosh. |
| Roll boost | 1 again, every 16 frames (32 after a ground-pound roll) | 23 after a crouch roll, 26 after anything else; a gust of air. |
| Roll cancel | Spin during a roll, 9 frames after its start or a boost | Out of the roll at its speed, through Galaxy's spin. |
| Roll jump | A during a roll (after 12 frames) | A jump a little higher than the first jump, at the roll's speed. Z then A quickly stays Galaxy's long jump. |
| Ground-pound roll | Z held and 1 as a ground pound lands | A roll at 30. |
| Ground-pound jump | A as a ground pound lands | Higher than the triple jump. |
| Dive | 1 in the air, or during a ground pound | Towards the stick at 20, rising 182 units (40 out of a pound). It cannot be cancelled except by the spin; only a fresh air spin earns another dive. It hits enemies as the spin does. Landing with Z held rolls on; otherwise a smooth belly slide at the dive's speed (no hard landing), out of which 1 rolls. |
| Run after a turn | Skid, then run the other way | Odyssey's RunAfterTurn: 17 for a second, then back to the run's cap at 7/60 a frame. |
| Rolling into a wall | | Galaxy's long-jump bounce off the wall. |

Running animations follow his speed: Galaxy's walk and run blend up to 15, Odyssey's Dash from 15 and
DashFast from 18 (with footsteps), played at his speed.

Galaxy's own moves are unchanged: long jump, backflip, side flip, triple jump, wall jump and slide,
ground pound, spin.

## How it works

The recompiler puts one site hook before Mario::update's call to Mario::actionMain
(`SiteHookKind::MarioActionMain`, 40; module generation 20). Once a frame, before the game moves Mario,
the runtime reads his state from the guest (MovementStates bits, jump type, ground speed, jump velocity,
facing, gravity, Mario's constant table), asks the game for his buttons, and changes the values the game
then uses: his ground speed, jump velocity and jump type, a few flags, and animations, effects and sounds
by the game's own names. Layouts and addresses are from Petari (RMGE01). `odyssey_moveset.h` is plain
arithmetic on a snapshot and is covered by `galaxy_odyssey_moveset_tests`.

`GALAXY_TRACE_MOVESET=1` logs each move's events and a state line every second.

## Limits

- Values the Wiki does not give are estimates: the run after a turn's length, the belly slide's length,
  the dive's rise out of a ground pound, the roll jump's height.
- Galaxy has no P-meter sound; footsteps and Galaxy's own whooshes stand in.
- No roll boost hop, no speedwalking, no cap moves.
