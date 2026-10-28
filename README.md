# RampageRE

[![build](https://github.com/jjmhalew/RampageRE/actions/workflows/build.yml/badge.svg)](https://github.com/jjmhalew/RampageRE/actions/workflows/build.yml)

A native PC port of *Rampage: Total Destruction* (PS2, Pipeworks / Midway, 2006) that runs the game from
**your own disc**: no game files are included in this repository.

It has two halves, the same way as [WoodyRE](https://github.com/jjmhalew/WoodyRE):
- **Recompiled base.** The original game code, statically recompiled to C++ with
  [PS2Recomp](https://github.com/ran-j/PS2Recomp). It runs the real game logic and serves as the reference.
- **Reimplementation.** The game's formats are reverse engineered, documented in `docs/` and read by tools
  that are checked byte for byte against every file on the disc. Those parts then replace the recompiled code
  with readable native code: first the scripts and data, then the renderer, then the game systems.

> Unofficial fan project for preservation. Not affiliated with or endorsed by Pipeworks, Midway, Warner Bros.
> or Sony. Rampage and related names are trademarks of their owners. You need your own copy of the original game.

## Status

**Playable from start to end in the native port** (`RampageRE.exe`, [docs/NATIVE.md](docs/NATIVE.md)): the front end
(legal screen, intro movies, the attract mode and its demo games, menus, profiles, monster and city select), the
campaign's seven cities block by block
with their challenges, bosses, results screens and the credits, the other play modes, one or two players as on the PS2 (computer monsters
included; up to four in King of the City, King of the World and the Timed Run, as on the game's other consoles (`four_players=0`
in `rampage.cfg` keeps the PS2's menu); the command line's `--players 4` in any mode), sound and music, the Pause menu and saving the profile. It runs on Windows, Linux, the Switch and the PS Vita (homebrew)
and Android, on keyboard, controllers (rumble included) or the touch screen, with texture packs and graphics
settings (GTAO, SMAA / MSAA). It has been played on Windows; the Switch, the Vita, phones and controllers are not yet tried on
real hardware. Still missing: the smaller gaps that the notes in `docs/` list under "Not ported" (and mark [G] where
the port guesses). After v1: the two arcade games of Bonus Content (Midway Classics), separate emulator programs on
the disc rather than part of the game ([docs/ARCADE_NOTES.md](docs/ARCADE_NOTES.md)).

**Optional, the Wii version's extras** (a port extra, off unless you give it your own Wii disc's files; see "The Wii's
monsters and Dallas" below): its ten more monsters, Dallas as an eighth city (the campaign's fourth) with its two-boss block, and
the fifth upgrade, SuperJump.

The recompiled game (`build.ps1`) is the reference the port is checked against. It runs the front end and the first
campaign level, Las Vegas, with errors in the 3D picture (black ground), slowly and without sound
([docs/NOTES.md](docs/NOTES.md)).

| Step | |
|---|---|
| Recompile the whole ELF to C++ (8,818 functions, 0 errors) | ✅ |
| Boot, C++ static constructors (280) | ✅ |
| IOP modules (sio2man, libsd, MultiStream) | ✅ |
| File system | ✅ |
| zlib asset loading, data archive mounted, menu loop | ✅ |
| Graphics (GS / VU1): legal screen, GUI (autosave notice) | ✅ |
| Game scripts compile and run in the recompiled game (shell, attract mode) | ✅ |
| Movies (`.pss`) decode and play | ✅ |
| Front end: attract mode, main menu, profile entry | ✅ |
| Into a game: campaign, monster select, Las Vegas loads and runs | ✅ (black ground, slow, no sound) |
| Scripts (`.bsf`) decompiled to source, 138/138 rebuilt byte for byte | ✅ [docs/FORMAT_BSF.md](docs/FORMAT_BSF.md) |
| Bundles (`.bdp`/`.cmp`): container parsed, 1,428 textures to PNG | ✅ [docs/FORMAT_BDP.md](docs/FORMAT_BDP.md) |
| Meshes and materials to glTF (5,577 meshes), three.js viewer | ✅ |
| Property books (`.prb`: city layout, buildings, AI, challenges), 28/28 decoded | ✅ |
| All 7 cities assembled block by block in glTF (buildings with generated facades, street objects) | ✅ |
| Skeletons and animations (60 skeletons, 5,379 animations): all 30 monsters animated in glTF | ✅ |
| City models with node trees (people, vehicles, bosses) with their clips; moving props loop in the blocks | ✅ |
| People and traffic in the blocks (379 people, 375 vehicles; lanes from the game's traffic code) | ✅ |
| Lighting: the game's lights, fog, drifting clouds; every block also at night | ✅ |
| Effects (297, rebuilt byte for byte), string tables (13, 9 languages), GUI layouts (XML) | ✅ [docs/FORMAT_EDF.md](docs/FORMAT_EDF.md) |
| Native viewer in C (`RampageRE-dev.exe`): every city block from `RAMPAGE.VOL`, lit, day and night, traffic, animated people and props, any monster with its clips | ✅ [docs/NATIVE.md](docs/NATIVE.md) |
| The game's play camera (TwoTilts) transcribed; walk a monster about with it following | ✅ [docs/CAMERA.md](docs/CAMERA.md) |
| Monster move graphs (`MONSTER_DATA`): all 30 parsed, the pad variables and the game's runner decoded; clip events (script calls, movement modes), jump physics and the landing slots, the state nodes (playback rate, flags), the game's steering, air control and clip blending; the native viewer plays every monster by its own graph, jumps included, its punches damage the buildings' cells by the game's rules until floors collapse and buildings fall to rubble, and it climbs them | ✅ [docs/FORMAT_MONSTER.md](docs/FORMAT_MONSTER.md) |
| The game in the native port: front end and shell, profiles, game flow, challenges, people, army, traffic, bosses, edibles, effects, sound and music, movies, HUD and Pause | ✅ [docs/NATIVE.md](docs/NATIVE.md) |
| Linux, Switch and PS Vita (homebrew) and Android builds, controllers and rumble, touch pad, texture packs, graphics settings, CI | ✅ (not yet tried on real hardware) [docs/PLATFORM_PLAN.md](docs/PLATFORM_PLAN.md) |
| The attract mode's demo games (a random city, block and monsters, played by the computer) | ✅ [docs/SHELL_NOTES.md](docs/SHELL_NOTES.md) |
| The Wii version's extras from your own Wii disc: 40 monsters, Dallas, SuperJump and its fifth star, the Wii's unlocks | ✅ [docs/WII_PLAN.md](docs/WII_PLAN.md) |
| The arcade games (Midway Classics: Digital Eclipse's emulators on the disc) | after v1 [docs/ARCADE_NOTES.md](docs/ARCADE_NOTES.md) |
| Coverage of the game's code: every function of the ELF ported, read, replaced by the port's own, unused by the data, or open (0.8 % of the code: the monsters' clip effects and roar, the tail motion, a few smaller items) | [docs/COVERAGE.md](docs/COVERAGE.md) |

**Supported versions:** the European PS2 disc, *Rampage - Total Destruction (Europe) (En,Fr,De,Es,It)*,
SLES-53800, `SLES_538.00` version 1.00, and the USA disc, *Rampage - Total Destruction (USA)*, SLUS-21323,
`SLUS_213.23` version 1.01. Both hold the same game, byte for byte (the ELF and `RAMPAGE.VOL`); only `SYSTEM.CNF`
differs, which picks the region: the USA disc plays the NTSC movies and has the Ballers box in the Midway Classics.
Other releases may have different addresses and may not work as is.

## Building
Needs Windows, Python 3, Git and Visual Studio 2022 or later with the *Desktop development with C++* workload.

```powershell
git clone --recursive https://github.com/<you>/RampageRE.git
cd RampageRE
.\build.ps1 -Iso "D:\path\to\Rampage - Total Destruction (Europe) (En,Fr,De,Es,It).iso" -Run
```

The first build extracts the disc into `build\disc`, recompiles the game to C++ and builds it (about 30 minutes).
After that, `.\build.ps1 -Run` rebuilds and starts it. Everything generated lives in `build\` and in the
PS2Recomp submodule's `src/runner` folder, which are not committed.

### Prebuilt
The native port ([docs/NATIVE.md](docs/NATIVE.md)) is built by GitHub Actions on every push and pull request:
`RampageRE.exe` and `RampageRE-dev.exe` for Windows (`build.bat`) and `rampage` for Linux (`build.sh`, needs SDL2). Each run
of the [build workflow](https://github.com/jjmhalew/RampageRE/actions/workflows/build.yml) keeps them as artifacts, and
each version tag publishes them on the [releases](https://github.com/jjmhalew/RampageRE/releases) page. They hold
only the port's own code (the programs, README.md, LICENSE and two docs), **no game data**.

**Windows**: start `RampageRE.exe` (the exe is not signed, so SmartScreen may say "Windows protected your PC" at the
first start: *More info*, then *Run anyway*). At the first start it asks for your disc image (`.iso`) of the game, or
the `RAMPAGE.VOL` of a copy of the disc, in a file dialog; it reads it where it is (nothing is copied) and remembers it
in `%LOCALAPPDATA%\RampageRE\disc.txt`. It starts in the front end; `rampage.cfg`, the profile, `rampage.log` and
`mods\` are written beside the exe (or in `%LOCALAPPDATA%\RampageRE` when that folder is read-only or temporary).
`RampageRE-dev.exe` is the same program with a console, for the command line of [docs/NATIVE.md](docs/NATIVE.md):
`RampageRE-dev.exe "D:\Rampage - Total Destruction (Europe) (En,Fr,De,Es,It).iso" Chicago 1`, or the `RAMPAGE.VOL` from
it (`RampageRE-dev.exe D:\RAMPAGE.VOL Chicago 1`). Without a path both look for the data beside themselves, in the current
folder and in `%LOCALAPPDATA%\RampageRE` first (`~/.local/share/RampageRE` on Linux; the whole list is in
[docs/NATIVE.md](docs/NATIVE.md) "Game data"). They do not need `build.ps1`. The recompiled game (`build.ps1`) is not
built there, as it needs the disc.

**Linux / Steam Deck**: unpack `RampageRE-linux-x86_64-<tag>.tar.gz` and start `./rampage ~/Rampage.iso` (your
disc image or `RAMPAGE.VOL`; needs SDL2, which SteamOS has). It starts in the front end. On the Steam Deck add
`rampage` to Steam as a non-Steam game with the disc image's path as its launch option, so Steam hands its controls over
as a pad.

**Nintendo Switch** (homebrew, experimental, not yet tried on a Switch): unpack `RampageRE-switch-<tag>.zip` onto the
SD card (or build `rampage.nro` with `./build_switch.sh`, devkitPro or its Docker image), put your disc image (or
`RAMPAGE.VOL` with `RAMPAGE.ELF`) into `/switch/rampagere/` beside it and start it from the Homebrew Menu, preferably
by holding R while starting a game (title takeover: more memory than the Album applet). It starts in the front end;
B is Cross, the buttons go by place; `rampage.cfg`, the profile and `rampage.log` live in `/switch/rampagere/`
([docs/NATIVE.md](docs/NATIVE.md) "Nintendo Switch").

**PS Vita** (homebrew, experimental, not yet tried on a Vita): install `rampage.vpk` from `RampageRE-vita-<tag>.zip`
with VitaShell (or build it with `./build_vita.sh`, VitaSDK or its Docker image) and put your disc image (or
`RAMPAGE.VOL` with `RAMPAGE.ELF`) into `ux0:data/rampagere/`. It needs `ur0:data/libshacccg.suprx`, the system's shader
compiler: run ShaRKBR33D once to extract it. It starts in the front end on OpenGL ES 2.0 (no GTAO / SMAA); the rear
touch pad's halves are L2 / R2; `rampage.cfg`, the profile and `rampage.log` live in `ux0:data/rampagere/`
([docs/NATIVE.md](docs/NATIVE.md) "PS Vita"). Whether a Vita is fast enough, and has memory enough, is not known yet.

**Android** (7.0 and up, arm64 or x86_64; so far tried on the emulator only): install the APK from
`RampageRE-android-<tag>.zip` (or build it with `android\gradlew.bat -p android assembleDebug`). At the first start it
asks for your disc image (`.iso`) in the system's file picker and copies `RAMPAGE.VOL` and the game's ELF out of it
into its own folder once (1.35 GB); an APK with your data already inside (`make_android_bundle.bat`, below) neither
asks nor copies. It starts in the front end and plays on the on-screen pad (a stick on the left,
Cross / Square / Triangle / Circle, R2 and Start on the right) or on a controller; the back button is back or the
Pause menu ([docs/NATIVE.md](docs/NATIVE.md) "Android").

### The Wii's monsters and Dallas (optional)
The Wii version adds to the PS2 game ten monsters (Boris and Ruby from the start; Plucky, Fifi and Cyril
revealed in Dallas; Squirmy, Flea, Sarah, Brian and VERN for 1, 3, 6, 12 and 20 monsters with all five upgrades),
Dallas as the fourth of eight cities with a block of two bosses, and a fifth upgrade, SuperJump (Cross pressed again
while landing). The port plays them from **your own Wii disc**, next to the PS2 data, which it still needs:
extract the disc with Dolphin (the game's *Properties*, *Filesystem*, right-click the disc: *Extract Entire Disc*). Only its
`DATA/files/Game`, `Shell` and `Audio` folders are read (about 320 MB). Tried with the European Wii disc (RPGP5D).

| Where | How |
|---|---|
| Windows | `RampageRE.exe` asks once, at a start when nothing names it: *Yes* opens a folder dialog for the extracted disc (the folder with `DATA`, or its `DATA/files`), *No* asks no more; the answer is kept in `rampage.cfg` as `wii_data=` (delete that line to be asked again). Or `RampageRE-dev.exe ... --wii DIR`, or `RAMPAGE_WII_DATA` |
| Linux | `wii_data=` in `rampage.cfg` set to the extracted disc, or `--wii DIR`, or `RAMPAGE_WII_DATA` |
| Switch, PS Vita | a `wii` folder with `Game`, `Shell` and `Audio` in `/switch/rampagere/` or `ux0:data/rampagere/`, found on its own |
| Android | the same `wii` folder in the app folder by USB cable, or the launcher's shortcut **Add the Wii's data** (a long press on the icon), which copies it from the folder you pick; after the first start's copy the app asks once |
| `make_standalone.bat`, `make_android_bundle.bat` | drag the extracted disc's folder onto the .bat with the rest (or leave `wii_data=` in the `rampage.cfg` beside it): it goes into the exe or APK and is used from there |

Without it the port is the PS2 game, byte for byte, and saves as it did. With it the shell, the profile and the
unlocks follow the Wii's roster of 40 monsters and its city order; the save stays the PS2's (`rampage_profile.bin`),
with what the PS2 file cannot hold in `rampage_profile.wii.bin` beside it. `wii_data=none` leaves it out (and a `wii` folder too).
It has been played on Windows (and on the Android emulator); the Linux build, the Switch, the Vita and phones are not yet tried with it. As with the PS2 data, the bundles that hold it
are for your own use only.

### Building the native port
Windows 10 or 11; nothing needs to be installed first (the scripts are WoodyRE's, adapted).

| Script | Builds |
|---|---|
| `build.bat` | `RampageRE.exe`, the player build: no console, the icon and version of `res/rampagere.rc`, starts in the front end, asks for the disc image at the first start, logs to `rampage.log`, says in a message box when it cannot start |
| `build.bat dev` | `RampageRE-dev.exe`, the developer build: the same program with a console that prints what each block holds (the one docs/NATIVE.md and the tests use; `build_viewer.bat`, the old name, builds it) |
| `build.sh` | `./rampage` for Linux and the Steam Deck (SDL2; `GLES=1` for OpenGL ES; `build_viewer.sh` is the old name) |
| `build_switch.sh` | `rampage.nro` for the Switch (devkitPro, or its Docker image) |
| `build_vita.sh` | `rampage.vpk` for the PS Vita (VitaSDK, or its Docker image) |
| `android\gradlew.bat -p android assembleRelease` | the Android APK (docs/NATIVE.md "Android") |
| `make_standalone.bat` | `RampageRE-standalone.exe`: `RampageRE.exe` and **your** game data (`RAMPAGE.VOL`, the ELF and `SYSTEM.CNF` from your disc, 1.35 GB) in one exe, which unpacks itself to `%LOCALAPPDATA%\RampageRE` at its first start and runs from there. Drag your disc image or `RAMPAGE.VOL` onto the .bat (else the data `RampageRE.exe` finds is taken), and your extracted Wii disc's folder for the Wii's extras (about 320 MB more) |
| `make_android_bundle.bat` | `RampageRE-bundle.apk`: the APK of a release (or of your own build) and **your** game data in one APK; the app reads the data from its own APK, so it neither asks for the disc nor copies anything. Drag the APK and your disc image (or `RAMPAGE.VOL`) onto the .bat, or put them next to it, and your extracted Wii disc's folder for the Wii's extras (about 320 MB more). It is signed with a key of your own (made once, kept in `%LOCALAPPDATA%\RampageRE\android-bundle.key`), so later bundles install as updates of it but not over a RampageRE from the Releases page: uninstall that first (its saves and copied files go with it) |

The C compiler is [Zig](https://ziglang.org): one in `tools\zig`, on `PATH` or from `pip install ziglang` is used,
otherwise `build.bat` downloads Zig 0.16.0 (about 95 MB) into `tools\zig` once and checks its SHA-256.
`build.bat standalone` and `build.bat apkbundle` are the steps the two make_ scripts run; `build.bat tools` only builds
their helpers (`build\bundle_stub.exe`, `build\pack.exe`, `build\apkbundle.exe`).

**The standalone exe and the APK bundle contain the game's data: they are for your own use on your own devices only.
Never share or upload them.**

## Repository layout
| Path | |
|---|---|
| `build.ps1` | the whole pipeline: disc → split ELF → C++ → `ps2EntryRunner.exe` |
| `build.bat`, `src/` | the native port `RampageRE.exe` / `RampageRE-dev.exe` (C99, Zig, OpenGL; see docs/NATIVE.md) |
| `build.sh`, `build_switch.sh`, `build_vita.sh` | the same port for Linux (SDL2), the Switch (homebrew `rampage.nro`) and the PS Vita (homebrew `rampage.vpk`) |
| `android/` | the same port as an Android app (Gradle, SDL2's activity, the on-screen pad of `src/touch.c`) |
| `make_standalone.bat`, `make_android_bundle.bat`, `tools/native/` | your own one-file builds with your game data inside: the standalone launcher (`bundle.c`) and its packer (`pack.c`), the APK bundler and signer (`apkbundle.c`) |
| `res/` | the port's own icon (`rampagere.ico`, made from `switch_icon.jpg` by `tools/mkicon.py`) and `rampagere.rc` (icon and version of the Windows exes); `vita/`, the Vita's LiveArea images (`tools/mkvita.py`) |
| `build_viewer.bat`, `build_viewer.sh` | the old names of `build.bat dev` and `build.sh`, kept for older instructions |
| `recomp/config.toml` | PS2Recomp config: stub bindings for the SCE libraries and newlib, plus Rampage fixes |
| `recomp/funcmap.csv` | function map (Ghidra CSV format) the current state was tested with |
| `tools/split_elf.py` | splits the retail ELF's single code+data segment into real sections |
| `tools/funcmap.py` | finds function starts without Ghidra (jal targets, vtables, constructors, ...) |
| `tools/pvol.py` | lists or extracts `RAMPAGE.VOL`, the game's data archive (352 named files) |
| `tools/bsf.py` | decompiles the 138 game scripts (`.bsf`) to source and compiles them back (`--verify`) |
| `tools/bdp.py` | lists and extracts bundle entries (`--verify`: all 117 bundles fully accounted for) |
| `tools/tex.py` | exports bundle textures to PNG |
| `tools/mesh.py` | exports bundle meshes with materials and textures as glTF (`--verify`: all 5,577 meshes) |
| `tools/city.py` | assembles a city's blocks (ground, buildings, street objects) into glTF scenes |
| `tools/edf.py` | decompiles the 297 effect files ("Text EDF file") to text and compiles them back (`--verify`) |
| `tools/monster.py` | monster move graphs (`MONSTER_DATA`): parses all 30 (`--verify`), prints states, transitions and their conditions by pad input (`Square==1(new) L.up==1`) |
| `tools/loc.py` | string tables (`.loc`): 574 keys in 9 languages to TSV (`--verify`) |
| `tools/anim.py` | skeletons and animations: exports each monster as an animated, skinned glTF, and a city's people, vehicles and bosses with their clips (`--verify`: all 615 node trees and 5,379 animations) |
| `tools/prb.py` | prints property books (design data) as readable trees |
| `viewer/index.html` | three.js viewer for the exported glTF, plays animations (serve the repository root) |
| `tools/refs.py` | finds the code that references an address (strings, tables) |
| `tools/coverage.py` | the coverage map: every function's status from the addresses `src/` and `docs/` cite and the rules in `tools/coverage_groups.txt` (`--md docs/COVERAGE.md`, `--html` for a clickable map, `--range` to sort more) |
| `tools/vudis.py` | disassembles VU micro code and decodes the runtime's VU1 traces |
| `tools/iso9660.py` | extracts a PS2 disc image |
| `patches/ps2recomp-runtime.diff` | runtime changes applied to the PS2Recomp submodule by `build.ps1` |
| `PS2Recomp/` | submodule, pinned to the tested commit |

## License
GPL-3.0, like PS2Recomp. See [LICENSE](LICENSE).

Third party: `src/stb/` holds Sean Barrett's stb_image and stb_image_write (public domain or MIT, see the end of each
file), used by the texture packs ([docs/TEXTURES.md](docs/TEXTURES.md)); `src/texpack.c`, `src/postfx.c` and
`src/gtao.c` come from [WoodyRE](https://github.com/jjmhalew/WoodyRE) (GPL-3.0-or-later). `src/smaa/smaa.h` is
[SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego
Gutierrez (shader source and lookup textures, as generated by WoodyRE), under the MIT licence printed at its top.
`android/` is WoodyRE's Android project renamed; `src/touch.c` and `src/android.c` follow WoodyRE's. `build.bat`,
`make_standalone.bat`, `make_android_bundle.bat`, `res/rampagere.rc`, `tools/mkicon.py` and `tools/native/` (`bundle.c`,
`pack.c`, `apkbundle.c`) are adapted from WoodyRE's files of the same names (GPL-3.0-or-later, the same author). The icon
(`res/switch_icon.jpg`, `res/rampagere.ico`) is the project's own art (the words RAMPAGE RE over a plain skyline), not
the game's. The Java files
in `android/app/src/main/java/org/libsdl/app/` are SDL 2.32.10's (`android-project`, unchanged), Copyright (C)
1997-2025 Sam Lantinga, under the zlib licence ([android/SDL-LICENSE.txt](android/SDL-LICENSE.txt)); the APK links
SDL 2.32.10 itself, which the build fetches, under the same licence.
