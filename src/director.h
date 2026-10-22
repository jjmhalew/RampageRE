/* director.h - the game flow's choreography: what game.c's events ask of a monster and of the camera, the way the game
 * does it (docs/FLOW_NOTES.md; director.c cites the code, [R] read, [G] a guess or a stand-in).
 *
 * [R] What the game has, and what this is the port of:
 *   - the monster's goals (MonsterAI at the monster's +0xB92C, its update 0x2EE430, run for an AI monster (+0x108) by
 *     0x2E1AA0 in place of the pad's transition pick 0x2EC3D0): a queue of PlayMoveGoal (vtable 0x44AEA0, update
 *     0x2F1DB0) and WalkToGoal (vtable 0x44AEE0, update 0x2F1F20) that the Monster bindings playMove (0x9B, 0x2AC1A8),
 *     walkTo (0x9C, 0x2AC220), clearAllGoals (0x9A, 0x2EE760) and WalkToGoal.addWayPoint (0x2F2020) fill;
 *   - the AI's "pad" (MonsterAI +0x28): it picks the move graph's transitions itself, by the target node's action code
 *     (0x2F2630), by the target state (0x2F2970), through the graph's next-hop table (0x2F2B90), or forces a state
 *     (0x2D9710); the walk pushes the stick towards the way point (0x2EDB40), in world axes (0x2C0AD0, 0x2E8410);
 *   - the camera manager (gCameraMgr, RCameraMgr at 0x4D3100: setCamMode 0x27EDF0, setPrevCamMode 0x27EDB0,
 *     getCamMode 0x27EE70) and its controller (CamController, 0x4D3100 +0x6C: setMode 0x167400, update 0x167690: the
 *     blend of two modes), the modes of camera.prb (table 0x433A60: TwoTilts 0x4D5C80, Free 0x4D5E60, Follow 0x4D5D10,
 *     Script 0x4D5D60) and the script camera (RScriptCam, vtable 0x44B3D0: activate 0x312100, update 0x311750 ->
 *     0x311820, the ScriptCamera bindings 0x2A6E30);
 *   - Monsters::startRage's camera (Monsters.scr: the script camera on the monster, cut, back after 1.5 s over 0.5 s).
 *   - the bots' DestroyBuildings goal (type 2, vtable 0x44AE60, its update 0x2F1A60; docs/MULTIPLAYER_NOTES.md): the
 *     nearest standing building of the front row, mounted on a side, punched cell by cell, climbed, its roof
 *     stomped; with none left the nearest other monster.
 *
 * Engine-agnostic: nothing here draws or plays. Positions are the game's (z up, the street at z = 0, the camera at -y).
 * The monster side drives the move graph runner of moves.h in place of moves_step while the monster is AI; the camera
 * side takes camera.c's TwoTilts result and gives the final camera (before the shake, cam_shaken). */
#ifndef RAMPAGE_DIRECTOR_H
#define RAMPAGE_DIRECTOR_H
#include "data.h"
#include "moves.h"
#include "camera.h"

/* ================================================================ the monster's goals */

/* a state node's action code (node +0x3C), what the AI asks for (0x2F2630's second argument) [R George's nodes] */
enum {
    DA_ATTACK = 0, DA_TRANSITION = 1, DA_HOP = 4, DA_JUMP = 5, DA_MANTLE = 6, DA_CLIMB_DOWN = 7, DA_CLIMB_UP = 8,
    DA_WALK = 9,             /* Ground_Walk: what the walk asks for */
    DA_MIRROR = 10,
    DA_IDLE = 11,            /* Ground_Idle, the idles: what a monster with no goal asks for */
    DA_UNMANTLE = 12, DA_NONE = 13,
    DA_KEEP = 15             /* a played move in its state: only the transitions that need nothing are taken */
};

/* what the AI reads of MONSTER_DATA besides moves.h's graph */
typedef struct {
    int n;                   /* states, as MoveGraph.n */
    int8_t *action;          /* per state: its node's action code (node +0x3C), DA_* */
    int8_t (*fallback)[16];  /* per state and action: the transition to pick when none is found (state +0x18), -1 */
    uint8_t *hop;            /* n x n after the states: [from * n + to] the next state on the way, 0xFF none */
} DirGraph;
int dir_graph_read(const Bundle *monster, DirGraph *ag);   /* 0 = ok */
void dir_graph_free(DirGraph *ag);
/* 0x2F2B90: the target can be reached from the state through usable transitions (or clip ends) */
int dir_graph_reachable(const DirGraph *ag, const MoveGraph *g, int from, int to, MoveCall call, void *ctx);

enum { DG_PLAY_MOVE = 0, DG_WALK_TO = 1, DG_DESTROY = 2 };   /* the goal types (0x2ED810) */

/* ---- the bots' goal: DestroyBuildings (Monster binding 157 destroyBuildings, 0x2AC2B0: goal type 2, vtable 0x44AE60,
 * its update 0x2F1A60; docs/MULTIPLAYER_NOTES.md). What it sees of the world each frame, the host's to fill: */
typedef struct {
    float min[3], max[3];    /* its box (building +0x1DC, +0x1E8) */
    float pos[3];            /* its place (+0x2C: the middle of its foot, 0x2BC920 [R]) */
    int state;               /* +0x14C: 1 .. 3 standing, 4 and up falling, 0 gone (building.h BUILDING_*) */
    int row, width, storeys; /* +0x10C, +0x1C8, the storeys standing (+0x1D0) */
} DirBuilding;
typedef struct {
    float pos[3];
    int mounted, rail;       /* the building it is on (+0xC640, -1 none) and its rail there */
    int human;               /* it became human (+0xAB4) */
    int ai;                  /* +0x108 */
    float radius;            /* (MONSTER_DATA +0x188 + the kept type-15 record's +0x8) x its scale (0x2E4FA0) [R] */
    float ref[3];            /* its reference point (0x2E4E90): the place + MONSTER_DATA +0x190 / +0x194 and the
                              * record's +0xC / +0x10 along its x / y axes, x its scale [R] */
} DirMonster;
typedef struct {
    /* the bot itself */
    float pos[3], fwd[2];    /* its place and its forward axis, flat */
    float centre[3];         /* its body centre (0x2E4D30: the place + MONSTER_DATA +0x1A0 / +0x1A4 and the kept
                              * type-16 record's along its axes, x its scale): 2.5 ahead for George */
    int mirror, mirror_queued;
    int mode;                /* +0xAD8 (1, 2: in the air) */
    int mounted, rail, storey;   /* its building's face: the building (-1 none), the rail and storey there */
    int smash;               /* JUMP SMASH (upgrade flag 8) or the rage: the butt slam on a roof */
    float hp, half_width;    /* +0xA4C; MONSTER_DATA +0x198 x its scale */
    float probe;             /* 0x2E4E40: (MONSTER_DATA +0x1A4 + the kept type-16 record's +0x10) x its scale */
    int self;                /* its slot in m */
    const DirBuilding *b; int nb;
    const DirMonster *m; int nm;
    int under;               /* the building under its body (0x2D6150), -1 none */
    int ni;                  /* an NI runs (0x4D6A9A, game_ni_on): the walk takes the way as it is */
    void *ctx;
    int (*cell)(void *ctx, int b, int storey, int column);   /* a front cell's damage state 0 .. 3, -1 none */
    int (*can_mount)(void *ctx, int side, int *building);     /* canMount(side, 0) (FACE_* sides), its candidate */
    int (*can_climb)(void *ctx, int dv, int dh);              /* canClimb: its raw answer (climb.c) */
    int (*place)(void *ctx, int b, const float pt[3]);        /* the place check 0x2DDC40 at a point of building b */
    void (*collapse)(void *ctx, int b);                       /* collapseBuilding (the 60 s guard) */
} DirBotView;
typedef struct {
    int target, side;        /* +0x20 the building (-1 none), +0x28 its side: 0 left, 1 right, 2 front, 3 none */
    float walk[3];           /* +0x2C the walk point */
    int use_hit; float hit_off[3];   /* +0x38, +0x3C: the last hit's point less the monster's place */
    int have_cell, cell_storey, cell_col;   /* +0x48 .. +0x54: the front cell it is after */
    int cur_storey, cur_col; /* +0x5C, +0x60 */
    float cooldown;          /* +0x64 */
    int avoid; float avoid_time;   /* +0x68, +0x6C: a building it was knocked off */
    float on_target;         /* +0x70: seconds on this target */
    uint32_t seed;           /* (unused: rand01 is the world's one state, dir_ai_seed) */
    int st_front, st_side_l, st_side_r, st_respawn, st_punch_down;   /* +0x74 .. +0x84: the states it goes to */
} DirBot;
#define DIR_GOALS 8
#define DIR_WAYPOINTS 8
typedef struct {
    int type;                /* DG_* */
    int state;               /* play move: the state (+0x20), -1 unknown */
    int notify;              /* its end callback is Levels::moveComplete: game_move_done when it ends */
    int started;             /* flags bit 0: it has begun (its start callback ran) */
    int restarted;           /* +0x24: its state's clip started again after it began */
    float wp[DIR_WAYPOINTS][4];   /* walk: the way points {x, y, z, radius squared} (+0x20) */
    int nwp, cur;            /* +0x28 count, +0x2C the one walked to */
} DirGoal;
typedef struct {
    DirGoal q[DIR_GOALS];    /* the queue (+0x20 / +0x24), q[0] the current one */
    int n;
    int current;             /* q[0] is the current goal (+0x1C): a new goal drops it */
    int force;               /* a state forced (0x2D9710: the start list at +0x110), taken next frame; -1 none */
    float stick[2];          /* out: the stick (+0xC) in world x, y, length 1 while walking, else 0 */
    int done;                /* out: goals with notify that ended this frame (Levels::moveComplete) */
    DirBot bot;              /* the DestroyBuildings goal's own (one per monster) */
    const DirBotView *view;  /* in: what a DestroyBuildings goal sees this frame (the host's), NULL none */
} DirAI;

void dir_ai_init(DirAI *ai);
void dir_ai_clear(DirAI *ai);                         /* clearAllGoals (0x2EE760); GE_RESET */
/* Monster.playMove(name, "", end): a PlayMoveGoal for state (moves_find); the goal running now is dropped (0x2EE650).
 * notify: the end callback is Levels::moveComplete. Returns the goal's index in the queue, -1 when it is full */
int dir_ai_play_move(DirAI *ai, int state, int notify);
/* Monster.walkTo(pos, radius, "", end): a WalkToGoal to pos, done within radius (in x, y) of it. The script's second
 * argument (1.5 in EndBlockNI, GameEvent.f) is that radius, not a speed [R 0x2F20E0: it keeps radius squared] */
int dir_ai_walk_to(DirAI *ai, const float pos[3], float radius, int notify);
/* WalkToGoal.addWayPoint(pos, radius) (0x2F2020) on the goal walk_to returned */
void dir_ai_add_waypoint(DirAI *ai, int goal, const float pos[3], float radius);
/* Monster.destroyBuildings (binding 157): the DestroyBuildings goal, which never ends (a bot in BeginBlockNI). The
 * states it goes to are looked up in g (0x2F1C10). Returns its index, -1 when the queue is full */
int dir_ai_destroy(DirAI *ai, const MoveGraph *g, uint32_t seed);
/* the world's rand01 state (gWorld +0x82C) from GameStatus seed[1], at the world's reset (0x2B74D0) */
void dir_ai_seed(uint32_t seed);
/* +0x38 (0x2EEBD0): the monster's foot lock moved it, or its hands lifted it: the destroy goal's cooldown */
void dir_ai_cooldown(DirAI *ai);
/* what the monster tells the goal (MonsterAI's forwarders 0x2ED680 .., only while it is AI): a hit on a building's
 * face off from the monster (hitBuildingPt; mirror: the monster plays mirrored, the x is kept negated), mounted on
 * building b at rail (mountBuilding), knocked off building b (avoid) */
void dir_ai_on_hit(DirAI *ai, const float off[3], int mirror);
void dir_ai_on_mount(DirAI *ai, int b, int rail, int width);
void dir_ai_avoid(DirAI *ai, int b, const DirBotView *view);

/* One frame of an AI monster (setIsAI(1)) in place of moves_step: the windows (0x2EBFE0), the goals (0x2EE430) and
 * their picks, the switch. pos: the monster's root position. call answers the transitions' script checks (as for
 * moves_step). Returns 1 when the state changed (r->from, r->via as moves_step: via -1 = forced or the clip ended);
 * ai->stick and ai->done are set. */
int dir_ai_step(DirAI *ai, const DirGraph *ag, MoveRunner *r, const float pos[3], float dt, float world_dt, MoveCall call,
                void *ctx);

/* ================================================================ the camera */

enum { DCM_NONE = -1, DCM_TWOTILTS = 0, DCM_FREE = 1, DCM_FOLLOW = 2, DCM_SCRIPT = 3, DCM_COUNT = 4 };   /* 0x433A60 */

typedef struct { float pos[3], rows[3][3], fov; } CamView;   /* rows: right, forward, up (as GameCamera.rot) */
typedef struct { float pos[3], facing; } DirObject;          /* what the script camera may be linked to */

typedef struct {             /* the script camera (RScriptCam, 0x4D5D60) */
    int tgt_obj, tgt_local;  /* setTarget: +0x24 the object (-1: none, the offset is a world point), +0x2C local */
    float tgt_off[3];        /* +0x30 */
    int loc_obj, loc_local;  /* setLocation: +0x3C, +0x45 */
    float loc_off[3];        /* +0x48 */
    int pos_inertia, rot_inertia;                     /* enableInertia (+0xEC, +0xED): 1 */
    float smoothing;                                  /* +0x54: 0.95 */
    float goal[3], cur[3], vel[3];                    /* the position follower (+0xA8) */
    float qgoal[4], q[4], w[3];                       /* the rotation follower (+0x58), w in radians / s */
    float fov, fov_goal;                              /* the fov follower (+0xDC), setFovTarg */
    int have_last;                                    /* +0x4 */
    float last_q[4], last_pos[3];                     /* +0x8, +0x18: what it shows */
    int cut;                                          /* a cut asked for, done at the next update */
} ScriptCam;

typedef struct {
    GameCamera *tt;          /* the TwoTilts mode: the host runs cam_update on it every frame, before dircam_update */
    ScriptCam script;
    int cur, old, prev;      /* controller +0x10 the mode, +0x14 the one blended from; manager +0x70 */
    float timer, inv;        /* +0x18 the blend's time left, +0x1C 1 / its length */
    int flags;               /* +0x20 the old mode's blend flags (1: its place stays live) */
    CamView snap;            /* +0x24 the camera when the blend began */
    CamView out;             /* +0x70 the result (before the shake munger) */
    float rage_time[8]; int nrage;   /* Monsters::restoreCam's timers, one installed by each startRage */
    CamView follow;          /* the Follow mode (FollowPlayerCamMode 0x4D5D10): the camera shown when it began */
    float follow_off[3];     /* +0x40: that camera's place less player 1's monster's then (0 without one) */
    float p1[3]; int have_p1;   /* player 1's monster (gWorld +0x40) as the last frame gave it */
    float frame_hz;          /* the world's step rate for the per-frame smoothing: 30 */
} CamDirector;

void dircam_init(CamDirector *d, GameCamera *tt);    /* TwoTilts current */
/* gCameraMgr.setCamMode(name, blend): "TwoTilts" (or "default"), "Free", "Follow", "Script"; blend in seconds, 0 a cut.
 * -1 = unknown name */
int dircam_set_mode(CamDirector *d, const char *name, float blend);
void dircam_set_prev_mode(CamDirector *d, float blend);   /* setPrevCamMode: back to the mode before the last set */
const char *dircam_mode(const CamDirector *d);            /* getCamMode */
/* ScriptCamera.setTarget / setLocation(object, local, x, y, z[, cut]): obj -1 = nul (the point is in the world), else
 * the index into dircam_update's objects (a monster slot), the offset added to its place (local: in its frame) */
void dircam_script_target(CamDirector *d, int obj, int local, const float off[3]);
void dircam_script_location(CamDirector *d, int obj, int local, const float off[3], int cut);
void dircam_script_cut(CamDirector *d);                   /* ScriptCamera.cut */
/* GE_CAMERA as game.c gives it: setCamMode(name, blend); for "Script" (game.c's script_camera, a = 1) then the
 * location pos and the target v as world points, and a cut. (game.c sends a Script event per placement where the
 * script sets the mode once; a second setCamMode("Script") changes nothing but the mode setPrevCamMode returns to) */
void dircam_game_event(CamDirector *d, const char *name, float blend, const float pos[3], const float v[3], int cut);
/* Monsters::startRage's camera for monster obj: the script camera from 50 back and 15 up, at 8 up, a cut, blended in
 * over 0.25 s unless it shows already; 1.5 s later setPrevCamMode(0.5) */
void dircam_rage(CamDirector *d, int obj);
/* a frame (0x167690): the modes' cameras and their blend. objs: the objects the script camera may follow */
void dircam_update(CamDirector *d, float dt, const DirObject *objs, int nobjs, CamView *out);
/* the world moved dy along y (the game's next block): the cameras' world places with it */
void dircam_shift_y(CamDirector *d, float dy);

#endif
