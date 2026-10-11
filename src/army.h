/* army.h - the army and the city's other attackers, their weapons, the droppables and the score (docs/ARMY_NOTES.md).
 *
 * [R] What the game has, and what this is the port of:
 *   - gAIMgr (0x4D7800): the current block's AIDef { AIInstance| { OTRef|Name, MinDelay, OffsetDelay, RespawnDelay,
 *     MaxOccurances } } read by 0x334C40 / 0x3350F0, spawned by its update 0x3333F0 once the block is activated
 *     (Levels.scr BeginBlockNI: gAIMgr.activate(1)), with the bindings removeAIObject (0x332A50), enableRespawn
 *     (0x3329B0) and the saved object name (0x2A6338 / 0x2A6420);
 *   - the attackers' scripts: Vehicle_Attack.bsf (tanks, with the native AttackMover 0x314B40 / 0x313F80),
 *     Vehicle_Helicopter.bsf, CombatTruck.bsf (army trucks, SWAT vans, police cars), Swat.bsf (the troops and cops
 *     they drop), Military_Window.bsf (soldiers in windows), StationaryAttackObject.bsf, Bomber.bsf and
 *     Droppable.bsf (what falls from a window);
 *   - the weapons (Weapons.bdp, read by 0x297DC0 / 0x296830 / 0x2960D0 / 0x33B860 / 0x33B5F0, the template's own
 *     Weapon group read over the book's, 0x299EC8): RProjectileWeapon (0x44B8F0; fire 0x2963C0, its shot
 *     RProjectile 0x44B9A0: init 0x2944F0, update 0x293120, step 0x294300, impact 0x33BCA0) and RBeamWeapon
 *     (0x44B840; fire 0x295C70, update 0x295C20, the beam's hit 0x295750, impact 0x33B440), their common countdown
 *     (0x2971A0, 0x297760);
 *   - the monster's weapon hit (0x2D9650 -> 0x2D6A50): the slot a ReactionLevel starts;
 *   - gCommon.addStats (0x2C51A0), Player.addPoints (0x33F530) and the buildings' Scoring (0x2BA4B0).
 *
 * Engine-agnostic: nothing here draws, plays a sound or starts an effect. The caller gives each frame what the units
 * see of the monsters and the buildings, and gets back what to draw (units, shots) and what happened (events:
 * effects by name, sounds, the monsters' hits, reactions, knocks, score). Positions are the game's: z up, the street
 * at z = 0, the buildings' fronts towards +y, the camera at -y; matrices column-major 4 x 4; facings in radians about
 * z as the game keeps them (0 faces +y, pi / 2 faces -x, -pi / 2 faces +x). */
#ifndef RAMPAGE_ARMY_H
#define RAMPAGE_ARMY_H
#include <stdint.h>
#include "data.h"
#include "collide.h"

typedef struct Army Army;
typedef struct ArmyUnit ArmyUnit;

/* ---- what the module asks of its host */
typedef struct {
    /* a property group by book and name: book "Objects1_Props" for a template (OTRef), "Weapons" for a weapon
     * (Weapons.bdp). Group.at = 0 when absent */
    Group (*find)(void *ctx, const char *book, const char *name);
    /* the length in seconds of the clip anim (NULL: the shape's own RawAnim) of shape; 0 = unknown */
    float (*clip_seconds)(void *ctx, const char *anim, const char *shape);
    /* optional: node (a weapon's WeaponNode: "Weapon_0", "MuzzleFlash_Small", "Bombs_Here") of the unit as drawn
     * now, in the world; 0 = unknown (the shot then leaves from the unit's place, a stand-in) */
    int (*node_world)(void *ctx, const ArmyUnit *u, const char *node, float out[16]);
    /* optional: a model's bounds in its own frame (AutoCollisionBox); 0 = unknown (4 x 6 x 3 stands in) */
    int (*shape_box)(void *ctx, const char *shape, float min[3], float max[3]);
    int (*shape_sphere)(void *ctx, const char *shape, float centre[3], float *radius);   /* the sphere the model keeps (0x184CB0); nonzero = found */
    /* gWorld.isSphereVisible (id 0x40 -> 0x16C0A0): the sphere inside the camera's frustum; unset, the view's x range */
    int (*visible)(void *ctx, const float p[3], float r);
    /* optional: the effect's collision rod (fx_rod, 0x1F9290): a beam's reach. Its direction in the effect's frame
     * (1 long), speed, length, the emitter's cycle and whether it loops; 0 = none (the beam then meets nothing).
     * Unset: the weapon's Velocity up to its MaxFiringRange stands in */
    int (*rod)(void *ctx, const char *effect, float dir[3], float *speed, float *length, float *cycle, int *loops);
    void *ctx;
} ArmyHost;

/* ---- what the units see of a monster, this frame (index = the monster's slot, 0 .. 3) */
typedef struct {
    float pos[3];                                    /* its place (the root, on the ground) */
    float heading;                                   /* the game's facing z, radians */
    const char *move;                                /* its state name: "Ground..." matters to CombatTruck */
    int mode;                                        /* movement mode (+0xAD8): 0 ground, 1 / 2 air, 4 landing, 5 on a building */
    const float (*spheres)[4]; int nspheres;         /* its collision spheres in the world (centre, radius): what shots hit */
    float box_min[3], box_max[3];                    /* its collision box in the world (0x2E2C50): used without spheres */
    float target[3];                                 /* the weapons' target point (0x2DB030): its `Bip01 Spine1`'s world
                                                      * x and y (0x2E6000), its feet's z + 6 */
} ArmyMonster;

/* a building as the units see it: its box in the world, its grid (columns of 5 along x, storeys of 3 above 6) */
typedef struct { float min[3], max[3]; int width, depth, storeys, gone; } ArmyBox;

/* ---- the units */
enum {                                               /* by the template's Script|ScriptName */
    AK_TANK,                                         /* Vehicle_Attack.bsf */
    AK_HELI,                                         /* Vehicle_Helicopter.bsf */
    AK_TRUCK,                                        /* CombatTruck.bsf: army trucks, SWAT vans, police cars */
    AK_TROOP,                                        /* Swat.bsf: soldiers, SWAT, cops on the street */
    AK_WINDOW,                                       /* Military_Window.bsf: a soldier or cop in a window */
    AK_TURRET,                                       /* StationaryAttackObject.bsf (PirateShip) */
    AK_BOMBER,                                       /* Bomber.bsf */
    AK_DROP,                                         /* Droppable.bsf */
    AK_OTHER                                         /* any other script: plays its DefaultAnim where it was made */
};
enum { AU_ALIVE, AU_HELD, AU_DYING, AU_GONE };

typedef struct {                                     /* one weapon (a Weapon| group, over its Weapons.bdp group) */
    const char *name;                                /* WeaponName */
    int type;                                        /* 0 RProjectileWeapon, 1 RBeamWeapon, 2 other (Shockwave: not fired) */
    const char *node;                                /* WeaponNode */
    int ammo_max, start_ammo, axis, axis_neg, damage_type, reaction;
    int aim_axis;                                    /* an actor's aimed axis (0x30F1F0 +0x20): Direction's letter - 'X', 0 */
    float firing_offset, refire, charge_time, gravity, velocity, range, duration, damage, impact_delay, vis_range,
          mobility, accel, init_vel, dps, radius, h_knock, v_knock;
    const char *fire_fx, *charge_fx, *impact_fx;
    uint32_t def_flags;                              /* +0x4: 2 it counts its ammo */
    /* the weapon (0x296760): +0xB4 the countdown, +0xEC ammo, +0xF5 firing (a beam), +0xF6 charging, +0x1B8 first */
    float timer, beam_left, scale;
    int ammo, on, firing, charging, first;
    float yaw, pitch;                                /* a cluster's turn from the unit's front (+0x54, +0x3C) */
    float rot_range, rot_rate, elev_range, elev_rate;   /* the WeaponCluster's, radians (rates a second) */
    float rot_min, rot_max, elev_min, elev_max;      /* the turn's and the pitch's ends (0x299640) */
    int clear_rot, clear_elev;                       /* RequiresRotation / ElevationClearToFire (flags 2, 1) */
    int recenter;                                    /* ReCenterWhenNoTarget (flag 0x10): back to 0, 0 without one */
    /* the nodes the cluster turns (Rotation / ElevationControlNode, +0x64 / +0x4C): the host draws the shape with
     * rot_node's pose turned by yaw about its own z and elev_node's by pitch about its own x (WWeaponProcAnimator
     * 0x298980), so the WeaponNode below them is the firing frame as it is */
    const char *rot_node, *elev_node;
    int shot;                                        /* a beam's shot id, -1 none */
} ArmyWeapon;

/* the AttackObject's wobble (vtable 0x44B470) [R]: +0x190 OscillationScale, +0x18C its phase (OscillationOffset),
 * +0x180 the last wobble. For the army's units, boss.c's parts and the street objects a block places as
 * AttackObjectInstance| (BOSS_NOTES.md "The AttackObject's oscillation") */
typedef struct { float scale, phase, last[3]; } ArmyOsc;

#define ARMY_WEAPONS 4
struct ArmyUnit {
    CollideHitList hits;                             /* the attack records that hit it (0x2E33A0) */
    const char *name;                                /* template name */
    Group tmpl, inst;                                /* the template, and the AIInstance group it came from (or none) */
    int kind, phase;                                 /* AK_*, AU_* */
    int state, flags;                                /* the script's own (Def.state / nState, Def.flags) */
    float pos[3], facing, tilt[2];                   /* tilt: turns about x and y (a wreck's tumble, a pitch) */
    float world[16];                                 /* column-major; always kept up to date */
    /* the look */
    const char *shape;                               /* the model now: Shape|Shape (DestShape once wrecked); actors: their look's */
    char anim[40];                                   /* actors: the ActorAnimShape| record they play */
    const char *clip;                                /* actors: its Anim|AnimName (NULL: the shape's own RawAnim) */
    float anim_time, anim_len; int anim_loop, anim_done;   /* seconds into the play, one play's length */
    /* the scripts' numbers (army.c says which is which per script) */
    float hp, hp_start, state_time, explode_time, pulse_delay, pulse_time, life, aux[8];
    int ihp, count, sub, side, life_state, bounce;
    float bounce_vel[3], vel[3], spin[3];
    int group_index, is_cop, is_grenadier, troops_left[4], troop_i;
    float reload, voice[4];
    const char *vehicle;                             /* Swat.bsf's vehicleType: the truck that brought it */
    struct {
        float pts[8][3], dist[8]; int n, seg, active, kind; float t, speed, resp2; uint32_t flags;
        float m[8][3], s; int ready, closed;         /* the SplineMover's tangents and its place along the path */
    } mv;
    struct { float vmax, app2, flee2, speed, timer, sign, goal, driven; int state, target, tread; } am;   /* AttackMover */
    int target;                                      /* the monster its weapons aim at, -1 none */
    float lock[3];                                   /* StationaryAttackObject's lTarget */
    ArmyWeapon w[ARMY_WEAPONS]; int nw;
    int weapons_on;
    uint32_t oflags;                                 /* object flags: 0x4000 PushOutMonsters, 0x8000 OnlyHitWithEffNodes, 0x10000 Pickupable, 0x20000 BeingHeld */
    int spawner, counted, friend_of, person;
    /* the SpherePhysicsMover of a flight (mv.kind 2: a wreck, a smashed actor, a thrown vehicle; collide.c) */
    CollideMover cm; float cmat[12]; int cm_on;
    int cobj;                                        /* its object in the frame's collision world, -1 none */
    CollideShape cshape; int cshape_ok;              /* its shape, read once (WSimpleObject's init, 0x28FD70) */
    ArmyOsc osc;                                     /* its wobble (the helicopters' 0.1); scale 0: none */
};

typedef struct {                                     /* a shot in the air, or a beam */
    int id, beam, alive;
    const char *weapon, *effect;                     /* WeaponName, its FireEffect (the shot's look) */
    int owner;                                       /* the unit, -1 (a person's) */
    float pos[3], dir[3], speed, age, length;        /* a beam: from pos along dir, length long */
    float head, tail;                                /* a beam's stream (0x226CC0's +0x30 / +0x34) */
    float R[3][3];                                   /* a beam: the weapon's firing frame (rows x, y, z), its effect's */
    float damage, gravity, duration, mobility, accel, velocity, h_knock, v_knock, vis_range, impact_delay;   /* +0xF0 .. */
    int damage_type, reaction, target;
    uint32_t flags;
    float phase;
    const char *impact_fx;
    uint32_t include, exclude;                       /* +0x134, +0x138: the ray's flag masks (0x2953E0, 0x33B770) */
} ArmyShot;

/* ---- what happened */
enum {
    AE_EFFECT,        /* start effect `name` at pos along dir; id its handle; unit >= 0: it follows that unit (offset
                       * pos - the unit's place); shot >= 0: it is that shot's look and follows the shot */
    AE_EFFECT_STOP,   /* the effect with handle id stops */
    AE_SOUND,         /* `name` at pos from unit; level 0: stop that unit's sound of that name (a tank's treads) */
    AE_HIT,           /* a weapon reached monster `monster`: amount damage, id the DamageType, level the ReactionLevel
                       * (-1: none, plain TakeDamage), v the knock (0x2D9650's), pos where, dir the shot's way, name the
                       * weapon. The monster side (0x2D6A50): see army_reaction_slot */
    AE_REACTION,      /* monster.setReaction(level): slot `level` (columns override as for any slot) */
    AE_PUSH,          /* monster.queuePushRequest(v) */
    AE_STATS_HEALTH,  /* addStats: monster's HP = max(1, HP + amount) within its most, only while HP > 0 */
    AE_STATS_ENERGY,  /* addStats: monster's energy + amount within 0 .. 100 (100: startRage) */
    AE_SCORE,         /* points: monster's category level (0 .. 5) got amount (army_stats has the totals) */
    AE_FLOAT,         /* a floating score number (0x331D50, scale 0.8) of amount at pos; level 1 a building's */
    AE_FART,          /* Monsters::timedDoFart on monster, amount (2) seconds from now */
    AE_REMOVED,       /* unit is gone (requestDelete) */
    AE_IMPACT,        /* unit's flight met an object of another module (0x287430): its TakeImpact (vtable +0x38) is the
                       * caller's to give. id the object's collision tag (collide.h COLLIDE_TAG), level its kind, pos
                       * where, dir the normal, amount the mover's Mass (a building: hit with radius Mass / 1000) */
    AE_WINDOW_CLOSE   /* the window soldier went back in (closeWindow, 0x30C3F0): its window at pos shuts over 0.5 s */
};
typedef struct { int type, id, unit, shot, monster, level; const char *name; float v[3], dir[3], pos[3], amount; } ArmyEvent;

Army *army_new(const ArmyHost *host);
/* the weapons for others (boss.c): one made over a template's Weapon| group, and one frame of it with no owning unit
 * (the shots, beams and hits stay the army's) */
int army_weapon_make(Army *a, ArmyWeapon *w, Group weapon_group);
/* frame: the weapon's firing frame (rows x, y, z in the world), NULL: one made with its beam axis along dir */
void army_weapon_tick(Army *a, ArmyWeapon *w, int may, const float from[3], const float dir[3],
                      const float frame[3][3], int target, float dt);
/* a WeaponCluster (cl) on a weapon of it (army.c's units, boss.c's parts, the fire truck): its turn's ranges, rates,
 * Requires...ClearToFire, ReCenterWhenNoTarget and control nodes (0x299640) */
void army_cluster_read(ArmyWeapon *w, Group cl);
/* one frame of the cluster's turn (0x29ADE0, 0x29BA50): w->yaw / pitch step at the rates towards target (in the world,
 * lifted for the shot's fall from `from`, 0x293050; NULL: no target, back to 0, 0 with ReCenterWhenNoTarget), the turn
 * from the heading of the object's +y row and the pitch corrected for its tilt (0x29B3B8 .. 0x29B7D8), world its
 * column-major matrix; within the ends; whether it is on target (pi / 50, pi / 100) */
int army_cluster_turn(ArmyWeapon *w, const float world[16], const float from[3], const float *target, float dt);
/* [R 0x100F50] row k of F made v (1 long), the other two square to it: row k + 1 (mod 3) from the world's x (its y
 * when v is within 45.6 degrees of x), row k + 2 = row k x row k + 1 made 1 long, then row k + 1 = row k + 2 x row k */
void army_frame_point(float F[3][3], const float v[3], int k);
/* the frame F (rows) as a weapon cluster turns it, for a shape drawn without the cluster's control nodes (the game
 * turns the nodes, 0x298980: the drawn WeaponNode carries it) [G: a stand-in]: about the unit's right by pitch, then
 * about the world's z by yaw, from the unit's front (facing) */
void army_frame_turn(float F[3][3], float facing, float yaw, float pitch);
/* a beam's axis (0x226950): the rod's direction through the firing frame F; with no frame, F is made with the rod's
 * main axis along dir (army_frame_point) and that is the axis. rod NULL: +x */
void army_beam_axis(float F[3][3], int have_frame, const float dir[3], const float *rod, float out[3]);
/* a facing (x, y, z) as a turn: Rz(z) Rx(x) Ry(y) [R 0x101330], into the 3 x 3 of the column-major 4 x 4 w; and
 * back [R 0x101220]: x = asin(w[6]), z = atan2(-w[4], w[5]), y = atan2(-w[2], w[10]) */
void army_facing_turn(const float f[3], float *w);
void army_turn_facing(const float *w, float f[3]);
/* the AttackObject's init of its wobble [R 0x315810 .. 0x3159F0]: OscillationScale the template's float, else (0 or
 * less) the instance book's; OscillationOffset the template's (read twice, the second time only with an instance
 * book, but from the template again: the instance's is never read); below 0 gWorld.frand x 2 pi. Only float records
 * count (the type byte tested for 1); the rest 0 (the constructor 0x315A90) */
void army_osc_init(ArmyOsc *o, Group tmpl, Group inst);
/* each frame after the object's own update (0x314EF0, after 0x309AC0), while the scale is above 0: the phase grows
 * by dt (the world's step, gWorld +0x4), the wobble is (sin 2 phase, cos 3 phase, 0) x the scale, and d the wobble
 * less the last one: what goes onto the object's facing (0 when it has none) */
int army_osc_step(ArmyOsc *o, float dt, float d[3]);
/* d onto the turn of w: its facing (0x101220) + d, made again (0x101330); the place stays */
void army_osc_turn(float *w, const float d[3]);
void army_free(Army *a);
/* the street's x range about the camera's x (getCameraPos, 0x2AF220: Bomber's FlybySound); also what is seen for a
 * host without `visible` (default -45 .. 45) */
void army_set_view(Army *a, float xmin, float xmax);

/* the block's AIDef (its BlockDef| group in <City>_Props; 0x334C40): the spawner's records; their count */
int army_load_block(Army *a, Group block_def);
void army_activate(Army *a, int on);                 /* gAIMgr.activate: the spawner's clocks run from here */
void army_clear(Army *a);                            /* every unit and shot gone (a new block) */

/* a unit made by name (createAttackObject + callOnInit / createActor) at pos facing facing (its script may move it:
 * the tanks, helicopters, trucks and bombers place themselves); -1 if unknown */
int army_spawn(Army *a, const char *tmpl_name, const float pos[3], float facing);
/* a window soldier (a Military_Window.bsf template) the occupant system put in a window at pos */
int army_spawn_window(Army *a, const char *tmpl_name, const float pos[3]);
/* PE_DROP: the block's droppable template falls from a window at from (Droppable.bsf onInit) */
int army_drop(Army *a, const char *tmpl_name, const float from[3], const ArmyMonster *m, int nm, const ArmyBox *b, int nb);
/* PE_WEAPON from people.c: person `who`'s weapon WeaponName on (at its world matrix, at target_monster) or off (NULL) */
void army_person_weapon(Army *a, int who, const char *weapon, const float person_world[16], int target_monster);
void army_person_moved(Army *a, int who, const float person_world[16]);
void army_person_forget(Army *a, int who);           /* the person is gone: its weapon with it */

void army_update(Army *a, float dt, const ArmyMonster *m, int nm, const ArmyBox *b, int nb);

/* ---- the collision world (collide.c): the frame's world the shots' rays and the flights ask (NULL: the street
 * only), and the units put into it (each alive unit not held, with its shape: tag COLLIDE_TAG(COLLIDE_OWN_ARMY,
 * unit), kind 0; flags 4, PushOutMonsters, IsObstacle as its own). The caller adds the monsters' spheres (flag
 * COLLIDE_F_MONSTER, tag COLLIDE_TAG(COLLIDE_OWN_MONSTER, slot)): a ray meeting one hits that monster */
void army_set_collide(Army *a, CollideWorld *w);
void army_collide_add(Army *a, CollideWorld *w);
void army_impact(Army *a, int unit);                 /* TakeImpact: another module's flying object met the unit */

int army_count(const Army *a);                       /* units (some may be AU_GONE: skip those) */
const ArmyUnit *army_unit(const Army *a, int i);
int army_shots(const Army *a);                       /* shots (skip !alive) */
const ArmyShot *army_shot(const Army *a, int i);
int army_events(Army *a, ArmyEvent *out, int max);   /* takes the events since the last call */

/* ---- the monster's side */
/* an armed attack sphere of the attack record `record` (the arming's number, its parts' own) reaching units (their
 * TakeDamage(monster, amount, ...)): how many it hit */
int army_hit(Army *a, int monster, const float centre[3], float radius, float amount, unsigned record);
/* doGrabObject's search (0x2D99C0) over the units: Pickupable, not held, in front of root (forward), nearest within
 * radius of hand; -1 none */
int army_nearest_pickup(const Army *a, const float hand[3], const float root[3], const float forward[3], float radius);
void army_grab(Army *a, int unit, int monster);      /* the hold (0x2DA310): onPickup */
/* each frame in the hand (the HeldObjectMover): the hand's world; mirrored: the monster's clip is (MirroredHoldOffset) */
void army_hold(Army *a, int unit, const float hand_world[16], int mirrored);
/* throwObject(eat, vel): onThrow; an actor eaten counts (addStats 1) and is gone, one not eaten is smashed; a
 * vehicle is thrown with vel */
void army_throw(Army *a, int unit, int monster, int eat, const float vel[3], float monster_heading);

/* [R] 0x2D6A50: the slot an AE_HIT starts. ReactionLevel 4 (pistols, rifles, the pulse cannon) and < 0 start none
 * (-1; the damage still counts). In the air: mode 2 42 / 44 (from behind), mode 1 43 / 45. On the ground or a
 * building (mode 0, 5): by the knock's direction made 1 long, z > 0.707 24 + level (27 + level from behind),
 * z < -0.707 18 + level (21 +), otherwise 0 + level (3 +), and level 2 with a knock of z <= 0.001 71 (72). The
 * column override applies after; the hit is ignored while the monster is not interruptible (+0xAA2), held in a
 * grab, or at 0 HP; the HP falls by the damage after the slot is chosen, and at 0 slot 67 (React_Defeated) */
int army_reaction_slot(int level, const float knock[3], int mode, int behind);
int army_hit_behind(const float monster_pos[3], float monster_heading, const float at[3]);   /* the hit came from behind */

/* ---- the score (gCommon.addStats, Player.addPoints) */
#define ARMY_MONSTERS 4
/* per player (0x4D7CA8 + slot x 0x60): +0x04 points[7], +0x20 kills[7]: 0 destruction, 1 civilian, 2 vehicle,
 * 3 bonus, 4 time bonus, 5 challenge, 6 the total */
typedef struct { float points[7]; int kills[7]; } ArmyStats;
const ArmyStats *army_stats(const Army *a, int monster);
/* addStats(object of template tmpl, monster, type) for what is not a unit here (people.c's eaten people: type 1) */
void army_add_stats(Army *a, int monster, Group tmpl, int type);
/* the buildings' Scoring (DefBuildingProps, 0x2BA4B0) for building index `building` (< 64), hit by monster (-1: no
 * attacker, the chain is reset): event ARMY_SCORE_CELL (a front cell hit, state its state after the hit),
 * ARMY_SCORE_FLOOR (a floor's FloorHP gone), ARMY_SCORE_BUILDING (its BuildingHP gone: front_states the front
 * cells' states). monster_move: the monster's state name (MultiFloor's key); width, storeys: the front's columns and
 * the storeys standing; at: the monster's place (the floating number). Returns the points */
enum { ARMY_SCORE_CELL, ARMY_SCORE_FLOOR, ARMY_SCORE_BUILDING };
float army_score_building(Army *a, int monster, int building, Group building_props, int event, int state,
                          const char *monster_move, int width, int storeys, const int *front_states, int nfront,
                          const float at[3]);

#endif
