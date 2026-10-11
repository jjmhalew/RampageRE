/* autoplay.h - the automated playthrough's watchdog (rampage --autoplay, docs/NATIVE.md "Autoplay"). Not the game's:
 * a test of the port. The viewer (main.c) plays the game as it goes, with every monster driven by the bots' AI
 * (director.c) and the menus and results screens answered for the player, and hands this module what it sees once a
 * frame. Here: a line per block to a log file (city, block, play type, the times, the result, the score, lives, HP,
 * the boss, the objects, the memory), and the problems seen, each a PROBLEM line (and a screenshot when asked for):
 * a block that never ends, a results screen that never answers, a shell screen that never moves on, the game's state
 * standing still, a monster whose place is not a number, a monster that does not move, one outside the street, under
 * the ground or inside a standing building, the objects' counts running away, the process' memory, and a crash (the
 * last frame's state to the log). The log's lines are key=value pairs, for tools/playthrough.py. */
#ifndef RAMPAGE_AUTOPLAY_H
#define RAMPAGE_AUTOPLAY_H

#define AP_MONSTERS 4
#define AP_BUILDINGS 64

typedef struct {
    float pos[3];
    float hp;                       /* 0 .. 200 */
    int lives;
    const char *state;              /* its move state's name */
    int mounted;                    /* on a building's face */
    float ground;                   /* the ground under it (a roof, a water's floor, the street) */
    int out;                        /* not in play now: defeated (a streaker, hidden), dropping in, the reveal's */
} ApMonster;

typedef struct { float min[3], max[3]; int standing; } ApBuilding;

typedef struct {
    double t;                       /* simulated seconds since the start */
    int in_game;                    /* a game plays (else the shell) */
    /* the shell */
    const char *focus;              /* shell.h shell_focus() */
    int shell_state, movie;         /* gShell's state; a movie plays */
    /* the game */
    int state, play_type;           /* game.h GS_*, GP_* */
    const char *city, *block;       /* the city, the current block's BlockDef name */
    int block_index;                /* in the BlockList */
    int results;                    /* a results screen shows */
    float clock;                    /* the block clock's seconds left */
    int nmon;
    ApMonster mon[AP_MONSTERS];
    int points[AP_MONSTERS];        /* this block's totals (GPT_TOTAL) */
    int city_score;                 /* the blocks' totals so far */
    int guard_falls;                /* buildings the bots' 60 s guard felled so far (director.c BOT_STUCK) */
    int boss;                       /* the block has a boss: 1 alive, 2 beaten (none 0) */
    float boss_hp;                  /* its health, 0 .. 1 */
    int nbuildings, standing;
    ApBuilding b[AP_BUILDINGS];
    int people, vehicles, army, particles, street, parade, rail, edibles;
} ApFrame;

typedef struct {
    double minutes;                 /* the simulated time to stop at (0: 600) */
    int blocks, cities, demos;      /* stop after this many blocks ended, games left, demos played (0: no limit) */
    int stop_at_menu;               /* stop when the shell's main menu shows after a game */
    double stuck, block, screen, shell;   /* seconds: a monster not moving, a block, a results screen, a shell
                                     * screen (0: 90, 1200, 240, 300) */
    int keep_every;                 /* keep every n-th block's end screenshot (0: 5) */
} ApLimits;

/* what ap_frame asks of the viewer */
enum {
    AP_SHOT = 1,                    /* a screenshot now, to ap_shot_path() */
    AP_STOP = 2,                    /* the run is over */
    AP_FORCE_BLOCK = 4,             /* the block never ends: fell every building (the run goes on, the block marked) */
    AP_FORCE_RESULT = 8,            /* the results screen never answers: answer it */
    AP_FORCE_SHELL = 16             /* the shell screen never moves on: the viewer tries Back */
};

typedef struct Autoplay Autoplay;
/* log: the log file (written as it goes, flushed each line); shots: the folder for the screenshots, NULL none;
 * what: the run's description (its first line). NULL when the log cannot be written */
Autoplay *ap_open(const char *log, const char *shots, const ApLimits *lim, const char *what);
int ap_frame(Autoplay *a, const ApFrame *f);           /* AP_* bits */
const char *ap_shot_path(const Autoplay *a);
void ap_shot_done(Autoplay *a, int ok);
void ap_event(Autoplay *a, const char *fmt, ...);      /* an EVENT line */
void ap_game_left(Autoplay *a, int demo);              /* a game went back to the shell (a demo's: demo 1) */
/* the SUMMARY line, the log closed; the process' exit code: 0 no problem seen, 3 problems */
int ap_close(Autoplay *a, const char *why);
double ap_peak_mb(void);                               /* the process' peak memory so far (MB), -1 unknown */
#endif
