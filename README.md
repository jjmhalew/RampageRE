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

**Playable from start to end in the native port** (`rampage.exe`, [docs/NATIVE.md](docs/NATIVE.md)): the front end
(legal screen, intro movies, menus, profiles, monster and city select), the campaign's seven cities block by block
with their challenges, bosses, results screens and the credits, the other play modes, up to four players (bots
included), sound and music, the Pause menu and saving the profile. It runs on Windows, Linux, the Switch (homebrew)
and Android, on keyboard, controllers (rumble included) or the touch screen, with texture packs and graphics
settings (GTAO, SMAA / MSAA). It has been played on Windows; the Switch, phones and controllers are not yet tried on
real hardware. Still missing: the two arcade games of Bonus Content (Midway Classics) and the demo, and the smaller
gaps that the notes in `docs/` list under "Not ported" (and mark [G] where the port guesses).

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
| Native viewer in C (`rampage.exe`): every city block from `RAMPAGE.VOL`, lit, day and night, traffic, animated people and props, any monster with its clips | ✅ [docs/NATIVE.md](docs/NATIVE.md) |
| The game's play camera (TwoTilts) transcribed; walk a monster about with it following | ✅ [docs/CAMERA.md](docs/CAMERA.md) |
| Monster move graphs (`MONSTER_DATA`): all 30 parsed, the pad variables and the game's runner decoded; clip events (script calls, movement modes), jump physics and the landing slots, the state nodes (playback rate, flags), the game's steering, air control and clip blending; the native viewer plays every monster by its own graph, jumps included, its punches damage the buildings' cells by the game's rules until floors collapse and buildings fall to rubble, and it climbs them | ✅ [docs/FORMAT_MONSTER.md](docs/FORMAT_MONSTER.md) |
| The game in the native port: front end and shell, profiles, game flow, challenges, people, army, traffic, bosses, edibles, effects, sound and music, movies, HUD and Pause | ✅ [docs/NATIVE.md](docs/NATIVE.md) |
| Linux, Switch (homebrew) and Android builds, controllers and rumble, touch pad, texture packs, graphics settings, CI | ✅ (not yet tried on real hardware) [docs/PLATFORM_PLAN.md](docs/PLATFORM_PLAN.md) |
| The arcade games, the demo | ❌ |

**Supported version:** the European PS2 disc, *Rampage - Total Destruction (Europe) (En,Fr,De,Es,It)*,
SLES-53800, `SLES_538.00` version 1.00. Other releases have different addresses and will not work as is.

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

### Prebuilt viewer
The native viewer ([docs/NATIVE.md](docs/NATIVE.md)) is built by GitHub Actions on every push and pull request:
`rampage.exe` for Windows (`build_viewer.bat`) and `rampage` for Linux (`build_viewer.sh`, needs SDL2). Each run of
the [build workflow](https://github.com/jjmhalew/RampageRE/actions/workflows/build.yml) keeps them as artifacts, and
each version tag publishes them on the [releases](https://github.com/jjmhalew/RampageRE/releases) page. They hold
only the port's own code (the program, README.md, LICENSE and two docs), **no game data**: start them with your own
disc image, read in place, e.g. `rampage.exe "D:\Rampage - Total Destruction (Europe) (En,Fr,De,Es,It).iso" Chicago 1`,
or the `RAMPAGE.VOL` from it (`rampage.exe D:\RAMPAGE.VOL Chicago 1`). Without a path the viewer looks for either
beside itself, in the current folder and in `%LOCALAPPDATA%\RampageRE` (`~/.local/share/RampageRE` on Linux; the
whole list is in [docs/NATIVE.md](docs/NATIVE.md) "Game data"). The viewer does not need `build.ps1`. The recompiled
game (`build.ps1`) is not built there, as it needs the disc.

**Nintendo Switch** (homebrew, experimental, not yet tried on a Switch): unpack `RampageRE-switch-<tag>.zip` onto the
SD card (or build `rampage.nro` with `./build_switch.sh`, devkitPro or its Docker image), put your disc image (or
`RAMPAGE.VOL` with `RAMPAGE.ELF`) into `/switch/rampagere/` beside it and start it from the Homebrew Menu, preferably
by holding R while starting a game (title takeover: more memory than the Album applet). It starts in the front end;
B is Cross, the buttons go by place; `rampage.cfg`, the profile and `rampage.log` live in `/switch/rampagere/`
([docs/NATIVE.md](docs/NATIVE.md) "Nintendo Switch").

**Android** (7.0 and up, arm64 or x86_64; so far tried on the emulator only): install the APK from
`RampageRE-android-<tag>.zip` (or build it with `android\gradlew.bat -p android assembleDebug`). At the first start it
asks for your disc image (`.iso`) in the system's file picker and copies `RAMPAGE.VOL` and the game's ELF out of it
into its own folder once (1.35 GB). It starts in the front end and plays on the on-screen pad (a stick on the left,
Cross / Square / Triangle / Circle, R2 and Start on the right) or on a controller; the back button is back or the
Pause menu ([docs/NATIVE.md](docs/NATIVE.md) "Android").

## Repository layout
| Path | |
|---|---|
| `build.ps1` | the whole pipeline: disc → split ELF → C++ → `ps2EntryRunner.exe` |
| `build_viewer.bat`, `src/` | the native viewer `rampage.exe` (C99, Zig, OpenGL; see docs/NATIVE.md) |
| `build_viewer.sh`, `build_switch.sh` | the same viewer for Linux (SDL2) and for the Switch (homebrew `rampage.nro`) |
| `android/` | the same viewer as an Android app (Gradle, SDL2's activity, the on-screen pad of `src/touch.c`) |
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
`android/` is WoodyRE's Android project renamed; `src/touch.c` and `src/android.c` follow WoodyRE's. The Java files
in `android/app/src/main/java/org/libsdl/app/` are SDL 2.32.10's (`android-project`, unchanged), Copyright (C)
1997-2025 Sam Lantinga, under the zlib licence ([android/SDL-LICENSE.txt](android/SDL-LICENSE.txt)); the APK links
SDL 2.32.10 itself, which the build fetches, under the same licence.
