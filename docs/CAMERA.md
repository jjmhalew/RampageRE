# The play camera ("TwoTilts")

The camera that follows the monsters in a city. Its settings are a property book of their own, `camera.bdp`
(`camera.prb`, four groups: `Follow`, `Free`, `Script`, `TwoTilts`); the code is the `RTTCamera` class (vtable
0x44B410), update 0x312B50. `src/camera.c` is a transcription of it.

## Settings
A table at 0x433A60 pairs each camera mode's settings object with its name and loader: `TwoTilts` → 0x4D5C80,
loader 0x3134E0 → 0x3134F0; `Free` → 0x4D5E60 (loader 0x311590); `Follow` → 0x4D5D10; `Script` → 0x4D5D60
(0x311580). The loader reads the keys through the property name table at 0x3F5990.

| Field | Key | Value | |
|---|---|---|---|
| +0x04 | `FOV` | 0.5 | vertical field of view, radians |
| +0x08 | `SmoothingFactor` | 0.9 | share of the last frame's camera that stays each frame |
| +0x0C | `NearTilt` | 0 | tilt near the ground |
| +0x10 | `FarExtraTilt` | −0.2 | added to it as the monsters spread out |
| +0x14 | `FarBorderScale` | 2 | margins grow to this as they spread out |
| +0x18 | `AboveTilt` | −0.3 | tilt when the lowest monster is higher up |
| +0x1C | `StateTransTime` | 0.3 | seconds the near / above state must hold before it switches |
| +0x20 | `MaxAngularVel` | 2 | tilt change per second |
| +0x24 | `MinCamDist` | 75 | above: the least distance |
| +0x28 | `MinCamHeight` | 10 | near: the least height |
| +0x2C | `RepMonsterHeight` | 15 | the height a monster stands for |
| +0x30 | `HeldObjMinCamDistOffset` | 5 | added to both minimums while a followed monster holds something not edible (0x2E5590: no flag 0x80000; a car, a tank, a street object, not a person) or threw something a second ago (+0xA94 = 1 at throwObject's throw, 0x2DA284, counted down in 0x2E1E88; 0x312CB0 .. 0x312D1C, the byte sp+0xC0 to 0x312950 and 0x312790) [R] |
| +0x34 | `Yaw` | 0.4 | largest turn about Z, radians |
| +0x38 | `ShowGroundHeight` | 30 | the lowest monster at or below this: the near state |

Derived by the loader: +0x40 / +0x44 sin and cos of half the FOV, +0x3C their 1 / sin; +0x48 / +0x4C sin and cos
of the horizontal half angle (tan = tan(FOV / 2) / r with r the global at 0x4D32B4), +0x50 its 1 / sin, +0x54 1 / tan.
r is +0x1B4 of `gCameraMgr` (the RCameraMgr at 0x4D3100), i.e. +0x13C of its view at +0x78 (made by 0x16BC10 at
0x27F43C: 1.0, the screen 320 × 240 at +0x154 / +0x158), set by the view's projection 0x16B6C0 (fov, near, far,
aspect): the mode passes aspect 0 (0x27F2F0), which makes it height / width (+0x158 / +0x154) × the global
0x4CB54C [R]. That factor sits in the display context at 0x4CB480 (+0xCC, .bss; +0xD0 next to it is the refresh rate,
which picks a 1/30 or 1/25 s world step at 0x2B7848); nothing stores to it by address, and the code that fills it
through a pointer was not found [G: the display's pixel shape; the viewer uses its window's height / width, the
picture's own shape, which such a factor would correct the frame buffer's to].
State: +0x5C first frame, +0x60 quaternion and +0x70 position of the smoothed camera, +0x7C near (1) or above (0),
+0x80 the time the other state has held, +0x84 the current tilt.

## A frame (0x312B50)
1. The box of the monsters' positions (0x2DAF20: the root of the monster's matrix) and of two more tracked objects
   (from the list of 0x2BFE80 over 0x4D3100, clamped to x within ±30 of the box centre, y −10 … 21, z 0 … 70). The
   point looked at is the box centre raised by `RepMonsterHeight` / 2. Spread s = min(width / 110, 1)².
2. State: near when the box's lowest point is at or below `ShowGroundHeight`; a change only counts once it has held
   for `StateTransTime`. Target tilt: near `NearTilt` + `FarExtraTilt` × s, above `AboveTilt`; the tilt moves to it
   at most `MaxAngularVel` × dt per frame (at once on the first frame). The camera's rows are then right (1, 0, 0),
   forward (0, cos t, sin t) and up (0, −sin t, cos t); k = 1 + s × (`FarBorderScale` − 1).
3. Near (0x312950): 0x313A70 finds the height and depth from which the view's lower and upper edges (tilt ∓ FOV / 2)
   just hold every monster plus `RepMonsterHeight` above it; then it backs off along the lower edge until the box's
   width plus 20k fits the horizontal angle, and rises to at least `MinCamHeight`, keeping the lower edge on the
   same ground point. Above (0x312790): 0x313C10 gives the distance at which every point's up offset (and the same
   plus `RepMonsterHeight` × k) fits the vertical half angle and its x offset plus 5k the horizontal one; the camera
   sits that far back along forward, and further back along the upper edge to at least `MinCamDist`.
4. 0x312600 keeps the side of the view within the street: the plane of the view's side edge may not pass x = ±60
   (at y = −15), times k.
5. Yaw: `Yaw` × x / 60, clamped to ±`Yaw`, a turn about Z applied to the axes and to the camera's offset from the
   point looked at (0x1015B0, 0x101A90).
6. The new camera (as a quaternion, 0x105090) and the last one blend: last + (new − last) × (1 − `SmoothingFactor`),
   i.e. 0x103FB0(new, `SmoothingFactor`, last); the first frame takes the new one as it is. The output is the matrix
   (0x104BB0), position and FOV.

The smoothing works per frame of the camera manager, which runs the modes' updates (TwoTilts' 0x312B50 at 0x44B424,
the script camera's 0x311750 at 0x44B3E4) once a world step, 1/30 s (gWorld +8). The viewer takes
`SmoothingFactor`^(30 dt) for a drawn frame of dt, as for the script camera (director.c `frame_keep`); up to v0.11.4
it took 0.9 a drawn frame, so at 60 Hz the play camera caught up twice as fast as the PS2's.

## Shake
The shake only turns the camera, by a small yaw X and pitch Y, after the TwoTilts smoothing (a camera "munger",
`CamShakeApplier` 0x27F550, run by `CamController::update` 0x167690), so it never feeds back into it. Its position
is kept. Shakes are effects (`type = camerashake`, `CameraShakeFx` vtable 0x446750, its definition 0x1E8C70):
`val.duration`, and degree-1 graphs of t = age / duration for `graph.mag.x/y` and `graph.theta_mul.x/y` (an angle,
not a factor: its slope is the frequency), `graph.wave` sin.

| Effect | duration | mag | theta x | theta y | started by |
|---|---|---|---|---|---|
| `camshake` | 0.75 | 0.005 − 0.005 t | 160 − 40 t | 135 − 50 t | a monster's clip events (type 9, its strength); a floor falling (`Collapse_Floor_*_comp`, 1) |
| `camshake_collapse` | 0.5 | 0.014 − 0.014 t | 160 − 80 t | 135 − 100 t | a toppling billboard (`building_collapse_comp`), the rage (`Rage.edf`), 1 |

- **Start** (0x1E7ED0): age 0, the strength, random phases (Park-Miller × 2⁻³¹ × 2π), its place. Each monster has
  one camshake of its own; a new start is dropped while the old one is stronger: strength × max(mag(t)) against
  the new strength × max(mag(0)) (0x1E8420).
- **Each frame** (0x1E86D0): x = strength × mag.x(t) × sin(phase.x + theta.x(t)), y likewise; nothing once t ≥ 1.
- **Sum** (0x207050): X = Σ x × f, Y = Σ y × f, f = 1 within 100 of the camera, falling to 0.2 at 800.
- **Turn**: S = rows (cX, sX, 0), (−cY sX, cY cX, sY), (sY sX, −sY cX, cY) (0x101330); the rows (right, forward,
  up) become S × rows; a right row tilted by more than 0.001 in z is levelled and the frame made square again.

A strength of 3 or 4 (the butt slam, the heavy punches) turns the camera by about 0.015 … 0.02 rad, at 8 … 11 Hz.
