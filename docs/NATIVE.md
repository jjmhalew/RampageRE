# The native port (`src/`, `RampageRE.exe`, `RampageRE-dev.exe`)

The second half of the hybrid approach: next to the recompiled game, a hand-written C program that reads the game's
data with what the format work found and draws it, the way WoodyRE reimplements its game. The first piece is a viewer
for the city blocks; it reads `RAMPAGE.VOL` itself (no extracted files, no Python), so it is the C port of the
Python tools' readers and of `tools/city.py`.

## Building and running
```bat
build.bat dev                                 :: RampageRE-dev.exe, the developer build (a console); build_viewer.bat does the same
RampageRE-dev.exe                                   :: the data it finds (below; build\disc\RAMPAGE.VOL first), Chicago, block 1
RampageRE-dev.exe NewYork 6 --night                 :: the same, another block
RampageRE-dev.exe "D:\Rampage - Total Destruction (Europe) (En,Fr,De,Es,It).iso" Chicago 1   :: straight from the disc image
RampageRE-dev.exe build\disc\RAMPAGE.VOL NewYork 6 --night
RampageRE-dev.exe build\disc\RAMPAGE.VOL Chicago 1 --monster George   :: play George by his own move graph, the game's camera follows
RampageRE-dev.exe build\disc\RAMPAGE.VOL Chicago 1 --shot shot.png   :: one frame to a PNG, then quit (offscreen: no window)
RampageRE-dev.exe build\disc\RAMPAGE.VOL NewYork 3 --monster George --do "at 25 -4, up 1" --cam 10 -10 8 30 5 --shot s.png
RampageRE-dev.exe build\disc\RAMPAGE.VOL Chicago 1 --players 2 --monster George --monster2 Lizzie   :: two on the keys
RampageRE-dev.exe build\disc\RAMPAGE.VOL Chicago 1 --players 4 --bots 2 --play kingcity            :: two players, two bots
```
`build.bat` compiles C99 with Zig (`zig cc`), like WoodyRE's: a Zig in `tools\zig`, on `PATH` or from
`pip install ziglang` is used, otherwise Zig 0.16.0 is downloaded once and checked against its SHA-256 ("Windows
builds" below has its other targets).

### Game data
The viewer needs only the disc: `RAMPAGE.VOL` and the game's ELF (`fx.c` reads the effects' word table from it). The
first argument may be the disc image itself (`.iso`, ISO 9660 with 2048-byte sectors, as the PS2 DVD dumps are; the
UDF bridge keeps the ISO 9660 tree): `RAMPAGE.VOL` and `SLES_538.00` are read in place from it, nothing is copied
or unpacked (src/iso.c; `Vol.base` is where the VOL starts in the image). Beside a `RAMPAGE.VOL` the ELF is
`RAMPAGE.ELF` (`build.ps1`'s split copy) or `SLES_538.00` (copied off the disc); either works. `build.ps1` is not
needed for the viewer.

Without a path (or with a city name first: `RampageRE-dev.exe NewYork 6`) it looks, in this order, and takes the first:
1. `RAMPAGE_VOL` (the environment variable: a `RAMPAGE.VOL` or an image, taken as it is)
2. `build\disc\RAMPAGE.VOL` (what `build.ps1` extracts), `.\RAMPAGE.VOL`, `data\RAMPAGE.VOL`, `RAMPAGE.VOL` beside the
   program
3. a disc image (`*.iso` with `RAMPAGE.VOL` in it; by name, other images are passed over) beside the program, then in
   the current folder, then in `data\`
4. the user's data folder, `RAMPAGE.VOL` or an image in it: `%LOCALAPPDATA%\RampageRE` on Windows,
   `$XDG_DATA_HOME/RampageRE` (else `~/.local/share/RampageRE`) elsewhere
5. the disc image or `RAMPAGE.VOL` chosen at an earlier start: the path in that folder's `disc.txt`, when it is still
   there and readable (a disc drive left empty asks nothing)

On the Switch `sdmc:/switch/rampagere` comes first ("Nintendo Switch" below), on the PS Vita `ux0:data/rampagere`
("PS Vita"), on Android the app folder, then the
app's own APK when it is a bundle of `make_android_bundle.bat` ("Android"; a first start without either asks for the
disc image).

It never searches drives. With nothing found, the Windows builds ask (not under `--shot`): a message box says what is
needed, then a file dialog takes the disc image (`*.iso`) or a `RAMPAGE.VOL` (with `SLES_538.00` or `RAMPAGE.ELF`
beside it); the choice is checked (an image with a `RAMPAGE.VOL` in it, or a VOL), read in place, nothing copied, and
written to `%LOCALAPPDATA%\RampageRE\disc.txt` (step 5) so the next start finds it. Cancel quits. Elsewhere, and in a
`--shot` run, it says where it looked and exits with 1 (a `--shot` run prints that to stderr, as every message). An
image without `RAMPAGE.VOL` is refused ("not Rampage - Total Destruction"). An APK of `make_android_bundle.bat` counts
as a disc image on every system (`RampageRE-dev.exe RampageRE-bundle.apk Chicago 1` works too): its stored entries
`assets/RAMPAGE.VOL`, `assets/SLES_538.00` and `assets/SYSTEM.CNF` are read in place (src/iso.c `zip_find`).

The supported releases are the European SLES-53800 version 1.00 and the USA SLUS-21323 version 1.01: their ELF
(`SLES_538.00`, `SLUS_213.23`) and `RAMPAGE.VOL` are the same files, byte for byte, and so are `DESI/`, `MODULES/` and
`RAMPAGE.ICN`; only `SYSTEM.CNF` differs (`BOOT2 = cdrom0:\SLUS_213.23;1`, `VER = 1.01`, `VMODE = NTSC`). When
`SYSTEM.CNF` (in the image, or beside the VOL) boots another ELF or another version, or the VOL's index is not theirs
(352 files ending at byte 1,342,827,563), the viewer says so and goes on. Beside a `RAMPAGE.VOL` the ELF may be
`RAMPAGE.ELF`, `SLES_538.00` or `SLUS_213.23`; fx.c finds its word table at 0x3B1C10, or looks for it in another build.

The region is `SYSTEM.CNF`'s, as the game takes it (gApp.getRegion, 0x10D290: `SLUS` 0, `SLES` 1, else 2; iso.c
`data_region`): the USA disc plays as on an NTSC console, the movies without `_pal` (`midway.pss` at 30 frames a
second), and its English shell has the Ballers box in the Midway Classics (SHELL_NOTES "Bonus Content"); the European
disc as on a PAL console. The game's own steps are the NTSC ones either way (30 a second, FLOW_NOTES).

Keys: W A S D move, Q E down and up, Shift faster, right mouse button to look around, the wheel changes the speed;
Page Up / Page Down the block, 1-7 the city, N day or night, F fog, H the armed attack spheres (off at the start), M the next monster (none, then all 30), K / L
the monster's clips, G play or the free camera, F5 .. F9 the graphics settings and F11 or Alt+Enter fullscreen ("Graphics
settings"), F12 a screenshot, Esc quit (in a game: the Pause menu; in the menus: back), Alt+F4 quit as the window's close. In play (the default with `--monster`)
the keys are a pad: W A S D the left stick, the arrows the right stick (the head), Space Cross (jump), J Square
(punch), K Triangle (kick), L Circle (grab), R R2 (roar), P or Enter Start (the Pause menu: the arrows choose, Enter
or Space take, Backspace back); player 2 (`--players 2`) plays on the numeric keypad, Num Lock on: 8 4 2 6 (or 5)
the left stick, 0 Cross, 7 Square, 9 Triangle, 1 Circle, 3 R2, / and * the right stick, and in the menus the same
keys are its pad (8 2 4 6 the d-pad, 0 Select, 1 Back: player 2's card in Monster Select); the monster runs its own
move graph and the game's camera follows it
([CAMERA.md](CAMERA.md)). `--clip N` picks a clip by number (and starts with the free camera), `--time T` poses a
`--shot`; in play a shot first lets the camera settle for 2 s, then plays `--do "right 1, cross, wait 0.8"` (stick
directions held and buttons pressed, each followed by that many seconds; `at x y` puts the monster there facing the
buildings; `pause` opens the Pause menu and `menu down` / `menu accept` / ... drive it (and the results screens:
`menu b0` / `menu b1` their Change Monster and Save; keys C and V); `award Name` counts an object of
that template for the block's challenge; `p2` (`p1` .. `p4`): the words after it play that player's monster, the
other players on keys idle meanwhile; `--walk S` is `right S`). `--players N` (1 .. 4) plays N monsters, the last
`--bots B` of them the computer's; `--monster2 Name` .. `--monster4 Name` pick players 2 .. 4's monsters (without
one: George, Lizzie, Ralph or Boosy, the first not taken). Controllers play too ("Input"): player 1 also on the
first one, player 2 on the second, players 3 and 4 on the third and fourth; without one, players 3 and 4 and the bots
are computer-driven (below, "Several monsters"); the front end (`--shell`) hands its players, bots and monsters over the same way. `--cam x y z yaw pitch` (degrees) draws the shot from a fixed view. `--play kingcity|kingworld|timed` plays another mode than the campaign
(its results screens). `--lang N` picks the texts' language (the .loc column: 0 English, 1 English 2, 2 Japanese,
3 French, 4 German, 5 Italian, 6 Spanish, 7 Dutch, 8 Portuguese; without it `rampage.cfg`'s `language=`, the console's language as the game
picks its column from it, `console_language` [R 0x10D300]: English 2 on the European disc, English on the USA one;
see "Graphics settings"); `--card-free-kb N` the "memory card"'s free room
(a save needs 140; else the drive's). The profile (profile.c, [PROFILE_NOTES.md](PROFILE_NOTES.md)) is read from and written to
`rampage_profile.bin` (`--profile FILE` another one, `--newprofile` a new one; a `--shot` run starts new and writes
nothing unless `--profile` is given): a new profile has six monsters and none of the four upgrade moves, which the
upgrade challenges teach; `--cheat 011235` gives every move. With the Wii's data (`wii_data=`) the file stays the
PS2's, its monsters by name, and what it has no room for (the Wii's monsters, their picks) goes to
`rampage_profile.wii.bin` beside it (profile.h, docs/WII_PLAN.md). `--shell` starts in the front end (shell.c, [SHELL_NOTES.md](SHELL_NOTES.md)): the
autosave notice, the movies (the logos and the intro, each press of Enter, Space, P or the pad's A / Start skips one, a tap on the movie on Android; the
Victory movie and the credits after the campaign), the profile popup (one profile, its file; letters and digits type
the name, Enter is DONE; it opens from Back on the main menu, "Profiles", and autosave is on once a profile is
loaded from it or the Results' Save is pressed), the main menu, players, monster select, city select, the loading screen, then the game it chose; the popup shows the
memory card's CHECKING / SAVING / LOADING and its errors for the profile file (one that cannot be written or read, a
corrupt one, a full drive), and the SaveIcon while an autosave writes, in the menus and over the Results; the Pause menu's Quit
goes back to the main menu. Its keys: the arrows, Enter / Space accept, Backspace back, P Start, C / V / R the
square, triangle and R2 buttons, F2 the cheat entry. With `--shot` the `--do` words drive it first (`wait S`,
`until NAME` (frames until that interface has the focus: `MainMenu`, `MonsterSelect`, `MonsterSheet`, ...; the boot's
movies run on the clock, so a script waits for its menus this way),
`menu up|down|left|right|accept|back|start`, `button N`, `type TEXT`, `done`; `p2` .. `p4` the menu words after it from that player's pad, `p1` again player 1's), the words left once a game
starts play in it, and those left once it goes back play on in the shell. After 120 s untouched in the menus the
attract mode runs its movies and then a demo game: a random city, block, day or night and one or two monsters, all
the computer's, for a minute (the 65 demos come in turn, 4 first; "Press START button" blinks); any key or
controller button that is a pad button (Enter, Space, Backspace, P, J, K, L, R, the keypad's, the d-pad and the
buttons; `pause` or a button word in `--do`) ends it for the main menu, the minute's end goes back to the attract
mode's movies and the next demo. `--do "wait 1, menu accept, wait 600, wait 8"` shoots the first one (the boot's
and the attract mode's movies play their length). The console prints
what each block holds (models, people, vehicles, anything not found) and, in play, every state the monster enters
(`RAMPAGE_TRACE=1`: every frame's state, time, place and heading; `RAMPAGE_HANDS=1`: the hands' grips each frame;
`RAMPAGE_NOIK=1`: the arms as the clips have them; `RAMPAGE_NOTAIL=1`: the tails as the clips have them, without
their springs; `RAMPAGE_MAGENTA=1`: a magenta background, to show holes in the
world). The fog is on (F toggles it, `--nofog` starts without). Texture packs: PNGs in `mods/textures/` replace the
game's textures, `--dumptex` writes every texture drawn to `mods/dump/` under the name a replacement takes
([TEXTURES.md](TEXTURES.md)). `--cheat` takes several codes, comma separated (`--cheat 986960,071767`).

### Autoplay (the playthrough test)
```bat
RampageRE-dev.exe build\disc\RAMPAGE.VOL --shell --autoplay --shot out\end.png --mute          :: the campaign, front end to credits
RampageRE-dev.exe build\disc\RAMPAGE.VOL Chicago 1 --autoplay --ap-cities 1 --shot out\c.png --mute   :: one city
python tools\playthrough.py out                  :: the whole suite, in parallel, out\report.md (--suite ci: one city, 2 blocks)
```
`--autoplay` (with `--shot`: offscreen, simulated time, 60 frames a second, about 30 times real time) plays the game
with nobody at the pad, to find what breaks; it is a test of the port, nothing of the game's. Every monster is the
computer's (`DRIVE_COMPUTER`: the bots' DestroyBuildings goal, director.c, for players and bots alike; the game still
counts them as players and bots as given), the results screens answer themselves (`hud_results_auto`, Continue on
Game Over), the SaveGame popup's messages are taken, and the shell is driven by an autopilot (main.c
`autopilot_shell`, a press every 0.4 s at most, reading the menus through `shell_auto_view`): the boot's notice, the
main menu to the `--play` mode and the `--players` / `--bots` asked for (the PS2's boxes: 1 or 2 players; up to 4 outside the campaign with `four_players`), every
card readied, the city select to `--ap-city` or the campaign's next city (the last one open and not destroyed), the
Victory's movies and the credits played their length. Started in a city instead of `--shell`, the shell takes over
when the city is left, as after a game it chose. It stops at the main menu after a game (the campaign's credits, a
quit), at `--ap-cities N` games left, `--ap-blocks N` blocks ended, `--ap-demos N` demos (with `--shell`: no
presses in the menus, the attract mode comes after its 120 s) or `--ap-minutes M` simulated minutes (600).
`--ap-seed S` gives the world's seeds and the bots' (several seeds, several runs).

The watchdog (src/autoplay.c) writes to `--ap-log FILE` (else the shot's name + `.log`), a line each, key=value:
`BLOCK` per block ended (city, block, play type, its time, won / timeup / gameover / left, the points, lives, HP,
states, the boss, the buildings standing, the objects' most, the memory), `EVENT` (the shell's screens, a game's
start and end, a city or the world done), `PROBLEM` (a block never ending: its buildings felled to go on, `(forced)`;
the same block lost three times running: its next try felled 30 s in; a results screen or a game state that never
moves on, a shell screen standing for 5 minutes (15 with a movie); a monster's place not a number, a monster not
moving for 90 s, outside the street, more than 4 under its ground or inside a standing building for 2 s, down at 0
HP for 90 s; the live people or an object list past its bound; more than 3 GB, or 500 MB more after ten blocks),
`CRASH` (the exception or signal, its place in the exe and the last second's state; on Windows then a
`CRASH_FRAME` line per frame of the stack, by dbghelp and the `RampageRE-dev.pdb` beside the exe; `RAMPAGE_AP_CRASH=1`
makes one 5 s in, to try it) and `SUMMARY`. A `BLOCK` line also counts the buildings the bots' 60 s guard felled
(`guard_falls`: the bots stuck on a building). Screenshots go to `--ap-shots DIR`: one
per problem (80 at most) and every fifth block's end. The exit code is 3 when it saw problems.

`tools/playthrough.py OUT` runs the suite (the campaign from the front end per seed and with the cheats 986960 +
071767, each city alone per seed, King of the City through the menus and with four players, King of the World, the
Timed Run through the menus and alone, two and four players in the campaign, six demos), each run's files in OUT
only (its log, console, screenshots, profile), and writes OUT\report.md and report.json: a table per run (how it
ended, blocks won / ended, simulated minutes, wall seconds, peak memory, problems), each block, each problem, and
the console's lines that look wrong ("not found", errors, `no ...`), counted. `--suite monsters` runs each monster of
the disc on its own (four of it in Chicago, three blocks, `RAMPAGE_TRACE=1`) and checks the trace against its
MONSTER_DATA (FORMAT_MONSTER.md): the HP before a hit never past +0x138 and once equal to it, the blocks' HP likewise,
and every blow between them one of its attack records' damage (+0x2C); a failed check is a `MONSTER CHECK` line.

### Windows builds
`build.bat` (after WoodyRE's) builds, with Zig, from the same SRC list:

| | |
|---|---|
| `build.bat` | `RampageRE.exe`, the player build (`-DRAMPAGE_GUI -Wl,--subsystem,windows`, the icon and version of `res/rampagere.rc`): no console; started without arguments it opens the front end (`--shell`); stdout and stderr go to `rampage.log` in the current folder (a new one each start, started again past 32 MB; `RAMPAGE_CONSOLE=1` keeps the streams, a `--shot` run too), the window title stays plain (`RAMPAGE_DEBUGTITLE=1`: as RampageRE-dev.exe's); a message box when it cannot start; started from a read-only folder (Program Files) or a temporary one (from inside a zip) without arguments it works in `%LOCALAPPDATA%\RampageRE` |
| `build.bat dev` | `RampageRE-dev.exe`, the developer build: a console, the same command line; what this document and the tests use (`build_viewer.bat` calls this) |
| `build.bat standalone [DATA] [WII]` | `RampageRE.exe`, then `RampageRE-standalone.exe` (`make_standalone.bat`, below) |
| `build.bat apkbundle` | `build\apkbundle.exe`, the tool of `make_android_bundle.bat` ("Android") |
| `build.bat tools` | only `build\bundle_stub.exe`, `build\pack.exe` and `build\apkbundle.exe` (the CI builds them) |

Both exes ask for the disc image at a first start without data ("Game data"), never under `--shot`, which keeps every
run of either offscreen and silent: no window, no message box, no question, messages on stderr.

**The standalone exe** (`make_standalone.bat [disc image or RAMPAGE.VOL] [Wii folder]`, WoodyRE's): `tools/native/pack.c` appends
`RampageRE.exe` and the disc's files the port reads (`RAMPAGE.VOL`, the ELF as `RAMPAGE.ELF`, `SYSTEM.CNF`: 1.35 GB),
taken from the disc image, from beside the VOL, or (no argument) from the data `RampageRE.exe` finds, to the launcher
`tools/native/bundle.c`. At its first start it unpacks them into `%LOCALAPPDATA%\RampageRE` (a progress window; the free
room is checked first; each file is written as `NAME.part` and renamed when whole; `.bundle` lists what is there by
size and FNV-1a hash, so a newer bundle rewrites only what changed: a new `RampageRE.exe` costs 1.7 MB, not 1.35 GB),
then starts `RampageRE.exe` there with `RAMPAGE_VOL` set to that VOL and its own command line, so `rampage.cfg`, the
profile, `rampage.log` and `mods\` live in that folder. The payload's offsets are 64-bit (the file APIs too); the
packer refuses a result of 4 GB or more (what Windows runs as an exe); the 1.35 GB one starts as any exe. With `--shot`
in its command line it shows nothing (stderr only); `RAMPAGE_STANDALONE_DIR` unpacks elsewhere (testing). It holds the
game's data: for your own use only, never share or upload it.

**The Wii's data in the standalone exe** (optional, docs/WII_PLAN.md): a folder dragged onto `make_standalone.bat` with
the disc image is your extracted Wii disc (the folder with `DATA`, its `DATA\files`, or the folder with `Game` in it);
without one `pack` takes `RAMPAGE_WII_DATA`, else the `wii_data=` of the `rampage.cfg` beside the script, else a `wii`
folder there, else none (`tools/native/wiifiles.h`, shared with the Android bundle). The files of its `Game`, `Shell`
and `Audio` (379, 315 MB) go in as `wii/Game/NAME` ..., and the exe is about 1.6 GB. The launcher unpacks them into
`%LOCALAPPDATA%\RampageRE\wii` with the rest (the index takes folder/name now, plain parts only) and, when the bundle
holds them, starts `RampageRE.exe` with `RAMPAGE_WII_DATA` that folder, unless `RAMPAGE_WII_DATA` or that folder's
`rampage.cfg` (`wii_data=`) names another or none; a standalone without them leaves a `wii` folder of an earlier one
unused. Tested with `build/rw.exe` in it (`RAMPAGE_STANDALONE_DIR`): Dallas 4 with Boris byte for byte the shot of
`--wii` on the folder; a second start unpacks nothing; `wii_data=` in that `rampage.cfg` and `RAMPAGE_WII_DATA` (none,
or the folder) win; one without the Wii's over the same folder plays the PS2's (Chicago for Dallas, its shot byte for
byte the plain run's); CI's fake-VOL standalone test as before.

## Platforms
As in WoodyRE (its docs/PLATFORM.md): the Windows build keeps its own Win32 + WGL window, and everything else runs on
SDL2 behind the same functions (`win_*` in render_gl.h, `plat_*` in plat.h). Keys are the Windows virtual-key codes
on every system, so main.c has one spelling of them.

| | Windows (`build.bat`) | Linux (`build.sh`) |
|---|---|---|
| Window, GL context, keys, mouse | render_gl.c (`#ifdef _WIN32`): Win32 + WGL | plat_sdl.c: SDL2; SDL scancodes to VK codes |
| Offscreen frame (`--shot`) | a window never shown, the frame in a framebuffer object (`gl_offscreen`, render_gl.c) | `SDL_WINDOW_HIDDEN`, never shown, the same framebuffer object; without one, no window at all |
| Files, messages, free room | plat.h: `<windows.h>`, MessageBoxA, GetDiskFreeSpaceExA | plat.h / plat_sdl.c: `fopen` looks paths up ignoring case and with `\` as `/` (so `build\disc\RAMPAGE.VOL` works), SDL message box, statvfs |
| Sound output | sound.c: waveOut (winmm.dll loaded at run time) | sound.c: SDL2's audio queue (`SDL_QueueAudio`), pushed from the main thread as waveOut is |
| Renderer | render_gl.c: OpenGL 1.1 fixed function, the same on both | the same; `GLES=1`: on OpenGL ES through src/gles (below) |

A `--shot` run opens no window, no sound and no message box anywhere: a message (a missing `RAMPAGE.VOL`) goes to
stderr and the run exits with 1. A shot from the disc image is byte for byte the one from the extracted files.

On Linux (Debian / Ubuntu; WSL2 too):
```sh
sudo apt install build-essential libsdl2-dev libgl-dev
./build.sh                                          # ./rampage (CC=clang ./build.sh for another compiler)
./rampage ~/Rampage.iso                             # the front end (so does ./rampage alone, with the data found)
./rampage build/disc/RAMPAGE.VOL Chicago 1 --monster George
SDL_VIDEODRIVER=offscreen ./rampage build/disc/RAMPAGE.VOL Chicago 1 --mute --shot s.png   # no display needed
```
`SDL_VIDEODRIVER=offscreen` is SDL's EGL pbuffer driver: it needs no X or Wayland server (CI, WSL without WSLg).
Linux's `rampage` is the player build there too: started alone or with only the data's path it opens the front end,
as `RampageRE.exe` does; given a city (or any option) it is the viewer, as `RampageRE-dev.exe`. On the Steam Deck add it to
Steam as a non-Steam game (*Add a Non-Steam Game*, the disc image's path as its launch option), so that in Game Mode, or
started from Steam in Desktop Mode, Steam Input hands its controls over as a pad.
The Wii's extra content ("Graphics settings", `wii_data=`): a `wii` folder with the extracted disc's `Game`, `Shell` and
`Audio` in the folder it is started from (Steam's *Start In*, the program's folder unless changed) is used on its own,
as on the Switch; nothing asks for it.
Pass the disc image (`./rampage ~/Rampage.iso Chicago 1`), or put it (or `RAMPAGE.VOL` with `SLES_538.00` or
`RAMPAGE.ELF`) in `~/.local/share/RampageRE` ("Game data" above). A shot on Mesa's llvmpipe differs from the Windows one only in rounding (under 0.1 % of the pixels by more
than 8 of 255).

### OpenGL ES
`GLES=1 ./build.sh` (needs `libgles-dev` instead of `libgl-dev`) builds the same program on OpenGL ES 3.0, or
2.0 where there is no 3.0: the path a Switch or Android build takes, testable on a PC (`SDL_VIDEODRIVER=offscreen`
gives Mesa's ES 3.2 with no display). It adds `-Isrc/gles` (so `#include <GL/gl.h>` is `src/gles/GL/gl.h`),
`src/gles/gles2.c`, `-lGLESv2` instead of `-lGL`, and `-fsigned-char` (ARM's `char` is unsigned). plat_sdl.c asks
for an ES context and hands `plat_gl_proc` to the shim (`gles_proc`). The renderer does not change: the shim, after
WoodyRE's, emulates the fixed function it uses with one shader pair whose stages uniforms switch:

| | |
|---|---|
| matrices | modelview, projection and texture stacks; `glGetFloatv(GL_MODELVIEW_MATRIX)` |
| lighting | up to 4 directional lights per vertex: `GL_POSITION` taken into eye space under the modelview of the call, `GL_DIFFUSE`, `GL_AMBIENT`, the light model's ambient, `GL_COLOR_MATERIAL` (ambient and diffuse), `GL_NORMALIZE`, `glNormalPointer` / `glNormal3f` |
| fog | `GL_LINEAR` (and `GL_EXP`, `GL_EXP2`) at the eye-plane distance; `glGetFloatv(GL_FOG_COLOR)` answers |
| textures | `GL_SPHERE_MAP` texgen (the reflection layers), the texture matrix (the drifting clouds), modulate / replace / add / decal / combine, the alpha test, `GL_CLAMP` as `GL_CLAMP_TO_EDGE` |
| drawing | client arrays as attributes, `GL_UNSIGNED_INT` indices (16 bit or gathered on a bare ES 2.0), `glBegin` / `glEnd` (quads as triangles, line loops), `glPushAttrib` / `glPopAttrib` by group, `glPushClientAttrib`, `glShadeModel` and `glPolygonMode` as no-ops, `glReadPixels` into RGB |
| shader passes | an own program (postfx.c, gtao.c) passes straight through, the shim's comes back after `glUseProgram(0)`; their GLSL 1.20 / 1.30 is taken as ES 1.00 / 3.00 (highp, `gl_Vertex` = attribute 0, `gl_FragColor` an output), gtao.c blits the depth (ES copies none), and the desktop framebuffer object names are ES 2.0's core ones |

Not there (the renderer uses none): point and spot lights, specular, `glMaterial`, the other texgen modes. On ES 2.0
GTAO, SMAA and MSAA report "not supported" (no blit, no GLSL 3.00). Shots on llvmpipe, ES against desktop GL: a
mean difference of 0.0001-0.0006 per channel (alpha-tested edges, the movie's clamped edge), the same with `--ao`,
`--smaa`, `--msaa`; ES 2.0 and 3.0 give the same picture.

### Nintendo Switch
Homebrew, for a Switch that runs custom firmware such as Atmosphère; experimental and **not yet tried on a Switch**.
`./build_switch.sh` builds `rampage.nro` with devkitPro's devkitA64, libnx, switch-sdl2 and switch-mesa
(`dkp-pacman -S switch-dev switch-sdl2 switch-mesa`); without `$DEVKITPRO` it runs itself in the official
`devkitpro/devkita64` Docker image, which has all of them. It is the OpenGL ES build above: the SRC list of
`build.sh` (keep the scripts in step) with `src/gles/gles2.c`, `-D__SWITCH__ -fsigned-char`, linked
with `-lSDL2 -lGLESv2 -lEGL -lglapi -ldrm_nouveau -lnx`, then `nacptool` and `elf2nro` with `res/switch_icon.jpg`
(the ELF, map and NACP stay in `build/switch/`). CI builds it (the `switch` job, in that image) and a release has
`RampageRE-switch-<tag>.zip`, laid out as on the SD card.

| | |
|---|---|
| Where | `/switch/rampagere/rampage.nro` on the SD card, started from the Homebrew Menu. That folder (`sdmc:/switch/rampagere`) is made at start and becomes the current one |
| Game data | in the same folder: your disc image (`*.iso`, read in place as on a PC) or `RAMPAGE.VOL` with `RAMPAGE.ELF` or `SLES_538.00`; `data_find` looks there first (`__SWITCH__` only). `RAMPAGE.VOL` is 1.34 GB; an image larger than 4 GB does not fit on a FAT32 card (exFAT, or copy the two files out of it) |
| The Wii's data | optional (docs/WII_PLAN.md): the `Game`, `Shell` and `Audio` folders of your extracted Wii disc (Dolphin's Extract Entire Disc, under `DATA/files`; 317 MB) in `sdmc:/switch/rampagere/wii`, found on its own as the game's data is; `wii_data=` in `rampage.cfg` points elsewhere |
| Files | `rampage.cfg`, `rampage_profile.bin` (the save), `mods/textures/` and `rampage.log` there: stdout and stderr go to the log, a new one each start. A message (no game data, another release) shows in the system's error dialog |
| Start | without arguments the front end (`--shell`); the Pause menu's Quit goes back to its main menu, HOME closes the program |
| Screen | the whole screen at 1280×720 (docked the system scales it up); `window=`, `fullscreen=` and F11 do nothing, the effects of `rampage.cfg` work (ES 3.0) at a cost not measured |
| Pads | through `pad_sdl.c`, see below |

**Controls.** The Joy-Cons attached (handheld), a detached pair, a Pro Controller: SDL's Switch mapping goes by place,
so B (the bottom button) is Cross (jump), A Circle (grab), Y Square (punch), X Triangle (kick), ZR R2 (roar), ZL L2,
R / L R1 / L1, + Start, − Select, the sticks the sticks. Up to four players, one controller each, in the order the
system numbers them: SDL lists all eight of the system's players whether a controller is there or not, so
`pad_sdl.c` asks libnx (`hidGetNpadStyleSet`) which are, and only those take a slot (players 3 and 4 without one are
the computer's, as on a PC). A single Joy-Con held sideways, when the system makes it a player of its own (in its
controller screen, which SDL opens when the controllers change, SL + SR on the Joy-Con): its four buttons by place
as above, SR R2 (roar), SL L2, pressing the stick Start; SDL hands its stick over as the d-pad, which `pad_sdl.c`
turns into the left stick (no right stick: the head is not turned). Not done: making single Joy-Cons the default
from the program (libnx `hidSetNpadJoyAssignmentModeSingleByDefault`), since SDL's Switch driver opens the
controller screen on every change of style. A USB keyboard plays as on a PC; the touch screen does nothing (no
mouse). Rumble goes through SDL's Switch driver (one strength, on or off).

**Memory.** The viewer reads what it needs from `RAMPAGE.VOL` into memory. Peak resident size of the Linux GLES build
(`/usr/bin/time -v`, `SDL_VIDEODRIVER=offscreen`, Mesa llvmpipe, whose textures are in that memory too):
`--players 4 --bots 2` on block 1 and 6 of each city 187 - 214 MB (New York 6 the largest), 222 MB after a
minute's play, 246 MB with `--ao --smaa 4 --mips`, the front end 148 MB. On the Switch the GPU's memory comes out of
the same heap, with the 16 MB program and the driver's own on top. Homebrew in applet mode (started from the Album)
gets roughly 400 MB, which should hold it with little room, so start it with title takeover: hold R while starting
a game, which gives the Homebrew Menu the game's memory (about 3 GB). Started as an applet, `rampage.log` says so in
its first line.

**Untested** (no Switch here): that it starts at all, the speed of Mesa's nouveau driver on the Tegra X1 with the
shim (four lights per vertex, one shader for everything), the error dialog, the log, sound (SDL's audio), rumble,
docked / handheld changes, several controllers and single Joy-Cons. The build itself is: the `devkitpro/devkita64`
image's devkitA64 (GCC 15.2), libnx 4.12, switch-sdl2 2.28.5 and switch-mesa 20.1 build `rampage.nro` (7 MB), GCC
warning only about shared code (collide.c, movie.c, shell.c, water.c), none in the Switch parts.

### PS Vita
Homebrew, for a PS Vita or PS TV with HENkaku / Ensō; experimental and **not yet tried on a Vita**.
`./build_vita.sh` builds `rampage.vpk` with VitaSDK, its SDL2 and vitaGL; without `$VITASDK` it runs itself in the
official `vitasdk/vitasdk` Docker image, which has all of them (from Git Bash on Windows too). It is the OpenGL ES build
above on ES 2.0: VitaSDK's SDL2 makes its GL contexts with vitaGL, which answers "OpenGL ES 2.0", so the shim takes its
ES 2.0 path (no GTAO, SMAA or MSAA). The SRC list of `build.sh` (keep the four lists in step) with `src/gles/gles2.c`,
`-fsigned-char -mcpu=cortex-a9 -mfpu=neon`, linked with `-lSDL2 -lvitaGL -lvitashark` and the system's stubs, then
`vita-elf-create`, `vita-make-fself`, `vita-mksfoex` (title ID `RMPR00001`, `ATTRIBUTE2=12`) and `vita-pack-vpk` with
the LiveArea images of `res/vita/` (made from `res/switch_icon.jpg` by `tools/mkvita.py`; the ELF, map, eboot.bin and
param.sfo stay in `build/vita/`). CI builds it (the `vita` job, in that image) and a release has
`RampageRE-vita-<tag>.zip` with `rampage.vpk`.

| | |
|---|---|
| Install | `rampage.vpk` with VitaShell; it starts from its LiveArea bubble. `ux0:data/rampagere` is made at start and becomes the current folder |
| Shader compiler | vitaGL compiles the shim's shaders with the system's own compiler, `ur0:data/libshacccg.suprx` (or `ur0:data/external/`), which is not shipped: install and run **ShaRKBR33D** once, which extracts it. Without it RampageRE says so at start |
| Game data | in `ux0:data/rampagere`: your disc image (`*.iso`, read in place) or `RAMPAGE.VOL` with `RAMPAGE.ELF` or `SLES_538.00`; `data_find` looks there first (`__vita__` only) |
| The Wii's data | optional (docs/WII_PLAN.md): the `Game`, `Shell` and `Audio` folders of your extracted Wii disc (Dolphin's Extract Entire Disc, under `DATA/files`; 317 MB) in `ux0:data/rampagere/wii`, found on its own as the game's data is; `wii_data=` in `rampage.cfg` points elsewhere |
| Files | `rampage.cfg`, `rampage_profile.bin` (the save), `mods/textures/` and `rampage.log` there: stdout and stderr go to the log, a new one each start. A message goes to the log and to SDL's message box, which may not show once vitaGL holds the screen |
| Start | without arguments the front end (`--shell`); the Pause menu's Quit goes back to its main menu, the PS button goes to LiveArea, which closes it |
| Screen | the whole screen at 960×544; `window=`, `fullscreen=` and F11 do nothing, the effects of `rampage.cfg` report "not supported" (ES 2.0) |
| Clocks | the highest at start: CPU 444 MHz, bus 222, GPU 222, its crossbar 166 (`plat_vita_home`) |

**Controls.** The handheld's own buttons through `pad_sdl.c` as a DualShock 4 would be: Cross jump, Circle grab,
Square punch, Triangle kick, L / R as L1 / R1, Start, Select, the sticks. The handheld has no L2 / R2: the rear touch
pad stands for them, its left half L2 and its right half R2 (roar). It has no L3 / R3 either. A PS TV should play with its
DualShock 3 / 4 as a pad (L2 / R2 / L3 / R3 included), up to four. The front touch screen does nothing (no mouse, no
on-screen pad). No rumble on the handheld.

**Memory.** `plat_sdl.c` gives newlib's heap 256 MB (`_newlib_heap_size_user`) and the main thread a 4 MB stack;
`ATTRIBUTE2=12` in param.sfo asks the system for the larger memory budget, and vitaGL takes the 128 MB of video memory
and what is left of main memory for textures and buffers, as TheFloW's ports on vitaGL do. The game needs 187 - 246 MB
on Linux with its textures ("Nintendo Switch" above); without them it should fit the heap, but four players on New
York 6 are the likeliest to run out.

**Untested** (no Vita here): that it starts at all; the speed (a Cortex-A9 at 444 MHz is far slower than the
Switch's A57: four players, cloth and the crowds are the question, as is the shim's one shader with four lights per
vertex on the SGX543 at 960×544); whether vitaGL's GLSL translator takes the shim's shaders; memory; the message box;
sound (SDL's audio); the rear touch as L2 / R2; a PS TV with several pads. The build itself is: the `vitasdk/vitasdk`
image's arm-vita-eabi GCC 15.2 and SDL 2.32.8 build `rampage.vpk` (1.9 MB), GCC warning only about shared code
(army.c, collide.c, main.c, movie.c, water.c), none in the Vita parts.

### Android
An app for phones, tablets and Chromebooks (Android 7.0 and up, arm64-v8a and x86_64), WoodyRE's `android/` renamed:
`io.github.jjmhalew.rampagere`. It is the OpenGL ES build above on SDL 2.32.10: `android/app/CMakeLists.txt` fetches
SDL's source (checked against its SHA-256) and builds `libmain.so` from the SRC list of `build_switch.sh` (keep them
in step) with `src/android.c` and `src/touch.c`, `-fsigned-char`, linked with `GLESv2 GLESv3 log android`; `main`
is compiled as `SDL_main`, which SDL's activity calls. 16 KB pages (Android 15) are aligned for.
```bat
:: Android Studio's JDK and SDK (with NDK 27.2.12479018, which Gradle installs when it is missing)
set JAVA_HOME=C:\Program Files\Android\Android Studio\jbr
set ANDROID_HOME=%LOCALAPPDATA%\Android\Sdk
android\gradlew.bat -p android assembleDebug       :: android\app\build\outputs\apk\debug\app-debug.apk
adb install -r android\app\build\outputs\apk\debug\app-debug.apk
```
(`./gradlew assembleDebug` in `android/` elsewhere; Gradle 8.13 comes through the wrapper, checked against its SHA-256.)
CI builds the release APK (the `android` job) and a release has `RampageRE-android-<tag>.zip` with
`RampageRE-<tag>.apk`. Signing: `-PrampageKeystore=... -PrampageKeyAlias=... -PrampageKeyPassword=...` (CI: the
secrets `ANDROID_KEYSTORE_B64`, `ANDROID_KEY_ALIAS`, `ANDROID_KEY_PASSWORD`); without them the APK carries a debug key,
and Android installs an update only over an APK with the same key: uninstalling first deletes the app folder, the
copied data and the save with it.

| | |
|---|---|
| App folder | `Android/data/io.github.jjmhalew.rampagere/files` on the shared storage (`getExternalFilesDir`; a USB cable reaches it, no permission is asked), the current folder from the start: `rampage.cfg`, `rampage_profile.bin` (the save), `mods/textures/` and the game's data; `data_find` looks there first (`__ANDROID__` only) |
| First start | without the data a dialog asks for **Disc image** or **Folder** and opens the system's file picker. A disc image (`.iso`) is not read in place as on a PC (a picked file's permission does not outlast the run): `RAMPAGE.VOL`, the ELF that `SYSTEM.CNF` boots (saved as `RAMPAGE.ELF`) and `SYSTEM.CNF` are copied out of it into the app folder once (1.35 GB; the free room is checked first, a progress dialog counts the MB, each file is written as `NAME.part` and renamed when its size is the image's, `RAMPAGE.VOL` last); the image can be deleted afterwards. A folder that holds `RAMPAGE.VOL` and `SLES_538.00` or `RAMPAGE.ELF` (and `SYSTEM.CNF`) has them copied the same way (`RampageActivity.java`). Or copy those files, or the image itself (then read in place), into the app folder by cable before the first start. Cancel quits |
| Start | the front end (`--shell`); the Pause menu's Quit goes back to its main menu. The app is left with Home, not quit |
| Screen | the whole screen in landscape (either way round); `window=`, `fullscreen=` and F11 do nothing, the effects of `rampage.cfg` need ES 3.0 |
| Messages | a dialog (`RampageActivity.dialog`, `plat_dialog`); stdout and stderr go to logcat: `adb logcat -s RampageRE` |
| Bundle | `make_android_bundle.bat` (below) makes an APK with the disc's files inside, and the Wii's if given; such an app reads them from its own APK (`getPackageCodePath`, `android_apk`) and neither asks nor copies |
| The Wii's data | optional (docs/WII_PLAN.md): the `Game`, `Shell` and `Audio` folders of your extracted Wii disc (Dolphin's Extract Entire Disc, under `DATA/files`; 317 MB) in the app folder's `wii`, found on its own as the game's data is (`wii_data=` in `rampage.cfg` points elsewhere). Copied there by USB cable, or picked and copied by the app (`RampageActivity.wiiSetup`): after the first start's copy it asks for them (**Folder** or **Not now**), and later the launcher's shortcut **Add the Wii's data** (a long press on the icon) asks again. The system's folder picker takes the extracted disc (the folder with `DATA`), its `DATA/files` or that folder's parent of `Game`; the three folders' files are copied into `wii.part` and renamed `wii` when all are in (a progress dialog counts the MB, the free room is checked first). Started from the shortcut the copy comes before the data is read; while RampageRE runs it copies at once and says the Wii's data counts from the next start |

**The APK bundle** (`make_android_bundle.bat [RampageRE.apk] [disc image or RAMPAGE.VOL] [Wii folder]`, WoodyRE's, with
`tools/native/apkbundle.c`): the APK of a release (or the one `android\` built: release, else debug) and the three
files the port reads (`SYSTEM.CNF`, the ELF it boots as `SLES_538.00`, `RAMPAGE.VOL`), taken from the disc image or
from beside the VOL (or from the data the PC build finds), become `RampageRE-bundle.apk`. Every entry of the APK stays
byte for byte at its offset (the 16 KB alignment of the native libraries holds: `zipalign -c -P 16 4` passes); the three
are added stored and 4096-byte aligned, so they are read in place in the installed APK: no second copy of 1.35 GB in
the app folder, and nothing to ask. Only these files go in, not the whole disc image (a DVD image of up to 4.7 GB): the
bundle is about 1.35 GB, below 2 GB, so it needs no ZIP64 (which the zip readers of older Android versions do not take)
and stays clear of signed 32-bit offsets in any tool on the way (adb, file managers, the installer, which copies the
whole APK into `/data/app`; the device needs that room once more while installing). Android sets no fixed size limit for an
APK installed by hand (the Play Store's limits are for its own uploads). The bundle is signed again with
APK Signature Scheme v2 (Android 7+, the app's minSdk 24; v3 would add only key rotation) by an RSA-3072 key made with
Windows' CNG at the first run and kept in `%LOCALAPPDATA%\RampageRE\android-bundle.key`, so later bundles install as
updates of it; it does not install over a release APK (another key): uninstall that first, which deletes its app folder
(copied data, rampage.cfg, the save) with it. `apksigner verify` takes it (v2), `aapt2 dump badging` reads it. It holds
the game's data: for your own devices only, never share or upload it. Tested here on an APK made with aapt2 and signed by
apksigner (v2 + v3), with the real 1.34 GB RAMPAGE.VOL: verified, aligned, and read as the data by the Windows and Linux
builds (`--shot` byte for byte the one from the VOL); not yet installed on a device or the emulator.

**The Wii's data in the bundle** (optional, docs/WII_PLAN.md): a folder dragged onto `make_android_bundle.bat` with
the rest is your extracted Wii disc (the folder with `DATA`, its `DATA\files`, or the folder with `Game` in it);
without one `apkbundle` takes `RAMPAGE_WII_DATA`, else the `wii_data=` of the `rampage.cfg` beside the script, else a
`wii` folder there, else none. The files of its `Game`, `Shell` and `Audio` (379, 315 MB) go in as
`assets/wii/Game/...`, stored and 4096-byte aligned as the disc's, and the bundle is about 1.6 GB. The app reads them in
place from its APK (`vol_add_wii` takes a ZIP as well as a folder, `iso.c` `zip_each`) when neither `wii_data=` nor the
app folder's `wii` names others; a bundle without them says nothing. On a PC `--wii RampageRE-bundle.apk` reads them the
same way. Tested: `apksigner verify` and `zipalign -c -P 16 4` pass; on Windows Dallas 4, three PS2 shots and the
shell's autoplay with `--wii` the bundle are byte for byte those with the Wii folder (the profile too, but for its save
time); installed on the emulator (Android 15 x86_64) with nothing in the app folder: the game's data and the Wii's from
`base.apk`, the roster of 40, Dallas 4 with Boris.

**Controls.** The on-screen pad (`src/touch.c`, WoodyRE's with the PS2 pad's buttons): a floating stick on the left
half of the screen (where the finger lands; move), on the right Cross (jump), Square (punch), Triangle (kick) and
Circle (grab) as on the PS2 pad, R2 (roar) above them and Start (pause) in the top corner. They are player 1's pad
(slot 0), so the menus take them as a controller's ("Input": Cross accepts, Circle goes back, Square / Triangle / R2
the pages' buttons); a finger may slide between the face buttons. The pad hides when a real controller's buttons or
sticks or a key are used and comes back at the next touch. Controllers (Bluetooth or USB) play through `pad_sdl.c` as
on Linux, up to four. The back button (or gesture) is Backspace in the menus (back) and Start in play (the Pause
menu). A tap anywhere on a movie (the logos, the intro, the attract mode's, Victory's and the credits) skips it as
Cross does (port extra, main.c `touch_tapped`); in the menus only the buttons count. Not there: the right stick (turning the head), L1 / L2 / R1, Select, and letters for the profile popup's name
entry, which types from a keyboard (an attached one works; there is no on-screen one yet).

**Testing.** `adb shell am start -n io.github.jjmhalew.rampagere/.RampageActivity -e args "'Chicago 1 --monster George'" -e env "RAMPAGE_TRACE=1;RAMPAGE_TOUCHLOG=1"`
passes a command line (instead of `--shell`) and environment variables. Tried on the SDK's emulator (Android 16,
x86_64, SwiftShader's ES 3.0, headless): the first-start dialog, the picker, the free-room check, the copy out of an
image, the front end and a game with the on-screen pad, the back button into the Pause menu. On Android 15 (x86_64,
16 KB pages): the folder copy, and the Wii's data: a `wii` put in the app folder (Dallas with Boris), the shortcut's
copy before the data is read (the extracted disc picked) and while running (its `DATA/files` picked), a folder
without `Game`, the question after the first start's copy and its Not now, `wii_data=none`. **Untested**: a real
phone or tablet (arm64, a real GPU's ES driver, its speed with four players, cloth and the effects), ES 2.0-only
devices, controllers on Android, rumble, sound output, a USB cable's copy (MTP) of the `wii` folder, a picker's cloud files (a file that cannot be
read where it is: copy it onto the device), an app sent to the background while copying, Chromebooks.

## Input
Keys as above, and today's controllers (port extra, WoodyRE's pad code: `src/pad.h`, `src/pad.c` on Windows,
`src/pad_sdl.c` elsewhere):

| | Windows (`pad.c`) | Linux and the rest (`pad_sdl.c`) |
|---|---|---|
| Sony | DualShock 4, DualSense, DualSense Edge as raw HID (hid.dll, setupapi; USB and Bluetooth, the Bluetooth reports with their CRC-32): no driver, no Steam | SDL's game controllers (its hidapi drivers) |
| Xbox and the rest | XInput (xinput1_4 / 1_3 / 9_1_0, loaded at run time), empty slots asked every 2 s and on a device change | the same SDL API |
| Nintendo | only what shows as XInput (Steam Input) | by place, not label (`SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS` 0): the bottom button is Cross |

**Players.** Up to four controllers, each in a slot of its own: a controller takes the lowest free slot when it is
plugged in (at start: in the order found) and keeps it until it goes, which frees the slot for the next one. Player 1
plays on the keys and the controller in slot 0, player 2 on the keypad and slot 1 (in the menus too), players 3 and 4 on slots 2 and 3
when those have a controller as the game starts (else the computer drives them, as before; bots are always the
computer's). Steam Input and DS4Windows show a Sony pad twice (its HID device and an XInput one): while a Sony pad is
open on Windows, XInput is not read (`RAMPAGE_PADXINPUT=1` reads both). Input and rumble pause while the window is not
in front. `RAMPAGE_PADLOG=1` prints every rumble.

**The mapping**, the PS2 pad's buttons in the slots its pack gives them [R table 0x3B18B0] (keys_pad's keys still
fill the same pad; buttons add up, the stick pushed further wins):

| PS2 | game variable (MV) | Xbox / SDL name | menus (shell, Pause, results) |
|---|---|---|---|
| left stick | LX, LY 0, 1 | left stick | a flick (0.6 out, again after coming 0.45 back): up / down / left / right, held as the d-pad; pad 0's x moves the sliders |
| right stick | RX, RY 2, 3 (the head) | right stick | |
| d-pad | UP .. RIGHT 4 .. 7 | d-pad | up / down / left / right (held: again after 0.5 s, then every 0.125 s; MenuEventHandler) |
| Start | START 8 | Start / Options / + | Start (in play: the Pause menu, from that pad) |
| Select | SELECT 9 | Back / Share / Create / − | |
| R2, L2 | R2 10, L2 11 | RT, LT (from a quarter of their travel) | R2 = button 4, L2 = button 3; L2 + R2 held on pad 0 alone: the main menu's cheat entry (F2 on the keys) |
| R1, L1 | R1 12, L1 13 | RB, LB | |
| Cross | CROSS 14 (jump) | A / Cross / the bottom button | accept |
| Circle | CIRCLE 15 (grab) | B / Circle | back (CommonGUI's `Controller_Back` is a circle) |
| Square | SQUARE 16 (punch) | X / Square | button 0 |
| Triangle | TRIANGLE 17 (kick) | Y / Triangle | button 1 |
| R3, L3 | R3 20, L3 21 | RS, LS | |

The sticks go through a dead zone of 0.35 per axis, the rest scaled straight to full, as the PS2 pack does it
(0x1A6050; the zone and curve its constructor 0x1A5B20 sets, which nothing changes). The pressures (variables 28, 29) are not read by the port's move graphs, so the
triggers' travel is not passed on. Each controller sends its menu events as its own pad (`shell_input_pad`,
`hud_input_pad`: the shell's player screens and the Pause menu's Options, pad 0's, tell them apart as the game does);
the held state (`shell_pad_held`, `hud_pad_held`: getController(0)) is pad 0's and the keys'.

**Rumble**, where the game makes it (PS2ControllerPack vtable +0x28 = 0x1A5020 (pad, ms): with the vibration on, the
big motor at 0xC0 of 255 and the small one off [R]); here the low-frequency motor at 0xC0 / 255 of the player's
controller, never for a computer-driven monster (+0x108):

| What | ms | Where (port) |
|---|---|---|
| a hit on the monster, level 1 / level 2 (0x2D8BE4 / 0x2D8C14, in 0x2D6A50's hit) | 400 / 800 | `monster_hit` |
| a landing from the air (0x2DC9F4, in 0x2DC450): dead, asked only in mode 1 or 2 (0x2DC9C4) after the landing has set mode 4 (0x2DC5DC) | (400) | none |
| the front of the building the monster stands on (0x2D6150) or climbs (+0xC640 while +0xC644) has cracked, the shake before the fall (0x2BD9A0, in 0x2BD7D0) | 2000 | `play_step`, around `scene_buildings_update` |
| `gCommon.rumble(250)` (0x2C34C8; Options.scr / Pause.scr when the vibration is turned on), pad 0 | 250 | `HUD_CALL_RUMBLE_PULSE`, `SHELL_EVT_RUMBLE_PULSE` |

The vibration is `gCommon.setEnableRumble` (0x2C34AC, the pack's vtable +0x34; off while the Pause menu shows, off
with cheat 082864): `HUD_CALL_RUMBLE`, `SHELL_EVT_RUMBLE` and the settings' saves set it, and it scales every motor at
once. Light: the light bar takes the player's colour (PlayStation's blue, red, green, pink for players 1 .. 4) and the
DualSense its player lights; SDL builds set the player index, which does the same (`RAMPAGE_PADNOLIGHT=1` leaves the
lights alone). At exit the motors stop and the DualSense's light goes back to the system's.

**Tests.** A `--shot` run never opens a controller (no light, no motor, nothing read): the `--do` words drive the
monsters' pads directly, so every shot is the same with and without controllers attached.

## Graphics settings
Port extras, WoodyRE's (its docs/DISPLAY.md §3, 5, 6): the PS2 draws its GS frame without antialiasing, ambient
occlusion or texture filtering beyond its own, so every effect is **off by default**, and with all of them off the
picture is exactly the one without them (the same bytes in a `--shot`). `src/settings.c` reads them, `src/postfx.c`
(MSAA, SMAA), `src/gtao.c` (ambient occlusion) and `render_gl.c` (textures, window) do them.

| Setting | `rampage.cfg` | Flag | Environment | Key | What it does |
|---|---|---|---|---|---|
| window size | `window=1280x720` | `--window WxH` | `RAMPAGE_WINDOW` | | the inside of the window, centred on the screen (shrunk to fit, its shape kept); a `--shot` is drawn at that size too |
| fullscreen | `fullscreen=0` | `--fullscreen`, `--windowed` | `RAMPAGE_FULLSCREEN` | F11, Alt+Enter | borderless over the whole monitor at the desktop's resolution (no mode switch) |
| vsync | `vsync=1` | `--vsync 0\|1` | `RAMPAGE_VSYNC` | | the swap interval (`wglSwapIntervalEXT`, SDL's) |
| ambient occlusion | `ao=0` | `--ao`, `--noao` | `RAMPAGE_AO` | F5 | GTAO: contact shadows where surfaces meet (building bases, ledges, under the monster's feet) |
| its radius | `ao_radius=4` | `--ao-radius R` | `RAMPAGE_AO_RADIUS` | | how far (world units; a monster is about 25 tall) a surface looks for what hides the sky |
| SMAA | `smaa=0` | `--smaa N` | `RAMPAGE_SMAA` | F6 (cycles) | edge smoothing, 0 off, 1 .. 4 (or `low`, `medium`, `high`, `ultra`, SMAA's presets): polygon edges and the alpha-tested cut-outs (fire escapes, trees, facade sheets) |
| MSAA | `msaa=0` | `--msaa N` | `RAMPAGE_MSAA` | F7 (cycles) | multisampling, 0, 2, 4, 8 samples: polygon edges only |
| anisotropy | `aniso=1` | `--aniso N` | `RAMPAGE_ANISO` | F8 (cycles) | `GL_TEXTURE_MAX_ANISOTROPY` 1 (off), 2, 4, 8, 16 on the world's textures: streets and walls at a grazing angle stay sharp |
| the profile at launch | `profile_autoload=1` | `--autoload`, `--noautoload` | `RAMPAGE_PROFILE_AUTOLOAD` | | not graphics, but read the same way: see "The profile at launch" below |
| four players in the menu | `four_players=1` | `--four-players`, `--no-four-players` | `RAMPAGE_FOUR_PLAYERS` | | not graphics either: see "Four players in the menu" below |
| the Wii's extra content | `wii_data=` | `--wii DIR` | `RAMPAGE_WII_DATA` | | not graphics either: your own extracted Wii disc (Dolphin's Extract Entire Disc: the folder with `DATA`, or its `DATA/files`), for its ten monsters and Dallas; empty (the default) leaves the PS2 game as it is, except on the Switch, the Vita, Android and Linux, where a `wii` folder in the home folder is used when there is one (Linux: the current folder, beside its `rampage.cfg`; see each below; `wii_data=none` leaves it out, unsaid). `RampageRE.exe` (the player build, never `RampageRE-dev.exe` nor a `--shot` run) asks once when it is empty: a message (Yes / No), then the system's folder dialog, the folder checked for `Game` (in it, `DATA/files` or `files`); the answer, the folder or `none`, is written into `rampage.cfg` (`settings_put`: that line alone, the rest and its comment kept), so it asks no more; deleting the line asks again. Work in progress, docs/WII_PLAN.md |
| language | `language=english` | `--language NAME` | `RAMPAGE_LANGUAGE` | | not graphics either: the console's language (`english`, `french`, `german`, `italian`, `spanish`; `japanese`, `dutch`, `portuguese` as the game takes them, English), which the PS2 game reads at start-up (sceScfGetLanguage, 0x10D300) for its texts and movies (the intro and Victory have French, German, Italian and Spanish cuts); `--lang N` picks a .loc column itself |
| mips | `mips=0` | `--mips`, `--nomips` | `RAMPAGE_MIPS` | F9 | a 2×2 box mip chain (alpha-weighted, so cut-outs keep their colour) with trilinear filtering on the world's textures: no shimmer in the distance; anisotropy needs it to do much |

**rampage.cfg** is read from the current directory (next to `rampage_profile.bin`) before the window opens: one
`key=value` per line, `#` starts a comment, unknown keys are reported. The keys change the running session and print
the settings as `rampage.cfg` lines (`graphics: ao=1 ...`) and in the title, without writing the file.

**The Language row** (shell.c `op_language_build`, hud.c `pause_language_build`): the Options screen of the main menu,
and the Pause menu's Options, have a fourth row under Vibration, a copy of it: Language (its title in the texts'
language: Langue, Sprache, Lingua, Idioma), EN, FR, DE, IT, ES, the frame 40 taller below. The Options' Accept
writes another language to `rampage.cfg` (`language=`), Back leaves it. Each screen reads its texts as it loads, so
like the console's setting it counts from the next start.

**The Graphics page** (shell.c `op_graphics_*`, hud.c `pause_graphics_*`): the Options screen of the main menu, and
the Pause menu's Options, have a third button, Triangle **Graphics** (key V), next to Cancel and Accept. It opens a page in the same frame,
widened and made taller, whose rows are copies of the Vibration row: Ambient Occlusion, SMAA (off, low, medium, high,
ultra), MSAA (off, 2x, 4x, 8x), Texture Filter (anisotropy: off, 2x .. 16x), Mipmaps, Fullscreen and VSync. Up and
down choose the row (its title pulses), left and right the value, which is applied at once. On the page the button is
Square **Defaults** (key C): every row back to the defaults above (all off, VSync on), applied at once too. Accept keeps them and
writes them to `rampage.cfg` (`settings_save`: each key's line changed in place with its comment, the missing keys
added at the end, every other line left as it was; never in a `--shot` run). Back puts back what the page opened
with. Either way it returns to the Options. The window size and the AO radius stay `rampage.cfg`'s alone. Order: the
defaults, `rampage.cfg`, the environment, the flags. **A `--shot` run reads no `rampage.cfg`** (flags and environment
only), so test pictures stay the same whatever the file says; there the window, fullscreen and vsync keys do nothing (the
frame is the offscreen framebuffer object, F11 is a no-op for a hidden window), and the effects are drawn into the
shot (they hand the picture back to the framebuffer that was bound, not to the window's).

**Where they sit in a frame** (`rnd_frame`): with MSAA or SMAA on, `postfx_begin` binds an own target of the frame's
size before the clear (RGBA8 + depth 24 / stencil 8, multisampled for MSAA) and `postfx_end` resolves it, runs SMAA's
three passes (edges, blend weights with the AreaTex / SearchTex lookups, neighbourhood blending) and puts it into the
frame just before the HUD, so the HUD, the shell's pages and its 3D layers (drawn among its 2D quads) and the movies stay
sharp. The ambient occlusion runs after the opaque world, the monsters and the debris: the depth is copied into a
texture (with MSAA resolved by a blit), a GLSL pass integrates 3 slices × 6 steps per side in the form of XeGTAO, a 4×4
depth-aware blur multiplies `visibility^1.5` into the colour, faded out over the fog's range (the fixed-function fog is
already in the colour, so a fogged distance would be darkened otherwise). Blended parts (glass, the see-through
material colours, the parts drawn by their alpha) are held back while it is on and drawn after it in their order, so it
never darkens what shows through them; the street's own blended parts lie on it and are darkened with it. That changes
the draw order only while the ambient occlusion is on. The water, the decals and wall shadows, the particles and the HUD
come after it and are never darkened.

**Needs**: SMAA GLSL 1.30, the ambient occlusion GLSL 1.20, both and MSAA framebuffer objects of GL 3.0 or
ARB_framebuffer_object (with the blit and multisampled renderbuffers); anisotropy GL_EXT/ARB_texture_filter_anisotropic;
mips any GL. What the GL cannot do prints "not supported" once and stays off (an OpenGL 1.x driver draws as without
them); the keys stop at what the GL offers.

**Cost**, Intel UHD Graphics 770 (the CPU's own GPU), 1920×1080, Chicago 1 with George, per frame: 2.5 ms with everything
off; ambient occlusion +13 ms (about +5 ms at 1280×720; it grows with the radius, +10 ms at 1, +16 ms at 12), SMAA
medium +1.2 ms, ultra +2.4 ms, MSAA 4× +1.2 ms, 8× +3 ms, anisotropy 16× with mips +0, everything at maximum +20 ms. A
discrete GPU does the ambient occlusion in well under a millisecond (WoodyRE: 0.1 - 0.5 ms at 1080p on an RX 6800).
Golden `--shot` pictures only hold with everything off: the MSAA resolve and the shaders' rounding differ between
drivers (Mesa's llvmpipe against the Windows driver: about 0.1 % of the pixels by more than 8 of 255, as without them).

### The profile at launch
A port extra (`profile_autoload=1`, the default). The game itself starts every time with no profile: the main menu
comes up with saving off until the player opens Profiles and picks one (SHELL_NOTES.md "Profile list"). The port keeps
one profile, `rampage_profile.bin`, so with `profile_autoload` on:
- **The file is there and reads:** it is loaded at launch as if picked from the list (main.c: loaded, saving on, its
  checkpoint restored, and marked shown, so the main menu opens no list). Profiles still opens the list, to load it
  again or make a new one. A file that does not read is left to the game's way (its load shows the error).
- **No file:** the main menu's first focus opens the list's Create New Profile at once (the room test first), once a
  launch. Back goes to the list and the list's Back to the game's "WARNING: No Profile", so a game without a profile
  can still be played.

`profile_autoload=0` (or `--noautoload`) is the game's way. A `--shot` run reads no `rampage.cfg` and starts a new
profile unless `--profile` names one.

### Four players in the menu
A port extra (`four_players=1`, the default; `four_players=0` in `rampage.cfg` or `--no-four-players` is the PS2's
way). The PS2's main menu offers one or two players. Its menu code holds the other consoles' too (shell.c `mm_play_mode_select`, processPlayModeSelect [R]):
where `getPlatform()` is 1, 4 or 8 (the PS2 is 2) the player count list shows 3 Player and 4 Player as well, except
in the campaign. With the option on the port takes that branch: King of the City and King of the World offer 2 .. 4
players, the Timed Run 1 .. 4, the campaign 1 or 2 as on every console. The rest is the game's own 4-player flow (the bots'
box up to players - 1, four cards in the monster select, its `DisableFor4Player` monsters, the pads of every human
player in the menus). Players 3 and 4 play on the third and fourth controller ("Several monsters"). The PS2's
layout puts 3 Player and 4 Player on one place (y 113.5, under 41.5 and 77.5), so with the option on the shown items
are laid one under another a step apart (four of them a step higher, clear of the Cancel / Select bar). Autoplay's
`--players` goes up to 4 there too.

## Source
| File | |
|---|---|
| `src/data.h` | the formats: declarations and little-endian readers |
| `src/vol.c` | `RAMPAGE.VOL` (`tools/pvol.py`), from the file or in place from the disc image |
| `src/iso.c` | the disc image (ISO 9660, `tools/iso9660.py`) or an APK bundle (ZIP, stored `assets/`): a file by its path, read in place; the game's ELF; where the data is (`data_find`, the remembered `disc.txt`, the Windows question `data_ask`) and whether it is the supported release ("Game data"); the disc's files for the packers (`data_parts`) |
| `tools/native/bundle.c`, `pack.c`, `apkbundle.c` | not part of the program: the standalone launcher and its packer, the APK bundler and v2 signer ("Windows builds", "Android") |
| `src/bundle.c` | Pipeworks bundles, segment 0 (`tools/bdp.py`) |
| `src/tex.c` | textures to RGBA: PSMT8 unswizzle, CSM1 palettes, PSMCT32 / PSMCT16 (`tools/tex.py`) |
| `src/mesh.c` | meshes, strips to triangles (`tools/mesh.py`) |
| `src/prb.c` | property books read in place, with `ParentPropertySet` inheritance across bundles (`tools/prb.py`) |
| `src/tree.c` | node trees, their rest pose (RawAnim at time 0, or the skeleton's bind pose) and inverse bind matrices; cloth particles (`tools/anim.py`) |
| `src/anim.c` | clips: KeyInterpAnim tracks with the game's rotation decode (0x192010), RawAnim segments (`tools/anim.py`) |
| `src/city.c` | a block: ground, buildings with generated facades, objects, people, traffic, lighting (`tools/city.py`); a monster; posing and CPU skinning every frame |
| `src/camera.c` | the game's play camera, TwoTilts, transcribed from 0x312B50 (docs/CAMERA.md) |
| `src/building.c` | a building's cells and the game's hit on them: 0x2BC150, 0x2BB7B0, 0x2BB950, the roof stomp's 0x2BBE80 (docs/FORMAT_BDP.md "Damage") |
| `src/moves.c` | the monster's move graph (`MONSTER_DATA`) and the game's runner for it: 0x2EBFE0, 0x2EC3D0, 0x2ECBE0 (docs/FORMAT_MONSTER.md) |
| `src/render_gl.c` | Win32 window, WGL and OpenGL 1.1 drawing |
| `src/plat_sdl.c`, `src/plat.h` | the window, keys and OS bits on SDL2 outside Windows; what is taken from the OS ("Platforms"); the Switch's and the Vita's folder and log, the Switch's error dialog |
| `build_switch.sh`, `res/switch_icon.jpg` | the Switch homebrew `rampage.nro` and its icon ("Nintendo Switch") |
| `build_vita.sh`, `res/vita/`, `tools/mkvita.py` | the PS Vita homebrew `rampage.vpk` and its LiveArea images ("PS Vita") |
| `android/`, `src/android.c`, `src/touch.c`, `src/touch.h` | the Android app: Gradle project, `RampageActivity` (file picker, dialogs), SDL's Java glue; logcat, the app folder, the installed APK's path (a bundle's data) and the first start's copy out of the disc image; the on-screen pad ("Android") |
| `src/gles/GL/gl.h`, `src/gles/gles2.c` | the fixed function on OpenGL ES 2.0 / 3.0 (`GLES=1`, WoodyRE's shim with lighting, fog and sphere maps; "OpenGL ES") |
| `src/pad.h`, `src/pad.c`, `src/pad_sdl.c` | the controllers: Sony HID and XInput on Windows, SDL's game controllers elsewhere; a slot per player, rumble, lights ("Input") |
| `src/shot.c` | PNG screenshots (stored zlib blocks, no compressor) |
| `src/texpack.c` | texture packs: `mods/textures/` replacements and `--dumptex` (WoodyRE's, [TEXTURES.md](TEXTURES.md)); `src/stb/` stb_image and stb_image_write |
| `src/settings.c` | the graphics settings: `rampage.cfg`, `RAMPAGE_*`, the flags, F5 .. F9 and F11 ("Graphics settings") |
| `src/postfx.c`, `src/smaa/smaa.h` | MSAA and SMAA (WoodyRE's; `smaa.h` is SMAA by Jimenez et al., MIT) |
| `src/gtao.c` | the ambient occlusion, GTAO (WoodyRE's) |
| `src/autoplay.c`, `tools/playthrough.py` | `--autoplay`'s watchdog and log, and the suite that runs it ("Autoplay") |
| `src/main.c` | the viewer: arguments, camera, keys; the monsters in play (`Mon`), their pads and their world |
| `src/director.c` | the game flow's choreography: the AI's goals (playMove, walkTo, the bots' DestroyBuildings) and the camera modes (docs/FLOW_NOTES.md, MULTIPLAYER_NOTES.md) |

The game lights its city at run time: texture × (ambient + Σ colour × N·L) from the light file
([FORMAT_BDP.md](FORMAT_BDP.md) "Lighting"), which is OpenGL's fixed-function lighting, so the light file's ambient
colour and directional lights become `GL_LIGHT_MODEL_AMBIENT` and `GL_LIGHT0..2`. Meshes are drawn from both sides
with their normals as stored; texels with alpha below 0.5 are dropped, as in the glTF export, but for the
materials with flag 0x100000, blended by their alpha ([FORMAT_BDP.md](FORMAT_BDP.md) "Flag 0x100000"). The cloud band
scrolls by the `AnimatingSkyData` rate (per second, an assumption). Traffic drives the two lanes of the game's spawn
code (0x332D90) at 15 units/s with a fixed pick from the block's vehicle mix.

What moves: props whose own RawAnim moves (helicopter rotors, the fountain), every person playing the clip of the
state its template starts in, the traffic, and a monster (`--monster`, M): its skeleton's keyframed clips, its body
skinned on the CPU with its two bones per vertex, its hair or fur with the bones and weights of the cloth particle
each vertex sits on. Between rotation keys, and blending clips, the viewer slerps (the game's own interpolation,
0x104590, is not read yet); with `--clip` root motion stays in, so a walk walks off and jumps back when it loops.

In play the monster is driven the way the game drives it: the keys fill the pad's variable table (0x2C0AD0), the
move graph picks the state and its clip (the transitions in their input window, by priority, until their switch
window), the clip plays at its state's rate and blends in from the pose before it as the transition says (over the
new state's blend time, [FORMAT_MONSTER.md](FORMAT_MONSTER.md) "Blending"), and the clip's root carries the monster as
its node says: each frame the root's travel along the axes it names (the walk forward, a kick forward and sideways)
moves it, turned by its heading, and the rest stays in the pose; when a state whose node says so ends (flag
0x40000: the climbs, mounts, knock-downs) the place and turn its root reached become the monster's
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "Root motion"; the root's height is not taken yet). The stick is read in the camera's frame:
where a state allows it (`Ground_Walk`, the flip) the monster turns towards it at 6 × the angle per second (0x2E8190),
and in a jump it drives the monster at up to 16 units/s (the stick's own velocity, +0xA68)
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "State nodes", "Steering"). The clip's events act too: the movement mode, the
slot column and `Monsters.bsf`'s velocity calls (`addVelocity`, `jumpNorm`, ...); in the air the monster moves by
its velocity and falls by 60 units/s² (unless `disableGravity`), and meeting the street fires the landing slot (56)
in its column, so a held Cross jumps higher, a double jump flips, a butt slam lands in its impact
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "Movement", "Slots"). The people are their templates' scripts ported (src/people.c, [PEOPLE_NOTES.md](PEOPLE_NOTES.md)):
window occupants lean out and fall when their window breaks, crowds run, punks throw, tourists take photos; the
monster grabs one from a window (`canGrabPerson`) or the street (`doGrabObject` at its `Helper R Hand`), holds,
eats (health, energy) or throws them, and its attacks send them flying; its roar (R2, with roar energy) is the
game's RoarWeapon, a burst of 10 on everything within 10 of its mouth: people, cars, the army, the other monsters,
and the front cells of the buildings around ([ARMY_NOTES.md](ARMY_NOTES.md) "The monster's weapons"). Script checks the graph asks (`Monsters::canMount`,
`canGrabPerson`, ...) answer no, except the monster's own abilities (roar energy, double jump, jump smash, spin,
punch); as Monsters.bsf has them, a jump has one double jump (`hasDoubleJump` / `didDoubleJump`, the monster's +0xAB7)
and one spin, three with MULTI-SPIN, cheat 8 or the rage (`canSpin`, the count +0xAC0), both back once it is out of
the air (0x2E0C38). While a clip's attack event has a body part armed, its collision spheres are drawn as red wire spheres (H
hides them): the punch's fist, the kick's foot. The army comes for the monster (src/army.c, [ARMY_NOTES.md](ARMY_NOTES.md)): each block's `AIDef` spawns
helicopters, SWAT vans, army trucks with troops and tanks, which fire the `Weapons.bdp` weapons at it (hits take its
health and start its reactions), the punks throw their bottles, droppables fall from windows; blows wreck the
units, the monster grabs and eats troops, and buildings and kills score as the game's `Scoring` says
(`RAMPAGE_TRACE=1` prints the hits and the score). At 0 HP the monster takes slot 67 (`React_Defeated`), lies down, shrinks to 0.2
(`setScale`: setDynamicScale) and waits; a button respawns it at full health and it grows back (`RAMPAGE_HP=5`
starts it low, for testing). The game's sound plays (src/sound.c, [FORMAT_SOUND.md](FORMAT_SOUND.md): Rampage.uber's event programs over
.samp's PS-ADPCM, 3D by the camera, each block's `.mic` music): the monster's clip sounds and the people's; only in
a window of one's own (never with `--shot`; `--mute` keeps it quiet). The monsters' hair, fur and whiskers are the game's cloth simulation (src/cloth.c,
[CLOTH_NOTES.md](CLOTH_NOTES.md)), stepped in the world so the monster's moves drag them. The face plays its own clip over the body's (the game's face layer:
`Expression_Idle` by default, the clip events' `faceAnim` expressions; FORMAT_MONSTER.md "The face"). A clip plays mirrored when its events (type 8, `setMirrorState`)
say so: the skin takes each bone's other side reflected across the monster's x (for a symmetric mesh the same as
the game's pose mirroring, 0x194BA0), and root motion, steering, the conditions and the spheres follow
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "Mirroring"; `--do "mirror"` flips it for the next clip). The clips' `hitBuildingFront` calls damage the buildings by the
game's rules: the front cell takes 2 HP, the hit spreads up or down and sideways as the cell weakens, floors and
buildings keep their HP pools, and each facade tile shows its cell's state (the sheet's row), so five punches crack,
break and wreck a cell and its neighbours, and a damaged storey shows its interior (the floor's `Skeleton`, then its
`DamagedSkeleton` once half its `FloorHP` is gone) through the holes ([FORMAT_BDP.md](FORMAT_BDP.md) "Damage").
The roof stomp (`damageRoof` → `hitBuildingRoof`: the punch down, `square+down` on a roof, the landings and the butt
slam) shows the roof's damaged `TopFloorShapes` model and hits the top storey's front cell below the foot, so a
monster or a bot on a roof wears the building down a storey at a time (FORMAT_BDP.md "The roof stomp").
The facade's other passes are the game's too ("Facade geometry"): intact windows show `InnerWindow` interiors behind
the sheet's clear panes, tinted `CubeTint`; hits dent the facade (its grid vertices move in the face, 0.1 … 0.45 by level) and fade in `CrackTexture` by the levels,
around the windows of lightly damaged cells; a broken corner cell shows the inner wall behind it (the sheet's
`InnerWallColumn`). The game's particle effects play (src/fx.c, [FX_NOTES.md](FX_NOTES.md): fx.bdp's 287 effects, their words from
RAMPAGE.ELF): glass and wall debris as cells break (`Glass_Large` or `Wall_Small` at a cell's first crack, by
whether its column has a window outline, `Wall_Small` and `Wall_Large` as it breaks: FX_NOTES.md "Building debris"), the floors' collapse clouds with their camera shake, adornments' `DislodgedFx`, the
people's effects. The buildings' adornments hang where the game puts them (fire escapes, AC units, awnings, roof
trims: "Adornments"), ride their storeys down, fall off with their cells and are knocked off by attacks. A floor whose HP runs out falls: the storeys above
come down onto it and crush the one below, and a building whose HP runs out cracks, shakes, drops storey by storey
and sinks, leaving its rubble (the camera shakes as each floor falls, and on the monsters' heavy blows: CAMERA.md
"Shake") ("Collapse"; `--do "collapse"` destroys the building in front of the monster, `--do "hitcell b floor col"` hits one
front cell; `--do "bosshit n [part]"` deals the block's boss (or that part) one blow of n, BOSS_NOTES.md; `--do "strike k"` one blow on
street object k, as `RAMPAGE_STREET=1` lists them; `--do "soldier PoliceUSWindow1"` puts a window soldier on the
front ahead of the monster, `--do "unit PoliceUSFromTruckEasy"` an army unit 2.5 ahead on the street, `--do "fx
Car_explode -8 18 2"` an effect there (6 ahead of the monster without the place)).
Monsters climb: the game's canMount, canClimb and the mount bindings run against the buildings' boxes, so a monster
pushed at a building's front mounts it, climbs with the stick (each stroke flipping the clip), hops along the front,
swings round a corner onto a side unless a neighbour is flush with it, climbs down and steps off at the foot, or
mantles onto the roof at the top (its hands gripping the wall: the game's arm IK holds a planted hand where it took
hold while the clip climbs on), steps across onto the front of a building it touches, and braces itself across a
10-unit alley to climb between two buildings and turn onto the other one ([FORMAT_MONSTER.md](FORMAT_MONSTER.md)
"Climbing"; a place is refused, as the game's 0x2DDC40 does, by the building's adornments flagged 2 and by another
monster climbing within 8.2 of it, which a bot answers with a street punch). `--do "at x y"` puts the monster there,
facing the buildings; `--do "square+up 0.3"` presses a button with the stick held that way. A start through a slot
(a landing, a hit) blends in over the new state's blend time, or cuts when it has none (0x2C0E24). The monster's
collision box is the game's (MONSTER_DATA +0x198 / +0x19C: 3 each way across, 8.5 tall, 0x2E2C50): it cannot go
into a building's box (the game's response, collide.c's `collide_monster`), it can land on a roof (6 + 3 × storeys
up) and stand there, held on it by the roof clamp; its ground is the roof under its body centre (2.5 ahead of
George's feet), and as the game does (0x2E0330) it is eased down onto a lower roof it walks across to, or up onto
one at most 5 higher, in the state it is in; it never falls from the ground (FORMAT_MONSTER.md "Slots"). The
building a hit goes to is the game's search (0x2D54F0 with 0x2A3D20: the building at the point, or the nearest one
that way, the point within 5 of its face; collide.c `collide_hit_point`). The street ends at x ±75 and y −30.

**The collision world** (collide.c, [COLLIDE_NOTES.md](COLLIDE_NOTES.md)): main.c's `world_build` fills the frame's
`CollideWorld` (a Viewer field) once a frame, before anything moves: the block's buildings, the monster's collision
spheres (`COLLIDE_F_MONSTER`), then each module's objects (`army_collide_add`, `vehicle_collide_add`,
`rail_collide_add`, `parade_collide_add`, `streetobj_collide_add`, `boss_collide_add`), each tagged
`COLLIDE_TAG(owner, index)`. The army, vehicles, rail and parade hold it (`xxx_set_collide`). Asked of it: the
monster's push-out (`push_out`: the buildings, the PushOutMonsters objects, the VolumeTriggers, whose touches go to
boss.c), the shots' and beams' rays (army.c), every flying object's SpherePhysicsMover (the street's OnHitGround, the
objects it meets: TakeImpact on both; another module's object or a building is told through the module's `*_IMPACT`
event, `impact_route`: a building is hit with radius Mass / 1000), and the street objects' blow and touch tests.
`RAMPAGE_TRACE` prints the pushes, the trigger touches, what each shot hit, what each flight met and its bounces.

**Several monsters** ([MULTIPLAYER_NOTES.md](MULTIPLAYER_NOTES.md)): main.c keeps each monster's own state in a
`Mon` (`Viewer.mon[4]`, `nmon`; slot k is the scene's `Scene.mon[k]` with its own skeleton, clips, skin, head turn, arm
and foot IK, the army's, the game's player k and the HUD `Hud<k>.ifc`), the world's in `Viewer`. Each frame every
monster steps from its pad (`Mon.pad`: player 1's keys, player 2's, the controllers, `--do`, or the computer's), then the world
around them (people, buildings, street, army, vehicles, rail, parade, edibles, bosses, the game) sees them all; the
modules' events go to the monster they name. The bots get the game's DestroyBuildings goal (director.c `DG_DESTROY`:
the nearest standing building of the front row, mounted on a side, punched cell by cell, climbed, its roof stomped;
with none left, the nearest monster), and so do players 3 and 4 without a controller. Monsters' attacks hit each other
(each attack once a victim, its damage and ReactionLevel, no points); a building's points go to the monster that hit it
last; the camera frames every monster still in play (not a human whose streaker is gone, not one dropping in); a
player off a building is kept within 30 of the players' middle x and below 70 (the camera's reach), the bots are
not; the respawns, lives, King block winners and the results screens are game.c's per player. `RAMPAGE_TRACE` marks
the other monsters' lines `[2]` .. `[4]`.

**The revealed monster** ([EDIBLE_NOTES.md](EDIBLE_NOTES.md) "The cryotube"): with one player in the campaign,
the world's extra slot after the players gets the city's monster to reveal (`Viewer.extra`, 0x2B6BE0), hidden and
out of the world. When its cryotube lands, it appears there at a tenth of its size, grows to full size in a second,
taunts, and at 4.25 s runs off along the street to x ±100 (away from the camera) at 1.5 × speed, where it hides
again (reveal.c's walk events: the AI's PlayMove and WalkTo goals, setDynamicScale, setSpeedScale). It is not the
camera's, the HUD's or the game's, and has no push-out: 0x2E2C50 skips a slot past the players' count, which is
why it can walk past the street's ±55. `--do "hitcell b floor col"` hits that front cell as a monster's attack
would (a test: the tube's cell without climbing to it).

**No grabbing another monster:** the game has none. No move graph has a state for being held (George's holds are
people, objects and boulders), and the held / holder fields (+0xB9C0 / +0xB9C4) are only ever cleared
([MULTIPLAYER_NOTES.md](MULTIPLAYER_NOTES.md)).

**Boosy** unlocks as the game has it: at a block's end with every monster of the city's MonsterRevealDef unlocked,
the city gets flag 8, and when every city's saved flags (the profile's record, this city's | 8) have 1, 2, 4 and 8,
Boosy's flags get 0x80 (profile.c `profile_reveal_boosy`, [PROFILE_NOTES.md](PROFILE_NOTES.md)); the city's end
also unlocks him with Philbert and Eyegore (`profile_update`).

**The front end's 3D** ([SHELL_NOTES.md](SHELL_NOTES.md) "The GUI's 3D objects"): CitySelect's globe turns to each
city with its ring on it, lit for day or night; the main menu's fires, MonsterSelect's lava lamp and the cards' glow
play, through the game's GUI camera, each in its place among the screen's pictures.

## Not there yet
- the arcade emulators, after v1 ([ARCADE_NOTES.md](ARCADE_NOTES.md)); the memory card's states a PC has no meaning for
  (formatting, a card changed or taken out); the [G] marks in the notes.

Ported as modules and driven by the viewer: the HUD with its banners, boss bar and results screens (hud.c), people
(people.c), the army (army.c), the traffic, rockets and fire truck (vehicle.c), the edibles and the rage (edible.c),
the profile (profile.c), the bosses (boss.c), the cryotube's monster reveal and the rubble's points (reveal.c), the trains, cable cars and freighters (rail.c,
[RAIL_NOTES.md](RAIL_NOTES.md)), the parade balloons, bikers and food carts (parade.c, [PARADE_NOTES.md](PARADE_NOTES.md)),
the street and roof objects (streetobj.c, [STREETOBJ_NOTES.md](STREETOBJ_NOTES.md)), the movies (movie.c, MPEG-2 and
their ADPCM sound decoded by hand, [MOVIE_NOTES.md](MOVIE_NOTES.md)), the buildings' light and
neighbours' shadows (building.c, city.c, [BUILDING_LIGHT_NOTES.md](BUILDING_LIGHT_NOTES.md)), the water (water.c), the cloth and flags (cloth.c), the game flow and lives (game.c), its
choreography and camera modes (director.c), the block challenges and their upgrades (challenge.c,
[CHALLENGE_NOTES.md](CHALLENGE_NOTES.md)), the Pause menu (hud.c, [PAUSE_NOTES.md](PAUSE_NOTES.md)), the face queue, head turn, friction and the tails' springs (look.c), shadows (shadow.c),
effects with their mesh debris and sounds (fx.c), the footfalls, the foot lock and drag with the legs' IK (footstep.c, ik.c,
[FOOTSTEP_NOTES.md](FOOTSTEP_NOTES.md)) and sound (sound.c).
