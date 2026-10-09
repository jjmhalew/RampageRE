/* building.h - a building's cells and how hits damage them (docs/FORMAT_BDP.md "Damage"): the port of the game's
 * Building::hit (vtable +0x34, 0x2BC150), its point-to-cell mapping (0x2BB7B0, 0x2CCCA0) and cell damage (0x2BB950).
 * A building is a grid of cells: per storey above the ground floor, its faces' columns (left side, right side, front;
 * depth columns on a side, width on the front), each with 10 HP and a damage state 0 .. 3 (the facade tile's row). */
#ifndef RAMPAGE_BUILDING_H
#define RAMPAGE_BUILDING_H
#include <stdint.h>

#define BUILDING_STOREYS 48
#define BUILDING_CELLS 48                            /* 2 x depth + width columns per storey */

enum { FACE_LEFT = 0, FACE_RIGHT = 1, FACE_FRONT = 2, FACE_GROUND = 4, FACE_ROOF = 5 };
enum { BUILDING_STANDING = 1, BUILDING_FLOOR_FALLEN = 3, BUILDING_DESTROYED = 4 };   /* +0x14C */

typedef struct {
    uint8_t state, hp, tile;                         /* +0 damage state, +4 HP (10), +5 the sheet's column */
} BuildingCell;

typedef struct {
    float min[3], max[3];                            /* in the world (the grid's +0x24 .. +0x38); the front faces -Y */
    int width, depth, nstoreys;
    int state;                                       /* BUILDING_*, +0x14C */
    int hp, hp_max;                                  /* BuildingHP (+0x390, +0x394; default 6 x width x storeys) */
    int floor_hp[BUILDING_STOREYS];                  /* FloorHP per storey (floor +0x14; default 8 x width) */
    BuildingCell cell[BUILDING_STOREYS][BUILDING_CELLS];   /* [storey][face x depth + column], fronts from 2 x depth */
    uint32_t seed;                                   /* for the spread to a random neighbour (Park-Miller, 0x106BD0) */
    int dirty;                                       /* a cell changed: redraw */
} Building;

void building_init(Building *b, const float min[3], const float max[3], int width, int depth, int nstoreys);
int building_cell_index(const Building *b, int face, int column);    /* -1 if out of range */
/* a hit at the world point p (on or near one of its faces) with radius r (2.0 from a monster's attack); 1 if a cell
 * took damage. Only front cells take damage from hits, as in the game. */
int building_hit(Building *b, const float p[3], float radius);

#endif
