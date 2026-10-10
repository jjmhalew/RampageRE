/* reveal.h - the monster reveal (the cryotube: which monster it holds, where its tubes are, what startReveal unlocks,
 * the monster's walk-on, the city's "All Monsters Revealed" and Boosy) and findPointInRubble (the block left behind's
 * rubble, where ResultsNI puts its fires). docs/EDIBLE_NOTES.md "The cryotube", docs/FLOW_NOTES.md "The rubble".
 *
 * [R] What the game has, and what this is the port of:
 *   - the reveal object at 0x4D6A10: +0x00 four RevealLocs of 16 bytes { Block, Building, floor, cell }, +0x40 the
 *     monster to reveal, +0x44 how many were revealed (-1: none to reveal), +0x48 how many there are (0 or 1), +0x4C
 *     the RevealLocs, +0x50 tubes placed in this block, +0x54 the slot shown, +0x58 the monster revealed, +0x5C
 *     wasMonsterRevealed's countdown, +0x60 the monster is shown (one player);
 *   - its load at the world's start (0x3236F0 from 0x2B6BE0), the tubes at each block's activation (0x3235F0 from
 *     0x304150, after the edibles 0x31A540), gWorld.startReveal (World id 0x31, 0x2AED78 -> 0x323520), stopReveal
 *     (id 0x32, 0x3234D0), wasMonsterRevealed (id 0x47, +0x5C != 0), the Continue's undo (0x323450 from 0x3250B0),
 *     the reset (0x323CB0 from 0x2B8040), "all revealed" (0x323240) and Boosy at the block's end (0x3255B0);
 *   - the revealed monster's appearance (0x2D40B0: "%s::onRevealNITick", else "Monsters::onRevealNITick") and
 *     Monsters.bsf's onRevealNITick / onRevealNIEnd;
 *   - findPointInRubble (World id 0x38, 0x2AEFE8 -> 0x303670) over the block manager's rubble list (0x4D4E98, built
 *     by 0x303C90 from 0x302AF0, activateNextBlock).
 *
 * Engine-agnostic: the host gives the books' groups, the monster roster and its flags, the players' monsters; it gets
 * back the monster indices, the tubes' cells, and what the revealed monster does (events). Positions are the game's:
 * z up, the street at z = 0, the camera at -y; the rubble list is in the world as the game has it after
 * activateNextBlock (the block left behind at -41 in y). */
#ifndef RAMPAGE_REVEAL_H
#define RAMPAGE_REVEAL_H
#include <stdint.h>
#include "data.h"

#define REVEAL_LOCS 4                                /* [R] 0x323B14: at most 4 RevealLoc */
#define REVEAL_MONSTERS 64
#define REVEAL_UNLOCKED 0x80u                        /* [R] GameStatus +0x234 + 4 x monster: 0x80 = selectable
                                                      * (MonsterSelect.isMonsterAvailable: flags & 128) */

/* ---- the roster: GameStatus +0x10 (12-byte records, the name at +8), +0x18 the count; 0x33DAF0 finds a name in any
 * case. Its order is ShellData's MonsterData Monster0 .. Monster<NumMonsters - 1> (George, Lizzie, Ralph, ...,
 * Boosy 29), as MonsterSelect.getMonsterName reads it [G: the table's own loader was not read] */
typedef struct {
    const char *name[REVEAL_MONSTERS];
    int n;
    uint32_t flags[REVEAL_MONSTERS];                 /* GameStatus +0x234: the profile's, REVEAL_UNLOCKED and the
                                                      * upgrades 1 / 2 / 4 / 8 (challenge.h); the host seeds them */
} RevealRoster;
/* ShellData's MonsterData group: Monster|Monster<i> { Name } for i < NumMonsters. Returns the count */
int reveal_roster_load(RevealRoster *r, Group monster_data);
int reveal_roster_find(const RevealRoster *r, const char *name);   /* 0x33DAF0: the index, -1 */

/* ---- the reveal (0x4D6A10) */
typedef struct { int block, building, floor, cell; } RevealLoc;   /* -1 for a key that is missing */
typedef struct {
    RevealLoc loc[REVEAL_LOCS]; int nlocs;           /* +0x00, +0x4C */
    int monster;                                     /* +0x40: the monster to reveal (roster index) */
    int revealed;                                    /* +0x44: -1 none to reveal, else 0 or 1 */
    int count;                                       /* +0x48: 0 or 1 */
    int placed;                                      /* +0x50: tubes placed in the current block */
    int shown_slot, shown;                           /* +0x54 the monster slot shown, +0x58 the monster revealed
                                                      * (-1 none) */
    int recent;                                      /* +0x5C: 2 at a reveal, -1 at each block's activation */
    int show;                                        /* +0x60: the revealed monster comes out (one player) */
} Reveal;

void reveal_reset(Reveal *r);                        /* 0x323CB0: nothing to reveal */
/* 0x3236F0, the world's start. Only in the campaign (play type 2): with one player the monster will be shown. Each
 * record of WorldData's MonsterRevealDef in order:
 *   - RevealDef| { Monster = name  - name ... }, while none was chosen: its Monster is taken when no player plays it
 *     (any case), the roster has it, it is not REVEAL_UNLOCKED, and its list names player 1's monster or "everyone"
 *     (any case). Then: revealed = 0, count = 1, and the slot after the players (GameStatus +0x214 + 4 x players)
 *     gets that monster: the world makes players + 1 monsters (0x2B6BE0 / 0x2B72B0), the last one hidden and out of
 *     the play (+0x108 = +0x109 = 1) until startReveal;
 *   - RevealLoc| { Block, Building, cell, floor } (ints, -1 when missing), at most 4.
 * players_monster[i]: player i's roster index (GameStatus +0x214 + 4 i). Returns the monster to reveal, -1 none */
int reveal_load(Reveal *r, Group world_data, int campaign, int players, const int *players_monster,
                const RevealRoster *roster);
/* the slot that holds the monster to reveal (players), -1 when no monster is to be shown */
int reveal_extra_slot(const Reveal *r, int players);

/* 0x3235F0, a block's activation (after the edibles' setup): wasMonsterRevealed's countdown goes on (2 -> 1 -> 0),
 * and while the monster is still to be revealed (revealed < count) each RevealLoc of this block (Block = the
 * BlockList index, game.h's game_block) whose Building is below nbuildings (the block's building list, the edibles'
 * building index) gets a tube: edible_add_cryotube(e, Building, floor, cell). Fills out (max), returns how many */
int reveal_block(Reveal *r, int block, int nbuildings, RevealLoc *out, int max);

/* gWorld.startReveal(pos) (0x323520 and the binding 0x2AED78), from Cryotube.bsf when the tube lands. Only when a
 * tube was placed in this block (placed > 0): roster flags |= REVEAL_UNLOCKED for the monster, revealed + 1,
 * recent = 2, placed = 0; with one player (show) the slot `players + revealed` (the extra one) is shown at pos
 * (*show_slot >= 0: the host plays reveal_walk on it), else stopReveal at once. Returns the monster (shown: the
 * banner's getMonsterName); a block with no tube returns the last one (-1 if none) and changes nothing */
int reveal_start(Reveal *r, int players, RevealRoster *roster, int *show_slot);
/* gWorld.stopReveal (0x3234D0, Monsters::onRevealNIEnd): returns the slot to hide (0x2D3F90: +0x109 = 1, taken out
 * of the world), -1 none; shown_slot = shown = -1 */
int reveal_stop(Reveal *r);
int reveal_was_revealed(const Reveal *r);            /* gWorld.wasMonsterRevealed: recent != 0 (Results' banner) */
/* 0x323450, the game over's Continue (0x3250B0) when wasMonsterRevealed: revealed - 1 (the tube comes back), the
 * shown monster hidden (returned, -1 none), recent = 0. The roster's flag stays [R: not cleared here] */
int reveal_continue(Reveal *r);

/* 0x323240: every RevealDef's Monster of WorldData's MonsterRevealDef is REVEAL_UNLOCKED (a city without one: 1).
 * At each block's end (0x3255B0: the city done, or isAtCheckpoint, which is always 1) a 1 sets the city's flag 8 */
int reveal_all_revealed(Group world_data, const RevealRoster *roster);
/* 0x3255B0 after flag 8: Boosy (0x434098) becomes REVEAL_UNLOCKED when every city's flags (GameStatus +0x100 +
 * 4 i, i < ncities; the current city's | 8) have 1, 2, 4 and 8. Returns 1 if it unlocked him now */
int reveal_boosy(RevealRoster *roster, const uint32_t *city_flags, int ncities, int current);

/* ---- the revealed monster's walk-on (0x2D40B0, Monsters::onRevealNITick on a 0.25 s object timer) */
enum {
    RW_SHOW,     /* slot appears at pos facing pi (setFacingZ, 0x1015B0), +0x109 = 0, added to the world */
    RW_AI,       /* setIsAI(1) */
    RW_MOVE,     /* playMove(name, "", "") */
    RW_SCALE,    /* setDynamicScale(v[0], over v[1] seconds) */
    RW_WALK,     /* walkTo(pos, radius v[0], "", "Monsters::onRevealNIEnd"): call reveal_walk_arrived when there */
    RW_SPEED,    /* setSpeedScale(v[0], v[1] seconds) */
    RW_HIDE      /* stopReveal: the slot hides (0x2D3F90) */
};
typedef struct { int type, slot; const char *name; float pos[3], v[2]; } RevealEvent;
typedef struct {
    int slot, active, timer;                         /* timer: revealTimerHandle is valid */
    float time, clock;                               /* revealTime; the seconds to the next tick */
    RevealEvent ev[16]; int nev;
} RevealWalk;
/* the tube landed (reveal_start gave a slot): RW_SHOW, then onRevealNITick's first call: RW_AI, "Ground_Idle",
 * scale 0.1 at once then 1.0 over 1 s */
void reveal_walk_start(RevealWalk *w, int slot, const float pos[3]);
/* the timer's ticks, every 0.25 s [G: the first 0.25 s after the start]: revealTime 0.5 (the 3rd tick, 0.75 s)
 * "Ground_Taunt"; 4.0 (4.25 s): walkTo (100, -10, 0), x negated when the monster is left of the camera, radius 1,
 * speed 1.5 for 100000 s; 5.0 (5.25 s): the timer is removed. monster_x: its x now; camera_x: getCameraPos().x */
void reveal_walk_update(RevealWalk *w, float dt, float monster_x, float camera_x);
/* the walk's end callback (Monsters::onRevealNIEnd): gWorld.stopReveal, RW_HIDE for the slot reveal_stop returns */
void reveal_walk_arrived(RevealWalk *w, Reveal *r);
int reveal_walk_events(RevealWalk *w, RevealEvent *out, int max);   /* takes the events since the last call */

/* ---- findPointInRubble (0x303670) over the block left behind (0x303C90) */
#define RUBBLE_SLOTS 16                              /* [R] room for 16 between 0x4D4E98 + 4 and the GroundShape at
                                                      * +0x104 (no bound is checked) */
typedef struct {
    struct { float pos[3], min[3], max[3]; } s[RUBBLE_SLOTS];   /* +4 .. +0xC the place, +0x10 the RubbleShape */
    int n;                                           /* 0x4D4E98 */
    uint32_t seed;                                   /* gWorld +0x828 (0x106BD0) */
} Rubble;
typedef struct {
    Group (*tmpl)(void *ctx, const char *name);      /* a building template by TemplateName (0x285710) */
    /* the shape's bounds in its own frame (its +0x20 .. +0x34, its parts' boxes: 0x193B80) [R]; 0 = ok */
    int (*shape_box)(void *ctx, const char *shape, float min[3], float max[3]);
    void *ctx;
} RubbleHost;
void rubble_clear(Rubble *r);
void rubble_seed(Rubble *r, uint32_t seed);
/* 0x303C90(list, the BlockList entry left behind, had_previous): emptied; with had_previous (0x302AF0: there was a
 * block before and it is not the new one) each BuildingInstance| of that BlockDef with LeftEdgeXPos in [-50, 50)
 * and a template RubbleShape adds { x = LeftEdgeXPos + 2.5 x Width (the template's), y = -36 (+ the BlockDef's
 * BackRowOffset when Row != 0), z = 0 } with the shape's box. Returns the slots */
int rubble_load(Rubble *r, Group block_left, int had_previous, const RubbleHost *host);
/* gWorld.findPointInRubble(pos, radius, &out): the slots whose box (+ place) meets the sphere (0x100810: the box's
 * nearest point within radius); none: 0. One of them at random ((int)((n - 0.01) x u)), a point in it half a unit in
 * from its sides (x, y), at its bottom (min z); when that is farther than radius from pos it is pulled in onto the
 * sphere. u: 0x106BD0 (16807 x seed mod 2^31 - 1) x 2^-31, three draws. 1 = found */
int rubble_find_point(Rubble *r, const float pos[3], float radius, float out[3]);

#endif
