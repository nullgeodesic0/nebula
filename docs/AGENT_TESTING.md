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
settings and shader cache, taken from the run folder (`C:\nb\run-mp`) the first time. It never writes to the real
saves or the run folder's saves. The window opens without taking the focus, audio is off, and a controller only
reaches the test copy while its window has the focus, so a game played at the same time is left alone. Stopping
a test copy ends only the process it started.

The run folder's `NebulaRuntime.exe` and RMGE01 modules are the build under test; the content folder is
`C:\nb\content-mp`.

## Command line

```
py -3 tools/agent/nebula_agent.py launch            # --solo for co-op off, --audio, --fresh-saves
py -3 tools/agent/nebula_agent.py boot              # title and file 1 until the player can move
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
with the frame, scene, stage, mission and, in a stage, `demo`, `event_camera`, `dead` and each player's position,
power-up, health and whether their input is held.

| Command | Effect |
|---|---|
| `ping`, `state` | Answered at once; `state` as status.json |
| `stage <StageName> <mission>` | Into that galaxy mission (`MR::requestChangeStageInGameMoving`); needs a stage, not the title |
| `wait <frames>` | Answered after that many frames; later commands wait for it |
| `press <1\|2> <A+B..> <frames> [nowait]` | Buttons held, then released (A B 1 2 PLUS MINUS UP DOWN LEFT RIGHT C Z HOME) |
| `stick <1\|2> <x> <y> <frames> [nowait]` | The Nunchuk stick, -1..1, y up |
| `pointer <x> <y>`, `pointer off` | The first player's pointer, -1..1 across the screen, y down |
| `release <1\|2>` | The controller back to the person playing |
| `powerup <1\|2> <normal\|rainbow\|fire\|ice\|bee\|spring\|boo\|fly> [force]` | Refused when the form's model is not loaded in the stage, unless `force` |
| `teleport <1\|2> <x> <y> <z>` | `MR::setPlayerPos` (the second player's in their own turn) |
| `screenshot <path.ppm>` | The next presented frame |
| `quit` | The game exits |

A power-up given by command first marks the form as seen in the test copy's save
(`MR::onGameEventFlag*MarioAtFirst`): the first-time demo turns the player towards the item they touched, and
without an item it crashed.

## Found with it

- Space Junk mission 1 in co-op: fixed (the second player's view unclipped the arena's web coins).
- Honeyhive mission 1 in co-op, loaded with `goto`: a heap allocation fails while the stage is built (solo loads).
