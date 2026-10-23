# Online co-op (netplay)

Two PCs play Mario and Luigi co-op over the internet. Both run the whole game in deterministic mode from the
host's save and exchange only controller input; each PC computes exactly the same frames.

## Playing

On the host's PC (Mario):

    netplay-host.cmd [port] [input delay]

On the joiner's PC (Luigi):

    netplay-join.cmd <host address>[:port] [input delay]

Port 7777 (UDP) by default; a host outside the joiner's network forwards it on their router. The input delay
(default 3 frames, about 50 ms) must be the same on both. The host's game waits at the start until the joiner
connects; the joiner first downloads the host's save and game settings, then both boot in lockstep.

`netplay-local-test.cmd` runs both on one PC: Mario on the controller (host window), Luigi on the keyboard (keep
the joiner's window focused).

### Joining a game in progress (F1 > Online)

Without the scripts above, from the game itself (`runtime/src/net_lobby.cpp`):

- The player who wants company turns **F1 > Online > Joinable** on (and picks the **Port**, UDP 7777 by default).
  The page shows the address to give the friend.
- The friend types that address on the **Join** row and presses Enter.
- The host sees "<address> WANTS TO JOIN" at the top of the screen; on the Online page's status row Enter
  accepts and Backspace declines.
- **Live join** (docs/HOT_JOIN.md), when the host's game started with Joinable already on (it then runs
  deterministic from boot): the host plays on; the friend's game restarts once, receives the host's running game
  (a snapshot of the whole machine, then the host's inputs), catches up and drops in as the other brother beside
  the host, everything the host did since the last save included.
- Otherwise (Joinable turned on in a game that started without it), both games restart into a session (`GALAXY_NET_HOST` / `GALAXY_NET_JOIN`, co-op on) from the host's save as it
  was last written, and play their way back to where the host was (`GALAXY_NET_REJOIN`: the file, Mario or
  Luigi, the galaxy, the mission and the first player's position): past the logo, the file loaded, the stage
  changed, the first player put on the spot and the friend, as the other brother, beside them. Everything the host
  did since the last save (stars, coins) is not in the session.

### In a session

There is no pause: + and HOME open this PC's own menu while the game plays on (its player stands still):
**Resume**, **Exit the galaxy** (both players back to the Observatory, on both PCs at the same frame: the command
travels with the input) and **Leave the session** (the other PC is told; both games restart on their own, the
joiner with its own saves and settings again). A peer silent for 15 s ends the session the same way.
`GALAXY_NET_TEST_MENU=<frame>:<row>[,...]` chooses rows in tests.

Before the session nothing is synchronised; each game plays on its own. `lobby-local-test.cmd` runs two
ordinary games on one PC to try it. For tests, `GALAXY_NET_AUTOJOIN=<address>` asks a host at startup and
`GALAXY_NET_AUTOACCEPT=1` accepts the first asker.

Both PCs must run the same build and content (a mismatched build is refused).

## How it works

- **Deterministic mode** (`GALAXY_DETERMINISTIC=1`, implied by a session): the game's clock comes from the
  instructions executed, not the wall clock; deadlines, disc and NAND completions, PE (draw-done) events and the
  native DSP all happen at fixed points of guest execution (a checkpoint, about one loop iteration, is 3 ticks of
  the 60.75 MHz time base: GALAXY_TICKS_PER_CHECKPOINT, the same on both PCs). Replays of one input recording are
  byte-identical on any PC, at any speed (docs/AGENT_TESTING.md, "Determinism probe").
- **Inputs** (`runtime/src/netplay.cpp`, `runtime/src/netplay_session.cpp`): the game's own Wii Remote is neutral
  in deterministic mode; at the per-frame controller read each PC's controllers (every pad and the keyboard: one
  player a PC), as the input thread published them, become a KPAD status: its player's input `delay` frames later,
  sent over UDP (every packet repeats the last 8 frames, so a lost packet costs nothing). A frame waits for the peer's input if it has not arrived
  (lockstep). Every other read of a controller in the game, and the co-op second player's camera steering, use
  the same synchronised input.
- **EFB peeks** (the Star Pointer reads screen depth every frame, a GPU-dependent value) are the host's answers,
  carried with its input.
- **Pacing**: guest time is kept to real time (as in paced deterministic play), not game frames: a game frame can
  span several VI frames, and audio must play at the rate it is made.
- **Session start**: the host snapshots its save directory (every file and directory with its NAND metadata
  stream) and its game settings (co-op, moveset options, text speed, language, custom model/voice/animations,
  audio gains) when it starts. The joiner downloads that bundle before its game reads its save and plays from
  `%LOCALAPPDATA%\Nebula\netplay\<host>\` (the rejoin cache; the joiner's own saves are never touched), with its
  own controls.
- **Desync check**: MEM1 hashes every 120 frames; a mismatch is logged as `[net] DESYNC`.
- The host's window size never reaches the game (the dynamic aspect is off in deterministic mode).

## Rollback

Rollback snapshots work (`GALAXY_SNAPSHOT_TEST`, docs/AGENT_TESTING.md): the whole machine is saved at a frame
boundary and restored exactly, and re-simulated frames run without rendering. A re-simulated frame still costs
about 9 ms, so rollback is not used yet: a session is lockstep with input delay. When re-simulation is cheap
enough to fit several frames into one, prediction and rollback replace the waits.

## Testing

- `py -3 tools/agent/determinism.py netpair [--scenario observatory]`: a host and a joiner over localhost, the
  host's agent playing; their MEM1 hashes must agree at every sample.
- `py -3 tools/agent/live_join.py [--star-after N] [--exit-after N] [--join-env GALAXY_TEST_MODULE_ELSEWHERE=1]`:
  a live join of a host mid-galaxy; MEM1 must agree every 30 frames from the join (docs/HOT_JOIN.md).
- Switches: `GALAXY_NET_HOST=<port>`, `GALAXY_NET_JOIN=<address>:<port>`, `GALAXY_NET_DELAY=<frames>`,
  `GALAXY_NET_JOINABLE=1|0` (joinable from boot regardless of the setting), `GALAXY_NET_HOTJOIN=1` (a joiner taking
  a running game; set by the lobby).
