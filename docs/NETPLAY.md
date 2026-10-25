# Online co-op (netplay)

Two PCs play the same game together: the host plays Mario (Wii Remote channel 0), the joiner Luigi (channel 1). Only
the players' input crosses the network; each PC computes the whole game itself. That works because the game runs in
**deterministic mode**: the same input on the same frames gives the same game, bit for bit, on any PC.

## Deterministic mode

`GALAXY_DETERMINISTIC=1`, or any netplay session. Implemented in `native/sdk/os/os_native.c` (the scheme is described
at `det_step`):

- The module keeps a virtual clock: time advances only at its idle points, never with the host's wall clock.
- Retraces come from the host's frame clock (`vi_thread`): retrace N is granted once the game took N - 1, with frame
  N's remote reports staged and committed at that retrace.
- Disc and NAND completions, the decrementer and the audio interface are delivered in a fixed order at idle points.
- PE events complete as they are sent; EFB peeks read a fixed far depth; the display extent is fixed.
- The game's memory, the module, the GX ring, the disc's file table and the main.dol copy sit at fixed addresses in
  every process, so pointers the game keeps are the same everywhere.
- Floating point is portable: the game's own `libm` (`native/libm`), no FMA, no contraction.
- Locals the game reads before writing start at zero (`-ftrivial-auto-var-init=zero`): the console's stack held the
  same leftovers on every run, a PC's holds its host's (a line check's hit classification read one).

`GALAXY_STATE_HASH=<file>` writes a hash of the game's memory every 60 scene frames (`GALAXY_STATE_HASH_EVERY`,
`GALAXY_STATE_HASH_CHUNKS=1` for one per megabyte), skipping what is a process's own (native stacks, host addresses).
Replays of the same input match at every sample.

## Lockstep (default)

`GALAXY_NET_HOST=<port>` hosts, `GALAXY_NET_JOIN=<address:port>` joins (`runtime/src/net_lockstep.cpp`).

- The joiner sends its build and mods; the host refuses a different build or mods, else answers with the session: the
  settings that change what the game computes (co-op, moveset, motion style, text speed, calendar, language), the
  input delay (`GALAXY_NET_DELAY`, 3 frames by default) and its save and Mii database. The joiner plays the host's
  save, kept apart from its own (`netplay/<host>` in its settings folder).
- Each PC sends its player's input for frame N + delay as it grants retrace N; retrace N waits for both inputs.
- Every 300 scene frames both PCs exchange the memory hash; a difference is logged as a desync.

## Rollback

`GALAXY_NET_ROLLBACK=1` on the host (the joiner follows the host). Each PC runs ahead on the other's predicted input
(its last confirmed one), at most 15 frames past the last confirmed frame.

- Every retrace boundary is kept (`native/sdk/os/snapshot.c`): write watch on the game's memory, a shadow copy and
  undo records per boundary; the module's data but its `.nbhost` section; the registers of the save point. A save
  costs about 0.6 ms.
- When a confirmed input differs from the one its frame ran with, the game goes back to the boundary before that frame
  (`svc_det_boundary`) and runs the undone frames again at once, off the frame clock's schedule and not shown.
- A restore also takes back the host's part of the game's state: the NAND (kept in memory in rollback sessions and
  written to disk once no rollback can undo it), the remotes' state the game set through services, the renderer's GPU
  registers (`stream_checkpoint` / `stream_rewind`, in stream order), the GX commands written but not submitted, the
  DSP mixer and the voice clips' playback, the scene frame count and the test scripts' progress. The renderer is idle
  before a save or restore (it reads the game's memory as it draws).
- A `[netplay]` line every 600 frames gives rollbacks, frames run again and the time spent waiting.

## Live join

`GALAXY_NET_LIVE=1` beside `GALAXY_NET_HOST` (implies rollback). The host plays from boot; a joiner may come at any
time.

1. The joiner shakes hands as above.
2. At the host's next boundary F the session starts (inputs up to F + delay are neutral on both PCs) and F is kept.
3. At the boundary after, the host sends the game as it was at F: its own part (scene frame count, remotes' state,
   audio interface, DSP, GPU registers, NAND) and the module's image (`nebula_game_snapshot_export`: module data,
   unsubmitted GX commands, every non-zero page of the game's memory; about 70 MB in Good Egg).
4. The joiner boots, waits at its first boundary for the image, takes it (`NEBULA_DET_LOAD`) and goes on from F.
   Co-op is on from boot on both PCs, so Luigi's remote connects at F + 1 and he drops in beside Mario.

The host waits for the joiner while it loads (it runs at most 15 frames ahead of the joiner's input).

## Staying in sync

What one PC alone decides never reaches the game online (`netplay_ui_locked`):

- Nebula's settings menu and the HOME Menu stay closed (the HOME button is no one's input): settings change between
  sessions.
- The settings that change what the game computes are the host's (above), the language too.
- The ray tracer's answers to the game's clipping (light reach, traced sun) are those without it, whatever each PC
  draws with.
- Achievements and stats are kept on each PC, but the game shows no pop-up and reads none back.

Checks and resync (rollback and live join): every 300 boundaries each PC hashes the game's memory (as the state hash,
on worker threads, about 20 ms) and sends the hash once every input up to that boundary is confirmed (no rollback can
change it). A difference is logged (`[netplay] desync at frame N`) and the host resyncs: it stops running ahead of the
joiner's input, keeps the first boundary up to which every input is in, and sends the game there as a live join's
image; the joiner becomes it at its next boundary and runs the frames since again at once (`[netplay] resync`), about
a second after the check on a fast link; the image is about 70 MB and shares the connection with the inputs, so
on a slow upload both PCs wait while it is sent (tens of seconds at 10 Mbit/s). Lockstep sessions log a desync but do not resync: use rollback
or live join.

The host's own work at a boundary (snapshots, the session, hashes) runs on the game's stack and leaves this PC's own
values below it; the game's uninitialized temporaries (a struct's padding, copied whole) would carry them into its
memory, a difference the checks see though the game never reads it. So each boundary ends with every game stack's
dead part zeroed (`nebula_clear_dead_stack`, only nonzero words written), the game's heaps hand out blocks zeroed
(what a freed block held is this PC's history), and `JMapInfoIter` (a pointer and an index, copied by value
everywhere) has its x64 padding as a member every constructor sets.

## Tests

- `GALAXY_NET_TEST_LAG=<ms>`: the other PC's inputs count only that long after they came (a slow link on one PC).
- `GALAXY_SNAPSHOT_TEST=<save>,<restore>`: keeps boundary `<save>` and goes back to it at `<restore>`;
  `<save>,<restore>,1` becomes `<save>`'s live-join image at `<restore>` instead. A run with it must match one without.
- `GALAXY_SNAPSHOT_TIMING=1`: each save's and restore's time, logged.
- `GALAXY_NET_TEST_DESYNC=<frame>` (joiner, rollback): that frame runs with the host's A button flipped on this PC only,
  a desync for the checks to find and the resync to repair.
- `GALAXY_NET_DESYNC_DUMP=<folder>`: on a desync each PC writes its memory at the failing check, to compare.
- `GALAXY_MUTE=1`: silent output for test copies (the game's audio runs as ever).

## Limits

- Input over TCP; the game image is sent uncompressed (zero pages skipped).
- Each PC renders its own split screen; both show the same picture.
- Achievements and stats are each PC's own (no pop-ups online).
- Either PC leaving ends the session.
