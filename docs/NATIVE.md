# The native viewer (`src/`, `rampage.exe`)

The second half of the hybrid approach: next to the recompiled game, a hand-written C program that reads the game's
data with what the format work found and draws it, the way WoodyRE reimplements its game. The first piece is a viewer
for the city blocks; it reads `RAMPAGE.VOL` itself (no extracted files, no Python), so it is the C port of the
Python tools' readers and of `tools/city.py`.

## Building and running
```bat
build_viewer.bat
rampage.exe                                   :: the data it finds (below; build\disc\RAMPAGE.VOL first), Chicago, block 1
rampage.exe NewYork 6 --night                 :: the same, another block
rampage.exe "D:\Rampage - Total Destruction (Europe) (En,Fr,De,Es,It).iso" Chicago 1   :: straight from the disc image
rampage.exe build\disc\RAMPAGE.VOL NewYork 6 --night
rampage.exe build\disc\RAMPAGE.VOL Chicago 1 --monster George   :: play George by his own move graph, the game's camera follows
rampage.exe build\disc\RAMPAGE.VOL Chicago 1 --shot shot.png   :: one frame to a PNG, then quit (offscreen: no window)
rampage.exe build\disc\RAMPAGE.VOL NewYork 3 --monster George --do "at 25 -4, up 1" --cam 10 -10 8 30 5 --shot s.png
rampage.exe build\disc\RAMPAGE.VOL Chicago 1 --players 2 --monster George --monster2 Lizzie   :: two on the keys
rampage.exe build\disc\RAMPAGE.VOL Chicago 1 --players 4 --bots 2 --play kingcity            :: two players, two bots
```
`build_viewer.bat` compiles C99 with Zig (`zig cc`), like WoodyRE's `build.bat`: a Zig in `tools\zig`, on `PATH` or
from `pip install ziglang` is used, otherwise Zig 0.16.0 is downloaded once and checked against its SHA-256.

### Game data
The viewer needs only the disc: `RAMPAGE.VOL` and the game's ELF (`fx.c` reads the effects' word table from it). The
first argument may be the disc image itself (`.iso`, ISO 9660 with 2048-byte sectors, as the PS2 DVD dumps are; the
UDF bridge keeps the ISO 9660 tree): `RAMPAGE.VOL` and `SLES_538.00` are read in place from it, nothing is copied
or unpacked (src/iso.c; `Vol.base` is where the VOL starts in the image). Beside a `RAMPAGE.VOL` the ELF is
`RAMPAGE.ELF` (`build.ps1`'s split copy) or `SLES_538.00` (copied off the disc); either works. `build.ps1` is not
needed for the viewer.

Without a path (or with a city name first: `rampage.exe NewYork 6`) it looks, in this order, and takes the first:
1. `RAMPAGE_VOL` (the environment variable: a `RAMPAGE.VOL` or an image, taken as it is)
2. `build\disc\RAMPAGE.VOL` (what `build.ps1` extracts), `.\RAMPAGE.VOL`, `data\RAMPAGE.VOL`, `RAMPAGE.VOL` beside the
   program
3. a disc image (`*.iso` with `RAMPAGE.VOL` in it; by name, other images are passed over) beside the program, then in
   the current folder, then in `data\`
4. the user's data folder, `RAMPAGE.VOL` or an image in it: `%LOCALAPPDATA%\RampageRE` on Windows,
   `$XDG_DATA_HOME/RampageRE` (else `~/.local/share/RampageRE`) elsewhere

It never asks and never searches drives: with nothing found it says where it looked and exits with 1 (a `--shot` run
prints that to stderr, as every message). An image without `RAMPAGE.VOL` is refused ("not Rampage - Total
Destruction"). The supported release is the European SLES-53800 version 1.00: when `SYSTEM.CNF` (in the image, or
beside the VOL) boots another ELF or another version, or the VOL's index is not that release's (352 files ending at
byte 1,342,827,563), the viewer says so and goes on.

Keys: W A S D move, Q E down and up, Shift faster, right mouse button to look around, the wheel changes the speed;
Page Up / Page Down the block, 1-7 the city, N day or night, F fog, H the armed attack spheres, M the next monster (none, then all 30), K / L
the monster's clips, G play or the free camera, F5 .. F9 the graphics settings and F11 fullscreen ("Graphics
settings"), F12 a screenshot, Esc quit. In play (the default with `--monster`)
the keys are a pad: W A S D the left stick, the arrows the right stick (the head), Space Cross (jump), J Square
(punch), K Triangle (kick), L Circle (grab), R R2 (roar), P or Enter Start (the Pause menu: the arrows choose, Enter
or Space take, Backspace back); player 2 (`--players 2`) plays on the numeric keypad, Num Lock on: 8 4 2 6 (or 5)
the left stick, 0 Cross, 7 Square, 9 Triangle, 1 Circle, 3 R2, / and * the right stick; the monster runs its own
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
3 French, 4 German, 5 Italian, 6 Spanish, 7 Dutch, 8 Portuguese); `--card-free-kb N` the "memory card"'s free room
(a save needs 140; else the drive's). The profile (profile.c, [PROFILE_NOTES.md](PROFILE_NOTES.md)) is read from and written to
`rampage_profile.bin` (`--profile FILE` another one, `--newprofile` a new one; a `--shot` run starts new and writes
nothing unless `--profile` is given): a new profile has six monsters and none of the four upgrade moves, which the
upgrade challenges teach; `--cheat 011235` gives every move. `--shell` starts in the front end (shell.c, [SHELL_NOTES.md](SHELL_NOTES.md)): the
autosave notice, the movies (the logos and the intro, Start or Enter skips them once a profile exists; the
Victory movie and the credits after the campaign), the profile popup (one profile, its file; letters and digits type
the name, Enter is DONE; it opens from Back on the main menu, "Profiles", and autosave is on once a profile is
loaded from it or the Results' Save is pressed), the main menu, players, monster select, city select, the loading screen, then the game it chose; the Pause menu's Quit
goes back to the main menu. Its keys: the arrows, Enter / Space accept, Backspace back, P Start, C / V / R the
square, triangle and R2 buttons, F2 the cheat entry. With `--shot` the `--do` words drive it first (`wait S`,
`menu up|down|left|right|accept|back|start`, `button N`, `type TEXT`, `done`), and the words left once a game
starts play in it. The console prints
what each block holds (models, people, vehicles, anything not found) and, in play, every state the monster enters
(`RAMPAGE_TRACE=1`: every frame's state, time, place and heading; `RAMPAGE_HANDS=1`: the hands' grips each frame;
`RAMPAGE_NOIK=1`: the arms as the clips have them; `RAMPAGE_MAGENTA=1`: a magenta background, to show holes in the
world). The fog is on (F toggles it, `--nofog` starts without). Texture packs: PNGs in `mods/textures/` replace the
game's textures, `--dumptex` writes every texture drawn to `mods/dump/` under the name a replacement takes
([TEXTURES.md](TEXTURES.md)).

## Platforms
As in WoodyRE (its docs/PLATFORM.md): the Windows build keeps its own Win32 + WGL window, and everything else runs on
SDL2 behind the same functions (`win_*` in render_gl.h, `plat_*` in plat.h). Keys are the Windows virtual-key codes
on every system, so main.c has one spelling of them.

| | Windows (`build_viewer.bat`) | Linux (`build_viewer.sh`) |
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
./build_viewer.sh                                   # ./rampage (CC=clang ./build_viewer.sh for another compiler)
./rampage build/disc/RAMPAGE.VOL Chicago 1 --monster George
SDL_VIDEODRIVER=offscreen ./rampage build/disc/RAMPAGE.VOL Chicago 1 --mute --shot s.png   # no display needed
```
`SDL_VIDEODRIVER=offscreen` is SDL's EGL pbuffer driver: it needs no X or Wayland server (CI, WSL without WSLg).
Pass the disc image (`./rampage ~/Rampage.iso Chicago 1`), or put it (or `RAMPAGE.VOL` with `SLES_538.00` or
`RAMPAGE.ELF`) in `~/.local/share/RampageRE` ("Game data" above). A shot on Mesa's llvmpipe differs from the Windows one only in rounding (under 0.1 % of the pixels by more
than 8 of 255).

### OpenGL ES
`GLES=1 ./build_viewer.sh` (needs `libgles-dev` instead of `libgl-dev`) builds the same program on OpenGL ES 3.0, or
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
plays on the keys and the controller in slot 0, player 2 on the keypad and slot 1, players 3 and 4 on slots 2 and 3
when those have a controller as the game starts (else the computer drives them, as before; bots are always the
computer's). Steam Input and DS4Windows show a Sony pad twice (its HID device and an XInput one): while a Sony pad is
open on Windows, XInput is not read (`RAMPAGE_PADXINPUT=1` reads both). Input and rumble pause while the window is not
in front. `RAMPAGE_PADLOG=1` prints every rumble.

**The mapping**, the PS2 pad's buttons in the slots its pack gives them [R table 0x3B18B0] (keys_pad's keys still
fill the same pad; buttons add up, the stick pushed further wins):

| PS2 | game variable (MV) | Xbox / SDL name | menus (shell, Pause, results) |
|---|---|---|---|
| left stick | LX, LY 0, 1 | left stick | past 0.5: up / down / left / right; pad 0's x moves the sliders |
| right stick | RX, RY 2, 3 (the head) | right stick | |
| d-pad | UP .. RIGHT 4 .. 7 | d-pad | up / down / left / right (held: again after 0.4 s, then every 0.1 s) |
| Start | START 8 | Start / Options / + | Start (in play: the Pause menu, from that pad) |
| Select | SELECT 9 | Back / Share / Create / − | |
| R2, L2 | R2 10, L2 11 | RT, LT (from a quarter of their travel) | R2 = button 4; L2 + R2 held on pad 0 alone: the main menu's cheat entry (F2 on the keys) |
| R1, L1 | R1 12, L1 13 | RB, LB | |
| Cross | CROSS 14 (jump) | A / Cross / the bottom button | accept |
| Circle | CIRCLE 15 (grab) | B / Circle | back (CommonGUI's `Controller_Back` is a circle) |
| Square | SQUARE 16 (punch) | X / Square | button 0 |
| Triangle | TRIANGLE 17 (kick) | Y / Triangle | button 1 |
| R3, L3 | R3 20, L3 21 | RS, LS | |

The sticks go through a dead zone of 0.2 per axis, the rest scaled to full (the PS2 pack does it per axis, 0x1A6050;
its own zone was not traced). The pressures (variables 28, 29) are not read by the port's move graphs, so the
triggers' travel is not passed on. Each controller sends its menu events as its own pad (`shell_input_pad`,
`hud_input_pad`: the shell's player screens and the Pause menu's Options, pad 0's, tell them apart as the game does);
the held state (`shell_pad_held`, `hud_pad_held`: getController(0)) is pad 0's and the keys'.

**Rumble**, where the game makes it (PS2ControllerPack vtable +0x28 = 0x1A5020 (pad, ms): with the vibration on, the
big motor at 0xC0 of 255 and the small one off [R]); here the low-frequency motor at 0xC0 / 255 of the player's
controller, never for a computer-driven monster (+0x108):

| What | ms | Where (port) |
|---|---|---|
| a hit on the monster, level 1 / level 2 (0x2D8BE4 / 0x2D8C14, in 0x2D6A50's hit) | 400 / 800 | `monster_hit` |
| a landing from the air (0x2DC9F4, in 0x2DC450) | 400 | `mon_step`'s landing |
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
| fullscreen | `fullscreen=0` | `--fullscreen`, `--windowed` | `RAMPAGE_FULLSCREEN` | F11 | borderless over the whole monitor at the desktop's resolution (no mode switch) |
| vsync | `vsync=1` | `--vsync 0\|1` | `RAMPAGE_VSYNC` | | the swap interval (`wglSwapIntervalEXT`, SDL's) |
| ambient occlusion | `ao=0` | `--ao`, `--noao` | `RAMPAGE_AO` | F5 | GTAO: contact shadows where surfaces meet (building bases, ledges, under the monster's feet) |
| its radius | `ao_radius=4` | `--ao-radius R` | `RAMPAGE_AO_RADIUS` | | how far (world units; a monster is about 25 tall) a surface looks for what hides the sky |
| SMAA | `smaa=0` | `--smaa N` | `RAMPAGE_SMAA` | F6 (cycles) | edge smoothing, 0 off, 1 .. 4 (or `low`, `medium`, `high`, `ultra`, SMAA's presets): polygon edges and the alpha-tested cut-outs (fire escapes, trees, facade sheets) |
| MSAA | `msaa=0` | `--msaa N` | `RAMPAGE_MSAA` | F7 (cycles) | multisampling, 0, 2, 4, 8 samples: polygon edges only |
| anisotropy | `aniso=1` | `--aniso N` | `RAMPAGE_ANISO` | F8 (cycles) | `GL_TEXTURE_MAX_ANISOTROPY` 1 (off), 2, 4, 8, 16 on the world's textures: streets and walls at a grazing angle stay sharp |
| mips | `mips=0` | `--mips`, `--nomips` | `RAMPAGE_MIPS` | F9 | a 2×2 box mip chain (alpha-weighted, so cut-outs keep their colour) with trilinear filtering on the world's textures: no shimmer in the distance; anisotropy needs it to do much |

**rampage.cfg** is read from the current directory (next to `rampage_profile.bin`) before the window opens: one
`key=value` per line, `#` starts a comment, unknown keys are reported. The viewer never writes it; the keys change the
running session and print the settings as `rampage.cfg` lines (`graphics: ao=1 ...`) and in the title. Order: the
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

## Source
| File | |
|---|---|
| `src/data.h` | the formats: declarations and little-endian readers |
| `src/vol.c` | `RAMPAGE.VOL` (`tools/pvol.py`), from the file or in place from the disc image |
| `src/iso.c` | the disc image (ISO 9660, `tools/iso9660.py`): a file by its path, read in place; the game's ELF; where the data is (`data_find`) and whether it is the supported release ("Game data") |
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
| `src/plat_sdl.c`, `src/plat.h` | the window, keys and OS bits on SDL2 outside Windows; what is taken from the OS ("Platforms") |
| `src/gles/GL/gl.h`, `src/gles/gles2.c` | the fixed function on OpenGL ES 2.0 / 3.0 (`GLES=1`, WoodyRE's shim with lighting, fog and sphere maps; "OpenGL ES") |
| `src/pad.h`, `src/pad.c`, `src/pad_sdl.c` | the controllers: Sony HID and XInput on Windows, SDL's game controllers elsewhere; a slot per player, rumble, lights ("Input") |
| `src/shot.c` | PNG screenshots (stored zlib blocks, no compressor) |
| `src/texpack.c` | texture packs: `mods/textures/` replacements and `--dumptex` (WoodyRE's, [TEXTURES.md](TEXTURES.md)); `src/stb/` stb_image and stb_image_write |
| `src/settings.c` | the graphics settings: `rampage.cfg`, `RAMPAGE_*`, the flags, F5 .. F9 and F11 ("Graphics settings") |
| `src/postfx.c`, `src/smaa/smaa.h` | MSAA and SMAA (WoodyRE's; `smaa.h` is SMAA by Jimenez et al., MIT) |
| `src/gtao.c` | the ambient occlusion, GTAO (WoodyRE's) |
| `src/main.c` | the viewer: arguments, camera, keys; the monsters in play (`Mon`), their pads and their world |
| `src/director.c` | the game flow's choreography: the AI's goals (playMove, walkTo, the bots' DestroyBuildings) and the camera modes (docs/FLOW_NOTES.md, MULTIPLAYER_NOTES.md) |

The game lights its city at run time: texture × (ambient + Σ colour × N·L) from the light file
([FORMAT_BDP.md](FORMAT_BDP.md) "Lighting"), which is OpenGL's fixed-function lighting, so the light file's ambient
colour and directional lights become `GL_LIGHT_MODEL_AMBIENT` and `GL_LIGHT0..2`. Meshes are drawn from both sides
with their normals as stored; texels with alpha below 0.5 are dropped, as in the glTF export. The cloud band
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
eats (health, energy) or throws them, and its attacks send them flying. Script checks the graph asks (`Monsters::canMount`,
`canGrabPerson`, ...) answer no, except the monster's own abilities (roar energy, double jump, jump smash, spin,
punch). While a clip's attack event has a body part armed, its collision spheres are drawn as red wire spheres (H
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
RAMPAGE.ELF): glass and wall debris as cells break (`Glass_*`, `Wall_*` of the building props; which goes with
which state is a stand-in), the floors' collapse clouds with their camera shake, adornments' `DislodgedFx`, the
people's effects. The buildings' adornments hang where the game puts them (fire escapes, AC units, awnings, roof
trims: "Adornments"), ride their storeys down, fall off with their cells and are knocked off by attacks. A floor whose HP runs out falls: the storeys above
come down onto it and crush the one below, and a building whose HP runs out cracks, shakes, drops storey by storey
and sinks, leaving its rubble (the camera shakes as each floor falls, and on the monsters' heavy blows: CAMERA.md
"Shake") ("Collapse"; `--do "collapse"` destroys the building in front of the monster, `--do "hitcell b floor col"` hits one
front cell).
Monsters climb: the game's canMount, canClimb and the mount bindings run against the buildings' boxes, so a monster
pushed at a building's front mounts it, climbs with the stick (each stroke flipping the clip), hops along the front,
swings round a corner onto a side unless a neighbour is flush with it, climbs down and steps off at the foot, or
mantles onto the roof at the top (its hands gripping the wall: the game's arm IK holds a planted hand where it took
hold while the clip climbs on), steps across onto the front of a building it touches, and braces itself across a
10-unit alley to climb between two buildings and turn onto the other one ([FORMAT_MONSTER.md](FORMAT_MONSTER.md)
"Climbing"; the checks the game also makes against attachments, cells flagged 0x1000 and other monsters are left
out). `--do "at x y"` puts the monster there, facing the buildings; `--do "square+up 0.3"` presses a button with the stick
held that way. Stand-ins for game code not read: a start through a slot (a landing) blends in (it takes no
transition, so which way it comes in is not traced). The monster's collision box is the game's (MONSTER_DATA +0x198 / +0x19C: 3 each way across, 8.5 tall, 0x2E2C50):
it cannot go into a building's box (the game's response, collide.c's `collide_monster`), it can land on a roof (6 + 3 × storeys up) and stand there, and walking off the edge it falls (a stand-in: it
drops in the state it is in until the landing slot catches it). The building a hit goes to is the one whose face
lies within 5 units along the side's direction (the game's search, 0x2A3D20, is not read). The street ends at
x ±75 and y −30.

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
- the arcade emulators and the demo game, skipped (SHELL_NOTES.md); the memory card's own popup states; the [G] marks
  in the notes.

Ported as modules and driven by the viewer: the HUD with its banners, boss bar and results screens (hud.c), people
(people.c), the army (army.c), the traffic, rockets and fire truck (vehicle.c), the edibles and the rage (edible.c),
the profile (profile.c), the bosses (boss.c), the cryotube's monster reveal and the rubble's points (reveal.c), the trains, cable cars and freighters (rail.c,
[RAIL_NOTES.md](RAIL_NOTES.md)), the parade balloons, bikers and food carts (parade.c, [PARADE_NOTES.md](PARADE_NOTES.md)),
the street and roof objects (streetobj.c, [STREETOBJ_NOTES.md](STREETOBJ_NOTES.md)), the movies (movie.c, MPEG-2 and
their ADPCM sound decoded by hand, [MOVIE_NOTES.md](MOVIE_NOTES.md)), the buildings' light and
neighbours' shadows (building.c, city.c, [BUILDING_LIGHT_NOTES.md](BUILDING_LIGHT_NOTES.md)), the water (water.c), the cloth and flags (cloth.c), the game flow and lives (game.c), its
choreography and camera modes (director.c), the block challenges and their upgrades (challenge.c,
[CHALLENGE_NOTES.md](CHALLENGE_NOTES.md)), the Pause menu (hud.c, [PAUSE_NOTES.md](PAUSE_NOTES.md)), the face queue, head turn and friction (look.c), shadows (shadow.c),
effects with their mesh debris and sounds (fx.c), the footfalls, the foot lock and drag with the legs' IK (footstep.c, ik.c,
[FOOTSTEP_NOTES.md](FOOTSTEP_NOTES.md)) and sound (sound.c).
