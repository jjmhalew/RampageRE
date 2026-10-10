# The game flow's choreography: goals, the AI's moves, the camera modes

How the game carries out what the flow scripts ask for (`docs/GAME_NOTES.md`): a monster plays a move or walks to a
point, and the camera changes mode, cuts or blends. Ported in `src/director.c` / `src/director.h`, plus
`cam_activate` in `src/camera.c`. Addresses are in RAMPAGE.ELF. **[R]** means read in the code or the files;
**[G]** means a guess or a stand-in.

## The script calls

| Script | Binding | What it does |
|---|---|---|
| `monster.playMove(move, start, end)` | Monster 0x9B (0x2AC1A8) | a PlayMoveGoal: the state by name (0x2D9890), queued, callbacks by name |
| `monster.walkTo(pos, r, start, end)` | Monster 0x9C (0x2AC220) | a WalkToGoal with one way point; returns it (`WalkToGoal`) |
| `wgi.addWayPoint(pos, r)` | WalkToGoal (0x2A5FA0 → 0x2F2020) | one more way point |
| `monster.clearAllGoals()` | Monster 0x9A (0x2EE760) | every goal deleted, no callback |
| `gCameraMgr.setCamMode(name, blend)` | CameraMgr (0x2A7370 → 0x27EDF0) | see below |
| `gCameraMgr.setPrevCamMode(blend)` | 0x27EDB0 | back to the mode the last `setCamMode` left |
| `gCameraMgr.getCamMode()` | 0x27EE70 | the mode's name |
| `ScriptCamera.setTarget / setLocation(obj, local, x, y, z, cut)` | ScriptCam ids 0 / 1 (0x2A6E30 → 0x3123B0 / 0x312370) | what it looks at / where it is |
| `ScriptCamera.cut()` | id 3 (0x312300) | everything snaps at once |

The Monster bindings: lookup 0x2AC3B0, dispatcher 0x2AA920, jump table 0x42CEE0 (index = id + 1). The goal manager is
the monster's +0xB92C.

**walkTo's second argument is a radius, not a speed [R]:** 0x2F20E0 and 0x2F2020 keep r² with each way point, and
the walk is at a way point when the monster's x, y are within r of it. `EndBlockNI` passes 1.5.

## The goals (MonsterAI, +0xB92C)

Layout: +0xC the stick, +0x18 the monster, +0x1C the current goal, +0x20 / +0x24 the queue, +0x28 the AI's "pad",
+0x88 the monster is AI. A goal: +0x8 type (0 play move, 1 walk, 2 destroy buildings; 0x2ED810), +0x10 / +0x14
the start and end callbacks, +0x18 flags (bit 0 begun, bit 1 delete when over: set on every new goal).

- **Adding** (0x2EE600, 0x2EE650): the goal goes to the end of the queue; the current goal, if any, is stopped and,
  having bit 1, deleted without its end callback. A script that queues several goals at once (EndBlockNI: walk,
  TurnAround, Idle) loses nothing, since no goal is current between the calls.
- **Each frame** (0x2EE430; run for an AI monster by the runner step 0x2EBFE0 in place of the pad's pick 0x2EC3D0,
  [R 0x2EC124]): the stick is cleared; the queue's head becomes current; its update runs; when it says over, it is
  unlinked and deleted, and the AI asks for an idle (action 11) the same frame. With no goal: action 11. The next goal
  starts the next frame.
- **Begun / over** (0x2F21C0): a goal begins (start callback) the first frame its update says it runs; it is over
  (end callback: `Levels::moveComplete`) the first frame after that its update says it does not.
- **PlayMoveGoal** (0x2F1DB0): in its state (and the state's clip has not started over since it began): it runs and
  asks for action 15 (only transitions that need nothing). Not in it and not begun: if the next-hop table leads there
  (0x2F2B90) it picks the transition to the next state on the way (0x2F2970); otherwise the state is forced (0x2D9710:
  the start list at the monster's +0x110, taken by the controller next frame, no transition). Not in it and begun:
  over. The state's clip starting again (0x2E6580 → 0x2EE3F0 → 0x2F1D80) also ends it. **"The move ended" is the
  monster leaving the move's state (or the move looping)**: React_Intro, React_Victory, Ground_TurnAround, Ground_Taunt
  are unreachable (their transitions are the game's own events, on "false"), so they are forced; their clip end leads
  to Ground_Idle and the goal is over one frame later. A played move the monster is already in (Ground_Idle after the
  turn-around) runs until that state's clip starts over.
- **WalkToGoal** (0x2F1F20): at the current way point (x, y within its radius): the next one; past the last: over
  (if it never began, both callbacks at once). Otherwise 0x2EDB40: the stick is the flat direction to the way point,
  length 1 ((1, 0) on top of it), and the AI asks for action 9 (the walk). The stick is in world axes: 0x2C0AD0 writes
  it ×255 into the per-player request for an AI monster, and 0x2E8410 reads an AI monster's stick as the world x, y
  (a player's goes through the camera's axes) [R]. So the move graph's Ground_Walk walks it (its root motion) and
  NODE_STEER turns it to the stick.

## How the AI picks transitions (the "pad", +0x28)

The AI never tests the pad conditions. It picks transitions itself, among the current state's, and the pick is taken
when its switch window opens, as a player's (the runner's +0x1E, 0x2EBF40) [R]:

- **Usable** (0x2F22D0): 2 = not for the AI (not open to a pad (flag 0x10), a condition on `false` (the game's own
  events), the script check says no); 0 = needs nothing (every condition is on `true`); 1 = needs an input (taken
  anyway).
- **By action** (0x2F2630): over the transitions in their input window and at least the priority already picked: the
  first usable-0 one is picked at once; else the last of the highest priority whose target state's node has that
  action code (node +0x3C) and is usable (not 2, target node flag 0x40000000 clear), unless the pick already goes
  there. With none, the state's own transition for that action (state +0x18 + action, a byte, −1 none; no window
  needed). Action 15 skips all but the first rule.
- **By state** (0x2F2970): the same with "the target is that state" (not when it is in it already), no fallback.
- **The next-hop table** (0x2F2B90): MONSTER_DATA ends with an n × n byte table after the states (176² for George):
  [from × n + to] the next state on the way, 0xFF none. A target is reachable when each hop has a usable transition
  (or is the clip's end).

Action codes (node +0x3C) for George: 0 attacks and grabs, 1 the transitions onto a building, 4 hops, 5 jumps, 6
mantles, 7 climbing down, 8 climbing up, **9 Ground_Walk**, 10 the mirror transition, **11 the idles** (Ground_Idle,
Side/Front idles, Faceup_*), 12 the roof unmantle, 13 everything else.

## The camera manager

`gCameraMgr` is the RCameraMgr at 0x4D3100 (vtable at +0x74; +0x1B4 the view's aspect, see `CAMERA.md`), +0x6C its
controller (RCamController, CamController 0x167370 / 0x167400 / 0x167690), +0x70 the mode before.
The modes (table 0x433A60, name → object → loader): `TwoTilts` 0x4D5C80 (RTTCamera, `camera.c`), `Free` 0x4D5E60,
`Follow` 0x4D5D10 (FollowPlayerCamMode), `Script` 0x4D5D60 (RScriptCam); "default" is the first. Each mode object has
+0xC activate, +0x10 deactivate, +0x14 update (the camera: 9 floats of rows, the place, fov, near, far), +0x1C the blend
flags (0x167660: 1 for TwoTilts and Script).

- **setCamMode(name, blend)** (0x27EDF0): the mode before := the current; if the new one differs, setMode.
- **setPrevCamMode(blend)** (0x27EDB0): if the current is not the mode before, setMode(the mode before).
- **setMode(mode, blend)** (0x167400): nothing if the mode is the one being blended from. The mode is activated
  (TwoTilts 0x313A50: its next frame is taken as it is, state 2, timer 1e20; Script 0x312100: it starts from the camera
  shown, nothing moving). Blend ≤ 0.001: a cut. Otherwise the old mode, its flags, and a copy of the camera shown now
  are kept, timer = blend.
- **Each frame** (0x167690): the current mode's camera. While blending: the timer falls by dt; at 0.001 or less the
  blend ends. Else t = timer / blend (1 → 0); the old mode is updated too (flags ≠ 0) and with flag 1 its live place
  replaces the copy's (flag 2 its rotation, 4 near / far); the rotation is a slerp from the new camera's towards the
  copy's by t (0x1042F0 / 0x167DE0, the old turned to the new's side), the place, fov, near and far a lerp. **So a blend
  is linear in time, the old camera's rotation frozen when it began, its place live.** Then the mungers (the shake).
- Why `BeginLevelNI` sets `Free` then `TwoTilts`: setCamMode to the mode already current does nothing, so the detour
  through Free makes TwoTilts activate again and snap to the monsters at once.

## The script camera (RScriptCam)

camera.prb's `Script` group is empty; the loader 0x3121C0 sets: nothing linked, smoothing (+0x54) 0.95, inertia on
(+0xEC place, +0xED rotation), a place follower (+0xA8, 0x13B1B0: speed 1000, accel 1000, radius 0.1), a rotation
follower (+0x58, 0x13AD70: π/2 rad/s, π/2 rad/s², 0.001) and a fov follower (+0xDC, 0x13B2E0: π/2 rad/s). The
followers keep per-frame steps (× the frame's dt at load: gWorld +4, the world's step, 1/30) [R 0x3121C0; the step
0x13AF00 takes no dt].

**The world's step** [R]: the world's init sets its rate +8 = 60 (50 below a 51 Hz display, 0x285A20), then RWorld's
(0x2B7800) +8 = 30 (25) and the step +4 = 1/30 (1/25); nothing else writes it. The game's tick (0x288340) and the
camera manager (0x27EFA0) pass that step as their dt, and `gWorld.getTimeStep()` (0x2AEFB8) answers it: the game
runs at 30 steps a second. The port's script modules (army, parade, people, rail, streetobj, vehicle) use 1/30 for
it, and the camera's smoothing is per 1/30 s step (`frame_hz` 30).

- **setTarget / setLocation(obj, local, x, y, z, cut)**: with an object, its place plus the offset (local: through its
  matrix) each frame; with `nul`, the offset is a world point. The sixth argument cuts.
- **Update** (0x311820): the place's goal; the place follower steps (0x13AF00: a speed of 2 × the distance per second,
  its change limited to accel × dt, the speed to 1000; there when within 0.1 and slow) or snaps (a cut, or no inertia).
  The look: rows forward = to the point, right = forward × z, up = right × forward, as a quaternion goal; the rotation
  follower steps (the same law on the angle, 0x13A980) or snaps, and the fov follower. What it shows is smoothed:
  last + (new − last) × 0.05 a frame (0x103FB0), taken as it is after a snap.
- **cut** (0x312300): the update forced at once (everything snaps, the smoothing restarts).
- The fov stays the one it took over (0.5, TwoTilts' FOV) unless `setFovTarg`; it reports near = far = 0.

## The scripts' camera moves

| Where | What |
|---|---|
| `BeginLevelNI` | Free (0), TwoTilts (0): TwoTilts snaps |
| `BeginBlockNI` | TwoTilts (0.5): nothing, it is current |
| `ResultsNI` | Script (0), at (20, −75, 2) looking at (5, −33, 8) (3 players (25, −73, 2) → (0, −33, 8), 4: (28, −73, 2) → (−2.5, −33, 8)), cut |
| `EndBlockNI` | TwoTilts (1.5): from the results shot, over 1.5 s |
| `Monsters::startRage` | Script (0.25) unless Script already; target the monster + (0, 0, 8), location the monster + (0, −50, 15) (world offsets, the camera follows the monster), cut; after 1.5 s `setPrevCamMode(0.5)` |
| `NewYork::BossResultsNI` | Script (0) at (4, −55, 78) → (−3, 10, 57), cut; at 1.25 s, without a cut, to (−2.61, −13.75, 74.4) → (−3.08, 11.93, 66.46) (the followers carry it there in about 3 s); TwoTilts (1.5) when the centre building falls |
| `NewYork::CrossParkNI` | Script (0) placed 41 back from the camera (cut), then TwoTilts (1.5) |
| Boss intros | `Levels::changeTwoTiltCameraRMH(35)`: TwoTilts' RepMonsterHeight (boss.c) |

## The HUD fade

`GuiInterface.setFade(f)` (GuiObject method 0x16, case 0x256F48 of the table 0x425750) stores f clamped to 0 … 1 at
+0x5C at once: no fade over time [R; the dispatcher read as GuiObject's]. `BeginBlockNI` also `setVisible(1)`s each
`Hud<n>`. The animated fades are the scripts' own (Clock.bsf, the results screens).

## The rubble: findPointInRubble [R]

`ResultsNI` puts three effects in the rubble of the block just won, with
`gWorld.findPointInRubble((0, −21, 0), 8, &loc)` (World id 0x38, case 0x2AEFE8 → 0x303670). For each point found:
`Rubble_Smolder` when `(int)(2 × frand) < 1` and loc.y > −23, otherwise `Fire_Loop`; it goes at loc + (0, 0, 1.5).
Ported in `src/reveal.c` (`rubble_load`, `rubble_find_point`).

**The list** (the block manager's +0x18, 0x4D4E98: a count, then 16-byte slots of a place +4 … +0xC and a shape at
+0x10, room for 16):
- **When it is built:** activateNextBlock (0x302AF0) calls 0x303C90(list, the BlockList entry left behind,
  had_previous). The entry left behind is +0xC, which 0x302800 sets from +0x8. had_previous is 0 on the city's first
  activation, or when the block left is the new one (a replayed King tie). The list is always emptied first, and
  with had_previous 0 it stays empty.
- **What goes in:** each `BuildingInstance|` of that BlockDef that passes these checks:
  - its LeftEdgeXPos is in [−50, 50) (0x303EB4);
  - its template (`TemplateName`, 0x285710) has a `RubbleShape`. Its `Width` comes from the template, its `Row`
    from the instance.
- **Its place:** x = LeftEdgeXPos + 2.5 × Width, y = −36, plus the BlockDef's `BackRowOffset` when Row ≠ 0, and z =
  0. This is the block left behind as it lies after activateNextBlock: 41 towards −y, with its fronts 5 in.
- **What is not in it:** nothing is checked about the building's state. Every building of the block left is in the
  list, standing or not (at the block's end they have all fallen).

**The search** (0x303670):
1. **The boxes.** Each slot's box is its shape's bounds (+0x20 … +0x34, min and max) moved to the slot's place. A
   box counts when it meets the sphere (0x100810: the box's nearest point is within the radius). If none does, the
   search answers 0 and that effect is skipped.
2. **The random numbers.** They come from 0x106BD0 on gWorld +0x828: seed = 16807 × seed mod (2³¹ − 1), the
   minimal standard generator. Each number is that value × 2⁻³¹, and three are drawn per call.
3. **The box.** The pick is `(int)((n − 0.01) × u)` among the boxes that counted.
4. **The point:**
   - x = min.x + 0.5 + (max.x − min.x − 1) × u;
   - y is worked out the same way;
   - z = min.z, the rubble's base;
   - a point farther than the radius from the centre (3D) is moved onto the sphere: centre + d × radius / |d|.

In Hong Kong's HK03 the six buildings give boxes about 16.6 × 11 (rubble −0.3 … 1.9 high), at x ±12.5, ±37.5 (back
row, y −26) and ±45 (front, y −36). The sphere around (0, −21) catches the two middle ones. Most picks land outside
it and are pulled onto it, so the fires sit about 8 from the centre, at z ≈ −0.2 (+1.5).

## What the port does not have

- findPointInRubble [R]: the shape's +0x20 … +0x34 are the union of its parts' boxes (0x193B80), which equal the
  meshes' vertex bounds (all 1645 type-0 meshes of HongKong.bdp and Chicago.bdp checked), so `city_shape_box` gives
  the same box; every BlockDef has its own BackRowOffset (the game would read address 0 without one); the search
  shares gWorld +0x828 with `frand` (0x106BD0), drawing only when a box counts, and ResultsNI draws its fire's frand
  after a point is found (Levels.scr 355 … 358): `game_rng`, `game_frand`.
- `Free` (0x166BA0) is a debug fly camera on a pad, holding the camera without input; the scripts pass through it
  within one call [R]. `Follow` (0x3124E0 / 0x3123F0) keeps the shown camera's turn and fov and follows player 1's
  monster at the offset it had when it began (a world offset; 0 without a monster) [R]; as here, `DCM_FOLLOW` (no
  script uses it). A local offset is
  off.x × row0 + off.y × row1 + off.z × row2 + place of the object's matrix (0x311884) [R]; here it turns by the
  object's facing, the same for an upright object (every script passes 0). The script camera's callback
  (`setCallback`), `setFovTarg`, `enableInertia`, `setPosParms` … are not bound (no script calls them).
- The walk (WalkToGoal 0x2F1F20) calls 0x2EDB40 with mode 0 and no avoidance: the stick along the flat direction,
  action 9 [R]; only the bots' callers avoid other monsters. 0x4D6A9A is a debug byte nothing writes [R].
- **[G]** A play-move goal whose state is unknown ends at once. In the game (0x2D9890 → NULL, 0x2F1DB0) it never
  begins and is forced every frame: no moveComplete, the queue stuck. Only `Ground_Shakeoff` is missing, asked by
  Levels.scr only when frand comes out at exactly 1.0 (odds about 6e-8).
