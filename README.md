# Nebula

Nebula runs **Super Mario Galaxy** natively on Windows. It translates the game's PowerPC
machine code into C++ ahead of time, compiles that code on your PC from your own copy of the
game, and runs it on a native runtime with a Direct3D 12 renderer and Windows audio and input.
Nebula contains no Nintendo code or assets; you need your own copy of the game.

[Download](https://github.com/keepvaibin/nebula/releases/latest) ·
[Controls](docs/KEYBOARD_CONTROLS.md) ·
[Building from source](docs/BUILDING.md) ·
[Architecture](docs/ARCHITECTURE.md)

Not affiliated with or endorsed by Nintendo.

## Setting up multiplayer (Nebula Plus)

This fork's **Nebula Plus** adds Mario and Luigi co-op: on one PC with split screen, and online between two PCs.

**1. Install.** You need Windows 10 or 11 (64-bit), a graphics card with Direct3D 12, about 16 GB free and **your
own dump of the Super Mario Galaxy USA disc** (RMGE01, the original release) as ISO, WBFS, RVZ or an extracted
folder. Nothing from Nintendo is included or downloaded.

- Download `NebulaPlus.exe` from the [latest release](https://github.com/nullgeodesic0/nebula/releases/latest) and
  run it (if Windows SmartScreen warns about an unknown app: More info > Run anyway).
- Choose a folder (default `C:\NebulaPlus`) and your disc image and press **Install**. Nebula does not need to be
  installed first. The first install downloads the compiler toolchain (about 400 MB; accept the Microsoft C++ Build
  Tools license when asked) and builds the game from your disc: a few minutes on a fast PC, 10-20 on an older one.

**2. The multiplayer build.** Nebula Plus *is* the multiplayer build: co-op and online play are always installed, and
one player is the default. The install folder has `Play Nebula Plus.cmd` (the game), `Host online game.cmd`,
`Join online game.cmd`, `Check for updates.cmd` and `Nebula Plus.cmd` (the Nebula Plus window).

**3. Local co-op (one PC).** Start `Play Nebula Plus.cmd` and load a save. Plug in a second controller and press its
**Start** (+ on the Wii layout, Menu on Xbox, Options on DualSense) to drop in as Luigi; the screen splits when the
brothers part. Hold − for a second to drop out. To choose who plays with which device (keyboard and mouse or a
controller; Mario, Luigi or off), open the pause menu: the **Players** panel beside it moves each device between the
seats. On the save select, **Options > Two players** starts every level with both brothers.

**4. Online, hosting.**
- Run `Host online game.cmd` (or in the game: F1 > ONLINE > JOINABLE: ON, which lets a friend drop in while you play).
- On your router, forward **UDP port 7777** to this PC, and allow `NebulaRuntime.exe` through Windows Firewall
  (Windows asks the first time: allow it on private and public networks, or add a rule for UDP 7777).
- Give your friend your **public IP address** (search "what is my IP" in a browser). On the same home network, use
  this PC's local address instead.

**5. Online, joining.** Run `Join online game.cmd` and type the host's address (`1.2.3.4`, or `1.2.3.4:port` if they
use another port), or in the game: F1 > ONLINE, the Join row, type the address and press Enter. You play Luigi in the
host's save; your own saves are not touched.

**6. Same version, same mods.** Both PCs need the same Nebula Plus version and the same mods. Turn **CUSTOM MODEL**
and **CUSTOM ANIMATIONS** (F1 > GENERAL) off unless both players built the identical packs, or the two games will
drift apart. **CUSTOM VOICE** is only sound and is safe to leave on.

**7. Updating.** `Check for updates.cmd` installs the newest release and keeps your saves and settings; the previous
version stays for rollback. Update both PCs: an older and a newer version cannot play together.

**8. Troubleshooting.**
- The friend cannot connect: check the router forwards **UDP** 7777 (not TCP) to the host's current local address,
  that Windows Firewall allows `NebulaRuntime.exe`, and that the joiner uses the host's public address (or local
  address on the same network).
- Wrong port: if the host starts `Host online game.cmd 7800`, the joiner types `address:7800`, and that port is the
  one forwarded.
- "Different build" or the join is refused: both PCs must run the same Nebula Plus version (both `Check for
  updates.cmd`) with the same mods on.
- The two games drift apart: turn off CUSTOM MODEL and CUSTOM ANIMATIONS on both, or build identical packs.
- Logs for a bug report: `data\logs` in the Nebula Plus folder.

## Project status

Nebula is a work in progress. The game is playable from the start, but some areas run below
60 FPS on weaker hardware, some lighting and bloom effects differ from the original, and bugs
remain. See [Known issues](#known-issues).

## AI-generated project

Nebula was built with AI coding agents (Anthropic's Claude). The recompiler, runtime,
installer, tests and documentation were written by AI agents and directed, reviewed and tested
by the project's author. The game's own logic, including Mario's physics, is not AI-written:
it is the original game code, translated instruction by instruction by the recompiler.

## How it works

1. **Recompiler** (`crates/`, Rust). Reads the game disc or extracted files, checks that it
   is the supported release (RMGE01, revision 0) and emits C++ for every function in the game
   executable, the HOME Menu module and the audio DSP program.
2. **Runtime** (`runtime/`, C++). Loads the compiled game module and provides what the Wii
   hardware provided: memory, a GX command processor drawn with Direct3D 12, the audio
   interface and DSP, Wii Remote input from keyboard, mouse and controllers, disc and NAND
   storage, and video timing.
3. **Installer and launcher** (`installer/`, C#). `Nebula-Setup.exe` downloads a pinned
   compiler toolchain, compiles the game modules from your copy of the game and installs the
   prebuilt runtime. `Nebula.exe` starts the game; `NebulaSettings.exe` changes settings.

## Features

- Keyboard and mouse, Xbox controllers, the Switch Pro Controller, the Switch 2 Pro
  Controller (USB) and the PlayStation DualSense (USB or Bluetooth), remappable. Switch and
  DualSense controllers point with the gyro (R/R1 recenters) and tilt for the Star Ball and
  ray surfing.
  Vibration follows the game's Wii Remote motor.
- Aspect ratios from 4:3 to 32:9 and internal resolution up to 16x.
- Anti-aliasing (SMAA, FXAA), AMD FidelityFX FSR 1 upscaling, anisotropic filtering and
  optional per-pixel lighting.
- In-game settings menu (F1, View + Menu on Xbox, Minus + Plus on Switch).
- Text speed: dialogue and Rosalina's storybook at the original pace, 2x or 3x.
- Motion style: Original; Smooth, which eases every character's animation blends and Mario's lean
  into turns without changing play; or Modern, which also makes Mario quicker to start, stop and turn
  at a run. The spin, jumps and top speed stay the game's in every style.
- The game in English, French or Spanish, the three languages of the USA release.
- Saves stored in `%LOCALAPPDATA%\Nebula\saves`, with export, import and Dolphin saves
  (see [Saves](#saves)).

## Requirements

- Super Mario Galaxy, USA (RMGE01), revision 0, as an ISO, RVZ or extracted folder
- Windows 10 (version 1803 or newer) or Windows 11, 64-bit
- A GPU with Direct3D 12 support
- 8 GB of RAM (Setup also installs on less, as an experimental install at your own risk);
  16 GB of free disk space during installation, about 5.5 GB afterwards
- An internet connection for the first installation

The least powerful system tested so far, an Intel Core Ultra 9 185H with its integrated
graphics and 16 GB of RAM, runs the game at a locked 60 FPS at 1440p (16:10).

## Install

1. Download `Nebula-Setup.exe` from the [latest release](https://github.com/keepvaibin/nebula/releases/latest).
   The file is not code-signed, so Windows SmartScreen shows "Unknown publisher"; choose
   **More info**, then **Run anyway**.
2. Select your game and choose **Install**. Setup downloads the toolchain and compiles the game
   modules; this takes a few minutes on a fast PC and longer on slower ones.
3. Start **Nebula** from the Start menu.

Setup also updates, repairs, restores the previous version and uninstalls. Updates are signed;
Setup checks the signature against the public key built into it before installing.

## Saves

Saves are kept in `%LOCALAPPDATA%\Nebula\saves`, laid out like the Wii's internal memory: the
game's save is `title\00010000\524d4745\data\GameData.bin`, next to its `banner.bin`. Each file and
folder also carries the Wii's file information in a hidden NTFS stream, which a plain copy or zip
drops, so move saves with the launcher's **Advanced** tab:

- **Export saves…** writes `Nebula-Saves-<date>.zip` with everything the game reads.
- **Import saves…** takes such a zip, another zip or folder of Nebula saves, or a save from Dolphin.
  Your current saves are moved to `saves-backup-<date>` beside them first. Close the game before
  importing or exporting.

To bring a save over from Dolphin:

1. In Dolphin, right-click Super Mario Galaxy and choose **Open Wii Save Folder**. It holds
   `GameData.bin` and `banner.bin` (in Dolphin's user folder,
   `Wii\title\00010000\524d4745\data`).
2. In Nebula, choose **Advanced**, **Import saves…**, **From a folder…**, and select that folder.

Only saves of the USA version (RMGE01) apply; the game itself can come from an ISO or an RVZ, as
Nebula and Dolphin use the same save format. A `GameData.bin` copied into the saves folder by hand
also works: at the next start Nebula rebuilds the folder around it, and keeps the previous folder
as `saves-rejected-<date>` if anything in it could not be carried over.

## Building from source

[docs/BUILDING.md](docs/BUILDING.md) covers the tools, building the recompiler, runtime and game
modules by hand, running the tests and building the installer.

## Testing

- Recompiler: more than 500 tests (`cargo test --workspace`).
- Runtime: 72 test programs (`ctest`) covering floating-point operations (checked against
  Berkeley SoftFloat), GX command parsing and texture decoding, input mapping, the audio queue
  and DSP, and settings files.
- `tools/dev/verify_thp_native.cpp` compares the native movie decoder with the game's own
  decoder frame by frame.
- Gameplay is checked by playing back recorded input in a hidden window and by manual
  playtesting.

There is no automated whole-game comparison against the original hardware or an emulator yet.
Floating-point results are close to, but not bit-exact with, the Wii.

## Known issues

- Heavy background CPU load can make the audio skip.
- Some areas, for example the Gateway Galaxy, drop below 60 FPS on weaker CPUs.
- Some lighting and bloom effects in the Comet Observatory differ from the original.

Report problems on the [issue tracker](https://github.com/keepvaibin/nebula/issues). If Nebula
closed unexpectedly, attach its crash log: in Nebula Settings choose **Advanced**, **Open
diagnostics**, then the newest file in `crash-logs`. It is a few lines of text about what failed;
for more detail, choose **Detailed recording** under **Recording (next launch)** and reproduce the
problem.

## License

Nebula is licensed under the GNU General Public License v3.0 only ([LICENSE](LICENSE)).
Third-party code and its licenses are listed in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)
and [docs/PROVENANCE.md](docs/PROVENANCE.md).

Nebula builds on work from Aurora, Dusklight, WiiCompiled, the Petari decompilation project,
Berkeley SoftFloat, the Dolphin project's free DSP ROM, SMAA, AMD FidelityFX and the `nod` disc
library.

"Super Mario Galaxy", "Wii" and "Nintendo" are trademarks of Nintendo.
