/* moves.h - a monster's move graph (MONSTER_DATA, docs/FORMAT_MONSTER.md) and the game's runner for it: the port of
 * 0x2EBFE0 (a state's time and windows), 0x2EC3D0 (which transition holds) and 0x2ECBE0 (where the next clip
 * starts). The input is the per player variable table that 0x2C0AD0 fills from the pad (MoveInput). */
#ifndef RAMPAGE_MOVES_H
#define RAMPAGE_MOVES_H
#include "data.h"

enum {                                               /* condition variables (tools/monster.py VARIABLES) */
    MV_LX = 0, MV_LY = 1, MV_RX = 2, MV_RY = 3,      /* sticks, -1 .. 1, y down */
    MV_UP = 4, MV_DOWN, MV_LEFT, MV_RIGHT, MV_START, MV_SELECT, MV_R2, MV_L2, MV_R1, MV_L1,
    MV_CROSS, MV_CIRCLE, MV_SQUARE, MV_TRIANGLE, MV_R3 = 20, MV_L3 = 21,
    MV_R_SIZE = 30, MV_L_SIZE = 31,                  /* max(|x|, |y|) of each stick */
    /* the left stick in the monster's own frame (0x2C03E0; its size where it points, else 0): quadrants, halves and
     * 45-degree sectors */
    MV_M_FRONT = 32, MV_M_LEFT, MV_M_RIGHT, MV_M_BACK, MV_M_FRONTHALF, MV_M_BACKHALF, MV_M_RIGHTHALF, MV_M_LEFTHALF,
    MV_M_FRONT8 = 40, MV_M_FRONTRIGHT8, MV_M_FRONTLEFT8, MV_M_RIGHT8, MV_M_LEFT8, MV_M_BACK8, MV_M_BACKRIGHT8,
    MV_M_BACKLEFT8,
    MV_L_UP = 48, MV_L_DOWN, MV_L_LEFT, MV_L_UPLEFT, MV_L_DOWNLEFT, MV_L_RIGHT, MV_L_UPRIGHT, MV_L_DOWNRIGHT,
    MV_TRUE = 56, MV_FALSE = 57, MV_COUNT = 58
};

typedef struct { uint32_t op, var, flags, value; } MoveCond;   /* var OP value; flags bit 0: held, else new */
typedef struct {
    int target; uint32_t flags;                      /* 0x10 / 0x8: allowed for a pad / the other controller,
                                                        0x1000 switch at once, 0x1 keep the clip's phase */
    float in0, in1, sw0, sw1;                        /* input window, switch window (seconds into the state) */
    int priority, ncond; MoveCond cond[4];
    const char *call;                                /* a script check (Monsters::canMount, ...), NULL = none */
    float args[4];                                   /* its four arguments (+0x64 .. +0x70) */
    int blend;                                       /* +0x0: how the next clip comes in, BLEND_* (0x2FFA00) */
} MoveTrans;
typedef struct {                                     /* a timed event of the state's clip (MONSTER_DATA's coded items) */
    uint32_t type;                                   /* 21 a script call, 7 the mode, 6 a sound, 0 an attack, ... */
    float at;                                        /* when, as a share of the clip's length */
    const char *name;                                /* 21: the script function, 6: the sound */
    float p[4]; uint32_t value;                      /* 21: its four arguments; 7: the mode; 0: the body
                                                        parts (bits), p[0] 1 arms / 0 disarms them, p[1..3] its
                                                        floats at +0x2C (5 a punch, 20 the kick), +0x30, +0x34 */
} MoveEvent;
typedef struct {
    const char *name, *clip;                         /* the clip by name, as in the anims bundle (any case) */
    float length; int next;                          /* the state that follows when the clip ends */
    int end_blend;                                   /* BLEND_* into it (+0x10, the "clip ended" transition) */
    /* the state's node: the clip plays at `rate` (length = the clip's duration / rate); with NODE_STEER the monster
     * turns towards the stick at `turn` × the angle per second (0x2E8190), its forward axis `fwd` (x, y); with
     * NODE_AIR_* the stick drives it at up to `speed` units / s; `blend` seconds to blend into the clip */
    float rate, turn, speed, blend, fwd[2]; uint32_t flags;   /* node +0x0, +0x4, +0x8, +0x14, +0x18, +0x2C */
    MoveTrans *trans; int ntrans;
    MoveEvent *events; int nevents;                  /* in time order */
} MoveState;
#define MOVES_SLOTS 74
typedef struct {                                     /* a collision sphere (+0xA48, 24 bytes; 0x2DA9E0 places them) */
    float off[3];                                    /* in its bone's frame */
    float size, radius;                              /* +0xC not traced; +0x10 the sphere's radius */
    int bone, part;                                  /* the skeleton node, the body part */
} MoveShape;
#define MOVES_PARTS 8
#define MOVES_PART_BONES 32
typedef struct {
    MoveState *states; int n; MoveTrans *all; MoveEvent *all_events;
    /* the game's own events (landing, hits, ...): per slot and column the state they start (-1 none); column 0 is
     * the default (+0x4), columns 1.. the overrides (+0xA98) the clip events of type 23 select (moves_slot) */
    int (*slot)[MOVES_SLOTS]; int ncolumns;
    MoveShape *shapes; int nshapes;
    float box[2];                                    /* +0x198 across, +0x19C up: the collision box (3, 4.25) */
    float probe[2];                                  /* +0x1A0, +0x1A4: where canMount looks for a face (0, 2.5) */
    /* the body parts (+0xA74: Arm L, Arm R, Body, Head, Leg L, Leg R) and their bones, which an attack event (type 0)
     * arms: a shape whose bone is armed can hit */
    const char *part_name[MOVES_PARTS]; int nparts;
    int part_nbones[MOVES_PARTS]; int16_t part_bone[MOVES_PARTS][MOVES_PART_BONES];
    /* every state node (+0xA4C / +0xA50), by name: setFacialAnim plays a node's clip on the face (George's
     * Expression_Idle .. Expression_Zap play George_Head_*), its rate 1 / the clip's duration */
    struct MoveNode { const char *name, *clip; float rate, blend; } *nodes; int nnodes;
    /* the attachment nodes (+0xA84 / +0xA88, 0x38 bytes): a matrix (rows, then the place) on a bone: Helper R Hand
     * (what the monster holds things by), fxNode_*, Tracking_Node ... */
    struct MoveAttach { const char *name; int bone; float m[12]; } *attach; int nattach;
} MoveGraph;
typedef struct MoveNode MoveNode;

int moves_read(const Bundle *monster, MoveGraph *g);  /* the bundle's MONSTER_DATA; 0 = ok */
void moves_free(MoveGraph *g);
int moves_find(const MoveGraph *g, const char *state);   /* -1 = none */
const MoveNode *moves_node(const MoveGraph *g, const char *name);   /* NULL = none (any case) */

typedef struct {
    float v[MV_COUNT];                               /* held values (booleans 0 / 1) */
    float fresh[MV_COUNT];                           /* > 0: pressed lately (seconds left), for "new" conditions */
    int mirrored;                                    /* the monster plays mirrored: conditions with flag 2 swap left
                                                        and right (0x2C15F8) */
} MoveInput;
/* fills the derived variables (30, 31, 48..57) from the sticks and buttons already in v */
void moves_derive(MoveInput *in);
/* the variables 32 .. 47 from the stick's direction in the monster's frame (a along its right, b its forward; the
 * stick as 0x2E8410 puts it in the world, against the monster's axes), after moves_derive */
void moves_sectors(MoveInput *in, float a, float b);

typedef int (*MoveCall)(void *ctx, const char *call, const float *args);   /* the answer of a script check */
typedef struct {
    const MoveGraph *g;
    int state; float time;
    int chosen;                                      /* transition picked and waiting for its switch window, -1 */
    uint8_t *in_window, *sw_window;                  /* per transition of the state */
    int from, via;                                   /* after a switch: the old state, the transition (-1: clip end) */
} MoveRunner;
void moves_start(MoveRunner *r, const MoveGraph *g, int state);
/* one frame: dt seconds (times the monster's speed); 1 when it switched state (r->from, r->via say how) */
int moves_step(MoveRunner *r, MoveInput *in, float dt, MoveCall call, void *ctx);
/* switch to `to` now through the transition whose only condition is false == 0 (the game's own events: landing,
 * turning around); 0 = done */
int moves_event(MoveRunner *r, const char *to);
void moves_stop(MoveRunner *r);
/* the events of state s whose time lies in (from, to] seconds; *first: the first, returns how many (they follow it) */
int moves_events_between(const MoveState *s, float from, float to, const MoveEvent **first);
/* the state slot k starts in a column (the monster's +0xBAB4, -1 = default), -1 if none */
int moves_slot(const MoveGraph *g, int slot, int column);
void moves_force(MoveRunner *r, int state);           /* the state now, from its start (0x2EBF50) */
enum { SLOT_LAND = 56, SLOT_LAND_BACK = 57 };         /* 0x2DC450: on its feet / the ground's normal against its up */

/* how a transition brings the next clip in (its +0x0, the low nibble of the clip request's flags >> 4): BLEND_CUT
 * at once, BLEND_IN from the pose it has over the new state's blend time (unless nothing plays yet) */
enum { BLEND_CUT = 0, BLEND_IN = 1 };

/* movement modes (event type 7; the monster's +0xAD8): 1 and 2 are airborne (Monster.isAirborne) */
enum { MODE_GROUND = 0, MODE_AIRBORNE = 1, MODE_TAKEOFF = 2, MODE_PRONE = 4, MODE_BUILDING = 5 };
#define MOVES_GRAVITY 60.0f                          /* units / s², 0x2DF128; off while setDisableGravity (+0xAB8) */

/* state node flags (+0x2C), as the monster's command (0x2E6580) and update (0x2DE530 ...) test them */
enum {
    NODE_AIR_Z = 0x2, NODE_AIR_X = 0x10, NODE_AIR_Y = 0x1000,   /* the stick drives it along its own axes */
    NODE_HOLD_FRAME = 0x4,                           /* the camera frame for the stick is kept until the stick moves */
    NODE_STEER = 0x8,                                /* it turns towards the stick (0x2E8190) */
    NODE_LOOK = 0x200,                               /* the right stick turns its head (0x2FBE30) */
    NODE_ROOT_X = 0x40, NODE_ROOT_Y = 0x80, NODE_ROOT_Z = 0x80000,   /* the root's travel along x / y / z and its */
    NODE_ROOT_TURN = 0x100,                          /* turn move the monster (0x301080); otherwise they stay in the pose */
    NODE_BAKE_ROOT = 0x40000                         /* when the next clip starts, the root's place and turn become
                                                        the monster's (0x2E6580) */
};
#define MOVES_AIR_ACCEL 50.0f                        /* units / s² towards the stick's speed, gp−0x722C */
#define MOVES_AIR_BRAKE 100.0f                       /* ... when slowing down, gp−0x7230 */

#endif
