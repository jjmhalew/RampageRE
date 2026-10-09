/* building.c - a building's cells and how hits damage them (building.h; docs/FORMAT_BDP.md "Damage"). */
#include <string.h>
#include "building.h"

#define CELL_W 5.0f
#define GROUND_H 6.0f
#define STOREY_H 3.0f

void building_init(Building *b, const float min[3], const float max[3], int width, int depth, int nstoreys)
{
    memset(b, 0, sizeof *b);
    memcpy(b->min, min, sizeof b->min);
    memcpy(b->max, max, sizeof b->max);
    b->width = width;
    b->depth = depth;
    b->nstoreys = nstoreys < BUILDING_STOREYS ? nstoreys : BUILDING_STOREYS;
    b->state = BUILDING_STANDING;
    b->hp = b->hp_max = 6 * width * b->nstoreys;     /* BuildingHP's default (0x2D2850) */
    for (int s = 0; s < b->nstoreys; s++) {
        b->floor_hp[s] = 8 * width;                  /* FloorHP's default (0x2CBBB4) */
        for (int c = 0; c < BUILDING_CELLS; c++)
            b->cell[s][c].hp = 10;                   /* 0x2CBEB4 */
    }
    b->seed = 1;
}

int building_cell_index(const Building *b, int face, int column)
{
    int n = face == FACE_FRONT ? b->width : b->depth;
    if (column < 0 || column >= n || (face != FACE_LEFT && face != FACE_RIGHT && face != FACE_FRONT))
        return -1;
    int i = face * b->depth + column;                /* left 0 .. d-1, right d .. 2d-1, front 2d .. */
    return i < BUILDING_CELLS ? i : -1;
}

static uint32_t rnd(Building *b)                     /* Park-Miller, seed x 16807 mod 2^31 - 1 (0x106BD0) */
{
    b->seed = (uint32_t)((uint64_t)b->seed * 16807 % 2147483647u);
    return b->seed;
}

/* 0x2BB410 / 0x2CFDA0: a front cell's state; the front's end cells are the sides' front cells too */
static void set_state_mirrored(Building *b, int storey, int col, int state)
{
    int i = building_cell_index(b, FACE_FRONT, col);
    if (i < 0)
        return;
    b->cell[storey][i].state = (uint8_t)state;
    if (col == 0 && (i = building_cell_index(b, FACE_LEFT, b->depth - 1)) >= 0)
        b->cell[storey][i].state = (uint8_t)state;
    if (col == b->width - 1 && (i = building_cell_index(b, FACE_RIGHT, 0)) >= 0)
        b->cell[storey][i].state = (uint8_t)state;
    b->dirty = 1;
}

/* 0x2BAE70: a floor whose FloorHP ran out: all its cells to state 3 */
static void floor_falls(Building *b, int storey)
{
    for (int c = 0; c < 2 * b->depth + b->width && c < BUILDING_CELLS; c++)
        b->cell[storey][c].state = 3;
    b->state = BUILDING_FLOOR_FALLEN;
    b->dirty = 1;
}

/* 0x2BB950: amt HP off the front cell (storey, col); with spread, the hit carries to a neighbour as the cell passes
 * 7 HP (above or below, by where in the cell it was hit) and 3 HP (left or right, at random). 1 if it took damage. */
static int cell_damage(Building *b, int storey, int col, int amt, int spread, float fv)
{
    int i = building_cell_index(b, FACE_FRONT, col);
    if (i < 0 || storey < 0 || storey >= b->nstoreys)
        return 0;
    BuildingCell *c = &b->cell[storey][i];
    int old = c->hp, now = old - amt > 0 ? old - amt : 0, d = old - now;
    if (d == 0)
        return 0;
    if (d >= b->hp) {                                /* the building's HP is gone: it falls */
        b->hp = 0;
        b->state = BUILDING_DESTROYED;
        b->dirty = 1;
        return 1;
    }
    b->hp -= d;
    if (d >= b->floor_hp[storey]) {                  /* the floor's HP is gone: it collapses */
        b->floor_hp[storey] = 0;
        floor_falls(b, storey);
        return 1;
    }
    b->floor_hp[storey] -= d;
    c->hp = (uint8_t)now;
    int s = c->state;
    if (s <= 0)
        set_state_mirrored(b, storey, col, 1);       /* cracked: glass (0x34) or wall (0x35) debris */
    if (now >= 7)
        return 1;
    if (old >= 7 && spread) {
        int nb = storey + (fv >= 0.5f ? 1 : -1);
        if (nb < 0 || nb >= b->nstoreys)
            return 1;                                /* the game returns here too, skipping the rest */
        cell_damage(b, nb, col, 3, 0, fv);
    }
    if (now >= 5)
        return 1;
    if (s < 2)
        set_state_mirrored(b, storey, col, 2);
    if (now >= 3)
        return 1;
    if (old >= 3 && spread) {                        /* a living neighbour to the left or right */
        int alive[2], n = 0;
        if (col > 0 && b->cell[storey][i - 1].hp > 0) alive[n++] = col - 1;
        if (col + 1 < b->width && b->cell[storey][i + 1].hp > 0) alive[n++] = col + 1;
        if (n)
            cell_damage(b, storey, alive[rnd(b) % (uint32_t)n], 3, 0, fv);
    }
    if (now > 0)
        return 1;
    if (s < 3)
        set_state_mirrored(b, storey, col, 3);       /* destroyed: heavy debris (0x36) */
    return 1;
}

int building_hit(Building *b, const float p[3], float radius)
{
    if (b->state != BUILDING_STANDING)
        return 0;
    float q[3];
    for (int k = 0; k < 3; k++)                      /* 0x2BB7B0: into the box, onto the nearest face (not the back) */
        q[k] = p[k] < b->min[k] ? b->min[k] : p[k] > b->max[k] ? b->max[k] : p[k];
    float roof = b->max[2] - q[2], front = q[1] - b->min[1], left = q[0] - b->min[0], right = b->max[0] - q[0];
    if (roof < front && roof < left && roof < right) q[2] = b->max[2];
    else if (front < left && front < right) q[1] = b->min[1];
    else if (left < right) q[0] = b->min[0];
    else q[0] = b->max[0];
    /* 0x2CCCA0: the face, storey and column, and where in the cell (f) */
    float z = q[2] - b->min[2];
    if (z < GROUND_H + 0.01f || q[2] >= b->max[2] - 1.51f)
        return 0;                                    /* the ground floor and the roof take no cell damage */
    float zr = z - GROUND_H;
    int storey = (int)(zr / STOREY_H), face, col;
    float fh, fv = (zr - STOREY_H * (float)storey) / STOREY_H;
    if (storey >= b->nstoreys)
        return 0;
    if (q[1] <= b->min[1] + 0.01f) {
        face = FACE_FRONT;
        col = (int)((q[0] - b->min[0]) * 0.2f);
        col = col < 0 ? 0 : col >= b->width ? b->width - 1 : col;
        fh = (q[0] - b->min[0] - CELL_W * (float)col) * 0.2f;
    } else if (q[0] >= b->max[0] - 0.01f) {
        face = FACE_RIGHT;
        col = (int)((q[1] - b->min[1]) / CELL_W);
        col = col < 0 ? 0 : col >= b->depth ? b->depth - 1 : col;
        fh = (q[1] - b->min[1] - CELL_W * (float)col) * 0.2f;
    } else if (q[0] <= b->min[0] + 0.01f) {
        face = FACE_LEFT;
        col = (int)((b->max[1] - q[1]) / CELL_W);
        col = col < 0 ? 0 : col >= b->depth ? b->depth - 1 : col;
        fh = -(q[1] - (b->max[1] - CELL_W * (float)col)) * 0.2f;
    } else {
        return 0;
    }
    (void)fh;                                        /* where across the cell: the dents (0x2CC900), not drawn here */
    if (radius < 2.0f)
        return 0;
    if (face == FACE_LEFT && col == b->depth - 1) { face = FACE_FRONT; col = 0; }        /* the corners count as the */
    if (face == FACE_RIGHT && col == 0) { face = FACE_FRONT; col = b->width - 1; }       /* front's end cells */
    return face == FACE_FRONT ? cell_damage(b, storey, col, 2, 1, fv) : 0;
}
