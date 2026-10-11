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
  `ActorAnimShape|Default`'s `Duration`, which is 30 s for all window edibles [R]: no edible has a `DefaultAnim`, so
  neither 0x317BC0 (0x317D3C) nor the init (0x30D5D0) starts an anim, but the current record (+0xE8) is 0 from the
  constructor (0x30E45C) and the actor's update plays it (0x30CE0C, the record's vtable +0xC; done →
  onAnimComplete): their only record, `Default`.
- **Rotate's RotationMover** (frame 0x31C540) [R]: its rate × the world's step is added to the facing (0x101220,
  then setFacing 0x101330): (0, 0, π) is half a turn a second.
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
When its cell breaks:
1. 0x317DC0 calls `onPickup(gWorld's monster list +0x40 [0], 1)` (0x318020) **[R]**. That is slot 0, whoever broke
   the cell, so the 500 points go to player 1.
2. `PickupFx`, then `addStats(type 0)`, which is 500 points.
3. A `LineMover` carries it 5 towards the street at 20, then 5 more and down to z = 1. It tumbles 2π / 24 every
   1/24 s (a pulse, at most one a frame). The mover is the people's (PEOPLE_NOTES.md, 0x315E10): the share of a
   segment grows by 20 × dt / its length, and past 1 the next segment starts from its beginning, the rest dropped.
4. When it lands: `HitGroundFx`, `HitGroundSound`, then `startReveal`, the "MonsterRevealed" banner and
   `V_Monster_Reveal_Seq`. Then it is deleted.

### The reveal object (0x4D6A10) [R]
Ported in `src/reveal.c` / `reveal.h`. The World bindings are `startReveal` (id 0x31, case 0x2AED78), `stopReveal`
(0x32, 0x2AEDE8) and `wasMonsterRevealed` (0x47, 0x2AF388). The table at 0x42B040 lists the ids, and the jump table
0x42DD70 gives the cases.

| Offset | What it holds |
|---|---|
| +0x00 | four `RevealLoc`s of 16 bytes: Block, Building, floor, cell |
| +0x40 | the monster to reveal (a roster index) |
| +0x44 | how many have been revealed; −1 when there is nothing to reveal |
| +0x48 | how many there are to reveal (0 or 1) |
| +0x4C | the number of RevealLocs |
| +0x50 | the tubes placed in this block |
| +0x54 / +0x58 | the slot shown and the monster revealed (−1 for none) |
| +0x5C | `wasMonsterRevealed`'s countdown |
| +0x60 | whether the monster is shown (one player only) |

**The roster.** GameStatus +0x10 holds 12-byte records with the name at +8. Its count is at +0x18 (`getNumMonsters`).
0x33DAF0 finds a name, ignoring case. The order is ShellData's `MonsterData.Monster0 .. 29`: George 0, Lizzie 1,
Ralph 2, … Crock 8, … Venus 13, … Boosy 29, as MonsterSelect's `getMonsterName` reads them: the loader 0x33E850
walks the MonsterData keys starting "mons", the index atoi(key + 7), at most 30 [R]. A monster's flags are at GameStatus +0x234 + 4 × index. `0x80` means it can be selected
(MonsterSelect's `isMonsterAvailable` tests `flags & 128`). 1, 2, 4 and 8 are the upgrades.

**The load** (0x3236F0, at the world's start from 0x2B6BE0):
1. +0x44 = −1, and +0x48, +0x4C, +0x50 and +0x60 = 0.
2. Only in the campaign (0x4D7E44 = 2). With one player (0x4D7E28 = 1), +0x60 = 1.
3. Then it reads each record of WorldData's `MonsterRevealDef`, in order:
   - **`RevealDef| { Monster = name  - name … }`** counts only while no monster has been chosen. Its `Monster` is
     taken when:
     - no player plays it (by name, ignoring case);
     - the roster has it;
     - it is not yet 0x80;
     - its list names player 1's monster (GameStatus +0x214) or `"everyone"`, ignoring case.

     Then +0x40 = that monster, +0x48 = 1 and +0x44 = 0. The slot after the players (GameStatus +0x214 + 4 ×
     players) gets that monster. So at most one monster per city visit, and it is the first that fits.
   - **`RevealLoc| { Block, Building, cell, floor }`**: ints, −1 when a key is missing. At most 4 are kept.
4. 0x2B6BE0 and 0x2B72B0 then make players + min(+0x48, 1) monsters when +0x60 is set. The extra one (slot ≥
   players) gets +0x108 = +0x109 = 1 (0x2E35C0, 0x2E34B0). It is loaded but hidden and out of the play.

The data has three RevealDefs and three RevealLocs per city. The first RevealDef is "Everyone"'s, except in New York.

| City | RevealDefs (Monster: whose players) | RevealLocs (Block, Building, floor, cell) |
|---|---|---|
| Las Vegas | Leon: everyone; Kingston: Lizzie Gilman Leon Kyle Icky Wally Natalie Dick; Jack: Venus Harry | (2,2,3,2) (6,2,5,0) (8,7,1,1) |
| San Francisco | Marco: everyone; Snapper: George Lizzie Cal Crock Harry Kingston Dick; Natalie: Gilman Rhett Crock Marco Snapper Rock Batty | (3,3,1,0) (7,2,1,3) (9,5,2,1) |
| London | Kyle: everyone; Icky: George Ralph Ramsey Leon Crock Kyle Wally Snapper Rock Jill; Dick: Kyle Wally Amanda | (2,3,3,1) (4,0,2,2) (7,6,1,1) |
| Los Angeles | Cal: everyone; Amanda: Lizzie Rhett Cal Kyle Rojo Wally Jill; Batty: Snapper Kingston | (2,1,1,2) (5,0,1,0) (7,4,4,1) |
| Chicago | Rojo: everyone; Harry: Ramsey Gilman Leon Marco Venus Amanda Furby Nick; Nick: Harry Rock | (2,0,3,1) (5,4,3,0) (8,2,6,0) |
| Hong Kong | Crock: everyone; Venus: Ralph Leon Crock Rojo Harry Kingston Batty Jack; Jill: Snapper Amanda | (3,3,4,2) (5,4,7,0) (6,3,5,2) |
| New York | Wally: Ramsey Rhett Kyle Marco Icky Natalie Nick; Rock: George Ralph Cal Kyle Rojo Venus Amanda Furby; Furby: Rojo Icky Natalie | (3,0,3,2) (6,3,5,0) (9,2,6,1) |

So George in Hong Kong frees Crock, and on the next visit nothing (no other RevealDef lists him). Ralph frees Crock,
then Venus.

**The tubes** (0x3235F0, at each block's activation from 0x304150, right after the edibles' 0x31A540):
1. +0x50 = 0, and +0x5C counts down (if it is not 0).
2. While +0x44 < +0x48, each RevealLoc whose Block is the current block (0x4D4E88, the BlockList index) and whose
   Building is below the block's building count (0x4D0958) gets a tube:
   `0x318FD0(edibles, building[Building], "Edible_Cryotube", floor, cell, 1, 1, 0)`, then +0x50 + 1.

   The 7th argument is the new kind's +8, the flag a `Hazard*` kind has (the EdibleInstance call passes 0 and
   MilitaryInstance 1). So `getBuildingOccupant` answers 3 for the tube. A cell broken once the building is being
   destroyed (+0x14C ≥ 4) does not let it out (0x317F9C). `edible_add_cryotube` does this.
3. A tube left unbroken simply goes with its block. The next RevealLoc block gets another, until one is broken.

**startReveal(pos)** (0x323520, from the binding 0x2AED78):
1. If no tube was placed in this block (+0x50 ≤ 0), nothing happens.
2. Otherwise:
   - if +0x44 > 0, +0x60 = 0;
   - with +0x60 set, the slot shown is players + +0x44, and 0x2D40B0 shows that monster at pos;
   - +0x58 = +0x40 [+0x44];
   - its flags (0x4D7BC4 + 4 × monster) |= 0x80: **the profile unlocks it**;
   - +0x50 = 0, +0x44 + 1, and +0x5C = 2.
3. The binding answers +0x58 (the banner's `getMonsterName`). Without +0x60 it then calls `stopReveal` at once.

**stopReveal** (0x3234D0): with +0x60 set, 0x2D3F90 hides the shown slot's monster (+0x109 = 1, taken out of the
world). Then +0x54 = +0x58 = −1.

**wasMonsterRevealed** answers +0x5C ≠ 0. It is 2 at the reveal. The next block's activation, which happens at the
block's end before the results, takes it to 1, so Results shows "Monster Revealed" (`!bIsLastBlock`). One more
activation takes it to 0.

**The Continue** (0x3250B0) calls 0x323450 when `wasMonsterRevealed` is set:
- +0x44 − 1 (if it is above 0), so the tube comes back;
- the shown monster is hidden, and +0x54 / +0x58 = −1;
- +0x5C = 0.

The monster's 0x80 is not cleared there. The world's reset (0x2B8040 → 0x323CB0) clears everything.

### The revealed monster comes out (one player) [R]
0x2D40B0 looks for `"<Monster>::onRevealNITick"` and falls back to `"Monsters::onRevealNITick"`. Only Monsters.bsf has
it. Then:
1. +0x109 = 0, and the monster is added to the world.
2. It is put at the tube's landing point, facing π.
3. The function is called with the monster.

`Monsters::onRevealNITick`:
1. The first call, when there is no timer yet, sets everything up:
   - `setIsAI(1)` and revealTime 0;
   - a 0.25 s object timer on itself;
   - `Ground_Idle`;
   - `setDynamicScale(0.1, 0)`, then `(1.0, 1.0)`: it grows to full size in 1 s.
2. On each tick, revealTime is checked, then goes up by 0.25:
   - at 0.5, `Ground_Taunt`;
   - at 4.0, `walkTo((100, −10, 0), 1, "", "Monsters::onRevealNIEnd")` and `setSpeedScale(1.5, 100000)`. x becomes
     −100 when the monster is left of `getCameraPos().x`. A facing (π/2, mirrored) is worked out but never used;
   - at 5.0, the timer is removed.
3. `onRevealNIEnd` calls `gWorld.stopReveal()`: the monster is hidden again.

`installObjectTimer(0.25, 0.0, 0)` (0x28A4A0; ticked by 0x28A780 each world step: acc += 1/30, at the period acc =
min(acc − period, 1) and it fires, once a step) [R]: the first tick on the 8th step (0.267 s), the taunt (tick 3)
about 0.77 s and the run (tick 17) about 4.27 s. reveal.c's timer is that one.

**Hiding** (0x2D3F90) [R]: the monster's reset 0x2DCB40 (scale 1, speed scale 1 for 0 s, …) and its AI's goals
cleared (0x2EE760), +0x109 = 1, then out of the world's list (0x2C1A80 / 0x2C1A90 on 0x4D3330 + 0x18); main.c's
`extra_hide` does the same. **Showing** (0x2D40B0): +0x109 = 0, into the list, the matrix from the place and the
facing π (0x1015B0), then the found `onRevealNITick` called with the monster.

So the monster bursts out of the tube on the street, taunts, and runs off to the side. With two or more players it
does not appear; only the banner shows and the profile changes. The HUD shows only Cryotube.bsf's banner: `"#" +
format(localize("MonsterRevealed"), localize(getMonsterName(i)))` ("%s Unlocked"), (220, 0, 0), scale 0.5,
`V_Monster_Reveal_Seq`. The results' "Monster Revealed" appears at the block's end.

### All Monsters Revealed and Boosy [R]
At each block's end (0x3255B0) the check below runs when the city is done, or when `isAtCheckpoint` answers, which
it always does (0x3030B0 answers 1). It runs in every play type:
1. 0x323240 answers 1 when every RevealDef's `Monster` is 0x80 in the profile. A city without a MonsterRevealDef
   also answers 1. Then the city's flags (GameStatus +0x2AC) |= 8.
2. Then all saved city flags (GameStatus +0x100 + 4 i, i < GameStatus +0xC) are ANDed together. The current city
   counts its saved value | 8, not +0x2AC. When the result has 1, 2, 4 and 8, Boosy's flags |= 0x80.

The flag tracks the profile, not this visit: a city whose three monsters were freed earlier earns its fourth star
at the first block's end.

## Driving it from the viewer
1. **Setup:**
   - `edible_new` with a host:
     - `find(book, name)` returns `book_group`, as for the army;
     - `cell_state(building, storey as built, column)` returns the front cell's state, −1 if gone;
     - `cell_place` returns where the edible stands (`edible_cell_place(b.min, column, building_live_of(b, o))`);
     - `building_place` is optional (for `ExtraEffectCentered`).
   - Per block: `edible_load_block(BlockDef)`, then `edible_add_building(i, its BuildingInstance| group, its
     template, width, standing storeys)` for each building, in the block's order. Then the monster reveal's tubes:
     `reveal_block(&reveal, game_block, nbuildings, locs, 4)` and `edible_add_cryotube(building, floor, cell)` for
     each loc it returns (reveal.h; `reveal_load` once per city, at the world's start).
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
| `EE_REVEAL` | `reveal_start(&reveal, players, &roster, &slot)`: the banner names `roster.name[i]`; slot ≥ 0: `reveal_walk_start(&walk, slot, pos)` |
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
- **Random numbers:** `rand()` is the lagged Fibonacci 0x106950 on the world's gWorld +0x82C (shared with the bots'
  rand01, seeded from GameStatus seed[1]; wrand.c) [R].
- **Movers:** placement stays fixed after the edible appears [R as far as traced: 0x317BC0 places it once, and an
  edible made after the block's start is not attached to its building (0x309E20 runs only then), so neither the
  building's shake nor its fall moves it].
- **The cryotube (reveal.c):**
  - the roster's order [R: ShellData's MonsterData, as the loader 0x33E850 reads it]; which monsters a profile
    starts with is the profile's (profile.c; main.c seeds `RevealRoster.flags` from it);
  - that RevealLoc's Building indexes the block's building list in BlockDef order: the list is 0x4D095C (the RGridMgr
    0x4D0900's +0x5C, its count +0x58), filled as the block's buildings are made; that they are made in BlockDef order
    is taken, as `edible_add_building` numbers them;
  - the walk-on in the viewer (main.c `reveal_events`): the monster is shown at the tube's landing place, z = 1 [R: the
    tube's last point], and falls the last 1 to the street by the monster's own physics; `playMove` and `walkTo` go
    to its AI as goals, and the walk's end callback comes when the WalkTo goal is done [G: the AI's goal machinery is
    director.c's];
  - four RevealLocs name a floor their building does not have (Las Vegas blocks 2 and 6, Los Angeles 7, New York 9:
    floor 3 of a 1-storey building, 5 of 4, 4 of 3, 6 of 6). The port builds those buildings as the game does (one
    storey per `Floor|` entry but the last, empty one), and the Block is the world's index +0x878 that 0x302AF0
    stores at 0x4D4E88, as GE_BLOCK's. The game keys the tube's slot by the storey table's entry for the floor
    (0x319128: building +0x200[floor]); the grid fills that table only for 0 .. its storey count (0x2D24A8:
    `table[k] = k`), so these tubes get the id of a storey the building lacks, and no broken cell lets them out,
    in the game as in the port **[R]**. Past the filled part [R]: the table is the grid's +0x48 words (the grid at
    the Building's +0x1B8, so building +0x200; ctor 0x2BFA10 -> 0x2D31F0, which zeroes +0x44, +0x94, +0x98 .. +0xBC
    and +0x658 .. +0x6EC but never +0x48 .. +0x93: room for 19), written only by that fill; the pool does not clear
    a freed slot (0x283BF0 rewrites +0 .. +0xC), so past it the game holds stale memory and the tube's id is
    undefined (it could by chance be a real storey's). The port's floor number is a stand-in for that.
- **Building and banner details:**
  - `b.getPosition()` / `getSizeZ()` without the host's `building_place` (the viewer gives one);
  - the scale of `MessageColor` (taken as 0 … 255, as the default's `set255`; the getColorProp binding's body was not
    read);
  - a fixed slot's side (0x2D0460: a byte of the cell's tile per side, at +0x100; taken as 0). An edible's slot opens
    no window, so the side shows only if an occupant shares the cell.
- **The rage's side:**
  - the rage's other parts (0x2F4710 / 0x2F46D0 on the monster `+0x7DFC`);
  - the abilities: `EdiblePower.abilities` starts at all four, until the viewer sets them from the profile
    (gCommon.getMonsterFlags; main.c `profile_moves`) [R].
