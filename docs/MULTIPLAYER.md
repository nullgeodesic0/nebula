# Mario and Luigi

Nebula can put Mario and Luigi in the same level, each with their own controller, progressing together to
the star. It is runtime code only (`runtime/include/galaxy/coop.h`, `runtime/src/coop_guest.cpp`) plus
hooks the recompiler wraps around eight game calls (`coop_call_id` in `translate.rs`): no game data
changes.

This is the first step of three:

1. **Local, one screen** (this). One world, two players, one shared camera that frames them both.
2. **Local split screen.** When the players are far apart or in different areas, the screen splits
   Lego-style, each half following its own player; it joins again when they meet.
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
- The camera follows the point between the players while they are within 1200 units, eases back to the
  first player until 2600 units, and follows the first player alone beyond that. After two seconds past
  2600 units the second player is brought back to the first.

## Known limits

These come from a game written for one player and are what the playtest should look at:

- Enemies, doors, stars and the HUD act on the first player. The second player can stomp and spin, but
  pickups, health and deaths may count for the first player or be ignored.
- Rumble goes to the first player's controller.
- Domes, warps and cutscenes move the first player only; the second rejoins them at the next level start.
- The Odyssey moves are the first player's only.

## Playtest

Run with `GALAXY_TRACE_COOP=1` to log each co-op step to the console. Things to check, in order:

1. The game boots to the file select with co-op on (archive loading worked).
2. Entering a level shows both characters (trace: a `players: first ... second ...` line).
3. The second controller moves the second character only, and jumping, spinning and Z work.
4. The camera frames both, and walking far apart brings the second player back.
5. Stomping an enemy, collecting star bits and touching the star with the second player.
6. Dying with either player, and leaving a level.

A crash or a missing character, with the trace's last lines, is the most useful report.
