# Nebula Plus

Nebula Plus installs this fork's Nebula 2 (the native game, built from the Petari decompilation) with its
additions: Mario and Luigi co-op on one PC with split screen, online co-op (lockstep, rollback and drop-in live
join), the Odyssey moveset and motion style, the HD button prompts and controls, Dolphin texture packs, the scene
effects and the custom voice support. It sits in its own folder, never changes an official Nebula install, and keeps its own saves
and settings.

`NebulaPlus.exe` installs, updates, rolls back and builds custom mods; `NebulaPlusCli.exe` is the same
program for terminals. The README's "For friends" section is the short version of this page.

## What a friend needs

- Windows 10 or 11, 64-bit; a GPU with Direct3D 12; a CPU with AVX2; about 8 GB free on an NTFS drive.
- Their own dump of the Super Mario Galaxy USA disc (RMGE01, revision 0) as ISO, WBFS, RVZ or an extracted folder. Nothing else: no official Nebula, no compiler, no Python (only to build optional custom mods).

## Installing

Download `NebulaPlus-Setup-<version>.zip` from the release page, unzip it and run `Install Nebula Plus.cmd` (or `NebulaPlus.exe`): it installs from the signed package in the zip, with no other download. Choose the Nebula Plus folder (default `C:\NebulaPlus`) and the disc
image, optionally files for custom mods and what to do with saves, and press Install. From a terminal:

```
NebulaPlusCli.exe install --root C:\NebulaPlus --disc D:\Games\RMGE01.wbfs
NebulaPlusCli.exe install --root C:\NebulaPlus --disc D:\Games\RMGE01.wbfs --detect-mods --import-saves --import-settings
```

Without `--package` the install uses the signed package beside the program (the setup zip), else downloads the newest release package (see Updates). What it does:

1. **The game files.** `nebula-import` checks that the image is RMGE01 revision 0 and packs its files into
   `content\game.pak` (about 3 GB, a minute or two). When an official Nebula 2 install with a packed game is found it
   is copied instead (`--disc` always packs from the disc). An official Nebula 1.x (recompiled) install's files are a
   different format and are not used.
2. **The game.** `NebulaNative.exe` and `smg_game.dll` come prebuilt in the package, built only from this
   repository's source (`docs/CONTENT_POLICY.md`); every Plus install runs the same `smg_game.dll`, which online
   play needs.
3. **Custom mods** (optional; see below) from files you choose.
4. **The switch.** The new version goes to `versions\<version>`; `current.json` and `current.cmd` are written last,
   then the launchers. Saves and settings are copied from an official install only when asked.

## The Plus folder

```
C:\NebulaPlus\
  Play Nebula Plus.cmd       one player
  Play co-op.cmd             two players on one PC (the second controller presses + to join as Luigi)
  Host online game.cmd       online co-op host (Mario): [port, default 7777 TCP] [input delay] [live | rollback | lockstep]
  Join online game.cmd       online co-op join (Luigi): <host address>[:port]
  Check for updates.cmd      the updater
  Nebula Plus.cmd            the Nebula Plus window
  current.cmd, current.json  the installed version
  plus.json                  where updates come from, the disc, the official install, the mod files
  versions\<version>\        NebulaNative.exe, smg_game.dll, nebula-import.exe, NebulaPlus(Cli).exe, the package
  content\                   game.pak and files\ (packed from your disc)
  source\  python\  logs\  downloads\  staging\  mod-previous\
  data\                      YOURS: never replaced by an install, update or rollback
    saves\                   the game's saves (the Wii's internal storage)
    Nebula\                  the game's %LOCALAPPDATA%\Nebula: runtime_settings.ini, config.ini, mods\, mods.ini,
                             shader cache, crash reports, netplay\ (the host's save while you join a session)
    logs\                    the game's output (run-, coop-, host-, join-<time>.log)
    launch-env.cmd           optional: your own `set GALAXY_...` lines, called by every launcher
  data-join\                 the joiner's own settings and caches (its mods mirror data\Nebula\mods at each start)
```

The launchers set `LOCALAPPDATA` to `data\` for the game and pass `--pak content\game.pak --nand data\saves`, so
nothing is written to the PC's own `%LOCALAPPDATA%\Nebula`.

## Saves and settings

- **Own saves (default)**: `data\saves`. "Copy the official saves" (`--import-saves`) copies
  `%LOCALAPPDATA%\Nebula\saves` once when Plus has none; with saves already there they go to
  `data\saves-imported-<time>` instead, so nothing is overwritten. Each file and folder keeps its NTFS streams
  (`galaxy.isfs.meta`, the Wii file metadata), and every stream is compared after the copy. The official saves are
  read, never moved or changed.
- **Shared saves** (`--shared-saves`): the launchers use the official saves folder itself. Plus then writes to it;
  back it up first. `--own-saves` switches back.
- **Settings**: `--import-settings` copies the official `runtime_settings.ini` and `config.ini` into `data\Nebula`
  when Plus has none. Settings migrations are the game's own.
- **Updates and rollbacks never write to `data\`**, with one exception: the mod folder Plus built there itself
  (`data\Nebula\mods\Odyssey Mario`, marked by `plus-mod.json`) is rebuilt when its inputs change. `mods.ini` (the
  mods' order and on/off) is the game's and is never written.

## Custom mods

Custom models, animations and voices can be added as mods (Nebula's mods folder, `data\Nebula\mods`; Settings >
Mods turns each on or off and sets their order). Nebula Plus can also build one from files you have, with the
repository's builders (`tools/animations`, `docs/CUSTOM_ANIMATIONS.md`): choose the files in the window (Browse, or
"Find my mod files") or pass `--mod key=path` / `--detect-mods`, and it writes the mod
`data\Nebula\mods\Odyssey Mario` on this PC. Parts without files are simply not built. Built mods stay on your PC:
they are never uploaded, and Nebula Plus downloads none.

| Key | File | Builds |
| --- | --- | --- |
| `marioModelBlend`, `marioTextures` | a model `.blend` on the Odyssey rig, and its textures folder | a Mario model (also needs `animationBlend` for the rig's rest frames) |
| `marioModelDae` | a `mario.dae` | a Mario model from COLLADA, when there is no `.blend` model |
| `animationBlend` | an animation `.blend` on the Odyssey rig | animations |
| `faceAnimations`, `faceSkeleton` | `PlayerAnimationFace.szs`, the face skeleton `.fbx` | face animation on the model |
| `luigiGltf` | a `Luigi.gltf` (its `textures` folder beside it) | a Luigi model for co-op, after the Mario model |
| `voiceClips` | a folder of clips named like `(Mario) Jump 1.ogg` | voice lines |

"Rebuild mods" (`NebulaPlusCli.exe build-mods`) builds again; an update rebuilds when the builders, the files
(path, size and time of every file) or the packed game change. The previous build is kept in `mod-previous\`. A mod
folder named "Odyssey Mario" that Plus did not build is left alone; Plus then uses "Odyssey Mario (Nebula Plus)".

The builders need Python 3.9 or newer with numpy, Pillow and soundfile. Plus looks for an installed `python.exe`
(Python.org installs, the Python install manager's runtimes, PATH); it never runs the `py` launcher, which can
download and install a Python on its own. When the packages are missing it asks before installing them (about
60 MB from PyPI) into its own `python\` environment (`--allow-python-venv`); the system Python is not changed. No
Blender is needed: the builders read `.blend` files directly.

## Updates

"Check for updates" (`Check for updates.cmd`, the window, `NebulaPlusCli.exe update [--check]`) compares the
installed build with GitHub (`plus.json`: `repository`, default `nullgeodesic0/nebula`):

- **Release channel (default).** The newest release tagged `plus2-v<version>` with `NebulaPlus-<version>.zip` and its
  `.sig`. (The `plus-v` releases are Nebula Plus for the old recompiled Nebula 1; installs of one never update to
  the other.)
- **Branch channel** (`"channel": "branch"`, `"branch": "nebula2"`). The branch's newest commit after the installed
  one, as GitHub's archive of that commit, built on this PC with `installer/plus/build-plus-package.ps1`. It needs
  Rust, LLVM/clang and Visual Studio 2022 (or its Build Tools) with the C++ tools, so it is for the maintainer: push,
  then Check for updates.

An update installs the new version beside the current one and switches only when it is complete; the game must be
closed for the switch (Plus asks and waits, and never closes it). The packed game is kept; a custom mod is rebuilt only
when needed; saves and settings are not touched. The previous version stays in `versions\` for **Roll back**
(`NebulaPlusCli.exe rollback`).

## Online play

`Host online game.cmd` listens on TCP port 7777 (an argument changes it); a host outside the friend's network
forwards that port on their router (or both use a virtual LAN such as Tailscale or ZeroTier). Modes: `live` (the
default: the host plays at once, the friend can join at any time and drops in as Luigi), `rollback` (both start
together; each PC runs ahead and corrects itself) and `lockstep`. `Join online game.cmd <address>` joins; the joiner
plays the host's save, which is kept in `data-join\Nebula\netplay`, never in its own saves. Both PCs must run the same
Nebula Plus version and the same mods (the handshake compares the game module and the mods and refuses a mismatch).
`docs/NETPLAY.md` has the details.

## Trust model

- **Releases are signed.** `NebulaPlus.exe` installs a downloaded package only when its RSA-SHA256 signature verifies
  against the public key it carries (`installer/plus/plus-release-public-key.xml`). The private key stays with the
  maintainer (`%USERPROFILE%\.nebula-plus\plus-release-key.xml`), never in the repository. GitHub's SHA-256 of the
  asset is checked as well when the API reports one. `--allow-unsigned` accepts a package on HTTPS and GitHub's
  SHA-256 alone; it is never the default.
- Inside a package, `plus-package.json` lists every file's SHA-256, and the source the mod builders run from is
  checked file by file against `source-manifest.json`.
- The **branch channel** trusts GitHub's archive of a commit id over HTTPS (no signature).
- **Python packages** come from PyPI over HTTPS through pip, unpinned, and only after the player agrees.

## Publishing an update (maintainer)

Once, on the PC that publishes:

1. Install Git, Rust (rustup), Visual Studio 2022 Build Tools with the C++ workload and Windows SDK, LLVM (clang 19
   or newer, `C:\Program Files\LLVM`), and the GitHub CLI; run `gh auth login`.
2. `installer\plus\new-plus-release-key.cmd` makes the signing key: the private half goes to
   `%USERPROFILE%\.nebula-plus\plus-release-key.xml` (keep it private and backed up; a release signed with another key
   does not install over this one), the public half to `installer/plus/plus-release-public-key.xml` (commit it: every
   NebulaPlus.exe built from then on trusts it). This PC's key already exists; do not make a new one.

For each update:

1. Commit the change on `nebula2` and push it: `git push fork nebula2`.
2. Run `installer\plus\publish-plus-release.cmd`. It fetches `fork/nebula2`, builds the package from that exact commit
   (nebula-import, NebulaNative.exe, smg_game.dll, NebulaPlus.exe), signs `NebulaPlus-<version>.zip` with the private
   key (RSA-SHA256 over the zip, base64 in `.sig`), and creates the GitHub release `plus2-v<version>` with the setup
   zip, the package, its signature and `plus-release.json`. The version is `2.<UTC date>.<UTC time>` unless
   `-Version` is given; it must be higher than the last release's. `-Prerelease` lists it as a pre-release (for a
   test build), `-Draft` makes a draft (invisible to the updater until published), `-Notes` sets the text players
   see in the update prompt.
3. Players get it from Check for updates. Nothing else is uploaded: no game files, no built mods.

`installer\plus\build-plus-package.cmd -Zip` builds a package without publishing it (`dist\plus-<version>`);
`NebulaPlusCli.exe install --package <zip or folder>` installs it. `installer\plus\tests\run-plus-tests.ps1` runs the
installer's checks.

## Content

The repository and the releases hold only source, scripts, tests, docs and what that source builds
(`docs/CONTENT_POLICY.md`, `tools/audit_repo.ps1`). `game.pak` and any custom mods are made on each player's PC
from their own disc and files, and never uploaded.

## When something goes wrong

- Install and update logs: `logs\install-<time>.log`. The game's: `data\logs\<launcher>-<time>.log`.
- `NebulaPlusCli.exe status` shows the version, the commit, the custom mod's parts and the update source.
- An install that stops leaves the previous version as it was: nothing switches until the new one is complete.
