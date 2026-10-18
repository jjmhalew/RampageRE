/* building.c - a building's cells, how hits damage them and how it falls (building.h; docs/FORMAT_BDP.md "Damage",
 * "Collapse"). */
#include <math.h>
#include <string.h>
#include "building.h"

#define CELL_W 5.0f
#define GROUND_H 6.0f
#define STOREY_H 3.0f
#define ROOF_H 1.5f

void building_init(Building *b, const float min[3], const float max[3], int width, int depth, int storeys)
{
    memset(b, 0, sizeof *b);
    memcpy(b->min, min, sizeof b->min);
    memcpy(b->max, max, sizeof b->max);
    b->width = width;
    b->depth = depth;
    b->storeys = b->n = storeys < BUILDING_STOREYS ? storeys : BUILDING_STOREYS;
    b->state = BUILDING_STANDING;
    b->hp = b->hp_max = 6 * width * b->n;            /* BuildingHP's default (0x2D2850) */
    for (int s = 0; s < b->n; s++) {
        b->live[s] = s;
        b->floor_hp[s] = 8 * width;                  /* FloorHP's default (0x2CBBB4) */
        for (int c = 0; c < BUILDING_CELLS; c++)
            b->cell[s][c].hp = 10;                   /* 0x2CBEB4 */
    }
    b->collapse = -1;
    b->collapse_time = 0.25f;                        /* DefBuildingProps */
    b->crack_time = 0.05f;
    b->hesitation = 0.4f;
    b->min_crack = 1;
    b->shake_length = 0.5f;                          /* ShakerDef */
    b->shake_amp = 0.05f;
    static const float SHAKE[4][3] = { { 0.05f, 1, 3 }, { 0.25f, 2, 0.25f }, { 0.33f, 3, 0.25f }, { 0.61f, 4, 0.25f } };
    memcpy(b->shake, SHAKE, sizeof b->shake);
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

static float rnd01(Building *b) { return (float)rnd(b) / 2147483648.0f; }

/* 0x2CC5E0: shake on, ShakeLength x (1 + a random share) from now [R]: a shake that is over starts again (+0x40 the
 * clock, +0x44 0); one still going has its length (+0x44) cut to the time it has shaken (clock - +0x40), so each hit
 * leaves 0.5 .. 1 s of it, not the sum of them all */
static void shake_more(Building *b)
{
    if (b->shake_time <= 0)
        b->shake_t = 0;
    b->shake_total = b->shake_t;
    float more = b->shake_length * (1 + rnd01(b));
    b->shake_time = more;
    b->shake_total += more;
}

float building_shake(const Building *b)
{
    if (b->shake_time <= 0 || b->shake_total <= 0)
        return 0;
    float x = 0;
    for (int i = 0; i < 4; i++) {                    /* a parabolic sine per triple (0x2CC450) */
        float u = (b->shake_t + b->shake[i][1]) / b->shake[i][0];
        u -= (float)(int)u;
        float w = u <= 0.5f ? 8 * u - 16 * u * u : 8 - 24 * u + 16 * u * u;
        w = w > 1 ? 1 : w < -1 ? -1 : w;
        x += b->shake[i][2] * w;
    }
    x *= b->shake_amp;
    float fade = b->shake_time / (0.2f * b->shake_total);   /* fading out over its last fifth */
    return fade < 1 ? x * fade : x;
}

static void window_drop(Building *b, int k)           /* the list's last record over this one */
{
    b->window[k] = b->window[--b->nwindows];
}

static int window_find(const Building *b, int o, int column, int lr)
{
    for (int k = 0; k < b->nwindows; k++)
        if (b->window[k].o == o && b->window[k].column == column && b->window[k].lr == lr)
            return k;
    return -1;
}

/* 0x2BB410 / 0x2CFDA0: a front cell's state; the front's end cells are the sides' front cells too. A damaged front
 * cell's open windows go at once, their flags with them (0x2BB580) */
static void set_state(Building *b, int o, int col, int state)
{
    int i = building_cell_index(b, FACE_FRONT, col);
    if (i < 0)
        return;
    for (int lr = 0; lr < 2 && state > 0; lr++) {
        int k = window_find(b, o, col, lr);
        if (k >= 0) {
            b->cell[o][i].flags &= (uint16_t)~(CELL_WINDOW_OPEN << lr);
            window_drop(b, k);
        }
    }
    b->cell[o][i].state = (uint8_t)state;
    if (col == 0 && (i = building_cell_index(b, FACE_LEFT, b->depth - 1)) >= 0)
        b->cell[o][i].state = (uint8_t)state;
    if (col == b->width - 1 && (i = building_cell_index(b, FACE_RIGHT, 0)) >= 0)
        b->cell[o][i].state = (uint8_t)state;
    b->dirty = 1;
}

/* 0x2BAE70: live storey k-1 falls (k = 0: the building sinks into the ground): its cells to state 3 */
static void floor_falls(Building *b, int k)
{
    if (b->state == BUILDING_FLOOR_FALLING)
        return;
    if (k > 0 && k - 1 < b->n) {
        int o = b->live[k - 1];
        for (int c = 0; c < 2 * b->depth + b->width && c < BUILDING_CELLS; c++)
            b->cell[o][c].state = 3;
        for (int w = b->nwindows - 1; w >= 0; w--)   /* its open windows (the flags stay, 0x2BB09C) */
            if (b->window[w].o == o)
                window_drop(b, w);
        for (int f = 0; f < 3; f++)                  /* its vertex rows to level 3, on every face */
            for (int c = 0; c <= (f == FACE_FRONT ? b->width : b->depth) && c < BUILDING_VCOLS; c++) {
                building_set_level(b, f, k - 1, c, 3);
                if (k < b->n)
                    building_set_level(b, f, k, c, 3);
            }
    }
    b->collapse = k;                                 /* 0x2CE240 */
    b->progress = 0;
    b->dirty = 1;
}

void building_set_level(Building *b, int face, int row, int col, int level)
{
    int ncol = face == FACE_FRONT ? b->width : b->depth;
    if (face < 0 || face > 2 || row < 0 || row > b->n || col < 0 || col > ncol || col >= BUILDING_VCOLS)
        return;
    BuildingVert *v = &b->vert[row][face][col];
    int old = v->level, lc = level < 3 ? level : 3;
    if (old == level)
        return;
    v->level = (uint8_t)level;
    if (level == 0) {
        v->along = v->up = 0;
    } else if (level > old) {
        if (old == 0 || v->along * v->along + v->up * v->up < 1e-6f) {   /* a new dent, 0x1063F0's sin and cos */
            float r = 0.2f + 0.1f * rnd01(b), th = 6.2831853f * rnd01(b) - 3.14159265f;
            v->along = sinf(th) * r * 0.5f * (float)lc;
            v->up = cosf(th) * r * 0.5f * (float)lc;
        } else {                                     /* deeper, the same way */
            float k = (float)lc / (float)(old < 3 ? old : 3);
            v->along *= k;
            v->up *= k;
        }
    }
    if (row == 0 || row == b->n)                     /* the bottom and the top row only move along */
        v->up = 0;
    /* the front's corners are the sides' front vertices: they stay on the corner and move up together */
    if ((face == FACE_FRONT && (col == 0 || col == b->width)) || (face == FACE_LEFT && col == b->depth) ||
        (face == FACE_RIGHT && col == 0))
        v->along = 0;
    if (face == FACE_LEFT && col == b->depth) b->vert[row][FACE_FRONT][0].up = v->up;
    if (face == FACE_RIGHT && col == 0) b->vert[row][FACE_FRONT][b->width].up = v->up;
    if (face == FACE_FRONT && col == 0) b->vert[row][FACE_LEFT][b->depth].up = v->up;
    if (face == FACE_FRONT && col == b->width) b->vert[row][FACE_RIGHT][0].up = v->up;
    b->dirty = 1;
}

/* 0x2CC900: one dent's worth at a vertex (row, u: the faces unwrapped, left 0 .. 2, front from 2, right from
 * W + 2, the depth taken as 2), or the first of its rings (0x4310D0: 1, 8, 12 vertices) below the ring's cap:
 * 3 at the vertex, 2 around it, 1 further out. 1 if a vertex took it. */
static int dent(Building *b, int row, int u, int amount)
{
    static const int8_t RING[21][2] = {               /* 0x4310E0: (du, drow) */
        { 0, 0 },
        { 1, 0 }, { 1, 1 }, { 0, 1 }, { -1, 1 }, { -1, 0 }, { -1, -1 }, { 0, -1 }, { 1, -1 },
        { 2, 0 }, { 2, 1 }, { 1, 2 }, { 0, 2 }, { -1, 2 }, { -2, 1 }, { -2, 0 }, { -2, -1 }, { -1, -2 }, { 0, -2 },
        { 1, -2 }, { 2, -1 } };
    static const int FIRST[3] = { 0, 1, 9 }, COUNT[3] = { 1, 8, 12 };
    uint32_t r = rnd(b);
    for (int t = 1; t <= 3; t++)
        for (int k = 0; k < t; k++) {
            int cap = t - k;
            for (int i = 0; i < COUNT[k]; i++) {
                const int8_t *d = RING[FIRST[k] + (int)((r + (uint32_t)i) % (uint32_t)COUNT[k])];
                int uu = u + d[0], rr = row + d[1], face, col;
                if (uu < 0 || uu >= b->width + 4 || rr < 0 || rr > b->n)
                    continue;
                if (uu < 2) { face = FACE_LEFT; col = uu; }
                else if (uu <= b->width + 2) { face = FACE_FRONT; col = uu - 2; }
                else { face = FACE_RIGHT; col = uu - b->width - 2; }
                if (col > (face == FACE_FRONT ? b->width : b->depth))
                    continue;
                int lv = b->vert[rr][face][col].level;
                if (lv >= cap)
                    continue;
                building_set_level(b, face, rr, col, lv + amount < cap ? lv + amount : cap);
                int tf = -1, tc = 0;                 /* the corner's twin on the side */
                if (face == FACE_FRONT && col == 0) { tf = FACE_LEFT; tc = 2; }
                if (face == FACE_FRONT && col == b->width) { tf = FACE_RIGHT; tc = 0; }
                if (tf >= 0 && tc <= b->depth) {
                    int tl = b->vert[rr][tf][tc].level;
                    if (tl < cap)
                        building_set_level(b, tf, rr, tc, tl + amount < cap ? tl + amount : cap);
                }
                return 1;
            }
        }
    return 0;
}

/* the building's HP is gone: it is destroyed (0x2BB8B0) */
static void destroyed(Building *b)
{
    b->hp = 0;
    b->state = BUILDING_CRACKING;
    b->timer = 0;
    b->dirty = 1;
}

/* a debris effect for front cell (o, col), for the host (Building.debris) */
static void debris(Building *b, int o, int col, int kind)
{
    if (b->ndebris < BUILDING_DEBRIS) {
        b->debris[b->ndebris].o = (uint8_t)o;
        b->debris[b->ndebris].col = (uint8_t)col;
        b->debris[b->ndebris].kind = (uint8_t)kind;
        b->ndebris++;
    }
}

/* 0x2BB950: amt HP off the front cell (live storey s, col); with spread, the hit carries to a neighbour as the cell
 * passes 7 HP (above or below, by where in the cell it was hit) and 3 HP (left or right, at random). 1 if it took
 * damage. The effects as the state rises [R 0x2BBB54..0x2BBE3C]: a first crack (from state 0) Glass_Large or
 * Wall_Small by the column's Outline, then Wall_Small below 5 HP, Wall_Large at 0; none for a cell flagged 0x400 at the
 * first crack and at 0 (FloorFlags' "hidden behind a front-row building"; not kept here: Chicago's 447 FloorFlags lists
 * are all 0) */
static int cell_damage(Building *b, int s, int col, int amt, int spread, float fv)
{
    int i = building_cell_index(b, FACE_FRONT, col);
    if (i < 0 || s < 0 || s >= b->n)
        return 0;
    int o = b->live[s];
    BuildingCell *c = &b->cell[o][i];
    int old = c->hp, now = old - amt > 0 ? old - amt : 0, d = old - now;
    if (d == 0)
        return 0;
    shake_more(b);
    if (d >= b->hp && !b->no_autocollapse) {         /* the building's HP is gone: it falls */
        destroyed(b);
        return 1;
    }
    b->hp = d >= b->hp ? 0 : b->hp - d;
    if (d >= b->floor_hp[o]) {                       /* the floor's HP is gone: it collapses */
        b->floor_hp[o] = 0;
        floor_falls(b, s + 1);
        b->state = BUILDING_FLOOR_FALLING;
        return 1;
    }
    b->floor_hp[o] -= d;
    c->hp = (uint8_t)now;
    int st = c->state;
    if (st <= 0) {
        set_state(b, o, col, 1);                     /* cracked: glass (0x34) or wall (0x35) debris */
        debris(b, o, col, DEBRIS_CRACK);
    }
    if (now >= 7)
        return 1;
    if (old >= 7 && spread) {
        int nb = s + (fv >= 0.5f ? 1 : -1);
        if (nb < 0 || nb >= b->n)
            return 1;                                /* the game returns here too, skipping the rest */
        cell_damage(b, nb, col, 3, 0, fv);
    }
    if (now >= 5)
        return 1;
    if (st < 2) {
        set_state(b, o, col, 2);
        debris(b, o, col, DEBRIS_WALL_SMALL);
    }
    if (now >= 3)
        return 1;
    if (old >= 3 && spread) {                        /* a living neighbour to the left or right */
        int alive[2], n = 0;
        if (col > 0 && b->cell[o][i - 1].hp > 0) alive[n++] = col - 1;
        if (col + 1 < b->width && b->cell[o][i + 1].hp > 0) alive[n++] = col + 1;
        if (n)
            cell_damage(b, s, alive[rnd(b) % (uint32_t)n], 3, 0, fv);
    }
    if (now > 0)
        return 1;
    if (st < 3) {
        set_state(b, o, col, 3);                     /* destroyed: heavy debris (0x36) */
        debris(b, o, col, DEBRIS_WALL_LARGE);
    }
    return 1;
}

/* cheat 071767 (0x40000000): Building::hit (0x2BC1F0) fells a standing building at its first hit, as a kind-8 hit */
static int one_hit;
void building_set_one_hit(int on) { one_hit = on; }

int building_hit(Building *b, const float p[3], float radius)
{
    if (b->state != BUILDING_STANDING)
        return 0;
    if (one_hit) {
        building_destroy(b);
        return 1;
    }
    float q[3];
    for (int k = 0; k < 3; k++)                      /* 0x2BB7B0: into the box, onto the nearest face (not the back) */
        q[k] = p[k] < b->min[k] ? b->min[k] : p[k] > b->max[k] ? b->max[k] : p[k];
    float roof = b->max[2] - q[2], front = q[1] - b->min[1], left = q[0] - b->min[0], right = b->max[0] - q[0];
    if (roof < front && roof < left && roof < right) q[2] = b->max[2];
    else if (front < left && front < right) q[1] = b->min[1];
    else if (left < right) q[0] = b->min[0];
    else q[0] = b->max[0];
    /* 0x2CCCA0: the face, storey and column, and where in the cell (f); against z itself (the street is at 0) */
    if (q[2] < GROUND_H + 0.01f) {
        /* [R 0x2BC5C4, 0x2BC6F4, 0x2BC840 .. 0x2BC890] the ground floor (face 4) takes no cell damage and no dents; a
         * hit of radius 2 or more by an attacker that is not an Actor (0x2842F0 with 0x4D5BF0) gives Wall_Small (0x35)
         * at the hit's own point. Every hit here is a monster's or a flying object's (an actor's Mass is 1: radius
         * 0.001), so none is an Actor's */
        if (radius >= 2.0f && b->nhit_debris < 8)
            memcpy(b->hit_debris[b->nhit_debris++], p, 3 * sizeof(float));
        return 0;
    }
    if (q[2] >= b->max[2] - ROOF_H - 0.01f)
        return 0;                                    /* the roof takes no cell damage */
    float zr = q[2] - GROUND_H;
    if (b->collapse > 0 && zr >= STOREY_H * (float)b->collapse - STOREY_H * b->progress)
        zr += STOREY_H * b->progress;                /* the storeys coming down are still their own */
    int s = (int)(zr / STOREY_H), face, col;
    float fh, fv = (zr - STOREY_H * (float)s) / STOREY_H;
    if (s >= b->n)
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
    /* Building::hit's dents: at the cell's corner nearest the hit, 3 x round(radius / 2.5) times (0x2CC900) */
    int vrow = s + (int)floorf(fv + 0.5f), vcol = col + (int)floorf(fh + 0.5f);
    int u = face == FACE_LEFT ? vcol : face == FACE_FRONT ? vcol + 2 : vcol + b->width + 2;
    int dented = 0;
    for (int i = 0, n = 3 * (int)floorf(radius / 2.5f + 0.5f); i < n; i++)
        dented |= dent(b, vrow, u, 1);
    if (radius < 2.0f)
        return dented;
    if (face == FACE_LEFT && col == b->depth - 1) { face = FACE_FRONT; col = 0; }        /* the corners count as the */
    if (face == FACE_RIGHT && col == 0) { face = FACE_FRONT; col = b->width - 1; }       /* front's end cells */
    return (face == FACE_FRONT ? cell_damage(b, s, col, 2, 1, fv) : 0) | dented;
}

/* 0x2BBE80 (hitBuildingRoof's hit): the roof one state worse (0x2CF7C0 / 0x2BB580 on face 5, G+0x658, capped at 3);
 * then the top storey's front cell under the point's x, or the nearest one to it still standing (state < 3; the left
 * one nearer wins, a tie the right), hit at its middle 0.3 under the storey's top with Building::hit (kind as given,
 * here 1; radius r) */
int building_hit_roof(Building *b, const float p[3], float radius, int *col_out)
{
    if (col_out)
        *col_out = -1;
    if (b->state != BUILDING_STANDING)
        return 0;
    if (one_hit) {
        building_destroy(b);
        return 1;
    }
    if (b->roof + 1 < 4) {                           /* 0x2BB580 refuses while a storey falls: none does here */
        b->roof++;
        b->dirty = 1;
    }
    int at = (int)(0.2f * (p[0] - b->min[0])), s = b->n - 1, front = 2 * b->depth;   /* cvt.w.s: towards 0 */
    if (s < 0)
        return 0;
    int o = b->live[s];
    int left = at < b->width ? at : b->width - 1;    /* the game reads past the storey's cell array beyond the width:
                                                      * [G: heap memory, not in the ELF; clamped] */
    while (left >= 0 && b->cell[o][front + left].state >= 3)
        left--;
    int right = at + 1 > 0 ? at + 1 : 0;             /* left of the box the game reads before the array [G: as
                                                      * above; from column 0] */
    while (right < b->width && b->cell[o][front + right].state >= 3)
        right++;
    int dl = left >= 0 ? at - left : at + 0x200, dr = right < b->width ? right - at : 0x400 - at;
    int col;
    if (dl < dr) {
        if (dl >= 0x200)
            return 0;                                /* the whole storey's front is destroyed */
        col = left;
    } else {
        col = right;
    }
    if (col < 0 || col >= b->width)
        return 0;
    if (col_out)
        *col_out = col;
    float q[3] = { b->min[0] + CELL_W * (float)col + 2.5f, b->min[1], GROUND_H + STOREY_H * (float)b->n - 0.3f };
    return building_hit(b, q, radius);
}

/* 0x2BD3B0: a floor has come down onto the one below (only a floor's own collapse, not the destruction's): the one
 * below is crushed by the weight of what fell, by the HP the cells on it have left */
static void landed(Building *b, int k)
{
    static const int CRUSH[11] = { 0, 0, 1, 1, 2, 2, 2, 6, 6, 6, 6 };   /* 0x3F5710 */
    if (b->state != BUILDING_FLOOR_FALLING)
        return;
    b->state = BUILDING_STANDING;
    int above = k - 1, below = k - 2, front = 2 * b->depth;
    if (below < 0)
        return;
    for (int c = 0; c < b->width && b->state == BUILDING_STANDING; c++) {
        int amt = above > 0 && above < b->n ? CRUSH[b->cell[b->live[above]][front + c].hp] : 2;
        cell_damage(b, below, c, amt, 0, 0);
    }
    if (above < b->n)
        for (int c = 0; c < b->width && b->state == BUILDING_STANDING; c++)
            if (b->cell[b->live[above]][front + c].hp >= 5)
                cell_damage(b, above, c, 2, 0, 0);
}

/* 0x2D2D00 / 0x2D2A40: the falling storey squashes as the ones above come down 3 per storey of progress */
static void collapse_step(Building *b, float dt)
{
    if (b->collapse < 0)
        return;
    shake_more(b);
    b->progress += dt / b->collapse_time;
    if (b->collapse == 0) {                          /* sinking into the ground, 7 units */
        if (b->progress >= 7.0f / 3) {
            b->progress = 7.0f / 3;
            b->collapse = -1;
            b->state = BUILDING_GONE;
            b->max[2] = b->min[2];
        } else {
            b->max[2] = b->min[2] + GROUND_H + ROOF_H - STOREY_H * b->progress;
        }
        b->dirty = 1;
        return;
    }
    if (b->progress < 1) {
        b->max[2] = b->min[2] + GROUND_H + STOREY_H * (float)b->n + ROOF_H - STOREY_H * b->progress;
        b->dirty = 1;
        return;
    }
    int k = b->collapse, s = k - 1;                  /* the storey is gone: the ones above take its place */
    if (s < b->n) {
        memmove(&b->live[s], &b->live[s + 1], sizeof *b->live * (size_t)(b->n - s - 1));
        memmove(&b->vert[s], &b->vert[s + 1], sizeof *b->vert * (size_t)(b->n - s));
        b->n--;
    }
    b->collapse = -1;
    b->progress = 0;
    b->max[2] = b->min[2] + GROUND_H + STOREY_H * (float)b->n + ROOF_H;
    b->dirty = 1;
    landed(b, k);
}

/* 0x2BD7D0: one step of the destroyed building's front cracking, bottom row first. A cell it raises gets a debris
 * effect (Glass_Large or Wall_Small by its Outline, as a first crack) when a random number mod (gWorld +0x28 + 1) is 0,
 * that member being 1: the world's init sets it (0x285A20, from RWorld's 0x2B7800) and its setter (vtable +0x14,
 * 0x284AF0) has no caller; so half the time [R 0x2BDA94..0x2BDB08] */
static int crack_step(Building *b)
{
    int cols = b->width + 1;
    for (int j = 0; j <= b->n; j++) {
        int start = (int)(rnd(b) % (uint32_t)cols);
        for (int i = 0; i < cols; i++) {
            int v = (start + i) % cols;
            if (b->vert[j][FACE_FRONT][v].level)
                continue;
            building_set_level(b, FACE_FRONT, j, v, (int)(1 + rnd(b) % 3));
            if (j > 0)                               /* the cells either side of it, in the storey below the row */
                for (int c = v - 1; c <= v; c++) {
                    int ci = building_cell_index(b, FACE_FRONT, c);
                    if (ci >= 0 && b->cell[b->live[j - 1]][ci].state < b->min_crack) {
                        if (rnd(b) % (1 + 1) == 0)
                            debris(b, b->live[j - 1], c, DEBRIS_CRACK);
                        set_state(b, b->live[j - 1], c, b->min_crack);
                    }
                }
            return 1;
        }
    }
    return 0;
}

/* setCellDamage (0x2BB580 -> 0x2CFDA0) [R]: a front cell set above 0 first drops its open windows (both LRPos), then
 * the one cell's state (+0x0) is written and the grid marked for its meshes; its HP (+4) is left as it was (0x2CFDA0
 * writes only the state and, when given, the sheet column +5) */
void building_set_cell(Building *b, int face, int o, int col, int state)
{
    if (state < 0 || state > 3)
        return;
    if (face == FACE_ROOF) {                         /* 0x2CFDA0: one state for the whole roof, G+0x658 */
        b->roof = state;
        b->dirty = 1;
        return;
    }
    if (o < 0 || o >= BUILDING_STOREYS)
        return;
    int i = building_cell_index(b, face, col);
    if (i < 0)
        return;
    for (int lr = 0; lr < 2 && face == FACE_FRONT && state > 0; lr++) {
        int k = window_find(b, o, col, lr);
        if (k >= 0) {
            b->cell[o][i].flags &= (uint16_t)~(CELL_WINDOW_OPEN << lr);
            window_drop(b, k);
        }
    }
    b->cell[o][i].state = (uint8_t)state;
    b->dirty = 1;
}

void building_destroy(Building *b)
{
    if (b->state == BUILDING_STANDING || b->state == BUILDING_FLOOR_FALLING)
        destroyed(b);
}

void building_update(Building *b, float dt)
{
    if (dt <= 0)
        return;
    if (b->shake_time > 0) {
        b->shake_time -= dt;
        b->shake_t += dt;
    }
    collapse_step(b, dt);
    switch (b->state) {
    case BUILDING_CRACKING:
        b->timer += dt;
        while (b->timer > 0 && b->state == BUILDING_CRACKING) {
            if (!crack_step(b)) {                    /* the whole front has cracked: it shakes for 2 s */
                b->state = BUILDING_SHAKING;         /* (its foot's dust, a rumble for the monsters on it) */
                b->timer = -2.0f;
                b->pre_collapse = 1;
                break;
            }
            b->timer -= b->crack_time;
        }
        break;
    case BUILDING_SHAKING:
        b->timer += dt;
        if (b->shake_time <= 0)
            shake_more(b);
        if (b->timer > 0) {
            b->state = BUILDING_FALLING;
            b->timer = 1e10f;                        /* the first storey goes at once */
        }
        break;
    case BUILDING_FALLING:                           /* 0x2BD610: storey by storey from the bottom, then it sinks */
        if (b->collapse >= 0)
            break;
        b->timer += dt;
        if (b->timer >= b->hesitation) {
            b->timer = 0.5f * b->hesitation * rnd01(b);
            floor_falls(b, b->n >= 1 ? 1 : 0);       /* (Floor_Large dust along the front) */
        }
        break;
    default:
        break;
    }
    for (int k = 0; k < b->nwindows; k++) {          /* the windows opening and shutting (0x2BDDD0 ..) */
        BuildingWindow *w = &b->window[k];
        if (w->rate == 0)
            continue;
        w->open += w->rate * dt;
        if (w->rate > 0) {
            if (w->open >= 1) {
                w->open = 1;
                w->rate = 0;
            }
        } else if (w->open <= 0) {                   /* shut: out of the list, its flag cleared (0x2CF890) */
            int i = building_cell_index(b, FACE_FRONT, w->column);
            if (i >= 0 && w->o >= 0 && w->o < BUILDING_STOREYS)
                b->cell[w->o][i].flags &= (uint16_t)~(CELL_WINDOW_OPEN << w->lr);
            window_drop(b, k--);
        }
    }
}

int building_window_open(Building *b, int o, int column, int lr, int open, float seconds, const float rect[4], int opens_h)
{
    int i = building_cell_index(b, FACE_FRONT, column);
    if (o < 0 || o >= BUILDING_STOREYS || i < 0 || lr < 0 || lr > 1 || building_live_of(b, o) < 0)
        return 0;
    int k = window_find(b, o, column, lr);
    if (k >= 0) {                                    /* already there: on its way open or shut */
        BuildingWindow *w = &b->window[k];
        if (seconds <= 0.001f) {
            w->open = open ? 1.0f : 0.0f;
            w->rate = 0;
        } else {
            w->rate = (open ? 1.0f : -1.0f) / seconds;
        }
        return 1;
    }
    if (!open || b->cell[o][i].state != 0 || !rect || b->nwindows >= BUILDING_WINDOWS)
        return 0;
    BuildingWindow *w = &b->window[b->nwindows++];
    w->o = o;
    w->column = column;
    w->lr = lr;
    w->opens_h = opens_h != 0;
    b->cell[o][i].flags |= (uint16_t)(CELL_WINDOW_OPEN << lr);
    if (seconds <= 0.001f) {
        w->open = 1;
        w->rate = 0;
    } else {
        w->open = 0;
        w->rate = 1 / seconds;
    }
    w->x0 = rect[0];                                 /* +0xC .. +0x18 (here in the cell, not the world) */
    w->w = rect[2] - rect[0];
    w->z0 = rect[1];
    w->h = rect[3] - rect[1];
    return 1;
}

int building_window_is_open(const Building *b, int o, int column, int lr)
{
    int k = window_find(b, o, column, lr);
    return k >= 0 && b->window[k].open >= 1;
}

void building_storey(const Building *b, int s, float *bottom, float *height)
{
    float drop = b->collapse > 0 && s >= b->collapse ? STOREY_H * b->progress : 0;
    *bottom = GROUND_H + STOREY_H * (float)s - drop - building_sink(b);
    *height = b->collapse > 0 && s == b->collapse - 1 ? STOREY_H * (1 - b->progress) : STOREY_H;
}

float building_sink(const Building *b)
{
    if (b->collapse == 0)
        return STOREY_H * b->progress;
    return b->state == BUILDING_GONE ? STOREY_H * 7.0f / 3 : 0;
}

int building_live_of(const Building *b, int o)
{
    for (int s = 0; s < b->n; s++)
        if (b->live[s] == o)
            return s;
    return -1;
}

/* ---------------------------------------------------------------- the building light */

void building_light_init(BuildingLight *l, const float ambient[3], int nlights, const float (*dir)[3],
                         const float (*colour)[3], float ramp_start, float ramp_end, float ramp_intensity)
{
    memset(l, 0, sizeof *l);
    memcpy(l->ambient, ambient, sizeof l->ambient);  /* +0x2EC: the light file's ambient (0x2B9518) */
    for (int i = 0; i < 3 && i < nlights; i++) {     /* 0x2B93E8 ..: the first three lights; the rest stay black */
        float d2 = dir[i][0] * dir[i][0] + dir[i][1] * dir[i][1] + dir[i][2] * dir[i][2];
        if (d2 < 1e-6f) {                            /* (1, 0, 0) when shorter than 1e-3 (0x2B9438) */
            l->dir[i][0] = 1;
        } else {
            float k = 1 / sqrtf(d2);
            for (int c = 0; c < 3; c++)
                l->dir[i][c] = dir[i][c] * k;
        }
        memcpy(l->colour[i], colour[i], sizeof l->colour[i]);
    }
    l->ramp_start = ramp_start;                      /* +0x27C .. +0x284, and +0x288 (0x2B9570 .. 0x2B9620) */
    l->ramp_end = ramp_end;
    l->ramp_intensity = ramp_intensity;
    l->ramp_k = ramp_end != ramp_start ? ramp_intensity / (ramp_end - ramp_start) : 0;
}

float building_light_ramp(const BuildingLight *l, float z)
{
    if (z <= l->ramp_start)                          /* 0x2B8C40's first lines */
        return 0;
    if (z < l->ramp_end)
        return l->ramp_k * (z - l->ramp_start);
    return l->ramp_intensity;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

void building_light_at(const BuildingLight *l, const float p[3], const float n[3], float rgb[3])
{
    float r = building_light_ramp(l, p[2]);
    for (int c = 0; c < 3; c++)
        rgb[c] = l->ambient[c];                      /* +0x2EC .. +0x2F4 */
    for (int i = 0; i < 3; i++) {                    /* the lights +0x2A4 / +0x2C8, +0x2B0 / +0x2D4, +0x2BC / +0x2E0 */
        float d = -(n[0] * l->dir[i][0] + n[1] * l->dir[i][1] + n[2] * l->dir[i][2]);
        if (d < 0)
            d = 0;
        for (int c = 0; c < 3; c++)
            rgb[c] += d * l->colour[i][c];
    }
    for (int c = 0; c < 3; c++)                      /* the ramp onto every channel, then 0 .. 1 */
        rgb[c] = clamp01(rgb[c] + r);
}

void building_light_ambient(const BuildingLight *l, float z, float rgb[3])
{
    float r = building_light_ramp(l, z);
    for (int c = 0; c < 3; c++)
        rgb[c] = clamp01(l->ambient[c] + r);
}

void building_light_grid(const Building *b, const BuildingLight *l, const ShadowFacade *f, const ShadowLighting *sl,
                         uint8_t faces[3], BuildingLightRow *out)
{
    static const float NORMAL[3][3] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 } };   /* 0x430EE0 */
    uint8_t test[3] = { 0, 0, 0 };                   /* the faces shaded last time (none without a shadow, 0x2CE360) */
    if (f)
        memcpy(test, faces, sizeof test);
    memset(faces, 0, 3);
    float cx = (b->min[0] + b->max[0]) / 2, cy = (b->min[1] + b->max[1]) / 2;   /* the grid's matrix, +0x24 / +0x28 */
    for (int r = 0; r <= b->n; r++) {
        float z, h;
        building_storey(b, r, &z, &h);               /* 6 + 3 x its slot, lower by the falling storey's progress */
        for (int face = 0; face < 3; face++) {
            int count = face == FACE_FRONT ? b->width : b->depth, shaded = 0;
            for (int i = 0; i <= count && i < BUILDING_VCOLS; i++) {
                float p[3];                          /* where the vertex is before its dent (0x2CE504 .. 0x2CE600) */
                if (face == FACE_LEFT) { p[0] = -2.5f * (float)b->width; p[1] = 2.5f * (float)(b->depth - 2 * i); }
                else if (face == FACE_RIGHT) { p[0] = 2.5f * (float)b->width; p[1] = 2.5f * (float)(2 * i - b->depth); }
                else { p[0] = 2.5f * (float)(2 * i - b->width); p[1] = -2.5f * (float)b->depth; }
                p[0] += cx;
                p[1] += cy;
                p[2] = z;
                float *rgb = out[r][face][i];
                building_light_at(l, p, NORMAL[face], rgb);
                if (f && test[face] && face == FACE_FRONT)          /* 0x31DA60 on S itself */
                    shaded |= shadow_facade_front(f, sl, p, rgb);
                else if (f && test[face] && face == FACE_RIGHT)     /* 0x31D7F0 on S + 0x88 */
                    shaded |= shadow_facade_right(f, sl, p, rgb);
            }
            faces[face] |= (uint8_t)shaded;          /* 0x2CE998 */
        }
    }
}
