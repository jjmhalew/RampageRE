# Rampage: Total Destruction (PS2, SLES-53800) – PS2Recomp bring-up notes

State on 2026-10-08: the recompiled game boots natively on Windows, runs all C++ static constructors,
loads its IOP modules, sets up the GS display, feeds VIF1 through MFIFO, runs `RApp::Init` and reaches the
front-end menu code, which stops because the game's data archive was never mounted (see "Current
blocker"). No picture yet.

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

## Current blocker
Being checked: with the directory functions in place, does the volume find `RAMPAGE.VOL;1`, and does the
MultiStream RPC give each file a position? Before that fix the menu code crashed calling through a null
GUI scene (0x345F1C, `profile_arrow_down` / `btn_select` screen) because nothing had been loaded.

## Debug aids
- `tools/dis.py ADDR`: disassembly with call names and strings; `tools/xref.py callers|callees ADDR`.
- `tools/trace_fn.py ADDR...`: logs entry into generated functions (`[enter]` lines).
- `tools/bind_stub.py ADDR HANDLER "why"`: bind a function to a runtime handler without a regenerate.
- `test.ps1`: runs the game for N seconds; the runtime writes a `[status]` line and `shot_NNN.png` in
  `build/disc` every ~10 s (temporary bring-up code in the runtime patch).

## Not tried yet
- Graphics: the runtime's GS/VU path is partial and slow ("very bad for VU and GS" in its README).
  Whether Rampage's VU1 renderer works is the big unknown.
