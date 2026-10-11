/* streetobj.h - the street and roof objects: StreetObject.bsf and RoofItem.bsf (docs/STREETOBJ_NOTES.md).
 *
 * [R] What the game has, and what this is the port of:
 *   - StreetObject.bsf: 75 templates of Objects1_Props (StreetLamp2, the parked cars, benches, trees, mailboxes,
 *     phone booths, fences, the street gongs HKPagodaGongAdorn_Street, the sampan, the spotlights, ...): a blow
 *     (TakeDamage of type 0) counts it (gCommon.addStats type 0), plays its HitSound and HitEffect, sets its
 *     DamageShape and then deletes it, or tips it over (FallOnHit: 1.4 rad/s away from the monster, its HitEffect
 *     again at 90 degrees, gone DieDuration later), or sets it rocking harder (OscillateWithDamage); the monster's
 *     touch (type 1) sways a tree (IsSwaying), rocks a boat (IsCustomOscillation) or breaks a fence
 *     (IsDamagedOnCollision); a thrown object (TakeImpact) breaks it; AttachEffect (the gong's ring, the spotlights'
 *     beams, the fire bowls) burns on it from AttachEffectDelay on. Its MonsterUpgrade part is challenge.c's.
 *   - RoofItem.bsf: 205 templates (ChimneyLondon, SmokeStack0-3, the roof gongs HKPagodaGongAdorn_Roof, the London,
 *     Las Vegas and Hong Kong roof trims, antennas, fans, the palace's HKPalaceTopAdorn*, the skywalks and signs
 *     that span two buildings): a monster's blow or touch from the roof (within 2 of its height, unless it is
 *     OnlyHitWithEffNodes) knocks it off: its HitEffect at once, FallLatency (0.4 s) later its HitSound and
 *     DamageShape and it falls at 6 a second to the street, turning over, and is deleted there; a chimney
 *     (DeleteOnHit) goes at once in its smoke; it counts (addStats type 0) and soots the monster's mouth and
 *     shoulders (AttachToMonsterMouthEffect); its building starting to come down (onBuildingDestruction) or, for
 *     one across two buildings, either of them, knocks it off too; forceDamage (the boss Leeper landing on the
 *     palace roof, boss.c's BE_FORCE_DAMAGE) does it without a monster (no score).
 *   - the engine around them: the monster's collision with the world's objects (0x2E22B0: TakeDamage(type 0, its
 *     attack's damage) from an armed sphere, once per attack for an OnlyHitWithEffNodes object (the attack's hit
 *     list, 0x2E33A0); every frame type 1 from any other sphere touching an object without that flag; skipped
 *     while held, 0x309A80), the attachment to a building at the block's start (0x2B6580 -> 0x309E20: the
 *     building whose footprint holds the object, 0x2A3D20), onBuildingDestruction (0x309AC0: once, when that
 *     building's collapsing storey G+0x1CC is set), getBuildingHeight (0x309890: that building's top, its box's
 *     max z, else 0), isFalling (0x2ADE10: G+0x1CC set) and findBuildingByPosition (0x2A3D20).
 *
 * Engine-free, in army.h's and vehicle.h's conventions: z up, the street at z = 0, the camera at -y; matrices
 * column-major 4 x 4; facings in radians about z, 0 faces +y (the viewer's heading / 57.2957795). The caller adds the
 * block's objects as the city placed them, gives each frame what they see of the monsters and the buildings, calls
 * the hits, and gets back where each one is now and what happened (events). The scripts run at the game's step
 * (1/60 s). */
#ifndef RAMPAGE_STREETOBJ_H
#define RAMPAGE_STREETOBJ_H
#include <stdint.h>
#include "data.h"
#include "collide.h"
#include "army.h"

typedef struct StreetObjs StreetObjs;

/* ---- what the module asks of its host */
typedef struct {
    /* optional: a model's bounds in its own frame (AutoCollisionBox); nonzero = found (army_shape_box's convention) */
    int (*shape_box)(void *ctx, const char *shape, float min[3], float max[3]);
    int (*shape_sphere)(void *ctx, const char *shape, float centre[3], float *radius);   /* the sphere the model keeps (0x184CB0); nonzero = found */
    void *ctx;
} StreetHost;

/* a monster as the objects see it (the TakeDamage's `who`, getNearestMonster) */
typedef struct { float pos[3]; float facing; } StreetMonster;

/* a building as the objects see it, by the caller's index */
typedef struct {
    float min[3], max[3];                            /* its box in the world (max z: its top now, getBuildingHeight) */
    int falling;                                     /* isFalling: its collapsing storey G+0x1CC is set (a storey
                                                      * coming down, or the sinking: Building.collapse >= 0) */
} StreetBuilding;

/* ---- the objects */
enum { SK_STREET, SK_ROOF };                         /* StreetObject.bsf, RoofItem.bsf */
/* object flags (+0x4C) [R]: the template's Flags names (the table 0x3F5F20), StreetObject's Construct (0x8000 unless
 * OnlyHitWithEffNodes is 0 or IsSwaying), onPickup / onThrow clearing 0x4000 / 0x10000, 0x20000 while held,
 * 0x4000000 attached to a building (0x309E20) */
enum { SF_PUSH_OUT = 0x4000, SF_EFF_NODES = 0x8000, SF_PICKUP = 0x10000, SF_HELD = 0x20000, SF_OBSTACLE = 0x40000,
       SF_ATTACHED = 0x4000000 };

typedef struct {
    const char *name;                                /* template name */
    Group tmpl, inst;                                /* the template, the ObjectInstance| group (its own props) */
    int kind;                                        /* SK_* */
    int alive;                                       /* 0 once deleted (requestDelete, or 4|8|16 from a pulse) */
    int moved;                                       /* it has left where it was placed (fallen, swaying, rocking,
                                                      * deleted): draw it at world, or not at all */
    float world[16];                                 /* where it is now */
    float rest[16];                                  /* where the city placed it */
    const char *shape;                               /* its model now: Shape|Shape, DamageShape once set */
    uint32_t oflags;                                 /* SF_* */
    int building;                                    /* the building it is attached to (the caller's index), -1 */
    float box_min[3], box_max[3];                    /* its collision shape's box in its own frame */
    CollideShape cshape;                             /* its collision shape (WCollisionInfo, 0x28FD70) */
    int cobj;                                        /* its object in the frame's collision world, -1 none */
    /* ---- the rest is the module's own */
    float rot[9];                                    /* world's 3 x 3 */
    float facing[3];                                 /* setFacing's angles (x, y tilt, z its turn) */
    int state, flags, num_hits, counted, attach_fx, hit_fx, hit_played, frame;
    CollideHitList hits;                             /* the attack records that hit it (0x2E33A0) */
    float pulse, pulse_delay;                        /* the next pulse in, the delay last set (0: none) */
    float attach_delay;
    float axis[3], prev_mpos[3], total_angle, sway_time, sway_intensity;   /* StreetObject: the sway, the fall */
    float osc, osc_scale, osc_base;                  /* StreetObject: the custom oscillation */
    ArmyOsc wobble;                                  /* an AttackObjectInstance|'s: the AttackObject's (0x314EF0) */
    float elapsed, move_time, y_per_frame, latency, ydir, rpf[3];   /* RoofItem: the fall */
} StreetObj;

/* ---- what happened */
enum {
    SE_EFFECT,        /* start effect `name` at pos; id its handle (for SE_EFFECT_STOP); obj >= 0 with level 1: it
                       * follows the object (FollowEffect.createAndStartAttached...): each frame at
                       * streetobj_point(obj, off), or at the object's model node `node` (a name, else node_id > 0)
                       * plus off when that is set; level 0: it stays where it started. When the object is deleted
                       * an attached effect stays where it is unless an SE_EFFECT_STOP comes for it */
    SE_EFFECT_STOP,   /* the effect with handle id stops (setTurnOffWhenObjectDies(1), or hitEffect.stop()) */
    SE_SOUND,         /* add3DSfx(name, level) at pos, from obj */
    SE_SHAPE,         /* obj's model is now `name` (setShape: DamageShape; also in obj->shape) */
    SE_STATS,         /* gCommon.addStats(obj, monster, 0): tmpl the template, name its name, amount its Points.
                       * Route it through army_add_stats(army, monster, tmpl, 0) (its AE_SCORE counts the name for the
                       * block's challenge), else challenge_add_stats(challenge, name, amount) */
    SE_MONSTER_EFFECT,/* effect `name` attached to monster `monster`'s node `node` (createAndStartAttachedToNodeByName):
                       * level 0 that node ("fxNode_Belch"); level 1 the series node + "1", "2", ... up to "20", until
                       * a node is missing ("fxNode_Shoulder") */
    SE_REMOVED        /* obj is gone (requestDelete): no longer drawn */
};
typedef struct {
    int type, id, obj, monster, level, node_id;
    const char *name, *node;
    Group tmpl;
    float pos[3], off[3], amount;
} StreetEvent;

StreetObjs *streetobj_new(const StreetHost *host);
void streetobj_free(StreetObjs *s);
/* SK_STREET or SK_ROOF by the template's Script|ScriptName, -1 for any other (and for the MonsterUpgrade, whose
 * StreetObject.bsf part is challenge.c's) */
int streetobj_kind_of(Group tmpl);
/* an object the city placed (its ObjectInstance|: the block's own, or one of its buildings' with building >= 0), at
 * world (its model's base); its Construct runs. attack: the block placed it as an AttackObjectInstance| (an
 * AttackObject, 0x3050F0 -> 0x315330: the trees' and palms' OscillationScale wobble, army.h ArmyOsc). The index, or -1
 * if the template is not ours or the list is full */
int streetobj_add(StreetObjs *s, Group tmpl, Group inst, const float world[16], int building, int attack);
/* the block's start (0x2B6580 -> 0x309E20): each object without a building is attached to the building whose
 * footprint holds it (inclusive, the last such one: 0x2A3D20 with no direction) */
void streetobj_start(StreetObjs *s, const StreetBuilding *b, int nb);

/* the next dt seconds: their pulses (at 1/60 s), onBuildingDestruction from the buildings, the falls */
void streetobj_update(StreetObjs *s, float dt, const StreetMonster *m, int nm, const StreetBuilding *b, int nb);

/* ---- the monster's side (0x2E22B0) */
/* an armed attack sphere of monster `monster` (at who) and its attack record `record` (the arming's number)
 * reaching objects: TakeDamage(who, amount (the attack's damage, +0x2C: 5 a punch, 20 a kick), type 0). At most one
 * TakeDamage an object a frame; an OnlyHitWithEffNodes object once a record. How many it reached */
int streetobj_hit(StreetObjs *s, int monster, const StreetMonster *who, const float centre[3], float radius,
                  float amount, unsigned record);
/* one of the monster's collision spheres, armed or not, touching objects (0x2E22B0: against each one's shape,
 * 0x280470): TakeDamage(who, 2, type 1) for each one without OnlyHitWithEffNodes, unless it took one this frame. How
 * many */
int streetobj_touch(StreetObjs *s, int monster, const StreetMonster *who, const float centre[3], float radius);
/* a thrown object (a car, a person, a unit) reaching objects: StreetObject's TakeImpact (its HitEffect, deleted; no
 * score). RoofItem has none. How many */
int streetobj_impact(StreetObjs *s, const float centre[3], float radius);
/* the same for object i, which a flying object met in the collision world (0x287430): 1 if it took it */
int streetobj_take_impact(StreetObjs *s, int i);
/* the collision world (collide.c): the objects put into the frame's world (each one alive and not held, with its
 * shape: tag COLLIDE_TAG(COLLIDE_OWN_STREET, i), kind 0, flags 4 and its own: PushOutMonsters, IsObstacle) */
void streetobj_collide_add(StreetObjs *s, CollideWorld *w);
/* RoofItem's forceDamage (boss.c's BE_FORCE_DAMAGE: name, pos): the object of that template nearest pos (within 5)
 * is knocked off as by a blow without a monster. Its index, -1 none */
int streetobj_force_damage(StreetObjs *s, const char *name, const float pos[3]);

/* the pick-up of a Pickupable one (the parked cars), for a host that grabs it: onPickup (DamageShape, PickupSound,
 * HitEffect, no longer PushOutMonsters), held (TakeDamage ignored), onThrow (it counts; no longer Pickupable),
 * OnHitGround (HitEffect where it lands, deleted). While held or flying the host moves it (streetobj_place) */
void streetobj_pickup(StreetObjs *s, int i);
void streetobj_throw(StreetObjs *s, int i, int monster);
void streetobj_hit_ground(StreetObjs *s, int i);
void streetobj_place(StreetObjs *s, int i, const float world[16]);

int streetobj_count(const StreetObjs *s);
const StreetObj *streetobj_get(const StreetObjs *s, int i);
/* the object's point off (in its own frame) in the world now */
void streetobj_point(const StreetObjs *s, int i, const float off[3], float out[3]);
int streetobj_events(StreetObjs *s, StreetEvent *out, int max);   /* takes the events since the last call */

#endif
