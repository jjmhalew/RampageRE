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
} Climber;

int climb_can_mount(Mount *m, const Scene *s, const Climber *c, int side, float zoff);   /* 1 = yes, else -2 .. -9 */
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
 * building falls), else -1. Mode: its movement mode (off a building it lets go). */
int climb_check(Mount *m, const Scene *s, const Climber *c, int mode);

#endif
