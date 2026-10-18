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
#include "shadow.h"

#define BUILDING_STOREYS 48
#define BUILDING_CELLS 48                            /* 2 x depth + width columns per storey */

enum { FACE_LEFT = 0, FACE_RIGHT = 1, FACE_FRONT = 2, FACE_GROUND = 4, FACE_ROOF = 5 };
enum {                                               /* +0x14C */
    BUILDING_GONE = 0,                               /* flattened: only its rubble is left */
    BUILDING_STANDING = 1, BUILDING_FLOOR_FALLING = 3,
    BUILDING_CRACKING = 4, BUILDING_SHAKING = 5, BUILDING_FALLING = 6   /* destroyed, in that order */
};

/* +0xE: FloorFlags (0x2CC014, the list in the Floor| order left, front, right) and the state bits: 1 the cell is
 * mirrored (its tile's u, its glass's and its Openables' x: 5 - x), 0x100 << LRPos its Openable window is open */
enum { CELL_MIRRORED = 0x1, CELL_WINDOW_OPEN = 0x100 };

typedef struct {
    uint8_t state, hp, tile;                         /* +0 damage state, +4 HP (10), +5 the sheet's column */
    uint8_t room;                                    /* +6 its InnerWindow tile 0 .. 15, drawn once (city.c) */
    uint16_t flags;                                  /* +0xE: CELL_MIRRORED, CELL_WINDOW_OPEN << LRPos (only these) */
} BuildingCell;

/* an open window (a record of 0x24 bytes in the building's list at +0xFC, count +0x104; openWindow 0x2BE8B0): one of
 * a front cell's Openable windows (the sheet's ColumnDef| Openable| entries, LRPos 0 or 1), open by `open`, 0 .. 1,
 * which runs at `rate` per second (the update 0x2BDC70). Drawn (0x2BECC0) as a near-black quad over the facade: from
 * the rect's bottom left, `open` of its width (OpensH) or of its height. */
#define BUILDING_WINDOWS 128                         /* (the game grows its list; it draws the first 40) */
typedef struct {
    int o, column, lr;                               /* +4 the storey (here template storey o), +0 column, +8 LRPos */
    int opens_h;                                     /* +9 OpensH: it opens sideways, else upwards */
    float x0, z0, w, h;                              /* +0xC .. +0x18: its rect in the cell (x from its left, z up) */
    float open, rate;                                /* +0x1C, +0x20 */
} BuildingWindow;

/* a grid vertex (32 bytes in the game): its dent level 0 .. 3 (+0x0's alpha / 85) and how far the dent moved it
 * along its face (+0x14, left to right seen from outside) and up (+0x18); per vertex row, left D + 1 (back to front),
 * right D + 1 (front to back), front W + 1 (left to right) */
typedef struct { uint8_t level; float along, up; } BuildingVert;
#define BUILDING_VCOLS (BUILDING_CELLS + 1)

/* a cell's debris effect: the key of the building's props it names (the key table 0x3F5990, indices 0x34 .. 0x36) */
enum {
    DEBRIS_CRACK,         /* Glass_Large (0x34) when the cell's sheet column has an Outline (its +0xEC, 0x2D0500), else
                           * Wall_Small (0x35): the host decides, it has the outlines */
    DEBRIS_WALL_SMALL,    /* Wall_Small (0x35) */
    DEBRIS_WALL_LARGE     /* Wall_Large (0x36) */
};
#define BUILDING_DEBRIS 64

/* a Scoring call (0x2BA4B0) the building made, for the host (Building.score) */
enum {
    BSCORE_CELL,          /* event 5: a front cell Building::hit itself took HP off (0x2BC7A0 .. 0x2BC834), with the
                           * cell's state after the hit; not the spread's cells nor the crush's (0x2BB950 alone) */
    BSCORE_FLOOR,         /* event 4: a storey's FloorHP gone, it comes down (0x2BBAD8) */
    BSCORE_BUILDING,      /* 0xF9: its HP gone (0x2BBA28) or collapseBuilding (0x2BB8D4) */
    BSCORE_NO_ONE         /* the one-hit cheat clears the attacker first (0x2BC208): no one's */
};
#define BUILDING_SCORES 32

typedef struct {
    float min[3], max[3];                            /* in the world (the grid's +0x24 .. +0x38); max z drops as it falls */
    int width, depth, storeys;                       /* storeys: as built */
    int row;                                         /* +0x10C: the block's row it stands in (0 the front row) */
    int n, live[BUILDING_STOREYS];                   /* the standing storeys, bottom up (G+0x18, G+0x48) */
    int state;                                       /* BUILDING_* */
    int hp, hp_max;                                  /* BuildingHP (+0x390, +0x394; default 6 x width x storeys) */
    int floor_hp[BUILDING_STOREYS];                  /* FloorHP per storey (floor +0x14; default 8 x width) */
    BuildingCell cell[BUILDING_STOREYS][BUILDING_CELLS];   /* [o][face x depth + column], fronts from 2 x depth */
    BuildingVert vert[BUILDING_STOREYS + 1][3][BUILDING_VCOLS];   /* by live row, face (FACE_*), column: the dents */
    int roof;                                        /* G+0x658: the roof's damage state 0 .. 3 (its TopFloorShapes model) */
    int collapse;                                    /* G+0x1CC: -1 none, k >= 1 live storey k-1 falls, 0 it sinks */
    float progress;                                  /* G+0x1D0: storeys fallen (0 .. 1; 0 .. 7/3 sinking) */
    float timer;                                     /* the destruction's clock (+0x150) */
    float collapse_time, crack_time, hesitation;     /* CollapseTime 0.25, CrackSpreadTime 0.05, HesitationTime 0.4 */
    int min_crack;                                   /* MinCollapseCrackLevel 1 */
    float shake_time, shake_length, shake_amp;       /* the shaker (+0x8AC): time left, ShakeLength, MasterAmp */
    float shake_t, shake_total;
    float shake[4][3];                               /* (period, phase, amp) */
    uint32_t seed;                                   /* Park-Miller (0x106BD0) */
    int no_autocollapse;                             /* disableAutoCollapse(1): its HP running out does not fell it */
    int dirty;                                       /* a cell or a storey changed: redraw */
    BuildingWindow window[BUILDING_WINDOWS]; int nwindows;   /* its open (and opening, closing) windows */
    /* the debris effects its front cells asked for since the host last took them (0x3101C0 on the building's effect
     * spawner +0x8F4, at the cell's face point 2.5 along and 1.5 up): the host plays them and sets ndebris to 0 */
    struct { uint8_t o, col, kind; } debris[BUILDING_DEBRIS]; int ndebris;
    /* and the ground floor's: Wall_Small at a hit's own point (0x2BC88C -> 0x30FCB0 -> 0x30FD40); the host plays them
     * and sets nhit_debris to 0 */
    float hit_debris[8][3]; int nhit_debris;
    /* the front has cracked, it shakes (0x2BD7D0 into state 5): the dust at its foot, Floor_Large at every other
     * column's edge on the ground (0x3101C0 with key 0x38, the point {4: the ground, along 5 x 1, 3, ..} on the
     * front) and Building_Pre_Collapse.edf 5 before its place (0x30FD40); the host plays them and clears it */
    int pre_collapse;
    /* the Scoring calls its hits and falls made, in order: the host pays them to the attacker (+0x190) and sets nscore
     * to 0 */
    struct { uint8_t event, state; } score[BUILDING_SCORES]; int nscore;
} Building;

void building_init(Building *b, const float min[3], const float max[3], int width, int depth, int storeys);
int building_cell_index(const Building *b, int face, int column);    /* -1 if out of range */
/* a hit at the world point p (on or near one of its faces) with radius r (2.0 from a monster's attack); 1 if the
 * front cell hit took damage (Building::hit's answer, s0 set only at 0x2BC838: not for the dents, a side face or a
 * cell already at 0). Only front cells take damage from hits, and only while it stands, as in the game. */
int building_hit(Building *b, const float p[3], float radius);
/* cheat 071767 (profile PCH_40000000): every hit fells a standing building (0x2BC1F0) */
void building_set_one_hit(int on);
/* hitBuildingRoof's hit (0x2BBE80), p the stomping bone in the world: the roof (roof) one state worse, up to 3, and
 * the top storey's front cell below p's x (or the nearest one not destroyed) hit at its middle, 0.3 under the storey's
 * top, by building_hit with radius r (2.0); 1 if that cell or its vertices took damage; *col the column hit, -1 none.
 * Only a standing building. */
int building_hit_roof(Building *b, const float p[3], float radius, int *col);
void building_destroy(Building *b);                 /* the collapseBuilding binding (0x2BB8B0): it falls */
/* setCellDamage(face, floor, column, state): the cell of template storey o (0 the first above the ground floor) on
 * that face to damage state 0 .. 3, its HP left as it was, that cell alone (a front cell above 0 drops its open
 * windows first, 0x2BB580); face 5 (the roof) sets the roof's state, whatever the storey and column (0x2CFDA0) */
void building_set_cell(Building *b, int face, int o, int col, int state);
void building_update(Building *b, float dt);         /* collapses, destruction, the shaker (0x2BDC70) */
float building_shake(const Building *b);             /* the sway along x now (0x2CC450) */
/* where live storey s stands now: its bottom and its height (a falling storey is squashed as the ones above come
 * down onto it); the drop of the whole body while it sinks into the ground */
void building_storey(const Building *b, int s, float *bottom, float *height);
float building_sink(const Building *b);
int building_live_of(const Building *b, int o);      /* storey o's live place, -1 if it has fallen */
/* 0x2CEAD0: a vertex's dent level; a new dent moves it 0.1 .. 0.45 (by level) in a random direction in its face's
 * plane, a deeper one further the same way; the front's corners are the sides' too */
void building_set_level(Building *b, int face, int row, int col, int level);

/* ---- the windows the occupants open (docs/FORMAT_BDP.md "Open windows") ------------------------------------------
 * openWindow(storey, column, LRPos, open, seconds) (Building binding 0x38, 0x2BE8B0) on front cell (o, column):
 * - a window already in the list runs to open (1) or shut (0) over `seconds` (at once if seconds <= 0.001);
 * - else, to open it: only while the cell is intact (state 0, 0x2CF7C0) and the column has that Openable (rect: x0, z0,
 *   x1, z1 in the cell, NULL none; opens_h its OpensH), a new one, shut (open at once if seconds <= 0.001), the cell's
 *   flag CELL_WINDOW_OPEN << lr set (0x2CF980); closing one that is not there does nothing.
 * Once shut it leaves the list and the flag goes (the update). A front cell set to a damaged state drops its windows
 * at once (0x2BB580), a storey that falls its own (0x2BAE70). 1 if a window was set going. */
int building_window_open(Building *b, int o, int column, int lr, int open, float seconds, const float rect[4], int opens_h);
/* isWindowOpen (binding 0x39, 0x2BE7C0): it is in the list and fully open */
int building_window_is_open(const Building *b, int o, int column, int lr);

/* ---- the building light (docs/BUILDING_LIGHT_NOTES.md) ----------------------------------------------------------
 * The buildings are lit on the CPU, not by the GS's lights: the lighting object (0x4D2680, filled by 0x2B92F0 from
 * the light file and WorldData.LightingData) gives a point with a normal its colour (0x2B8C40): the ambient, the three
 * directional lights and a height ramp (RampStart .. RampEnd, up to RampIntensity: brighter higher up by day, darker
 * by night), clamped 0 .. 1. The facade's grid vertices get it (0x2CE300), less a neighbour's shadow, and the facade
 * quads, cracks and the playable buildings' shaded models are drawn with it as their vertex colour. */
typedef struct {
    float ambient[3];                                /* +0x2EC: the light file's BEGIN_AMBIENT */
    float dir[3][3], colour[3][3];                   /* +0x2A4, +0x2C8: three lights, the way each travels (unit) */
    float ramp_start, ramp_end, ramp_intensity;      /* +0x27C .. +0x284: RampStart 6, RampEnd 35, RampIntensity */
    float ramp_k;                                    /* +0x288: RampIntensity / (RampEnd - RampStart) */
} BuildingLight;
/* 0x2B92F0: the light file's ambient and first three lights (dir: the way they travel, normalised here; fewer: the
 * rest black), and LightingData's ramp (day 6, 35, 0.35; night 6, 35, -0.5 in every city) */
void building_light_init(BuildingLight *l, const float ambient[3], int nlights, const float (*dir)[3],
                         const float (*colour)[3], float ramp_start, float ramp_end, float ramp_intensity);
float building_light_ramp(const BuildingLight *l, float z);   /* 0 to RampStart, rising to RampIntensity at RampEnd */
/* 0x2B8C40: the colour at world point p of a surface facing n (unit): ambient + ramp(p.z) + each light's colour x
 * max(-n.dir, 0), each channel clamped 0 .. 1 */
void building_light_at(const BuildingLight *l, const float p[3], const float n[3], float rgb[3]);
/* 0x2B8AD0: the ambient a model at height z is given for the GS's own lighting: ambient + ramp(z), clamped */
void building_light_ambient(const BuildingLight *l, float z, float rgb[3]);
/* 0x2CE300: every grid vertex's colour, by live vertex row 0 .. n, face (FACE_*), column (building.h order): the light
 * at its place before the dents (the row's height now, the face's normal), less the neighbours' shadow f casts on it
 * (the front face's vertices by shadow_facade_front, the right face's by shadow_facade_right; f NULL: none, as for
 * the blocks behind). faces[3] (G+0x1EC, all 1 at the start) says which faces may still be shaded: a face none of
 * whose vertices was shaded is not tested again (the shadows only ever shrink). */
typedef float BuildingLightRow[3][BUILDING_VCOLS][3];
void building_light_grid(const Building *b, const BuildingLight *l, const ShadowFacade *f, const ShadowLighting *sl,
                         uint8_t faces[3], BuildingLightRow *out);

#endif
