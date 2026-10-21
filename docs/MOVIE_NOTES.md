# The movies (.pss)

The front end's movies: the Midway, Pipeworks and Digital Eclipse logos and the intro (AttractMode), the Victory
movie and the credits after the campaign (Victory), the cheat videos (`66730n`) and the Ballers video (the main
menu's retro box, NTSC). `src/movie.c` reads and decodes them by hand; the shell (`src/shell.c`) and the viewer
(`src/main.c`) play them in the interfaces' full-screen `Video` quad. [R] read in the files, [G] guessed.

## The files [R]

| File | Pictures | Time | Pictures' kinds |
|---|---|---|---|
| `midway.pss` | 393 | 13.1 s | I, field and frame DCT |
| `pipeworks.pss` | 250 | 8.3 s | I, field and frame DCT |
| `digital.pss` | 150 | 5.0 s | I, P, B (11 GOPs, a sequence header in each) |
| `intro.pss` | 4420 | 2:27 | I, progressive |
| `victory.pss` | 1758 | 58.7 s | I, progressive |
| `credits.pss` | 3243 | 1:48 | I, field and frame DCT |
| `ballers.pss` | 2163 | 1:12 | I, field and frame DCT |

Every one has a `_pal` twin at 25 frames a second, with fewer pictures (midway_pal 328, intro_pal 3687), and the
intro and Victory movies have `_f_pal`, `_g_pal`, `_i_pal` and `_s_pal` ones (they decode too).

**Which file** [R] (0x169360, `FullMotionVideoManager`'s pick): the name without its extension, the language's
suffix by gApp +0x70 (the .loc column: 1 `_a`, 2 `_e`, 3 `_j`, 4 `_f`, 5 `_g`, 6 `_i`, 7 `_s`, 8 `_d`, 9 `_p`; 0
none; the table 0x40F0A0), then up to four tries: `<name><lang>_pal`, `<name>_pal`, `<name><lang>`, `<name>`, the
two `_pal` ones only when the display's rate (0x4CB550) is under 59; each with the name's own extension (if any),
then the manager's (slot +0x28, 0x1A6E70: `.PSS` alone), until a file opens. The rate handed to the player is 25 for
a `_pal` one, else 30. gApp +0x70 comes from the console's language at start-up (0x10D300, `sceScfGetLanguage`:
English 1, or 2 on an SLES disc; French 4, Spanish 7, German 5, Italian 6; Japanese 3 (2 on SLES); Dutch and
Portuguese English). The display is 50 Hz on a PAL console unless the app's flags have 0x80 (0x25E1A0), and they are
0xC (0x169020): the European disc on a European console plays the `_pal` ones, the French intro `intro_f_pal`, the
English one `intro_pal` (no `_a`/`_e` files), and the NTSC ones only on an NTSC console.

The port plays as the European disc on a PAL console (`movie_pick`, main.c's `movie_start` with `--lang` + 1): the
`_pal` files, the language's where there is one; and the USA disc (the same VOL, region 0 by its `SYSTEM.CNF`) as on
an NTSC console: the files without `_pal` (`intro.pss`; no `intro_a`). The pictures are timed by the movie's own rate, so the 25 frames a
second play right whatever the window's rate (the game's frame steps stay at 60 Hz, a port choice).

**The stream:** an MPEG-2 program stream in packs of 4096 bytes (pack header, system header in the first, then one
PES packet each, padding stream 0xBE to fill), ending in 0xB9.

**The video (stream 0xE0):**
- MPEG-2 Main Profile at Main Level, 512 × 384 (4:3), 4:2:0, 5 Mbit/s, 29.97 frames a second.
- Its own intra matrix in the sequence header.
- Every picture a frame picture (`picture_structure` 3): the non-linear quantiser scale (`q_scale_type` 1),
  `intra_vlc_format` 1, intra DC precision 8 bits (digital: 9).
- The interlaced ones (`progressive_frame` 0, `frame_pred_frame_dct` 0) choose field or frame DCT per macroblock
  and use the alternate scan; the progressive ones (intro, victory) the zigzag one.
- digital.pss alone has P and B pictures: frame prediction only (`frame_pred_frame_dct` 1), no concealment vectors.
- The movies have their own black bars: midway's picture is rows 44 .. 340.

**The sound (private stream 1, 0xBD):**
- Each packet's data starts with four bytes, FF A1 00 00.
- The first packet's then has Sony's header: `SShd`, 0x18 bytes (type 0x10 SPU ADPCM, 48000 Hz, 2 channels,
  interleave 0x20, loop start and end −1), then `SSbd` and the data's size (midway: 0xAF780 bytes).
- The data is SPU ADPCM (16-byte frames of 28 samples, the SPU's five filters, as the sound banks' and the `.mic`
  music's), the channels interleaved by 0x20 bytes: two frames of the left, two of the right.
- The interleave is the header's. Decoded with 0x20, the intro's two channels correlate 1.0 (its sound is mono);
  with 0x10, 0x40, 0x200 or 0x800 about 0.

## The decoder (movie.c)

**Reading:** the file is read from the Vol in 64 KB pieces as it plays (intro.pss is 100 MB): the packets'
video goes on a buffer of the elementary stream, their sound on another. `movie_open` reads ahead to the first
sequence header for the size.

**The video:** the parts of ISO 13818-2 that frame pictures use:
- the sequence header and extension, the quant matrix and picture coding extensions;
- slices, macroblock types, the address increments with escapes and skipped macroblocks;
- the coded block pattern, the intra DC sizes and prediction, both coefficient tables (B-14, B-15) with the MPEG-2
  escape;
- the inverse quantisation with saturation and mismatch control, field or frame DCT, the zigzag and alternate scans;
- motion vectors with their wrap, half-pel frame and field prediction, the averaged bidirectional one; the skipped
  macroblocks of P (from the reference as it is) and B (the macroblock before's motion).

Not there, as none of the movies has it: field pictures, dual prime, MPEG-1 streams.

**The IDCT:** separable, in floats, rows of zero coefficients skipped.

**Pictures in order:** three frames, two references. An I or P picture shows the reference before it, a B picture
itself; at the end the last reference is shown.

**Colour:** ITU-R 601 at studio range, as the IPU's colour conversion, to RGBA; the chroma taken from the nearest
sample.

**Checked against FFmpeg** (`ffmpeg -f rawvideo -pix_fmt yuv420p` of the same file, build\cmake's): the planes
agree to 63–76 dB PSNR, at most 1 level apart in the I pictures and 4 in digital's P and B ones (the IDCT's rounding).
The intro decodes at about 800 pictures a second.

**Time:** `movie_video(m, t)` decodes until the picture shown t seconds in (t × frame rate pictures); a late call
decodes the ones between. `movie_done` once the last one is shown and its time is over.

## Playing them (shell.c, main.c)

**The shell:**
- `playVideo(name)` (am_video, the cheat video, victory_next) sends `SHELL_EVT_MOVIE` with the name, as before.
- The viewer opens `<name>.pss` (Victory's "Victory" and "Credits" too: the Vol's names are case-insensitive) and
  says it plays it (`shell_movie_hold`). The movie then lasts until the viewer ends it (`shell_movie_end`) or the
  pad skips it (Start or Cross, as the scripts allow).
- A movie nobody holds lasts `shell_set_movie_time`'s seconds of black, as before.

**The time:** `shell_movie_clock`, the seconds the shell has run (its frames' dt) since the movie started. The
viewer asks for that picture after each `shell_update`; a `--shot` run's `wait` plays the movie at 1/60 s a frame.

**The picture:** the `Video` objects (AttractMode.ifc's, Victory.ifc's) are marked. With a picture
(`shell_movie_picture`), their quad comes out white with no bundle and `tex` `HUD_TEX_VIDEO`, and render_gl.c draws
it with the movie's texture (`rnd_video`, uploaded when the picture changes). "Press START" still blinks over it
(state 2).

**The sound:** the mixer pulls it (`sound_stream`) at full scale: the audio start (0x1AF5C0) sets the master and
the stream's volumes to constants (MVOL 0x3FFF, BVOL 0x7FFF, the voices 0x3FFF), never the music setting [R]. With no audio output (`--mute`) the mixer still moves on in real time and drops it. A `--shot` run keeps at
most 4 MB of it.

The sound and the pictures are not synchronised by the PTS [R]: 0x1A6990 decodes up to the picture (timer − start)
× rate / the timer's frequency + 1, at most 2 past the current one, the start set when the first picture is out and
the sound starts (0x1A6C90; a pause moves it on, 0x1A6620); the sound streams on its own. Here both start at the
movie's start, the pictures by the shell's clock.
