# Sound: the banks (.uber/.samp), the event programs, 3D sounds, the music (.mic)

The game's sound is in RAMPAGE.VOL: two sound banks, `Rampage.uber` + `Rampage.samp` (728 events, 543 samples, the
game) and `RShell.uber` + `RShell.samp` (27 events, 34 samples, the front end), and 11 music streams `*.mic`.
`tools/sound.py` lists and decodes them; `src/sound.c` plays them. Addresses are in SLES_538.00 (RAMPAGE.ELF).

## Who does what in the game
- **SoundDevice** `gSndDev` at 0x4CF1B0 (script interface: handlers 0x275630.. through the table 0x427110;
  `playSound` 0x26CFC0, `killSound` 0x26B960, `playMusic` 0x26C4D0, `replaceMusic` 0x26BFC0, `set3DData` 0x26D240,
  `setSfx/Voice/MusicVolume` 0x26B5E0). 32 named-sound slots (+0x54, 0x1C each), 2 music slots (+0x3D4), 64 volume
  groups (+0x440: type, +0x444: volume byte; 0x26E0CC sets every group to 0x6A, group 3 = music to 0x3F).
- **Event player** at 0x4CFAB0: instances of 0x3C bytes, run each frame by the interpreter **0x2716E0**; spawn
  0x272170, start by name 0x272050, kill 0x2725C0.
- **Bank list** at 0x4CFB18 (0x38 per bank): `PackageResLoader` 0x28C270 reads `<name>.uber` into memory and
  remembers where `<name>.samp` is on the disc; 0x272AB0 parses the UBER, 0x2736E0 sorts the names, 0x273480
  searches them (case-insensitive, 0x32C9C8), 0x272F20 = PROJ entry by name.
- **3D sound list** at 0x4CFB40 (`add3DSfx` → 0x26D560 → 0x274840), updated by 0x273CF0.
- **Voice layer** at 0x4CF800: start 0x26E3E0, 3D volume/pan/pitch 0x26E8F0, per-frame 3D update 0x26E840, set
  volume 0x26EE90, stop 0x26EF60, playing 0x26EEF0, clock 0x26F230 (ms).
- **MultiStream V7.1** (SCEE; `STREAM.IRX` on the IOP, EE side 0x131990, 0x138030, 0x138F30 ...): SPU2 voices,
  an SPU-RAM sample cache, disc streams. The samples are streamed/cached from the `.samp` on the disc.

## UBER (little endian)
```
0x00 "UBER" u16 1 u16 1
0x08 u32 offset of PROJ, POOL, SDIR, group counts, group names, names   (from the file start)
```
- **PROJ** (`"PROJ" u16 2 u16 1, u32 count, u32 0`, then count × 12 bytes) — one per event (= per name):
  `f32 scale` (the 3D range's multiplier: range = (given or 1000) × scale, 0x274B10; `set3DData` writes it),
  `u32 flags` (bit 0: 3D, an object's position is used; bit 1: Doppler (velocity from the moves); bit 2: loop-ish
  (looked at by 0x272CA0); bit 3: may steal (+0x1E)), `u16 event` (the POOL program, = its own index),
  `u8 group` (0xFF everywhere; 0x272BC0 turns anything ≥ 0x40 into 0), `u8 max` (how many may already play: a
  start is refused when more than `max` instances run the event's program; `V_Looker` has 0).
  Values: flags 9 (one-shots), 13 (loops), 8 (`Click`, `Rollover`, `V_Traffic_Reporter`: not 3D), 5, 0.
- **POOL** (`"POOL" u16 1 u16 1, u32 count, u32 0`, count × `i32 offset` from the POOL start, < 0 = none): the
  event programs, 8-byte instructions (multi-byte operands big endian):

| op | operands | meaning (0x2716E0) |
|---|---|---|
| 00 | | end (the instance is freed, its voice stopped) |
| 05 | [1] stop-aware, [4..5] target, [6..7] times | loop: counter +1; jump to target unless ([1] and killed) or (times ≠ 0xFFFF and counter ≥ times) |
| 06 | [2..3] event, [4..5] pc | goto another event's program |
| 07 | [1], [3], [4], [6..7] ms | wait until: [1] a kill came, or [3] the voice ended, or [4] [6..7] ms passed. None set = forever (`Leeper_land` waits forever after its first part) |
| 08 | [2..3] event, [4..5] pc | start that event beside this one (a child: 2D, killed with its parent) |
| 0D | [1] v | volume byte = v × volume / 128, kept to 8 bits |
| 0F | [1] v, [6..7] ms | fade to v over ms (a fade reaching 0 stops the voice and counts as killed) |
| 10 | [1..2] sample | play SDIR sample (skipped while the instance's voice is busy) |
| 11 | | stop the voice |
| 13 | [1] p, [2..3] event, [4..5] pc | if a random byte (Park–Miller 0x106BD0, low 8 bits) > p: goto |
| 14 | [1..6] pcs, [7] turn | jump to the next non-zero pc of the list in turn (writes [7] back) |
| 32 | [1] n, [2] v | flag n = v (`querySoundFlag`) |

  Several instructions run in one frame until one waits (a jump also waits a frame). Random variants are `13`
  chains: `Bonk_RND` = `13 7f→@4, 13 7f→@3, 06 Bonk_7, 06 Bonk_3, 13 7f→@6, 06 Bonk_6, 06 Bonk_9` (a quarter each);
  `14` cycles (`Chickens`, `V_Arni_RND`, `V_Boss_Damaged`). A one-shot is `0D vol, 10 sample, 07 wait voice end,
  00`; a loop `0D, 10 (a looping sample), 07 01 (wait for a kill), 0F 00 1000 ms, 07 (voice end), 11, 00`.
  A kill (0x2725C0) ends a stop-aware wait, otherwise fades the instance to 0 in 100 ms; it kills the children too.
- **SDIR** (`"SDIR" u16 1 u16 1, u32 .samp size, u32 count, then count × {u32 offset, u32 size, u32 flags,
  u32 rate}`; entry 0 is a dummy of 0xFFFFFFFF): the samples in the `.samp`, PS-ADPCM, 7500–22050 Hz; flags bit 0 =
  looping (27 in Rampage: their frames carry the SPU loop flags 6 / 2 / 3).
- **group counts** (u16 each) and **group names** (`u32 n, n × u32 offset`): the events in order, by load group:
  Rampage `Loading` 19, `Effects` 696, `Shell` 7, `DuckSfx` 0, `DuckMusic` 6 (the announcer `V_combo` ...),
  `NoDucking` 0; RShell `Loading` 19, `Shell` 8.
- **names** (`u32 n, n × u32 offset`): one per PROJ entry.

## .samp
Raw PS-ADPCM (SPU format): 16-byte frames `shift | filter << 4, flags, 14 bytes = 28 nibbles`, filters (0,0),
(60,0), (115,−52), (98,−55), (122,−60) / 64; flags bit 0 end, bit 1 repeat, bit 2 loop start.

## .mic (music)
```
0x00 u32 0x40 (header size)   0x04 u32 ? (0x30_1020 .. 0x4A_F020, not used here)
0x08 u32 channels (2)         0x0C u32 rate (48000)
0x10 u32 interleave (0x6000)  0x14 u32 blocks
0x40 blocks × channels × interleave bytes of PS-ADPCM (left 0x6000, right 0x6000, ...), no end flags
```
2 to 3 minutes each: `bbop` 162 s, `oingo` 119 s, ... `playMusic("bbop")` opens `bbop.mic` (".mic" is added when
the name has no '.', 0x26FF98); the BlockDefs' `Music` names them. Two slots crossfade (`replaceMusic`). Volume =
group 3's byte (0x3F) << 7 (0x26C6D4).

## Volumes
The volume byte of an instance starts at its group's (0x6A = 106 for group 0, 0x272288) and is kept as
`byte << 7` (0x26FC80); `0D` multiplies the byte and keeps the low 8 bits. MultiStream and the SPU2 take a volume
of 0x4000..0x7FFF as negative (phase-inverted): so for a 2D sound a byte over 127 (0D with more than ~0x9A on a
fresh instance: `Gong`, the announcer at 0xFF) plays inverted and quieter; the 3D path scales the raw value first
(distance, then × 0.707) and mostly comes back under 0x4000. `src/sound.c` does the same (`sound_wrap_volumes(0)`
clamps instead, to compare). `setSfxVolume` / `setVoiceVolume` change groups 1 / 2, which no event is in.

## 3D sounds (0x26E8F0, 0x273CF0)
- Range r (PROJ scale × 1000 unless given). Volume = v × (r − d) / r for d ≤ r, else 0 (d = 3D distance).
- Pan: the listener (0x26FCB0, from the camera at 0x288550) has rows R0, R1 and R2 = (0,0,0) (0x409FF8), and the
  angle is atan2(d·R0, d·R2): so ±90° only. In front of R0: both channels × 0.707 in phase; behind: the right
  channel inverted (Dolby Pro Logic surround). The angle moves a tenth of the way per update; crossing behind turns
  a ±1 phase flag over.
- Doppler: radial speed v over the next 10 ms, pitch 350 / (350 + v) for −192.5 ≤ v ≤ 350, else 1. Only with PROJ
  flags bit 1, which no event has (their velocity is a fixed zero vector, 0x4CFB60), so pitch stays 1.
- The list: objects sorted by their x, y distance to the listener; the nearest 16 are seen to each frame: started
  when within range, killed when outside (and dropped unless added with an id, as `add3DSfx` does), dropped when
  their sound has ended.
