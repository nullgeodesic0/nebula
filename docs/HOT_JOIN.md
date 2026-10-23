# Hot join: moving a running game to another process

Question: can a running Nebula game send its whole deterministic state to another PC, so a joiner "pops in" next
to the host without the host restarting? Answer: **yes**. A rollback snapshot can be written to a file in one
process and resumed in a second process, which then plays on bit-identically (MEM1 hash equal every frame). The
prototype does this between two processes on one PC; the parts that differ between two PCs are listed under
[Across two PCs](#across-two-pcs), and none of them is a wall.

It is built: [live join](#live-join) takes a joiner from the lobby into the host's running game, Luigi beside
Mario, without the host restarting.

## Live join

`runtime/src/netplay_session.cpp` (with `net_lobby.cpp`, `netplay.cpp`, `coop_guest.cpp`):

1. **Joinable from boot.** A game whose settings have `net_joinable=on` when it starts (read from the file: the
   settings load later) is deterministic from boot and runs as a session host on its own: its controller is the
   first player's input at once (no delay), the second player's input is neutral, and Luigi sits out (co-op's
   drop-in state) whatever the Multiplayer switch says. Turning Joinable on later keeps the old restart-based
   join (both PCs restart into a session at the host's place), since that game was not deterministic from boot.
2. **Accept.** The lobby closes its listening socket and the session opens on the same UDP port. At the next frame
   start the host captures the snapshot (`hot_join_capture`), builds the save bundle as of that frame F and plays
   on. A worker thread compresses the image (Windows Compression API, XPRESS with Huffman). The stream is
   `[magic, F, compressed size][compressed image][a record per frame from F on]`, a record being the host's input
   as that frame used it, with its EFB peek answers.
3. **The joiner** restarts once into `GALAXY_NET_JOIN` + `GALAXY_NET_HOTJOIN=1`: it fetches the bundle at startup
   (the existing path), boots deterministic to its own frame 30, receives the stream (chunks of 1200 bytes asked for
   from the first missing one with a window that doubles while it fills and halves when it stalls; the host resends
   from there after three unmoved asks), decompresses, loads (`hot_join_load_image`) and wakes up in the host's
   capture as frame F. It replays the records unpaced and unrendered (`hot_join_catch_up`), and when it is within
   two frames of the newest record it renders and paces again and sends Ready.
4. **The join frame.** On Ready the host names J = its frame + 60, which every later record carries. After its read
   of J - 1 it makes the inputs of J .. J + delay - 1 (J - 1's held, J's carrying `NetCommand::Join`); from J on its
   reads go `delay` frames ahead and the joiner's own inputs apply from J + delay. Both PCs carry out Join at the
   same frame: co-op's drop-in puts Luigi beside Mario (`coop_request_drop_in`). From there it is the ordinary
   lockstep session: inputs, hash checks, the online menu.
5. **A joiner that goes away before J** (no ask for 15 s): the host closes the session and plays on alone; the
   lobby listens again. After J a silent peer ends the session as before.

The EFB peeks of a frame come before the frame starts (the game draws frame f - 1 after both reads have moved the
frame counter to f), which is why the join frame's inputs exist right after the host's read of J - 1.

Tested with `tools/agent/live_join.py` (two hidden copies on one PC, the host mid-galaxy in Good Egg, the joiner
asking with `GALAXY_NET_AUTOJOIN`, the host accepting with `GALAXY_NET_AUTOACCEPT`):

| run | result |
|---|---|
| join, 1,800 frames on | MEM1 identical at all 62 samples (every 30 frames) from J; 60 fps on both |
| `--exit-after 600`: Exit Galaxy after the join | both to the Observatory at the same frame; identical through it |
| `--star-after 300`: a Power Star appears (its cutscene) after the join | cutscene on both; 52 samples identical |
| both, and `--join-env GALAXY_TEST_MODULE_ELSEWHERE=1` (the joiner's game module at another address, as on another PC) | 5,835 stack words relocated; 61 samples identical; lowest 60.0 fps on both |

Times in those runs: the capture 54 ms (100 ms once), compressing 101.9 MB to 40.8 MB 270 ms on the worker, the
joiner booting and fetching the bundle about 10 s, the snapshot over localhost 0.9 s, unpacking 147 ms, loading
23 ms, catching up ~250 frames, and Luigi in 16 s after the joiner started. Deterministic mode itself costs solo
play no frame rate (60 fps either way) and about 20-35% more CPU per frame (16.3 vs 11.7-14.0 ms, all threads).

## Why it is not just "send the rollback snapshot"

A rollback snapshot (`RollbackSnapshot` in `runtime/src/native_runtime.cpp`) is guest RAM and device state plus
every guest thread's **native stack**: the game is statically recompiled, so a guest thread parked in
`OSSleepThread` has a real chain of x64 frames (`fiber_entry → run_guest_flat → call_guest_impl → generated code →
… → Scheduler::select_thread → switch_to`). Those frames hold host addresses. `tools/agent/snapshot_scan.py` reads a
`GALAXY_SNAPSHOT_SCAN=<file>` dump (written when a save captures, with every fiber parked: each stack, its unwound
frames and saved-register slots, the module list, the VirtualQuery map, every busy heap block) and classifies every
8-byte word of every stack.

Observatory gameplay, frame 6000 of `det-rec1` (13 guest threads, 8–49 KB of live stack each):

| what the words point at | words | in saved-register slots |
|---|---:|---:|
| the runtime's objects on the simulation thread's own stack (RuntimeState, PpcContext, GuestAddressSpace, NativeServicesV1, GuestMemoryV1) | 4416 | 415 |
| the fiber stacks themselves (frame pointers, addresses of locals) | 2659 | 143 |
| code: NebulaRuntime.exe 1672, RMGE01_game.dll 636 | 2308 | 148 |
| code in system DLLs (ntdll, MSVCP140, ucrtbase, KERNELBASE) | 182 | 0 |
| module data (statics in the exe and game module) | 358 | 69 |
| heap: the simulation thread's thread-local block 233, Scheduler / DeadlineBroker / timeline, map nodes, … | 405 | 3 |
| other private memory (large heap blocks, GPU upload heaps: mostly packed 32-bit pairs) | 425 | 13 |
| guest memory (the fixed flat view at 0x1000_0000_0000, host views of MEM1/MEM2) | 297 | 56 |

The unwind shows the system-DLL code words all sit inside `call_guest_impl_body`'s 13 KB local buffer: stale bytes
from earlier calls, never read. Everything else that is live points at one of a handful of things a second process
also has, only elsewhere.

## The approach: pin what is cheap to pin, relocate the rest by name

1. **Fiber stacks at fixed addresses.** `coop::Scheduler` puts stack slot *i* at `0x2000_0000_0000 + i × 512 MiB`
   (`add_stack`). Pointers between and within stacks, and the TEB stack bounds saved in each context frame, are then
   valid in any process. `adopt_stacks` creates any slot the snapshot uses that this process has not.
2. **The snapshot's object graph in a fixed arena.** While a `hot_join::ArenaScope` is open, the thread's
   `operator new` allocates from a bump arena reserved at `0x3000_0000_0000` (`runtime/src/hot_join.cpp` replaces
   the global allocation functions; other threads and other times use the CRT heap as before). The writer
   constructs a fresh `RollbackSnapshot` there and saves into it, so every vector, map, `shared_ptr` and nested state
   it owns lies in the arena. The file is the arena's bytes: no per-type serializer, and every pointer inside the
   snapshot is valid wherever the arena is mapped at the same address. The one thing a snapshot shared with live
   objects (open NAND files' `shared_ptr<NandBacking>`) is copied into the arena first
   (`GuestAddressSpace::own_device_snapshot`). `hot_join::audit_arena` then checks every arena word (guest RAM copies
   and stacks aside) for addresses of this process's own private or mapped memory; at frame 6000 the only hits are
   the three pointers the loader overwrites and padding bytes.
3. **Everything else by name.** The file carries a table of host ranges: each module image, the simulation thread's
   stack allocation and thread-local blocks (one per module with TLS), and named runtime objects (the five on the
   simulation thread's stack, the Scheduler, DeadlineBroker, RuntimeTimeline and the VI deadline consumer). The
   loader builds the same table for itself and moves every stack word that falls in a range to the same offset in its
   own range (objects first: they are not at the same depth of the simulation thread's stack in two processes). About
   4,900 words move.

Pinning everything instead (fixed image bases, a fixed-address stack for the simulation thread, a fixed arena for
the runtime's heap objects) would remove the relocation pass but not the heap-object list, and the thread-local
blocks cannot be pinned at all (the loader allocates them). Rebasing by scanning alone, without names, cannot tell a
packed pair of 32-bit values from a pointer; naming the targets makes a wrong move both unlikely (it needs a value
inside one of ~120 small ranges) and harmless when it hits a stale word.

### Resuming

`GALAXY_SNAPSHOT_WRITE=<frame>,<file>` writes at the start of that frame (in `netplay_kpad_before`, the frame
boundary rollback uses) and the writer plays on. `GALAXY_SNAPSHOT_LOAD=<frame>,<file>` loads when the second
process's own frame count reaches that frame (it boots normally to, say, the title screen first, so every device,
the renderer and the audio thread exist): map the arena, point the snapshot at this process's RuntimeState,
register file and memory, relocate the stacks, adopt the stack slots, then the ordinary `rollback_restore`. The
fiber that wrote the file wakes up **inside the writer's `hot_join_write` call**, finishes the restore half of
`rollback_save` (GX parse state, co-op, Odyssey, voice, guest RAM, runtime fields, devices, cache invalidation) and
returns to netplay, which takes the writer's frame number. From there the second process is the first one.

## Results

`py -3 tools/agent/determinism.py join --replay <rec> --save N --load-at K --frames M` runs it: copy A replays the
recording and writes at frame N; copy B boots, loads at its own frame K, and both hash MEM1 every frame from N.

Recording `det-rec1` (into file 1 by the menus, then both brothers run and jump around the Observatory):

| written at (A) | loaded at B's own frame | compared | result |
|---|---|---|---|
| 6000 (Observatory) | 300 (title screen) | 6001–9000 | identical every frame |
| 6000, same file loaded into two copies at once | 300 | 6001–9000 | both identical to A and to each other |
| 2500 | 120 | 2501–4500 | identical every frame |

What it took to get there, in order (each found with `GALAXY_HOT_JOIN_TRACE=1`, the arena audit or the
deterministic trace):

1. An open NAND file's backing was shared with the live device, so the snapshot pointed into the writer's heap:
   copied into the arena.
2. The runtime's objects on the simulation thread's stack are not at the same depth in two processes (relocating
   the whole stack by its base was wrong): each became a named range.
3. A NAND write-back checks the host file's NTFS identity recorded when the guest opened it. In the second process
   it is another file, the write failed with `kIsfsErrAccess`, and 1,073 frames later the game closed instead of
   seeking (the first difference in the trace: request `135e01a0`, seek in A, close in B). The loader now rebinds open
   NAND files to its own files at the same guest paths (`GuestAddressSpace::adopt_device_snapshot`).

Note for anyone repeating this: two straight replays of `det-rec1` on this build also part now and then (at frames
2700, 2820 in two bisect runs, re-replaying the window identically), so a single long comparison can fail for that
reason alone; loading one file into two copies separates the two causes.

## Costs

- **Size**: 102 MB raw (guest RAM is 92 MB of it: MEM1 24, MEM2 64, the rest small; 58% zero bytes). Compressed:
  XPRESS-Huffman (what live join uses, built into Windows) 41 MB in 0.27 s, unpacked in 0.15 s; zlib level 1 36 MB
  (0.4 s), level 6 30 MB (1.2 s), lzma preset 1 26 MB (7.6 s). Most of MEM2 is disc data the joiner's own boot loads too:
  sending only the 4 KB pages whose hash differs from the joiner's would cut this a lot (not measured).
- **Time on the host**: the save into the arena 30–40 ms (a 92 MB copy plus the rest), the audit and the file write
  60–100 ms more, on the simulation thread: one hitch of about 5–8 frames, or less with the write moved to another
  thread (the arena is not touched again until the next write).
- **Transfer**: 30 MB is ~2.5 s at 100 Mbit/s upload, ~5 s at 50, ~25 s at 10. The host keeps playing meanwhile and
  keeps its inputs; the joiner replays them unrendered (re-simulation runs 2–4× real time) and joins when caught up.
- **Restore on the joiner**: reading 102 MB 26–30 ms from the OS cache, relocation and stack adoption ~3 ms, the
  restore itself ~6 ms.

## Across two PCs

The prototype ran both processes on one PC from the same files. Between two PCs:

- **Identical binaries** (NebulaRuntime.exe and RMGE01_game.dll): already required by a netplay session (the build
  identity check); a game module compiled on each PC would not do. The joiner should receive the host's module, or
  both install the same build.
- **Image bases**: code addresses in the stacks are relocated by module, and so are the arena's own words outside
  the guest RAM copies and the stacks (`relocate_arena`: the vtables of `shared_ptr` control blocks), so no fixed
  bases are needed. Tested by loading the joiner's game module at another address
  (`GALAXY_TEST_MODULE_ELSEWHERE=1`); the exe cannot be moved that way on one PC, and its words go through the same
  code. System DLLs differ between Windows builds: only stale stack words point there.
- **CPU and OS**: the same module runs the same instructions; MXCSR is saved per fiber. Deterministic mode already
  keeps host time out of the guest.
- **Saves**: the snapshot carries open NAND files' contents and rebinds them to the joiner's files; the save
  directory itself still travels as it does today, and must be the host's as of the snapshot.

## Open points before a real feature

- The arena is single-shot: a second write resets it while objects a save left there (a device container that grew
  during the save) might still be in use. A production writer should save into a fresh RollbackSnapshot and then
  copy it into the arena, or use a dedicated allocator for snapshot types only.
- Tested: snapshots in Observatory and Good Egg gameplay, a stage change and a Power Star cutscene after a join.
  Not yet: a join during a level load, a movie (THP), a DVD read in flight or the home-button menu; over a real
  internet link (the stream's window was only tuned on localhost).
- Heap memory a resumed stack frame owns is the host's: the joiner would free a pointer it never allocated if such
  a frame ended. None did in these runs (the capture path holds none across the save); keep it that way.
- `operator new` checks a thread-local depth on every allocation: 0.1-0.2 ns on a 19.5 ns malloc/free pair (a
  global flag measured 0.5 ns), so it stays.

## Files

- `runtime/include/galaxy/hot_join.h`, `runtime/src/hot_join.cpp`: arena, allocation functions, host ranges,
  relocation, file format, audit, resume trace (`GALAXY_HOT_JOIN_TRACE=1`).
- `runtime/src/coop_scheduler.cpp`: the fixed stack pool, `adopt_stacks`, the `captured` hook.
- `runtime/src/native_runtime.cpp`: `hot_join_capture` / `hot_join_load_image` (and the file forms
  `hot_join_write` / `hot_join_load`), `hot_join_catch_up`, `rollback_write_scan`.
- `runtime/src/netplay.cpp`: the capture and load points at a frame's start; `GALAXY_SNAPSHOT_WRITE` /
  `GALAXY_SNAPSHOT_LOAD`.
- `runtime/src/netplay_session.cpp`: live join (phases, stream, join frame); `net_lobby.cpp`: the live accept.
- `tools/agent/live_join.py`, `tools/agent/snapshot_scan.py`, `tools/agent/determinism.py join`.
