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
rampage.exe build\disc\RAMPAGE.VOL NewYork 3 --monster George --do "at 25 -4, up 1" --cam 10 -10 8 30 5 --shot s.png
rampage.exe build\disc\RAMPAGE.VOL Chicago 1 --players 2 --monster George --monster2 Lizzie   :: two on the keys
rampage.exe build\disc\RAMPAGE.VOL Chicago 1 --players 4 --bots 2 --play kingcity            :: two players, two bots
```
`build_viewer.bat` compiles C99 with Zig (`zig cc`), like WoodyRE's `build.bat`: a Zig in `tools\zig`, on `PATH` or
from `pip install ziglang` is used, otherwise Zig 0.16.0 is downloaded once and checked against its SHA-256. The disc
must have been extracted by `build.ps1` first (or pass the path to your own `RAMPAGE.VOL`).

Keys: W A S D move, Q E down and up, Shift faster, right mouse button to look around, the wheel changes the speed;
Page Up / Page Down the block, 1-7 the city, N day or night, F fog, H the armed attack spheres, M the next monster (none, then all 30), K / L
the monster's clips, G play or the free camera, F12 a screenshot, Esc quit. In play (the default with `--monster`)
the keys are a pad: W A S D the left stick, the arrows the right stick (the head), Space Cross (jump), J Square
(punch), K Triangle (kick), L Circle (grab), R R2 (roar), P or Enter Start (the Pause menu: the arrows choose, Enter
or Space take, Backspace back); player 2 (`--players 2`) plays on the numeric keypad, Num Lock on: 8 4 2 6 (or 5)
the left stick, 0 Cross, 7 Square, 9 Triangle, 1 Circle, 3 R2, / and * the right stick; the monster runs its own
move graph and the game's camera follows it
([CAMERA.md](CAMERA.md)). `--clip N` picks a clip by number (and starts with the free camera), `--time T` poses a
`--shot`; in play a shot first lets the camera settle for 2 s, then plays `--do "right 1, cross, wait 0.8"` (stick
directions held and buttons pressed, each followed by that many seconds; `at x y` puts the monster there facing the
buildings; `pause` opens the Pause menu and `menu down` / `menu accept` / ... drive it; `award Name` counts an object of
that template for the block's challenge; `p2` (`p1` .. `p4`): the words after it play that player's monster, the
other players on keys idle meanwhile; `--walk S` is `right S`). `--players N` (1 .. 4) plays N monsters, the last
`--bots B` of them the computer's; `--monster2 Name` .. `--monster4 Name` pick players 2 .. 4's monsters (without
one: George, Lizzie, Ralph or Boosy, the first not taken). Players 3 and 4 and the bots are computer-driven
(below, "Several monsters"); the front end (`--shell`) hands its players, bots and monsters over the same way. `--cam x y z yaw pitch` (degrees) draws the shot from a fixed view. `--play kingcity|kingworld|timed` plays another mode than the campaign
(its results screens). The profile (profile.c, [PROFILE_NOTES.md](PROFILE_NOTES.md)) is read from and written to
`rampage_profile.bin` (`--profile FILE` another one, `--newprofile` a new one; a `--shot` run starts new and writes
nothing unless `--profile` is given): a new profile has six monsters and none of the four upgrade moves, which the
upgrade challenges teach; `--cheat 011235` gives every move. `--shell` starts in the front end (shell.c, [SHELL_NOTES.md](SHELL_NOTES.md)): the
autosave notice, the movies (the logos and the intro, Start or Enter skips them once a profile exists; the
Victory movie and the credits after the campaign), the profile popup (one profile, its file; letters and digits type
the name, Enter is DONE), the main menu, players, monster select, city select, the loading screen, then the game it chose; the Pause menu's Quit
goes back to the main menu. Its keys: the arrows, Enter / Space accept, Backspace back, P Start, C / V / R the
square, triangle and R2 buttons, F2 the cheat entry. With `--shot` the `--do` words drive it first (`wait S`,
`menu up|down|left|right|accept|back|start`, `button N`, `type TEXT`, `done`), and the words left once a game
starts play in it. The console prints
what each block holds (models, people, vehicles, anything not found) and, in play, every state the monster enters
(`RAMPAGE_TRACE=1`: every frame's state, time, place and heading; `RAMPAGE_HANDS=1`: the hands' grips each frame;
`RAMPAGE_NOIK=1`: the arms as the clips have them; `RAMPAGE_MAGENTA=1`: a magenta background, to show holes in the
world). The fog is on (F toggles it, `--nofog` starts without).

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
| `src/building.c` | a building's cells and the game's hit on them: 0x2BC150, 0x2BB7B0, 0x2BB950, the roof stomp's 0x2BBE80 (docs/FORMAT_BDP.md "Damage") |
| `src/moves.c` | the monster's move graph (`MONSTER_DATA`) and the game's runner for it: 0x2EBFE0, 0x2EC3D0, 0x2ECBE0 (docs/FORMAT_MONSTER.md) |
| `src/render_gl.c` | Win32 window, WGL and OpenGL 1.1 drawing |
| `src/shot.c` | PNG screenshots (stored zlib blocks, no compressor) |
| `src/main.c` | the viewer: arguments, camera, keys; the monsters in play (`Mon`), their pads and their world |
| `src/director.c` | the game flow's choreography: the AI's goals (playMove, walkTo, the bots' DestroyBuildings) and the camera modes (docs/FLOW_NOTES.md, MULTIPLAYER_NOTES.md) |

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
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "Movement", "Slots"). The people are their templates' scripts ported (src/people.c, [PEOPLE_NOTES.md](PEOPLE_NOTES.md)):
window occupants lean out and fall when their window breaks, crowds run, punks throw, tourists take photos; the
monster grabs one from a window (`canGrabPerson`) or the street (`doGrabObject` at its `Helper R Hand`), holds,
eats (health, energy) or throws them, and its attacks send them flying. Script checks the graph asks (`Monsters::canMount`,
`canGrabPerson`, ...) answer no, except the monster's own abilities (roar energy, double jump, jump smash, spin,
punch). While a clip's attack event has a body part armed, its collision spheres are drawn as red wire spheres (H
hides them): the punch's fist, the kick's foot. The army comes for the monster (src/army.c, [ARMY_NOTES.md](ARMY_NOTES.md)): each block's `AIDef` spawns
helicopters, SWAT vans, army trucks with troops and tanks, which fire the `Weapons.bdp` weapons at it (hits take its
health and start its reactions), the punks throw their bottles, droppables fall from windows; blows wreck the
units, the monster grabs and eats troops, and buildings and kills score as the game's `Scoring` says
(`RAMPAGE_TRACE=1` prints the hits and the score). At 0 HP the monster takes slot 67 (`React_Defeated`), lies down, shrinks to 0.2
(`setScale`: setDynamicScale) and waits; a button respawns it at full health and it grows back (`RAMPAGE_HP=5`
starts it low, for testing). The game's sound plays (src/sound.c, [FORMAT_SOUND.md](FORMAT_SOUND.md): Rampage.uber's event programs over
.samp's PS-ADPCM, 3D by the camera, each block's `.mic` music): the monster's clip sounds and the people's; only in
a window of one's own (never with `--shot`; `--mute` keeps it quiet). The monsters' hair, fur and whiskers are the game's cloth simulation (src/cloth.c,
[CLOTH_NOTES.md](CLOTH_NOTES.md)), stepped in the world so the monster's moves drag them. The face plays its own clip over the body's (the game's face layer:
`Expression_Idle` by default, the clip events' `faceAnim` expressions; FORMAT_MONSTER.md "The face"). A clip plays mirrored when its events (type 8, `setMirrorState`)
say so: the skin takes each bone's other side reflected across the monster's x (for a symmetric mesh the same as
the game's pose mirroring, 0x194BA0), and root motion, steering, the conditions and the spheres follow
([FORMAT_MONSTER.md](FORMAT_MONSTER.md) "Mirroring"; `--do "mirror"` flips it for the next clip). The clips' `hitBuildingFront` calls damage the buildings by the
game's rules: the front cell takes 2 HP, the hit spreads up or down and sideways as the cell weakens, floors and
buildings keep their HP pools, and each facade tile shows its cell's state (the sheet's row), so five punches crack,
break and wreck a cell and its neighbours, and a damaged storey shows its interior (the floor's `Skeleton`, then its
`DamagedSkeleton` once half its `FloorHP` is gone) through the holes ([FORMAT_BDP.md](FORMAT_BDP.md) "Damage").
The roof stomp (`damageRoof` → `hitBuildingRoof`: the punch down, `square+down` on a roof, the landings and the butt
slam) shows the roof's damaged `TopFloorShapes` model and hits the top storey's front cell below the foot, so a
monster or a bot on a roof wears the building down a storey at a time (FORMAT_BDP.md "The roof stomp").
The facade's other passes are the game's too ("Facade geometry"): intact windows show `InnerWindow` interiors behind
the sheet's clear panes, tinted `CubeTint`; hits dent the facade (its grid vertices move in the face, 0.1 … 0.45 by level) and fade in `CrackTexture` by the levels,
around the windows of lightly damaged cells; a broken corner cell shows the inner wall behind it (the sheet's
`InnerWallColumn`). The game's particle effects play (src/fx.c, [FX_NOTES.md](FX_NOTES.md): fx.bdp's 287 effects, their words from
RAMPAGE.ELF): glass and wall debris as cells break (`Glass_*`, `Wall_*` of the building props; which goes with
which state is a stand-in), the floors' collapse clouds with their camera shake, adornments' `DislodgedFx`, the
people's effects. The buildings' adornments hang where the game puts them (fire escapes, AC units, awnings, roof
trims: "Adornments"), ride their storeys down, fall off with their cells and are knocked off by attacks. A floor whose HP runs out falls: the storeys above
come down onto it and crush the one below, and a building whose HP runs out cracks, shakes, drops storey by storey
and sinks, leaving its rubble (the camera shakes as each floor falls, and on the monsters' heavy blows: CAMERA.md
"Shake") ("Collapse"; `--do "collapse"` destroys the building in front of the monster, `--do "hitcell b floor col"` hits one
front cell).
Monsters climb: the game's canMount, canClimb and the mount bindings run against the buildings' boxes, so a monster
pushed at a building's front mounts it, climbs with the stick (each stroke flipping the clip), hops along the front,
swings round a corner onto a side unless a neighbour is flush with it, climbs down and steps off at the foot, or
mantles onto the roof at the top (its hands gripping the wall: the game's arm IK holds a planted hand where it took
hold while the clip climbs on), steps across onto the front of a building it touches, and braces itself across a
10-unit alley to climb between two buildings and turn onto the other one ([FORMAT_MONSTER.md](FORMAT_MONSTER.md)
"Climbing"; the checks the game also makes against attachments, cells flagged 0x1000 and other monsters are left
out). `--do "at x y"` puts the monster there, facing the buildings; `--do "square+up 0.3"` presses a button with the stick
held that way. Stand-ins for game code not read: a start through a slot (a landing) blends in (it takes no
transition, so which way it comes in is not traced). The monster's collision box is the game's (MONSTER_DATA +0x198 / +0x19C: 3 each way across, 8.5 tall, 0x2E2C50):
it cannot go into a building's box (the game's response, collide.c's `collide_monster`), it can land on a roof (6 + 3 × storeys up) and stand there, and walking off the edge it falls (a stand-in: it
drops in the state it is in until the landing slot catches it). The building a hit goes to is the one whose face
lies within 5 units along the side's direction (the game's search, 0x2A3D20, is not read). The street ends at
x ±75 and y −30.

**The collision world** (collide.c, [COLLIDE_NOTES.md](COLLIDE_NOTES.md)): main.c's `world_build` fills the frame's
`CollideWorld` (a Viewer field) once a frame, before anything moves: the block's buildings, the monster's collision
spheres (`COLLIDE_F_MONSTER`), then each module's objects (`army_collide_add`, `vehicle_collide_add`,
`rail_collide_add`, `parade_collide_add`, `streetobj_collide_add`, `boss_collide_add`), each tagged
`COLLIDE_TAG(owner, index)`. The army, vehicles, rail and parade hold it (`xxx_set_collide`). Asked of it: the
monster's push-out (`push_out`: the buildings, the PushOutMonsters objects, the VolumeTriggers, whose touches go to
boss.c), the shots' and beams' rays (army.c), every flying object's SpherePhysicsMover (the street's OnHitGround, the
objects it meets: TakeImpact on both; another module's object or a building is told through the module's `*_IMPACT`
event, `impact_route`: a building is hit with radius Mass / 1000), and the street objects' blow and touch tests.
`RAMPAGE_TRACE` prints the pushes, the trigger touches, what each shot hit, what each flight met and its bounces.

**Several monsters** ([MULTIPLAYER_NOTES.md](MULTIPLAYER_NOTES.md)): main.c keeps each monster's own state in a
`Mon` (`Viewer.mon[4]`, `nmon`; slot k is the scene's `Scene.mon[k]` with its own skeleton, clips, skin, head turn, arm
and foot IK, the army's, the game's player k and the HUD `Hud<k>.ifc`), the world's in `Viewer`. Each frame every
monster steps from its pad (`Mon.pad`: player 1's keys, player 2's, `--do`, or the computer's), then the world
around them (people, buildings, street, army, vehicles, rail, parade, edibles, bosses, the game) sees them all; the
modules' events go to the monster they name. The bots get the game's DestroyBuildings goal (director.c `DG_DESTROY`:
the nearest standing building of the front row, mounted on a side, punched cell by cell, climbed, its roof stomped;
with none left, the nearest monster), and so do players 3 and 4, who have no keys. Monsters' attacks hit each other
(each attack once a victim, its damage and ReactionLevel, no points); a building's points go to the monster that hit it
last; the camera frames every monster still in play (not a human whose streaker is gone, not one dropping in); a
player off a building is kept within 30 of the players' middle x and below 70 (the camera's reach), the bots are
not; the respawns, lives, King block winners and the results screens are game.c's per player. `RAMPAGE_TRACE` marks
the other monsters' lines `[2]` .. `[4]`.

**The revealed monster** ([EDIBLE_NOTES.md](EDIBLE_NOTES.md) "The cryotube"): with one player in the campaign,
the world's extra slot after the players gets the city's monster to reveal (`Viewer.extra`, 0x2B6BE0), hidden and
out of the world. When its cryotube lands, it appears there at a tenth of its size, grows to full size in a second,
taunts, and at 4.25 s runs off along the street to x ±100 (away from the camera) at 1.5 × speed, where it hides
again (reveal.c's walk events: the AI's PlayMove and WalkTo goals, setDynamicScale, setSpeedScale). It is not the
camera's, the HUD's or the game's, and has no push-out: 0x2E2C50 skips a slot past the players' count, which is
why it can walk past the street's ±55. `--do "hitcell b floor col"` hits that front cell as a monster's attack
would (a test: the tube's cell without climbing to it).

**No grabbing another monster:** the game has none. No move graph has a state for being held (George's holds are
people, objects and boulders), and the held / holder fields (+0xB9C0 / +0xB9C4) are only ever cleared
([MULTIPLAYER_NOTES.md](MULTIPLAYER_NOTES.md)).

**Boosy** unlocks as the game has it: at a block's end with every monster of the city's MonsterRevealDef unlocked,
the city gets flag 8, and when every city's saved flags (the profile's record, this city's | 8) have 1, 2, 4 and 8,
Boosy's flags get 0x80 (profile.c `profile_reveal_boosy`, [PROFILE_NOTES.md](PROFILE_NOTES.md)); the city's end
also unlocks him with Philbert and Eyegore (`profile_update`).

**The front end's 3D** ([SHELL_NOTES.md](SHELL_NOTES.md) "The GUI's 3D objects"): CitySelect's globe turns to each
city with its ring on it, lit for day or night; the main menu's fires, MonsterSelect's lava lamp and the cards' glow
play, through the game's GUI camera, each in its place among the screen's pictures.

## Not there yet
- the arcade emulators and the demo game, skipped (SHELL_NOTES.md); the memory card's own popup states; the [G] marks
  in the notes.

Ported as modules and driven by the viewer: the HUD with its banners, boss bar and results screens (hud.c), people
(people.c), the army (army.c), the traffic, rockets and fire truck (vehicle.c), the edibles and the rage (edible.c),
the profile (profile.c), the bosses (boss.c), the cryotube's monster reveal and the rubble's points (reveal.c), the trains, cable cars and freighters (rail.c,
[RAIL_NOTES.md](RAIL_NOTES.md)), the parade balloons, bikers and food carts (parade.c, [PARADE_NOTES.md](PARADE_NOTES.md)),
the street and roof objects (streetobj.c, [STREETOBJ_NOTES.md](STREETOBJ_NOTES.md)), the movies (movie.c, MPEG-2 and
their ADPCM sound decoded by hand, [MOVIE_NOTES.md](MOVIE_NOTES.md)), the buildings' light and
neighbours' shadows (building.c, city.c, [BUILDING_LIGHT_NOTES.md](BUILDING_LIGHT_NOTES.md)), the water (water.c), the cloth and flags (cloth.c), the game flow and lives (game.c), its
choreography and camera modes (director.c), the block challenges and their upgrades (challenge.c,
[CHALLENGE_NOTES.md](CHALLENGE_NOTES.md)), the Pause menu (hud.c, [PAUSE_NOTES.md](PAUSE_NOTES.md)), the face queue, head turn and friction (look.c), shadows (shadow.c),
effects with their mesh debris and sounds (fx.c), the footfalls, the foot lock and drag with the legs' IK (footstep.c, ik.c,
[FOOTSTEP_NOTES.md](FOOTSTEP_NOTES.md)) and sound (sound.c).
