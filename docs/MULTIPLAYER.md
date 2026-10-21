# Mario and Luigi

Nebula can put Mario and Luigi in the same level, each with their own controller, progressing together to
the star. It is runtime code only (`runtime/include/galaxy/coop.h`, `runtime/src/coop_guest.cpp`) plus
hooks the recompiler wraps around thirteen game calls (`coop_call_id` in `translate.rs`): no game data
changes.

There are three steps:

1. **Local, one screen** (done). One world, two players, one shared camera that frames them both.
2. **Local split screen** (done). When the players are far apart, the screen splits Lego-style along
   the line between them, each half following its own player; it joins again when they meet.
3. **Online.** One player hosts a lobby and the other joins. Both PCs run the same two-player world and
   exchange only controller inputs each frame (lockstep, as Dolphin's netplay does), so enemies, objects
   and everything else stay identical without sending game state. Each PC draws its own player's camera.
   A per-frame checksum of the game state catches drift.

## Switching it on

Nebula Settings > General > "Two players: Mario and Luigi", or `coop=on` in `runtime_settings.ini`
(`GALAXY_TWO_PLAYERS=1`; not `GALAXY_COOP`, which is the cooperative thread scheduler). It is read when the`r`ngame starts.

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
- The camera looks at the point between the players. When one is about to leave the picture the screen splits
  (below).
  More than 15000 apart for two seconds, the second player is brought back to the first.

## Split screen

When a player is about to leave the shared picture (85% of the way from its middle to an edge, or 6000 units
apart) the screen splits, Lego Indiana Jones 2 style: the line between the halves runs
through the middle of the screen, square to where the second player is from the first on screen, so each
player takes the side they are on (left and right, top and bottom, or any angle between), and the line turns
smoothly as they move around each other; a black line between the halves fades in with the split. It joins
again when both are back within 60% of the shared picture (the gap keeps it from flickering). The change takes
1.25 s, eased to be still at both ends, and is seamless:

1. At the split each half shows exactly its part of the shared picture, so nothing on screen moves.
2. Each half then slides to centre on its own player: the first player's camera eases from the midpoint to
   them, and the second player's camera pans from the first player's view to its own.
3. Joining runs the same backwards, and the single screen comes back only when both halves show one
   picture again.

The second player's camera trails them 1000 units behind and 380 above along their gravity. They steer it
with the stick bound to the pointer (usually the right stick; it has no pointer in co-op) or the D-pad: left
and right orbit it around them, up and down raise or lower it; C puts it behind them again. It stops short of
walls between it and the player (the game's `MR::getFirstPolyOnLineToMap`).

A camera event the second player sets off (a warp, launch star or pipe) plays in their half with the game's
own camera (easing in from the second player's), and the first player's half keeps its view from before,
moving with them; after it, the first player's half holds that view until the game camera is back with them,
then eases into it.
Each half's EFB copies (the bloom, blur and final scene captures) are its own, so neither half's effects
show the other's picture. While split, the second player's stick is relative to their own camera, things only the second player can
see are not clipped (they move and are drawn), their side keeps its shadows (`ShadowController`'s far clip is
measured from their camera too), and the HUD is drawn once over the whole screen.

How: `GameScene::draw3D` runs a second time with the second player's camera in `CameraContext` and its
view matrices recalculated (`SceneFunction::executeCalcViewAndEntryList`). Viewports and scissors stay
full-screen, so the game's screen-space passes (the shadow and bloom composites, which draw textures of the
whole screen) line up. Each half's camera picture is moved to the middle of its side by shifting the
projection's centre as the split eases in, and a `SplitClip` marker in the GX stream tells the renderer the
line: for draws over the whole screen every vertex shader clips the other side away before rasterizing
(`SV_ClipDistance0`), so nothing is written there, colour, depth or stencil (smaller viewports, such as
Mario's shadow capture in a corner of the EFB, are left whole; lines and points are not clipped).
`GALAXY_COOP_SPLIT=0` keeps one shared screen (the camera then hands back to the first player between 1200
and 2600 units, and the second player is brought back after two seconds past 2600).
## Lives, HUD and launch stars

Lives work as in New Super Mario Bros. Wii, without the bubble:

- Each player has their own lives and life meter. The first player's lives are the game's own (in the save);
  the second player's are kept in `coop_lives.txt` beside the save folder and start as many as the first player has.
- A player who goes down (no health left, a fall, a crush) while the other plays on spends one of their lives,
  vanishes after their fall and comes back beside the other about two seconds later, with full health. The
  game's own miss (the "Too Bad!", the restart from the checkpoint) does not happen.
- With no lives left a player sits out, carried along hidden beside the other, until the stage reloads.
- Both down at once is the game's own miss, and each spends a life; game over only when both are out of lives.
- A coin heals both players.

HUD: the second player's life meter, tinted green, stands left of the first player's, and their lives counter,
with their face, above the first player's (comet timers use the bottom centre). They are the game's own layouts
(HitPointMeter, PlayerLeft), made with the stage's HUD and shown, hidden and killed with the first player's; a
life lost alone shows on the counter.

Launch stars: the player on a launch star (either one) flies, and the other flies beside them under the
star's camera; both land together. A launch star ridden by the second player runs with them as the game's
player, so the flight's animations and its end are theirs.

The runtime side is in `coop_guest.cpp` (`on_game_over_call`, `update_down_player`, `create_hud`,
`rides_star`); the rules are `coop::go_down_alone` and `coop::respawn_due`, tested in `coop_tests.cpp`.

## Known limits

These come from a game written for one player and are what the playtest should look at:

- Enemies, doors and stars act on the first player. The second player can stomp and spin; pickups other than
  coins may count for the first player.
- Rumble goes to the first player's controller.
- The Observatory's doors and domes (AstroChangeStageCube areas) test the game's player, the first: when the
  second player walks into one the first is not in, the players swap places, so the game takes them through
  (and both arrive in the next scene). Warps, launch stars and cutscenes the game starts for the first player
  move the first player only; the second rejoins them at the next level start.
- The Odyssey moves are the first player's only.
- While the game has the first player's controls (a dome's Pull Star, the flight into a galaxy) or a
  cutscene plays, the second player travels beside the first; otherwise they are free.
- Power-ups: a stage loads the second player's power-up archives (BeeLuigi, IceLuigi, ...) with the first
  player's, so both can take every power-up. Both cannot be Boos at once (the second's Boo parts share the
  first's Boo draw buffer): whoever becomes a Boo second keeps their form.
- The players push each other aside; neither stomps the other (a landing on the partner bounced forever).
- Split screen: mirror reflections follow the first player's camera only; the second player's camera has no
  camera areas (the game's fixed and rail cameras are the first player's); an event the second player starts
  through an object (not their own move) plays in the first player's half; the temporal upscaler and motion blur assume one camera a
  frame, so turn them off while playing split. The 3D scene costs twice as much to draw while split.
- Memory: the Wii's 24 MB of MEM1 has no room for a second player on the fullest stages. When the stage's MEM1
  heap is full, the runtime takes the allocation from the stage's MEM2 heap instead (`[heap]` in the log); a
  stage that fills both still stops.

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

An agent can run most of these unattended with the play-testing tools (docs/AGENT_TESTING.md): a test copy
that jumps straight into a galaxy mission, gives either player power-ups, moves them and reports crashes.
