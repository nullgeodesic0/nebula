# Mario and Luigi

Nebula can put Mario and Luigi in the same level, each with their own controller, progressing together to
the star. It is runtime code only (`runtime/include/galaxy/coop.h`, `runtime/src/coop_guest.cpp`) plus
hooks the recompiler wraps around twelve game calls (`coop_call_id` in `translate.rs`): no game data
changes.

There are three steps:

1. **Local, one screen** (done). One world, two players, one shared camera that frames them both.
2. **Local split screen** (first version: left and right halves). When the players are far apart, the
   screen splits Lego-style, each half following its own player; it joins again when they meet. Splitting
   along the line between the players is to come.
3. **Online.** One player hosts a lobby and the other joins. Both PCs run the same two-player world and
   exchange only controller inputs each frame (lockstep, as Dolphin's netplay does), so enemies, objects
   and everything else stay identical without sending game state. Each PC draws its own player's camera.
   A per-frame checksum of the game state catches drift.

## Switching it on

Nebula Settings > General > "Two players: Mario and Luigi", or `coop=on` in `runtime_settings.ini`
(`GALAXY_COOP=1`). It is read when the game starts.

## Controllers

The first player plays whichever character the save file plays; the second player is the other one.

- Two or more controllers: the first connected controller is the first player's, the second is the
  second player's. Keyboard and mouse stay the first player's.
- One controller: the keyboard and mouse are the first player's, the controller is the second player's.

The second player uses their controller's own bindings (F1 > Controls). Their pointer is not used; the
spin is the shake binding or a real shake.

## What happens in game

- Both characters' models are loaded at the start (the player heap grows by 2 MB for it).
- The second player appears beside the first when the first is placed in a level, and moves through the
  game's own player code each frame with their own controller.
- The players bump into each other: when their bodies (50 units round, 150 tall) overlap, each is pushed
  half the overlap apart through the game's own `Mario::push`, so walls and floors still hold them.
- The camera looks at the point between the players. Past 1600 units apart the screen splits (below).
  More than 15000 apart for two seconds, the second player is brought back to the first.

## Split screen

Past 1600 units apart the screen splits left and right, the second player taking the side they are on;
it joins again below 1100 (the gap keeps it from flickering at one distance). The change takes 0.6 s and
is seamless, as in the Lego games:

1. At the split each half shows exactly its part of the shared picture (an off-centre projection), so
   nothing on screen moves.
2. Each half then slides to centre on its own player: the first player's camera eases from the midpoint to
   them, and the second player's camera pans from the first player's view to its own, a follow camera that
   trails them 1000 units behind and 380 above along their gravity.
3. Joining runs the same backwards, and the single screen comes back only when both halves show one
   picture again.

While split, the second player's stick is relative to their own camera, and things only the second player
can see are not clipped (they move and are drawn). The HUD is drawn once over the whole screen.

How: `GameScene::draw3D` runs a second time with the second player's camera in `CameraContext` and its
view matrices recalculated (`SceneFunction::executeCalcViewAndEntryList`). Viewports stay full-screen, so the
game's screen-space passes (the shadow and bloom composites, which draw textures of the whole screen) line
up; a scissor over the whole screen is narrowed to the half, and each camera's picture is moved into its
half by shifting the projection's centre a quarter screen as the split eases in.
`GALAXY_COOP_SPLIT=0` keeps one shared screen (the camera then hands back to the first player between 1200
and 2600 units, and the second player is brought back after two seconds past 2600).

## Known limits

These come from a game written for one player and are what the playtest should look at:

- Enemies, doors, stars and the HUD act on the first player. The second player can stomp and spin, but
  pickups, health and deaths may count for the first player or be ignored.
- Rumble goes to the first player's controller.
- Domes, warps and cutscenes move the first player only; the second rejoins them at the next level start.
- The Odyssey moves are the first player's only.
- Split screen: mirror reflections follow the first player's camera only; the second player's camera has no camera areas
  or collision (it can go through walls); the temporal upscaler and motion blur assume one camera a frame,
  so turn them off while playing split. The 3D scene costs twice as much to draw while split.

## Playtest

Run with `GALAXY_TRACE_COOP=1` to log each co-op step to the console. Things to check, in order:

1. The game boots to the file select with co-op on (archive loading worked).
2. Entering a level shows both characters (trace: a `players: first ... second ...` line).
3. The second controller moves the second character only, and jumping, spinning and Z work.
4. The camera frames both, and walking far apart brings the second player back.
5. Walking into each other pushes both apart; walking apart splits the screen smoothly and walking back
   joins it (trace: `split: second player on the right`).
6. While split, enemies near only the second player move, and the second player's stick follows their half.
7. Stomping an enemy, collecting star bits and touching the star with the second player.
6. Dying with either player, and leaving a level.

A crash or a missing character, with the trace's last lines, is the most useful report.
