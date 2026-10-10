/* edible.h - the edibles: power-ups and hazards behind the windows, and what eating or touching them does to the
 * monster (docs/EDIBLE_NOTES.md).
 *
 * [R] What the game has, and what this is the port of:
 *   - the occupant manager at 0x4D5F00 (set up per block by 0x31A540): its kinds (20-byte records, 0x318B40 from the
 *     block's Edibles { FillRate, EdibleInstance| { Frequency, OTRef|Name } }, a kind is a hazard when its name begins
 *     with "Hazard", 0x32D1B0 "Hazard", 6) and its window slots (0x18 bytes: +0 building, +4 column, +8 floor, +0xC
 *     side, +0xD occupant kind, +0xE edible kind, +0x10 the edible's actor, +0x14 the occupant's actor);
 *   - the edibles' slots, 0x3196F0 from 0x319B78 on: the building's front cells (0x2BE750 -> 0x2D01F0 with flags 6)
 *     shuffled, round(n x FillRate) of them (or the building's NumEdibles) get a kind picked by Frequency; then the
 *     building instance's own EdibleInstance| { cell, floor, Name } (0x318FD0);
 *   - the reveal: a cell broken to state 2 or more (0x2BB580 -> 0x317DC0) creates its edible (0x317BC0), a floor that
 *     falls (0x2BAE70 -> 0x317A70) deletes the edibles of that floor and creates those of the floors above and below;
 *   - the monster's side, the Monster bindings: getBuildingOccupant (0x2B37C0: 2 a power-up, 3 a hazard, only once
 *     the cell is broken), getBuildingOccupantName (0x2B3510), grabBuildingOccupant[AsType] (0x2B3350: an item is not
 *     held, its script's onPickup(monster, eat) runs where it is, 0x30C510), and Monsters.bsf's canGrabPowerup,
 *     checkForHazard, doGrabPowerup, destroyPowerup, canDoPunch with the monsters' own substitutions (George's banana,
 *     Jill's shock, Venus' fire ...);
 *   - Edible.bsf (its effect, rotation mover, onPickup, onAnimComplete, onPulse) and Cryotube.bsf (the monster
 *     reveal's tube);
 *   - gCommon.addStats (0x2C51A0) type 3, the energy and the rage: setEnergy (0x2E5730), startRage (0x2D4360 and
 *     Monsters::startRage), the rage's and the speed scale's clocks (0x2E1CB0, 0x2DE530), setSpeedScale (0x2E8650),
 *     canStartRage / inRageMode, and Monsters.bsf's ability checks (hasRoarEnergy, hasChargePunch, ...).
 *
 * Engine-agnostic: nothing here draws, plays a sound or starts an effect. The caller says which cells broke and which
 * floors fell, asks for the monster's window checks and grabs, and gets back what to draw and what happened (events,
 * as army.h's: effects by name, sounds, reactions by name, the stats, banners). Positions are the game's: z up, the
 * street at z = 0, the buildings' fronts towards -y (their min y), the camera at -y. */
#ifndef RAMPAGE_EDIBLE_H
#define RAMPAGE_EDIBLE_H
#include <stdint.h>
#include "data.h"

typedef struct Edibles Edibles;

/* ---- what the module asks of its host */
typedef struct {
    /* a property group by book and name: book "Objects1_Props" for a template. Group.at = 0 when absent */
    Group (*find)(void *ctx, const char *book, const char *name);
    /* the front cell (building, storey as built, column): its damage state 0 .. 3, -1 if it is gone */
    int (*cell_state)(void *ctx, int building, int storey, int column);
    /* where the edible of that cell appears (0x317BC0; edible_cell_place says how); 0 if the storey has fallen */
    int (*cell_place)(void *ctx, int building, int storey, int column, float out[3]);
    /* optional: the building's place (getPosition) and storeys (getSizeZ), for an ExtraEffectCentered; 0 = unknown
     * (the front's middle, at the street, stands in) */
    int (*building_place)(void *ctx, int building, float pos[3], int *size_z);
    void *ctx;
} EdibleHost;

/* [R] 0x317BC0: an edible stands in its cell, half a cell in from the front: x = the grid's min x + 5 x column + 2.5,
 * y = the front (min y) + 2.5, z = (the building's foot) + 6 + 3 x the storey's place now (0x2CD010) */
void edible_cell_place(const float building_min[3], int column, int live_storey, float out[3]);

/* ---- the kinds and the slots */
typedef struct {
    const char *name;                                /* OTRef|Name (the template) */
    Group tmpl;
    float freq;                                      /* Frequency (0: placed by a building only) */
    int hazard;                                      /* the name begins with "Hazard": getBuildingOccupant 3 */
} EdibleKind;

typedef struct {                                     /* a window that hides an edible */
    int building, storey, column, side;              /* storey: as built (the game's floor id, +0x200); side: +0xC */
    int kind;                                        /* -1: none (eaten, gone) */
    int item;                                        /* its edible once revealed, -1 none */
} EdibleSlot;

/* ---- the edibles in the world */
enum { EP_WINDOW, EP_PICKED, EP_FALLING, EP_GONE };  /* in its window; picked (destroyPowerup: gone in 0.5 s); a
                                                      * cryotube falling out; deleted */
enum { EK_EDIBLE, EK_CRYOTUBE };                      /* Edible.bsf, Cryotube.bsf */
typedef struct {
    const char *name;                                /* its kind's name */
    Group tmpl;                                      /* its template (grabBuildingOccupantAsType replaces it, +0x80) */
    int script, phase, slot;                         /* EK_*, EP_*, its slot (-1 once out of it) */
    float pos[3], spin, pitch;                       /* place; the RotationMover's turn about z; the tube's tumble */
    float world[16];                                 /* column-major; always kept */
    const char *shape;                               /* ActorAnimShape|Default's ShapeName; NULL for InvisShape (draw
                                                      * nothing: the effect is the look) */
    float age, life;                                 /* seconds since it appeared; the Default record's Duration */
    int rotate;                                      /* Rotate */
    int fx;                                          /* its Effect's handle (follows it), -1 none */
    int played_fx, counted;                          /* bPlayedPUEffect; addStats paid (0x40000000) */
    float pulse;                                     /* seconds to its onPulse (< 0: none) */
    struct { float pts[3][3]; int n, seg; float t; } mv;   /* the tube's LineMover */
} Edible;

/* ---- what happened */
enum {
    EE_EFFECT,        /* start effect `name` at pos; id its handle; edible >= 0: it follows that edible (offset pos -
                       * the edible's place; FollowEffect.createAndStartAttachedWithOffset, turned off when it dies) */
    EE_EFFECT_STOP,   /* the effect with handle id stops */
    EE_SOUND,         /* the 3D sound `name` at pos */
    EE_REACTION,      /* monster.playReaction(name): the state of that name ("React_Front_Yum", "React_Side_L_Zap"),
                       * only while its HP > 0 (0x2D9890, 0x2D9710) */
    EE_SPEED,         /* monster.setSpeedScale(amount, v[0] seconds): edible_power_speed */
    EE_STATS,         /* gCommon.addStats(edible, monster, level): tmpl the template, v = (Health, Energy, Points);
                       * level 3 (bonus) for an edible, 0 for the cryotube. Route it: army_add_stats(army, monster,
                       * tmpl, level) for the score and AE_STATS_*, and edible_power_add_stats for HP and energy */
    EE_BANNER,        /* PointsBanner.createAndStart(amount, pos, white, 1.0): a floating score number (ShowBanner) */
    EE_MESSAGE,       /* PointsBanner.createAndStartText(name, colour v, 0.5, sound name2): a line of text across the
                       * screen; name is a localisation key ("InstantRampage", "SpeedIncrease"; "\n" splits it) unless
                       * it begins with '#'; name2 ("v_rampage", "v_speed") the banner's sound, may be NULL */
    EE_MONSTER_FUNCTION, /* monster.callMonsterFunction(name, 0, 0, 0, 0): "<Monster>::name" or "Monsters::name"
                       * ("doFart", "doCatSound") */
    EE_REVEAL,        /* the cryotube landed at pos: gWorld.startReveal(pos), the "MonsterRevealed" banner, sound
                       * "V_Monster_Reveal_Seq" */
    EE_REMOVED        /* edible is gone (requestDelete) */
};
typedef struct {
    int type, id, edible, monster, level;
    const char *name, *name2;
    Group tmpl;
    float pos[3], v[4], amount;
} EdibleEvent;

Edibles *edible_new(const EdibleHost *host);
void edible_free(Edibles *e);
void edible_clear(Edibles *e);                       /* every kind, slot and edible gone (a new block) */

/* 0x31A540 / 0x318B40: the block's (BlockDef) Edibles group: its FillRate and kinds; returns the kinds */
int edible_load_block(Edibles *e, Group block);
/* 0x3196F0's edible part for one building (call it for each, in the block's order, after edible_load_block): its
 * front cells on live storeys 1 .. storeys - 1 (0x2D01F0 from 1, at most 100 up, cells not damaged; with flags 6 a
 * cell needs no window), shuffled, round(n x FillRate) of them (inst's or props' NumEdibles, at most n, overrides)
 * given a kind by Frequency; then inst's EdibleInstance| { cell, floor, Name }. inst: its BuildingInstance| group;
 * props: its template (either may be absent). Returns the slots it made */
int edible_add_building(Edibles *e, int building, Group inst, Group props, int width, int storeys);
/* 0x318FD0 (edible): the edible `name` in the cell (building, storey, column), replacing a kind there; a name not in
 * the block's list becomes a kind of Frequency 0 (the monster reveal's tube: edible_add_cryotube). Returns the slot,
 * -1 */
int edible_add_fixed(Edibles *e, int building, int storey, int column, const char *name);
/* [R] 0x3235F0 -> 0x318FD0(mgr, building, "Edible_Cryotube", floor, cell, 1, 1, 0): the monster reveal's tube in the
 * cell (building, storey = RevealLoc floor, column = RevealLoc cell; reveal.h's reveal_block says where). Its kind,
 * made here when the block's list lacks it, has +8 = 1 like a hazard's: getBuildingOccupant answers 3 for it, and a
 * cell that breaks once the building is being destroyed (+0x14C >= 4) does not let it out. Returns the slot, -1 */
int edible_add_cryotube(Edibles *e, int building, int storey, int column);

int edible_kinds(const Edibles *e);
const EdibleKind *edible_kind(const Edibles *e, int k);
int edible_slots(const Edibles *e);
const EdibleSlot *edible_slot(const Edibles *e, int i);

/* ---- the building's side */
/* 0x317DC0: the front cell (building, storey as built, column) is at state `state` (call once when it reaches 2 or
 * more); building_state the building's (+0x14C, BUILDING_*): from 4 on (destroyed) a hazard stays hidden. Its edible
 * appears; the monster reveal's tube falls out at once */
void edible_cell_broken(Edibles *e, int building, int storey, int column, int state, int building_state);
/* 0x317A70: floor `storey` (as built) of building falls: its edibles are deleted, and those of the floors next to it
 * (above / below: the storeys as built standing on and under it, -1 none) appear */
void edible_floor_fell(Edibles *e, int building, int storey, int above, int below);

/* ---- each frame */
/* the lifetimes (the Default record's Duration, then onAnimComplete deletes it), the pulses (a picked edible goes
 * 0.5 s later), the RotationMover, the tube's fall */
void edible_update(Edibles *e, float dt);
int edible_count(const Edibles *e);                  /* edibles (skip EP_GONE) */
const Edible *edible_get(const Edibles *e, int i);
int edible_events(Edibles *e, EdibleEvent *out, int max);   /* takes the events since the last call */

/* ---- the monster's side: the window checks (getBuildingOccupant's probe finds the cell: people_probe; a person
 * there comes first, the game looks at the slot's occupant before its edible) */
enum { EDIBLE_NONE = 0, EDIBLE_POWERUP = 2, EDIBLE_HAZARD = 3 };
/* [R] 0x2B37C0's item part: 2 or 3 once the cell is broken (state 2 or more) and its edible has appeared, else 0 */
int edible_occupant(const Edibles *e, int building, int storey, int column);
/* [R] 0x2B3510: the edible's kind name there (whatever the cell's state), NULL none */
const char *edible_occupant_name(const Edibles *e, int building, int storey, int column);
/* [R] grabBuildingOccupant(loc, eat) / grabBuildingOccupantAsType(loc, as_type, eat) on an edible (0x2B3350): only
 * once the cell is broken; as_type (NULL: none) replaces its template; then Edible.bsf's onPickup(monster, eat):
 * eat = 0 (destroyPowerup) plays its PickupFx and deletes it 0.5 s later; eat = 1 also plays the monster's reaction
 * ("React_Front_<Reaction>", "React_Side_L_..." when monster_move holds "Side"), its SpeedFactor, addStats, banners,
 * ExtraEffect, MonsterFunction, and it is gone. Returns 1 if there was one */
int edible_grab(Edibles *e, int building, int storey, int column, int monster, const char *as_type, int eat,
                const char *monster_move);
/* the monsters' own checkForHazard / doGrabPowerup (George.bsf, Jill.bsf, ...): the template script `ns` ("Jill")
 * grabs `name` as instead (NULL: as itself) */
const char *edible_substitute(const char *ns, const char *func, const char *name);
/* Monsters.bsf's window calls in one: call is the clip event's or the transition's function ("Monsters::canGrabPowerup",
 * "Jill::checkForHazard", "George::doGrabPowerup", "Monsters::destroyPowerup", "Monsters::canDoPunch"); the cell the
 * probe found (building -1: none) and whether a person is in it (person_there: canGrabPerson's answer). Returns the
 * check's answer (0 / 1) or, for an action, 1 if it took an edible, 0 if not; -1 for any other call. The actions do
 * nothing with a person there (grabBuildingOccupant takes the person: doGrabPerson's business) */
int edible_monster_call(Edibles *e, const char *call, int building, int storey, int column, int person_there,
                        int monster, const char *monster_move);

/* ---- the monster's energy, rage and speed (the Monster's +0xA50 .. +0xA90) */
typedef struct {
    float energy;                                    /* +0xA50: 0 .. 100 */
    int rage;                                        /* +0xA58: inRageMode */
    float rage_time;                                 /* +0xA54: the rage's clock (25 s) */
    float speed, speed_time;                         /* +0xA8C: the clips' rate (1); +0xA90 its clock */
    uint32_t abilities;                              /* gCommon.getMonsterFlags(slot, monster): 1 charge punch, 2 more
                                                      * spins, 4 roar, 8 jump smash */
    uint32_t cheats;                                 /* gCommon.getCheatFlags(): 8 every move */
} EdiblePower;
void edible_power_init(EdiblePower *p);              /* energy 0, no rage, speed 1 */
/* 0x2E5730 setEnergy: 100 or more becomes 100, or 99.9 when another monster (others_full: one is raging or at 100)
 * is */
void edible_power_set_energy(EdiblePower *p, float energy, int others_full);
/* addStats (0x2C51A0) on the monster: hp = max(1, hp + health) within hp_max, only while hp > 0; energy + energy
 * within 0 .. 100, not while raging (unless energy >= 100: the rage starts over). Returns 1 when Monsters::startRage
 * is to run (edible_power_start_rage, then slot 54 "Rampage", the camera's Script shot and the Rage banner) */
int edible_power_add_stats(EdiblePower *p, float *hp, float hp_max, float health, float energy, int others_full);
/* Monsters::startRage: startRage (0x2D4360: rage on, 25 s, speed 1.33 for 25 s) and setEnergy(100). The host then
 * plays slot 54 (setReaction(54)), points its script camera at the monster (target 8 up, from 50 back and 15 up, a
 * cut, 1.5 s, then back over 0.5 s) and loads the "Rage" interface */
void edible_power_start_rage(EdiblePower *p, int others_full);
/* setSpeedScale(scale, seconds) (0x2E8650) */
void edible_power_speed(EdiblePower *p, float scale, float seconds);
/* the monster's update (0x2E1CB0, 0x2DE530): the speed scale's clock (then 1); while raging the clock runs down and
 * the energy with it at 4 per second (12 once the clock is out); the rage ends when both are out, or at 0 HP.
 * Returns 1 the frame the rage ends. The host plays the move graph's clips at speed x their rate (0x2EBFE0) */
int edible_power_update(EdiblePower *p, float dt, float hp);
/* Monsters.bsf's ability checks: hasRoarEnergy, hasRoarAttack, hasChargePunch, hasJumpSmash, canStartRage
 * (others_full: no), inRageMode; -1 for any other call */
int edible_power_answer(const EdiblePower *p, const char *call);
void edible_power_use_roar(EdiblePower *p);         /* useRoarEnergy: 15 off, not while raging */
/* hitBuildingFront's energy (Monsters.bsf): a hit that landed with energy_gain (kinds 0 .. 2 and 6) gives 2, not while
 * raging; in the air it stops at 99. Returns 1 when the rage is to start (Monsters::startRage). While raging the
 * script hits the building twice (hitBuildingPt before and in the test) */
int edible_power_hit_building(EdiblePower *p, int energy_gain, int airborne, int others_full);

#endif
