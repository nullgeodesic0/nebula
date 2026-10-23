# Hot join: moving a running game to another process

Question: can a running Nebula game send its whole deterministic state to another PC, so a joiner "pops in" next
to the host without the host restarting? Answer: **yes**. A rollback snapshot can be written to a file in one
process and resumed in a second process, which then plays on bit-identically (MEM1 hash equal every frame). The
prototype does this between two processes on one PC; the parts that differ between two PCs are listed under
[Across two PCs](#across-two-pcs), and none of them is a wall.

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
  zlib level 1 36 MB (0.4 s), level 6 30 MB (1.2 s), lzma preset 1 26 MB (7.6 s); zstd was not available to measure
  and should land near zlib-6's size at zlib-1's speed. Most of MEM2 is disc data the joiner's own boot loads too:
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
- **Image bases**: code addresses in the stacks are already relocated by module. The arena holds a few too (the
  vtables of `shared_ptr` control blocks); with ASLR the exe's base can differ between PCs. Either link the exe and
  game module with fixed bases (`/DYNAMICBASE:NO`) or relocate image words in the arena's non-RAM bytes as the
  stacks are. System DLLs differ between Windows builds: only stale stack words point there today, and the audit
  should be extended to flag any arena word that does.
- **CPU and OS**: the same module runs the same instructions; MXCSR is saved per fiber. Deterministic mode already
  keeps host time out of the guest.
- **Saves**: the snapshot carries open NAND files' contents and rebinds them to the joiner's files; the save
  directory itself still travels as it does today, and must be the host's as of the snapshot.

## Open points before a real feature

- The arena is single-shot: a second write resets it while objects a save left there (a device container that grew
  during the save) might still be in use. A production writer should save into a fresh RollbackSnapshot and then
  copy it into the arena, or use a dedicated allocator for snapshot types only.
- Two writer points were tested (frame 2500 and Observatory gameplay at 6000), 5,000 frames compared in all. Level loads, cutscenes,
  movies (THP), a DVD read in flight and the home-button menu need runs of their own.
- Joining as the second brother: the snapshot gives the joiner the host's exact game; spawning Luigi has to be a
  deterministic event both PCs apply at the same frame (a join input), using co-op's existing respawn-beside-partner.
- The host must already run deterministic mode (and keep its inputs) for anyone to join, so "joinable" means
  deterministic from boot.
- `operator new` now checks a thread-local depth on every allocation; not measurable in these runs, but it should
  be timed in a sim profile.

## Files

- `runtime/include/galaxy/hot_join.h`, `runtime/src/hot_join.cpp`: arena, allocation functions, host ranges,
  relocation, file format, audit, resume trace (`GALAXY_HOT_JOIN_TRACE=1`).
- `runtime/src/coop_scheduler.cpp`: the fixed stack pool, `adopt_stacks`, the `captured` hook.
- `runtime/src/native_runtime.cpp`: `hot_join_write` / `hot_join_load`, `rollback_write_scan`.
- `runtime/src/netplay.cpp`: `GALAXY_SNAPSHOT_WRITE` / `GALAXY_SNAPSHOT_LOAD`.
- `tools/agent/snapshot_scan.py`, `tools/agent/determinism.py join`.
