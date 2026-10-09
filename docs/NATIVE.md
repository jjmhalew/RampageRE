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
rampage.exe build\disc\RAMPAGE.VOL Chicago 1 --monster George   :: play George by his own move graph, the game's camera follows
rampage.exe build\disc\RAMPAGE.VOL Chicago 1 --shot shot.png   :: one frame to a PNG, then quit (offscreen: no window)
rampage.exe build\disc\RAMPAGE.VOL NewYork 3 --monster George --do "at 25 -9, up 1" --cam 10 -15 8 30 5 --shot s.png
```
`build_viewer.bat` compiles C99 with Zig (`zig cc`), like WoodyRE's `build.bat`: a Zig in `tools\zig`, on `PATH` or
from `pip install ziglang` is used, otherwise Zig 0.16.0 is downloaded once and checked against its SHA-256. The disc
must have been extracted by `build.ps1` first (or pass the path to your own `RAMPAGE.VOL`).

Keys: W A S D move, Q E down and up, Shift faster, right mouse button to look around, the wheel changes the speed;
Page Up / Page Down the block, 1-7 the city, N day or night, F fog, H the armed attack spheres, M the next monster (none, then all 30), K / L
the monster's clips, G play or the free camera, F12 a screenshot, Esc quit. In play (the default with `--monster`)
the keys are a pad: W A S D or the arrows the left stick, Space Cross (jump), J Square (punch), K Triangle (kick),
L Circle (grab), R R2 (roar); the monster runs its own move graph and the game's camera follows it
([CAMERA.md](CAMERA.md)). `--clip N` picks a clip by number (and starts with the free camera), `--time T` poses a
`--shot`; in play a shot first lets the camera settle for 2 s, then plays `--do "right 1, cross, wait 0.8"` (stick
directions held and buttons pressed, each followed by that many seconds; `at x y` puts the monster there facing the
buildings; `--walk S` is `right S`). `--cam x y z yaw pitch` (degrees) draws the shot from a fixed view. The console prints
what each block holds (models, people, vehicles, anything not found) and, in play, every state the monster enters
(`RAMPAGE_TRACE=1`: every frame's state, time, place and heading; `RAMPAGE_HANDS=1`: the hands' grips each frame;
`RAMPAGE_NOIK=1`: the arms as the clips have them).

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
| `src/camera.c` | the game's play camera, TwoTilts, transcribed from 0x312B50 (docs/CAMERA.md) |
| `src/building.c` | a building's cells and the game's hit on them: 0x2BC150, 0x2BB7B0, 0x2BB950 (docs/FORMAT_BDP.md "Damage") |
| `src/moves.c` | the monster's move graph (`MONSTER_DATA`) and the game's runner for it: 0x2EBFE0, 0x2EC3D0, 0x2ECBE0 (docs/FORMAT_MONSTER.md) |
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
each vertex sits on. Between rotation keys, and blending clips, the viewer slerps (the game's own interpolation,
0x104590, is not read yet); with `--clip` root motion stays in, so a walk walks off and jumps back when it loops.

In play the monster is driven the way the game drives it: the keys fill the pad's variable table (0x2C0AD0), the
move graph picks the state and its clip (the transitions in their input window, by priority, until their switch
window), the clip plays at its state's rate and blends in from the pose before it as the transition says (over the
new state's blend time, [FORMAT_MONSTER.md](FORMAT_MONSTER.md) "Blending"), and the clip's root carries the monster as
its node says: each frame the root's travel along the axes it names (the walk forward, a kick forward and sideways)
moves it, turned by its heading, and the rest stays in the pose; when a state whose node says so ends (flag
0x40000: the climbs, mounts, knock-downs) the place and turn its root reached become the monster's
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "Root motion"; the root's height is not taken yet). The stick is read in the camera's frame:
where a state allows it (`Ground_Walk`, the flip) the monster turns towards it at 6 × the angle per second (0x2E8190),
and in a jump it drives the monster at up to 16 units/s (the stick's own velocity, +0xA68)
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "State nodes", "Steering"). The clip's events act too: the movement mode, the
slot column and `Monsters.bsf`'s velocity calls (`addVelocity`, `jumpNorm`, ...); in the air the monster moves by
its velocity and falls by 60 units/s² (unless `disableGravity`), and meeting the street fires the landing slot (56)
in its column, so a held Cross jumps higher, a double jump flips, a butt slam lands in its impact
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "Movement", "Slots"). Script checks the graph asks (`Monsters::canMount`,
`canGrabPerson`, ...) answer no, except the monster's own abilities (roar energy, double jump, jump smash, spin,
punch). While a clip's attack event has a body part armed, its collision spheres are drawn as red wire spheres (H
hides them): the punch's fist, the kick's foot. The face plays its own clip over the body's (the game's face layer:
`Expression_Idle` by default, the clip events' `faceAnim` expressions; FORMAT_MONSTER.md "The face"). A clip plays mirrored when its events (type 8, `setMirrorState`)
say so: the skin takes each bone's other side reflected across the monster's x (for a symmetric mesh the same as
the game's pose mirroring, 0x194BA0), and root motion, steering, the conditions and the spheres follow
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "Mirroring"; `--do "mirror"` flips it for the next clip). The clips' `hitBuildingFront` calls damage the buildings by the
game's rules: the front cell takes 2 HP, the hit spreads up or down and sideways as the cell weakens, floors and
buildings keep their HP pools, and each facade tile shows its cell's state (the sheet's row), so five punches crack,
break and wreck a cell and its neighbours, and a damaged storey shows its interior (the floor's `Skeleton`, then its
`DamagedSkeleton` once half its `FloorHP` is gone) through the holes ([FORMAT_BDP.md](FORMAT_BDP.md) "Damage").
The facade's other passes are the game's too ("Facade geometry"): intact windows show `InnerWindow` interiors behind
the sheet's clear panes, tinted `CubeTint`; hits dent the facade (its grid vertices move in the face, 0.1 … 0.45 by level) and fade in `CrackTexture` by the levels,
around the windows of lightly damaged cells; a broken corner cell shows the inner wall behind it (the sheet's
`InnerWallColumn`). A floor whose HP runs out falls: the storeys above
come down onto it and crush the one below, and a building whose HP runs out cracks, shakes, drops storey by storey
and sinks, leaving its rubble ("Collapse"; `--do "collapse"` destroys the building in front of the monster).
Monsters climb: the game's canMount, canClimb and the mount bindings run against the buildings' boxes, so a monster
pushed at a building's front mounts it, climbs with the stick (each stroke flipping the clip), hops along the front,
swings round a corner onto a side unless a neighbour is flush with it, climbs down and steps off at the foot, or
mantles onto the roof at the top (its hands gripping the wall: the game's arm IK holds a planted hand where it took
hold while the clip climbs on), steps across onto the front of a building it touches, and braces itself across a
10-unit alley to climb between two buildings and turn onto the other one ([FORMAT_MONSTER.md](FORMAT_MONSTER.md)
"Climbing"; the checks the game also makes against attachments, cells flagged 0x1000 and other monsters are left
out). `--do "at x y"` puts the monster there, facing the buildings. Stand-ins for game code not read: a start through a slot (a landing) blends in (it takes no
transition, so which way it comes in is not traced). The monster's collision box is the game's (MONSTER_DATA +0x198 / +0x19C: 3 each way across, 8.5 tall, 0x2E2C50):
it cannot go into a building's box (the viewer pushes it out the shorter way; the game's response, 0x2801A0, is not
read), it can land on a roof (6 + 3 × storeys up) and stand there, and walking off the edge it falls (a stand-in: it
drops in the state it is in until the landing slot catches it). The building a hit goes to is the one whose face
lies within 5 units along the side's direction (the game's search, 0x2A3D20, is not read). The street ends at
x ±75 and y −30.

## Not there yet
- the face layer's blend into a new face clip (it cuts), `queueFacialAnim`, the head turning towards a target, cloth
  simulation;
- friction on the velocity, the head turning with the right stick;
- debris and dust effects, scoring;
- effects (`tools/edf.py`), the GUI, sound;
- grabbing, the hands' grip on the wall (attachHand / detachHand: IK), the game's rules;
- vertex-coloured interiors of damaged buildings, shadows, reflections.
