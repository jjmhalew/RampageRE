/* vehicle.h - the street traffic, the fire truck and the rockets (docs/VEHICLE_NOTES.md).
 *
 * [R] What the game has, and what this is the port of:
 *   - the traffic of gAIMgr (0x4D7800): each loaded block's TrafficDef read by 0x334240 into a 0x28-byte record per
 *     block slot, the first update's fill and the two lanes' release timers (0x3333F0), the release itself
 *     (0x332D90: a type by Frequency x the room left under min(MaxCount, 5), a LineMover at 15 along a lane), and
 *     the script's gAIMgr.removeVehicle (0x332BA0);
 *   - the LineMover (init 0x3173D0 / 0x317650 with a starting share, update 0x315E10: callback 2 at the end, 4 while
 *     a monster is nearer than MonsterResponseDist);
 *   - Vehicle.bsf: every car, bus, truck, the Humvee, the police cars, the bicycle (Edible), the Boulder a monster
 *     tears out of the street (Monsters.bsf grabBoulder), and Traffic.bsf's HiddenVehicle (the police loudspeaker);
 *   - Vehicle_FireTruck.bsf (the template FireTruck, an AttackObject with the beam FireEngineStream; no block of the
 *     shipped cities spawns it) - the street FireTruckVehicle is a plain Vehicle.bsf car;
 *   - Rocket.bsf and RocketBase.bsf (the Hong Kong roof rockets, with their RocketBase and RocketPipe).
 * Vehicle_Attack.bsf (tanks) and Vehicle_Helicopter.bsf (army and news helicopters) are army.c's (AK_TANK,
 * AK_HELI): not here.
 *
 * Engine-agnostic, in army.h's conventions: z up, the street at z = 0, the camera at -y; matrices column-major
 * 4 x 4; facings in radians about z, 0 faces +y, pi / 2 faces -x, -pi / 2 faces +x. The caller gives each frame what
 * the objects see of the monsters and the buildings (army.h's ArmyMonster and ArmyBox), and gets back what to draw
 * (objects: a model at a matrix) and what happened (events). The score goes through army.c when an Army is bound
 * (vehicle_bind_army): its events then carry the gains, as for the army's own units. */
#ifndef RAMPAGE_VEHICLE_H
#define RAMPAGE_VEHICLE_H
#include <stdint.h>
#include "data.h"
#include "army.h"
#include "collide.h"

typedef struct Vehicles Vehicles;
typedef struct VehObj VehObj;

/* ---- what the module asks of its host */
typedef struct {
    /* a property group by book and name: "Objects1_Props" for a template, "Weapons" for a weapon (Weapons.bdp);
     * case-insensitive as the game's template table (TrafficDef names "Citybus" for the template "citybus") */
    Group (*find)(void *ctx, const char *book, const char *name);
    /* optional: a model's bounds in its own frame (AutoCollisionBox); 0 = unknown (the ShadowRect stands in) */
    int (*shape_box)(void *ctx, const char *shape, float min[3], float max[3]);
    int (*shape_sphere)(void *ctx, const char *shape, float centre[3], float *radius);   /* the sphere the model keeps (0x184CB0); nonzero = found */
    /* optional: node of the object as drawn now, in the world (the fire truck's WeaponNode_Y); 0 = unknown */
    int (*node_world)(void *ctx, const VehObj *o, const char *node, float out[16]);
    /* optional: the effect's collision rod (fx_rod, 0x1F9290): a beam's reach. Its speed, length, the emitter's
     * cycle and whether it loops; 0 = none (the beam then meets nothing). Unset: the weapon's Velocity up to its
     * MaxFiringRange stands in */
    int (*rod)(void *ctx, const char *effect, float *speed, float *length, float *cycle, int *loops);
    void *ctx;
} VehicleHost;

/* ---- the objects */
enum {
    VK_CAR,           /* Vehicle.bsf: traffic, the Boulder, the HiddenVehicle */
    VK_FIRETRUCK,     /* Vehicle_FireTruck.bsf */
    VK_ROCKET,        /* Rocket.bsf */
    VK_ROCKET_PART    /* RocketBase.bsf: a rocket's Base or BasePipe */
};
enum { VP_ALIVE, VP_HELD, VP_GONE };

/* object flags (+0x4C, names from the table at 0x3F5F20) [R]; 0x80000 and 0x8000000 are not in that table: Vehicle.bsf
 * sets them for an Edible and an Animates template (no vehicle book sets Animates), their engine side not traced */
enum { VF_PUSH_OUT = 0x4000, VF_EFF_NODES = 0x8000, VF_PICKUP = 0x10000, VF_HELD = 0x20000, VF_EDIBLE = 0x80000,
       VF_ANIMATES = 0x8000000 };
/* Vehicle.bsf's this.flags [R]: 8 wrecked (explodes on its next pulse), 16 picked up, 32 thrown (or knocked away),
 * 64 touched the street since the last pulse, 128 a whole pulse in the air, 1024 hidden; 4056 an edible smashed */
enum { CF_WRECK = 8, CF_PICKED = 16, CF_THROWN = 32, CF_GROUNDED = 64, CF_AIRBORNE = 128, CF_HIDDEN = 1024,
       CF_SMASHED = 4056 };

struct VehObj {
    const char *name;                                /* template name */
    Group tmpl;
    int kind, phase;                                 /* VK_*, VP_* */
    float pos[3], facing;
    float rot[9];                                    /* its turn, column-major 3 x 3 */
    float world[16];                                 /* [rot | pos]; always kept up to date */
    const char *shape;                               /* the model to draw now: Shape|Shape, DamageShape once hit */
    int hidden;                                      /* not drawn (IsHidden: the HiddenVehicle's InvisShape) */
    uint32_t oflags;                                 /* VF_* */
    int flags;                                       /* Vehicle.bsf: CF_*; Rocket: 0 waiting, 2 shaking, 1 flying;
                                                      * RocketBase: 0 standing, 1 falling */
    int slot;                                        /* traffic: the block slot it was released in, -1 none */
    int building;                                    /* a rocket: the building it stands on (the caller's index), -1 */
    int parent, child[2];                            /* a rocket's parts and back, -1 none */
    /* beam: the fire truck's stream while it runs, from along dir, length long (its look is the FireEffect event) */
    struct { int on; float from[3], dir[3], length; } beam;
    /* ---- the rest is the module's own */
    float pulse_delay, pulse_timer;
    float state_time, fly_duration;
    int horn, bounce_count, paid, in_list, ambient_fx, beam_fx, gone_age;
    struct { float a[3], b[3], t, speed, resp2; int on; } line;   /* LineMover: from a to b, the share t */
    struct { float vel[3], spin[3]; int on, tumble, rest; } sphere;   /* SphereMover / smash */
    CollideMover cm; float cmat[12]; int cm_on;     /* the sphere's SpherePhysicsMover (collide.c), its matrix */
    int cobj;                                        /* its object in the frame's collision world, -1 none */
    CollideShape cshape; int cshape_ok;              /* its shape, read once (WSimpleObject's init, 0x28FD70) */
    float shake_step, shake_time, cur[3];            /* Rocket.bsf */
    float elapsed, rpf[3], xpf, ypf;                 /* RocketBase.bsf */
    /* Vehicle_FireTruck.bsf */
    int ft_building, ft_stopped, ft_has_target;
    float ft_target[3], ft_fire_left, ft_velocity, ft_target_dist, ft_yaw, ft_pitch;
    struct {
        const char *name, *node, *fire_fx, *impact_fx;
        int ammo, counts, reaction, on, firing, first;
        float timer, charge, offset, refire, duration, damage, dps, velocity, range, h_knock, v_knock, left, age;
        float rot_rate, elev_rate, elev_min, elev_max;
    } w;
};

/* ---- what happened */
enum {
    VE_EFFECT,        /* start effect `name` at pos along dir; id its handle; obj >= 0: it follows that object (offset
                       * pos - the object's pos) while the object is there, and stays where it is after (unless a
                       * VE_EFFECT_STOP comes) */
    VE_EFFECT_STOP,   /* the effect with handle id stops */
    VE_SOUND,         /* 3D sound `name` at pos from obj (add3DSfx); level: the script's second argument */
    VE_SHAPE,         /* obj's model is now `name` (also in obj->shape) */
    VE_HIT,           /* as army.h's AE_HIT: the fire truck's beam reached monster `monster`: amount damage, level the
                       * ReactionLevel, v the knock, pos where, dir the beam's way; route it through army_reaction_slot */
    VE_REACTION,      /* monster.setReaction(level): slot `level` */
    VE_SET_VEL,       /* monster.setVel(v, 0): the monster is thrown with velocity v */
    VE_HP,            /* monster.addHitPoints(amount): HP + amount within 0 .. its most, nothing at 0 HP (0x2DA530) */
    VE_STATS,         /* gCommon.addStats(obj, monster, level): name the template, amount its Points, v[0] its Health,
                       * v[1] its Energy; id 1 when an Army was bound and has applied it (its AE_SCORE /
                       * AE_STATS_* events follow from army_events), 0 when the caller must apply it */
    VE_FART,          /* Monsters::timedDoFart on monster, amount (2) seconds from now */
    VE_REMOVED,       /* obj is gone (requestDelete, or eaten) */
    VE_IMPACT         /* obj flying met an object of another module (0x287430): that object's TakeImpact is the
                       * caller's. id its collision tag (collide.h), level its kind, pos where, dir the normal, amount
                       * obj's Mass (a building: hit with radius Mass / 1000) */
};
typedef struct { int type, id, obj, monster, level; const char *name; float v[3], dir[3], pos[3], amount; } VehicleEvent;

Vehicles *vehicle_new(const VehicleHost *host);
void vehicle_free(Vehicles *v);
void vehicle_bind_army(Vehicles *v, Army *army);     /* addStats through army_add_stats (NULL: VE_STATS only) */

/* ---- the traffic (gAIMgr) */
/* block slot `slot` (0 the current block, 1 and 2 the ones behind it, at y = 41 x slot) gets the TrafficDef of
 * block_def (its BlockDef| group; 0x334240); the number of vehicle types read */
int vehicle_load_traffic(Vehicles *v, int slot, Group block_def);
void vehicle_clear(Vehicles *v);                     /* every object gone, every slot empty, the first fill again */
/* Traffic.bsf onInit (once a level): the HiddenVehicle, the police loudspeaker that drives the street unseen */
int vehicle_level_start(Vehicles *v);

/* an object by template name, if its script is one of ours (Vehicle.bsf, Vehicle_FireTruck.bsf, Rocket.bsf,
 * RocketBase.bsf), at pos facing facing (the fire truck places itself). A rocket stands on building `building` (the
 * caller's index, -1 none) and makes its Base and BasePipe. -1 if the template is unknown or not ours */
int vehicle_spawn(Vehicles *v, const char *tmpl_name, const float pos[3], float facing, int building);
int vehicle_kind_of(Vehicles *v, const char *tmpl_name);   /* VK_* of a template, -1 not ours */
/* the building the caller numbers `building` fell (onBuildingDestruction): its rockets fly off */
void vehicle_building_gone(Vehicles *v, int building);

void vehicle_update(Vehicles *v, float dt, const ArmyMonster *m, int nm, const ArmyBox *b, int nb);

/* ---- the collision world (collide.c): the frame's world the flying objects ask (NULL: the street only), and the
 * objects put into it (each one alive with its shape: tag COLLIDE_TAG(COLLIDE_OWN_VEHICLE, obj), kind 0, flags 4 and
 * its own: PushOutMonsters for the traffic) */
void vehicle_set_collide(Vehicles *v, CollideWorld *w);
void vehicle_collide_add(Vehicles *v, CollideWorld *w);

int vehicle_count(const Vehicles *v);                /* objects (skip VP_GONE) */
const VehObj *vehicle_obj(const Vehicles *v, int i);
int vehicle_events(Vehicles *v, VehicleEvent *out, int max);   /* takes the events since the last call */

/* ---- the monster's side */
/* an armed attack sphere (only those: the vehicles are OnlyHitWithEffNodes) reaching objects: their
 * TakeDamage(monster, amount); how many it hit */
int vehicle_hit(Vehicles *v, int monster, const float centre[3], float radius, float amount);
/* TakeImpact: something flying hit obj (it is wrecked and explodes on its next pulse) */
void vehicle_impact(Vehicles *v, int obj);
/* doGrabObject's search over the objects: Pickupable, not held, in front of root, nearest within radius of hand */
int vehicle_nearest_pickup(const Vehicles *v, const float hand[3], const float root[3], const float forward[3],
                           float radius);
void vehicle_grab(Vehicles *v, int obj, int monster);   /* the hold (0x2DA310) and onPickup */
/* each frame while held: the hand's world matrix; mirrored picks MirroredHoldOffset (HeldObjectMover 0x310ED0) */
void vehicle_hold(Vehicles *v, int obj, const float hand_world[16], int mirrored);
/* throwObject(eat, vel): onThrow; an Edible eaten counts (addStats 1) and is gone, an Edible not eaten is smashed
 * away, anything else flies with vel */
void vehicle_throw(Vehicles *v, int obj, int monster, int eat, const float vel[3], float monster_heading);

#endif
