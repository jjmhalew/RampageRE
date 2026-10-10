/* parade.h - the parade balloons, the bikers and their bikes, the food carts (docs/PARADE_NOTES.md).
 *
 * [R] What the game has, and what this is the port of:
 *   - ParadeBalloon.bsf (ParadeBalloonGeorge / Lizzie / Ralph, ObjectInstance|s of New York, London, Los Angeles and
 *     Hong Kong blocks): a balloon drifting on LineMovers about where it was placed, wobbling every frame (Process,
 *     event 3, which the ScriptObject's update 0x309AC0 sends every frame), knocked aside and up by each blow, back to
 *     its place after, popped by its HitPoints-th blow (addStats type 0, 200 points);
 *   - Biker.bsf and Bike.bsf (the Biker ActorInstance|s of San Francisco and Las Vegas): a biker rides a LineMover
 *     from x = -80 to 80 and back at WalkSpeed with a Bike object on its own LineMover beside it, throws a bottle
 *     (StreetPunkBottle) at a monster ahead of it within ThrowingRange, is smashed by a blow (the bike too), can be
 *     grabbed, eaten (addStats type 1) or thrown; the block's spawn chain behind it: SpawnDelay (0x304450 hands such
 *     an instance to gAIMgr, 0x334A20, which makes it later and sends onWalkIn, event 17, 0x334194) and SpawnsItem<n>
 *     (the Actor's destructor 0x30DF80 makes the named ActorInstance| when the actor goes, in world state 6, and sends
 *     it onWalkIn, 0x30E114), whose DelayedSpawn holds the new biker back;
 *   - GroundCart.bsf (HotdogCart1, BurgerCart1, IceCreamStand1, ObjectInstance|s): the food on top (an Edible,
 *     Pickupable object) sits at node 1 of its stand (OTRef|Attachment, a plain object); grabbing it breaks the stand,
 *     eating it counts (addStats type 0) and makes the monster fart 2 s later, throwing or hitting it smashes it away
 *     (Player.addPoints only: no challenge count);
 *   - the natives they lean on, as people.c / vehicle.c have them: the LineMover (0x315E10), smashObject (0x2B2F50),
 *     the SpherePhysicsMover, the hold (0x2DA310, HeldObjectMover 0x310ED0 / 0x3109E0), gCommon.addStats (0x2C51A0)
 *     and addPoints, FollowEffect.createAndStartAttached, add3DSfx, Monsters::doChuckle and timedDoFart.
 *
 * Engine-agnostic, in army.h's conventions: z up, the street at z = 0, the camera at -y; matrices column-major 4 x 4;
 * facings in radians: setFacing (x, y, z) is R = Rz(z) Ry(y) Rx(x), z = 0 faces +y, -pi / 2 (FACE_RIGHT) faces +x.
 * The caller says what the objects see of the monsters (army.h's ArmyMonster) and of the camera, routes the
 * monster's blows, grabs and throws here, and gets back what to draw (objects: a model and its clip at a matrix) and
 * what happened (events). Nothing here draws, plays a sound, starts an effect or scores. */
#ifndef RAMPAGE_PARADE_H
#define RAMPAGE_PARADE_H
#include <stdint.h>
#include "data.h"
#include "army.h"
#include "collide.h"

typedef struct Parade Parade;

/* ---- what the module asks of its host */
typedef struct {
    /* a property group by book and name: "Objects1_Props" for a template; Group.at = 0 when absent */
    Group (*find)(void *ctx, const char *book, const char *name);
    /* optional: a model's bounds in its own frame (a balloon's AutoCollisionBox); 0 = unknown (its ShadowRect in x
     * and y, -3 .. 3 in z, stands in) */
    int (*shape_box)(void *ctx, const char *shape, float min[3], float max[3]);
    /* optional: node `node` of a model's tree at its rest pose, in the model's frame (getNodePosition: the food's
     * place on its stand); 0 = unknown (the stands' node 1 as read from the bundles, else 3.6 up, the script's own) */
    int (*shape_node)(void *ctx, const char *shape, int node, float out[3]);
    /* optional: one play of a look's clip in seconds (its Anim|AnimName, NULL: the shape's own RawAnim); 0 = unknown
     * (the bikers' looks as read from the bundles, else 1 s) */
    float (*clip_seconds)(void *ctx, const char *anim, const char *shape);
    void *ctx;
} ParadeHost;

/* ---- the objects */
enum {
    PAK_BALLOON,      /* ParadeBalloon.bsf */
    PAK_BIKER,        /* Biker.bsf (an actor) */
    PAK_BIKE,         /* Bike.bsf: the biker's Attachment, made for each ride */
    PAK_CART,         /* GroundCart.bsf: the food on top (Hotdog00, Burger, Icecream_Cone) */
    PAK_CART_BASE     /* the cart's stand (its OTRef|Attachment: no script) */
};
enum { PAP_ALIVE, PAP_HELD, PAP_FLYING, PAP_GONE };   /* by its mover: its script's / in a hand / smashed / deleted */

/* object flags (+0x4C) [R]: the names of the table at 0x3F5F20, 0x80000 an actor or an Edible, 0x40000000 counted */
enum { PAF_PUSH_OUT = 0x4000, PAF_EFF_NODES = 0x8000, PAF_PICKUP = 0x10000, PAF_HELD = 0x20000, PAF_EDIBLE = 0x80000,
       PAF_COUNTED = 0x40000000 };

typedef struct {
    const char *name;                                /* its template's name */
    Group tmpl;
    int kind, phase;                                 /* PAK_*, PAP_* */
    int state;                                       /* the script's own: Def.state (balloon, biker, bike) or
                                                      * this.flags (cart: 0, 2 smashed) */
    float pos[3], rot[3];                            /* place; setFacing (x, y, z) */
    float world[16];                                 /* [R(rot) | pos], or the hand's frame while held; always kept */
    /* the look: the model and its clip (NULL: the shape's own RawAnim, which loops), one play's length and where in
     * it (the actor's anim record, ActorAnimShape|<anim>); scale: the template's RenderScale */
    const char *shape, *clip;
    char anim[24];
    float anim_time, anim_len, scale;
    int anim_loop;
    uint32_t oflags;                                 /* PAF_* */
    int partner;                                     /* a biker's bike of this ride / a bike's biker, a cart's stand /
                                                      * a stand's cart; -1 none */
    /* ---- the rest is the module's own */
    Group instance;                                  /* a biker's ActorInstance| (SpawnsItem<n>, DelayedSpawn) */
    int hp, last_blow;
    float osc, osc_scale, start[3];                  /* ParadeBalloon: fOscillate, fOscillateScale, startingPosition */
    float pulse_delay, pulse_time, fly_duration;     /* onPulse; flyingDuration / flyDuration / dyingDuration */
    int has_throw, weapon_on, anim_done;
    float walk_speed, throwing_range;
    struct { float a[3], b[3], t, speed; int on; } line;   /* LineMover */
    struct { float vel[3], spin[3]; int on, landed; } sphere;   /* SpherePhysicsMover (smashObject) */
    CollideMover cm; float cmat[12]; int cm_on;     /* the sphere's mover (collide.c) and its matrix */
    int cobj;                                        /* its object in the frame's collision world, -1 none */
    CollideShape cshape; int cshape_ok;              /* its shape, read once (WSimpleObject's init, 0x28FD70) */
    float blend, blend_left, from_q[4], from_p[3];   /* the hold's blend */
} ParadeObj;

/* ---- what happened */
enum {
    PAE_EFFECT,       /* effect `name` at pos; obj >= 0: started attached (createAndStartAttached), it follows obj
                       * (offset pos - obj's pos) while obj is there, and stays where it is after */
    PAE_SOUND,        /* add3DSfx: 3D sound `name` at pos from obj; level: the script's second argument */
    PAE_MONSTER_SOUND,/* a sound at monster `monster` (Monsters::doChuckle: APE_CHUCKLE_HELIUM) */
    PAE_STATS,        /* gCommon.addStats(obj, monster, level): tmpl / name its template (the challenge counts the
                       * name), amount its Points into category level, v[0] its Health, v[1] its Energy; once per
                       * object (army_add_stats(army, monster, tmpl, level) does all of it) */
    PAE_POINTS,       /* gCommon.addPoints(monster, level, amount): category level only, no challenge count */
    PAE_FART,         /* Monsters::timedDoFart(monster), due now: doFart (Expression_Smile, the Fart effect at
                       * fxNode_Fart, the monster's velocity + 20 up) */
    PAE_WEAPON,       /* obj's Weapon|RifleWeapon on (name its WeaponName, monster its target) or off (name NULL) */
    PAE_REMOVED,      /* obj is gone (requestDelete, or eaten) */
    PAE_IMPACT        /* obj flying met an object of another module (0x287430): that object's TakeImpact is the
                       * caller's (no script here has one of its own). id its collision tag (collide.h), level its
                       * kind, pos where, v the normal, amount obj's Mass (a building: hit with radius Mass / 1000) */
};
typedef struct { int type, obj, monster, level; const char *name; Group tmpl; float pos[3], v[3], amount; int id; } ParadeEvent;

Parade *parade_new(const ParadeHost *host);
void parade_free(Parade *p);
void parade_seed(Parade *p, uint32_t seed);          /* gWorld.frand()'s stand-in */
void parade_clear(Parade *p);                        /* every object gone, nothing waiting */

/* 1 when an object with this Script|ScriptName is this module's (ParadeBalloon.bsf, Biker.bsf, Bike.bsf,
 * GroundCart.bsf): the block's own placement should leave it out */
int parade_script_ours(const char *script);
/* the block's objects (BlockDef| group): its ObjectInstance| balloons and carts, its ActorInstance| bikers (those
 * with SpawnDelay made that many seconds later, those with SpawnedItem = 1 only through a SpawnsItem<n>). The
 * number of objects made or waiting */
int parade_load_block(Parade *p, Group block_def);
/* an object by template name, if its script is ours, at pos turned facing (radians, ZRotation); -1 otherwise */
int parade_spawn(Parade *p, const char *tmpl_name, const float pos[3], float facing);

/* the frame: the monsters as the objects see them, the camera's x (the bikers' VisibleSound) */
void parade_update(Parade *p, float dt, const ArmyMonster *m, int nm, float camera_x);

/* ---- the collision world (collide.c): the frame's world the smashed objects ask (NULL: the street only), and the
 * objects put into it (each one alive, not held, with its shape: tag COLLIDE_TAG(COLLIDE_OWN_PARADE, obj), kind 0,
 * flags 4 and its own: the carts' PushOutMonsters push the monster in collide_monster) */
void parade_set_collide(Parade *p, CollideWorld *w);
void parade_collide_add(Parade *p, CollideWorld *w);

int parade_count(const Parade *p);                   /* objects (skip PAP_GONE) */
const ParadeObj *parade_obj(const Parade *p, int i);
int parade_events(Parade *p, ParadeEvent *out, int max);   /* takes the events since the last call */

/* ---- the monster's side */
/* an armed attack sphere of monster `monster` (all three are OnlyHitWithEffNodes) reaching objects: their
 * TakeDamage. blow: the caller's number for the attack (it changes when an attack is armed again); [G] an object
 * takes one TakeDamage per blow. How many it hit */
int parade_hit(Parade *p, int monster, const float centre[3], float radius, int blow);
/* doGrabObject's search: Pickupable, not held, in front of root, nearest within radius of hand; -1 none */
int parade_nearest_pickup(const Parade *p, const float hand[3], const float root[3], const float forward[3],
                          float radius);
void parade_grab(Parade *p, int obj, int monster, float blend);   /* the hold (0x2DA310) and onPickup */
/* each frame while held: the hand's world matrix (LiftNodeName); mirrored picks MirroredHoldOffset */
void parade_hold(Parade *p, int obj, const float hand_world[16], int mirrored, float dt);
/* throwObject(eat): onThrow(monster, eat); eaten, it is gone; not eaten, it is smashed away. monster_heading: the
 * monster's facing (radians), which smashObject's cone narrows to */
void parade_throw(Parade *p, int obj, int monster, int eat, float monster_heading);
/* hasGrabbedObject's 2 (0x2E5590: flag 0x80000): a biker and a cart's food are eaten, not thrown */
int parade_is_edible(const Parade *p, int obj);

#endif
