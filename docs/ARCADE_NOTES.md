# The Midway Classics: the arcade games (after v1)

Bonus Content's "Midway Classics" box offers the two arcade Rampage games. They are not part of Rampage: Total
Destruction itself but separate PS2 programs on the same disc (Digital Eclipse's emulators, their Midway Arcade
Treasures 2 framework "MAT2"), so they are left for after v1: the port's menu offers them as the PS2 does, and taking
one returns to the menu (`SHELL_EVT_EMULATOR`). This page keeps what a first look found, for when they are done.
[R] means read in the code or the files, [G] a guess (here mostly from what MAME knows of the hardware).

## The menu [R MainMenu.scr 356-363, 450-486]
- Main menu → Bonus Content (button 0, Square) → `RetroGames`: 0 **Rampage** (1986, `rg_rampage1`), 1 **Rampage
  World Tour** (1997, `rg_rampage2`); `RetroGamesNTSC` adds 2 **Ballers**, a movie (`ballers.pss`), not a game.
- Always offered, not unlocked: `processRetroGames` calls `gShell.startEmulator(nRetroGame)` after 0.25 s, no check.

## The hand-over [R]
- `startEmulator` (the binding's case at 0x3063A0): Rampage runs `desi/m1` with `-nextelf -nextarg -reentry -game
  rampage`, World Tour `desi/jarvos` with `-game rwt`. 0x308BB0 makes `cdrom0:\DESI\<NAME>.ELF;1` (or `host0:`), adds
  `-cd` / `-dvd` and calls LoadExecPS2.
- Back: the emulator LoadExecs the boot ELF again; RApp's argument handler 0x2A3350 sees `-reentry`, sets the shell
  state 0x4D51F0 to 7 and copies the next argument (15 characters) to 0x4D79B8, the profile's name [R; that 7 is the
  main menu: G].

## The programs (DESI/, GCC / C++, stripped; source paths `C:/projects/Rampage/source/emutech/...`)
| File | Game | CPU core | Size |
|---|---|---|---|
| `M1.ELF` (`emutech/M1_Unit`) | Rampage, Bally/Midway MCR-3 [G] (Z80 main CPU, "Sounds Good" 68000 sound board) | **Z80**: a 9 × 256 handler table at 0x161AE0 [R from its structure: CB, DDCB, FDCB with 256 handlers; DD, FD 89; ED mostly the illegal stub 0x10B614], 0x10B600 .. 0x115400 (962 functions) | `.text` 0x49AD4, ~2,100 functions; the machine and game code 0x100000 .. 0x10B600 (~284) |
| `JARVOS.ELF` (`emutech/ZYTWunit`, `emutech/TMS34010`) | Rampage World Tour, Midway Wolf unit [G] (TMS34010 at 50 MHz, DCS sound on an ADSP-2105) | **TMS34010**, a predecoding interpreter [R]: 0x107DB4 predecodes 12 bytes per opcode word, two 64K tables (handler, decoder) at gp − 0x7E08 / − 0x7E04, ~200 (handler, decoder, pattern, mask) entries registered at ~0x118A98 .. 0x11A800; handlers 0x10D780 .. 0x1167C8, decoders .. 0x118610 | `.text` 0x5EE38, ~2,100 functions; "Registering additional RWT MACHINE ops", three program addresses patched at 0x107F9C (0xFFD1FB60, 0xFFD1FE20, 0xFFD1EDA0: idle loops? G); "Wolf DMA fix" |

Both name other games of the framework (Defender, Robotron, Joust, ... in M1; NARC, Total Carnage, MK1-3 in JARVOS);
the disc ships only Rampage's sets. **Neither emulates the sound board** [R]: both play recorded samples by the sound
command (M1: `rmpg_%02x.wav`, 85 samples loaded by 0x10B120 from RAMPAGE.SR, the code list at 0x14B148; JARVOS:
`Sound65`, `rmpgwt-XXXX.wav`, a talk queue, fades, streaming). The sound boards' ROMs are not on the disc.

Also in DESI/: IOPRP300.IMG and the IOP modules CDVDSTM, LIBSD, MCMAN, MCSERV, MTAPMAN, PADMAN, SDRDRV, SIO2MAN.

## The data: `.SR` files [R]
A text header line `count 80 headersize\n`, then `count` lines of 80 characters, `offset size type rate bits name`
(offsets from the header's end; type 100 raw data, 1 PCM: raw 16-bit mono, no RIFF header).
- `M1.SR` (354,304 bytes): `rampage.dat`, 0x56000 bytes: the Z80 program raw at 0 (`C3 00 01`, JP 0100h) [R]; about
  0x10000 .. 0x50000 the 4-bit sprite graphics, 0x50000 .. 0x56000 not known [G].
- `RAMPAGE.SR` (3,155,968): the 85 samples rmpg_08 .. rmpg_72, 7,500 or 17,000 Hz.
- `RWT.SR` (38,162,432): `RampageWorldTourEerom.rom` (48 KB), `RampageWorldTour.rom` (1 MB of TMS34010 code words),
  `RampageWorldTourImage.rom` (16 MB of graphics), 324 samples `rmpgwt-*.wav` at 11,025 Hz (0003 .. 0009 the music
  loops, ~1.3 MB each).
- `MAT2.SR`: the framework's own front end (pause menu, loading pictures `.ia`) and 4 UI sounds.

Nothing of them is in RAMPAGE.VOL. The port can read them from the disc image in place (`iso_find`, src/iso.c); the
Android first start would copy `DESI/*.SR` too.

## The plan
- **Rampage first, natively** (about 3.5 .. 5K lines): a reused Z80 core with a GPL-3.0-compatible licence (e.g.
  floooh's chips `z80.h`, zlib; superzazu's z80, MIT; MAME's z80 is BSD-3-Clause — check the file headers), the MCR-3
  machine by hand (memory map, NVRAM, the Z80 CTC interrupts, the 32 × 30 background of 16 × 16 tiles, the 32 × 32
  sprites, a 64-entry palette), the sound commands onto RAMPAGE.SR's samples as M1 does (0x10B120 / 0x14B148), the
  inputs and the settings (lives, difficulty, bonus life). Risks: rampage.dat's graphics layout, the CTC timing, what
  each sound command means.
- **World Tour last** (about 8 .. 12K lines): the TMS34010 (MAME's is BSD-3-Clause, ~6-7K lines of C++ with PIXBLT),
  the Wolf DMA blitter, CMOS / EEPROM and the I/O, perhaps the security PIC (Digital Eclipse may have patched it
  out), and the DCS commands onto RWT.SR's samples and music from JARVOS's Sound65 (the riskiest part).
- Not chosen: recompiling M1.ELF / JARVOS.ELF with PS2Recomp (two more runtime builds beside the native port, no
  LoadExecPS2 or libsdr sound in the runtime, not for the Switch or Android); an outside MAME (the disc's sets lack
  the sound ROMs and use Digital Eclipse's packing).
