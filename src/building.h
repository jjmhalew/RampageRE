/* building.h - a building's cells, how hits damage them and how it falls (docs/FORMAT_BDP.md "Damage", "Collapse"):
 * the port of the game's Building::hit (vtable +0x34, 0x2BC150), its point-to-cell mapping (0x2BB7B0, 0x2CCCA0), cell
 * damage (0x2BB950), floor collapse (0x2BAE70, 0x2D2D00, 0x2BD3B0) and destruction (the update 0x2BDC70: 0x2BD7D0,
 * 0x2BD610) with its shaker (0x2CC450). A building is a grid of cells: per storey above the ground floor, its faces'
 * columns (left side, right side, front; depth columns on a side, width on the front), each with 10 HP and a damage
 * state 0 .. 3 (the facade tile's row). Storeys are kept by their place in the template (o); the standing ones, bottom
 * up, are live[0 .. n-1], so a storey that falls leaves the list and the ones above it come down. */
#ifndef RAMPAGE_BUILDING_H
#define RAMPAGE_BUILDING_H
#include <stdint.h>

#define BUILDING_STOREYS 48
#define BUILDING_CELLS 48                            /* 2 x depth + width columns per storey */

enum { FACE_LEFT = 0, FACE_RIGHT = 1, FACE_FRONT = 2, FACE_GROUND = 4, FACE_ROOF = 5 };
enum {                                               /* +0x14C */
    BUILDING_GONE = 0,                               /* flattened: only its rubble is left */
    BUILDING_STANDING = 1, BUILDING_FLOOR_FALLING = 3,
    BUILDING_CRACKING = 4, BUILDING_SHAKING = 5, BUILDING_FALLING = 6   /* destroyed, in that order */
};

typedef struct {
    uint8_t state, hp, tile;                         /* +0 damage state, +4 HP (10), +5 the sheet's column */
} BuildingCell;

typedef struct {
    float min[3], max[3];                            /* in the world (the grid's +0x24 .. +0x38); max z drops as it falls */
    int width, depth, storeys;                       /* storeys: as built */
    int n, live[BUILDING_STOREYS];                   /* the standing storeys, bottom up (G+0x18, G+0x48) */
    int state;                                       /* BUILDING_* */
    int hp, hp_max;                                  /* BuildingHP (+0x390, +0x394; default 6 x width x storeys) */
    int floor_hp[BUILDING_STOREYS];                  /* FloorHP per storey (floor +0x14; default 8 x width) */
    BuildingCell cell[BUILDING_STOREYS][BUILDING_CELLS];   /* [o][face x depth + column], fronts from 2 x depth */
    uint8_t vert[BUILDING_STOREYS + 1][BUILDING_CELLS];    /* the front's vertex levels 0 .. 3 by live row (dents) */
    int collapse;                                    /* G+0x1CC: -1 none, k >= 1 live storey k-1 falls, 0 it sinks */
    float progress;                                  /* G+0x1D0: storeys fallen (0 .. 1; 0 .. 7/3 sinking) */
    float timer;                                     /* the destruction's clock (+0x150) */
    float collapse_time, crack_time, hesitation;     /* CollapseTime 0.25, CrackSpreadTime 0.05, HesitationTime 0.4 */
    int min_crack;                                   /* MinCollapseCrackLevel 1 */
    float shake_time, shake_length, shake_amp;       /* the shaker (+0x8AC): time left, ShakeLength, MasterAmp */
    float shake_t, shake_total;
    float shake[4][3];                               /* (period, phase, amp) */
    uint32_t seed;                                   /* Park-Miller (0x106BD0) */
    int dirty;                                       /* a cell or a storey changed: redraw */
} Building;

void building_init(Building *b, const float min[3], const float max[3], int width, int depth, int storeys);
int building_cell_index(const Building *b, int face, int column);    /* -1 if out of range */
/* a hit at the world point p (on or near one of its faces) with radius r (2.0 from a monster's attack); 1 if a cell
 * took damage. Only front cells take damage from hits, and only while it stands, as in the game. */
int building_hit(Building *b, const float p[3], float radius);
void building_destroy(Building *b);                 /* the collapseBuilding binding (0x2BB8B0): it falls */
void building_update(Building *b, float dt);         /* collapses, destruction, the shaker (0x2BDC70) */
float building_shake(const Building *b);             /* the sway along x now (0x2CC450) */
/* where live storey s stands now: its bottom and its height (a falling storey is squashed as the ones above come
 * down onto it); the drop of the whole body while it sinks into the ground */
void building_storey(const Building *b, int s, float *bottom, float *height);
float building_sink(const Building *b);
int building_live_of(const Building *b, int o);      /* storey o's live place, -1 if it has fallen */

#endif
