# RampageRE platform plan: SDL2/Linux, controllers, CI, texture packs, graphics options, Switch, Android

Based on WoodyRE `master` (github.com/jjmhalew/WoodyRE; the default branch is `master`, not `main`) and RampageRE `main` at
8522f36. The WoodyRE files are read from its repository (raw.githubusercontent.com/jjmhalew/WoodyRE/master/...).

## 0. Licence and copying

- WoodyRE: **GPL-3.0-or-later** (README "License"; LICENSE is the GPLv3 text). RampageRE: **GPL-3.0** (LICENSE, README).
  GPLv3-or-later code can go into a GPLv3 project, so files can be copied as they are. The same person owns both
  (jjmhalew), which also allows relicensing, but it is not needed.
- Third-party parts that come along keep their own notices. Add them to RampageRE's README "License" section:
  - `src/stb/stb_image.h`, `stb_image_write.h`: public domain or MIT.
  - `src/smaa/smaa.h`: MIT (iryoku/smaa). The licence text sits at the top of the file; `tools/smaa_embed.py` regenerates it.
  - `android/app/src/main/java/org/libsdl/`: SDL 2.32.10 Java glue, zlib licence. The Android build downloads SDL itself
    (FetchContent with SHA-256).
- WoodyRE's README makes the data point clearly: no game data in the repo or in builds, and anything made with the
  user's data (standalone exe, bundle APK) is for their own use only. RampageRE's `.gitignore` already keeps `build/`,
  `*.iso`, `*.VOL` and `*.ELF` out of the repo.

## 1. Where RampageRE stands

| Area | RampageRE today | Platform-bound bits |
|---|---|---|
| OS layer | `src/plat.h`: `<windows.h>`, `plat_gl_proc` = wglGetProcAddress, `plat_message` = MessageBoxA, `plat_free_kb` = GetDiskFreeSpaceExA. There is already a small `#else` (strcasecmp, fseeko, stderr message, free_kb -1) | VK_* constants come from `<windows.h>`; none are defined outside Windows |
| Window/GL/input | `src/render_gl.c` lines 14-148: Win32 class, `wnd_proc` (WM_KEYDOWN → `keys[]`, `pressed[]`; right-drag mouse; wheel; focus loss clears keys), WGL context, vsync, `offscreen()` EXT FBO for `win_open(..., hidden)`. `Window` = `{width,height,keys[256],pressed[256],mouse_dx/dy,mouse_right,wheel,quit,hwnd,hdc,hglrc}` | All of it |
| Renderer | OpenGL 1.1 fixed function: GL lighting (LIGHT0..3, `GL_COLOR_MATERIAL`, `GL_NORMALIZE`), linear fog, sphere-map texgen (`glTexGeni`, env maps), `glPushAttrib`/`glPushClientAttrib`, `glGetFloatv(MODELVIEW / FOG_COLOR)`, `glPolygonOffset`, client arrays + `glDrawElements(GL_UNSIGNED_INT)`, `glBegin` lines/quads. **Every texture is uploaded in one place**: `texture()` (render_gl.c:229), plus the movie's `video_texture()` | Desktop GL only; GLES needs a shim (see phase 7) |
| Input use | `src/main.c` only (57 sites): `keys_pad()` (5107-5132), `shell_frame()` (5020-5048), the results/pause blocks in the main loop (5350-5373), viewer keys (5385-5464, 5526). `vehicle.c`'s `VK_CAR`/`VK_ROCKET`… are its own enum, not key codes (no clash, but the prefix is a trap) | VK codes |
| Sound | `src/sound.c` 920-1017: waveOut from winmm.dll loaded at run time, **pushed from the main thread** (`out_feed()` in `sound_update`, 5 of 12 buffers of 1024 frames queued, ~107 ms). The `#else` is a stub (`sound_start` returns -1) | waveOut |
| Screenshots | `shot.c`: glReadPixels → own PNG writer | none |
| Data | `RAMPAGE.VOL` (1.34 GB, 352 files, offsets u32) + `RAMPAGE.ELF` (3.4 MB, fx.c's words) next to it; default path `build\disc\RAMPAGE.VOL` (backslashes), extracted by `build.ps1` (recomp flow, needs Python + VS) | backslash path |
| Build | `build_viewer.bat`: one `zig cc -std=c99 -O2 -Wall -target x86_64-windows-gnu -mcpu=baseline` of 42 files, `-lopengl32 -lgdi32 -luser32` | Windows only |
| Tests rule | `--shot` → `win_open(hidden=1)`: WS_EX_NOACTIVATE tool window at -32000, never shown, frame in an FBO; sound never opened | must hold everywhere |

WoodyRE's split (docs/PLATFORM.md): the **Windows build keeps its own Win32 + WGL window in `render_gl.c`**. `plat_sdl.c`
implements the same `win_*` + `plat_*` API on SDL2 for everything else, and keys are stored as **Windows VK codes on
every platform**, so the game code has one spelling. Copy this split as it is. The Windows build then does not change,
and the offscreen `--shot` path that already works stays as it is.

### PS2 pad → RampageRE variable → current keys → controller

[R] The PS2 pack's table (0x3B18B0, 16 × {libpad mask, slot, pressure slot}, mask = pad byte 3 | byte 2 << 8) puts
each button in the controller's slot of the same number as the MV id: L2 → 11 (pressure 29), R2 → 10 (pressure 28),
L1 → 13, R1 → 12, Triangle → 17, Circle → 15, Cross → 14, Square → 16, Select → 9, L3 → 21, R3 → 20, Start → 8,
Up → 4, Right → 7, Down → 5, Left → 6 (no pressure: 0x3B). The GUI's buttons are shown by CommonGUI's pictures
`Controller_Select` (a cross: Accept), `Controller_Back` (a circle: Back), `Controller_B0` (a square) and
`Controller_B1` (a triangle), so on the PS2 the GUI's Back (GuiEvent 32) is Circle.

The pad variables are the game's own (docs/FORMAT_MONSTER.md "variables", `MV_*` in moves.h). The game's logical pad
is Xbox-named (SHELL_NOTES.md: the PS2 pack 0x1A5220 / table 0x3B18B0 maps Cross→A, Circle→B, Square→X, Triangle→Y,
L1/R1→shoulders, L2/R2→triggers, Select→Back), so SDL's GameController names fit it directly.

| PS2 | MV id | P1 keys (`keys_pad`) | P2 keys | Menus (shell / pause / results) | SDL / Xbox / pad.h |
|---|---|---|---|---|---|
| Left stick | MV_LX/LY 0,1 (y down) | W A S D | Num 8 4 2 6 (5) | arrows → HUD_EV_UP/DOWN/LEFT/RIGHT; held left/right → `hud_pad_held` / `shell_pad_held` (both already take `stick_x`) | left stick (SDL y is already down = +) |
| Right stick | MV_RX/RY 2,3 | arrows | Num / * (x only) | – | right stick |
| D-pad | MV_UP..MV_RIGHT 4-7 | – (not mapped in play) | – | arrows | D-pad (`PAD_UP..PAD_RIGHT`) |
| Start | MV_START 8 | P or Enter (pause) | – | P = HUD_EV_START | Start / Options / + |
| Select | MV_SELECT 9 | – | – | – (the logical "Back" button of getButtons, not the GUI's Back) | Back / Share / − |
| R2 | MV_R2 10 (+ pressure 28) | R (roar) | Num 3 | R = HUD_EV_BUTTON 4 | RT (`rt` analog → pressure 28) |
| L2 | MV_L2 11 (+ pressure 29) | – | – | F2 = L2+R2 held (cheat entry) | LT (`lt` → pressure 29) |
| R1 / L1 | 12 / 13 | – | – | – | RB / LB |
| Cross | MV_CROSS 14 | Space (jump) | Num 0 | Enter / Space = ACCEPT | A / Cross / B-position on Nintendo |
| Circle | MV_CIRCLE 15 | L (grab) | Num 1 | Backspace = HUD_EV_BACK | B / Circle |
| Square | MV_SQUARE 16 | J (punch) | Num 7 | C = HUD_EV_BUTTON 0 (Change Monster / Bonus) | X / Square |
| Triangle | MV_TRIANGLE 17 | K (kick) | Num 9 | V = HUD_EV_BUTTON 1 (Autosave / Options) | Y / Triangle |
| R3 / L3 | 20 / 21 | – | – | – | RS / LS |

Viewer-only keys need no pad equivalent: PgUp/PgDn block, 1-7 city, N, F, H, M, G, K/L clips (free camera only), Q/E,
Shift, mouse look, wheel, F12, Esc.

---

## Phase 1: SDL2 platform layer and a Linux build (medium)

**WoodyRE files to adapt**
- `src/plat.h` (52 lines) → replace RampageRE's `#else` half. Take the VK enum and **add `VK_F2` (0x71) and keep
  `VK_NEXT/PRIOR`, `VK_NUMPAD0-9`, `VK_MULTIPLY`, `VK_DIVIDE`, `VK_F12`**. Also take `Sleep`, `_stricmp`,
  `plat_fopen` (case-insensitive lookup that also turns `\` into `/`, which fixes the `build\disc\RAMPAGE.VOL` default),
  `plat_exists`, `plat_gl_proc` and `plat_message`. Add `plat_free_kb` via `statvfs`.
- `src/plat_sdl.c` (277 lines): `vk_of`, `plat_fopen`/`resolve`, `win_open/poll/swap/close/title/time`. Changes:
  - `win_open(w, title, w, h, hidden)`: RampageRE has the extra `hidden` argument. When it is set, use `SDL_WINDOW_HIDDEN`,
    never `SDL_ShowWindow` / raise, and draw into an FBO. Move `offscreen()` from render_gl.c into a shared helper that
    takes `plat_gl_proc` and core `glGenFramebuffers` (falling back to the EXT names). With no FBO, **fail** instead of
    showing the window. For headless Linux/WSL/CI document `SDL_VIDEODRIVER=offscreen` (SDL's EGL pbuffer driver).
  - `win_poll`: fill RampageRE's `pressed[]` on key-down transitions (cleared each poll, as wnd_proc does) and `wheel` from
    `SDL_MOUSEWHEEL`. Keep `mouse_dx/dy` only while the right button is held (as Win32). Keep WoodyRE's "pressed and
    released within one poll stays down a frame". Clear keys on focus loss.
  - Add `focused` and `dev_changes` to `Window` (WoodyRE has both; phase 2 needs them). On Win32, set `focused` from
    WM_SETFOCUS/WM_KILLFOCUS and count WM_DEVICECHANGE.
  - Drop WoodyRE's `touch_*` calls for now (phase 9), `win_mode`/`win_vsync` (phase 5) and the Android/Switch branches
    (phases 8-9; keep them `#if 0`, or copy them later).
- `build.sh` (18 lines) → `build_viewer.sh`: `CC=${CC:-cc}`, `-std=gnu99 -O2 -Wall -D_FILE_OFFSET_BITS=64`, the SRC
  list from build_viewer.bat (forward slashes) plus `src/plat_sdl.c`, `$(sdl2-config --cflags --libs) -lGL -lm`. Keep
  one SRC list per script, as WoodyRE does (three copies: .bat, .sh, CMakeLists). A `GLES=1` switch comes in phase 7.

**RampageRE changes**
- `src/render_gl.c`: wrap lines 14-148 (window) in `#ifdef _WIN32`; the renderer below stays shared.
- `src/render_gl.h`: `Window` gains `focused`, `dev_changes`; `hwnd/hdc/hglrc` are `SDL_Window*`/-/`SDL_GLContext` under SDL.
- `src/sound.c` `#else` branch: an SDL backend that **keeps the push model**. Use `SDL_OpenAudioDevice(NULL, 0, {48000,
  AUDIO_S16SYS, 2, 1024, callback=NULL})` with `SDL_QueueAudio`, and keep `SDL_GetQueuedAudioSize` at 5 × 1024 frames
  in `out_feed()`. That is the same shape as the waveOut code: no thread or mutex, and `sound_mix`/`manual_mix` are
  unchanged. (WoodyRE's `audio.c` uses an SDL callback plus mutex because its mixer was built for that; RampageRE's
  is not.)
- `src/main.c`: no input changes (VK names stay). The default VOL path still works through `plat_fopen`. Check
  `fx.c:727`'s plain `fopen` of the ELF (also through the `fopen` → `plat_fopen` macro).
- `src/plat.h` (Windows half) and main.c's `vol_open` failure: `plat_message` must **not open a message box in a
  `--shot` run**. Today `MessageBoxA` pops up if the VOL is missing even with `--shot`, which breaks the no-window rule.
  Add a `plat_quiet` flag set by `--shot` (print to stderr, exit non-zero).
- `docs/NATIVE.md`: a "Platforms" section like WoodyRE's docs/PLATFORM.md (table of which file implements what).

**Testing**: WSL2 Ubuntu (`apt install build-essential libsdl2-dev`), run `--shot` with `SDL_VIDEODRIVER=offscreen` or a
hidden window. Compare PNGs with the Windows build (WoodyRE reports <1 % pixel difference between drivers). Do not
adopt WoodyRE's xdotool/WSLg visible-window testing (PLATFORM.md §2), because it opens windows.

**Risks**: `-std=c99` hides POSIX on glibc, so use gnu99 or `-D_DEFAULT_SOURCE`. Mesa needs a compatibility profile
(SDL's default context is compat; do not request core). `GL_UNSIGNED_INT` indices and `glPushClientAttrib` are fine on
desktop GL. Watch `-Wall` warnings from gcc that zig/clang did not show.

## Phase 2: Controllers (medium)

**WoodyRE files**: `src/pad.h` (29), `src/pad.c` (345: DualShock 4 / DualSense / Edge raw HID over USB+BT with CRC-32
output reports, light bar, rumble; XInput 1_4/1_3/9_1_0 loaded at run time, ordinal 100 for Guide; empty slots polled
every 2 s and on WM_DEVICECHANGE), `src/pad_sdl.c` (168: SDL GameController, PS4/PS5 rumble + LED hints, Switch label
kind, virtual-controller test hook `WOODY_VPAD`).

**The main difference**: WoodyRE is single-player and **merges every pad into one** `PadState`. RampageRE has up to 4
players (`--players`, `DRIVE_KEYS1/2/COMPUTER`).
- Change the API to `int pad_poll_all(PadState st[4], int focused, unsigned devchanges)`. Slots are assigned in
  connection order and kept per instance id; a pad that leaves frees its slot.
- Keep a merged `pad_menu()` (any pad drives the shell / pause / results), or player 1's pad only, to match the
  original's "pad 0" checks (the shell's cheat entry reads pad 0).
- `pad_rumble(slot, strength, seconds)` per player.

**RampageRE changes**
- `main.c`: a new `DRIVE_PAD` (or pads layered over the keys). P1 = keys + pad 0, P2 = keypad + pad 1, P3/P4 = pad 2/3
  when connected, else the computer as today. A new `pad_to_pad(const PadState*, const PadState *prev, Pad *pd)`
  fills **all** MV buttons from the table above: `pressed[]` = edge against the previous frame, `held[]`. Add MV_R2
  pressure from `rt` and MV_L2 from `lt` if the move graphs read ids 28/29; check `moves.c`'s variable fill.
  `keys_pad` still writes the keys. Combine by OR for buttons and the larger magnitude for sticks.
- `shell_frame` / pause / results blocks: turn the pad into HUD events (D-pad or stick past 0.5 with repeat → UP/DOWN/LEFT/RIGHT, Cross →
  ACCEPT, Circle → BACK, Start → START, Square/Triangle/R2 → HUD_EV_BUTTON 0/1/4). `shell_pad_held`
  gets `stick_x` and L2+R2 from the triggers (the real cheat entry instead of F2).
- Rumble: `HUD_CALL_RUMBLE` (setEnableRumble) → `pad_set_strength(0/1)`; `HUD_CALL_RUMBLE_PULSE` (rumble(250 ms)) →
  `pad_rumble`. The game's own pad effects go through PS2ControllerPack (0x488700) vtable +0x28 = 0x1A5020
  (pad, milliseconds) [R]: with the vibration on (+0xC84) it sets the big motor to 0xC0 and the small one to 0
  (scePadSetActDirect) for that long. Every call (the object at gp−0x6F5C, the pad the monster's +0xFC, never for a
  computer's monster, +0x108) [R]:
  - a hit on the monster (0x2D6A50; its level, the 8th argument): level 1 400 ms, level 2 800 ms, level 0 none;
  - a landing from the air, from movement modes 1 / 2 (0x2DC450): 400 ms, but dead: the landing has set mode 4
    (0x2DC5DC) before the test (0x2DC9C4);
  - the front of the building it climbs (0x2D6150) or stands on (+0xC640 while +0xC644) starting to crack
    (0x2BD7D0, the collapse): 2000 ms;
  - `gCommon.rumble(250)` (0x2C34AC), from Options.scr / Pause.scr when the vibration is turned on;
  - the controller script binding (0x169E38), which no script uses for rumble.
  The PS2's big motor has one speed; map 0xC0 / 255 to the pad's low-frequency motor.
- `render_gl.c` (Win32): count `WM_DEVICECHANGE` into `w->dev_changes`, track focus. `build_viewer.bat` LIBS:
  add `-lhid -lsetupapi` (pad.c loads xinput at run time; WoodyRE links `-lhid -lsetupapi`).
- Light bar: WoodyRE paints the DualSense "Woody red". Use a per-player colour (the monster's) or the PS player LEDs
  (`SDL_GameControllerSetPlayerIndex(slot)`).
- Nintendo pads: WoodyRE maps by **label** (A = right button = jump). For RampageRE, A by position (bottom = Cross)
  matches PS2 muscle memory better. Choose by position with SDL's `SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS=0`, and
  write the choice down.
- Button glyphs: the .loc texts embed `\vController_Back\v`-style pictures (hud.c:267). They stay PS2 glyphs. Optional
  later: Xbox glyphs when `kind == PADK_XBOX`.
- Test hooks: `RAMPAGE_VPAD="T:LX:LY:BUTTONS[:D] ..."` (SDL virtual controller, the whole SDL path) and `RAMPAGE_PADLOG=1`.
  `--do` scripts keep driving `Pad` directly, so existing shots are unchanged.

**No-window rule**: in a `--shot` run **do not open pads at all**. That avoids light-bar or rumble side effects on the
user's hardware and keeps shots deterministic. WoodyRE pauses input while unfocused; a hidden window is never focused,
so `focused=0` already blocks input, but the open itself already writes LED reports.

**Risks**: the per-slot assignment UI (who is player 2) does not exist in the original's PC-less world. The PS2 shell's
multiplayer screens read pads 0..3, so check SHELL_NOTES / MULTIPLAYER_NOTES for "press Start to join". Steam Input
can show one pad twice (XInput + HID); WoodyRE skips Sony devices in WinMM, and RampageRE has no WinMM, so only
HID+XInput can double up when DS4Windows is installed. Dedupe by not taking XInput slots while a Sony HID pad is open.

## Phase 3: CI (small; can come right after phase 1)

**WoodyRE file**: `.github/workflows/build.yml` (155 lines): jobs `windows` (build.bat, version check against
`res/woodyre.rc`, zip of exe+README+LICENSE), `linux` (ubuntu-22.04 for glibc 2.35 / SDL 2.0.20, `libsdl2-dev
libgl-dev`, `ldd`), `android` (gradlew assembleRelease, optional signing secrets), `switch` (container
`devkitpro/devkita64`), `release` on `v*` tags (`gh release create` with the tag message as notes). No tests run, and
no game data is involved: the programs hold only the port's code.

**RampageRE**: `.github/workflows/build.yml` with `on: push: branches: [main]`, `pull_request`, `tags: ['v*']`.
- `windows`: `build_viewer.bat` (Zig is fetched with SHA-256; cache `tools/zig`); artifact `RampageRE-<sha>.zip` with
  `RampageRE-dev.exe`, README.md, LICENSE.
- `linux`: `sh build_viewer.sh rampage` on ubuntu-22.04; artifact tar.gz.
- Later phases add `switch` and `android` jobs.
- **Do not check out submodules** (actions/checkout's default `submodules: false`; PS2Recomp and the recomp need the
  disc) and do not run `build.ps1`.
- Keeping data out: the artifacts are listed file by file (no globs over `build/`). Add a step that fails if any
  `*.VOL|*.ELF|*.iso|SLES_*` is in the package. Nothing in CI can run `--shot` (no data), so make the smoke test
  "starts, finds no VOL, prints the message, exits 1" under `--shot` (also checks the quiet-message fix of phase 1).
- Optional: a `res/rampagere.rc` with VERSIONINFO + icon, as WoodyRE uses for the tag/version check and the APK version.

**Risks**: none to speak of. Pin actions by major version.

## Phase 4: Texture packs (small)

**WoodyRE files**: `src/texpack.c` (205) and `.h` (18), `src/stb/stb_image.h`, `stb_image_write.h` (copied verbatim),
docs/TEXTURES.md (38).
- Lookup: FNV-1a 64 over a 9-byte header (kind tag, w, h LE32) and the source bytes. Files are
  `mods/textures/**/<anything>_<16 hex>.png` (8 levels deep, first match wins), scanned once at the first upload into an
  open-addressing table.
- `tp_replace(hash, alpha_mode)` uploads the PNG of any size with a full 2×2 box mip chain and trilinear filtering; the
  caller still sets the wrap mode.
- `--dumptex` writes `mods/dump/<scope>/<w>x<h>_<hash>.png` (once per hash).
- Paths are wide on Windows and UTF-8 elsewhere.

**RampageRE hook**: one place, `texture()` in render_gl.c:245-261. It covers world, buildings, damage layers, sphere-map
env textures, water, shadows, particles and the HUD/GUI (all go through `texture(r, b, name)`). The movie frames
(`video_texture`) stay out.
- Hash input: **the decoded RGBA of mip 0 from `tex_decode` plus w, h**, tag `'T'`. The palette is part of the
  picture, and PSMT8 swizzling and CSM1 palette order are already undone, so the same texture in two bundles or cities
  gets one name. (Hashing `data2` raw would split palette variants and depend on the GS-order flag.) This is a
  RampageRE choice; WoodyRE hashes the source texels because its .tex texels are already linear.
- `tp_scope`: the bundle's city (`Chicago`, …), `Shell`, `GUI`, `fx`. Set it in `rnd_scene` / by bundle pointer; keep a
  `const char *scope` per `BundleTex` cache entry.
- Alpha: RampageRE draws with an alpha test at 0.5 and blends see-through parts, and `tex_decode` already maps PS2
  alpha 0x80 to 255 (`alpha2`). Use `TP_ASIS` (PNG alpha as is). The colour-key mode is not needed.
- `.ifl` animated textures: each frame is its own texture, as in WoodyRE.
- `--dumptex` flag in main.c. Docs: a "Texture packs" section in NATIVE.md or a new docs/TEXTURES.md.
- Home folder: next to `rampage_profile.bin` (the current directory) → `mods/`.

**Risks**: tex_decode output must be deterministic (it is pure). Large packs cost load time at the first upload of
each texture; WoodyRE reads lazily per hash, which is fine. Check that `GL_TEXTURE_MAX_LEVEL` (0x813D) exists on GL 1.1
headers (WoodyRE uses the number).

## Phase 5: Graphics settings: settings file, anisotropy, MSAA/SMAA, GTAO (medium-large)

**WoodyRE files**: `src/postfx.c` (367) and `.h`, `src/smaa/smaa.h` (166 KB generated; MIT), `src/gtao.c` (331) and
`.h`, `tools/smaa_embed.py` (optional; needs PIL), stb_image (from phase 4, decodes SMAA's AreaTex/SearchTex PNGs).
docs/DISPLAY.md §5-6 describe them.
- **MSAA** (2/4/8): `postfx_begin(w,h)` binds an own target (multisample renderbuffers RGBA8 + D24S8); `postfx_end`
  resolves it with `glBlitFramebuffer`. Needs GL 3.0 / ARB_fbo.
- **SMAA** 1x (Low/Medium/High/Ultra = SMAA presets): edge detection, blend weights, neighbourhood blending in GLSL 1.30
  (ES 3.00). Runs after the MSAA resolve. It also smooths alpha-tested cut-outs, which matter in RampageRE (facade
  sheets, trees).
- **GTAO** (XeGTAO form): after the opaque geometry, copy depth into a texture (`glCopyTexSubImage2D`; with MSAA a depth
  blit), run a GLSL 1.20 pass (3 slices × 6 steps, Bayer 4×4 noise), apply a 4×4 depth-aware blur, then multiply
  `vis^1.5` into the colour (blend ZERO/SRC_COLOR). Inputs: `gtao_frame(zn, zf, f/aspect, f)`.
- **Anisotropy**: `GL_TEXTURE_MAX_ANISOTROPY_EXT` on level textures.
- Toggles in WoodyRE: `woodyre.cfg` (`ao= aniso= smaa= msaa=`), env overrides (`WOODY_AO` …), and a port menu page
  ("Graphics", 0x42). A `--shot` run ignores the cfg's display keys so test images are reproducible.

**RampageRE changes**
- Settings: RampageRE has no settings file (the profile is the PS2 card layout). Add `rampage.cfg` (key=value, read before
  `win_open`): `window=1280x720 fullscreen=0 vsync=1 ao=0 aniso=1 smaa=0 msaa=0`. Add command-line flags `--ao --smaa N
  --msaa N --aniso N` and env `RAMPAGE_AO/SMAA/MSAA/ANISO`. `--shot` uses flags only (no cfg). An in-game page would
  have to be added to the faithful GUI (Shell.bdp Options / Pause layouts), which is hard. Instead, start with viewer
  hotkeys (free F-keys: F5-F8 cycle AO/SMAA/MSAA/aniso, printed to the console and title) and keep a GUI page as a
  later option.
- `win_mode`/`win_vsync` from WoodyRE (fullscreen F11, window size) come along here (plat_sdl.c and Win32 render_gl.c).
- `rnd_frame` (render_gl.c:754) hooks:
  1. `postfx_begin(w->width, w->height)` before `glClear` (line 764).
  2. `gtao_frame(0.5f, 3000.0f, f/aspect, f)` after the two item passes and the fx debris meshes (after line 846),
     before `draw_water`, decals/wall shadows, particles and spheres.
  3. `postfx_end()` before `draw_hud` (line 888), so the HUD stays sharp. `draw_gui3d` (the shell's 3D monsters) is part
     of the 3D picture; decide whether it counts as 3D (smoothed) or HUD.
- **Must-fix when copying postfx.c**: it presents by binding framebuffer **0** (postfx.c:333-358). In RampageRE's
  hidden `--shot` mode the "window" is the offscreen FBO, so `postfx_begin` must save `GL_FRAMEBUFFER_BINDING` and
  `postfx_end` must blit/draw into that binding (gtao.c already reads the binding at line 267). Otherwise a `--shot` with
  SMAA writes an empty PNG. Also make `offscreen()` and postfx use the same (core/ARB) entry points.
- **Opaque/translucent interleave**: `draw_mesh` draws blended parts (`p->blend || see_through`, render_gl.c:387-397:
  glass, interiors, cloth) and the damage layer inline within the item loop. GTAO after the loop would darken
  translucent surfaces that are already composited. Fix: split `draw_mesh` into an opaque pass and a deferred
  translucent list drawn after `gtao_frame`. That also lines up with the PS2's draw order; check the 0x2A53E0 flush
  order before changing it.
- `RADIUS` (gtao.c: 120 units for a 193-unit Woody) must be re-scaled. RampageRE units are about 1/10 of that (monster
  speed 16 units/s, camera 55 units back), so start at about 3-6 units and make it a cfg key.
- Fog: GTAO multiplies after fog, so distant fogged buildings get darkened. Fade AO by the fog factor (pass
  `fog_start/end` as uniforms), or apply AO before fog is impossible in fixed function. Simplest: scale `vis` toward 1
  with `(z - fog_start)/(fog_end - fog_start)`.
- Anisotropy: `glTexParameterf(GL_TEXTURE_MAX_ANISOTROPY_EXT)` in `texture()`. Today's textures have no mips
  (GL_LINEAR), so anisotropy has little effect unless mips are added. Add a `mips=` option (generate a 2×2 box chain as
  texpack does) at the same time.

**Risks**: GLSL needs a GL 2.1+ driver. The passes must report "not supported" and stay off on GL 1.1 (WoodyRE does this
with `gtao_supported` / `postfx_smaa_supported`). Cost is small (WoodyRE: about 0.1-0.4 ms at 1080p). Driver
differences in the MSAA resolve mean golden PNGs only hold with everything off; that is the `--shot` default.

## Phase 6: Game data straight from the disc image, and a data finder (small-medium)

Needed by Switch and Android, and useful on desktop: the viewer needs only `RAMPAGE.VOL` + `RAMPAGE.ELF`, not
`build.ps1` (Python + VS + the 30-minute recomp).

**WoodyRE files**: `src/datasetup_posix.c` (502: SHA-1, the ISO 9660 + Joliet reader `iso_open/iso_find/iso_pread`
with a 64-bit base offset, the CD search under /media, /run/media, /mnt, XDG home, the Android picker, the Switch SD
folder), `src/datasetup.c` (276: Windows drive scan, BCrypt SHA-1, %LOCALAPPDATA%), `src/datasetup.h`, `src/datafiles.h`
(manifest).
- **Simplify for RampageRE**: two files instead of 232. Option A (recommended): `vol_open` accepts an `.iso` and finds
  `RAMPAGE.VOL` in it with the ~60-line ISO reader, and `Vol` gets a `base` offset (vol.c:47 adds it to every
  `_fseeki64`; movie.c:257 too). `fx_open` likewise reads `RAMPAGE.ELF` from the ISO. No copy at all: point the viewer at
  the user's ISO. Option B: copy the two files out (WoodyRE style), needed where the ISO cannot stay open (Android
  content URIs).
- `data_find()`: argv, `RAMPAGE_VOL`, `build/disc/RAMPAGE.VOL`, `./RAMPAGE.VOL`, `data/`, any `*.iso` next to the exe,
  `$XDG_DATA_HOME/RampageRE`, `%LOCALAPPDATA%\RampageRE`. Check the image is SLES-53800 1.00 (SLES_538.00 in SYSTEM.CNF,
  the README's supported release), by size + SHA-1 of RAMPAGE.ELF.
- **Never in `--shot`**: no CD search dialogs, no message boxes; print and exit.

**Risks**: PS2 DVDs are ISO 9660 + UDF bridge; the ISO 9660 tree is there (tools/iso9660.py already reads it). Files
above 4 GB do not occur (the VOL is 1.34 GB, u32 offsets).

## Phase 7: The OpenGL ES path (medium-large; prerequisite for Switch and Android, testable on Linux)

**WoodyRE files**: `src/gles/GL/gl.h` (149) + `src/gles/gles2.c` (338). This is a fixed-function emulation on ES 2.0/3.0
with one shader pair: matrix stacks, two texture units with MODULATE/REPLACE/ADD/DECAL/COMBINE ×2, alpha test, client
arrays as attributes, glBegin/glEnd as arrays, `GL_UNSIGNED_INT` indices on bare ES 2, PushAttrib emulation, and
pass-through for the postfx/gtao programs. `GLES=1 ./build.sh` builds it for a Linux PC (`-Isrc/gles -lGLESv2`).
`plat_gl_proc` → `gles_proc`.

**What RampageRE needs on top** (the WoodyRE shim says "lighting and fog are not there"). Comparing the GL calls of
`src/*.c` with the shim:
- **Lighting**: `glLightfv` (POSITION as world direction under the current modelview, DIFFUSE, AMBIENT, SPECULAR=0),
  `glLightModelfv(AMBIENT)`, `glLightModeli(TWO_SIDE, FALSE)`, `GL_COLOR_MATERIAL` (AMBIENT_AND_DIFFUSE),
  `GL_NORMALIZE`, `glNormalPointer`/`glNormal3f`, up to 4 directional lights. Per-vertex in the vertex shader:
  `col = vcol·(ambient + Σ dif·max(N·L,0))`.
- **Fog**: `glFogi(LINEAR)`, `glFogf(START/END)`, `glFogfv(COLOR)` → eye-distance fog in the fragment shader;
  `glGetFloatv(GL_FOG_COLOR)` must answer (render_gl.c:683).
- **Sphere-map texgen** (`glTexGeni(GL_S/T, GL_SPHERE_MAP)`, `GL_TEXTURE_GEN_S/T`, render_gl.c:426-429): the environment
  maps. Compute the reflection-based UV in the vertex shader.
- `glGetFloatv(GL_MODELVIEW_MATRIX)` (render_gl.c:676), `glPushClientAttrib/glPopClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT)`,
  `glShadeModel` (no-op), `GL_LIGHTING_BIT` in `glPushAttrib`, `GL_LINE_LOOP` via glBegin (ES has it).
- Write the shader as an uber-shader with uniforms (light count, fog on, texgen on), or a few variants.

**Build**: `build_viewer.sh` gains `GLES=1`. A Linux `GLES=1` `--shot` compared against desktop GL is the test before any
device (WoodyRE checked < 0.02 per pixel on llvmpipe).

**Risks**: this is the biggest unknown. The renderer must not change on desktop. Precision: use `highp` for depth
samplers (WoodyRE hit 8-bit banding with lowp). On ARM `char` is unsigned, so add `-fsigned-char` (WoodyRE does it for
Switch and Android).

## Phase 8: Nintendo Switch homebrew (medium, after 6-7)

**WoodyRE files**: `build_switch.sh` (31). devkitA64 + libnx + switch-sdl2 + switch-mesa (`-lSDL2 -lGLESv2 -lEGL -lglapi
-ldrm_nouveau -lnx`), `-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE -fsigned-char -D__SWITCH__`, falls
back to the `devkitpro/devkita64` Docker image when `$DEVKITPRO` is unset, `nacptool` + `elf2nro` with
`res/switch_icon.jpg`. Also the plat_sdl.c `__SWITCH__` bits (fullscreen 1280×720, error applet for `plat_message`)
and datasetup_posix.c's Switch branch (`sdmc:/switch/woodyre`, the libnx console for unpacking progress, and the
`PadState` name clash with libnx avoided by including only `switch/applets/error.h`). CI job: `container:
devkitpro/devkita64`.

**RampageRE**: `build_switch.sh` with the SRC list + `src/gles/gles2.c`; data under `sdmc:/switch/rampagere/`. Either
the ISO itself (phase 6 option A: read in place, no unpacking) or `RAMPAGE.VOL` + `RAMPAGE.ELF`. `rampage.log` next
to it (WoodyRE's `WOODY_GUI` log-to-file define).
- Pads: SDL lists 8 player slots on the Switch. Joy-Con pairs, handheld and the Pro Controller map through pad_sdl. For
  4-player, support single horizontal Joy-Cons (libnx `hidSetNpadJoyAssignmentModeSingle`); a single Joy-Con has no
  right stick and only 4 face buttons, so Rampage needs Cross/Square/Triangle/Circle on its face buttons and roar on SR.
- **Memory**: the viewer reads VOL files into malloc. Homebrew in applet mode (Album) gets roughly 400 MB, so require
  title takeover (hold R on a game), as WoodyRE's README says. Measure peak RSS on Linux first.

**Risks**: memory and GPU speed (Tegra X1 under Mesa nouveau, with fixed-function emulation and 4 lights per vertex).
The FAT32 4 GB limit only matters if the ISO is above 4 GB; check the image size.

## Phase 9: Android (large, after 6-7)

**WoodyRE files**: `android/` (Gradle 8.13 plugin, compileSdk 35, minSdk 24, NDK 27.2, ABIs arm64-v8a + x86_64,
16 KB page sizes), `android/app/CMakeLists.txt` (26: FetchContent SDL 2.32.10 with SHA-256, `libmain.so`,
`main=SDL_main` on the main file, `-fsigned-char`, links `GLESv2 GLESv3 log android`),
`AndroidManifest.xml` (GLES 2.0, gamepad/touch optional, VIBRATE, landscape), `WoodyActivity.java` (184: SDLActivity
subclass; the system file picker for an ISO (fd handed to C) or a folder; progress dialog; `args`/`env` intent extras
for testing), SDL's Java glue `org/libsdl/app/*` (zlib), `src/touch.c/.h` (180/13: floating left stick, A/B/X/Y +
LB/RB + Start, drawn after the frame, hidden when a real pad is used), plat_sdl.c `__ANDROID__` bits (`plat_dialog` via
JNI, logcat tee, back button = Esc), datasetup_posix.c's Android branch (app folder, ISO through the picker's fd, bundled
`assets/game.iso`), the signing secrets in CI, and optionally `make_android_bundle.bat` + `tools/native/apkbundle.c`.

**RampageRE**:
- Rename to `io.github.jjmhalew.rampagere`, `RampageActivity`. CMakeLists with RampageRE's SRC + gles.
  `main.c` → `SDL_main`.
- Data: the picker hands an ISO fd. Content URIs cannot stay open across runs without
  `takePersistableUriPermission`, so **copy `RAMPAGE.VOL` + `RAMPAGE.ELF` (1.35 GB) out of the ISO** into
  `getExternalFilesDir()`, check the free space first, and verify by SHA-1 (phase 6 option B).
- Touch (`touch.c`): Rampage's layout is the left stick (move), four face buttons Cross (jump), Square (punch),
  Triangle (kick), Circle (grab), R2 (roar) and Start. The right stick (head) is optional: a right-half drag area.
  Menus go through the pad mapping of phase 2.
- `--shot` on Android: no. Test with `am start ... -e args "... --shot /sdcard/..."` only for manual checks.
- Skip the bundle APK (1.3 GB+ assets) unless asked.

**Risks**: APK signing continuity (WoodyRE's CI notes: the debug key changes per run, so updates fail and uninstalling
loses the copied data and saves). Store the profile in the app folder. GPU/driver variety on ES 2.0 vs 3.0: postfx/GTAO
only on ES 3.0. Phone performance with 4 players and cloth.

---

## Notes on the "never open a window in tests" rule (`--shot` renders offscreen)

What to preserve or change from WoodyRE:
1. **Keep the Win32 window on Windows** (WoodyRE does too). Do not move the Windows build to SDL; the current
   `win_open(hidden)` path is already right.
2. **SDL `win_open(hidden)`**: `SDL_WINDOW_HIDDEN` + FBO, never shown. If no FBO, fail; do not fall back to showing.
   Headless: `SDL_VIDEODRIVER=offscreen`. WoodyRE's own SDL `win_open` has no hidden mode, and its Linux testing
   (PLATFORM.md §2: WSLg windows driven by xdotool) opens windows. Do not adopt that.
3. **Message boxes**: WoodyRE shows `SDL_ShowSimpleMessageBox`/`MessageBoxA` for errors and for the first-start data
   setup. In RampageRE, every `plat_message` in a `--shot` run goes to stderr. This is already a gap today: a missing
   VOL pops a MessageBox even with `--shot`.
4. **Data setup** (phase 6): no CD search prompts, pickers or progress windows in `--shot`.
5. **Pads** (phase 2): not opened in `--shot` (no light-bar/rumble writes; deterministic input).
6. **Audio**: never opened in `--shot` (as now; keep it so in the SDL backend).
7. **postfx/GTAO**: restore the caller's framebuffer binding instead of 0, and `--shot` ignores `rampage.cfg` (flags only),
   like WoodyRE's "a screenshot run ignores the cfg's display keys".
8. **Fullscreen/F11** (`win_mode`) must be a no-op for a hidden window.

## Summary table

| # | Phase | WoodyRE sources | RampageRE touch points | Size |
|---|---|---|---|---|
| 1 | SDL2 layer + Linux build | plat.h, plat_sdl.c, build.sh, audio.c (SDL part, for reference) | plat.h, render_gl.c/.h (`#ifdef _WIN32`, Window fields), sound.c `#else`, build_viewer.sh, main.c quiet messages, NATIVE.md | medium |
| 2 | Controllers | pad.h, pad.c, pad_sdl.c, docs/INPUT.md §6 | main.c (`keys_pad`, `shell_frame`, pause/results, DRIVE_PAD, rumble calls), render_gl.c (WM_DEVICECHANGE, focus), build_viewer.bat LIBS | medium |
| 3 | CI | .github/workflows/build.yml | new workflow (windows + linux; later switch, android), optional res/rampagere.rc | small |
| 4 | Texture packs | texpack.c/.h, stb/, docs/TEXTURES.md | render_gl.c `texture()`, main.c `--dumptex`, scope per bundle | small |
| 5 | Graphics settings | postfx.c/.h, smaa/smaa.h, gtao.c/.h, DISPLAY.md §5-6, win_mode/win_vsync | rnd_frame hooks, draw_mesh opaque/translucent split, rampage.cfg + flags + hotkeys, FBO-binding fix, AO radius/fog | medium-large |
| 6 | Data from the ISO | datasetup_posix.c (ISO reader, SHA-1), datasetup.c, datasetup.h | vol.c (`base`), movie.c, fx.c, main.c data_find | small-medium |
| 7 | OpenGL ES path | src/gles/GL/gl.h, gles2.c | extend shim: lighting, fog, sphere-map texgen, normals, client-attrib, getFloatv; GLES=1 build | medium-large |
| 8 | Switch | build_switch.sh, plat_sdl.c/datasetup `__SWITCH__`, res/switch_icon.jpg, CI job | build_switch.sh, data folder, Joy-Con single mode, memory | medium |
| 9 | Android | android/ (Gradle, CMake, manifest, activity, SDL glue), touch.c/.h, plat_sdl.c `__ANDROID__`, CI job | renamed project, copy-out data setup, Rampage touch layout | large |
