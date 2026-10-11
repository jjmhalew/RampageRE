/* game.c - the game flow of a city: the world state machine, the lives and respawns, the block clock, the score and
 * the results, the play modes (game.h; docs/GAME_NOTES.md).
 * [R] marks what is read from the game's code or scripts, [G] what is a guess or a stand-in. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"
#include "wrand.h"

#define PI 3.14159265f
#define MAX_EVENTS 256
#define MAX_BLOCKS 64
#define BONUS_ENTRIES 6                              /* [R 0x347E84: slti 6] */
#define STREET_Y (-10.0f)                            /* [R 0x2D3B50: startRespawn's 0x2D3510(-10.0)] */

/* the script functions the world runs (0x324180 finds them by name: the block's string property of that name, else
 * WorldData's, else the name itself; then the name as given, "<City>::<name>", "Levels::<name>") [R] */
enum {
    FN_NONE,
    FN_BEGIN_LEVEL, FN_CONTINUE_LEVEL, FN_BEGIN_BLOCK, FN_RESULTS, FN_SKIP_RESULTS, FN_END_BLOCK, FN_END_LEVEL,
    FN_END_WORLD,                                    /* Levels.bsf's NI functions */
    FN_IS_BLOCK_FINISHED,                            /* Levels::IsBlockFinished */
    FN_BOSS_FINISHED,                                /* <City>::isBossBlockFinished (Las Vegas .. San Francisco) */
    FN_NY_BOSS_FINISHED,                             /* NewYork::isBossBlockFinished */
    FN_NY_BOSS_RESULTS,                              /* NewYork::BossResultsNI */
    FN_HOST                                          /* another script: the host's, falling back to `fallback` */
};

typedef struct {
    int points[7], kills[7];                         /* +0x4, +0x20 of the player (0x4D7990 + 0x318 + 0x60 n) [R] */
    int cities_won, blocks_won, city_score;          /* +0x3C, +0x40, +0x44 [R 0x325888, 0x325858, 0x325710] */
} Player;

struct Game {
    GameHost host;
    GameConfig cfg;
    /* the city */
    char city[32];
    Group world;                                     /* <City>_Props WorldData */
    const char *blocks[MAX_BLOCKS]; int nblocks;     /* BlockData.BlockList, repeats kept (index 0 doubles 1) */
    int first, last, cur;                            /* 0x4D4E80 +0x14 BlockStart, +0x10 BlockEnd, +0x8 [R 0x303240] */
    int par_score;
    /* the state object 0x4D6A80 [R] */
    int state, entered;                              /* +0x0, +0x4 */
    float ni_freq, ni_timer;                         /* +0x8, +0xC */
    int ni, ni_fallback; char ni_name[64];           /* +0x10 the running NI */
    int fin, fin_fallback; char fin_name[64];        /* +0x14 IsBlockFinished */
    int level_done, game_over;                       /* +0x18, +0x19 */
    float dt;                                        /* this frame's (the world's +0x4, 0x4D17A4) */
    /* Levels.bsf's globals [R Levels.scr] */
    float ni_time, ni_frequency;
    int ni_move_done[GAME_PLAYERS], ni_move_step[GAME_PLAYERS];
    /* the clock 0x4D5190 [R 0x347DC0 .. 0x3483A0] */
    int bonus_time[BONUS_ENTRIES], bonus_points[BONUS_ENTRIES];   /* +0x0: 6 x { time, bonus } */
    float limit, start, stop, max_time;              /* +0x30 TimeLimit, +0x34, +0x38, +0x3C */
    int time_bonus;                                  /* +0x40 */
    int enabled, running, visible;                   /* +0x44 the block ends at 0, +0x45, +0x46 ClockVisible */
    float clock_limit;                               /* 0x4D7E48: the seconds the clock counts down from */
    double now;                                      /* the world's play time (gp-0x6CFC +0x30) */
    /* the game status 0x4D7990 [R] */
    int lives[GAME_PLAYERS];                         /* +0x224 */
    int start_block;                                 /* +0x210 (0x4D7BA0) */
    struct { int start_block, lives[GAME_PLAYERS]; } checkpoint;   /* +0x148 .. (0x33E290 / 0x33D820) */
    Player p[GAME_PLAYERS];
    int city_total;                                  /* 0x4D7C40 */
    uint32_t city_flags;                             /* 0x4D7C3C */
    int king_block, king_city, king_world;           /* 0x4D7E50, 0x4D7E54, 0x4D7E58 */
    int first_place, second_place;                   /* 0x4D7E5C, 0x4D7E60 */
    int city_destroyed, quit, select, world_done;    /* 0x4D7E4C, 0x4D7E4D, 0x4D7E4E, 0x4D7E4F */
    /* WorldGuiDriver.bsf */
    int result;                                      /* nResultState */
    int results_screen; float results_age, auto_results;
    /* the monsters */
    float hp[GAME_PLAYERS];
    int human[GAME_PLAYERS];                         /* +0xAB4: it became human (a streaker) */
    int buildings;
    int boss_finished;                               /* the city script's bIsBossBlockFinished */
    int centre_down, ny_watch, ny_moved;             /* NewYork::BossResultsNI: the centre building, the watch */
    int left;                                        /* GE_EXIT given: the world is being left, nothing runs */
    int button, button_end;                          /* gWorld +0x1B (0x4D6A9B): a pad button (game_attract_button);
                                                      * the demo ended by it */
    int weapons_on;                                  /* gp-0x772C (game_weapons_on) */
    GameEvent ev[MAX_EVENTS]; int nev;
};

static void step(Game *g);

/* ---------------------------------------------------------------- helpers */

/* gWorld.frand (TypeWorldScriptInterface 0x17 -> 0x106BD0) [R]: Park-Miller on gWorld +0x828, answered as s / 2^31;
 * the one state every script and findPointInRubble share (wrand.c) */
static float frand(Game *g)
{
    (void)g;
    return wrand_frand();
}

static GameEvent *emit(Game *g, int type, int slot)
{
    static GameEvent spare;
    GameEvent *e = g->nev < MAX_EVENTS ? &g->ev[g->nev++] : &spare;
    memset(e, 0, sizeof *e);
    e->type = type;
    e->slot = slot;
    return e;
}

static int same(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return *a == *b;
}

static int np(const Game *g) { return g->cfg.num_players; }

static int total_lives(const Game *g)
{
    int n = 0;
    for (int i = 0; i < np(g); i++)
        n += g->lives[i];
    return n;
}

static Group block_def(const Game *g, int index)     /* BlockDef|<name> of the BlockList's entry index [R 0x303160] */
{
    Group none = { NULL, 0 };
    if (!g->world.at || index < 0 || index >= g->nblocks)
        return none;
    char key[96];
    snprintf(key, sizeof key, "BlockDef|%s", g->blocks[index]);
    return group_sub(group_sub(g->world, "BlockData"), key);
}

static Group parent_of(const Game *g, Group grp)     /* ParentPropertySet = "Group:Book" (the block's DefBlockProps) */
{
    Group none = { NULL, 0 };
    const char *ref = grp.at ? group_str(grp, "ParentPropertySet") : NULL, *colon = ref ? strchr(ref, ':') : NULL;
    if (!colon || !g->host.find)
        return none;
    char name[64];
    size_t len = (size_t)(colon - ref) < sizeof name - 1 ? (size_t)(colon - ref) : sizeof name - 1;
    memcpy(name, ref, len);
    name[len] = 0;
    return g->host.find(g->host.ctx, colon + 1, name);
}

/* ---------------------------------------------------------------- the clock (0x4D5190) */

static float clock_left(const Game *g)               /* 0x347DC0 [R] */
{
    float t = g->running ? (float)g->now : g->stop;
    float left = g->clock_limit - (t - g->start);
    return left < 0 ? 0 : left;
}

static void clock_start(Game *g)                     /* 0x347EA0 [R] */
{
    g->start = (float)g->now;
    g->stop = 0;
    g->time_bonus = 0;
    g->running = 1;
}

static void clock_stop(Game *g)                      /* 0x347E10: the time bonus [R] */
{
    if (!g->running)
        return;
    g->running = 0;
    g->stop = (float)g->now;
    g->time_bonus = 0;
    int elapsed = (int)(g->stop - g->start);
    for (int i = 0; i < BONUS_ENTRIES; i++)          /* the most of the entries whose time is not reached yet */
        if (g->bonus_time[i] && elapsed < g->bonus_time[i] && g->time_bonus < g->bonus_points[i])
            g->time_bonus = g->bonus_points[i];
}

static void clock_reset(Game *g)                     /* 0x347EC0 [R] */
{
    g->start = g->stop = 0;
    g->running = 0;
}

static Group either(Group a, Group b, const char *key)   /* a's key, else b's */
{
    Group r = a.at ? group_sub(a, key) : a;
    return r.at || !b.at ? r : group_sub(b, key);
}

/* 0x347ED0, from the new block's TimeBonus (or its parent's): the bonus table, TimeLimit and ClockVisible of the
 * play type's group, and the seconds to count down [R] */
static void clock_block(Game *g)
{
    memset(g->bonus_time, 0, sizeof g->bonus_time);
    memset(g->bonus_points, 0, sizeof g->bonus_points);
    g->start = g->limit = g->max_time = 0;
    g->visible = 0;
    Group blk = block_def(g, g->cur), par = parent_of(g, blk);
    Group tb = blk.at ? group_sub(blk, "TimeBonus") : blk, ptb = par.at ? group_sub(par, "TimeBonus") : par;
    if (!tb.at && !ptb.at)
        return;                                      /* [R 0x347FB8]: the rest stays as it was */
    Group def = either(tb, ptb, "BonusDef");
    if (def.at) {
        Record r[16];
        int n = (int)group_items(def, r, 16);
        for (int t = 0, i = 0; t < 2 * BONUS_ENTRIES && t + 1 < n; t += 2, i++) {
            g->bonus_time[i] = (int)record_num(r[t]);
            g->bonus_points[i] = (int)record_num(r[t + 1]);
            if ((float)g->bonus_time[i] > g->max_time)
                g->max_time = (float)g->bonus_time[i];
        }
    }
    /* the play type's group: "Campaign", "King" (both King modes; the books call theirs KingOfCity, so it is never
     * found [R 0x3F5D44 "King"]), "TimedRun" */
    const char *mode = g->cfg.play_type == GP_CAMPAIGN ? "Campaign" :
                       g->cfg.play_type == GP_KING_CITY || g->cfg.play_type == GP_KING_WORLD ? "King" :
                       g->cfg.play_type == GP_TIMED_RUN ? "TimedRun" : NULL;
    Group m = mode ? either(tb, ptb, mode) : tb;
    if (mode && m.at) {
        g->limit = (float)(int)group_num(m, "TimeLimit", g->limit);
        g->visible = (int)group_num(m, "ClockVisible", (float)g->visible) != 0;
    }
    if (g->cfg.play_type == GP_TIMED_RUN) {          /* the time left carries on, plus this block's */
        g->clock_limit += g->limit;
        g->enabled = 1;
    } else if (g->cfg.play_type == GP_ATTRACT) {     /* a minute at most */
        g->clock_limit = 60;
        if (g->limit != 0 && g->clock_limit > g->limit)
            g->clock_limit = g->limit;
        g->enabled = 1;
        g->visible = 0;
    } else {
        g->enabled = g->limit >= 1;                  /* a time limit (the boss blocks: 600) */
        g->clock_limit = g->limit;
        if (!g->enabled) {                           /* none: the clock shows the bonus time running out */
            g->limit = g->max_time;
            g->visible = 1;
            g->clock_limit = g->limit;
        }
    }
}

/* ---------------------------------------------------------------- the game status (0x4D7990) */

static void clear_points(Player *p)                  /* 0x33F5F0 [R] */
{
    memset(p->points, 0, sizeof p->points);
    memset(p->kills, 0, sizeof p->kills);
}

static void status_reset(Game *g)                    /* 0x33DB90 [R] */
{
    for (int i = 0; i < GAME_PLAYERS; i++)
        clear_points(&g->p[i]);
    if (g->start_block != 0)
        return;
    for (int i = 0; i < GAME_PLAYERS; i++) {
        switch (g->cfg.play_type) {
        case GP_CAMPAIGN: g->lives[i] = 3; break;    /* [R 0x33DC6C] */
        case GP_KING_CITY: g->lives[i] = 0; g->p[i].blocks_won = 0; break;
        case GP_KING_WORLD:
            g->lives[i] = 0;
            g->p[i].blocks_won = 0;
            if (g->cfg.city_index == 0)
                g->p[i].cities_won = 0;
            break;
        case GP_TIMED_RUN: g->lives[i] = 0; break;
        default: break;
        }
    }
}

static void checkpoint_save(Game *g)                 /* 0x33E290: the campaign only [R] */
{
    if (g->cfg.play_type != GP_CAMPAIGN)
        return;
    g->checkpoint.start_block = g->start_block;
    memcpy(g->checkpoint.lives, g->lives, sizeof g->lives);
}

static void checkpoint_restore(Game *g)              /* 0x33D820: back to the checkpoint, three lives each [R] */
{
    g->start_block = g->checkpoint.start_block;
    for (int i = 0; i < GAME_PLAYERS; i++)
        g->lives[i] = 3;
}

static void take_life(Game *g, int slot)             /* 0x2D3950 / 0x2D37A4: a player out of lives takes the richest's */
{
    if (g->cfg.play_type != GP_CAMPAIGN || slot < 0 || slot >= GAME_PLAYERS)
        return;
    int k = slot;
    if (g->lives[k] <= 0)
        for (int i = 0, most = 0; i < np(g); i++)
            if (most < g->lives[i]) {
                most = g->lives[i];
                k = i;
            }
    if (g->lives[k] > 0)
        g->lives[k]--;
}

static void add_points(Game *g, int slot, int cat, int amount)   /* Player.addPoints, 0x33F530 [R] */
{
    if (slot < 0 || slot >= GAME_PLAYERS || cat < 0 || cat > 6)
        return;
    Player *p = &g->p[slot];
    p->points[cat] += amount;
    p->kills[cat]++;
    if (cat != GPT_TOTAL) {
        p->points[GPT_TOTAL] += amount;
        p->kills[GPT_TOTAL]++;
    }
}

/* ---------------------------------------------------------------- the monsters */

/* cancelRespawn (0x2D36A0): a defeated monster (or one turned human) comes back where it is, for a life; in the
 * campaign with no lives left between the players it stays down, hidden [R] */
static void cancel_respawn(Game *g, int slot, int full)
{
    if (!g->human[slot] && g->hp[slot] > 0)
        return;
    int sum = total_lives(g);
    take_life(g, slot);
    if (sum == 0 && g->cfg.play_type == GP_CAMPAIGN) {
        g->human[slot] = 1;                          /* +0xAB4 = 1, HP 0, object flag 0x20 off */
        g->hp[slot] = 0;
        return;
    }
    g->human[slot] = 0;
    g->hp[slot] = 1e9f;                              /* the most (MONSTER_DATA +0x138) until the host says */
    if (full >= 0)                                   /* -1: the caller tells the host itself */
        emit(g, GE_REVIVE, slot)->a = full;
}

/* ---------------------------------------------------------------- the blocks */

/* activateNextBlock (0x2B6580): with cheat 0x10000 (0x33D740) the world's random states start again from
 * GameStatus' seeds (+0x828 = seed[0], 0x1069C0(+0x82C, seed[1])); the world's block index (+0x878) goes on by one,
 * the start block follows it, the block manager makes it the current one (0x302AF0) and the clock reads its
 * TimeBonus (0x347ED0) [R] */
static void activate_next_block(Game *g, int in_game)
{
    if (g->cfg.cheats & 0x10000u) {
        wrand_seed_pm(g->cfg.seed);
        wrand_seed_lf(g->cfg.seed1);
    }
    g->cur++;
    g->start_block = g->cur;
    clock_block(g);
    GameEvent *e = emit(g, GE_BLOCK, -1);
    e->a = g->cur;
    e->b = in_game;                                  /* a1: 1 in the game (the old block torn down), 0 the reset */
    e->name = g->cur >= 0 && g->cur < g->nblocks ? g->blocks[g->cur] : NULL;
}

/* ---------------------------------------------------------------- Levels.bsf [R Levels.scr] */

static void setup_ni(Game *g, float freq)
{
    g->ni_time = 0;
    g->ni_frequency = freq;
    for (int i = 0; i < GAME_PLAYERS; i++)
        g->ni_move_step[i] = g->ni_move_done[i] = 0;
}

static void play_move(Game *g, int slot, const char *move)   /* Levels::playMove: moveComplete when it ends */
{
    if (slot == -1) {
        for (int i = 0; i < np(g); i++)
            play_move(g, i, move);
        return;
    }
    GameEvent *e = emit(g, GE_MOVE, slot);
    e->name = move;
    e->a = 1;
    emit(g, GE_AI, slot)->a = 1;
    g->ni_move_done[slot] = 0;
}

static void reset_at(Game *g, int slot, float x, float y, float facing)
{
    GameEvent *e = emit(g, GE_RESET, slot);
    e->a = 1;
    e->pos[0] = x;
    e->pos[1] = y;
    e->f = facing;
}

static void camera(Game *g, const char *mode, float blend)
{
    GameEvent *e = emit(g, GE_CAMERA, -1);
    e->name = mode;
    e->f = blend;
}

static void script_camera(Game *g, float lx, float ly, float lz, float tx, float ty, float tz)
{
    GameEvent *e = emit(g, GE_CAMERA, -1);
    e->name = "Script";
    e->a = 1;
    e->pos[0] = lx; e->pos[1] = ly; e->pos[2] = lz;
    e->v[0] = tx; e->v[1] = ty; e->v[2] = tz;
}

static float begin_level_ni(Game *g, float last)     /* BeginLevelNI = ContinueLevelNI: the intro */
{
    g->ni_time += last;
    if (last == 0) {
        setup_ni(g, 0.25f);
        float x = -17.0f * (float)(np(g) - 1) / 2.0f;
        for (int i = 0; i < np(g); i++, x += 17.0f) {   /* side by side at y -5.2, facing the camera */
            reset_at(g, i, x, -5.2f, PI);
            play_move(g, i, "React_Intro");
        }
        camera(g, "Free", 0);
        camera(g, "TwoTilts", 0);
    }
    int done = 0;
    for (int i = 0; i < np(g); i++)
        if (g->ni_move_done[i]) {
            g->ni_move_step[i]++;
            done++;
        }
    return done == np(g) ? 0 : g->ni_frequency;
}

static float begin_block_ni(Game *g, float last)     /* BeginBlockNI: the HUD, the AI and the camera; done at once */
{
    g->ni_time += last;
    if (last == 0) {
        setup_ni(g, 0.25f);
        for (int i = 0; i < np(g); i++) {
            emit(g, GE_RESET, i);
            GameEvent *e = emit(g, GE_AI, i);
            e->a = i >= np(g) - g->cfg.num_bots;
            e->b = e->a;                             /* the bots also destroyBuildings (b = 1: the host's DG_DESTROY) */
        }
        emit(g, GE_HUD, -1)->a = 1;
        emit(g, GE_GUI, -1)->name = "ChallengeText";
        emit(g, GE_AI_ACTIVATE, -1);
        camera(g, "TwoTilts", 0.5f);
        emit(g, GE_RUBBLE_FX_STOP, -1);
        return 0;
    }
    return g->ni_frequency;
}

static void rank(const Game *g, int type, int out[GAME_PLAYERS])   /* gCommon.getWinnersByType (0x2C4FF0) [R] */
{
    int score[GAME_PLAYERS];
    for (int i = 0; i < GAME_PLAYERS; i++) {
        out[i] = i;
        score[i] = type == 0 ? g->p[i].points[GPT_TOTAL] : type == 1 ? g->p[i].blocks_won : g->p[i].cities_won;
    }
    for (int i = 0; i < np(g); i++)                  /* highest first; ties keep their order */
        for (int j = i + 1; j < np(g); j++)
            if (score[j] > score[i]) {
                int t = score[i]; score[i] = score[j]; score[j] = t;
                t = out[i]; out[i] = out[j]; out[j] = t;
            }
}

static float results_ni(Game *g, float last)         /* ResultsNI: the winners pose by the rubble */
{
    g->ni_time += last;
    if (last == 0) {
        setup_ni(g, 0.25f);
        int w[GAME_PLAYERS];
        rank(g, 0, w);
        script_camera(g, 20, -75, 2, 5, -33, 8);
        reset_at(g, w[0], 13, -42, PI);
        emit(g, GE_AI, w[0])->a = 1;
        if (np(g) >= 2) {
            reset_at(g, w[1], 5, -38, PI * 0.75f);
            emit(g, GE_AI, w[1])->a = 1;
        }
        if (np(g) >= 3) {                            /* the second moves over (the script moves the last one placed) */
            GameEvent *e = emit(g, GE_PLACE, w[1]);
            e->pos[0] = 3;
            e->pos[1] = -38;
            script_camera(g, 25, -73, 2, 0, -33, 8);
            reset_at(g, w[2], -12, -32, PI * 1.25f);
            emit(g, GE_AI, w[2])->a = 1;
        }
        if (np(g) >= 4) {
            script_camera(g, 28, -73, 2, -2.5f, -33, 8);
            reset_at(g, w[3], -28, -24, PI * 1.25f);
            emit(g, GE_AI, w[3])->a = 1;
        }
        for (int i = 0; i < 3; i++) {                /* fires in the rubble around (0, -21): findPointInRubble(8); */
            GameEvent *e = emit(g, GE_RUBBLE_FX, -1);   /* the host draws (int)(2 frand) once a point is found */
            e->a = i;                                /* (Levels.scr 355 .. 358), from game_rng's state */
            e->pos[1] = 20.0f - 41.0f;
            e->f = 8;
        }
        emit(g, GE_HUD, -1)->a = 0;
        for (int i = 0; i < np(g); i++) {
            g->ni_move_step[i] = (int)(frand(g) * 15.0f);
            if (i == 0)
                play_move(g, 0, "React_Victory");
            else
                g->ni_move_done[i] = 1;
        }
    }
    for (int i = 0; i < np(g); i++)                  /* now and then a taunt, a victory or a punch at the ground */
        if (g->ni_move_done[i] && --g->ni_move_step[i] <= 0) {
            g->ni_move_step[i] = (int)(frand(g) * 60.0f);
            static const char *const moves[] = { "Ground_Taunt", "React_Victory", "Ground_Punch_Down" };
            play_move(g, i, moves[(int)(frand(g) * 3.0f) % 3]);
        }
    return g->ni_frequency;
}

/* NewYork::BossResultsNI [R NewYork.scr]: with the blimp beaten (flag 1) the monsters taunt on the Empire State's
 * roof, watched from the air, until the centre building falls; with the buildings gone (flag 2) Levels' ResultsNI.
 * It never ends by itself */
static float ny_boss_results_ni(Game *g, float last)
{
    if (g->boss_finished == 2)
        return results_ni(g, last);
    static const float AT[2][2] = { { 3, 15 }, { -3, 15 } }, FACE[2] = { -0.75f * PI, 0.75f * PI };
    g->ni_time += last;
    if (last == 0) {
        setup_ni(g, 0.25f);
        script_camera(g, 4, -55, 78, -3, 10, 57);
        int two = np(g) >= 2;
        for (int i = 0; i < (two ? 2 : 1); i++) {
            GameEvent *e = emit(g, GE_RESET, i);
            e->a = 1;
            e->pos[0] = two ? AT[i][0] : 0;
            e->pos[1] = two ? AT[i][1] : 18;
            e->pos[2] = 61.5f;
            e->f = two ? FACE[i] : PI;
            emit(g, GE_AI, i)->a = 1;
        }
        emit(g, GE_HUD, -1)->a = 0;
        for (int i = 0; i < np(g); i++) {
            g->ni_move_step[i] = (int)(frand(g) * 15.0f);
            if (i == 0)
                play_move(g, 0, "Ground_Taunt");
            else
                g->ni_move_done[i] = 1;
        }
        g->ny_watch = 1;
        g->ny_moved = 0;
    }
    if (!g->ny_moved && g->ni_time >= 1.25f) {       /* the camera comes in */
        GameEvent *e = emit(g, GE_CAMERA, -1);
        e->name = "Script";
        e->pos[0] = -2.61f; e->pos[1] = -13.75f; e->pos[2] = 74.4f;
        e->v[0] = -3.08f; e->v[1] = 11.93f; e->v[2] = 66.46f;
        g->ny_moved = 1;
    }
    for (int i = 0; i < np(g); i++)                  /* now and then a taunt, a grab or a punch at the roof */
        if (g->ni_move_done[i] && --g->ni_move_step[i] <= 0) {
            g->ni_move_step[i] = (int)(frand(g) * 60.0f);
            static const char *const moves[] = { "Ground_Taunt", "Ground_Grab_Down", "Ground_Punch_Down" };
            play_move(g, i, moves[(int)(frand(g) * 3.0f) % 3]);
        }
    if (g->ny_watch) {
        int two = np(g) >= 2;
        for (int i = 0; i < (two ? 2 : 1); i++) {    /* held on the roof */
            GameEvent *e = emit(g, GE_PLACE, i);
            e->pos[0] = two ? AT[i][0] : 0;
            e->pos[1] = 18;
            e->pos[2] = 61.5f;
        }
        if (g->centre_down) {                        /* the centre building goes: the usual view */
            camera(g, "TwoTilts", 1.5f);
            g->ny_watch = 0;
        }
    }
    return g->ni_frequency;
}

static float skip_results_ni(Game *g, float last)    /* SkipResultsNI: no pose; the monsters keep their place */
{
    g->ni_time += last;
    if (last == 0) {
        setup_ni(g, 0.25f);
        for (int i = 0; i < np(g); i++) {
            GameEvent *e = emit(g, GE_PLACE, i);     /* pos.y - 41 (the block left behind): relative, a = 1 */
            e->a = 1;
            e->pos[1] = -41;
            emit(g, GE_AI, i)->a = 1;
        }
        emit(g, GE_HUD, -1)->a = 0;
    }
    return 0;
}

static float end_block_ni(Game *g, float last)       /* EndBlockNI: the monsters walk on to the next block */
{
    g->ni_time += last;
    if (last == 0) {
        setup_ni(g, 0.25f);
        for (int i = 0; i < np(g); i++) {
            float x = (-17.0f * (float)(np(g) - 1) / 2.0f + 17.0f * (float)i) * 0.7f;
            emit(g, GE_RESET, i);
            emit(g, GE_AI, i)->a = 1;
            GameEvent *e = emit(g, GE_WALK, i);
            e->pos[0] = x; e->pos[1] = -5.2f - 8.0f;
            e->v[0] = x / 0.7f; e->v[1] = -5.2f;
            e->f = 1.5f;
            e = emit(g, GE_MOVE, i);
            e->name = "Ground_TurnAround";
            e->a = 1;                                /* Levels::moveComplete (monster.playMove: the done flag is
                                                      * not cleared here, setup_ni did) */
            emit(g, GE_MOVE, i)->name = "Ground_Idle";
        }
        camera(g, "TwoTilts", 1.5f);
    }
    int done = 0;
    for (int i = 0; i < np(g); i++)
        done += g->ni_move_done[i] != 0;
    if (done == np(g))
        return 0;
    if (g->ni_time > 5.0f) {                         /* taking too long: put them there */
        for (int i = 0; i < np(g); i++) {
            float x = (-17.0f * (float)(np(g) - 1) / 2.0f + 17.0f * (float)i) * 0.7f;
            reset_at(g, i, x, -13.2f, PI);
            emit(g, GE_AI, i)->a = 1;
        }
        return 0;
    }
    return g->ni_frequency;
}

static float taunt_ni(Game *g, float last)           /* EndLevelNI = EndWorldNI: everyone taunts, for as long as it lasts */
{
    g->ni_time += last;
    if (last == 0) {
        setup_ni(g, 0.25f);
        play_move(g, -1, "Ground_Taunt");            /* after clearAllGoals */
    }
    return g->ni_frequency;
}

static int is_finished(Game *g, int fn)
{
    switch (fn) {
    case FN_IS_BLOCK_FINISHED: return g->buildings <= 0;
    case FN_BOSS_FINISHED: return g->boss_finished && g->buildings <= 0;   /* LasVegas.scr .. [R] */
    case FN_NY_BOSS_FINISHED:                        /* NewYork.scr [R] */
        if (g->buildings <= 0)
            g->boss_finished = 2;
        return g->boss_finished;
    default: return g->buildings <= 0;
    }
}

static float call_ni(Game *g, int fn, int fallback, const char *name, float last)
{
    if (fn == FN_HOST) {
        float r = g->host.script ? g->host.script(g->host.ctx, name, last) : -1;
        if (r >= 0)
            return r;
        fn = fallback;
    }
    switch (fn) {
    case FN_BEGIN_LEVEL: case FN_CONTINUE_LEVEL: return begin_level_ni(g, last);
    case FN_BEGIN_BLOCK: return begin_block_ni(g, last);
    case FN_RESULTS: return results_ni(g, last);
    case FN_NY_BOSS_RESULTS: return ny_boss_results_ni(g, last);
    case FN_SKIP_RESULTS: return skip_results_ni(g, last);
    case FN_END_BLOCK: return end_block_ni(g, last);
    case FN_END_LEVEL: case FN_END_WORLD: return taunt_ni(g, last);
    default: return 0;
    }
}

/* 0x324180: the function a block runs for `key` ("BeginBlockNI", "IsBlockFinished" ...): the block's string
 * property of that name (the block before the current one when prev, i.e. the block just won), else WorldData's,
 * else the key; looked up as given, as "<City>::<name>", as "Levels::<name>" [R]. The port knows Levels.bsf's and
 * the cities' isBossBlockFinished; any other name goes to the host (FN_HOST) */
static int levels_fn(const char *fn)                 /* Levels.bsf's function of that name, FN_NONE if it has none */
{
    static const struct { const char *name; int fn; } levels[] = {
        { "BeginLevelNI", FN_BEGIN_LEVEL }, { "ContinueLevelNI", FN_CONTINUE_LEVEL },
        { "BeginBlockNI", FN_BEGIN_BLOCK }, { "ResultsNI", FN_RESULTS }, { "SkipResultsNI", FN_SKIP_RESULTS },
        { "EndBlockNI", FN_END_BLOCK }, { "EndLevelNI", FN_END_LEVEL }, { "EndWorldNI", FN_END_WORLD },
        { "IsBlockFinished", FN_IS_BLOCK_FINISHED }
    };
    for (size_t i = 0; i < sizeof levels / sizeof levels[0]; i++)
        if (same(fn, levels[i].name))
            return levels[i].fn;
    return FN_NONE;                                  /* GameOverNI: in no script */
}

static int resolve(Game *g, const char *key, int prev, char *name, size_t size)
{
    Group blk = block_def(g, prev ? g->cur - 1 : g->cur);
    const char *s = blk.at ? group_str(blk, key) : NULL;
    if (!s && g->world.at)
        s = group_str(g->world, key);
    if (!s)
        s = key;
    const char *colon = strstr(s, "::"), *fn = colon ? colon + 2 : s;
    snprintf(name, size, "%s", s);
    if (colon && same(fn, "isBossBlockFinished")) {  /* a city's own */
        int ny = (size_t)(colon - s) == 7 && strncmp(s, "NewYork", 7) == 0;
        return ny ? FN_NY_BOSS_FINISHED : FN_BOSS_FINISHED;
    }
    if (!colon || strncmp(s, "Levels::", 8) == 0)
        return levels_fn(fn);                        /* no city script has functions of Levels.bsf's names [R] */
    if (same(s, "NewYork::BossResultsNI"))
        return FN_NY_BOSS_RESULTS;
    return FN_HOST;
}

static void start_ni(Game *g, const char *key, int prev)   /* the entries' "NI" start: +0x10, +0xC = 0, +0x8 = 0 */
{
    g->ni = resolve(g, key, prev, g->ni_name, sizeof g->ni_name);
    g->ni_fallback = levels_fn(key);
    g->ni_timer = 0;
    g->ni_freq = 0;
}

/* ---------------------------------------------------------------- the results */

static void start_results(Game *g, int screen)       /* WorldGuiDriver::startResults [R] */
{
    g->result = GR_NONE;
    g->results_screen = screen;
    g->results_age = 0;
    emit(g, GE_RESULTS, -1)->a = screen;
}

static void leave(Game *g, int where, int city)      /* the app's next package becomes the shell (+0xA8 = 0x4D53F0) */
{
    if (g->left)
        return;
    g->left = 1;                                     /* the app drops the world next frame: the port stops here */
    GameEvent *e = emit(g, GE_EXIT, -1);
    e->a = where;
    e->b = city;
}

/* ---------------------------------------------------------------- the state machine (0x324340) */

static void set_state(Game *g, int s)                /* 0x325BF0: 1, 2, 3 and 15 run at once [R] */
{
    g->state = s;
    if (s == GS_SHUTDOWN || s == GS_LOADING || s == GS_2 || s == GS_STARTING)
        step(g);
}

/* the block won (0x3255B0, entering 7): the clock's time bonus, the block's winner, the next block, the checkpoint,
 * the results screen [R] */
static void block_won(Game *g)
{
    clock_stop(g);
    if (g->cfg.play_type == GP_CAMPAIGN) {
        for (int i = 0; i < np(g); i++)              /* the time bonus, shared */
            add_points(g, i, GPT_TIME_BONUS, g->time_bonus / np(g));
        /* the "Clock" challenge is told the seconds left (0x346780 on 0x4D5050): the host's challenge_block_won */
    } else if (g->cfg.play_type == GP_TIMED_RUN)
        g->clock_limit = clock_left(g);              /* what is left goes on to the next block */
    clock_reset(g);
    g->king_block = g->king_city = g->king_world = -1;
    for (int i = 0, best = 0; i < np(g); i++) {      /* the block's King: the most points, alone */
        int pts = g->p[i].points[GPT_TOTAL];
        g->city_total += pts;
        g->p[i].city_score += pts;
        if (pts == best)
            g->king_block = -1;
        else if (best < pts) {
            best = pts;
            g->king_block = i;
        }
    }
    g->first_place = g->second_place = -1;
    for (int i = 0, a = -1, b = -1; i < np(g); i++) {
        int pts = g->p[i].points[GPT_TOTAL];
        if (a < pts) {
            b = a;
            g->second_place = g->first_place;
            a = pts;
            g->first_place = i;
        } else if (b < pts) {
            b = pts;
            g->second_place = i;
        }
    }
    if (g->king_block != -1) {                       /* King modes: blocks make a city, cities the world */
        Player *p = &g->p[g->king_block];
        if (++p->blocks_won >= game_blocks_to_win(g)) {
            p->cities_won++;
            g->king_city = g->king_block;
            if (p->cities_won >= game_cities_to_win(g))
                g->king_world = g->king_city;
        }
    }
    for (int i = 0; i < np(g); i++)                  /* 0x2DCB40: a monster still down comes back, for a life */
        cancel_respawn(g, i, 0);
    g->level_done = !(g->cur < g->last);
    if (!g->level_done)
        activate_next_block(g, 1);
    if (g->level_done && g->city_total >= g->par_score)
        g->city_flags |= 2;
    /* isAtCheckpoint is always 1 (0x3030B0); 0x323240: every monster of the city revealed, flag 8 (Boosy's unlock
     * after it, 0x3259E4, needs every city's saved flags: the host's profile, profile_block_won) */
    if (g->host.all_revealed && g->host.all_revealed(g->host.ctx))
        g->city_flags |= 8;
    g->city_destroyed = g->level_done;
    if (g->level_done)
        g->city_flags |= 1;
    checkpoint_save(g);
    if (g->level_done && g->cfg.play_type == GP_CAMPAIGN && g->cfg.city_index == g->cfg.num_cities - 1)
        g->world_done = 1;
    start_results(g, g->cfg.play_type == GP_CAMPAIGN ? GR_SCREEN_RESULTS :
                     g->cfg.play_type == GP_TIMED_RUN ? GR_SCREEN_TIMED_RUN : GR_SCREEN_KING_BLOCK);
    start_ni(g, "ResultsNI", !g->level_done);
}

/* entering a state (table 0x434040); returns 1 when it is over at once */
static int enter(Game *g)
{
    int s = g->state, adv = 0;
    GameEvent *e = emit(g, GE_STATE, -1);
    e->a = s;
    switch (s) {
    case GS_LOADING:                                 /* 0x324398 */
        g->city_destroyed = g->quit = g->select = g->world_done = 0;
        g->weapons_on = 1;                           /* 0x3243A8 */
        break;
    case GS_BEGIN_LEVEL:                             /* 0x3243D0: the intro, or ContinueLevelNI past the first block */
        start_ni(g, g->first < g->cur ? "ContinueLevelNI" : "BeginLevelNI", 0);
        break;
    case GS_BEGIN_BLOCK:                             /* 0x324458 */
        start_ni(g, "BeginBlockNI", 0);
        adv = g->ni == FN_NONE;
        g->weapons_on = 1;                           /* 0x324494 */
        break;
    case GS_PLAY:                                    /* 0x3244A0: IsBlockFinished, the clock runs */
        g->fin = resolve(g, "IsBlockFinished", 0, g->fin_name, sizeof g->fin_name);
        g->fin_fallback = FN_IS_BLOCK_FINISHED;
        clock_start(g);
        break;
    case GS_BLOCK_WON:                               /* 0x324500 */
        block_won(g);
        adv = g->ni == FN_NONE;
        break;
    case GS_GAME_OVER:                               /* 0x324520: GameOverNI is in no script */
        start_results(g, GR_SCREEN_GAME_OVER);
        start_ni(g, "GameOverNI", 1);
        g->weapons_on = 0;                           /* 0x324588; then enableSfx(0), every 3D sound stopped
                                                      * (0x26D500), the sound's frame, enableSfx back (0x324584 ..
                                                      * 0x3245B0): the host's GE_STATE */
        break;
    case GS_END_BLOCK:                               /* 0x3245C0 */
        start_ni(g, "EndBlockNI", 1);
        adv = g->ni == FN_NONE;
        g->weapons_on = 0;                           /* 0x324510 */
        break;
    case GS_END_LEVEL:                               /* 0x324600 */
        start_results(g, g->cfg.play_type == GP_KING_CITY || g->cfg.play_type == GP_KING_WORLD ?
                         GR_SCREEN_KING_CITY : GR_SCREEN_LEVEL);
        start_ni(g, "EndLevelNI", 0);
        adv = g->ni == FN_NONE;
        break;
    case GS_END_WORLD:                               /* 0x3246A0 */
        start_results(g, g->cfg.play_type == GP_KING_CITY || g->cfg.play_type == GP_KING_WORLD ?
                         GR_SCREEN_KING_WORLD : GR_SCREEN_WORLD);
        start_ni(g, "EndWorldNI", 0);
        adv = g->ni == FN_NONE;
        break;
    case GS_EXIT:                                    /* 0x324740 */
        leave(g, 0, -1);
        break;
    default:
        break;
    }
    return adv;
}

/* the state's end (table 0x434020 for 6 .. 11, otherwise the next state) [R] */
static void transition(Game *g)
{
    int pt = g->cfg.play_type, king = pt == GP_KING_CITY || pt == GP_KING_WORLD;
    switch (g->state) {
    case GS_PLAY:                                    /* 0x324D08 */
        if (pt == GP_ATTRACT)
            leave(g, 2, g->button_end);
        else
            set_state(g, g->game_over ? GS_GAME_OVER : GS_BLOCK_WON);
        break;
    case GS_BLOCK_WON:                               /* 0x324DE8 */
        for (int i = 0; i < np(g); i++)              /* the next block's points start from 0 */
            clear_points(&g->p[i]);
        if (g->select || g->quit)
            leave(g, 0, -1);
        if (g->level_done && pt == GP_TIMED_RUN) {
            g->city_destroyed = 0;
            leave(g, 0, -1);
        } else if (g->level_done)
            set_state(g, GS_END_LEVEL);
        else if (!king)
            set_state(g, GS_END_BLOCK);
        else if (g->king_city != -1)
            set_state(g, GS_END_LEVEL);
        else if (g->king_block != -1)
            set_state(g, GS_END_BLOCK);
        else {                                       /* nobody won the block: it is played again */
            g->cur -= 2;
            activate_next_block(g, 1);
            set_state(g, GS_BEGIN_LEVEL);
        }
        break;
    case GS_GAME_OVER:                               /* 0x3250B0 */
        if (g->select || g->quit) {
            if (pt == GP_TIMED_RUN && g->select)
                g->start_block = 0;
            else
                checkpoint_restore(g);
            leave(g, 0, -1);
            break;
        }
        for (int i = 0; i < np(g); i++) {            /* continue: everyone back up (the life given is taken) */
            g->lives[i] = 1;
            cancel_respawn(g, i, -1);
        }
        clock_reset(g);
        if (pt == GP_TIMED_RUN)
            g->start_block = 0;
        else
            checkpoint_restore(g);                   /* three lives, the checkpoint's block */
        status_reset(g);                             /* the points */
        g->game_over = 0;
        if (pt == GP_TIMED_RUN)
            g->cur = g->first - 1;
        else
            g->cur = (g->first > g->start_block ? g->first : g->start_block) - 1;
        activate_next_block(g, 1);
        for (int i = 0; i < np(g); i++) {            /* full HP (min(most, 100000)), 0x2E5730(m, 0) */
            GameEvent *e = emit(g, GE_REVIVE, i);
            e->a = 1;
            g->human[i] = 0;
            g->hp[i] = 1e9f;
        }
        set_state(g, GS_BEGIN_LEVEL);
        break;
    case GS_END_BLOCK:                               /* 0x325308 */
        set_state(g, GS_BEGIN_BLOCK);
        break;
    case GS_END_LEVEL:                               /* 0x325360 */
        if (pt == GP_CAMPAIGN) {
            if (g->world_done)
                set_state(g, GS_END_WORLD);
            else
                leave(g, 0, -1);                     /* the city select: the next city */
        } else if (!king || pt == GP_KING_CITY)
            leave(g, 0, -1);
        else if (g->king_world != -1)
            set_state(g, GS_END_WORLD);
        else {                                       /* King of the World goes on in the next city */
            g->start_block = 0;
            g->cfg.city_index++;
            g->city_destroyed = 1;
            leave(g, 1, g->cfg.city_index);
        }
        break;
    case GS_END_WORLD:                               /* 0x3254E8 */
        g->quit = 1;
        leave(g, 0, -1);
        break;
    default:
        set_state(g, g->state + 1);
        break;
    }
}

static void step(Game *g)
{
    int adv = 0;
    if (g->left)
        return;
    if (g->state != g->entered)
        adv = enter(g);
    g->entered = g->state;
    if (g->ni != FN_NONE) {                          /* the NI function, every ni_freq seconds */
        g->ni_timer -= g->dt;
        if (g->ni_timer <= 0) {
            float r = call_ni(g, g->ni, g->ni_fallback, g->ni_name, g->ni_freq);
            g->ni_freq = r;
            if (r <= 0.001f) {
                g->ni = FN_NONE;
                adv = 1;
            }
            g->ni_timer += r;
        }
    }
    if (g->state == GS_PLAY) {
        int finished;                                /* IsBlockFinished, every frame */
        if (g->fin == FN_HOST) {
            float r = g->host.script ? g->host.script(g->host.ctx, g->fin_name, 0) : -1;
            finished = r >= 0 ? r > 0 : is_finished(g, g->fin_fallback);
        } else
            finished = g->fin != FN_NONE && is_finished(g, g->fin);
        if (finished)
            adv = 1;
        else if (g->enabled && clock_left(g) <= 0) { /* time up */
            int over = !(g->cur < g->last) || g->cfg.play_type == GP_TIMED_RUN;
            if (over)
                g->game_over = 1;
            emit(g, GE_TIME_UP, -1)->a = over;
            adv = 1;
        } else if ((g->cfg.cheats & 0x10000u) && (g->cfg.cheats & 0x20000u) && g->cur < g->last)
            adv = 1;                                 /* [R 0x324930] cheat 874098: the block is won at once */
        if (!g->game_over && g->cfg.play_type == GP_CAMPAIGN) {   /* game over: fewer lives than monsters down */
            int dead = 0;
            for (int i = 0; i < np(g); i++)
                dead += g->hp[i] <= 0;
            if (total_lives(g) < dead) {
                g->game_over = 1;
                adv = 1;
            }
        }
    }
    if (g->state == GS_BLOCK_WON || g->state == GS_GAME_OVER || g->state == GS_END_LEVEL ||
        g->state == GS_END_WORLD) {                  /* WorldGuiDriver::getResultState */
        switch (g->result) {
        case GR_QUIT: g->quit = 1; adv = 1; break;
        case GR_SELECT: g->select = 1; adv = 1; break;
        case GR_CONTINUE: case GR_DONE: adv = 1; break;
        default: break;
        }
        if (adv)
            g->results_screen = -1;                  /* the stand-in's clock stops */
    }
    /* [R 0x324C80] the demo and a pad's button (RControllerMgr 0x2C1BF4): in states 4 .. 11 the state becomes 6 (not
     * entered), gShell's idle timer is set to 120 (0x308240, GE_EXIT's b) and the state ends: the shell's again */
    if (g->button && g->cfg.play_type == GP_ATTRACT && g->state >= GS_BEGIN_LEVEL && g->state <= GS_END_WORLD) {
        g->state = g->entered = GS_PLAY;
        g->button_end = 1;
        adv = 1;
    }
    if (adv)
        transition(g);
}

/* ---------------------------------------------------------------- the API */

Game *game_new(const GameHost *host, const GameConfig *cfg)
{
    Game *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    if (host)
        g->host = *host;
    if (cfg)
        g->cfg = *cfg;
    else {
        g->cfg.play_type = GP_CAMPAIGN;
        g->cfg.num_players = 1;
    }
    if (g->cfg.num_players < 1 || g->cfg.num_players > GAME_PLAYERS)
        g->cfg.num_players = 1;
    if (g->cfg.num_bots < 0 || g->cfg.num_bots > g->cfg.num_players ||
        (g->cfg.num_bots == g->cfg.num_players && g->cfg.play_type != GP_ATTRACT))   /* the demo: all of them */
        g->cfg.num_bots = 0;
    if (g->cfg.num_cities <= 0)
        g->cfg.num_cities = 7;
    wrand_seed_pm(g->cfg.seed ? g->cfg.seed : 0x2545F491u);   /* RWorld's reset: GameStatus seed[0] */
    g->start_block = g->cfg.start_block;
    g->king_block = g->king_city = g->king_world = g->first_place = g->second_place = -1;
    g->results_screen = -1;
    g->weapons_on = 1;                               /* gp-0x772C: 1 in the ELF's data */
    for (int i = 0; i < GAME_PLAYERS; i++) {
        g->p[i].cities_won = g->cfg.cities_won[i];   /* GameStatus outlives the world [R 0x4D7990 +0x318 + 0x60 i] */
        g->hp[i] = 1e9f;
        if (g->cfg.play_type == GP_ATTRACT && i < g->cfg.num_players)
            g->lives[i] = 3;                         /* the shell's demo set them (0x3086C8); 0x33DB90 leaves them */
    }
    return g;
}

void game_free(Game *g) { free(g); }

int game_load_city(Game *g, const char *city)
{
    if (!g || !city)
        return -1;
    char book[48];
    snprintf(g->city, sizeof g->city, "%s", city);
    snprintf(book, sizeof book, "%s_Props", city);
    g->world = g->host.find ? g->host.find(g->host.ctx, book, "WorldData") : (Group){ NULL, 0 };
    g->nblocks = 0;
    Group list = g->world.at ? group_sub(group_sub(g->world, "BlockData"), "BlockList") : g->world;
    if (list.at) {
        Record r[MAX_BLOCKS];
        uint32_t n = group_items(list, r, MAX_BLOCKS);
        for (uint32_t i = 0; i < n; i++) {
            const char *s = record_str(list, r[i]);
            if (s)
                g->blocks[g->nblocks++] = s;
        }
    }
    /* 0x303240: BlockEnd (default the list's count - 3), BlockStart (default 0) */
    g->last = g->world.at ? (int)group_num(g->world, "BlockEnd", (float)(g->nblocks - 3)) : g->nblocks - 3;
    g->first = g->world.at ? (int)group_num(g->world, "BlockStart", 0) : 0;
    g->par_score = g->world.at ? (int)group_num(g->world, "ParScore", 0) : 0;
    g->cur = -1;
    g->left = 0;
    g->boss_finished = 0;
    g->city_total = 0;
    g->city_flags = 0;
    set_state(g, GS_LOADING);
    return g->world.at && g->nblocks > 0 ? 0 : -1;
}

void game_start(Game *g)
{
    if (!g)
        return;
    int keep = g->start_block;
    g->start_block = 0;                              /* a new city: the lives (0x33DB90 with no start block yet) */
    status_reset(g);
    g->start_block = keep;
    if (g->cfg.play_type == GP_TIMED_RUN)          /* the run's time starts from nothing [G: 0x4D7E48 is written
                                                      * only at a block's win (0x3256A0) and is 0 at the boot (.bss);
                                                      * no traced code clears it as a run starts, so a second run would
                                                      * begin with the first's time left] */
        g->clock_limit = 0;
    g->cur = (g->first > g->start_block ? g->first : g->start_block) - 1;   /* 0x2B74D0 */
    activate_next_block(g, 0);
    checkpoint_save(g);                              /* the checkpoint starts here: the shell's startGame takes it
                                                      * (0x3088F0 -> 0x33E290, the campaign's) before the world [R] */
    for (int i = 0; i < GAME_PLAYERS; i++)
        clear_points(&g->p[i]);
    set_state(g, GS_STARTING);                       /* 0x2B7D40: 1, then 4 */
    g->state = GS_BEGIN_LEVEL;
}

void game_update(Game *g, float dt, const GameInput *in)
{
    if (!g)
        return;
    g->dt = dt;
    g->now += dt;
    if (in) {
        for (int i = 0; i < np(g); i++)
            g->hp[i] = in->hp[i];
        g->buildings = in->buildings;
        g->centre_down = in->centre_down;
    }
    if (g->results_screen >= 0 && g->auto_results > 0 && g->result == GR_NONE) {
        g->results_age += dt;
        if (g->results_age >= g->auto_results)
            g->result = g->results_screen == GR_SCREEN_GAME_OVER ? GR_CONTINUE : GR_DONE;
    }
    step(g);
}

int game_events(Game *g, GameEvent *out, int max)
{
    if (!g)
        return 0;
    int n = g->nev < max ? g->nev : max;
    memcpy(out, g->ev, (size_t)n * sizeof *out);
    memmove(g->ev, g->ev + n, (size_t)(g->nev - n) * sizeof *out);
    g->nev -= n;
    return n;
}

void game_move_done(Game *g, int slot)
{
    if (g && slot >= 0 && slot < GAME_PLAYERS)
        g->ni_move_done[slot] = 1;
}

void game_set_result(Game *g, int result) { if (g) g->result = result; }
void game_auto_results(Game *g, float seconds) { if (g) g->auto_results = seconds; }
void game_restart_shell(Game *g) { if (g) set_state(g, GS_EXIT); }
void game_attract_button(Game *g) { if (g) g->button = 1; }
void game_add_points(Game *g, int slot, int cat, int amount) { if (g) add_points(g, slot, cat, amount); }

uint32_t *game_rng(Game *g) { return g ? wrand_pm_state() : NULL; }
float game_frand(Game *g) { return g ? frand(g) : 0; }

void game_set_boss_finished(Game *g, int centre_down)
{
    if (g)
        g->boss_finished = centre_down ? 2 : 1;      /* NewYork.scr: 2 when the centre building is down [R] */
}

int game_become_human(Game *g, int slot)
{
    if (!g || slot < 0 || slot >= GAME_PLAYERS || g->state != GS_PLAY)
        return 0;                                    /* Monsters::becomeHuman: only in state 6 [R] */
    g->human[slot] = 1;
    return 1;
}

int game_can_respawn(const Game *g, int slot, int streaker_held)
{
    if (!g || slot < 0 || slot >= GAME_PLAYERS)
        return 0;
    if (total_lives(g) == 0 && g->cfg.play_type == GP_CAMPAIGN)
        return 0;
    return g->human[slot] && !streaker_held;
}

void game_start_respawn(Game *g, int slot, const GameRespawnIn *in, GameRespawn *out)
{
    memset(out, 0, sizeof *out);
    if (!g || slot < 0 || slot >= GAME_PLAYERS)
        return;
    take_life(g, slot);
    out->fall = 1;
    if (in && in->streaker) {                        /* at the streaker's place, facing as it does; it goes */
        memcpy(out->pos, in->streaker_pos, sizeof out->pos);
        out->facing = in->streaker_facing;
        out->delete_streaker = 1;
        out->fall = !in->on_line;
    }
    if (out->fall && in) {                           /* 0x2D3510: where the camera's look meets y = -10 */
        float dx = in->cam_dir[0], dy = in->cam_dir[1], len2 = dx * dx + dy * dy;
        if (len2 < 1e-6f) {
            dx = 1;
            dy = 0;
        } else {
            float r = 1.0f / sqrtf(len2);
            dx *= r;
            dy *= r;
        }
        float t = fabsf(dy) > 1e-6f ? (STREET_Y - in->cam_pos[1]) / dy : 0;
        out->pos[0] = in->cam_pos[0] + dx * t;
        out->pos[1] = in->cam_pos[1] + dy * t;
        /* z: where the vertical through that point meets the camera's plane at +0x7C of its object (gp-0x6CFC
         * +0x660), put through its matrix (0x102F60), 0 when the plane is all but vertical (|normal . up| <= 0.001)
         * [R]; which plane that is is not traced: here the view's top [G] */
        float h = sqrtf(in->cam_dir[0] * in->cam_dir[0] + in->cam_dir[1] * in->cam_dir[1]);
        float pitch = atan2f(in->cam_dir[2], h > 1e-6f ? h : 1e-6f);
        float d = sqrtf((out->pos[0] - in->cam_pos[0]) * (out->pos[0] - in->cam_pos[0]) +
                        (out->pos[1] - in->cam_pos[1]) * (out->pos[1] - in->cam_pos[1]));
        out->pos[2] = in->cam_half_fov > 0 ? in->cam_pos[2] + d * tanf(pitch + in->cam_half_fov) : in->cam_pos[2];
        if (out->pos[2] < 0)
            out->pos[2] = 0;
        out->facing = PI;                            /* 0x101ED0(0, -1): towards the camera */
    } else if (out->fall)
        out->pos[1] = STREET_Y;
    g->human[slot] = 0;
    g->hp[slot] = 1e9f;
}

int game_state(const Game *g) { return g ? g->state : GS_NONE; }
int game_weapons_on(const Game *g) { return g ? g->weapons_on : 1; }
int game_left(const Game *g) { return g ? g->left : 1; }
int game_play_type(const Game *g) { return g ? g->cfg.play_type : GP_CAMPAIGN; }
int game_lives(const Game *g, int slot) { return g && slot >= 0 && slot < GAME_PLAYERS ? g->lives[slot] : 0; }

float game_clock(const Game *g, int *visible)
{
    if (!g) {
        if (visible)
            *visible = 0;
        return 0;
    }
    if (visible)                                     /* 0x2AF880: ClockVisible, and states 5 and 6 [R] */
        *visible = g->visible && g->state >= GS_BEGIN_BLOCK && g->state < GS_BLOCK_WON;
    return clock_left(g);
}

float game_clock_limit(const Game *g) { return g ? g->limit : 0; }   /* lwc1 0x4D51C0 [R 0x2AF834] */
int game_city_index(const Game *g) { return g ? g->cfg.city_index : 0; }

int game_points(const Game *g, int slot, int cat)
{
    return g && slot >= 0 && slot < GAME_PLAYERS && cat >= 0 && cat < 7 ? g->p[slot].points[cat] : -1;
}
int game_kills(const Game *g, int slot, int cat)
{
    return g && slot >= 0 && slot < GAME_PLAYERS && cat >= 0 && cat < 7 ? g->p[slot].kills[cat] : -1;
}
int game_city_score(const Game *g, int slot) { return g && slot >= 0 && slot < GAME_PLAYERS ? g->p[slot].city_score : 0; }
int game_city_total(const Game *g) { return g ? g->city_total : 0; }
int game_blocks_won(const Game *g, int slot) { return g && slot >= 0 && slot < GAME_PLAYERS ? g->p[slot].blocks_won : 0; }
int game_cities_won(const Game *g, int slot) { return g && slot >= 0 && slot < GAME_PLAYERS ? g->p[slot].cities_won : 0; }
int game_blocks_to_win(const Game *g) { return g ? 6 - np(g) : 5; }   /* 0x33DAE0 [R] */
int game_cities_to_win(const Game *g)                /* 0x33DAB0 [R] */
{
    return g ? (g->cfg.num_cities - 1) / np(g) + 1 : 7;
}
int game_king(const Game *g, int which)
{
    if (!g)
        return -1;
    return which == 0 ? g->king_block : which == 1 ? g->king_city : g->king_world;
}
int game_city_destroyed(const Game *g) { return g ? g->city_destroyed : 0; }
int game_quitting(const Game *g) { return g ? g->quit : 0; }
int game_choose_monster(const Game *g) { return g ? g->select : 0; }
int game_world_done(const Game *g) { return g ? g->world_done : 0; }
int game_par_score(const Game *g) { return g ? g->par_score : 0; }
uint32_t game_city_flags(const Game *g) { return g ? g->city_flags : 0; }

int game_block(const Game *g, const char **name)
{
    if (name)
        *name = g && g->cur >= 0 && g->cur < g->nblocks ? g->blocks[g->cur] : NULL;
    return g ? g->cur : -1;
}

const char *game_block_name(const Game *g, int which)   /* 0x2B4B20: 0 = the one before, unless the city is done */
{
    if (!g)
        return NULL;
    int i = g->cur;
    if (i > 0 && which == 0 && !g->level_done)
        i--;
    return i >= 0 && i < g->nblocks ? g->blocks[i] : NULL;
}

int game_num_blocks(const Game *g) { return g ? g->last - g->first + 1 : 0; }   /* 0x2AF270 [R] */
int game_result_state(const Game *g) { return g ? g->result : GR_NONE; }
