# Nebula Plus

Nebula Plus is a separate Nebula install with this fork's additions: Mario and Luigi co-op (local split screen,
drop-in/drop-out, online netplay with live join and rollback), the Odyssey moveset, the Odyssey Mario and Luigi
models, animations and voice, the graphics and controller additions, and the fixes that came with them. It sits
in its own folder next to the official Nebula, never changes the official install, and keeps its own saves and
settings unless you choose otherwise.

`NebulaPlus.exe` installs it, updates it, rolls it back, rebuilds the mods and starts the game.

## What is in it

The `plus` branch: `multi-player` merged with `final`.

- **multi-player**: co-op with split screen, drop-in/drop-out and the Players panel; online co-op
  (`docs/NETPLAY.md`) with live join (`docs/HOT_JOIN.md`) and the join/accept dialogs; deterministic play and
  rollback; the per-frame speed-ups (`sim-perf`); the themed in-game menus and F1; the HOME Menu disabled;
  the co-op fixes.
- **final**: DualSense support, HD button prompts, Dolphin texture packs, the graphics effects (per-actor
  shadows, the Star Pointer and Pull Star fix), motion style, the Odyssey moveset, controls v8, and the
  animation mod builders' latest changes.

Not in it: `coop-voice-wip` (Luigi's own voice, unfinished) and work still on other sessions' branches until it
is merged into `multi-player` or `final`.

The model, animation and voice mods are **not** in the repository or in any release. They are converted from
your own files on your PC (see Mods). Without those files Plus runs with Galaxy's own Mario.

## Requirements

- Windows 10 or 11, 64-bit, an NTFS drive with about 12 GB free (more for the first build).
- The official Nebula installed with its game files (`Nebula-Setup.exe`), or your Super Mario Galaxy disc
  image (RMGE01, revision 0: ISO, WBFS, RVZ or an extracted folder).
- For the mods: Python 3.9 or newer (the `py` launcher from python.org) and your mod input files.

## Installing

Run `NebulaPlus.exe` (from the release page, or `versions\<version>\NebulaPlus.exe` of an install). It finds the
official Nebula (the folder `Nebula-Setup.exe` registered, normally `%LOCALAPPDATA%\Programs\Nebula`) and your
mod files in Downloads, Documents and Desktop. Choose the Plus folder (default `C:\NebulaPlus`), check the mod
files, choose what to do with saves, and press Install.

From a terminal:

```
NebulaPlus.exe --cli install --root C:\NebulaPlus --detect-mods --import-saves --import-settings
NebulaPlus.exe --cli install --root C:\NebulaPlus --package <NebulaPlus-1.x.y.zip | package folder> --disc D:\Games\RMGE01.wbfs
```

What the install does:

1. **Game files.** Copies the official install's retained game inputs (`game-inputs`, hash-checked against its
   `inputs.json`) and packed content (`content\game.pak`, 4 GB). Without them, it extracts and packs your disc
   with the package's `nebula-recomp` (`--disc`).
2. **Compiler.** Copies the official install's compiler toolchain when it is the same pinned one
   (`installer/toolchain.json`), or downloads it from Microsoft and GitHub with pinned SHA-256s (you accept the
   Microsoft C++ Build Tools license first; `--accept-build-tools-license` in a terminal).
3. **Game modules.** Generates the game's C++ from the game inputs with the package's `nebula-recomp` and
   compiles `RMGE01_game.dll`, `RMGE01_home_button.dll` and `RMGE01_dsp.dll` (about a minute on a 24-core PC,
   10-15 on 8 cores). SSE2 by default, as the co-op and netplay builds were tested; `--module-isa AVX2`.
   Each module must load and match the runtime's module contract before Plus switches to it.
4. **Mods.** Builds the model, Luigi, animation and voice mods into `content\mods` (see Mods).
5. **Switch.** Moves the assembled version into `versions\<version>`, writes `current.json` and
   `current.cmd` last, writes the launchers, and copies the official saves and settings if you asked.

Nothing is written to the official install or `%LOCALAPPDATA%\Nebula`. The official install is only read.

## The Plus folder

```
C:\NebulaPlus\
  Play Nebula Plus.cmd       play (co-op and the Odyssey moves switch on the save-select screen; mods in F1 > General)
  Host online game.cmd       online co-op host (Mario): [port, default 7777 UDP] [input delay, default 3]
  Join online game.cmd       online co-op join (Luigi): <host address>[:port] [input delay]
  Check for updates.cmd      the updater
  Nebula Plus.cmd            the Nebula Plus window
  current.cmd, current.json  the installed version (written last by an install, update or rollback)
  plus.json                  where updates come from, the official install, your mod input files
  versions\<version>\        runtime, game modules, C++ runtime DLLs, NebulaPlus.exe, the package it came from
  content\                   game.pak and the other packed game files; content\mods the built mods
  game-inputs\  toolchain\  source\  python\  logs\  downloads\  staging\
  data\                      YOURS: never replaced by an install, update or rollback
    saves\                   the game's saves (NAND), unless you share the official ones
    runtime_settings.ini     game settings (F1 and the in-game menus); config.ini display settings
    shadercache\  crashdumps\  netplay\  logs\
    launch-env.cmd           optional: your own `set GALAXY_...` lines, called by every launcher
```

The launchers set `GALAXY_NAND_ROOT`, `GALAXY_RUNTIME_SETTINGS_PATH`, `GALAXY_RENDER_CONFIG_PATH`,
`GALAXY_SHADER_CACHE_DIR`, `GALAXY_CRASH_DUMP_DIR` and `GALAXY_NETPLAY_DIR` (the netplay rejoin cache, new in
Plus's runtime) to `data\`, so the runtime never falls back to `%LOCALAPPDATA%\Nebula`. The game's output goes to
`data\logs\run-<time>.log`.

## Saves and settings

- **Own saves (default).** `data\saves`. "Copy the official saves" (`--import-saves`) copies
  `%LOCALAPPDATA%\Nebula\saves` once, when Plus has no saves yet; with saves already there it copies them to
  `data\saves-imported-<time>` instead, so nothing is overwritten. Each file and folder keeps its NTFS streams
  (`galaxy.isfs.meta` holds the Wii file metadata; a plain copy drops it), and every stream is compared after
  the copy. The official saves are read, never moved or changed. `NebulaPlus.exe --cli import-saves` does it
  later.
- **Shared saves** (`--shared-saves`, or the window's checkbox). The launchers point `GALAXY_NAND_ROOT` at the
  official saves folder itself. Plus then writes to the official saves (co-op adds `coop_lives.txt`); back them up
  first. `--own-saves` switches back.
- **Settings.** `--import-settings` copies the official `runtime_settings.ini` and `config.ini` when Plus has
  none. After that the two installs' settings are separate. Settings migrations are the runtime's own, as in the
  official Nebula.

## Mods

Plus converts these from your own files with the repository's builders (`tools/animations`,
`docs/CUSTOM_ANIMATIONS.md`). Set them in the window (Find my mod files, or Browse) or with
`--mod key=path` / `--detect-mods`:

| Key | File | Builds |
| --- | --- | --- |
| `marioModelBlend` | `mario-model-FK.blend` | Odyssey Mario model (`build_odyssey_model_mod.py`), with `marioTextures` and `animationBlend` |
| `marioTextures` | folder with `MarioBody_alb.png` ... | the model's textures |
| `animationBlend` | the Odyssey rig's actions (`64  24.blend`, 2 GB) | animations (`build_animation_mod.py --luigi-originals`), and the model's rest frames |
| `faceAnimations`, `faceSkeleton` | `PlayerAnimationFace.szs`, `Custom Bone vis Armature.fbx` | face animation on the Odyssey model |
| `luigiGltf` | `Luigi.gltf` (its `textures` folder beside it) | Odyssey Luigi for co-op (`build_odyssey_luigi_mod.py`), after the Mario model |
| `voiceClips` | folder with `(Mario) Jump 1.ogg` ... | Mario's voice (`build_voice_mod.py`) |
| `marioModelDae` | `mario.dae` | another Mario model (`build_model_mod.py`), used when there is no Odyssey model |

Each mod whose files are missing is skipped with the reason (`NebulaPlus.exe --cli status` lists them), and one
that fails does not stop the others or the install. The builders read `game.pak` through a hard link in staging
and write into staging; the finished `mods` folder replaces `content\mods` in one rename, and the previous one
stays as `content\mods.previous`. Your input files are only read.

Python: Plus uses its own environment (`python\`) if it made one, else the `py` launcher, else `python` on
PATH. When numpy, Pillow or soundfile are missing it asks before installing them (about 60 MB from PyPI) into
`python\` (`--allow-python-venv`); it never changes your Python installation. No Blender is needed: the
builders read `.blend` files directly.

## Updates

"Check for updates" (`Check for updates.cmd`, the window, or `NebulaPlus.exe --cli update [--check]`) compares
the installed build with GitHub (`plus.json`: `repository`, default `nullgeodesic0/nebula`):

- **Release channel (default).** The newest release tagged `plus-v<version>` with `NebulaPlus-<version>.zip`
  and its `.sig`. The package holds the prebuilt runtime, built only from this repository's source as the
  official Setup's is.
- **Branch channel** (`"channel": "branch"`, `"branch": "plus"`). The branch's newest commit, downloaded as
  GitHub's archive of that commit and built on this PC with `installer/plus/build-plus-package.ps1`. It needs
  Rust and Visual Studio 2022 (or its Build Tools) with the C++ tools, so it is for the maintainer's own PC:
  push, then Check for updates.

An update builds the new version in staging and switches to it only when it is complete:

- **Runtime**: always the new one.
- **Game modules**: recompiled only when the package's module key changes (a hash of `crates/`, `metadata/`,
  `third_party/`, `runtime/include/`, the float and DSP library sources and `Cargo.*`), or the toolchain, the
  instruction set or the game files change; otherwise copied from the installed version. `--force-modules`.
- **Mods**: rebuilt only when the builders (`tools/animations/`), your input files (path, size and time of
  every file) or the game content change. `--force-mods`, or "Rebuild mods".
- **Never written**: `data\` (saves, settings, caches), `plus.json` (except by an install that you give new
  choices), and your mod input files.

The game must be closed for the switch; Plus asks and waits, and never closes it. The previous version stays in
`versions\` for **Roll back** (`--cli rollback`), which also restores the previous mods when they still match.

## Online play between two PCs

Both players need the same Plus build and the same mods. Plus's launchers set `GALAXY_NET_BUILD_ID` from the
source commit, the module key, the module instruction set, the toolchain and a digest of the built mods (new in
Plus's runtime; the official runtime compares the exe's and the game module's size and write time, which never
match between two PCs that compiled the game themselves). A friend with different mod files is told the builds
differ. Live join moves the game's native stacks between the two processes and relies on identical code; two
PCs on the same toolchain and module instruction set compile identical code, but this has not been tried on two
separate PCs yet.

## Trust model

- **Releases** are signed: `NebulaPlus.exe` installs a downloaded package only when its RSA-SHA256 signature
  verifies against the public key it carries (`installer/plus/plus-release-public-key.xml`). The private key
  stays with the maintainer (`%USERPROFILE%\.nebula-plus\plus-release-key.xml`), never in the repository.
  GitHub's SHA-256 of the asset is checked as well when the API reports one. `--allow-unsigned` accepts a
  package on HTTPS and GitHub's SHA-256 alone (for a build made before the key existed); it is never the
  default.
- Inside a package, `plus-package.json` lists every file's SHA-256; the source zip is checked file by file
  against `source-manifest.json` before the game is compiled against it.
- **Branch channel**: the source is GitHub's archive of a commit id over HTTPS; there is no signature, so it
  trusts GitHub and the account that pushes to the branch.
- **Toolchain** downloads (Microsoft's C++ libraries and SDK, llvm-mingw, CMake, Ninja) are checked against the
  SHA-256s pinned in `installer/toolchain.json`.
- **Python packages** come from PyPI over HTTPS through pip, unpinned, and only after you agree.

## Publishing a release (maintainer)

```
installer\plus\new-plus-release-key.cmd             once: the key pair (commit the public key)
git push fork plus                                   the commit must be on GitHub first
installer\plus\publish-plus-release.cmd              build, sign and create the plus-v<version> release
```

`publish-plus-release` builds from `fork/plus` (`-Revision`), names the version `1.<date>.<time>` unless
`-Version` is given, and uploads `NebulaPlus-<version>.zip`, `.sig`, `plus-release.json` and `NebulaPlus.exe`
with the GitHub CLI. `installer\plus\build-plus-package.cmd [-Zip]` builds a package without publishing it
(`dist\plus-<version>\package`); `NebulaPlus.exe --cli install --package <folder>` installs it.

## Content

The repository and the releases hold only source, scripts, tests, docs and what that source builds
(`docs/CONTENT_POLICY.md`, `tools/audit_repo.ps1`). The game modules, the packed game, the Odyssey models,
animations and voice clips are made on each player's PC from their own disc and files, and never uploaded.

## When something goes wrong

- Install and update logs: `logs\install-<time>.log`. The game's: `data\logs\run-<time>.log`.
- `NebulaPlus.exe --cli status` shows the version, the commit, the mods built and skipped, and the update source.
- An install that stops leaves the previous version running: nothing switches until the new one is complete.
