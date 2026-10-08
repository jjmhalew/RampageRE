# Rampage: Total Destruction (PS2, SLES-53800) – PS2Recomp bring-up notes

State on 2026-10-08 (night): the recompiled game boots natively on Windows, goes through the front end
(legal screen, autosave notice, intro movies, attract mode, menus, profile, monster select) and plays the first
campaign level (Las Vegas). See "Current state" for what is still wrong.

## The disc
- `SLES_538.00`: main ELF, Metrowerks CodeWarrior MIPS 2.4.1.01, stripped, one RWX segment that holds
  code and data together. About 7,500 functions (8,818 in the current map, including small thunks).
- `RAMPAGE.VOL`: PVOL archive, 352 files with their original names (`pvol.py` lists/extracts it):
  `.bsf` (138 compiled scripts, most of the game logic: decompiled by `tools/bsf.py`, see
  [FORMAT_BSF.md](FORMAT_BSF.md)), `.bdp` and `.cmp` ("Pipeworks Bundle File version 1.3", 57 + 60, see
  [FORMAT_BDP.md](FORMAT_BDP.md)),
  `.zip` (real PKZIP, 60: each holds a byte-identical copy of the `.cmp` of the same name),
  `.pss` (21 MPEG-2 videos), `.mic` (11 audio streams), `.samp`, `.uber`, `.txt`.
- `DESI/JARVOS.ELF` + `M1.ELF` + `*.SR`: emulators for the unlockable arcade Rampage games (skip).
- 0x430000–0x443b50 also holds a table of script-binding names (`ChallengeStatus::updateBossHealth`,
  `getNumPlayers`, ...): useful for naming functions later.
- Metrowerks RTTI is intact: about 516 vtables with class names (`RApp`, `PS2PreEnumFileSystemVolume`,
  `GuiScreenJunction`, `RWorldPackage`, ...). A vtable is `{typeinfo, 0, methods...}` and typeinfo starts
  with a pointer to the class name. That names most virtual methods for free.

## Approach
Hybrid, like WoodyRE: the PS2Recomp build runs the original code and is the reference; formats get documented
(`docs/FORMAT_*.md`) with tools verified byte for byte on every disc file, and parts of the game are then
replaced by readable native code (scripts and data first, then the renderer, then game systems).

## Program flow (what is known)
- `main` = 0x1A7A70: SIF/CD init, IOP reset with `IOPRP300.IMG`, module loads, GS/DMA setup (0x267980),
  constructors (0x13C750), then the app object: `vtbl+0x24` in a loop, `vtbl+0x0C` (`RApp` 0x2A33B0, init),
  `vtbl+0x10` (run), `vtbl+0x14`, `vtbl+0x18`.
- `RApp` init mounts `RAMPAGE.VOL` through 0x10C380: it asks the `cdrom0:` volume
  (`PS2PreEnumFileSystemVolume`) for `RAMPAGE.VOL;1` and silently skips the mount if it is not found.
- `PS2PreEnumFileSystemVolume` lists the disc with `sceDopen`/`sceDread` on `cdrom0:` into a 97-bucket hash
  table at start-up; for each file `PS2PEFSVProcessor` asks the MultiStream library (EE side 0x133740,
  0x135170, 0x135690; RPC to `STREAM.IRX`) for its position.
- Rendering: the game streams display lists with MFIFO: fromSPR DMA (ch 8) writes packets into a ring buffer
  at 0x500000, VIF1 drains it in chain mode, and the game waits on `D1_TADR` for free space.

## Memory map of the ELF (found by code-density scan)
| Range | Contents |
|---|---|
| 0x100000–0x349910 | `.text` (game, SCE libs, newlib, MW runtime) |
| 0x349910–0x435d00 | data |
| 0x435d00–0x443b50 | C++ static constructors (`__sinit_*`, called through the table below) |
| 0x443b50–0x443fb0 | constructor table, 280 entries |
| 0x443fb0–0x44ca80 | data |
| 0x44ca80–0x4d8600 | bss |

## Pipeline (`build.ps1` runs all of it)
1. `tools/iso9660.py` extracts the disc to `build/disc` (the runtime serves `cdrom0:` from the ELF's folder).
2. `tools/split_elf.py` writes `build/disc/RAMPAGE.ELF` with real `.text`/`.data` sections. Without this
   the recompiler treats data as code (22k errors).
3. `recomp/config.toml` came from `ps2_analyzer` (SCE/newlib stub bindings) and was edited by hand.
4. `recomp/funcmap.csv` (Ghidra CSV format, no Ghidra needed) came from `tools/funcmap.py`: jal targets,
   code pointers in data (vtables), every constructor-table entry, lui/addiu-built code addresses, and
   frame-opening functions after `jr $ra` / tail-call `j`, plus what PS2Recomp's analyzer found itself.
5. `ps2_recomp recomp/config.toml` writes C++ into `PS2Recomp/ps2xRuntime/src/runner/`; the two
   `ps2_recompiled_*.h` headers move to `ps2xRuntime/include/`.
6. Build `ps2EntryRunner` (Release) in `build/cmake`. A full build takes about 30 min. MSBuild does not
   notice regenerated sources inside unity bundles, so the script deletes the bundle objects first.
7. Run `ps2EntryRunner.exe RAMPAGE.ELF` from `build/disc`.

To try a fix on one function without a full regenerate: replace its generated `sub_XXXXXXXX_0x....cpp` by
a call to a runtime handler, delete the `unity_N_cxx.obj` that includes it, rebuild (a few minutes), and
add the binding to `recomp/config.toml` so the next regenerate keeps it.

## Fixes so far
| Problem | Fix |
|---|---|
| Code and data in one segment | `split_elf.py` |
| Constructors only reachable through the ctor table | `funcmap.py` |
| `_realloc_r` (0x32BA98) native while `_malloc_r`/`_free_r` are runtime stubs, so it read a chunk header that does not exist and called memcpy with size -4 | bound to `realloc_r` (`0x32BA70` is `realloc`; the SCE symbol DB calls it "setlocale") |
| `sceFsSemInit` and friends were TODO stubs that throw | return 0 (file I/O is done on the host) – `patches/ps2recomp-runtime.diff` |
| Other TODO stubs (Deci2, Tty, DMA recv, MPEG RAW8) threw | `TODO_NAMED` now warns and returns 0 (bring-up only) |
| `sceFsSifCallRpc` (0x14B290) called the IOP fileio server with an unbound client | `ret0` |
| `_memalign_r` (0x3293E8) trims newlib chunks while malloc/free are runtime stubs: it freed the block it returned, so later allocations overlapped (zlib state overwritten with 0x11 fill) | bound to `memalign_r` (`0x3293C0` is `memalign`, DB name "setlocale" again) |
| VIF1 MFIFO (`D_CTRL.MFD = 2`) not emulated: VIF1 ran the empty ring at once and the game waited forever on `D1_TADR` | MFIFO in the runtime: fromSPR wraps in `RBOR/RBSR`, VIF1 drains up to fromSPR's `MADR` and keeps running |
| `sceDopen/sceDread/sceDclose` missing in the runtime, so the disc listing was empty and `RAMPAGE.VOL` was "not found" | implemented on the host (CD names upper case with `;1`, iomanX modes) and bound at 0x14D688/0x14D8B8/0x14D750 |
| `PMFHL.LW` packed the wrong halves (newlib `strtod` looped forever) | `ps2_pmfhl_*` helpers in `ps2_runtime_macros.h` |
| `STREAM.IRX` calls an EE RPC server (sid 0x12344321) from the IOP; the IOP side had no client for EE servers | IOP→EE RPC: `BindRpc` to an EE server, `CallRpc` queues a guest invocation of the server function and wakes the IOP thread with the reply |
| VIF1 PATH2: after a `DIRECT` whose GIF tag starts an `IMAGE` transfer, the runtime took the next raw bytes of the VIF stream as pixels. Rampage sends the pixels in the next `DIRECT` (`NOP`, `DIRECT 0x40`, data), so the interpreter swallowed VIF codes and parsed texture data as commands (opcodes 0x2A/0x55) | the image continuation is taken from the next `DIRECT` payload |
| VIF1 commands whose payload continues in the next DMA packet or MFIFO drain were dropped | incomplete commands are carried over to the next transfer |
| `vfprintf` (0x32EFC8) is bound to the runtime stub, which only knows host `FILE`s; newlib's `vsnprintf` passes a string `FILE` on the stack, so every `snprintf` in the game produced "" | the stub writes into newlib string `FILE`s (`_flags & __SSTR`, `_p`/`_w`) |
| the runtime read `va_list` arguments 4 bytes apart; Metrowerks spills variadic registers with `sd`, so each takes 8 (`%s[]%i` gave `Color[]0` for `Color[4]`) | 8-byte slots (`PS2X_VA_SLOT=4` for the old behaviour) |

The `snprintf` bug was the one that kept the front end from starting: the script compiler names array types
with `snprintf("%s[]%i")`, every array type got the name "", `float[]` resolved to `GuiObject[]`, binding
failed for every script that uses arrays ("Error: Binding failed." from the PP compiler), `Shell.bsf` was
deleted, the shell stayed in state 1 (memory card check) forever and only the legal screen was drawn.

| PS2Recomp `dispatchGuestBranch`: when a callee is unwound to the scheduler (tail jump or checkpoint) with `ctx->pc` equal to the entry of a call that is still pending further up, that call took it for a return and carried on with the inner frames' stack pointer. Rampage's script interpreter is recursive (0x1bf3b0 → 0x1c0430 → 0x1bf3b0) and tail-jumps to 0x1c0430, so it returned to a heap address | an explicit unwinding flag (`g_ps2GuestUnwinding`), cleared by the scheduler before it enters guest code |

| `sceVu0MulMatrix` stub multiplied in the wrong order (libvu0 computes m0[i] = Σ m1[k]·m2[i][k], i.e. M1×M2 for column vectors); every GUI vertex came out at x ≈ 2 | operands swapped |
| the pad backend (`PSPadBackend`) is always used, so `PS2X_AUTOPRESS` in the stub never applied | autopress moved into the backend (Start, then Cross, held for a second every 6 s) |
| `sceMpegGetPicture` waited for frames that only the waiting thread can feed: Rampage's player feeds libmpeg from its `sceMpegCbNodata` callback | the stub calls the registered Nodata callback (two per vsync, then waits a frame for the stream) |
| that callback polls for stream data that arrives through the IOP→EE RPC, which was never started inside another invocation | RPC-callback invocations may nest on top of another RPC-callback invocation |
| `sceMpegReset` after the first chunk dropped the movie's only sequence header, so FFmpeg never got one | the last sequence header is remembered and put in front of the next GOP |
| movie end was only known from `sceCdSt` EOF; the player asked for data past the end of the file forever | the video's sequence_end_code with no pictures left ends the stream (`GetPicture`, `sceMpegIsEnd`) |

| the player ends a movie on `sceMpegIsEnd` (0x15EC98, reads `mp->sys` internals the stubs never update) | bound to the runtime stub, like `sceMpegIsRefBuffEmpty` (0x15ECA8) |
| after the last picture `sceMpegGetPicture` returned 0 as if there was more | returns -1 once a game-fed stream ended |
| `sceMpegReset` kept a stream "ended", and the next movie's first chunk (read before `sceMpegCreate`) was dropped with its sequence header | sequence headers are remembered from the raw PSS data; reset starts over after a sequence end |
| DMA chain tags with ADDR bit 31 (SPR: data in scratchpad) were read from main RAM (the generic chain code masked the bit, the MFIFO drain kept it), so VIF1 parsed EE code as VIF commands and PATH3 stayed masked | the SPR bit maps the address to the scratchpad |
| libmpeg runs stream callbacks (`sceMpegAddStrCallback`) inside `sceMpegDemuxPss`; the runtime queued them for later. Rampage copies the video into its own ring in that callback and parses the sequence header right after the demux, so it read the previous movie's bytes and asked for 25 MB | the demux stubs run the callbacks before returning (`invokeCurrentSequence`) |

| the GIF keeps a path's tag open across transfers; the runtime started every PATH2/PATH3 transfer as a new packet. Rampage sends an A+D tag with one `DIRECT` and the blend template (ALPHA, TEST) through the next DMA REF, so ALPHA_1 was read as a GIF tag and stayed 0x1 (inverted blend: grey boxes behind text, near-black movies) | `GifArbiter::submit` keeps each path's open tag and continues it in the next transfer (this replaces the VIF-side IMAGE continuation) |

## Current state: in game
`.\test.ps1 -ToGameplay` drives the pad through the front end (autosave notice, movies, main menu, Create New
Profile, name "AA", campaign, monster select with George) and the first level loads and runs: Las Vegas with
the HUD (lives, timer, score), the city blocks, the "Welcome to Vegas" sign and the opening Upgrade Challenge.
What is still wrong or missing:
- the ground renders black and the monster is hard to make out (3D renderer issues to look at);
- the frame rate is low (about 8 game frames per second with the interpreted VU1);
- no audio.

How the GUI renders: VU1 microprogram at 0x14d8/0xf70 transforms GUI vertices with a matrix at VU data
0x89..0x8c (built on the EE with libvu0, see `sceVu0MulMatrix` above) and kicks packed `XYZF2` triangle
strips. `tools/vudis.py` disassembles VU micro code and decodes `PS2X_VUTRACE=1` traces (`build/disc/vutrace.txt`,
plus `vucode_*.bin`/`vudata_*.bin` snapshots of the first two runs of each microprogram).

Before that: `Shell::onInit` loads `VShell`, `Shell` (7 MB) and `ShellData`, the `ControlRoom` and
`AttractMode` interfaces load, and the attract mode walks its states (`BootCheck`, `ShortDelay`,
`MidwayVideo`, `PipeworksVideo`, `DigitalVideo`, `BinkStill`, `IntroVideo`, ...); the videos are `.pss`
files played through `GuiQuad::setVideo`.

## How the script system works (PP language)
- `.bsf` = "Compiled PPTokenStream": tokens of preprocessed source. At load time the game parses them with a
  bison parser (`yyparse` 0x1c5cb0, error reporter 0x1C61F0, `%s(%i) Error: %s`), binds symbols (0x1C0810,
  node classes `P*Entry`/`P*Node` in RTTI) and executes the tree (AST interpreter, no generated code).
- Script load: `GuiScript::load` 0x2344E0 → open via the file system 0x10C220 → parse/bind 0x1D4870.
  Debug output goes to 0x1C6110, which is an empty function in the retail build; probing its arguments
  shows the compiler's errors (`Cannot reconcile types %s and %s in expresssion.` etc.).
- Shell: `RShellPackage` (vtable 0x44b170) init 0x309500 creates the `SaveGame` popup (0x4d7e80, loads
  `MemoryCard.BDP`/`.ifc`), then 0x308950 loads `Shell.bsf` and calls `Shell::onInit`. Shell state lives at
  0x4d51f0 (1 = memory card check, 2 = attract mode, ...), updated by 0x308710 every frame.
- Stream loader (EE side): requests are queued and sent with `sceSifCallRpc` (0x135690, client 0x450040,
  0x800-byte buffers); `STREAM.IRX` polls the EE server 0x132030 for the next command.

## Debug aids
- `tools/dis.py ADDR`: disassembly with call names and strings; `tools/xref.py callers|callees ADDR`.
- `tools/trace_fn.py ADDR...`: logs entry into generated functions (`[enter]` lines).
- `tools/bind_stub.py ADDR HANDLER "why"`: bind a function to a runtime handler without a regenerate.
- `test.ps1`: runs the game for N seconds; the runtime writes a `[status]` line and `shot_NNN.png` in
  `build/disc` every 600 frames (`PS2X_SHOT_EVERY`). `-PadInput "12:cross;20.5:down/0.3"` scripts the pad
  (`PS2X_INPUT`), `-ToGameplay` plays into the first level. With the status line:
  `[thread]` lines with a heuristic stack backtrace (`[stack]`), a VIF1 opcode histogram (`[vifops]`),
  `PS2X_WATCH=0x4d51f0,gp-0x70a4,...` prints guest words (absolute or relative to the main thread's
  `$gp`) and `PS2X_DUMPRAM=1` writes the EE RAM to `build/disc/ram.bin`.
- IOP: `[iop-import]` traces every IOP import (60 lines per import), `[iopthreads]` dumps IOP threads,
  semaphores and event flags every ~10 s of IOP time, `[cdread]` names the host file of every CD read.

## Not tried yet
- Graphics: the runtime's GS/VU path is partial and slow ("very bad for VU and GS" in its README).
  Whether Rampage's VU1 renderer works is the big unknown.
