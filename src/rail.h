/* rail.h - the rail and water vehicles: Chicago's elevated trains, New York's freight trains, San Francisco's cable
 * cars and Hong Kong's SCUM freighters (docs/RAIL_NOTES.md).
 *
 * [R] What the game has, and what this is the port of (every one a script object, class 0x4D5BD0, run by its
 * template's script):
 *   - ElevatedTrainController.bsf (ElevatedTrainController1, an ObjectInstance of CG03_Block3 and CG06_Block6) and
 *     ElevatedTrain.bsf (SlowCommuterTrain1, SpeedingCommuterTrain1): chains of three cars on the two elevated tracks
 *     (y 3 and 7.5, z 10) over the block's row of RR_Bldg buildings; they brake, derail and burn when a track
 *     building falls, the speeding ones knock a monster off the track, the slow ones are punched or thrown;
 *   - NewYorkTrainController.bsf (NewYorkTrainController1, NY02_Industrial) and NewYorkTrain.bsf (TrainNYEngine and
 *     its TrainNYFlat / TrainNYTanker or TrainNYFreight cars) on the front street (y -12.5);
 *   - CableCarController.bsf (NoTurnaroundCableCarController, TurnaroundCableCarController: SF01, SF02, SF08) and
 *     CableCar.bsf (CableCar, CableCarForTurnaround, which turns round on the turntable);
 *   - freighter.bsf (ScumFreighter, an AIInstance of HK01 and HK02 spawned by gAIMgr 0x3333F0 as army.c's are, with
 *     gAIMgr.removeAIObject 0x332A50);
 *   - Train.bsf (Train1: no block places it), for completeness;
 *   - the natives they use: LineMover (0x3173D0 / 0x315E10, as vehicle.c), SplineMover (vtable 0x44B800, update
 *     0x33ACA0, curve 0x13B780, as boss.c), SphereMover (0x28E440, as vehicle.c), the HeldObjectMover (0x310ED0, as
 *     vehicle.c),
 *     Building.isDestroyed (+0x14C = 0, case 0x2AD780) and isFalling (grid +0x1CC != 0x7FFFFFFF, 0x2CE230),
 *     gWorld.findBuildingByPosition (0x2AEAA0 -> 0x2A3D20: the last building whose footprint holds the point).
 *
 * Engine-agnostic, in army.h's and vehicle.h's conventions: z up, the street at z = 0, the camera at -y; matrices
 * column-major 4 x 4; facings in radians about z, 0 faces +y, pi / 2 faces -x, -pi / 2 faces +x. The caller gives each
 * frame what the objects see of the monsters (army.h's ArmyMonster) and the buildings (RailBox), and gets back what to
 * draw (objects: a model at a matrix) and what happened (events). The score goes through army.c when an Army is bound
 * (rail_bind_army), as vehicle.c's does. */
#ifndef RAMPAGE_RAIL_H
#define RAMPAGE_RAIL_H
#include <stdint.h>
#include "data.h"
#include "army.h"
#include "collide.h"

typedef struct Rail Rail;
typedef struct RailObj RailObj;

/* ---- what the module asks of its host */
typedef struct {
    /* a property group by book and name: "Objects1_Props" for a template; case-insensitive (army.h's find) */
    Group (*find)(void *ctx, const char *book, const char *name);
    /* optional: a model's bounds in its own frame (AutoCollisionBox); 0 = unknown (the shape is then a
     * sphere of radius 1, collide_shape_pick) */
    int (*shape_box)(void *ctx, const char *shape, float min[3], float max[3]);
    int (*shape_sphere)(void *ctx, const char *shape, float centre[3], float *radius);   /* the sphere the model keeps (0x184CB0); nonzero = found */
    void *ctx;
} RailHost;

/* a building as the scripts see it: its box in the world, Building.isDestroyed (+0x14C = 0: BUILDING_GONE, only the
 * rubble left) and Building.isFalling (a storey falling or the whole collapse: building.h's collapse != -1) */
typedef struct { float min[3], max[3]; int destroyed, falling; } RailBox;

/* ---- the objects */
enum {
    RK_ETRAIN_CTRL,   /* ElevatedTrainController.bsf */
    RK_ETRAIN,        /* ElevatedTrain.bsf: SlowCommuterTrain1, SpeedingCommuterTrain1 */
    RK_NYTRAIN_CTRL,  /* NewYorkTrainController.bsf */
    RK_NYTRAIN,       /* NewYorkTrain.bsf: TrainNYEngine, TrainNYFlat, TrainNYTanker, TrainNYFreight */
    RK_CABLE_CTRL,    /* CableCarController.bsf */
    RK_CABLECAR,      /* CableCar.bsf: CableCar, CableCarForTurnaround */
    RK_FREIGHTER,     /* freighter.bsf: ScumFreighter */
    RK_LOOPTRAIN      /* Train.bsf: Train1 */
};
enum { RP_ALIVE, RP_HELD, RP_GONE };

/* object flags (+0x4C, the table at 0x3F5F20) [R] as vehicle.h's. Two more the scripts set by number: 0x10000000
 * (ElevatedTrain) [R 0x2BC1D0]: Building::hit (0x2BC150) does nothing for an attacker so flagged, so the cars never
 * damage a building; 0x2000000 (ElevatedTrain, NewYorkTrain, MerryGoRound; CombatTruck clears it) [G: no code tests
 * it with an immediate (no lui 0x200 mask, no shift by 25 of +0x4C); a mask read from data was not followed] */
enum { RF_PUSH_OUT = 0x4000, RF_EFF_NODES = 0x8000, RF_PICKUP = 0x10000, RF_HELD = 0x20000,
       RF_NO_BUILDING_HIT = 0x10000000 };

struct RailObj {
    const char *name;                                /* template name */
    Group tmpl, inst;                                /* the template; the ObjectInstance / AIInstance it came from */
    int kind, phase;                                 /* RK_*, RP_* */
    float pos[3];
    float facing[3];                                 /* getFacing / setFacing: turns about x, y and z; [2] the heading */
    float rot[9];                                    /* its turn, column-major 3 x 3 */
    float world[16];                                 /* [rot | pos]; always kept up to date */
    const char *shape;                               /* the model to draw now: Shape|Shape, DamageShape once hit */
    int hidden;                                      /* not drawn: the controllers (InvisShape) */
    uint32_t oflags;                                 /* RF_* (and the untraced bits the scripts set) */
    int state, flags;                                /* the script's own Def.state and Def.flags */
    /* ---- the rest is the module's own */
    uint32_t uid;                                    /* isValid: the object a script keeps is still this one */
    int gone_age;                                    /* frames since it went (its slot is reused after 2) */
    float pulse_delay, pulse_timer;                  /* setPulseDelay; the time since its last onPulse */
    int mv;                                          /* the mover: MV_* in rail.c */
    struct { float a[3], b[3], t, speed, resp2; int flags; } line;
    struct { struct { float p[3], s, k, m[3]; } pt[8]; int n, ready, flags; float s, vel; } sp;
    struct { float vel[3], spin[3]; int tumble; } sphere;
    CollideMover cm; float cmat[12]; int cm_on;     /* the sphere's SpherePhysicsMover (collide.c), its matrix */
    int cobj;                                        /* its object in the frame's collision world, -1 none */
    CollideShape cshape; int cshape_ok;              /* its shape, read once (WSimpleObject's init, 0x28FD70) */
    int own_rot;                                     /* the mover keeps rot (a SplineMover's flag 2, a tumble, the hand) */
    int paid, spawner;
    int sfx[6], nsfx;                                /* its 3D sounds (add3DSfx ids), ended with it */
    int fx_off[6], nfx_off;                          /* its effects that setTurnOffWhenObjectDies(1) */
    /* the scripts' numbers (rail.c says which is which per script) */
    int hp, bounce, id, horn_i, sub_state;
    float horn, die, hit_delay, t0, t1, t2, t3;
    int ambient_sfx, stopping_sfx, sparks_fx, ambient_fx, light_fx;
    const char *train_type, *str0, *str1;
    /* objNextTrain; objNextTrains; an ElevatedTrainController's trainsFront / trainsBack, a NewYorkTrainController's
     * curTrain (link[0]) */
    struct { int i; uint32_t uid; } next, link[8], link2[8];
    int nlink, nlink2;
    float path[2][5][3];                             /* CableCar.bsf's splinePath1, splinePath2 */
};

/* ---- what happened */
enum {
    RE_EFFECT,        /* start effect `name` at pos, its +Y along dir (fx_spawn's; the object's forward for an attached
                       * one, up for one on its own); id its handle; obj >= 0: it follows that object (offset
                       * pos - the object's pos) while the object is there, and stays where it is after (unless an
                       * RE_EFFECT_STOP comes) */
    RE_EFFECT_STOP,   /* the effect with handle id stops */
    RE_SOUND,         /* add3DSfx: a 3D sound `name` with handle id that follows obj (sound_add3d, sound_move) */
    RE_SOUND_STOP,    /* remove3DSfx: the sound with handle id ends (also every sound of an object that goes) */
    RE_SHAPE,         /* obj's model is now `name` (also in obj->shape) */
    RE_REACTION,      /* monster.setReaction(level): slot `level` */
    RE_SET_VEL,       /* monster.setVel(v, 0): the monster is thrown with velocity v */
    RE_HP,            /* monster.addHitPoints(amount): HP + amount within 0 .. its most, nothing at 0 HP */
    RE_PUSH,          /* the monster is pushed: pos the place it is asked to go (queuePushRequest(pos)), v = pos - its
                       * place (a freighter's pushMonster [R]; a PushOutMonsters box pushes through the collision
                       * world, collide_monster) */
    RE_STATS,         /* gCommon.addStats(obj, monster, level): name the template, amount its Points, v[0] its Health,
                       * v[1] its Energy; id 1 when an Army was bound and has applied it (its AE_SCORE / AE_STATS_*
                       * events follow from army_events), 0 when the caller must apply it (and count `name` for the
                       * block's challenge) */
    RE_REMOVED,       /* obj is gone (requestDelete) */
    RE_IMPACT         /* obj flying met an object of another module (0x287430): that object's TakeImpact is the
                       * caller's. id its collision tag (collide.h), level its kind, pos where, dir the normal, amount
                       * obj's Mass (a building: hit with radius Mass / 1000) */
};
typedef struct { int type, id, obj, monster, level; const char *name; float v[3], dir[3], pos[3], amount; } RailEvent;

Rail *rail_new(const RailHost *host);
void rail_free(Rail *r);
void rail_bind_army(Rail *r, Army *army);            /* addStats through army_add_stats (NULL: RE_STATS only) */

/* a template's Script|ScriptName is one of ours (no Rail needed: for the block loader's and army.c's hand-over) */
int rail_script_is_ours(const char *script_name);
int rail_kind_of(Rail *r, const char *tmpl_name);    /* RK_* of a template, -1 not ours */
/* an object by template name, if its script is ours, at pos facing facing; inst its ObjectInstance group (the
 * block's, read first by getIntProp / getFloatProp; {0} none). The controllers are placed this way (an ObjectInstance
 * of the block, or of a block behind with LoadWhenWaiting / LoadWhenDistant, at its place in the world), and run
 * their onInit at once. -1 if the template is unknown or not ours */
int rail_spawn(Rail *r, const char *tmpl_name, const float pos[3], float facing, Group inst);
/* gAIMgr's side for our templates (0x334C40 / 0x3350F0, as army_load_block): the block's AIDef AIInstances whose
 * template is ours (ScumFreighter); their spawner runs while active, as army.c's. The number of records */
int rail_load_block(Rail *r, Group block_def);
void rail_activate(Rail *r, int on);                 /* gAIMgr.activate: the spawner's clocks run from here */
void rail_clear(Rail *r);                            /* every object gone, no spawner records */

void rail_update(Rail *r, float dt, const ArmyMonster *m, int nm, const RailBox *b, int nb);

/* ---- the collision world (collide.c): the frame's world the flying objects ask (NULL: the street only), and the
 * objects put into it (each one alive and seen, not a controller, with its shape: tag COLLIDE_TAG(COLLIDE_OWN_RAIL,
 * obj), kind 0, flags 4 and its own). A PushOutMonsters one pushes the monster out in collide_monster (0x2E2C50) */
void rail_set_collide(Rail *r, CollideWorld *w);
void rail_collide_add(Rail *r, CollideWorld *w);

int rail_count(const Rail *r);                       /* objects (skip RP_GONE and hidden) */
const RailObj *rail_obj(const Rail *r, int i);
int rail_events(Rail *r, RailEvent *out, int max);   /* takes the events since the last call */

/* ---- the monster's side */
/* an armed attack sphere reaching objects (against their shapes, 0x280470): their TakeDamage(monster, amount, 0)
 * [R 0x2E22B0: an armed sphere's blow is DamageType 0, Concussive]; how many it reached. (A touch, TakeDamage(2.0, type
 * 1 Collision) every frame, comes from rail_update's own test of the monsters' unarmed spheres with the objects'
 * shapes [R 0x2E22B0]; the PushOutMonsters push is the collision world's, collide_monster.) */
int rail_hit(Rail *r, int monster, const float centre[3], float radius, float amount);
void rail_impact(Rail *r, int obj);                  /* TakeImpact: something flying hit obj */
/* doGrabObject's search over the objects: Pickupable, not held, in front of root, nearest within radius of hand */
int rail_nearest_pickup(const Rail *r, const float hand[3], const float root[3], const float forward[3], float radius);
void rail_grab(Rail *r, int obj, int monster);       /* the hold (0x2DA310) and onPickup */
/* each frame while held: the hand's world matrix; mirrored picks MirroredHoldOffset (HeldObjectMover 0x310ED0) */
void rail_hold(Rail *r, int obj, const float hand_world[16], int mirrored);
/* throwObject(eat, vel) (0x2D9DC0): the object flies with vel on a SpherePhysicsMover, then onThrow(monster, eat) */
void rail_throw(Rail *r, int obj, int monster, int eat, const float vel[3]);

#endif
