# The block challenges

Each block of the campaign has a challenge ("Destroy 4 Fire Trucks"). Meeting it is worth 5,000 points, and on most
blocks it also teaches player 1's monster a new move. This file covers the game's side, traced from the ELF: the
challenge object, what counts towards it, the upgrade and the city's flag. `src/challenge.c` ports it. The HUD side
(ChallengeStatus, ChallengeText, Pause) is in [SCREENS_NOTES.md](SCREENS_NOTES.md) and `src/hud.c`. [R] means read
in the code or the files, [G] means a guess.

## Where it lives

| What | Where |
|---|---|
| The challenge object | 0x4D5050 = block manager (0x4D4E80) + 0x1D0 |
| Its load | 0x3468F0, called from 0x302CA8 in 0x302AF0 (the block becomes current: activateNextBlock 0x2B6580) |
| A goal by name? | 0x3466A0 (from the world object's init 0x30D280: the `ChallengeEffect`) |
| Count an award | 0x346780 (from addStats 0x2C51A0, the building's Scoring 0x2BA4B0, the block won 0x3255B0) |
| Met? | 0x346860 (from the block won 0x3255B0 and gAIMgr's update 0x3333F0) |
| Sums | 0x3464F0 required, 0x3465D0 collected, 0x346690 met (+0x138) |
| The upgrade object | gAIMgr (0x4D7800) +0x50 bit 4 (made this block), +0x58 its delay; `aquireUpgrade` 0x2A62B8 |
| The moves table | 0x3F67A0: `ChargePunch` 1, `DoubleJump` 2, `RoarAttack` 4, `JumpSmash` 8 |

**The object** [R]:

| Offset | What |
|---|---|
| +0x0 + 0x4C n | goal n (n < 4): +0x0 up to 16 template names, +0x40 their number, +0x44 Required, +0x48 collected |
| +0x130 | the number of goals |
| +0x134 | `Name` (the identifier) |
| +0x138 | met: 0 not (yet), 1 met, −1 no goals. Only 0x346860 writes it |
| +0x13C | the upgrade on offer: 0 or a move's flag |

**The World bindings** [R] (lookup 0x2B02D0 at 0x2B0A6C .. 0x2B0B84, descriptors 0x42B1B0 .. 0x42B220, cases of
the dispatcher 0x2AE3D0 via the jump table 0x42DD70):

| Method | Id | Case | Answer |
|---|---|---|---|
| `getChallengeUpgrade` | 0x48 | 0x2AF3C0 | +0x13C |
| `wasChallengeMet` | 0x49 | 0x2AF3F0 | +0x138 (0x346690) |
| `getChallengeIdentifier` | 0x4A | 0x2AF428 → 0x2B4970 | the current block's `ChallengeDef.Name` |
| `getChallengeIcon` | 0x4B | 0x2AF458 → 0x2B47C0 | `ChallengeDef.Icon` |
| `getChallengeIconScale(pause)` | 0x4C | 0x2AF488 | `PauseScale` if pause ≠ 0 and the key is there, else `Scale` |
| `getChallengeIconShift` | 0x4D | 0x2AF5C8 | `ShiftX` |
| `getChallengeItemRequired` | 0x4E | 0x2AF698 → 0x3464F0 | the goals' Required, summed; −1 with no goal |
| `getChallengeItemCollected` | 0x4F | 0x2AF6D0 → 0x3465D0 | their counts, summed |
| `hideChallengeCompletionUI` | 0x66 | 0x2B01C0 | removes the `Removeable` banner (the CHALLENGE MET / 5,000 PTS one) |

The four book getters read the current block's `ChallengeDef` every time (0x303160 on the block manager). They do
this whatever the play type and whether or not goals were loaded. Scale and ShiftX must be float records. A missing
key writes nothing into the answer, which the float type made 0.0 first (gp −0x6EF0, TypePFloat 0x1CE830:
`sw zero, 4(a1)`), so a missing Scale or ShiftX is 0 [R]. A missing Name or Icon (not a string record) is given to the
answer string as none, which clears it to "" (0x2B4970: 0x2B4A48 .. 0x2B4A70) [R]. (`wasMonsterRevealed`, id 0x47, sits
next to them: 0x4D6A6C ≠ 0.)

## The data
`ChallengeDef` is a group of each block's `BlockDef|` in `<City>_Props` [R]. Its keys:
- `Name`, `Icon`, `Scale`, and on some blocks `PauseScale` and `ShiftX`;
- `Resource|` (the icon's bitmap, for the bundle builder);
- one `Goal| { Required = n  - "Template" ... }`;
- on the ordinary blocks four `Upgrade| { Monster = "Rhett"  Upgrade = "DoubleJump"  - "Cal" - ... }`.

No book sets `Points`. ChallengeStatus asks for `ChallengeDef.Points` with a default of 5000, so every challenge is
worth 5,000. No block uses a type key or a "Clock" / "Score" goal. Every goal names templates, Required 1 … 20.

The identifier is a key of the city's `.loc`. The boss blocks use `ChallengeBoss`, and their card shows
`ChallengeBossTitle`, `ChallengeBoss`, `ChallengeBossObjectives` and `ChallengeBossHint` instead. Game.loc has the
rest:
- `CT_Challenge` "Bonus Challenge" and `CT_Challenge_Upgrade` "Upgrade Challenge";
- `Reward` "Reward:" and `ChallengeReward` "5,000 PTS";
- `Challenge_Met` "CHALLENGE MET!";
- `MonsterUpgrade` "Monster\nUpgrade";
- `Upgrade1` "CHARGE PUNCH", `Upgrade2` "MULTI-SPIN", `Upgrade4` "ROAR ATTACK" and `Upgrade8` "JUMP SMASH";
- `Inst_Upgrade1/2/4/8`, how to do each move.

**The upgrades** [R data]: on every ordinary block the four `Upgrade|` groups share out the 30 monsters. Each group
names one move, and each monster is in exactly one group (its `Monster` or its list). So each monster has its own move
to win on each block. There are two typos:
- CG02_Block2 lists `Eygore`, so Eyegore gets nothing there;
- CG07_Block7 lists `Natalaie`, so Natalie gets nothing there.

The boss blocks and the unused CG07_Block8 have no `Upgrade|`.

The column "Upgrades" gives each group as move: Monster + list. CP is ChargePunch (1), MS DoubleJump (2, "MULTI-SPIN"),
RA RoarAttack (4) and JS JumpSmash (8). Blocks are in BlockList order, the first entry not repeated and the buffer
blocks left out. Blocks with a ChallengeDef that no BlockList names come last.

## Every block's challenge [R]

### Chicago

| Block | Identifier | Text (English) | Goal templates | Required | Icon (Scale / PauseScale / ShiftX) | Upgrades (1 CP, 2 MS, 4 RA, 8 JS: Monster + list) |
|---|---|---|---|---|---|---|
| CG01_Block1 | Challenge01 | Destroy 4 Fire Trucks | FireTruckVehicle | 4 | Pause_FireTruck (1.2) | MS: Rhett + Cal, Rock, Dick, Batty, Nick, Eyegore<br>JS: George + Leon, Venus, Snapper, Amanda, Jill, Boosy<br>CP: Ralph + Kyle, Rojo, Icky, Natalie<br>RA: Lizzie + Ramsey, Gilman, Crock, Marco, Wally, Harry, Kingston, Furby, Jack, Philbert |
| CG02_Block2 | Challenge02 | Collect 1 Gold Coin | Hazard_Coin | 1 | Gong (1) | MS: Lizzie + Gilman, Rhett, Cal, Rojo, Batty<br>JS: George + Marco, Wally, Snapper, Dick, Jack, Jill, Nick, Eygore, Boosy<br>CP: Ramsey + Crock, Icky, Venus, Harry, Rock<br>RA: Ralph + Leon, Kyle, Amanda, Natalie, Kingston, Furby, Philbert |
| CG03_Block3 | Challenge03 | Destroy 4 Trains | SlowCommuterTrain1, SpeedingCommuterTrain1 | 4 | Pause_Train (1.2) | MS: Rojo + Nick, Philbert, Eyegore<br>JS: Ralph + Gilman, Cal, Leon, Marco, Amanda, Kingston, Dick, Jack, Jill<br>CP: Lizzie + Ramsey, Crock, Kyle, Venus, Snapper, Natalie, Rock, Boosy<br>RA: George + Rhett, Icky, Wally, Harry, Furby, Batty |
| CG04_Block4 | Challenge04 | Eat 4 Grenadiers | SoldierGrenadierFromTruck3 | 4 | Pause_Granadier (1) | MS: Gilman + Cal, Dick, Batty, Nick, Eyegore<br>JS: Rhett + Venus, Snapper, Amanda, Kingston, Jill, Boosy<br>CP: George + Ralph, Ramsey, Kyle, Rojo, Icky, Rock<br>RA: Lizzie + Leon, Crock, Marco, Wally, Harry, Natalie, Furby, Jack, Philbert |
| CG05_Block5 | Challenge05 | Destroy 8 Tanks | Tank17sec | 8 | Pause_Tank (1.2) | MS: Gilman + Rojo, Rock, Dick, Nick, Philbert<br>JS: Ralph + Ramsey, Rhett, Leon, Crock, Wally, Amanda, Jill, Eyegore<br>CP: George + Cal, Harry, Snapper, Natalie, Furby, Boosy<br>RA: Lizzie + Kyle, Marco, Icky, Venus, Kingston, Batty, Jack |
| CG06_Block6 | Challenge06 | Destroy 6 Trains | SlowCommuterTrain1, SpeedingCommuterTrain1 | 6 | Pause_Train (1.2) | MS: George + Lizzie, Leon, Kyle, Marco, Icky, Harry, Eyegore<br>JS: Ramsey + Cal, Natalie, Dick, Batty, Nick<br>CP: Rhett + Venus, Rock, Furby, Jill<br>RA: Ralph + Gilman, Crock, Rojo, Wally, Snapper, Amanda, Kingston, Jack, Philbert, Boosy |
| CG07_Block7 | Challenge07 | Eat 5 Hot Dogs | Powerup_HotDog, HotdogCart1 | 5 | Powerup_HotDog (1.2) | MS: Ralph + Ramsey, Rhett, Dick, Batty, Jill, Philbert, Eyegore<br>JS: George + Lizzie, Gilman, Cal, Venus, Kingston, Nick, Boosy<br>CP: Leon + Rojo, Icky, Snapper<br>RA: Crock + Kyle, Marco, Wally, Harry, Amanda, Natalaie, Rock, Furby, Jack |
| CG09_Block9 | Challenge09 | Eat 6 Bus Commuters | Reader | 6 | Pause_Reader (1) | MS: George + Ralph, Cal, Wally, Kingston, Rock, Jill<br>JS: Marco + Venus, Harry, Snapper, Furby, Philbert<br>CP: Lizzie + Gilman, Rhett, Crock, Kyle, Rojo, Icky, Amanda, Natalie, Dick, Batty, Jack, Nick, Eyegore, Boosy<br>RA: Ramsey + Leon |
| CG10_Block10 | ChallengeBoss | the boss card (ChallengeBoss*) | BossSaucer | 1 | Pause_Saucer (1) | - |
| CG07_Block8 (not in BlockList) | Challenge08 | Eat 5 Tourists | TouristFemale1, TouristFemale2, TouristMale1, TouristMale2, TouristMale3, TouristMale4, TouristMale5, TouristMale6 | 5 | Pause_Tourist (1) | - |

### London

| Block | Identifier | Text (English) | Goal templates | Required | Icon (Scale / PauseScale / ShiftX) | Upgrades (1 CP, 2 MS, 4 RA, 8 JS: Monster + list) |
|---|---|---|---|---|---|---|
| GB01_Chelsea | Challenge01 | Collect 4 British Flags | Hazard_UK_Flag | 4 | Pause_BritFlag (2.25 / 2) | MS: Lizzie + Ralph, Rock, Jill, Nick, Eyegore<br>JS: Gilman + Cal, Marco, Venus, Wally, Amanda, Kingston, Dick, Boosy<br>CP: Ramsey + Rhett, Crock, Kyle, Rojo, Icky, Harry, Natalie<br>RA: George + Leon, Snapper, Furby, Batty, Jack, Philbert |
| GB02_Mayfair | Challenge02 | Destroy 5 Chimneys | ChimneyLondon | 5 | Pause_SmokeStack (1) | MS: Gilman + Cal, Dick, Batty, Nick, Eyegore<br>JS: George + Rhett, Venus, Snapper, Amanda, Jill, Boosy<br>CP: Ramsey + Kyle, Rojo, Icky, Natalie, Rock<br>RA: Lizzie + Ralph, Leon, Crock, Marco, Wally, Harry, Kingston, Furby, Jack, Philbert |
| GB03_WestEnd | Challenge03 | Collect 5 Money Bags | Hazard_Moneybag | 5 | moneybag (1) | MS: George + Ralph, Cal, Kyle, Rojo, Icky, Wally, Kingston<br>JS: Ramsey + Rhett, Leon, Marco, Dick, Jack, Nick, Philbert, Eyegore<br>CP: Crock + Venus, Harry, Snapper, Rock, Furby, Jill, Boosy<br>RA: Lizzie + Gilman, Amanda, Natalie, Batty |
| GB04_BakerSt | Challenge04 | Eat 4 Watermelons | Powerup_melon | 4 | Powerup_melon (1) | MS: Cal + Rojo, Harry, Jill, Philbert<br>JS: Gilman + Crock, Wally, Snapper, Kingston, Batty, Jack, Eyegore<br>CP: George + Lizzie, Ralph, Icky, Rock, Dick, Furby, Nick, Boosy<br>RA: Ramsey + Rhett, Leon, Kyle, Marco, Venus, Amanda, Natalie |
| GB05_Trafalgar | Challenge05 | Eat 15 SWAT | SWATFromTruck0, SWATWindow0 | 15 | Pause_Swat (1) | MS: Ramsey + Marco, Natalie, Jill, Philbert<br>JS: Lizzie + Ralph, Cal, Leon, Icky, Dick, Furby, Nick<br>CP: George + Gilman, Rhett, Kyle, Venus, Amanda, Jack, Eyegore<br>RA: Crock + Rojo, Wally, Harry, Snapper, Kingston, Rock, Batty, Boosy |
| GB06_PicadillyCircus | Challenge06 | Pop the Ralph Balloon | ParadeBalloonRalph | 1 | Pause_Ralph (1.25 / 1.1) | MS: Lizzie + Gilman, Rhett, Cal, Dick, Batty, Nick, Eyegore<br>JS: George + Ramsey, Venus, Snapper, Amanda, Jill, Boosy<br>CP: Ralph + Kyle, Rojo, Icky, Natalie, Rock<br>RA: Ralph + Leon, Crock, Marco, Wally, Harry, Kingston, Furby, Jack, Philbert |
| GB07_Parliament | Challenge07 | Eat 6 Royal Guards | RoyalGuard | 6 | Pause_RoyalGuard (1 / x -10) | MS: George + Rhett, Rock, Dick, Batty, Nick, Eyegore<br>JS: Marco + Venus, Natalie, Kingston, Jill, Boosy<br>CP: Lizzie + Ralph, Cal, Leon, Crock, Rojo, Icky, Harry, Snapper<br>RA: Ramsey + Gilman, Kyle, Wally, Amanda, Furby, Jack, Philbert |
| GB08_BigBen | ChallengeBoss | the boss card (ChallengeBoss*) | BossSaucer | 1 | Pause_Saucer (1) | - |

### LasVegas

| Block | Identifier | Text (English) | Goal templates | Required | Icon (Scale / PauseScale / ShiftX) | Upgrades (1 CP, 2 MS, 4 RA, 8 JS: Monster + list) |
|---|---|---|---|---|---|---|
| LV01_Casino1 | Challenge01 | Eat 2 Ice Cream Cones | Powerup_Icecream | 2 | Ice_cream (1) | MS: Gilman + Rhett, Cal, Dick, Batty, Nick, Eyegore<br>JS: George + Venus, Snapper, Jill, Boosy<br>CP: Ralph + Ramsey, Crock, Kyle, Rojo, Icky, Natalie, Rock<br>RA: Lizzie + Leon, Marco, Wally, Harry, Amanda, Kingston, Furby, Jack, Philbert |
| LV02_Casino2 | Challenge02 | Collect 4 Aces | Hazard_Ace_Spd | 4 | Ace_Sp (1.5) | MS: George + Cal, Dick, Batty, Nick, Eyegore<br>JS: Ramsey + Leon, Marco, Venus, Natalie, Kingston, Jill, Boosy<br>CP: Lizzie + Gilman, Kyle, Rojo, Wally, Snapper<br>RA: Ralph + Rhett, Crock, Icky, Harry, Amanda, Rock, Furby, Jack, Philbert |
| LV03_Casino3 | Challenge03 | Reveal a Jackpot | Powerup_cherry | 3 | Powerup_cherry (1) | MS: Lizzie + Ramsey, Cal, Dick, Batty, Nick, Eyegore<br>JS: George + Snapper, Amanda, Jill, Boosy<br>CP: Rhett + Kyle, Rojo, Icky, Venus, Natalie, Rock<br>RA: Ralph + Gilman, Leon, Crock, Marco, Wally, Harry, Kingston, Furby, Jack, Philbert |
| LV04_Lancelots | Challenge04 | Collect 2 Kings | Hazard_King | 2 | King_Card (1) | MS: Lizzie + Amanda, Kingston, Rock, Jill, Philbert<br>JS: Ralph + Rhett, Cal, Marco, Venus, Wally, Dick, Jack, Eyegore<br>CP: George + Ramsey, Crock, Icky, Harry, Snapper, Furby, Nick, Boosy<br>RA: Gilman + Leon, Kyle, Rojo, Natalie, Batty |
| LV05_JollyRogers | Challenge05 | Eat 10 Mimes | Mime | 10 | Pause_Mime (1 / x -10) | MS: Gilman + Rhett, Cal, Dick, Batty, Eyegore<br>JS: George + Lizzie, Leon, Venus, Snapper, Amanda, Jill, Nick, Boosy<br>CP: Ralph + Ramsey, Kyle, Rojo, Harry, Natalie, Rock<br>RA: Crock + Marco, Icky, Wally, Kingston, Furby, Jack, Philbert |
| LV06_NoName | Challenge06 | Collect 3 Gold Coins | Hazard_Coin | 3 | Gong (1) | MS: Rhett + Rojo, Harry, Nick, Philbert<br>JS: Ramsey + Gilman, Leon, Marco, Wally, Kingston, Dick, Jack, Jill, Eyegore<br>CP: Ralph + Cal, Icky, Snapper, Rock, Furby, Boosy<br>RA: George + Lizzie, Crock, Kyle, Venus, Amanda, Natalie, Batty |
| LV07_HardLuck | Challenge07 | Eat 3 Hamburgers | Powerup_hamburger | 3 | Powerup_hamburger (1) | MS: George + Ralph, Marco, Rojo, Wally, Rock<br>JS: Gilman + Natalie, Kingston, Furby, Batty, Nick, Philbert, Eyegore, Boosy<br>CP: Lizzie + Rhett, Leon, Crock, Venus, Harry, Amanda, Dick, Jack, Jill<br>RA: Ramsey + Cal, Kyle, Icky, Snapper |
| LV08_Sinbads | Challenge03 | Reveal a Jackpot | Powerup_cherry | 3 | Powerup_cherry (1) | MS: Ralph + Gilman, Rock, Dick, Batty, Jill, Eyegore<br>JS: Rhett + Cal, Snapper, Amanda, Jack, Nick<br>CP: George + Kyle, Rojo, Icky, Venus<br>RA: Lizzie + Ramsey, Leon, Crock, Marco, Wally, Harry, Natalie, Kingston, Furby, Philbert, Boosy |
| LV09_Neros | ChallengeBoss | the boss card (ChallengeBoss*) | BossTank | 1 | Pause_Carbonator (1) | - |

### LosAngeles

| Block | Identifier | Text (English) | Goal templates | Required | Icon (Scale / PauseScale / ShiftX) | Upgrades (1 CP, 2 MS, 4 RA, 8 JS: Monster + list) |
|---|---|---|---|---|---|---|
| LA01_MidwayStudios | Challenge01 | Destroy 5 Police Cars | PoliceCarWithCopMedium | 5 | Pause_PoliceCar (1.25) | MS: George + Gilman, Cal, Rock, Dick, Batty, Nick, Eyegore<br>JS: Ralph + Leon, Venus, Snapper, Kingston, Jill<br>CP: Lizzie + Ramsey, Rojo, Harry, Boosy<br>RA: Rhett + Crock, Kyle, Marco, Icky, Wally, Amanda, Natalie, Furby, Jack, Philbert |
| LA02_StudioB | Challenge02 | Pop the Lizzie Balloon | ParadeBalloonLizzie | 1 | Pause_Lizzie (1.2) | MS: Gilman + Cal, Rock, Dick, Batty, Nick, Eyegore<br>JS: George + Rhett, Venus, Snapper, Amanda, Jill, Boosy<br>CP: Ralph + Ramsey, Kyle, Rojo, Icky, Natalie<br>RA: Lizzie + Leon, Crock, Marco, Wally, Harry, Kingston, Furby, Jack, Philbert |
| LA03_BackLot | Challenge03 | Eat 10 Business People | HumanMaleBusiness | 10 | Pause_OfficeWorker (1.25 / 1.15) | MS: Lizzie + Rojo, Philbert<br>JS: Ramsey + Gilman, Leon, Kyle, Marco, Wally, Dick, Jack, Jill, Eyegore, Boosy<br>CP: George + Rhett, Cal, Crock, Snapper, Natalie, Rock, Nick<br>RA: Ralph + Icky, Venus, Harry, Amanda, Kingston, Furby, Batty |
| LA04_La_Brea | Challenge04 | Destroy 8 Helicopters | HelicopterMedium, HelicopterHomingMedium, NewsHelicopterLA | 8 | Pause_Helicopter (1.5) | MS: Ralph + Rhett, Dick, Batty, Nick, Eyegore<br>JS: George + Ramsey, Cal, Venus, Snapper, Amanda, Jill, Boosy<br>CP: Kyle + Rojo, Icky, Rock<br>RA: Lizzie + Gilman, Leon, Crock, Marco, Wally, Harry, Natalie, Kingston, Furby, Jack, Philbert |
| LA05_Burbank | Challenge05 | Destroy 5 Tanks | Tank14sec | 5 | Pause_Tank (1.2) | MS: Rhett + Cal, Harry, Dick, Batty, Jill, Nick<br>JS: Lizzie + Marco, Snapper, Amanda, Kingston<br>CP: Ralph + Gilman, Crock, Rojo, Icky, Venus, Rock, Furby, Eyegore, Boosy<br>RA: George + Ramsey, Leon, Kyle, Wally, Natalie, Jack, Philbert |
| LA06_Rodeo_Drive | Challenge06 | Collect 3 Diamonds | Hazard_Diamond | 3 | Powerup_Diamond (1) | MS: Lizzie + Kyle, Icky, Philbert<br>JS: Gilman + Cal, Crock, Wally, Nick, Eyegore, Boosy<br>CP: George + Rhett, Leon, Venus, Harry, Amanda, Natalie, Kingston, Rock, Dick, Furby, Jack<br>RA: Ralph + Ramsey, Marco, Rojo, Snapper, Batty, Jill |
| LA07_Beverly_Hills | Challenge07 | Eat 5 Cakes | Powerup_Cake | 5 | Powerup_Cake (1) | MS: George + Ralph, Ramsey, Marco, Rojo, Venus, Wally, Kingston, Jack<br>JS: Rhett + Cal, Leon, Amanda, Natalie, Dick, Furby, Batty, Nick, Philbert<br>CP: Lizzie + Crock, Kyle, Icky, Snapper, Jill<br>RA: Gilman + Harry, Rock, Eyegore, Boosy |
| LA08_Hollywood_Blv | ChallengeBoss | the boss card (ChallengeBoss*) | BossTank | 1 | Pause_Carbonator (1) | - |

### SanFrancisco

| Block | Identifier | Text (English) | Goal templates | Required | Icon (Scale / PauseScale / ShiftX) | Upgrades (1 CP, 2 MS, 4 RA, 8 JS: Monster + list) |
|---|---|---|---|---|---|---|
| SF01_Fishermans_Wharf | Challenge01 | Eat 3 Crabs | Powerup_Crab | 3 | Powerup_Crab (1.25 / 1.25) | MS: Cal + Rock, Batty<br>JS: Ralph + Marco, Venus, Kingston, Dick, Jill, Nick, Eyegore, Boosy<br>CP: George + Lizzie, Crock, Kyle, Rojo, Harry, Amanda, Natalie<br>RA: Ramsey + Gilman, Rhett, Leon, Icky, Wally, Snapper, Furby, Jack, Philbert |
| SF02_The_Park | Challenge02 | Destroy 5 Cable Cars | CableCarForTurnaround | 5 | Pause_CableCar (1.2) | MS: Lizzie + Gilman, Rhett, Cal, Dick, Batty, Nick, Eyegore<br>JS: George + Venus, Snapper, Amanda, Jill, Boosy<br>CP: Ralph + Ramsey, Kyle, Rojo, Icky, Rock<br>RA: Leon + Crock, Marco, Wally, Harry, Natalie, Kingston, Furby, Jack, Philbert |
| SF03_Pacific_Heights | Challenge03 | Eat 5 Skaters | RollerbladerMale | 5 | Pause_Skater (1) | MS: Rojo + Dick, Eyegore<br>JS: George + Rhett, Cal, Marco, Wally, Snapper, Jack, Jill, Nick, Boosy<br>CP: Ramsey + Venus, Natalie, Furby<br>RA: Lizzie + Ralph, Gilman, Leon, Crock, Kyle, Icky, Harry, Amanda, Kingston, Rock, Batty, Philbert |
| SF04_HaightSt | Challenge04 | Eat 6 Bikers | Biker | 6 | Pause_Biker (1.1) | MS: Rhett + Cal, Dick, Batty, Nick, Eyegore<br>JS: George + Ramsey, Venus, Snapper, Jill, Boosy<br>CP: Ralph + Gilman, Crock, Kyle, Rojo, Icky, Harry, Natalie, Rock<br>RA: Lizzie + Leon, Marco, Wally, Amanda, Kingston, Furby, Jack, Philbert |
| SF05_SoMa | Challenge05 | Eat 10 SWAT Officers | SWATFromTruck0, SWATWindow0 | 10 | Pause_Swat (1) | MS: George + Lizzie, Ramsey, Rojo, Wally, Rock, Jill<br>JS: Gilman + Leon, Natalie, Kingston, Furby, Jack, Philbert, Eyegore<br>CP: Rhett + Cal, Crock, Icky, Snapper, Amanda, Dick, Nick, Boosy<br>RA: Ralph + Kyle, Marco, Venus, Harry, Batty |
| SF06_Tenderloin_District | Challenge06 | Destroy 4 Streetlights | StreetLamp2 | 4 | Pause_StreetLight (1) | MS: Gilman + Harry, Rock, Dick, Batty, Nick, Philbert, Eyegore<br>JS: Ramsey + Rhett, Cal, Venus, Wally, Snapper, Amanda, Jill, Boosy<br>CP: George + Ralph, Kyle, Rojo, Icky<br>RA: Lizzie + Leon, Crock, Marco, Natalie, Kingston, Furby, Jack |
| SF07_Chocolate_Factory | Challenge07 | Eat 4 Giant Chocolate Bars | Powerup_Chocolate | 4 | Chocolate_Bar (1.1) | MS: Ralph + Cal, Kyle, Rojo, Icky, Kingston, Furby, Philbert<br>JS: Crock + Marco, Natalie, Dick, Batty, Nick<br>CP: Lizzie + Rhett, Leon, Venus, Harry, Jack, Jill, Eyegore, Boosy<br>RA: George + Ramsey, Gilman, Wally, Snapper, Amanda, Rock |
| SF08_MarketSt | Challenge08 | Eat 4 Steaks | Powerup_Steak | 4 | Powerup_Steak (1.2) | MS: George + Ralph, Marco, Harry, Dick, Batty, Jill, Nick, Eyegore<br>JS: Lizzie + Ramsey, Gilman, Rhett, Cal, Leon, Wally, Amanda, Kingston, Jack<br>CP: Venus + Snapper, Natalie, Rock, Furby, Boosy<br>RA: Crock + Kyle, Rojo, Icky, Philbert |
| SF09_Chinatown | Challenge09 | Launch 1 Rocket | Rocket | 1 | Pause_Rocket (1.25 / 1.1) | MS: Lizzie + Gilman, Rhett, Cal, Kingston, Jill, Philbert<br>JS: Venus + Amanda, Dick, Nick, Eyegore<br>CP: George + Ramsey, Crock, Kyle, Rojo, Icky, Snapper, Rock, Boosy<br>RA: Ralph + Leon, Marco, Wally, Harry, Natalie, Furby, Batty, Jack |
| SF10_Presidio | ChallengeBoss | the boss card (ChallengeBoss*) | BossHelicopter | 1 | Pause_Scumhawk (1) | - |

### HongKong

| Block | Identifier | Text (English) | Goal templates | Required | Icon (Scale / PauseScale / ShiftX) | Upgrades (1 CP, 2 MS, 4 RA, 8 JS: Monster + list) |
|---|---|---|---|---|---|---|
| HK01_WaterfrontA | Challenge01 | Launch 2 Rockets | Rocket | 2 | Pause_Rocket (1.25) | MS: Gilman + Cal, Dick, Batty, Nick, Eyegore<br>JS: George + Rhett, Venus, Snapper, Jill, Boosy<br>CP: Ralph + Ramsey, Kyle, Rojo, Icky, Harry, Rock<br>RA: Lizzie + Leon, Crock, Marco, Wally, Amanda, Natalie, Kingston, Furby, Jack, Philbert |
| HK02_WaterfrontB | Challenge02 | Destroy 5 SCUM Freighters | ScumFreighter | 5 | Pause_Freighter (1.2) | MS: Lizzie + Rhett, Cal, Dick, Eyegore<br>JS: George + Ralph, Ramsey, Venus, Amanda, Jill, Nick, Boosy<br>CP: Gilman + Crock, Kyle, Rojo, Snapper, Rock<br>RA: Leon + Marco, Icky, Wally, Harry, Natalie, Kingston, Furby, Batty, Jack, Philbert |
| HK03_Faceoff | Challenge03 | Destroy 4 Bombers | BomberCarpetRow0 | 4 | Pause_Bomber (1.2) | MS: George + Ralph, Gilman, Cal, Marco, Rojo, Rock, Dick, Batty, Jill, Nick, Philbert<br>JS: Leon + Crock, Wally, Snapper, Kingston, Jack, Eyegore<br>CP: Rhett + Kyle, Icky, Venus, Harry, Natalie, Furby, Boosy<br>RA: Lizzie + Ramsey, Amanda |
| HK04_Shrines | Challenge04 | Hit 6 Gongs | HKPagodaGongAdorn_Street, HKPagodaGongAdorn_Roof | 6 | Pause_Gong (1) | MS: Ralph + Ramsey, Rhett, Nick, Eyegore<br>JS: Lizzie + Cal, Snapper, Amanda, Kingston, Jill, Boosy<br>CP: George + Kyle, Rojo, Icky, Rock, Dick, Jack<br>RA: Gilman + Leon, Crock, Marco, Venus, Wally, Harry, Natalie, Furby, Batty, Philbert |
| HK05_Apartments | Challenge05 | Eat 8 Cherries | Powerup_Cherry | 8 | Powerup_Cherry (1) | MS: Cal + Crock, Wally, Rock, Batty, Jill, Nick, Philbert, Eyegore<br>JS: Rhett + Marco, Kingston, Furby, Jack<br>CP: George + Lizzie, Ramsey, Icky, Venus, Amanda, Natalie, Boosy<br>RA: Ralph + Gilman, Leon, Kyle, Rojo, Harry, Snapper, Dick |
| HK07_Wave | Challenge07 | Eat 8 Grenadiers | SoldierGrenadierFromWindow, SoldierGrenadierFromTruck3 | 8 | Pause_Granadier (1) | MS: Lizzie + Kyle, Dick, Batty<br>JS: Ramsey + Gilman, Leon, Marco, Venus, Amanda, Jill, Nick, Eyegore, Boosy<br>CP: Cal + Crock, Rojo, Snapper, Natalie, Rock<br>RA: George + Ralph, Rhett, Icky, Wally, Harry, Kingston, Furby, Jack, Philbert |
| HK08_Upscale | Challenge08 | Eat 12 Business People | HumanMaleBusiness | 12 | Pause_OfficeWorker (1) | MS: George + Rojo, Icky, Harry, Kingston, Dick<br>JS: Gilman + Cal, Venus, Wally, Amanda, Natalie, Batty, Jack<br>CP: Lizzie + Ralph, Rhett, Leon, Marco, Snapper, Furby, Jill, Nick, Philbert, Eyegore<br>RA: Ramsey + Crock, Kyle, Rock, Boosy |
| HK10_Fortress | ChallengeBoss | the boss card (ChallengeBoss*) | BossJumper | 1 | Pause_Leeper (1) | - |
| HK06_Towers (not in BlockList) | Challenge06 | Load this block | StreetLampChina | 2 | - (-) | - |
| HK09_Gardens (not in BlockList) | Challenge09 | Find this location | StreetLampChina | 20 | - (-) | - |

### NewYork

| Block | Identifier | Text (English) | Goal templates | Required | Icon (Scale / PauseScale / ShiftX) | Upgrades (1 CP, 2 MS, 4 RA, 8 JS: Monster + list) |
|---|---|---|---|---|---|---|
| NY01_School | Challenge01 | Eat 6 Street Punks | StreetPunk1Hard, StreetPunk2Hard | 6 | Pause_StreetPunk (1) | MS: Gilman + Rhett, Cal, Dick, Batty, Nick, Eyegore<br>JS: George + Venus, Snapper, Amanda, Jill, Boosy<br>CP: Ralph + Ramsey, Crock, Kyle, Rojo, Rock<br>RA: Lizzie + Leon, Marco, Icky, Wally, Harry, Natalie, Kingston, Furby, Jack, Philbert |
| NY02_Industrial | Challenge02 | Destroy 3 SCUM Freight Cars | TrainNYFlat | 3 | Pause_FreightCar (1.2) | MS: Ramsey + Cal, Rojo, Rock, Dick, Batty, Jill, Nick<br>JS: George + Rhett, Leon, Wally, Snapper, Eyegore, Boosy<br>CP: Lizzie + Ralph, Icky, Venus, Natalie, Furby<br>RA: Gilman + Crock, Kyle, Marco, Harry, Amanda, Kingston, Jack, Philbert |
| NY03_Garment | Challenge03 | Eat 4 Steaks | Powerup_Steak | 4 | Powerup_Steak (1.2) | MS: Lizzie + Rojo, Kingston, Batty, Philbert, Eyegore<br>JS: Ralph + Gilman, Cal, Marco, Amanda, Dick, Jack, Jill, Nick<br>CP: George + Crock, Kyle, Icky, Harry, Snapper, Rock, Boosy<br>RA: Ramsey + Rhett, Leon, Venus, Wally, Natalie, Furby |
| NY04_Greenwich | Challenge04 | Destroy 3 Bombers | BomberCarpetRow0 | 3 | Pause_Bomber (1.2) | MS: Gilman + Rhett, Dick, Nick, Eyegore<br>JS: George + Venus, Snapper, Amanda, Batty, Jill, Boosy<br>CP: Ramsey + Cal, Kyle, Rojo, Natalie, Rock<br>RA: Lizzie + Ralph, Leon, Crock, Marco, Icky, Wally, Harry, Kingston, Furby, Jack, Philbert |
| NY05_Slippery_Soap | Challenge05 | Eat 20 Soldiers | SoldierRifleman0, SoldierRiflemanFromTruck2, SoldierRiflemanFromTruck3, SoldierGrenadierFromTruck3 | 20 | Pause_Soldier (1) | MS: George + Lizzie, Ralph, Cal, Kyle, Icky, Batty<br>JS: Gilman + Crock, Marco, Rojo, Venus, Wally, Kingston, Rock, Dick, Philbert, Eyegore<br>CP: Ramsey + Rhett, Leon, Harry, Natalie, Furby, Jack, Jill, Boosy<br>RA: Snapper + Amanda, Nick |
| NY06_Central_Park_West | Challenge08 | Pop 3 Balloons | ParadeBalloonGeorge, ParadeBalloonLizzie, ParadeBalloonRalph | 3 | Pause_3Balloons (1.25) | MS: Harry + Rock, Dick, Batty, Eyegore<br>JS: Ramsey + Rhett, Cal, Venus, Snapper, Amanda, Jill, Nick, Boosy<br>CP: Kyle + Rojo, Icky<br>RA: George + Lizzie, Ralph, Gilman, Leon, Crock, Marco, Wally, Natalie, Kingston, Furby, Jack, Philbert |
| NY07_Central_Park_East | Challenge07 | Eat 8 Money Bags | Hazard_Moneybag | 8 | moneybag (1) | MS: Ralph + Cal, Marco, Wally, Snapper, Philbert, Boosy<br>JS: Leon + Natalie, Kingston, Furby, Jack<br>CP: George + Lizzie, Rhett, Crock, Venus, Harry, Amanda, Dick, Jill, Nick, Eyegore<br>RA: Ramsey + Gilman, Kyle, Rojo, Icky, Rock, Batty |
| NY08_Broadway | Challenge06 | Destroy 6 Tanks | Tank6sec | 6 | Pause_Tank (1.2) | MS: Lizzie + Gilman, Rhett, Rock, Dick, Nick, Philbert, Eyegore<br>JS: Ramsey + Cal, Snapper, Jack, Jill<br>CP: George + Rojo, Icky, Venus, Natalie, Boosy<br>RA: Ralph + Leon, Crock, Kyle, Marco, Wally, Harry, Amanda, Kingston, Furby, Batty |
| NY09_Times_Square | Challenge09 | Eat 12 Business People | HumanMaleBusiness | 12 | Pause_OfficeWorker (1) | MS: George + Cal, Rojo, Kingston, Jill<br>JS: Lizzie + Ramsey, Rhett, Leon, Marco, Venus, Wally, Amanda, Dick, Nick, Eyegore, Boosy<br>CP: Ralph + Gilman, Crock, Kyle, Snapper, Natalie, Rock, Furby<br>RA: Icky + Harry, Batty, Jack, Philbert |
| NY10_Empire_State | ChallengeBoss | the boss card (ChallengeBoss*) | NYBlimp | 1 | Pause_Blimp (1) | - |

## The load (0x3468F0) [R]
The load runs each time a block becomes current (0x302AF0: the city's first block, every next block, the replay of a
tied King block, the block a Continue restarts). Its arguments are the object and the block's `ChallengeDef` group.
1. +0x130 (goals) = 0 and +0x13C (upgrade) = 0. +0x138 keeps its old value until the next check.
2. Goals are read only in two cases:
   - the play type (0x4D7E44) is 5, the Timed Run, and the current block (+0x8) is the last one (+0x10, BlockEnd);
   - there is a group and the play type is 2, the campaign.

   In every other case (the King modes, the Timed Run's other blocks) there are no goals: ChallengeStatus hides
   itself (−1) and ChallengeText unloads at once.
3. +0x134 = `Name`.
4. Then it walks the group's records from the group's +0xC on. That field is the count of named keys, which the book
   stores first, so the walk covers the list elements.
   - **`Goal|`** (the type compared in any case, all of it), while there are fewer than 4:
     - Required is read from the group's `Required` (an int record), else 0. A goal with Required 0 is left out.
     - Otherwise it becomes a new goal: that Required, 0 collected, and the name records of its list (16 at most).
   - **`Upgrade|`**: needs both `Monster` and `Upgrade`. The candidates are `Monster`, then every name in the list.
     - A candidate matches when its monster index (0x33DAF0, any case) is player 1's monster (GameStatus +0x214).
     - When the `Upgrade` name is also in the moves table, the group's search ends.
     - +0x13C becomes that move's flag, unless the monster's flags already have it. The flags are GameStatus
       +0x234 + 4 × monster, as `getMonsterFlags` reads them.
     - A later group could overwrite an earlier one. The data never names a monster twice.

So the upgrade on offer depends only on player 1's monster (GameStatus +0x214 is player 0's) and on what it has
already learnt. A monster that has its block's move gets a plain "Bonus Challenge".

## What counts (0x346780) [R]
`award(name, amount)`: for each goal, the first of its names equal to `name` (any case) adds `amount` to its
collected count. A name that two goals list counts for both. Nothing else is checked: how the object went (eaten,
destroyed, picked up, popped) does not matter. There are three callers:
- **gCommon.addStats(obj, slot, type)** (0x2C51A0), after the points go to the player (0x2C55B4):
  - `award("Score", points)`, then `award(template name, 1)` when the object's template (obj +0x80, name at +0x4)
    has a name that is not empty;
  - addStats does nothing at all for an object already counted (flag 0x40000000), so each object counts once;
  - every object that scores goes through here: the people eaten, the army destroyed or eaten, the vehicles, the
    edibles, the street and roof objects, the bosses.
- **The building's Scoring** (0x2BA4B0, at 0x2BAA24): `award("Score", the building's points)`.
- **The block won** (0x3255B0, campaign only): `award("Clock", (int) the clock's seconds left)`, after the time
  bonus.

A goal could therefore name `Score` (reach n points) or `Clock` (finish with n seconds left). No block does.

**Which scripts score each goal** [R, Objects1_Props `Script|ScriptName`], and whether the viewer reports them:

| Goal templates | Script | addStats type | Reported by the port |
|---|---|---|---|
| Hazard_*, Powerup_* | Edible.bsf | 3 | edible.c EE_STATS (`tmpl`) |
| FireTruckVehicle | Vehicle.bsf | 2 | vehicle.c VE_STATS (`name`) |
| Rocket | Rocket.bsf | 0 | vehicle.c VE_STATS |
| Tank6sec / 14sec / 17sec | Vehicle_Attack.bsf | 2 | army.c (AE_SCORE has no name, see below) |
| HelicopterMedium, HelicopterHomingMedium, NewsHelicopterLA | Vehicle_Helicopter.bsf | 2 | army.c |
| PoliceCarWithCopMedium | CombatTruck.bsf | 2 | army.c |
| BomberCarpetRow0 | Bomber.bsf | 2 | army.c |
| SoldierGrenadierFromTruck3, SWATFromTruck0, SoldierRiflemanFromTruck2/3 | Swat.bsf | 1 (eaten) | army.c |
| SWATWindow0, SoldierGrenadierFromWindow, SoldierRifleman0 | Military_Window.bsf | 1 | army.c |
| TouristFemale1 …, Mime, Reader, HumanMaleBusiness, RollerbladerMale, StreetPunk1/2Hard, RoyalGuard | Tourist / Mime / StationaryPerson / Person_Window / WalkingAroundPerson / RoyalGuard.bsf | 1 | people.c `people_eat` (main.c) |
| BossSaucer, BossTank, BossHelicopter, BossJumper, NYBlimp | Boss_*.bsf | 2 | boss.c BE_STATS (`tmpl`) |
| SlowCommuterTrain1, SpeedingCommuterTrain1 | ElevatedTrain.bsf | | rail.c (RE_STATS; a speeding train never: 1000 HP) |
| TrainNYFlat | NewYorkTrain.bsf | | rail.c (RE_STATS) |
| CableCarForTurnaround | CableCar.bsf | | rail.c (RE_STATS) |
| ScumFreighter | freighter.bsf | | rail.c (RE_STATS) |
| HotdogCart1 | GroundCart.bsf | | parade.c: only eating counts (PAE_STATS) |
| Biker | Biker.bsf | | parade.c (PAE_STATS, eaten) |
| ParadeBalloonGeorge / Lizzie / Ralph | ParadeBalloon.bsf | | parade.c (PAE_STATS, popped) |
| ChimneyLondon, HKPagodaGongAdorn_Roof | RoofItem.bsf | 0 | streetobj.c (SE_STATS) |
| StreetLamp2, HKPagodaGongAdorn_Street | StreetObject.bsf | 0 | streetobj.c (SE_STATS) |

**The goal marker** [R] (0x30D280, every world object's init): when its template is a goal (0x3466A0), the object
gets the effect named by its `Effect|ChallengeEffect`, from the instance book or else the template. Only
`DefActorProps` sets one, `NPC_Target`, so the actors a challenge wants (people, soldiers) carry a marker.

## Met (0x346860) [R]
- +0x138 = 0.
- Each goal must be met. With Required > 0 that means collected ≥ Required. With Required ≤ 0 it means collected <
  −Required, a goal not to exceed (none in the data).
- The first goal not met returns 0. If all are met, +0x138 = 1 (−1 with no goals) and it returns 1.
- With no goals it returns 1, so a block without a challenge never costs the city its flag.
- It runs when the block is won (below) and every frame in gAIMgr while an upgrade is on offer.

**The HUD's own test** (ChallengeStatus.scr) is different: it uses the summed counts. It fires once when collected
changes and is > 0 and ≥ the summed Required. Then come "CHALLENGE MET!" (green, `v_challenge_complete`), Points /
players into each player's category 5 (GPT_CHALLENGE), and 2 s later the "5,000 PTS" banner. With one goal per
block, as in all the data, the two tests agree. The boss blocks show the boss bar instead: at 0 health
`updateBossHealth` gives the 5,000 (on every such call), and the boss's addStats also meets the `Boss*` goal.

**Can a challenge fail?** Not as such. Nothing stops the counting: there is no timer and no limit, and the
negative-Required form is unused. A challenge not met when the block is won only costs the city's flag 4. The count
starts again from 0 whenever the block is loaded again (a Continue, a King tie).

## The upgrade [R]
1. **The object.** gAIMgr's update (0x3333F0, every world update, from the block's second frame: +0x50 & 2) checks
   three things: the object is not made yet (+0x50 & 4), an upgrade is on offer (+0x13C ≠ 0), and 0x346860 says met.
   Then:
   - +0x50 |= 4;
   - the template `MonsterUpgrade` is made (0x285710 on 0x4D17A0), added to the world, and placed at its (PosX,
     PosY, PosZ) = (0, −5, 5), the middle of the block over the front sidewalk; its onInit runs;
   - +0x58 = its `UpgradeDelay` (1.5; 1 without).

   Every frame +0x58 counts down to 0. The AI manager's load (0x334C40) sets +0x50 = 1 and +0x58 = 0 for each block.
2. **StreetObject.bsf** (the template's script, `IsMonsterUpgrade` 1 → nFlags 2). The object is `InvisShape` with
   the `Powerup_Upgrade` effect on it (`AttachEffect`, CollisionRadius 1). Thrown things pass through it
   (TakeImpact returns). A Monster's TakeDamage on it, while it is not yet taken (nFlags & 4):
   - nug = getChallengeUpgrade();
   - `gAIMgr.aquireUpgrade()` (0x2A62B8). While +0x58 > 0 it answers 0 and the blow does nothing. At 0, GameStatus
     +0x234 + 4 × (player 1's monster) |= +0x13C, +0x13C = 0, and it answers 1;
   - on 1, the script:
     - sets nFlags |= nug << 3 | 4;
     - calls `hideChallengeCompletionUI`;
     - shows the banner TextID `MonsterUpgrade` in TextColor (200, 0, 0), scale 1, with `v_upgrade`;
     - calls setPulseDelay(0.95), moves the object to (0, −50, 0) and stops the effect.
   - 0.95 s later its onPulse shows `createAndStartText2("Upgrade" + nug, "Inst_Upgrade" + nug, green, 1.0, 3.0)`
     ("JUMP SMASH" / "Jump, then Press B0") and deletes it.
3. The move goes to **player 1's monster** whoever hits the object, and it lands in the game status's flags. Those
   are what `getMonsterFlags` answers (the moves the monster can do, MonsterSelect's upgrade icons) and what the
   profile keeps.

## The block's end and the city's flag [R]
- **The flags.** GameStatus +0x2AC (0x4D7C3C) is the city's flags, which LevelResults draws as stars:
  - 1 allBlocks;
  - 2 parScore;
  - 4 challenges;
  - 8 reveal.
- **The start.** A new campaign city (0x33DB90, the start block +0x210 still 0) sets them to **0x84**. Flag 4 starts
  set, and so does 0x80: the "city available" bit of the profile's city flags, which the city's destruction ORs the
  checkpoint's flags into (0x33DE10 at 0x33DF04), so it changes nothing there; flag 4 becomes the challenge star [R].
  A city started at a later block keeps the record's flags as restored or checkpointed, so a challenge missed earlier
  stays missed [R].
- **The checkpoint.** Restoring it (0x33D820 / 0x33D910, a Continue) copies them back from +0x1F4. Saving it at
  each block won (0x33E290) stores them.
- **The block won** (0x3255B0, entering state 7) runs in this order:
  1. the clock stops;
  2. campaign only: the time bonus is shared out, then `award("Clock", seconds left)`;
  3. the Kings are worked out;
  4. **0x346860: when it answers 0, flags &= ~4**;
  5. activateNextBlock (the next block's challenge is loaded);
  6. flag 2 (par) and flag 1 (the city destroyed);
  7. the reveal (flag 8, Boosy's 0x80).

  So flag 4 survives only if every block's challenge was met by the time that block was won. The check runs in
  every play type, but outside the campaign there are no goals, so it answers 1.

## What the port does (src/challenge.c)
- `challenge_load(c, block_def, play_type, last_block, monster, flags)`: 0x3468F0 and the AI manager's reset.
- The getters: `challenge_identifier`, `challenge_icon`, `challenge_icon_scale(c, pause)`, `challenge_icon_shift`,
  `challenge_required`, `challenge_collected` and `challenge_points` (5000). `challenge_hud` fills hud.h's
  HudChallenge with them.
- The counting: `challenge_award` (0x346780), `challenge_add_stats(c, tmpl, points)` (what addStats does) and
  `challenge_is_goal` (0x3466A0).
- The met test: `challenge_check` (0x346860) and `challenge_was_met`.
- The upgrade: `challenge_upgrade_props` (the MonsterUpgrade template) and `challenge_upgrade_tick` (gAIMgr's part:
  CUE_SPAWN, then CUE_BANNER 0.95 s after the take). `challenge_upgrade_hit(c, centre, radius, &flags)` is the blow
  and aquireUpgrade.
- The block's end: `challenge_block_won(c, play_type, seconds_left)` ("Clock", then the check; 0 = clear flag 4).

## Not traced / guessed
- A missing Scale / PauseScale / ShiftX answers 0.0 (above) [R]; the Pause menu uses PauseScale, else Scale, as it
  comes (Pause.scr).
- TextColor (200, 0, 0): getTemplateColorProp (0x278C38 → 0x27B4F0) wants 3 or 4 float records, truncates each to a
  byte (& 0xFF, alpha 255 with three) and gives them back × 1/255 (0x1C97F0) [R]: (0.784, 0, 0, 1).
- The upgrade object is reached by the monster's object test (0x2E22B0): each collision sphere against its shape,
  CollisionRadius 1 about InvisShape's middle (its place), so radius + 1 centre to centre [R]. The object has no
  0x8000, so an unarmed sphere's touch (TakeDamage type 1, 2.0, every frame) takes it too, and StreetObject.scr's
  branch ignores the type. Here each of the monster's collision spheres touches it (main.c `mon_spheres`).
- A new profile's monsters start with their moves locked: 0x33F630 writes 0x80 into each of monsters 0 … 5 [R]
  (PROFILE_NOTES.md); the viewer reads them from the profile, edible.c's 0xF only for a monster the roster lacks.
- The unused blocks HK06_Towers ("Load this block", 2 × StreetLampChina) and HK09_Gardens ("Find this location", 20)
  are test data, and so is CG07_Block8 (not in Chicago's BlockList).
- In the game the next block's challenge is loaded while the results screen is up (state 7). Anything counted from
  then until the next block begins goes to the next block.
