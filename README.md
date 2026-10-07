# RampageRE

A native PC port of *Rampage: Total Destruction* (PS2, Pipeworks / Midway, 2006), built by statically recompiling
the original game code to C++ with [PS2Recomp](https://github.com/ran-j/PS2Recomp). It runs the game from
**your own disc**: no game files are included in this repository.

> Unofficial fan project for preservation. Not affiliated with or endorsed by Pipeworks, Midway, Warner Bros.
> or Sony. Rampage and related names are trademarks of their owners. You need your own copy of the original game.

## Status

**Early bring-up, not playable yet.** The recompiled game starts on Windows, runs all its C++ constructors,
loads its IOP modules and file system, and stops while unpacking its first compressed asset. There is no picture
yet. See [docs/NOTES.md](docs/NOTES.md) for what works, what was fixed and the current blocker.

| Step | |
|---|---|
| Recompile the whole ELF to C++ (8,818 functions, 0 errors) | ✅ |
| Boot, C++ static constructors (280) | ✅ |
| IOP modules (sio2man, libsd, MultiStream) | ✅ |
| File system | ✅ |
| zlib asset loading | ❌ current blocker |
| Graphics (GS / VU1) | not reached yet |

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

## Repository layout
| Path | |
|---|---|
| `build.ps1` | the whole pipeline: disc → split ELF → C++ → `ps2EntryRunner.exe` |
| `recomp/config.toml` | PS2Recomp config: stub bindings for the SCE libraries and newlib, plus Rampage fixes |
| `recomp/funcmap.csv` | function map (Ghidra CSV format) the current state was tested with |
| `tools/split_elf.py` | splits the retail ELF's single code+data segment into real sections |
| `tools/funcmap.py` | finds function starts without Ghidra (jal targets, vtables, constructors, ...) |
| `tools/pvol.py` | lists or extracts `RAMPAGE.VOL`, the game's data archive (352 named files) |
| `tools/iso9660.py` | extracts a PS2 disc image |
| `patches/ps2recomp-runtime.diff` | runtime changes applied to the PS2Recomp submodule by `build.ps1` |
| `PS2Recomp/` | submodule, pinned to the tested commit |

## License
GPL-3.0, like PS2Recomp. See [LICENSE](LICENSE).
