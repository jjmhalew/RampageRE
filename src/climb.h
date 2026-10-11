/* climb.h - a monster on a building's face (docs/FORMAT_MONSTER.md "Climbing"): the port of the Monster bindings
 * canMount (0x2DDA20, its candidates 0x2A4470 and the check of a place 0x2DDC40), canClimb (0x2DD100), canTransfer (0x2DD8B0, its
 * candidates 0x2A41F0), transferMount (0x2DD680), the spider's neighbour across an alley (0x2DD710), prepareToMount
 * (0x2D6450), mountBuilding (0x2D63D0), transitionToMount (0x2D50C0), unmountBuilding (0x2D6380), changeMountRail
 * (0x2D4FF0), transitionFromBuildingTop (0x2D50A0), and the monster's place on the face each frame (0x2D5870,
 * 0x2E14F0). A face has rails 5 apart along the front's plane: rail 0 is the left side at the front-left corner,
 * rail = width the right side at the front-right corner, the ones between the front. */
#ifndef RAMPAGE_CLIMB_H
#define RAMPAGE_CLIMB_H
#include "city.h"

typedef struct {
    int building;                                    /* in the scene, -1 none */
    float pt[3];                                     /* on the face: x = min x + 5 x rail, y = min y */
    int rail, storey;
} MountSpot;

typedef struct {
    MountSpot at;                                    /* +0xC640, +0xC634, +0xC630, +0xC62C */
    int mounted;                                     /* +0xC644 */
    int from_roof;                                   /* +0xC645: the spot came from the roof's edge (side 5) */
    int rail_changed;                                /* +0xC646, until the next clip */
    int from_top;                                    /* +0xC778: coming down from the roof, until the next clip */
    MountSpot cand; int have_cand;                   /* +0xC708: canMount's choice */
    MountSpot xfer; int have_xfer;                   /* +0xC724: canTransfer's choice */
    int spider;                                      /* the state is Spider_Climb_Down, _Climb_Up or _Idle */
    float move_time, move_left;                      /* transitionToMount: +0xBA4C */
    float move_from[3], move_to[3], heading_from;
} Mount;

typedef struct {                                     /* what the climbing code asks of the monster */
    float pos[3], heading;                           /* heading in degrees, 0 facing +y */
    float half_width;                                /* MONSTER_DATA +0x198 (3 for George) */
    float focus_x;                                   /* the middle of the players' x (0x336360): its own x alone */
    float probe[2];                                  /* MONSTER_DATA +0x1A0, +0x1A4: where it looks for a face */
    int in_water;                                    /* its box is in a water (0x2D5F00; on a face only in a climb
                                                        down): climb_check lets go (0x2E1660) */
    int mounted;                                     /* its own +0xC644 */
    struct {                                         /* the other monsters (0x2DDC40's grid objects flagged 0x40) */
        float pt[3];                                 /* on a face its mount point (+0xC634), else its place */
        float lo[3], hi[3];                          /* its box (its object in the grid) */
        int mounted;                                 /* +0xC644 */
    } other[4];
    int nother;
} Climber;

int climb_can_mount(Mount *m, const Scene *s, const Climber *c, int side, float zoff);   /* 1 = yes, else -2 .. -9 */
/* the check of a place on building bi (0x2DDC40): 1 fine, else -2 .. -9 (the bots' mount points) */
int climb_place_ok(const Scene *s, int bi, const float pt[3], const Climber *c);
/* spider: canSpiderClimb, the top is the lower of the two buildings either side of the alley */
int climb_can_climb(const Mount *m, const Scene *s, const Climber *c, int dv, int dh, int spider);
int climb_can_transfer(Mount *m, const Scene *s, const Climber *c, int right);   /* 1 = yes, else -7, -2 .. -9 */
void climb_transfer(Mount *m);                       /* transferMount: onto canTransfer's choice */
void climb_prepare(Mount *m);
void climb_mount(Mount *m, const Climber *c, float t);   /* mountBuilding, then transitionToMount(t) */
void climb_unmount(Mount *m);
void climb_change_rail(Mount *m, int dir);
void climb_clip_started(Mount *m, const char *state);   /* 0x2E7F70 clears the rail and top flags */
float climb_wall_heading(const Mount *m, const Scene *s);
/* each frame on a building (mode 5), after the clip's root motion: where the monster is held (pos, heading) by the
 * state's node flags; delta the root's travel this frame (for transitionToMount); dt seconds */
void climb_place(Mount *m, const Scene *s, uint32_t flags, const float delta[3], float dt, float pos[3], float *heading);
/* each frame (0x2E14F0): the place re-checked; the slot to fire when the monster is knocked off (31, or 73 when the
 * building falls), else -1. Mode: its movement mode (off a building it lets go). In a water (c->in_water) a
 * good place does not hold it: it lets go (0x2E1660). */
int climb_check(Mount *m, const Scene *s, const Climber *c, int mode);

/* the hands on the wall (monster +0xC77C, 0x323180): attachHand (0x3228B0, 0x3219D0), detachHand (0x3218C0), the
 * release on unmounting (0x322EC0, 0.1 s) and each frame's update (0x322C50, 0x322960); the arm IK they drive is
 * ik.c's. Hand 0 is the left one, 1 the right (as seen: the script's hand swapped when the clip plays mirrored). */
typedef struct {
    float target, start, remaining, duration;        /* +0x0 .. +0xC: the share it blends to, from, time left, of */
    int attaching, planted, valid;                   /* +0x10, +0x11, +0x12 */
    float sq[4], sp[3];                              /* +0x14, +0x24: where it is held, smoothed (world) */
    float tq[4], tp[3];                              /* +0x30, +0x40: where it is to be */
} Hand;
float hand_weight(const Hand *h);                    /* now: a smoothstep from start to target */
/* time <= 0: 0.2, or nothing while it already grips; pos (snapped) and q where the clip puts the hand; 1 if taken */
int hand_attach(Hand *h, float time, const float pos[3], const float q[4]);
void hand_detach(Hand *h, float time);
/* 0x322680: the hand's place held to the mount's face (0.75 off it; on a side one hand at the corner, the other 2.5
 * along), between the building's foot and top less 1.5 */
void climb_hand_snap(const Mount *m, const Scene *s, int hand, float pos[3]);
/* each frame: the timer, the target following the clip's hand while it attaches (anim) and held to the face, then
 * the smoothing (0.7 of the target, 0.3 of the last place) */
void hand_update(Hand *h, int hand, const Mount *m, const Scene *s, const float anim[3], float dt);

#endif
