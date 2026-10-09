# The native viewer (`src/`, `rampage.exe`)

The second half of the hybrid approach: next to the recompiled game, a hand-written C program that reads the game's
data with what the format work found and draws it, the way WoodyRE reimplements its game. The first piece is a viewer
for the city blocks; it reads `RAMPAGE.VOL` itself (no extracted files, no Python), so it is the C port of the
Python tools' readers and of `tools/city.py`.

## Building and running
```bat
build_viewer.bat
rampage.exe                                   :: build\disc\RAMPAGE.VOL, Chicago, block 1
rampage.exe build\disc\RAMPAGE.VOL NewYork 6 --night
rampage.exe build\disc\RAMPAGE.VOL Chicago 1 --monster George   :: George idling in the street
rampage.exe build\disc\RAMPAGE.VOL Chicago 1 --shot shot.png   :: one frame to a PNG, then quit
```
`build_viewer.bat` compiles C99 with Zig (`zig cc`), like WoodyRE's `build.bat`: a Zig in `tools\zig`, on `PATH` or
from `pip install ziglang` is used, otherwise Zig 0.16.0 is downloaded once and checked against its SHA-256. The disc
must have been extracted by `build.ps1` first (or pass the path to your own `RAMPAGE.VOL`).

Keys: W A S D move, Q E down and up, Shift faster, right mouse button to look around, the wheel changes the speed;
Page Up / Page Down the block, 1-7 the city, N day or night, F fog, M the next monster (none, then all 30), K / L
the monster's clips, F12 a screenshot, Esc quit. `--clip N` picks a clip by number and `--time T` poses a `--shot`. The console prints
what each block holds (models, people, vehicles, anything not found).

## Source
| File | |
|---|---|
| `src/data.h` | the formats: declarations and little-endian readers |
| `src/vol.c` | `RAMPAGE.VOL` (`tools/pvol.py`) |
| `src/bundle.c` | Pipeworks bundles, segment 0 (`tools/bdp.py`) |
| `src/tex.c` | textures to RGBA: PSMT8 unswizzle, CSM1 palettes, PSMCT32 / PSMCT16 (`tools/tex.py`) |
| `src/mesh.c` | meshes, strips to triangles (`tools/mesh.py`) |
| `src/prb.c` | property books read in place, with `ParentPropertySet` inheritance across bundles (`tools/prb.py`) |
| `src/tree.c` | node trees, their rest pose (RawAnim at time 0, or the skeleton's bind pose) and inverse bind matrices; cloth particles (`tools/anim.py`) |
| `src/anim.c` | clips: KeyInterpAnim tracks with the game's rotation decode (0x192010), RawAnim segments (`tools/anim.py`) |
| `src/city.c` | a block: ground, buildings with generated facades, objects, people, traffic, lighting (`tools/city.py`); a monster; posing and CPU skinning every frame |
| `src/render_gl.c` | Win32 window, WGL and OpenGL 1.1 drawing |
| `src/shot.c` | PNG screenshots (stored zlib blocks, no compressor) |
| `src/main.c` | the viewer: arguments, camera, keys |

The game lights its city at run time: texture × (ambient + Σ colour × N·L) from the light file
([FORMAT_BDP.md](FORMAT_BDP.md) "Lighting"), which is OpenGL's fixed-function lighting, so the light file's ambient
colour and directional lights become `GL_LIGHT_MODEL_AMBIENT` and `GL_LIGHT0..2`. Meshes are drawn from both sides
with their normals as stored; texels with alpha below 0.5 are dropped, as in the glTF export. The cloud band
scrolls by the `AnimatingSkyData` rate (per second, an assumption). Traffic drives the two lanes of the game's spawn
code (0x332D90) at 15 units/s with a fixed pick from the block's vehicle mix.

What moves: props whose own RawAnim moves (helicopter rotors, the fountain), every person playing the clip of the
state its template starts in, the traffic, and a monster (`--monster`, M): its skeleton's keyframed clips, its body
skinned on the CPU with its two bones per vertex, its hair or fur with the bones and weights of the cloth particle
each vertex sits on. Between rotation keys the viewer slerps (the game's own interpolation, 0x104590, is not read
yet); root motion stays in, so a walk walks off and jumps back when it loops.

## Not there yet
- the monster's face clips (`<MONSTER>_HEAD_*`) layered over its body, cloth simulation;
- effects (`tools/edf.py`), the GUI, sound;
- the game's camera, collision and rules (the game systems come after the renderer);
- vertex-coloured interiors of damaged buildings, shadows, reflections.
