# Rampage: Total Destruction (PS2, SLES-53800) – PS2Recomp bring-up notes

State on 2026-10-07: the recompiled game boots natively on Windows, runs all C++ static constructors,
loads its IOP modules (sio2man, libsd, MultiStream V7.1), initialises file I/O and gets into its first
zlib asset decompression. It stops there (see "Current blocker"). No picture yet.

## The disc
- `SLES_538.00`: main ELF, Metrowerks CodeWarrior MIPS 2.4.1.01, stripped, one RWX segment that holds
  code and data together. About 7,500 functions (8,818 in the current map, including small thunks).
- `RAMPAGE.VOL`: PVOL archive, 352 files with their original names (`pvol.py` lists/extracts it):
  `.bsf` ("Comp", 138), `.bdp` ("Pipe", 57), `.zip` (real PKZIP, 60, animations), `.cmp` (60),
  `.pss` (21 MPEG-2 videos), `.mic` (11 audio streams), `.samp`, `.uber`, `.txt`.
- `DESI/JARVOS.ELF` + `M1.ELF` + `*.SR`: emulators for the unlockable arcade Rampage games (skip).
- 0x430000–0x443b50 also holds a table of script-binding names (`ChallengeStatus::updateBossHealth`,
  `getNumPlayers`, ...): useful for naming functions later.

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

## Current blocker
`jalr 0x11111111` at 0x29F3F4: zlib 1.1 `inflate_flush`, `s->checkfn` (offset 0x3C, adler32 = 0x29CCD0)
has been overwritten with 0x11 bytes after a `memcpy` from the inflate window. Leads to check:
- the three `__udivmoddi4` bindings (0x143030, 0x1436F8, 0x143DC8): the SCE DB gave the same name to
  three functions; EE code passes 64-bit values in single registers;
- the game's zalloc and the window/output buffers it hands to inflate.

## Not tried yet
- Graphics: the runtime's GS/VU path is partial and slow ("very bad for VU and GS" in its README).
  Whether Rampage's VU1 renderer works is the big unknown.
