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
} MoveTrans;
typedef struct {
    const char *name, *clip;                         /* the clip by name, as in the anims bundle (any case) */
    float length; int next;                          /* the state that follows when the clip ends */
    MoveTrans *trans; int ntrans;
} MoveState;
typedef struct { MoveState *states; int n; MoveTrans *all; } MoveGraph;

int moves_read(const Bundle *monster, MoveGraph *g);  /* the bundle's MONSTER_DATA; 0 = ok */
void moves_free(MoveGraph *g);
int moves_find(const MoveGraph *g, const char *state);   /* -1 = none */

typedef struct {
    float v[MV_COUNT];                               /* held values (booleans 0 / 1) */
    float fresh[MV_COUNT];                           /* > 0: pressed lately (seconds left), for "new" conditions */
} MoveInput;
/* fills the derived variables (30, 31, 48..57) from the sticks and buttons already in v */
void moves_derive(MoveInput *in);

typedef int (*MoveCall)(void *ctx, const char *call);    /* the answer of a script check */
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

#endif
