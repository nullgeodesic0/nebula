# Agent play testing

A test driver can run a copy of the game, jump straight into any galaxy mission, give either player power-ups,
move them, press buttons, take screenshots and read crashes, so an agent can load levels and try things without
anyone at the controls. It was built for developing the Mario and Luigi co-op mode (docs/MULTIPLAYER.md).

## Pieces

| Piece | What it does |
|---|---|
| `runtime/include/galaxy/agent.h`, `runtime/src/agent_guest.cpp` | The game's command channel, on with `GALAXY_AGENT_DIR` |
| `tools/agent/nebula_agent.py` | Launches and drives a test copy (Python library and command line) |
| `tools/agent/nebula_mcp.py` | The same as MCP tools, registered in `.mcp.json` as `nebula-playtest` |

## Test copies

Each test copy lives in `C:\nb\agent-mp\<name>` (`--instance`, default `default`) with its own copy of the saves,
settings and shader cache, taken from the run folder (`C:\nb\run-mp`, or `--run` for another build such as
`C:\nb\run-mp-dev`) the first time. It never writes to the real saves or the run folder's saves. It runs with no
window at all (`GALAXY_HIDE_WINDOW`; `--show` opens one), audio off and controllers only for a focused window,
so a game played at the same time is left alone; screenshots still see it. Stopping a test copy ends only the
process it started.

The run folder's `NebulaRuntime.exe` and RMGE01 modules are the build under test; the content folder is
`C:\nb\content-mp`.

## Command line

```
py -3 tools/agent/nebula_agent.py launch            # --solo for co-op off, --audio, --fresh-saves
py -3 tools/agent/nebula_agent.py boot              # opens save file 1 (--file N) with no menus, about 15 s
py -3 tools/agent/nebula_agent.py goto "space junk" 1
py -3 tools/agent/nebula_agent.py cmd powerup 2 fire
py -3 tools/agent/nebula_agent.py shot out.png
py -3 tools/agent/nebula_agent.py crash
py -3 tools/agent/nebula_agent.py stop
```

`goto` takes the game's stage name (`StarDustGalaxy`) or a common name listed in `nebula_agent.GALAXIES`, and
waits until no cutscene or event camera runs and the first player has control.

## MCP tools

`game_launch` (boots by default), `game_stop`, `game_status`, `game_goto`, `game_screenshot` (an image),
`game_powerup`, `game_teleport`, `game_input`, `game_command`, `game_log`, `game_crash` and `level_smoke_test`
(load a mission, screenshot, give both players each listed power-up, walk them about, report any crash).
Claude Code offers the server from `.mcp.json` when a session starts in this folder.

## The command channel

With `GALAXY_AGENT_DIR` set, the game reads `commands.txt` in that folder (the driver appends
`<id> <verb> <arguments>` lines) and answers each command in `replies.jsonl` (`{"id":..,"ok":..,"frame":..}`),
once a frame on the game thread at the first player's controller read. `status.json` is rewritten twice a second
with the frame, scene, stage, mission and, in a stage, `demo`, `event_camera`, `intro` (the mission's opening
camera), `dead`, the free space of the game's heaps (`heap_free_kb`: stationed, game and stage, MEM1 and MEM2)
and each player's position, power-up, health, lives, `down`/`out` (co-op lives) and whether their input is held.

| Command | Effect |
|---|---|
| `ping`, `state` | Answered at once; `state` as status.json |
| `file <1-6> [luigi]` | From the logo or title, reads the saves and starts that file as the file select would (`FileSelector::exeDemo`), no menus; answered when its first stage is requested |
| `stage <StageName> <mission>` | Into that galaxy mission (`MR::requestChangeStageInGameMoving`); needs a stage, not the title |
| `wait <frames>` | Answered after that many frames; later commands wait for it |
| `press <1\|2> <A+B..> <frames> [nowait]` | Buttons held, then released (A B 1 2 PLUS MINUS UP DOWN LEFT RIGHT C Z HOME; SHAKE spins) |
| `stick <1\|2> <x> <y> <frames> [nowait]` | The Nunchuk stick, -1..1, y up |
| `pointer [1\|2] <x> <y>`, `pointer [1\|2] off` | A player's Star Pointer (the second's is the orange co-op one), -1..1 across the screen, y down |
| `release <1\|2>` | The controller back to the person playing |
| `powerup <1\|2> <normal\|rainbow\|fire\|ice\|bee\|spring\|boo\|fly> [force]` | Refused when the form's model is not loaded in the stage, unless `force` |
| `teleport <1\|2> <x> <y> <z>` | `MR::setPlayerPos` (the second player's in their own turn) |
| `kill <1\|2>` | Health to 0 and down (`MarioActor::forceGameOver`), as a hit would |
| `actors <name part \| .> [max]` | Live actors (`MR::getAllLiveActorGroup`) whose name contains it: name (Shift-JIS shows as `?`), address, vtable (the class: see Petari's symbols), position, dead |
| `peek <address> [words]`, `poke <address> <word>` | Guest memory (development) |
| `call <address> [r3 r4 r5 r6]` | A game function on the game thread; its r3 back (development) |
| `screenshot <path.ppm>` | The next presented frame |
| `quit` | The game exits |

A power-up given by command first marks the form as seen in the test copy's save
(`MR::onGameEventFlag*MarioAtFirst`): the first-time demo turns the player towards the item they touched, and
without an item it crashed.

## Level sweep

`py -3 tools/agent/sweep.py` loads every mission of every galaxy in a hidden test copy (co-op; `--solo` for one
player), walks the second player away and back, and writes `C:\nb\agent-mp\sweep\results.json` and
`report.md` with a screenshot per mission (and of the screen when one never becomes playable). A crash or hang
relaunches and carries on; a rerun resumes. `--retry <results.json>` runs only the missions that failed there,
for example in solo to tell co-op bugs from missions the direct jump cannot start (hidden stars and comets need
their own entrance or save state).

## Cutscene sweep

`py -3 tools/agent/cutscene_sweep.py` plays each mission's Power Star cutscenes in a hidden test copy: it makes one
of the stage's Power Stars appear (`PowerStar::requestAppear`, the appear cutscene) unless one is out, has a
brother take it (alternately the second and the first player in co-op), and presses A through the star-get
scene, the dome's result and the save prompt (pointing at Yes) until the Observatory has been calm for 3 s.
Results are ok, no-star, not-taken (the star was not reached), stuck or crash, in `C:\nb\agent-mp\cutscenes` (`--out`), with screenshots. Leaving the dome while it plays story scenes one after another (the
butler's report) frees what the story sequence still holds, so the sweep waits for calm before the next jump.

## Determinism probe

Rollback netplay needs the game to be deterministic: the same inputs from the same start give the same guest
memory. `GALAXY_DETERMINISTIC=1` runs it that way (a virtual clock from executed checkpoints, deadlines, disc and
NAND completions and PE events all on the simulation thread, the native DSP run in lockstep with it). On top of
it `py -3 tools/agent/determinism.py` checks:

- `record --scenario observatory --frames 9000` records a run's controller data (`input.rec` in `--out`): into
  file 1 through the menus by input alone, then both brothers run and jump. `--scenario opening` presses A
  through the title instead.
- `bisect --replay <file.rec> --runs 6` replays it in six hidden copies at once and compares MEM1 hashes every
  `--every` frames. At the first disagreement it replays the window with a hash every frame, dumps MEM1 at the
  first differing frame in two copies and lists the differing words (`--symbols` names them from a decomp symbol
  map). `--trace` also logs each copy's clock jumps, delivered interrupts and IPC replies
  (`GALAXY_DETERMINISTIC_TRACE`) and shows where two diverging copies' traces part.

Replays run unpaced (`GALAXY_DETERMINISTIC_UNPACED=1`: no waiting for real time, no presents, no audio): six
copies replay 2400 frames in about ten seconds. `--paced` replays at real-time speed. Use a run folder of its own
(`--run`), since the copies keep the executable open.

## Found with it

- Space Junk mission 1 in co-op: fixed (the second player's view unclipped the arena's web coins).
- Honeyhive mission 1 in co-op: the stage's MEM1 heap ran out (solo leaves it 1.2 MB; the second player takes
  about 600 KB and co-op's other additions the rest). Fixed by letting a full MEM1 stage heap borrow from the
  MEM2 one (`[heap]` lines in the log).
- A power-up given without its item crashed in the first-time demo (handled by the `powerup` command).
