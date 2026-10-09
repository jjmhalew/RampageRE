# Edibles: power-ups and hazards

This file covers what `src/edible.c` ports, and where each part comes from in the game. Edibles are `Actor`s that
hide behind the buildings' windows. Each one runs its template's script: `Edible.bsf`, or `Cryotube.bsf` for the
monster reveal's tube. They are not held in the hand like people. When a monster "grabs" or touches one, the script's
`onPickup(monster, eat)` runs where the edible stands. The effects on the monster go through `gCommon.addStats`,
`playReaction`, `setSpeedScale` and the rage.

## The kinds (0x31A540, 0x318B40)
The occupant manager lives at 0x4D5F00. Its kind records are 20 bytes each: `+0` name, `+4` sort (1 means edible),
`+8` flag, `+0xC` Frequency, `+0x10` DropRate.

- **Where the kinds come from:** the block's `Edibles { FillRate, EdibleInstance| { Frequency, OTRef|Name } }`. The
  blocks inherit it from `DefBlockProps`.
- **Which records count:** a record needs both a name and a `Frequency`.
- **Hazards:** a kind is a hazard (flag 1) when its name begins with `Hazard` (case-sensitive `strncmp(name,
  "Hazard", 6)`, 0x32D1B0). So `Hazard_Points_*`, `Hazard_Rage`, `Hazard_Coin` ... are "hazards" too: the monster
  touches them with a punch rather than collecting them.
- **Frequency sum:** the edible kinds' Frequencies are summed into `+0x28`.

## The slots (0x3196F0 from 0x319B78)
A slot is 0x18 bytes. Its fields: `+0` building, `+4` column, `+8` floor id (`+0x200[floor]`), `+0xC` side,
`+0xD` occupant kind, `+0xE` edible kind, `+0x10` the edible's actor, `+0x14` the occupant's actor. The edible part
of 0x3196F0 works like this:

1. **The cells.** `0x2BE750(building, list, 300, 1, 100, 6)` calls `0x2D01F0`. That lists the front cells on live
   storeys 1 … min(100, standing − 1), storey by storey and column by column, keeping only cells whose state
   (`cell +0`) is 0.
   - Flag 4 drops the window test (`+0xE & 0x400`, the outline's halves), so every front cell counts.
   - Flag 2 keeps one entry per cell, on a random side.
   - The occupants use flags 2, so they need a window.
   - Storey 0 (z 6 … 9) is never used, for edibles or for occupants. `city.c`'s occupant slots do use it.
2. **How many.** total = `cvt.w.s(0.5 + n × FillRate)`. The building's `NumEdibles` (an int, `+0x7C`) overrides it,
   capped at n. No book sets `NumEdibles`.
3. **Shuffle.** Each index is swapped with `rand() % n`.
4. **The pick.** For each of the first `total` cells: r = rand() / 2³² × sum, and the kind is the first one whose
   running Frequency sum is ≥ r.
   - If the cell already has a slot (an occupant's), it gets the edible in `+0xE`.
   - Otherwise a new slot is made, with occupant 0xFF.
5. **The building's own edibles.** Each `EdibleInstance| { cell, floor, Name }` of the BuildingInstance goes through
   0x318FD0.
   - A name not in the list becomes a new kind with Frequency 0.
   - It replaces the cell's kind or adds a slot.
   - The data uses this for themed rows: New York's Moneybags and Steaks, Las Vegas' aces and coins, London's flags,
     and so on. Some names are lower case (`Powerup_cherry`); the template lookup ignores case.

## When an edible appears
- **A cell breaks** (0x2BB580 → 0x317DC0, when a front cell reaches state 2 or more):
  - the slot's edible actor is created (0x317BC0);
  - exception: while the building is being destroyed (`+0x14C ≥ 4`), hazards stay hidden.
  - the cell is not hit through: canDoPunch says no at a power-up's window.
- **A floor falls** (0x2BAE70 → 0x317A70):
  - the fallen floor's slots lose their actors and kinds;
  - the slots of the floor ids just above and below it (`+0x204`, `+0x1FC`) get their edibles created.
- **Where it stands** (0x317BC0):
  - x = the grid's min x + 5 × column + 2.5;
  - y = the front (min y) + 2.5, half a cell inside;
  - z = 6 + 3 × the storey's place now (0x2CD010);
  - `+0xF4` = 1.5.
- **Its look:** nearly every edible uses `InvisShape`. The look is its `Effect`, attached with no offset.
- **Gone for good:** once an edible is deleted (eaten, expired or lost with its floor), the slot forgets it
  (0x318140: `+0x10 = 0`, `+0xE = 0xFF`).

## Edible.bsf
- **Construct:**
  - the template's `Effect` starts attached to the actor, and turns off when the actor dies;
  - `Rotate` gives it a `RotationMover` of (0, 0, π). Only `Hazard_Points_*` and the cryotube use it.
- **onAnimComplete:** stops the effect and returns 4|8|16, so the actor is deleted. This comes after
  `ActorAnimShape|Default`'s `Duration`, which is 30 s for all window edibles.
- **onPulse:** `requestDelete`.
- **onPickup(monster, eat):**
  1. Stop the effect.
  2. Play `PickupFx` at its place, once (`bPlayedPUEffect`).
  3. If not eaten: `setPulseDelay(0.5)` and return 1, so it is gone half a second later.
  4. `Reaction` R: `playReaction("React_Front_" + R)`, or `"React_Side_L_" + R` when the monster's move name holds
     "Side".
  5. `SpeedFactor`: `setSpeedScale(f, 20)`.
  6. `gCommon.addStats(actor, slot, 3)`.
  7. `ShowBanner` with `Points` > 0: `PointsBanner.createAndStart(points, pos, white, 1.0)`.
  8. `Message`: a literal `\n` becomes a line break, then `createAndStartText(message, MessageColor (default 220, 0, 0),
     0.5, PickupSound)`.
  9. `ExtraEffect`:
     - centred: at the building's place, z = (storeys × 3 + 1.5 + 6) / 2, y − 7;
     - otherwise: its own place, y − 6.
  10. `MonsterFunction`: `callMonsterFunction` (`doFart` for Beans, `doCatSound` for Cat).
  11. Return 16: 0x30C510 takes it out of its slot and deletes it.

## The monster's side (Monster bindings: lookup 0x2AC3B0, handler for id i at table 0x42CEE0[i + 1])
- **getBuildingOccupant** (0x2B37C0):
  - 1 if the slot has an occupant actor;
  - otherwise, if the edible actor exists and the cell state is ≥ 2: 3 for a hazard, 2 for any other kind;
  - otherwise 0.
- **getBuildingOccupantName** (0x2B3510): the edible's name first (whatever the cell's state), else the occupant's.
- **grabBuildingOccupant[AsType](loc, eat)** (0x2B3350):
  - an occupant is held, which is the people's grab;
  - an edible needs cell state ≥ 2. AsType replaces its template (`+0x80`), so the stats, reaction and banner come
    from the substitute. Then `onPickup(monster, eat)` runs. The edible is never held.

Monsters.bsf builds its checks on these:

| Call | Meaning |
|---|---|
| `canGrabPowerup` | occupant == 2 |
| `canDoPunch` | not 1 and not 2 (a hazard can still be punched) |
| `checkForHazard` | == 3 → grab(loc, 1); called in punch, kick and charge clips just before their `hitBuildingFront` |
| `doGrabPowerup` | grab(loc, 1) |
| `destroyPowerup` | grab(loc, 0) |

The Collect clips (`Front_Collect`, `Side_L_Collect`) call `destroyPowerup` at about 0.47 s and `doGrabPowerup`
about 0.42 s later. That is inside the 0.5 s pulse, so the second call eats the edible the first one picked.

Monsters with their own versions grab as another template:

| Monster | Function | Substitution |
|---|---|---|
| Cal, Gilman, Marco, Natalie | `checkForHazard` | `Hazard_BrokenPipe` → `Hazard_Gilman_BrokenPipe` |
| Jill | `checkForHazard` | `Hazard_Shock` → `Hazard_Jill_Shock`, BrokenPipe → Gilman's |
| Nick, Rock | `checkForHazard` | `Hazard_Fire` / `Hazard_FireWithPopcorn` → their own |
| Venus | `checkForHazard` | Fire, FireWithPopcorn, Shock, BrokenPipe → hers |
| Venus | `doGrabPowerup` | Radiation, Biohazard → hers |
| George | `doGrabPowerup` | Banana |
| Crock | `doGrabPowerup` | Cat |
| Icky | `doGrabPowerup` | Cockroach |
| Kingston | `doGrabPowerup` | Poison |
| Rojo | `doGrabPowerup` | Steak (−50 HP) |

The substitutes have no look record; they exist only to be eaten.

## What each kind does (Objects1_Props)
| Kind | Health | Energy | Points | Reaction | Other |
|---|---|---|---|---|---|
| Hazard_Rage | | +100 | 50 | Flex | "InstantRampage" banner, `v_rampage`: starts the rage |
| Hazard_Speed | | | 25 | Ok | speed ×1.25 for 20 s, "SpeedIncrease", `v_speed` |
| Hazard_Shock | −35 | −25 | | Zap | (no PickupFx; the Zap clip has its own effect) |
| Hazard_Fire | −25 | −25 | | Spew | |
| Hazard_BrokenPipe | −10 | −50 | | Heavy | |
| Hazard_Bomb | −55 | | | Heavy | `Car_explode` |
| Hazard_Points_100/250/500 | | | 100/250/500 | Ok | banner, rotates |
| Hazard_Coin/Diamond/Moneybag/King/Aces/UK_Flag | | | 100 … 500 | Flex | banner |
| Hazard_FireWithFireworks | | | 250 | Flex | centred `Fireworks_Comp` |
| Hazard_FireWithPopcorn | −25 | −25 | | Spew | centred `Popcorn` |
| Powerup_ food (Banana, Cake, Donut, Fish, Hamburger, HotDog, Icecream, Melon, Cherry, Chocolate, Steak, Turkey, Crab) | +30 … +50 | | 10 … 100 | Yum/Flex | |
| Powerup_Beans | +30 | | 25 | Yum | `doFart` |
| Powerup_Cat | −20 | | | Choke | `doCatSound` |
| Powerup_Cockroach | −20 | | | Barf | |
| Powerup_Radiation / Biohazard / Poison | −40 | −25 | | Barf/Yuck | |
| Powerup_Syringe | −60 | | | Yuck | |
| Powerup_Lemon / Cactus / RedChile | −25 / −20 / −10 | 0 / −25 / 0 | | Yuck/Choke/Spew | |

## gCommon.addStats (0x2C51A0), the energy and the rage
**addStats:**
- At 0 HP only the points count.
- HP becomes max(1, HP + Health), up to the most.
- Energy becomes e = clamp(energy + Energy, 0, 100):
  - not raging: `setEnergy(e)`, and at e ≥ 100 `Monsters::startRage`;
  - raging: Energy is ignored, unless Energy ≥ 100 (`Hazard_Rage`). Then 0x2D4340 stops the rage, energy is set, and
    `Monsters::startRage` starts it over.
- `Player.addPoints(type, Points)`, then the GUI's "Score".

**The energy:**
- **setEnergy** (0x2E5730): at 100 or more it becomes 99.9 if another monster is raging or full, otherwise 100.
- **addEnergy** (0x2E5720): `setEnergy(energy + x)`.

**The rage:**
- **startRage** (0x2D4360): rage on (`+0xA58`), its clock `+0xA54` = 25, speed scale `+0xA8C` = 1.33 for
  `+0xA90` = 25 s.
- **Monsters::startRage** then:
  - `setEnergy(100)`;
  - the script camera on the monster (target 8 up, from 50 back and 15 up, a cut; back after 1.5 s over 0.5 s);
  - `setReaction(54)` (`*_Rampage`);
  - the "Rage" interface.
- **The update** (0x2E1CB0):
  - the speed clock runs down, then the scale goes back to 1;
  - while raging, the rage clock runs down and the energy falls by 100 × dt / 25 (×3 once the clock is out);
  - 0x2DE530 ends the rage when both are out, or at 0 HP.
- **setSpeedScale** (0x2E8650): sets `+0xA8C` and `+0xA90`. The move runner (0x2EBFE0) advances the clip time by
  dt × `+0xA8C`.

**The checks** (Monsters.bsf):
- `hasRoarEnergy`: energy ≥ 15 with ability 4 (or cheat 8), or raging.
- `hasRoarAttack`, `hasChargePunch`, `hasJumpSmash`: ability 4 / 1 / 8, cheat 8, or raging.
- `canStartRage` (0x2ABF00): energy ≥ 100 and no monster raging.
- `useRoarEnergy`: −15, not while raging.
- `hitBuildingFront` (kinds 0 … 2 and 6, on a hit):
  - +2 energy, not while raging;
  - in the air the energy stops at 99;
  - at 100, `startRage`;
  - while raging the script hits the building twice.

## The cryotube (Cryotube.bsf)
WorldData's `MonsterRevealDef { RevealLoc| { Block, Building, cell, floor } }` puts `Edible_Cryotube` in a cell. The
native code that reads it (0x304150 → 0x3235F0 → 0x318FD0) was not traced.

When its cell breaks:
1. 0x317DC0 calls `onPickup(the world's monster +0x40, 1)`.
2. `PickupFx`, then `addStats(type 0)`, which is 500 points.
3. A `LineMover` carries it 5 towards the street at 20, then 5 more and down to z = 1. It tumbles 2π / 24 every
   1/24 s.
4. When it lands: `HitGroundFx`, `HitGroundSound`, then `startReveal`, the "MonsterRevealed" banner and
   `V_Monster_Reveal_Seq`. Then it is deleted.

## Driving it from the viewer
1. **Setup:**
   - `edible_new` with a host:
     - `find(book, name)` returns `book_group`, as for the army;
     - `cell_state(building, storey as built, column)` returns the front cell's state, −1 if gone;
     - `cell_place` returns where the edible stands (`edible_cell_place(b.min, column, building_live_of(b, o))`);
     - `building_place` is optional (for `ExtraEffectCentered`).
   - Per block: `edible_load_block(BlockDef)`, then `edible_add_building(i, its BuildingInstance| group, its
     template, width, standing storeys)` for each building, in the block's order.
2. **Buildings:**
   - where `scene_buildings_update` calls `people_cell_broken` (a front cell reaching 2 or more), also call
     `edible_cell_broken(i, o, col, state, b.state)`;
   - when a floor starts to fall (`b.collapse` = k changes), call `edible_floor_fell(i, live[k−1], live[k] or −1,
     live[k−2] or −1)`.
3. **Each frame:** call `edible_update(dt)`. Then:
   - draw each edible that is not `EP_GONE` and has a `shape`, at `world`; the others are drawn by their effects;
   - move the attached effects to `pos` (`fx_move`);
   - handle the events:

| Event | What the viewer does |
|---|---|
| `EE_EFFECT` | `fx_spawn`; keep it attached when `edible >= 0` |
| `EE_EFFECT_STOP` | `fx_stop` |
| `EE_SOUND` | `sound_play` |
| `EE_REACTION` | play the state of that name, if HP > 0 |
| `EE_SPEED` | `edible_power_speed` |
| `EE_STATS` | `army_add_stats(army, monster, tmpl, level)` for the score; `edible_power_add_stats(&power, &hp, 200, v[0], v[1], 0)` for HP and energy; if that returns 1: `edible_power_start_rage`, slot 54, the camera and the Rage banner |
| `EE_BANNER` / `EE_MESSAGE` | the HUD |
| `EE_MONSTER_FUNCTION` | `doFart` / `doCatSound` |
| `EE_REVEAL` | the monster reveal |
| `EE_REMOVED` | nothing to do |

4. **Monster calls:** for any clip event or transition check named `*::canGrabPowerup`, `*::checkForHazard`,
   `*::doGrabPowerup`, `*::destroyPowerup` or `Monsters::canDoPunch`:
   - find the cell with `people_probe` and convert the live storey to the storey as built;
   - pass `person_there` (`people_window_at` ≥ 0) to `edible_monster_call`; it returns −1 for anything it does not
     answer.
5. **The monster's energy:**
   - keep an `EdiblePower` per monster, and send `AE_STATS_ENERGY` and `hitBuildingFront`'s energy through it;
   - call `edible_power_update(dt, hp)` each frame;
   - play clips at dt × `power.speed`;
   - let `edible_power_answer` answer the ability checks.

## Not traced (stand-ins in edible.c)
- **Random numbers:** `rand()` (0x106950) and 0x106BD0; an LCG stands in.
- **Lifetime:** that the actor plays its `Default` record, so the 30 s lifetime comes from its `Duration`. The Actor's
  init (0x30D280) starts only a `DefaultAnim`.
- **Movers:**
  - the RotationMover's units (taken as radians per second);
  - the tube's LineMover end (it stops at its last point);
  - placement stays fixed after the edible appears, as the game sets it once.
- **The cryotube:**
  - when the native code places it (which monster is to be revealed);
  - the monster its `onPickup` names (taken as monster 0).
- **Building and banner details:**
  - `b.getPosition()` / `getSizeZ()` without the host's `building_place`;
  - the scale of `MessageColor` (taken as 0 … 255);
  - a fixed slot's side (0x2D0460, taken as 0).
- **The rage's side:**
  - the rage's other parts (0x2F4710 / 0x2F46D0 on the monster `+0x7DFC`);
  - which abilities the player has unlocked (`getMonsterFlags`, taken as all four).
