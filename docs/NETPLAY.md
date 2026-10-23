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
- Switches: `GALAXY_NET_HOST=<port>`, `GALAXY_NET_JOIN=<address>:<port>`, `GALAXY_NET_DELAY=<frames>`.
