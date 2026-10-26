# Sound: the banks (.uber/.samp), the event programs, 3D sounds, the music (.mic)

The game's sound is in RAMPAGE.VOL: two sound banks, `Rampage.uber` + `Rampage.samp` (728 events, 543 samples, the
game) and `RShell.uber` + `RShell.samp` (27 events, 34 samples, the front end), and 11 music streams `*.mic`.
`tools/sound.py` lists and decodes them; `src/sound.c` plays them. Addresses are in SLES_538.00 (RAMPAGE.ELF).

## Who does what in the game
- **SoundDevice** `gSndDev` at 0x4CF1B0 (script interface: handlers 0x275630.. through the table 0x427110;
  `playSound` 0x26CFC0, `killSound` 0x26B960, `playMusic` 0x26C4D0, `replaceMusic` 0x26BFC0, `set3DData` 0x26D240,
  `setSfx/Voice/MusicVolume` 0x26B5E0). 32 named-sound slots (+0x54, 0x1C each; playSound is refused when all are
  taken), 2 music slots (+0x3D4, 0x14 each), 64 volume groups (+0x440: type, +0x444: volume byte; 0x26E0CC sets
  every group to 0x6A, group 3 = music to 0x3F). Its frame (0x26D960): the event player, the 3D list, the named slots
  whose instance has ended freed, the music slots' fades, the voice layer.
- **Event player** at 0x4CFAB0: 40 instances of 0x3C bytes (0x270F20: 0x2E − (2 × 2 streams + 2), the streams'
  count from the voice layer's constructor 0x270FC0), run each frame by the interpreter **0x2716E0**; spawn 0x272170,
  start by name 0x272050, kill 0x2725C0. Handles from 100 (0x44086C).
- **Bank list** at 0x4CFB18 (0x38 per bank): `PackageResLoader` 0x28C270 reads `<name>.uber` into memory and
  remembers where `<name>.samp` is on the disc; 0x272AB0 parses the UBER, 0x2736E0 sorts the names, 0x273480
  searches them (case-insensitive, 0x32C9C8), 0x272F20 = PROJ entry by name.
- **3D sound list** at 0x4CFB40: 128 objects of 0x40 bytes (0x274FE0) (`add3DSfx` → 0x26D560 → 0x274840, the
  monsters' clip sounds 0x274CF0), updated by 0x273CF0.
- **Voice layer** at 0x4CF800 (constructor 0x270FC0, a static's; init 0x270EB0): start 0x26E3E0, 3D
  volume/pan/pitch 0x26E8F0, per-frame 3D update 0x26E840, set volume 0x26EE90, stop 0x26EF60, playing 0x26EEF0,
  clock 0x26F230 (ms). Of the SPU2's 48 voices, 2 .. 41 are the sound effects' (0x270B60), one per instance at most,
  so none is ever short (the steal path 0x26F280 for PROJ flags bit 3 never comes); the streams have the top six,
  music slot k the voices 0x2F − 2k (left) and 0x2E − 2k (right). Each instance's voice record (0x26F1C0) keeps its
  surround phase (+0x12, 1 when made) from one play to the next.
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

**The Wii's banks** (`DATA/files/Audio`; `tools/sound.py --verify DIR` checks either platform's): `Rampage` (881
events, 662 samples, 1,359 s) and `RShell` (27 events, 34 samples). The same layout, big endian, so the tags read
backwards (`REBU`, `JORP`, `RIDS`); the counts of the names and group names are u16 (then 2 bytes), as on the PS2.
The event programs are the same instructions. An SDIR entry is 64 bytes: u32 nibble address of the first sample (the
frame's two header nibbles count), u32 nibbles, u32 flags, u32 rate, s16[16] the DSP-ADPCM coefficients, u16 gain,
u16 the first frame's header, zeros. The `.samp` is DSP-ADPCM: 8-byte frames, a header (predictor << 4 | scale) and
14 nibbles; every sample follows the one before and starts with the header its entry gives. RShell's 34 samples
decode like the PS2's (the same rates and lengths within a frame, correlation 0.989 to 0.999). There are no `.mic`:
the Wii's 11 music tracks are Bink files in `Video` (`bbop.bik` ... `yeahyeah.bik`), not read by these tools.

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
the name has no '.', 0x26FF98); the BlockDefs' `Music` names them. They loop: setMusicRepeat's flag is 1 from the
start (+0x43C, 0x26E384; the voice layer's copy +0xBC, 0x271118) and no script calls it.
- **The slots** (+0x3D4: +0 the stream, +4 / +8 the fade's end / start in ms, +0xC stop when it is over, +0xD the
  volume byte now, +0xE the target, +0xF the start, +0x10 the slot): a new stream goes into the first free slot, else
  the one after the last used (+0x434), and a stream still in it is cut. `playMusic` (0x26C4D0) starts it at group
  3's byte at once, the other slot playing on; `replaceMusic(name, in, out)` (0x26BFC0; the script's seconds × 1000)
  brings it from 0 up to group 3's byte × 1 over `in` and every other slot down to 0 over `out`, then stops them;
  `replaceMusic("")` only sends them down. Each frame (0x26DA50) a slot fading is set to from + t × (to − from) in
  bytes, the end reached at once when end ≤ now or end = start. A slot's volume is its byte << 7 on its two voices
  (0x26FE00). `isMusicPlaying` (0x26BDB0): a slot in use, a fading one too.
- **Which music** (0x323CE0, each time the world enters a state, 0x3247A8): nothing while `getMusicVolume` (group
  3's byte / 127) is under 0.05; else the state's name (4 `BeginLevel` / `ContinueLevel`, 5 `BeginBlock`, 7
  `Results`, 8 `GameOver`, 9 `ContinueBlock`, 10 `EndLevel`, 11 `Victory`; 6 none, names 0x3F5990 + 4 × 0x54..)
  looked up in the city book's and the block's `Music` groups (the cities' _Props have none), and for BeginBlock the
  block's `Music`; defaults `Wilder` (BeginBlock) and `Oingo` (Results); then `replaceMusic(name, 0.5, 0.5)`. So a
  block's music comes in as it begins and Oingo with its results. (0x23395C fades every slot out over 500 ms.)

## Volumes
The volume byte of an instance starts at its group's (0x6A = 106 for group 0, 0x272288) and is kept as
`byte << 7` (0x26FC80); `0D` multiplies the byte and keeps the low 8 bits. `setXVolume(v, ms)` (0x26B5E0) sets the
group's byte to (int)(127 v); for group 3 the music slots go to it over ms (at once for 0), for the others the
group's running instances fade to it (0x2724B0; group 0 would be all). The profile's and the Options page's volumes
(0 .. 0.75, default sound 0.75, music 0.25) go in as setSfxVolume on groups 1, 9, 10 and 11 and setMusicVolume
(0x33EFD8 at a profile's load, music over 500 ms; 0x2C2FA4 from the Options page; and SinglePlayer's first frame,
0x2B7BC0 with the +0x610 its load 0x2B7F50 sets: to the profile's volume over 250 ms, before BeginBlock's
replaceMusic, which nothing starts under 0.05). The loading screen does not touch group 3: showLoading (0x3485E0)
calls 0x26BE90(0.0, 500, 1), which sends both music slots down to 0 over 500 ms with their stop byte (+0xC) set, so
the menu's music fades out and is stopped, and the level's intro is silent until BeginBlock's music. Every event is in group 0, so
the sound effects' slider changes nothing, while the music's byte is 127 × the setting (31 by default). MultiStream and the SPU2 take a volume
of 0x4000..0x7FFF as negative (phase-inverted): so for a 2D sound a byte over 127 (0D with more than ~0x9A on a
fresh instance: `Gong`, the announcer at 0xFF) plays inverted and quieter; the 3D path scales the raw value first
(distance, then × 0.707) and mostly comes back under 0x4000. `src/sound.c` does the same (`sound_wrap_volumes(0)`
clamps instead, to compare). The master volume is 0x3FFF on both cores (the boot logos' sound start 0x1AF608; nothing
sets it again). Pitch: a voice's rate (× the Doppler factor) goes to the SPU2 as rate × 4096 / 48000 cut to an
integer (0x135320). [G: the SPU2 interpolates with its 4-point gaussian (hardware); `src/sound.c` linearly.]

## 3D sounds (0x26E8F0, 0x273CF0)
- Range r (PROJ scale × 1000 unless given). Volume = v × (r − d) / r for d ≤ r, else 0 (d = 3D distance).
- Pan: the listener (0x26FCB0, from 0x288550: the camera manager's camera, 0x27F500) has R0 = the camera's forward,
  R1 = its up (the inverse of the view matrix 0x167D10, rows right, up, forward), R2 = R0 × R1 normalised = right
  ((1,0,0) if under 1e-6; 0x288550's fifth argument 0x409FF8 is not read), the place × setVelocityScalar (1). The
  angle θ = atan2(d·R0, d·R2) (VU0 atan, the quadrant fixed after; ±π/2 when d·R2 = 0): right 0, ahead π/2, left π,
  behind −π/2; x = (π − θ)/2 for θ > 0, else (−π − θ)/2; left = v cos x, right = v sin x, both × the phase flag
  (so ahead 0.707 each, right / left one channel, behind the right one inverted: Dolby Pro Logic surround). The
  angle moves a tenth of the way per update; crossing behind (|θ| ≥ π/2) turns the ±1 phase flag over.
- Doppler: radial speed v over the next 10 ms, pitch 350 / (350 + v) for −192.5 ≤ v ≤ 350, else 1. Only with PROJ
  flags bit 1, which no event has (their velocity is a fixed zero vector, 0x4CFB60), so pitch stays 1.
- The list: objects sorted by their x, y distance to the listener; the nearest 16 are seen to each frame: started
  when within range, killed when outside (and dropped unless added with an id, as `add3DSfx` does), dropped when
  their sound has ended.

## The scripts' SoundEffect (0x2750F0)
- [R] A script type (methods by the dispatcher 0x275180, its jump table 0x427060: `start` 0x2751C0, `stop` 0x2751E8,
  `getPosition`, `setPosition`, `setPoint` 0x2752B8, `getVelocity`, `setVelocity`, `setVelPoint`, `isValid`
  0x2753A8 = a slot); the variable holds a slot of the table at 0x4CFB90: 16 of 0x20 bytes (+0x00 the 3D object,
  +0x04 the place, +0x10 the velocity, +0x1C `start`'s second argument != 0, +0x1D a frame gone by).
- `start(name, autoStop)` (0x276490): the free slot from the last down, zeroed, then `add3DSfx` with the slot as the
  object, the slot's place (the world's origin until a `setPoint`) and range 0; the velocity pointer only when it is
  not all zero (never). The object reads the place through the pointer, so `setPoint` moves it at once.
- Every frame after the device's (PackageApp's frame 0x28BDE0: 0x26D960, then 0x2765C0): a slot with autoStop
  whose sound is not playing (0x274CF0 finds the object, 0x26BC90 asks after its instance +0x3C) a frame after its
  start loses its object (0x26D530): it has played, or never started (out of range). When the package changes the
  same frame kills all 16 first (0x276430 at 0x28BE1C).
- The only user is `Traffic::onActivateBlock`: `sfx.start("Crowd_Panic", 1)` and `setPoint(0, 0, 0)` with the two
  crowds (a one-shot of sample 208, 4.7 s, range 1000). `SoundEffect_array` is declared by no script.
- `src/sound.c` keeps the table (`sound_effect_start`, `_set_point`, `_stop`, `_stop_all`) and runs 0x2765C0's
  pass at the end of `sound_update`; city.c starts the crowd's, main.c stops them all when the world goes back to the
  shell.

## Not settled
- [G] The event player's random seed (+0x50): 0x106C80 sets it at start-up to the EE timer's count (0x10D4D0, which
  starts that timer on its first call) | 1, a value of a few ticks that cannot be known; `src/sound.c` uses 12345.
- [G] The SPU2's 4-point gaussian interpolation and the IOP's mixing (STREAM.IRX) are not in the ELF: linear here.
