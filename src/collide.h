/* collide.h - the game's collision world (docs/COLLIDE_NOTES.md): gWorld's collision, the RGridMgr at 0x4D0900
 * (gp - 0x6D60, a SpatialDataMgr with the block's two building rows) with the RSceneryMgr at 0x4D0B00 (gp - 0x6D5C,
 * the street: the plane z = 0), their queries (the ray 0x283730, the sphere 0x2835B0 and box 0x282FB0 queries), the
 * shapes (WCollisionInfo: a sphere or a box in its object's frame, 0x280470 / 0x2801A0 / 0x27FFF0 / 0x27F620), the
 * SpherePhysicsMover (0x28E440: sub-steps, the street, the impulse 0x28EB30, the objects it meets: TakeImpact both
 * ways, 0x287430) and the monster's own response (0x2E2C50: PushOutMonsters, the VolumeTriggers, the building rows
 * 0x2A35A0, the roof 0x2A49C0, the building below 0x2D6150) and the building searches (0x2A3D20, hitBuildingPt's
 * 0x2D54F0).
 *
 * Engine-free: the viewer fills a CollideWorld each frame (collide_begin, then collide_add_building for the block's
 * buildings and collide_add for the objects with a shape, then collide_finish) and asks it. Matrices are the game's
 * 3 x 4 (the object's +0x8 .. +0x34): m[0..2] its x axis, m[3..5] y, m[6..8] z, m[9..11] its position; a point l in
 * the object's frame is at l.x x + l.y y + l.z z + position. */
#ifndef RAMPAGE_COLLIDE_H
#define RAMPAGE_COLLIDE_H
#include <stdint.h>

/* object flags (+0x4C) the collision world looks at (the names: the table 0x3F5F70; PEOPLE_NOTES.md) */
#define COLLIDE_F_RAYSKIP   0x8u                     /* set on a weapon's owner while its own shot is cast [R 0x295750] */
#define COLLIDE_F_MONSTER   0x40u                    /* a monster: the monster's box query leaves them out */
#define COLLIDE_F_BUILDING  0x2000u                  /* IsBuilding */
#define COLLIDE_F_PUSHOUT   0x4000u                  /* PushOutMonsters */
#define COLLIDE_F_OBSTACLE  0x40000u                 /* IsObstacle */
#define COLLIDE_F_TRIGGER   0x400000u                /* a VolumeTrigger (0x3360F8: always, | PushOutMonsters with DoPushout) */

enum { COLLIDE_SPHERE = 0, COLLIDE_BOX = 1 };        /* WCollisionInfo +4 */

typedef struct {                                     /* WCollisionInfo (vtable 0x449000), in its object's frame */
    int type;
    float centre[3];                                 /* +0xC */
    float radius;                                    /* +0x18: a sphere's; a box's bounding sphere, |half| */
    float half[3];                                   /* +0x1C: a box's half sizes (a sphere: r, r, r) */
    float raw[6];                                    /* +0x28: as set (a sphere: centre, r^2; a box: min, max) */
} CollideShape;

void collide_shape_sphere(CollideShape *s, const float centre[3], float r);   /* setSphere 0x281110 */
void collide_shape_box(CollideShape *s, const float min[3], const float max[3]);   /* setBox 0x281170 */
/* WSimpleObject's shape (0x28FD70): the template's book, then the instance's, each one's CollisionBox (min, max),
 * else its CollisionRadius (about the model's bounding sphere's centre), else AutoCollisionBox and the model's box;
 * none: the model's bounding sphere (no model: radius 1 at the origin). Pass NULL / a negative radius for what a
 * book does not set; auto_box is the model's box (NULL: no model). [R] */
void collide_shape_pick(CollideShape *s, const float bound_c[3], float bound_r,
                        const float *tmpl_box, float tmpl_radius, int tmpl_auto,
                        const float *inst_box, float inst_radius, int inst_auto, const float *auto_box);
/* the SpherePhysicsMover's radius (0x28DE70: sqrt of the shape's +0x34): a sphere's radius; for a box sqrt(max.x),
 * as the game reads it [R] */
float collide_mover_radius(const CollideShape *s);

typedef struct {
    CollideShape shape;
    float m[12];
    uint32_t flags;                                  /* +0x4C */
    int tag, kind;                                   /* the viewer's: which module and which of its objects */
    int building;                                    /* a building's own box: its index in CollideWorld.b, else -1 */
    struct CollideMover *mv;                         /* +0x84: its SpherePhysicsMover while it flies, else NULL */
    float *mvm, *mvel;                               /* that mover's matrix, and where its module keeps the velocity */
} CollideObj;

typedef struct {
    float min[3], max[3];                            /* the grid's box in the world (+0x1DC .. +0x1F0): max z drops as it falls */
    int row;                                         /* +0x10C: 0 the front row (its fronts at y 0), 1 the back row */
    int state;                                       /* +0x14C: building.h's BUILDING_* (0 gone) */
    int falling;                                     /* a storey or the whole falling (0x2CE230 != 0x7FFFFFFF) */
    int tag;
    int obj;                                         /* its box in CollideWorld.o */
    int nb[4];                                       /* the roof's neighbours (0x2A4C70): left, right, front, back;
                                                      * 1 flush and as high, 0 flush and higher, 2 none */
} CollideBuilding;

#define COLLIDE_MAX_OBJ 768
#define COLLIDE_MAX_BUILDINGS 32
#define COLLIDE_ROW_MAX 16

typedef struct {
    CollideObj o[COLLIDE_MAX_OBJ]; int no;
    CollideBuilding b[COLLIDE_MAX_BUILDINGS]; int nb;
    int row[2][COLLIDE_ROW_MAX], nrow[2];            /* +0x10 / +0x18: each row by its x (0x2A51B0) */
    float back_row;                                  /* BackRowOffset (0x4D502C): the back row's fronts' y */
    int street_open;                                 /* [G] the street's y limits off (the block's end: main.c) */
} CollideWorld;

/* each frame: empty it; back_row is the block's BackRowOffset (10 when the block does not say; < 0: taken from the
 * back row's buildings' fronts at collide_finish) */
void collide_begin(CollideWorld *w, float back_row);
/* a building of the block (the grid list +0x5C, the rows +0x18); its box goes in as an object too (Building is a
 * WorldObject with its grid's box as a WCollisionInfo, 0x2BC920 / 0x2BDC70), flags 4 | IsBuilding. Returns its index */
int collide_add_building(CollideWorld *w, const float min[3], const float max[3], int row, int state, int falling, int tag);
/* an object with a shape (in the world list gp - 0x6CFC +0x58); returns its index, -1 full */
int collide_add(CollideWorld *w, const CollideShape *s, const float m[12], uint32_t flags, int tag, int kind);
void collide_finish(CollideWorld *w);                /* the rows sorted, the roofs' neighbours (0x2A4C70) */

/* helpers for the matrices: a heading (degrees about z, main.c's: x axis (cos, sin, 0)) and a place */
void collide_matrix(float m[12], float heading_deg, const float pos[3]);

/* ---- one shape (in the world, the shape in its object's frame) */
/* 0x2801A0: the box centre +- ext (world-aligned, taken as aligned with the object's axes) against the shape; push
 * moves the box out (the smallest overlap's axis), normal and point in the world. 1 if they meet */
int collide_obj_box(const CollideObj *o, const float centre[3], const float ext[3], float normal[3], float point[3],
                    float push[3]);
/* 0x280470: the sphere against the shape; push moves the sphere out */
int collide_obj_sphere(const CollideObj *o, const float c[3], float r, float normal[3], float point[3], float push[3]);
/* 0x27FFF0: the ray start + dir t, 0 <= t <= maxt, against the shape: t and the surface's normal */
int collide_obj_ray(const CollideObj *o, const float start[3], const float dir[3], float maxt, float *t, float normal[3]);
/* 0x27F620: object a against object b (a box against a box never meets, 0x27FF18 -> 0x27FFC0 [R]); normal pushes a
 * away from b, point on b, depth how far in. 1 if they meet */
int collide_pair(const CollideObj *a, const CollideObj *b, float normal[3], float point[3], float *depth);

/* ---- the world's queries */
typedef struct { float t, normal[3], point[3]; int obj; } CollideHit;   /* obj -1: the street */
/* the ray (vtable +0x18, 0x283730): the street first (RSceneryMgr 0x282460: going down, from above z 0), then every
 * object with (flags & include) and none of exclude; the nearest, or with any the first found. Shots pass
 * include -1 (or the weapon's +0x134), exclude the weapon's +0x138 | COLLIDE_F_RAYSKIP with the owner flagged */
int collide_ray(const CollideWorld *w, const float start[3], const float dir[3], float maxt, uint32_t include,
                uint32_t exclude, int any, CollideHit *hit);
/* vtable +0x20 (0x2835B0): the objects whose bounding spheres reach the sphere */
int collide_sphere_query(const CollideWorld *w, const float c[3], float r, uint32_t include, uint32_t exclude,
                         int *out, int nmax);
/* vtable +0x30 / +0x34 (0x282FB0 / 0x282D70): the objects whose turned boxes overlap the box */
int collide_box_query(const CollideWorld *w, const float min[3], const float max[3], uint32_t include, uint32_t exclude,
                      int *out, int nmax);

/* ---- the buildings */
/* 0x2A3D20: dx = dy = 0: the (last) building whose footprint holds p, any state; else the nearest one that way from
 * the building at p (or from p), across p's row (dy = 0) or column (dx = 0). -1 none; *start the one at p */
int collide_building_at(const CollideWorld *w, const float p[3], float dx, float dy, int *start);
/* hitBuildingPt's search (0x2D54F0): side 0 the left face (+x), 1 the right (-x), 2 the front (+y), 5 the roof:
 * the building and the point on its face within 5 of p (q), -1 none. The hit itself is building_hit */
int collide_hit_point(const CollideWorld *w, const float p[3], int side, float q[3]);
/* 0x2A35A0: the circle p (x, y; z for the tops) of radius r against the two rows; r2 the alley's measure (a gap
 * narrower than 0.1 + 2 r2 is a wall), then the street's limits (x +-55, y -10 .. 21). 1 if p moved */
int collide_buildings_push(const CollideWorld *w, float p[3], float r, float r2);
/* 0x2A49C0: p kept on building b's roof, margin m from an edge with a neighbour, m2 from one without */
void collide_roof_clamp(const CollideWorld *w, int b, const float p[3], float m, float m2, float out[3]);

/* ---- the monster (0x2E2C50, the monster's update) */
typedef struct {
    float pos[3];                                    /* its feet (in, out) */
    float heading;                                   /* degrees (main.c's) */
    int mode;                                        /* +0xAD8: moves.h MODE_* (0 ground, 1 air, 5 on a building) */
    float half_w, half_h;                            /* MONSTER_DATA +0x198, +0x19C (3, 4.25), x scale */
    float probe[2];                                  /* MONSTER_DATA +0x1A0, +0x1A4 (0, 2.5) x scale: its body's centre */
    int wide;                                        /* +0xB9C0 set (Monster.isClimbing's word): the alleys'
                                                      * measure 5 more */
    int no_push;                                     /* +0x108 (setIsAI, 0x2E35C0) set: PushOutMonsters objects do
                                                      * not push it */
    int dead;                                        /* +0xA4C <= 0: no building below */
    float vel[3], stick[3];                          /* +0xBD30, +0xA68: zeroed against a building it is pushed off */
    int below_set, below;                            /* set: the building under it, 0x2D6150's answer this frame
                                                      * (from its place before the move), as collide_monster takes it;
                                                      * else collide_building_below at pos */
} CollideMonster;
#define COLLIDE_MAX_TOUCH 16
typedef struct {
    int touch[COLLIDE_MAX_TOUCH]; float touch_normal[COLLIDE_MAX_TOUCH][3]; int ntouch;   /* the VolumeTriggers it
                                                      * touched (0x335BF0 fires them: FireFunc, MinRefireTime) */
    int pushed[COLLIDE_MAX_TOUCH]; int npushed;      /* the objects that pushed it */
    int below;                                       /* the building it is over (0x2D6150), -1 */
    int on_roof;                                     /* kept on that roof (0x2A49C0) */
    int walled;                                      /* pushed by the buildings (0x2A35A0) */
    int feet_lift;                                   /* a PushOutMonsters object pushed it: with foot IK (+0x258C)
                                                      * both feet are let go, 0x33BE90 */
} CollideMonsterOut;
int collide_building_below(const CollideWorld *w, const CollideMonster *m);   /* 0x2D6150 */
void collide_monster(const CollideWorld *w, CollideMonster *m, CollideMonsterOut *out);

/* ---- the SpherePhysicsMover (vtable 0x449500) */
typedef struct CollideMover {
    float vel[3];                                    /* +0x10 */
    float axis[3], rate;                             /* +0x3C, +0x48: the spin, in the world */
    float gravity, mass, restitution, friction;      /* +0x8, +0xC, +0x1C, +0x20 (0x28F220: 9.81, 1, 0.8, 0.4) */
    float inertia[3];                                /* +0x24: (0.67 - 0.27 InertiaSolidity) mass r^2 */
    float radius;                                    /* +0x38 */
    int kinematic;                                   /* a driven object's mover (LineMover, SplineMover,
                                                      * RotationMover, HelicopterMover, AttackMover, HeldObjectMover):
                                                      * its spin getter (+0x10) 0x2873A0 gives the zero vector, its
                                                      * impulse (+0x24) 0x287390 does nothing [R] */
} CollideMover;
/* 0x28F220 + 0x28DE70; a property not set: pass the game's default (Gravity: the template's, DefObjectProps 35,
 * else 9.81; Mass 1; Restitution 0.8; SlidingFriction 0.4; InertiaSolidity 1) */
void collide_mover_init(CollideMover *mv, float radius, float gravity, float mass, float restitution, float friction,
                        float solidity);
/* a driven object's mover (0x28F220 alone, as their vtables' +0x1C): velocity 0, inertia FLT_MAX, the template's
 * Mass, Restitution and SlidingFriction; it never steps. Given to collide_set_mover, a flying object meeting it has
 * the exchange (0x285E90) with it, which moves and turns only the flying one [R] */
void collide_mover_kinematic(CollideMover *mv, float mass, float restitution, float friction);
void collide_mover_spin(CollideMover *mv, const float w[3]);   /* setRotation-like: the spin vector (0x28DD80) */
/* 0x28E0D0: an impulse j at the world point p on the object at m (its velocity and spin) */
void collide_mover_impulse(CollideMover *mv, const float m[12], const float j[3], const float p[3]);
/* 0x28EB30: a contact at p with the surface's normal n (out of it): the restitution and Coulomb friction impulse */
void collide_mover_contact(CollideMover *mv, const float m[12], const float p[3], const float n[3]);
/* called for each contact of a sub-step: other -1 the street (OnHitGround, event 11, with the ground's material 0),
 * else the object index met (TakeImpact, event 10, on both: the thrown one and the one it meets) */
typedef void (*CollideContactFn)(void *user, int other, const float point[3], const float normal[3]);
/* one frame (vtable +0x30, 0x28E440): sub-steps of at most 0.75 r of travel; each moves and turns m, falls, meets
 * the street (pushed out, impulse, fn(-1)) and then the objects its shape reaches (impulse off a static one, an
 * exchange with a flying one (collide_set_mover), fn(i); 0x287430). self: its own index in w (its shape and m are read there and its m kept up), -1 none (then only the
 * street). Returns the number of street contacts */
int collide_mover_step(CollideWorld *w, CollideMover *mv, float m[12], int self, float dt, CollideContactFn fn,
                       void *user);
/* the same, the objects with any of `exclude` left out of the objects it meets (0x287430 asks with include -1,
 * exclude 0: collide_mover_step). The viewer's movers leave out the monsters (COLLIDE_F_MONSTER), as the game's meet
 * none: a monster's shape is its WCISpheres, type 3, which 0x27FD70 never meets [R] */
int collide_mover_step_ex(CollideWorld *w, CollideMover *mv, float m[12], int self, float dt, uint32_t exclude,
                          CollideContactFn fn, void *user);
/* the object obj of the frame's world flies on mv (its matrix m, its module's velocity vel, which the exchange keeps
 * up): another flying object meeting it exchanges an impulse with it (0x285E90) instead of meeting it as static.
 * A kinematic mv: m NULL takes the object's own matrix, vel (NULL: the mover's, 0) is only read */
void collide_set_mover(CollideWorld *w, int obj, CollideMover *mv, float m[12], float vel[3]);

/* ---- 0x2E33A0's hit lists, kept the other way round: each attack record (an arming, 0x2E7880: the parts it arms
 * share it, 0x2E45A0) lists the objects it has hit (8 ids at +0x6); here an object keeps the records (monster,
 * record number) that hit it, the last eight */
typedef struct { uint32_t key[8]; int next; } CollideHitList;
static inline int collide_hit_seen(const CollideHitList *h, int monster, unsigned record)   /* on its list */
{
    uint32_t k = (((uint32_t)monster & 0xFu) << 28 | (record & 0x0FFFFFFFu)) + 1u;
    for (int i = 0; i < 8; i++)
        if (h->key[i] == k)
            return 1;
    return 0;
}
/* 1 when the record has not hit the object yet (and now has), 0 when it has */
static inline int collide_hit_once(CollideHitList *h, int monster, unsigned record)
{
    if (collide_hit_seen(h, monster, record))
        return 0;
    h->key[h->next] = (((uint32_t)monster & 0xFu) << 28 | (record & 0x0FFFFFFFu)) + 1u;
    h->next = (h->next + 1) & 7;
    return 1;
}

/* ---- the viewer's side: who owns an object of the frame's world */
#define COLLIDE_F_WORLD 0x4u                         /* every object in the world (the queries' "4") */
/* CollideObj.tag / CollideBuilding.tag: the module that added it and its index there; kind is the module's own
 * (which of its lists: boss.c's parts 0 and VolumeTriggers 1, the monster's spheres by number, ...) */
enum { COLLIDE_OWN_NONE, COLLIDE_OWN_BUILDING, COLLIDE_OWN_MONSTER, COLLIDE_OWN_ARMY, COLLIDE_OWN_VEHICLE,
       COLLIDE_OWN_RAIL, COLLIDE_OWN_PARADE, COLLIDE_OWN_STREET, COLLIDE_OWN_BOSS };
#define COLLIDE_TAG(owner, index) ((int)(((unsigned)(owner) << 24) | ((unsigned)(index) & 0xFFFFFFu)))
#define COLLIDE_TAG_OWNER(tag) ((int)((unsigned)(tag) >> 24))
#define COLLIDE_TAG_INDEX(tag) ((int)((unsigned)(tag) & 0xFFFFFFu))
/* an object index of this frame's world still its owner's (its tag and kind as added); -1 otherwise */
int collide_check(const CollideWorld *w, int obj, int tag, int kind);

/* the game's 3 x 4 matrix from and to a column-major 4 x 4 (the modules' world matrices) */
void collide_matrix_from16(float m[12], const float w16[16]);
void collide_matrix_to16(const float m[12], float w16[16]);

#endif
