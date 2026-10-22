/* main.c - RampageRE's native viewer: draws the city blocks straight from RAMPAGE.VOL (the game's own archive),
 * with the game's layout, lights, traffic, people and a monster (city.c, anim.c, render_gl.c).
 *   rampage [RAMPAGE.VOL] [City] [block number] [--night] [--monster NAME] [--clip N] [--shot FILE.png] [--time T]
 *   [--dumptex] (texture packs, texpack.c: every texture drawn to mods/dump/; mods/textures/ replaces them)
 * RAMPAGE.VOL may be the game's disc image (.iso), read in place; without it data_find() looks (iso.h: build\disc
 * first). Default Chicago, 1. --shot draws one frame (at T seconds) as a PNG and quits; with a
 * monster in play the camera settles for 2 s, then the monster plays --do "right 1, cross, wait 1.5, ..." (held
 * stick directions and pressed buttons, each for its seconds; --walk S = "right S").
 * Keys: W A S D move, Q E down and up, Shift faster, right mouse button look, wheel speed; Page Up / Page Down the
 * block, 1-7 the city, N day / night, F fog, M the next monster (none, then the 30), K / L its clips, F12 a screenshot
 * (shot_NNN.png), Esc quit. With a monster, G switches to play: the monster runs its own move graph (MONSTER_DATA,
 * moves.c) from a pad made of keys - W A S D / the arrows the left stick, Space Cross (jump), J Square (punch),
 * K Triangle (kick), L Circle (grab), R R2 (roar) - and the game's own camera (camera.c) follows it. Controllers
 * (pad.h) play too, one a player, and rumble where the game does (docs/NATIVE.md "Input").
 * Built with RAMPAGE_GUI (build.bat: RampageRE.exe, no console) it is the player build: without arguments the front end
 * (--shell), stdout and stderr in rampage.log, a plain window title, and a message box when it cannot start. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "render_gl.h"
#include "camera.h"
#include "moves.h"
#include "climb.h"
#include "ik.h"
#include "fx.h"
#include "sound.h"
#include "army.h"
#include "hud.h"
#include "vehicle.h"
#include "rail.h"
#include "edible.h"
#include "game.h"
#include "look.h"
#include "shadow.h"
#include "director.h"
#include "footstep.h"
#include "challenge.h"
#include "reveal.h"
#include "profile.h"
#include "shell.h"
#include "movie.h"
#include <ctype.h>
#include <time.h>
#include "boss.h"
#include "streetobj.h"
#include "parade.h"
#include "collide.h"
#include "texpack.h"
#include "pad.h"
#include "iso.h"
#include "settings.h"
#include "wrand.h"
#include "autoplay.h"
#ifdef __ANDROID__
#include "touch.h"                               /* the on-screen pad, player 1's pad slot */
#endif

static const char *const CITIES[] = { "Chicago", "NewYork", "SanFrancisco", "LosAngeles", "London", "HongKong", "LasVegas" };
/* the campaign's order (ShellData CityData.City0 .. City6, GAME_NOTES.md) */
static const char *const CAMPAIGN[] = { "LasVegas", "SanFrancisco", "London", "LosAngeles", "Chicago", "HongKong", "NewYork" };
static int campaign_index(const char *city)
{
    for (int i = 0; i < 7; i++)
        if (!strcmp(CAMPAIGN[i], city))
            return i;
    return 0;
}
#define NCITIES 7

/* the pad a monster is driven by this frame: the left stick (x right, y down), the right stick, the buttons pressed
 * this frame (MV_CROSS ...) and held */
typedef struct {
    float lx, ly, rx, ry;
    uint8_t pressed[MV_COUNT], held[MV_COUNT];
} Pad;

/* how a monster is driven (docs/MULTIPLAYER_NOTES.md): player 1's keys (and pad 0), player 2's keys (and pad 1), the
 * computer (the bots, and players 3 and 4 without a pad), or players 3 and 4's own pads (pad.h slot 2, 3) */
enum { DRIVE_KEYS1 = 0, DRIVE_KEYS2 = 1, DRIVE_COMPUTER = 2, DRIVE_PAD = 3 };

#define MONSTERS 4                                   /* the game's players and bots together (Common 0x4D7E28) */
#define TIMESTEP (1.0f / 30.0f)                      /* gWorld.getTimeStep(): the world's step [R 0x2B7800]; the monster's
                                                      * update runs once a step, here once a drawn frame */

#define MON_FX_SLOTS 32
#define MON_FX_NAMED 8
/* a monster in play, one per player or bot: its slot is the scene's (Scene.mon), the army's, the game's player and
 * the HUD's (Hud<slot>.ifc). The game keeps all of this in the monster object (the MONSTER_DATA offsets below). */
typedef struct Mon {
    struct Viewer *v;                                /* the world it plays in */
    int slot;                                        /* 0 .. 3 */
    int monster;                                     /* which of the 30 (Viewer.monsters), -1 none */
    int bot;                                         /* a bot: the last num_bots of the players (setIsAI in each block) */
    int drive;                                       /* DRIVE_*: whose pad */
    Pad pad;                                         /* this frame's pad (keys, --do, or the computer's) */
    float mx, my, mz, heading;                       /* where the monster stands, its turn about Z (0: facing +Y) */
    float vel[3]; int mode, no_gravity, column;      /* velocity (+0xBD30), movement mode (+0xAD8), no gravity
                                                        (+0xAB8), the slots' column (+0xBAB4) */
    MoveGraph graph; MoveRunner run; MoveInput input; int have_graph;
    int yaw_axis; float entry_yaw, entry_root[3], entry_t;    /* the root's axis that tells its turn; its turn and place at the
                                                        state's start */
    float frame[2][3], stick[2];                     /* the camera's right and forward axes the stick is read in
                                                        (+0xA18, +0xA24) and the stick they were taken for (+0xA3C) */
    float air[3];                                    /* the stick's own velocity (+0xA68), NODE_AIR_* */
    uint32_t armed;                                  /* body parts an attack event armed (bits) */
    int mirror, mirror_queued;                       /* the clip plays mirrored (+0xAA0); for the next clip (+0xAA1) */
    Mount mount;                                     /* on a building's face (climb.c) */
    Hand hand[2];                                    /* its hands on the wall (0 left, 1 right, as seen) */
    float hand_thick;                                /* the hands' +0x4 (monster +0xC780): off the face they grip */
    int held;                                        /* the person in its hand (people.c), -1 none */
    int held_unit;                                   /* or the army unit (army.c), -1 none */
    int force_to;                                    /* a state played from outside (playReaction), + 1, taken in at
                                                      * its next step as the slots' starts are; 0 none */
    int held_vehicle;                                /* or a vehicle (vehicle.c), -1 none */
    int held_parade;                                 /* or a biker or a cart's food (parade.c), -1 none */
    int held_rail, rail_knock;                       /* or a rail object (rail.c), -1 none; a knock taken */
    int held_sobj;                                   /* or a parked car (streetobj.c), -1 none */
    int streaker;                                    /* the defeated monster's streaker (people.c), -1 none */
    EdiblePower power;                               /* the monster's energy, rage and speed (energy mirrored in .energy) */
    ArmyMonWeapons weapons;                          /* its WeaponList (+0xC5E4, army.c): the roar's Shockwave */
    int pending_slot;                                /* a reaction the army caused: started at the next step, -1 */
    int defeated;                                    /* its HP ran out: React_Defeated .. the respawn */
    float look_yaw, look_pitch;                      /* the head animator's angles now (+0xBC, +0xE8) */
    float scale, scale_from, scale_to, scale_time, scale_left;   /* setDynamicScale */
    DirGraph ag; int have_ag;                        /* the move graph as the AI reads it (director.c) */
    DirAI ai; int is_ai;                             /* the monster's goals while the game drives it (setIsAI) */
    float hp, energy;                                /* its hit points (most: MONSTER_DATA +0x138), energy, eating */
    const MoveNode *face;                            /* the face layer's node playing now */
    IkFeet feet_ik; int feet_due;                   /* the legs bent to the held feet (ik.c), once a frame */
    float feet_dt;                                   /* the time the foot object's next update covers */
    FootProps feet_fx; FootState feet;              /* the footfalls' names and the foot records (footstep.c) */
    FaceQueue faces;                                 /* setFacialAnim / queueFacialAnim's records, the belches (look.c) */
    HeadLook head; LookLimits look_lim;              /* the head animator (look.c) */
    TailAnim tail;                                   /* the tail animator (look.c) */
    float friction;                                  /* +0xF4: the ground velocity's friction, set by the last knock */
    int msph0, nmsph;                                /* the monster's spheres in the frame's world (objects msph0 ..) */
    ShadowMonster smon;                              /* its blob shadows (shadow.c) */
    Hud *hud;                                        /* its HUD (Hud<slot>.ifc); slot 0's is the screen's (Viewer.hud) */
    int human;                                       /* it became human (+0xAB4): its streaker runs */
    int dropping;                                    /* dropping in after a respawn (+0xAB5), until it lands */
    int double_jumped;                               /* +0xAB7: setHasDoubleJumped (Monsters::didDoubleJump, binding
                                                      * 90, 0x2AB2A0), hasDoubleJumped (89, 0x2AB270) asked by
                                                      * hasDoubleJump; cleared each frame out of the air (0x2E0C38) [R] */
    int spins;                                       /* +0xAC0: the jump's spins (getJumpSpinCount 92, 0x2AB2D0;
                                                      * incJumpSpinCount 94, 0x2AB318), cleared with it [R] */
    /* each body part's attack (0x2E7880: the record's every bone takes it, +0xBD60 + 0x1C x bone, the newest arming
     * a bone's): damage (+0x2C), the knock's distance and time (+0x30, +0x34), ReactionLevel (+0x24), the flags
     * (+0x14: 2 in front, 4 behind), the monsters it may hit (+0x18), and the ones it hit already (the bones' +0x18,
     * cleared by 0x2E33A0 on every part of the record that hit, 0x2E45A0: each once an arming); parts: the record's;
     * seq orders the armings; kind .. over what the blow does (+0x20, +0x1C, +0x38, +0x3C, +0x64 .. +0x70, look.c
     * monster_blow) */
    struct MonAtk { float damage, knock, knock_time; int level; uint32_t flags, victims, hit, parts; unsigned seq;
                    int kind, side; float lift, turn; int over[4]; }
        atk[MOVES_PARTS];
    int air_hits;                                    /* +0xA9C: the blows it took in the air (MONSTER_AIR_HITS land) */
    unsigned atk_seq;
    int sph0, nsph;                                  /* its armed spheres this frame: Scene.spheres sph0 .. */
    float ev15, ev15x, ev15r;                        /* the kept type-15 record's +0x10, +0xC and +0x8 (+0xBA9C) */
    float ev16, ev16x;                               /* the kept type-16 event record's +0x10 and +0xC (+0xBAA0),
                                                      * 0 none */
    const Water *water_in; int water_tested;         /* 0x2D5F00's answer this frame (+0xC700), asked (+0xC704) */
    int under, under_tested;                         /* 0x2D6150's answer this frame (+0xC6F8), asked (+0xC6FC) */
    int landed; float landed_z;                      /* it landed this frame from the air at landed_z: the box test
                                                      * (0x2E2C50, in its update) came before the landing (0x2E2770,
                                                      * the world's pass after, 0x2B6D20) and saw it in the air */
    const Water *water_was;                          /* the answer the frame before (for the trace) */
    float sph_prev[64][3]; int sph_prev_n;           /* its collision spheres' centres last frame (+0x430, valid +0x688) */
    int reveal, shown;                               /* the cryotube's monster (Viewer.extra): out of the world
                                                      * (+0x108 / +0x109) until the reveal shows it */
    /* the clip events' effect slots (type 12; the record +0x7DFC's +0x0 .. +0xFF: 32 x {FollowEffect, its serial}):
     * slot k's effect id and the node it follows (MoveGraph.fxnode), while bit k of fx_used is set */
    int fx_slot[MON_FX_SLOTS], fx_node[MON_FX_SLOTS]; uint32_t fx_used;
    /* the scripts' FollowEffects on a node of the monster by name (createAndStartAttachedToNodeByName: doFart's,
     * doBarf's; Nick's fire and eyes): the effect and its node, kept on it while the effect lives */
    struct { int fx; char node[48]; } fx_named[MON_FX_NAMED]; int nfx_named;
    float fart_in[4];                                /* installObjectTimer("Monsters::timedDoFart", 2): due in, 0 none */
    float thrown_t;                                  /* +0xA94: 1 s from a throw (0x2DA28C), the camera's */
    char rage_fx_name[32]; int rage_fx, rage_on;     /* its book's MonsterFx RageFx (+0x164) and the one on (+0x168) */
    int nick, nick_fx[3];                            /* Nick's fire and eyes (+0x174 .. +0x17C): 0 not Nick, 1 to
                                                      * start, 2 on, 3 shut down (becomeHuman, till the respawn) */
    float face_ev;                                   /* the face's event player's time (+0x7DE8 +0x8), a share */
} Mon;

typedef struct Viewer {
    Vol vol;
    City *city; int city_index;
    const char *blocks[64]; uint32_t nblocks, block;
    int night, fog;
    const char *monsters[64]; uint32_t nmonsters; int clip;   /* clip -1 = its idle (player 1's monster) */
    Mon mon[MONSTERS]; int nmon;                     /* the monsters in play: players then bots (Mon.monster of slot
                                                      * 0 -1: none, the free camera only) */
    int num_bots;                                    /* the last num_bots of the nmon are bots */
    Scene scene; int have_scene;
    Renderer *rnd;
    int play; CamSettings cam_settings; GameCamera cam;   /* play: the monsters under control, the game camera */
    int show_hits;                                   /* H draws the attack spheres */
    /* 0: monster 0's own shake (monster +0x7DFC +0x160), 1 .. 7 the effects', 8 .. 10 monsters 1 .. 3's own */
    CamShake shakes[8 + MONSTERS - 1];
    uint32_t shake_seed;
    int last_collapse[64];                           /* per building: its collapse when last looked at */
    Parade *parade;                                  /* the balloons, bikers and food carts (parade.c) */
    struct { int fx, obj, marker; float off[3]; } pfx[32]; int npfx;   /* the parade's effects that follow their object
                                                                         * (marker: a ChallengeEffect, stopped with it) */
    Vehicles *veh;                                   /* the traffic, the fire truck and the rockets (vehicle.c) */
    Rail *rail;                                      /* the trains, cable cars and freighters (rail.c) */
    struct { int id, fx, obj; float off[3]; } rfx[64]; int nrfx;   /* their effects (following them; off in its frame) */
    struct { int id, h, obj; } rsnd[64]; int nrsnd;                /* and sounds */
    /* the objects' own 3D sounds (add3DSfx, 0x287860: in one of the object's 16 slots, at its place +0x2C so that
     * it follows it): its module (OS_*), the object and its serial, the sound, its name and keep (the script's
     * second argument: a deleted object, 0x288050 -> 0x274710, kills its sounds without keep) */
    struct { int src, obj, serial, h, keep; const char *name; } osnd[128]; int nosnd;
    Edibles *edibles;                                /* the power-ups and hazards behind the windows (edible.c) */
    Game *game;                                      /* the world's states, the lives, the clock (game.c) */
    Boss *boss;                                      /* the block's boss and the ambient ones (boss.c) */
    /* the bosses' effects that follow (monster: the monster slot followed, -1 none) */
    struct { int id, fx, obj, monster, node_id; const char *node; float off[3]; } bfx[64]; int nbfx;
    struct { int obj, h; } bsnd[64]; int nbsnd;      /* and their sounds (add3DSfx: each follows its part) */
    float play_now;                                  /* the world's play time (gWorld.curTime): the buildings' alarms */
    int profile_autoload;                            /* rampage.cfg profile_autoload (port extra, settings.h) */
    int game_block;                                  /* a block the game moved on to, to build after the frame; -1 */
    int left_block;                                  /* the block the game left, laid at -41 by that build; -1 */
    int game_built;                                  /* this build is the game's: keep the game going */
    /* Traffic.bsf, once a world: fNextExitTime (onEndFrame draws it again each time it runs out, world steps in
     * traffic_acc), the HiddenVehicle's start x, and its state kept over the port's rebuilds of the block's traffic */
    float traffic_exit, traffic_acc, hidden_x;
    VehicleHidden hidden;
    struct { int fx, edible, id; float off[3]; } efx[64]; int nefx;   /* the edibles' effects that follow them (id:
                                                     * edible.c's handle of it) */
    struct { int id, fx, obj; float off[3]; } vfx[64]; int nvfx;   /* the vehicles' effects that follow them */
    StreetObjs *sobj;                                /* the street and roof objects (streetobj.c) */
    int sobj_of[SCENE_STREET_MAX];                   /* per Scene.street entry its streetobj index, -1 */
    int street_of[SCENE_STREET_MAX];                 /* per streetobj index its Scene.street entry */
    /* their effects that follow an object (obj >= 0: at its point off, or its model node) or monster mon's node */
    struct { int id, fx, obj, node_id, mon; const char *node; char mnode[32]; float off[3]; } sofx[64]; int nsofx;
    int weather_fx[4], nweather_fx, weather_due;     /* the block's Weather.bsf effects on, and whether to start them */
    int force_strike;                                /* --do airstrike: the time-up airstrike as if the clock were out */
    Army *army;
    struct { int id, fx, unit, shot, node, die_with; float off[3], head, speed, len; } fxa[128]; int nfxa;   /* the army's effects that follow
                                                      * (a beam's: its stream's head last frame, its rod) */
    int armed_people[32]; int narmed_people;         /* people firing (army_person_moved) */
    /* the people's own effects (PE_FX_ON): per person and slot, following it (follow) at off, until PE_FX_OFF */
    struct { int fx, who, slot, follow; float off[3]; } ppfx[64]; int nppfx;
    Bundle fx_bundle; int have_fx_bundle;            /* fx.bdp: the effects (fx.c) */
    FxLib *fxlib; FxWorld *fxw;
    ShadowList shl; ShadowGround sg; ShadowLighting slight;   /* the shadows (shadow.c) */
    int boss_block;                                  /* the block has a boss (the HUD's boss bar) */
    CamDirector cd; CamView view;                    /* the camera modes and their blend; the camera shown */
    /* the game's next block, activated at the results: the scene keeps the block left (the next one stands 41 behind
     * it, slot 1) until the walk is over and the next block begins; meanwhile the game's places, which are the next
     * block's, are block_shift (41) further on in y here */
    int next_block; float block_shift;
    int rubble_fx[3];                                /* ResultsNI's three effects in the rubble (fx ids, -1) */
    int unattended;                                  /* --shot: the results screens are answered for the player */
    SceneMesh fx_shapes[32][8]; int fx_nshapes[32]; /* the effects' debris models (fx.bdp's .pwk), by shape */
    Bundle gui_world, gui_common; int have_gui;      /* WorldGui.bdp, CommonGUI.bdp: the HUD (hud.c) */
    Hud *hud; int no_hud;                            /* player 1's HUD, which has the screens; --nohud: none */
    Camera hud_cam;                                  /* the frame's camera, for the floating scores */
    Challenge challenge;                             /* the block's challenge, its goals counted (challenge.c) */
    uint32_t upgrades[64];                           /* the moves each monster took from upgrades (the profile's
                                                      * getMonsterFlags; a new profile: none [G]) */
    int challenges_lost;                             /* a block of this city was won with its challenge not met */
    int cities_won[MONSTERS];                        /* GameStatus' players' cities won (+0x3C), across the worlds */
    int upgrade_fx;                                  /* the MonsterUpgrade object's effect, -1 */
    int exit_game;                                   /* the game went back to the shell (the Pause menu's Quit) */
    float sfx_vol, music_vol, music_now;             /* the Options page's volumes / 0.75 (the profile's 0.75 = 1);
                                                      * the music now */
    int music_state;                                 /* a state whose music waits for its block to be built, 0 none */
    Reveal reveal; RevealRoster roster;              /* the cryotube's monster and the unlocks (reveal.c): the
                                                      * roster's flags stand for the profile's, from 0 */
    RevealLoc tubes[REVEAL_LOCS]; int ntubes;        /* the tubes of a block activated but not built yet */
    int extra;                                       /* the slot after the players that holds the monster to reveal
                                                      * (0x2B6BE0: players + 1 monsters, the last hidden), -1 none */
    RevealWalk rwalk;                                /* its walk-on (Monsters::onRevealNITick) */
    Rubble rubble;                                   /* findPointInRubble's slots: the block left's buildings */
    Profile prof;                                    /* the player's profile (profile.c): unlocks, moves, settings */
    int prof_on_card;                                /* its file exists (the shell's profile list) */
    char card_name[16];                              /* the name in it */
    int card_free_kb;                                /* --card-free-kb: the "card"'s free room; -1: the drive's */
    float card_poll;                                 /* seconds to the next look at the drive */
    int results_update;                              /* the Results' updateProfile is the campaign's */
    Shell *shell; Scene *shell_scene;                /* the front end while it shows (shell.c), its empty scene */
    Shell *popup;                                    /* the SaveGame popup over the game's screens (Results'
                                                      * saveProfile: the SaveIcon, the card's messages) */
    struct GuiDraw *gd;                              /* its 3D objects: models, effects, this frame's layers */
    float gui_side;                                  /* the window past each side of the 640 x 480 screen, its pixels */
    char data_path[512];                             /* RAMPAGE.VOL or the disc image (fx_open: the ELF beside / in it) */
    Movie *movie;                                    /* the movie the shell plays (movie.c), NULL none */
    int shell_mode, shell_go;                        /* --shell: a game's Quit goes back to it; a game to start */
    int lang;                                        /* --lang: the .loc column (hud.h: 0 English .. 8 Portuguese) */
    int region;                                      /* the disc's (iso.c data_region): 0 USA, 1 Europe, 2 another */
    ShellGame last_game;                             /* the shell's last choices (back from a game) */
    uint32_t world_rng;                              /* gShell +0x1C: the state the world's seeds are drawn from */
    ShellStart kept;                                 /* what gShell keeps while the game plays (shell_keep) */
    const char *prof_path; int prof_write;           /* its file; written at the Results (not by --shot runs) */
    Settings *gfx; Window *win; int gfx_write;       /* the graphics settings and the window (the Options' Graphics
                                                      * page), rampage.cfg written (not by --shot runs) */
    int play_type;                                   /* --play: GP_CAMPAIGN (default), King of the City / World, Timed Run */
    int attract;                                     /* the game is the attract mode's demo (play type 0, all bots) */
    int attract_button;                              /* its end came from a button (GE_EXIT 2, b 1) */
    CollideWorld cw;                                 /* the frame's collision world (collide.c): built by world_build
                                                      * before anything moves, asked by the monsters and the modules */
    float mid_x;                                     /* the middle of the players' x this frame (0x336360) */
    uint32_t bot_seed;                               /* the bots' rand01 */
    /* the controllers (pad.h; never opened by a --shot run: pads_live 0): this frame's and the last one's per slot, the
     * slots that have one, the menus' repeat clocks (up, down, left, right) */
    int pads_live; unsigned pads_on;
    PadState pads[PAD_SLOTS], pads_prev[PAD_SLOTS];
    struct MenuPad {                                 /* [R] MenuEventHandler's per pad (+0x10 + 0x3C k, 0x16A810) */
        uint8_t armed[2];                            /* +0x00: the left stick's flick (x, y) */
        float ref[2];                                /* +0x04: where it has to move 0.45 from */
        int held;                                    /* +0x20: the event code held (0 none) */
        float now, start, last;                      /* the handler's clock +0x1F4; +0x24 the press, +0x28 the last */
    } menu_pad[PAD_SLOTS];
    int vibrate;                                     /* gCommon.setEnableRumble's answer: the pad's +0xC84 [R 0x1A5020] */
    /* --autoplay (autoplay.h; a test, not the game's): every monster the computer's, the menus answered, the watchdog;
     * simulated time ap_t; the autopilot's next press, the mode, players, bots and city (-1 the campaign's next) it
     * goes for, the demos it waits for (no presses in the menus) */
    Autoplay *ap; double ap_t; float ap_wait;
    int ap_type, ap_players, ap_bots, ap_city, ap_demos;
    int bot_guard_falls;                             /* buildings the bots' 60 s guard felled (for the watchdog) */
} Viewer;

/* gCommon.getPlayType() of the game played: the demo's 0, else --play's or the shell's (unset: the campaign) */
static int play_type(const Viewer *v) { return v->attract ? GP_ATTRACT : v->play_type ? v->play_type : GP_CAMPAIGN; }

/* the monster's instance in the scene, NULL none */
static Instance *mon_in(const Mon *mo)
{
    const Scene *s = &mo->v->scene;
    int i = mo->slot < s->nmon ? s->mon[mo->slot].inst : -1;
    return i >= 0 ? &mo->v->scene.instances[i] : NULL;
}

/* a monster is in play: there is one at all (slot 0's), and it is in the scene */
static int have_monster(const Viewer *v)
{
    return v->nmon > 0 && v->mon[0].monster >= 0;
}

/* another monster rages or has its energy full (0x2E5730: then a full energy is 99.9, the rage one at a time) */
static int others_full(const Mon *mo)
{
    for (int k = 0; k < mo->v->nmon; k++)
        if (k != mo->slot && (mo->v->mon[k].power.rage || mo->v->mon[k].power.energy >= 100))
            return 1;
    return 0;
}

/* the monster's own camera shake (slot 0 first, then after the effects') */
static CamShake *mon_shake(Mon *mo)
{
    return &mo->v->shakes[mo->slot == 0 ? 0 : 7 + mo->slot];
}

/* the game's pad effects (PS2ControllerPack vtable +0x28, 0x1A5020 (pad, ms) [R]: with the vibration on, the big motor
 * at 0xC0 and the small one off for that long), on the monster's pad (+0xFC); never a computer's monster (+0x108) */
static void mon_rumble(const Mon *mo, int ms)
{
    if (mo->v->pads_live && mo->v->vibrate && !mo->is_ai && mo->drive != DRIVE_COMPUTER)
        pad_rumble(mo->slot, 0xC0 / 255.0f, ms / 1000.0f);
}

static void monster_hit(Mon *mo, const char *name, float amount, int level, const float knock[3], const float from[3]);
static void monster_blow_hit(Mon *mo, const Mon *by, const struct MonAtk *atk, const char *name);
static int mon_water(Mon *mo);
static void challenge_stats(Viewer *v, const char *name, int points);
static void power_from_profile(Mon *mo);
static void upgrade_touch(Mon *mo, const float centre[3], float radius);
static void street_step(Viewer *v, float dt);
static float ground_at(Mon *mo);
static void mon_body_centre(const Mon *mo, float out[3]);
static float mon_radius(const Mon *mo);
static float mon_half_height(const Mon *mo);

static void hud_frame(Viewer *v, int after);
static void rage_started(Mon *mo);
static void save_profile(Viewer *v);
static void popup_step(Viewer *v, float dt);

/* seconds into the monster's clip at the state's time: the game runs a clip on a 0 .. 1 clock that the state's rate
 * (node +0x0, 1 / its length) moves (0x2FFF10), so the state's time x rate x the clip's duration */
static double clip_at(Mon *mo, const MoveState *st, double t)
{
    const Instance *in = mon_in(mo);
    return t * st->rate * (in && in->has_clip ? in->clip.duration : 1);
}

/* the clip's root at time t (seconds of the clip, clip_at), held at the clip's last frame (clip_pose loops: its end
 * is its start) */
static int root_at(Mon *mo, double t, float tr[3], float q[4])
{
    Viewer *v = mo->v;
    const Instance *in = mon_in(mo);
    if (in && in->has_clip && t > in->clip.duration - 1e-4)
        t = in->clip.duration - 1e-4;
    return scene_monster_root(&v->scene, mo->slot, t < 0 ? 0 : t, tr, q);
}

/* a root's place and rotation (rows of 3) in the monster's frame as drawn: mirrored, Sx R Sy, the place across x
 * (Bip01's rule, 0x194DC0 / 0x194A60) [R] */
static void seen_root(int mirrored, float p[3], const Mat4 *m, float r[3][3])
{
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r[i][j] = m->m[4 * j + i] * (mirrored && (i == 0) != (j == 1) ? -1.0f : 1.0f);
    if (mirrored)
        p[0] = -p[0];
}

/* the root's turn about Z from clip time ta to tb, in degrees: the yaw of its rotation at tb against the one at ta
 * (D = M(tb) x M(ta)^T, so a root that leans or crouches does not seem to turn) */
static float root_turn(Mon *mo, double ta, double tb)
{
    float tr[3], qa[4], qb[4], zero[3] = { 0, 0, 0 };
    if (root_at(mo, ta, tr, qa) != 0 || root_at(mo, tb, tr, qb) != 0)
        return 0;
    Mat4 a = mat_tq(zero, qa), b = mat_tq(zero, qb);
    float d00 = 0, d10 = 0;
    for (int k = 0; k < 3; k++) {
        d00 += b.m[4 * k] * a.m[4 * k];
        d10 += b.m[4 * k + 1] * a.m[4 * k];
    }
    return atan2f(d10, d00) * 57.2957795f;
}

static float root_yaw(Mon *mo, double t)
{
    float tr[3], q[4];
    if (root_at(mo, t, tr, q) != 0)
        return 0;
    float zero[3] = { 0, 0, 0 };
    Mat4 m = mat_tq(zero, q);
    return atan2f(m.m[4 * mo->yaw_axis + 1], m.m[4 * mo->yaw_axis]) * 57.2957795f;
}

/* the monster plays the clip of the runner's state, from the runner's time */
static void play_state(Mon *mo)
{
    Viewer *v = mo->v;
    const MoveState *st = &mo->graph.states[mo->run.state];
    if (scene_monster_play(&v->scene, mo->slot, st->clip) != 0)
        printf("%s: no clip %s\n", st->name ? st->name : "?", st->clip ? st->clip : "-");
    mo->entry_yaw = root_yaw(mo, clip_at(mo, st, mo->run.time));
    mo->entry_t = clip_at(mo, st, mo->run.time);
    if (getenv("RAMPAGE_ROOT")) {
        float rt[3], rq[4];
        if (root_at(mo, clip_at(mo, st, mo->run.time), rt, rq) == 0)
            printf("  root of %s: %.2f %.2f %.2f\n", st->clip, rt[0], rt[1], rt[2]);
    }
    mo->armed = 0;                                    /* 0x2E7F70: a clip starts with no attack armed, on the ground */
    mo->ev16 = mo->ev16x = 0;                         /* (and no event record kept) */
    mo->ev15 = mo->ev15x = mo->ev15r = 0;
    mo->mode = MODE_GROUND;                           /* (its events set the mode it is in) */
    climb_clip_started(&mo->mount, st->name);
    tail_blend(&mo->tail, st->tail, st->blend);      /* 0x2E6884: to node +0x10 over the request's +0x4, which is
                                                        always the node's blend (0x2ECC08), cut or not */
    mo->mirror = mo->mirror_queued;                    /* 0x2E67E8: a clip starts in the queued mirror state */
    mo->input.mirrored = mo->mirror;
    scene_monster_mirror(&v->scene, mo->slot, mo->mirror);
    float q[4];
    if (root_at(mo, clip_at(mo, st, mo->run.time), mo->entry_root, q) != 0)
        memset(mo->entry_root, 0, sizeof mo->entry_root);
}

/* setFacialAnim (0x2E9200, 0x2E9350): the state node of that name plays its clip on the face, from its start; a name
 * no node has does nothing */
static void clip_event(Mon *mo, const MoveEvent *e);

/* the face clip's events, those after `from` up to `to` (shares of the clip): the face's own event player (monster
 * +0x7DE8) through the body's handler 0x2E7840 [R]: 0x2F45D0 at the face's start (setFacialAnim 0x2E9470, its
 * replay 0x2E964C) fires those at 0, 0x2F4500 each frame (the update, 0x2DE600) those its time reaches.
 * Expression_Burp's belch, Expression_Spew's fire and smoke (type 12), their sounds (6), Expression_Barf's doBarf */
static void face_events(Mon *mo, const MoveNode *n, float from, float to)
{
    for (int i = 0; n && i < n->nevents; i++)
        if (n->events[i].at > from && n->events[i].at <= to)
            clip_event(mo, &n->events[i]);
}

static void face_start(Mon *mo)
{
    Viewer *v = mo->v;
    const FaceRec *f = &mo->faces.list[0];
    const MoveNode *n = f->node;
    int r = scene_monster_face(&v->scene, mo->slot, n->clip, f->head);
    if (getenv("RAMPAGE_TRACE"))
        printf("face %s: %s -> %d\n", f->name, n->clip, r);
    if (r != 0)
        return;
    mo->face = n;
    scene_monster_face_blend(&v->scene, mo->slot, n->blend);   /* 0x2FF910: from the face as it was, over the node's blend */
    mo->face_ev = 0;
    face_events(mo, n, -1, 0);
}

static void face_play(Mon *mo, const char *name, int head, int loop, int count)
{
    const MoveNode *n = moves_node(&mo->graph, name);
    if (n && n->clip && face_set(&mo->faces, n, name, head, loop, count) == FACE_START)
        face_start(mo);
}

/* Monsters.bsf's DefaultFacialAnim: Expression_Idle, looping, the head itself left to the body */
static void face_default(Mon *mo)
{
    face_play(mo, "Expression_Idle", 0, 1, 0);
}

/* the face clip's clock: 0 .. 1 at its node's rate; at its end (0x2E95D0) it plays again while it loops or has
 * plays left, else the next queued face, else the default face (look.c); the delayed belches (appendBelch) */
static void face_step(Mon *mo, float dt)
{
    Viewer *v = mo->v;
    if (!mo->face || !mon_in(mo))
        return;
    if (face_tick(&mo->faces, dt))
        face_play(mo, "Expression_Burp", 0, 0, 1);
    float ev = mo->face_ev;                          /* 0x2F4500: the node's rate x dt x the speed (+0xA8C) */
    mo->face_ev += mo->face->rate * dt * mo->power.speed;
    face_events(mo, mo->face, ev, mo->face_ev);
    Instance *in = mon_in(mo);
    in->face_t += mo->face->rate * dt;
    if (in->face_t < 1)
        return;
    switch (face_clip_end(&mo->faces)) {
    case FACE_RESTART:
        in->face_t = 0;
        scene_monster_face_blend(&v->scene, mo->slot, mo->face->blend);
        mo->face_ev = 0;                             /* 0x2E964C */
        face_events(mo, mo->face, -1, 0);
        break;
    case FACE_START: face_start(mo); break;
    default: face_default(mo); break;
    }
}

/* the most HP: MONSTER_DATA +0x138 (0x2D9374 clamps a hit to it, addStats 0x2C51A0, the respawn 0x2D3930) [R] */
static float mon_hp_max(const Mon *mo)
{
    return mo->have_graph && mo->graph.hp_max > 0 ? mo->graph.hp_max : 200;
}

static void mon_fx_event(Mon *mo, const MoveEvent *e);
static void mon_fx_stop_all(Mon *mo);
static void mon_fx_follow(Mon *mo);
static int mon_fx_named(Mon *mo, const char *name, const char *node);
static void mon_fart(Mon *mo);
static void mon_fart_later(Mon *mo, float delay);
static void mon_function(Mon *mo, const char *name);
static void force_state(Mon *mo, const char *name);
static void mon_rage_fx(Mon *mo, int on);
static int rage_fx_at(Mon *mo, float at[3]);
static void mon_nick_fx(Mon *mo);
static Group army_find(void *ctx, const char *book, const char *name);

static void start_moves(Mon *mo)
{
    Viewer *v = mo->v;
    mon_fx_stop_all(mo);
    mon_rage_fx(mo, 0);
    mo->nick = mo->monster >= 0 && !_stricmp(v->monsters[mo->monster], "Nick");   /* the setup's (0x2F5D80) */
    if (mo->have_graph) {
        moves_stop(&mo->run);
        moves_free(&mo->graph);
        mo->have_graph = 0;
    }
    if (!mon_in(mo) || !v->scene.mon[mo->slot].bundle || moves_read(v->scene.mon[mo->slot].bundle, &mo->graph) != 0)
        return;
    mo->have_graph = 1;
    {                                                /* the property book's footfalls (MonsterFx), DisableFootDrag */
        char book[64];
        snprintf(book, sizeof book, "%s", mo->graph.props ? mo->graph.props : "");
        char *dot = strrchr(book, '.');
        if (dot)
            *dot = 0;
        foot_props_default(&mo->feet_fx);
        Group g = book[0] ? city_book_group(v->city, book, "MonsterFx") : (Group){ 0 };
        foot_props_read(&mo->feet_fx, g);
        const char *rage = g.at ? group_str(g, "RageFx") : NULL;   /* (0x2F5D80) */
        snprintf(mo->rage_fx_name, sizeof mo->rage_fx_name, "%s", rage ? rage : "");
        g = book[0] ? city_book_group(v->city, book, "MiscData") : (Group){ 0 };
        foot_init(&mo->feet, g.at && group_num(g, "DisableFootDrag", 0) != 0);
        if (mon_in(mo))                   /* Use3LinkFootIK, RTWSmoothFactor, the ankle (0x2E5640) */
            foot_ik_props(&mo->feet, g, &mon_in(mo)->tree);
        memset(&mo->feet_ik, 0, sizeof mo->feet_ik);
        mo->feet_ik.links = mo->feet.links;
        memset(&mo->tail, 0, sizeof mo->tail);       /* found on the next posing (0x2FABC0) */
        for (int i = 0; i < mo->weapons.n; i++)      /* a burst on stops with the weapons it had */
            if (mo->weapons.w[i].firing && mo->weapons.w[i].fx >= 0 && v->fxw)
                fx_stop(v->fxw, mo->weapons.w[i].fx);
        g = book[0] ? city_book_group(v->city, book, "WeaponList") : (Group){ 0 };   /* its weapons (0x2D4C40) */
        army_mon_weapons_read(&mo->weapons, g, army_find, v);
    }
    if (mo->have_ag)
        dir_graph_free(&mo->ag);
    mo->have_ag = dir_graph_read(v->scene.mon[mo->slot].bundle, &mo->ag) == 0;
    dir_ai_init(&mo->ai);
    mo->is_ai = 0;
    moves_start(&mo->run, &mo->graph, moves_find(&mo->graph, "Ground_Idle"));
    mo->face = NULL;
    face_init(&mo->faces);
    memset(&mo->head, 0, sizeof mo->head);
    look_limits_default(&mo->look_lim);
    mo->friction = MONSTER_FRICTION;
    mo->air_hits = 0;                                /* (0x2E9D90, the setup's only) */
    face_default(mo);                                 /* Monsters::PostInit */
    /* [R 0x323180] the hands' +0x4 is 0.75; Lizzie.bsf's own PostInit (0x2E75C0 asks "<monster>::PostInit" first)
     * sets setHandThickness(0.5) (binding 95, 0x2AB330: +0xC780) in place of Monsters' setMaxJumpSpinCount(2), whose
     * +0xABC nothing reads */
    mo->hand_thick = mo->monster >= 0 && !_stricmp(v->monsters[mo->monster], "Lizzie") ? 0.5f : 0.75f;
    memset(&mo->input, 0, sizeof mo->input);
    memset(mo->vel, 0, sizeof mo->vel);
    mo->mirror = mo->mirror_queued = 0;
    memset(&mo->mount, 0, sizeof mo->mount);
    memset(mo->hand, 0, sizeof mo->hand);
    mo->held = -1;
    mo->held_unit = -1;
    mo->force_to = 0;
    mo->held_vehicle = -1;
    mo->held_rail = -1;
    mo->held_parade = -1;
    mo->held_sobj = -1;
    mo->pending_slot = -1;
    mo->defeated = 0;
    mo->human = mo->dropping = 0;                    /* (a game quit with its streaker running or dropping in: the
                                                      * next one's camera framed no monster, its view never set) */
    mo->scale = mo->scale_to = 1;
    mo->scale_left = 0;
    mo->hp = mon_hp_max(mo);
    if (getenv("RAMPAGE_HP"))                        /* a test's start (the game starts at MONSTER_DATA +0x138) */
        mo->hp = (float)atof(getenv("RAMPAGE_HP"));
    mo->energy = 0;
    edible_power_init(&mo->power);
    power_from_profile(mo);
    memset(mon_shake(mo), 0, sizeof *mon_shake(mo));
    mo->mount.at.building = mo->mount.cand.building = -1;
    memset(mo->air, 0, sizeof mo->air);
    memset(mo->stick, 0, sizeof mo->stick);
    memcpy(mo->frame, v->view.rows, sizeof mo->frame);
    mo->mz = 0;
    mo->mode = MODE_GROUND;
    mo->no_gravity = 0;
    mo->column = -1;
    const Instance *in = mon_in(mo);
    mo->yaw_axis = 0;                                 /* the most level axis of the root at rest */
    for (uint32_t i = 0; i < in->tree.n; i++)
        if (in->tree.nodes[i].parent < 0) {
            float zero[3] = { 0, 0, 0 };
            Mat4 m = mat_tq(zero, in->tree.nodes[i].q);
            for (int k = 1; k < 3; k++)
                if (fabsf(m.m[4 * k + 2]) < fabsf(m.m[4 * mo->yaw_axis + 2]))
                    mo->yaw_axis = k;
            break;
        }
    if (v->play)                                     /* with --clip the chosen clip stays until G */
        play_state(mo);
}

/* the world's side of a monster's start (with player 1's): the game's next block, the upgrade, the shakes */
static void play_reset(Viewer *v)
{
    v->game_block = -1;
    v->upgrade_fx = -1;
    if (!v->game_built) {
        v->next_block = -1;
        v->block_shift = 0;
        for (int k = 0; k < 3; k++)
            v->rubble_fx[k] = -1;
    }
    memset(v->shakes, 0, sizeof v->shakes);
    for (int i = 0; i < 64; i++) v->last_collapse[i] = -1;
}

/* Monster.isFemale (script method 0x96, 0x2AC0E0): the monster's +0xAB6, which its setup (0x2E98C0) sets from
 * MiscData|IsFemale of its property book (MONSTER_DATA +0xAA4): Amanda, Jill, Lizzie, Natalie, Snapper, Venus */
static int monster_female(Mon *mo)
{
    Viewer *v = mo->v;
    char book[64];                                   /* "George_prop.prb": the book George_prop(.bdp) */
    snprintf(book, sizeof book, "%s", mo->have_graph && mo->graph.props ? mo->graph.props : "");
    char *dot = strrchr(book, '.');
    if (dot)
        *dot = 0;
    Group g = book[0] ? city_book_group(v->city, book, "MiscData") : (Group){ 0 };
    return g.at && group_num(g, "IsFemale", 0) != 0;
}

/* gWorld.isSphereVisible (0x16C0A0, the camera's six planes) for people.c, army.c and boss.c: the sphere inside the
 * game camera's view (its fov across the 640 x 480 GUI screen's height, 4:3 across) */
static int people_visible(void *ctx, const float p[3], float radius)
{
    const Viewer *v = ctx;
    const float (*r)[3] = (const float (*)[3])v->view.rows;
    float d[3] = { p[0] - v->view.pos[0], p[1] - v->view.pos[1], p[2] - v->view.pos[2] };
    float x = d[0] * r[0][0] + d[1] * r[0][1] + d[2] * r[0][2], y = d[0] * r[2][0] + d[1] * r[2][1] + d[2] * r[2][2];
    float z = d[0] * r[1][0] + d[1] * r[1][1] + d[2] * r[1][2];
    if (z < -radius)
        return 0;
    float t = tanf((v->view.fov > 0 ? v->view.fov : 1) / 2), zz = z > 0.1f ? z : 0.1f;
    return fabsf(x) - radius <= zz * t * 4 / 3 && fabsf(y) - radius <= zz * t;
}

static int edible_cell_state(void *ctx, int building, int storey, int column);
static int edible_cell_where(void *ctx, int building, int storey, int column, float out[3]);
static int edible_cell_follow(void *ctx, int building, int storey, int column, float out[3]);
static int edible_call(Mon *mo, const char *call, const float *xyz);
static void monster_stats(Mon *mo, float health, float energy);
static int boss_weapon_make(void *ctx, ArmyWeapon *w, Group g);
static void boss_weapon_tick(void *ctx, ArmyWeapon *w, int may, const float from[3], const float dir[3],
                             const float frame[3][3], int target, float dt, int part);

/* the army's host (army.h): books, clip lengths and model bounds from the city */
static Group rubble_tmpl(void *ctx, const char *name)
{
    return city_template(((Viewer *)ctx)->city, name);
}
static int rubble_box(void *ctx, const char *shape, float min[3], float max[3])
{
    return city_shape_box(((Viewer *)ctx)->city, shape, min, max) == 0 ? 0 : -1;
}
/* the city's WorldData (<City>_Props): its MonsterRevealDef */
static Group world_data(Viewer *v)
{
    char book[64];
    snprintf(book, sizeof book, "%s_Props", CITIES[v->city_index]);
    return city_book_group(v->city, book, "WorldData");
}
static int game_all_revealed(void *ctx)
{
    Viewer *v = ctx;
    return reveal_all_revealed(world_data(v), &v->roster);
}
static void game_block_rubble(void *ctx, const char *name)   /* the block left's rubble (0x303C90) */
{
    Viewer *v = ctx;
    int k = -1;
    for (uint32_t i = 0; name && i < v->nblocks; i++)
        if (!_stricmp(v->blocks[i], name))
            k = (int)i;
    static RubbleHost rh;
    rh.tmpl = rubble_tmpl;
    rh.shape_box = rubble_box;
    rh.ctx = v;
    rubble_load(&v->rubble, v->scene.block, k >= 0 && k != (int)v->block, &rh);
}
static int game_rubble_point(void *ctx, const float pos[3], float radius, float out[3])
{
    Viewer *v = ctx;
    uint32_t *rng = game_rng(v->game);
    if (rng)                                         /* the world's one state (gWorld +0x828) */
        v->rubble.seed = *rng;
    int found = rubble_find_point(&v->rubble, pos, radius, out);
    if (rng)
        *rng = v->rubble.seed;
    return found;
}
/* the cryotubes of the block: the edibles get them in their cells */
static void add_tubes(Viewer *v)
{
    for (int i = 0; v->edibles && i < v->ntubes; i++)
        if (v->tubes[i].building >= 0 && v->tubes[i].building < (int)v->scene.nbuildings) {
            int s = edible_add_cryotube(v->edibles, v->tubes[i].building, v->tubes[i].floor, v->tubes[i].cell);
            if (getenv("RAMPAGE_TRACE"))
                printf("  cryotube: building %d (%d storeys, %d wide), floor %d, cell %d: %s\n", v->tubes[i].building,
                       v->scene.buildings[v->tubes[i].building].b.n, v->scene.buildings[v->tubes[i].building].b.width,
                       v->tubes[i].floor, v->tubes[i].cell, s >= 0 ? "placed" : "no such cell");
        }
    v->ntubes = 0;
}

static Group army_find(void *ctx, const char *book, const char *name)
{
    return city_book_group(((Viewer *)ctx)->city, book, name);
}
static int army_shape_box(void *ctx, const char *shape, float min[3], float max[3])
{
    return city_shape_box(((Viewer *)ctx)->city, shape, min, max) == 0;
}
/* a FireEffect's collision rod (fx.c fx_rod, 0x1F9290): the beams' reach */
static int effect_rod(void *ctx, const char *effect, float dir[3], float *speed, float *length, float *cycle,
                      int *loops)
{
    const Viewer *v = ctx;
    FxRod r;
    if (!v->fxlib || !fx_rod(v->fxlib, effect, &r))
        return 0;
    memcpy(dir, r.dir, sizeof r.dir);
    *speed = r.speed;
    *length = r.length;
    *cycle = r.cycle;
    *loops = r.loops;
    return 1;
}

static int model_sphere(void *ctx, const char *shape, float centre[3], float *radius)
{
    return city_shape_sphere(((Viewer *)ctx)->city, shape, centre, radius) == 0;
}
static int army_node_world(void *ctx, const ArmyUnit *u, const char *node, float out[16])
{
    Viewer *v = ctx;                                 /* the weapon's node on the unit's look as drawn */
    int k = v->army ? (int)(u - army_unit(v->army, 0)) : -1;
    int ok = v->army && (scene_unit_node(&v->scene, k, node, out) == 0 ||   /* else the look of what it plays now */
                         scene_unit_node_now(&v->scene, k, u, node, out) == 0);
    if (getenv("RAMPAGE_NODES"))
        printf("  %s node %s: %s (%s, %s)\n", u->name ? u->name : "?", node, ok ? "found" : "not found", u->anim,
               u->shape ? u->shape : "-");
    return ok;
}
static int boss_node_world(void *ctx, const BossPart *p, const char *node, float out[16])
{
    Viewer *v = ctx;                                 /* a node of the part's look as drawn */
    int ok = 0;
    for (int i = 0, n = v->boss ? boss_count(v->boss) : 0; i < n && !ok; i++)
        if (boss_part(v->boss, i) == p)
            ok = scene_boss_node(&v->scene, i, node, out) == 0;
    if (getenv("RAMPAGE_NODES"))
        printf("  %s node %s: %s\n", p->name ? p->name : "?", node, ok ? "found" : "not found");
    return ok;
}
static int vehicle_node_world(void *ctx, const VehObj *o, const char *node, float out[16])
{
    Viewer *v = ctx;                                 /* a node of the object's look as drawn */
    int ok = v->veh && scene_vehicle_node(&v->scene, (int)(o - vehicle_obj(v->veh, 0)), node, out) == 0;
    if (getenv("RAMPAGE_NODES"))
        printf("  %s node %s: %s\n", o->name ? o->name : "?", node, ok ? "found" : "not found");
    return ok;
}
static float parade_clip_seconds(void *ctx, const char *anim, const char *shape)
{
    return city_clip_seconds(((Viewer *)ctx)->city, anim, shape);
}
static float army_clip_seconds(void *ctx, const char *anim, const char *shape)
{
    (void)ctx; (void)anim; (void)shape;
    return 0;                                        /* army.c falls back to the clip's own length */
}

/* a world point on the GUI's 640 x 480 screen through the frame's camera (render_gl.c's look and perspective; the GUI
 * at the window's height, centred): the floating scores */
static int hud_project(void *ctx, const float p[3], float screen[2], float *px_per_unit)
{
    const Viewer *v = ctx;
    const Camera *c = &v->hud_cam;
    float cp = cosf(c->pitch), f[3] = { cp * cosf(c->yaw), cp * sinf(c->yaw), sinf(c->pitch) };
    float s[3] = { f[1], -f[0], 0 }, sl = sqrtf(s[0] * s[0] + s[1] * s[1]);
    s[0] /= sl; s[1] /= sl;
    float u[3] = { s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2], s[0] * f[1] - s[1] * f[0] };
    if (c->use_rows)
        for (int k = 0; k < 3; k++) { s[k] = c->rows[0][k]; f[k] = c->rows[1][k]; u[k] = c->rows[2][k]; }
    float d[3] = { p[0] - c->pos[0], p[1] - c->pos[1], p[2] - c->pos[2] };
    float x = d[0] * s[0] + d[1] * s[1] + d[2] * s[2], y = d[0] * u[0] + d[1] * u[1] + d[2] * u[2];
    float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    if (z < 0.5f)
        return 0;
    float k = 1.0f / tanf(c->fov_deg * 3.14159265f / 360.0f) / z;
    screen[0] = HUD_SCREEN_W / 2 + HUD_SCREEN_H / 2 * x * k;
    screen[1] = HUD_SCREEN_H / 2 * (1 - y * k);
    *px_per_unit = HUD_SCREEN_H / 2 * k;
    return 1;
}

/* the players' HUDs closed (player 1's is the screen's, Viewer.hud) */
static void huds_close(Viewer *v)
{
    for (int k = 0; k < MONSTERS; k++) {
        if (v->mon[k].hud != v->hud)
            hud_close(v->mon[k].hud);
        v->mon[k].hud = NULL;
    }
    hud_close(v->hud);
    v->hud = NULL;
}

static int monster_index(const Viewer *v, const char *name);

/* 0x2B6BE0 / 0x2B72B0: the slot after the players gets the monster to reveal (roster index rm), hidden and out of
 * the play (+0x108 = +0x109 = 1) until startReveal */
static void extra_add(Viewer *v, int k, int rm)
{
    int mi = monster_index(v, v->roster.name[rm]);
    if (k != v->nmon || k >= MONSTERS || mi < 0 || v->scene.nmon != k)
        return;
    Mon *mo = &v->mon[k];
    mo->v = v;
    mo->slot = k;
    mo->monster = mi;
    mo->bot = 0;
    mo->drive = DRIVE_COMPUTER;
    mo->hud = NULL;
    mo->mx = 0; mo->my = -10; mo->mz = 0;
    mo->heading = 180;
    memset(&mo->pad, 0, sizeof mo->pad);
    mo->human = mo->dropping = 0;
    mo->streaker = -1;
    mo->reveal = 1;
    mo->shown = 0;
    if (city_monster(v->city, &v->scene, v->monsters[mi], -1, mo->mx, mo->my, mo->heading) != k)
        return;
    start_moves(mo);
    v->extra = k;
    if (mon_in(mo))
        mon_in(mo)->hidden = 1;
}

/* 0x2D3F90: the extra monster hides and leaves the world (stopReveal, the Continue) */
static void extra_hide(Viewer *v, int k)
{
    if (k < 0 || k != v->extra)
        return;
    Mon *mo = &v->mon[k];
    mo->shown = 0;
    mo->is_ai = 0;
    dir_ai_clear(&mo->ai);
    mo->scale = mo->scale_to = 1;
    mo->scale_left = 0;
    edible_power_init(&mo->power);
    if (mon_in(mo))
        mon_in(mo)->hidden = 1;
    if (getenv("RAMPAGE_TRACE"))
        printf("  reveal: [%d] hidden\n", k + 1);
}

/* the monsters in the world: the players and bots, and the one being revealed while it shows */
static int mon_world(const Viewer *v)
{
    return v->nmon + (v->extra >= 0 && v->mon[v->extra].shown);
}

/* the walk-on's calls (reveal.c's RevealEvents) on the extra monster */
static void reveal_events(Viewer *v)
{
    RevealEvent ev[16];
    int n = reveal_walk_events(&v->rwalk, ev, 16);
    for (int i = 0; i < n; i++) {
        const RevealEvent *e = &ev[i];
        int k = e->type == RW_HIDE ? e->slot : v->rwalk.slot;
        if (k < 0 || k != v->extra)
            continue;
        Mon *mo = &v->mon[k];
        switch (e->type) {
        case RW_SHOW:                                /* at the tube, facing pi (setFacingZ), into the world */
            mo->mx = e->pos[0]; mo->my = e->pos[1]; mo->mz = e->pos[2];
            mo->heading = e->v[0] * 57.2957795f;
            mo->mode = MODE_GROUND;
            memset(mo->vel, 0, sizeof mo->vel);
            mo->shown = 1;
            if (mon_in(mo))
                mon_in(mo)->hidden = 0;
            scene_monster_place(&v->scene, k, mo->mx, mo->my, mo->mz, mo->heading);
            break;
        case RW_AI: mo->is_ai = 1; break;
        case RW_MOVE:
            if (mo->have_graph && mo->have_ag)
                dir_ai_play_move(&mo->ai, e->name ? moves_find(&mo->graph, e->name) : -1, 0);
            break;
        case RW_SCALE:                               /* setDynamicScale(scale, seconds) */
            mo->scale_from = mo->scale;
            mo->scale_to = e->v[0] > 0 ? e->v[0] : 1;
            mo->scale_time = mo->scale_left = e->v[1] > 0 ? e->v[1] : 0;
            if (mo->scale_left <= 0)
                mo->scale = mo->scale_to;
            break;
        case RW_WALK: {                              /* walkTo(pos, radius, "", "Monsters::onRevealNIEnd") */
            float to[3] = { e->pos[0], e->pos[1] + v->block_shift, e->pos[2] };
            if (mo->have_ag)
                dir_ai_walk_to(&mo->ai, to, e->v[0], 1);
            break;
        }
        case RW_SPEED: edible_power_speed(&mo->power, e->v[0], e->v[1]); break;
        case RW_HIDE: extra_hide(v, k); break;
        }
        if (getenv("RAMPAGE_TRACE"))
            printf("  reveal walk %d (%s) [%d] at %.1f %.1f %.1f\n", e->type, e->name ? e->name : "-", k + 1,
                   e->pos[0], e->pos[1], e->pos[2]);
    }
}

static void block_left(Viewer *v);

static void build(Viewer *v)
{
    if (v->have_scene)
        scene_free(&v->scene);
    if (!v->game_built && v->fxw)                    /* a new world (a city, a game): the last one's effects gone
                                                      * with it (its fountains' sprays, its fires went on where they
                                                      * stood, in the new city's street) */
        fx_clear(v->fxw);
    if (!v->game_built) {                            /* a new world: its scripts' onInit (Traffic's crowds) */
        /* RWorld's reset, before the block's edibles are dealt (their kinds and cells draw on it): gWorld +0x82C
         * from GameStatus seed[1]. Seeded after them, the first world of a run dealt from the unseeded state, all
         * zeros: every window the list's first kind (NY01: all Hazard_Rage) */
        dir_ai_seed(v->prof.current.seed[1]);
        city_new_world(v->city);
        /* [R Traffic.scr onInit] the world's init (0x2852E0 -> WorldScript +0x28: slot 0) before any block is made:
         * fNextExitTime = 5 + 5 frand, the HiddenVehicle's x = -100 + 100 frand, and its Construct's frand
         * (Vehicle.bsf's pulse, 1 s for a hidden one), from the world's stream */
        v->traffic_exit = 5.0f + wrand_frand() * 5.0f;
        v->hidden_x = -100.0f + wrand_frand() * 100.0f;
        (void)wrand_frand();
        v->traffic_acc = 0;
        memset(&v->hidden, 0, sizeof v->hidden);
    }
    v->have_scene = city_build(v->city, v->blocks[v->block], v->night, &v->scene) == 0;
    if (v->have_scene && v->game_built && v->left_block >= 0)
        city_build_left(v->city, v->blocks[v->left_block], &v->scene);
    if (!v->game_built) {                            /* a new world: its monster to reveal comes with reveal_load */
        v->extra = -1;
        memset(&v->rwalk, 0, sizeof v->rwalk);
    }
    for (int k = 0; k < v->nmon + (v->extra >= 0); k++) {   /* the monsters, slot by slot (player 1's: none, none at
                                                      * all), and the one to reveal */
        Mon *mo = &v->mon[k];
        if (v->have_scene && have_monster(v) && mo->monster >= 0 &&
            city_monster(v->city, &v->scene, v->monsters[mo->monster], k ? -1 : v->clip, mo->mx, mo->my,
                         mo->heading) == k)
            printf("%s: %s\n", v->monsters[mo->monster], v->scene.mon[k].clip ? v->scene.mon[k].clip : "-");
        {                                            /* the game's next block: its goals and AI go on */
            DirAI ai = mo->ai;
            int is_ai = mo->is_ai;
            float hp = mo->hp, energy = mo->energy;
            EdiblePower power = mo->power;
            start_moves(mo);
            if (v->game_built) {
                mo->ai = ai;
                mo->is_ai = is_ai;
                mo->ai.bot.target = mo->ai.bot.avoid = -1;   /* (the buildings are the new block's) */
                mo->ai.bot.have_cell = mo->ai.bot.use_hit = 0;
                /* the monster outlives the block (0x2B6580 does not touch it): its HP (+0xA4C, set only by the
                 * world's start 0x2E98C0, a Continue 0x32528C, respawns, hits and stats), energy (+0xA50), rage and
                 * speed go on in the next block [R] */
                if (hp > 0) {
                    mo->hp = hp;
                    mo->energy = energy;
                    power.abilities = mo->power.abilities;   /* (the profile's, read again: an upgrade learnt) */
                    power.cheats = mo->power.cheats;
                    mo->power = power;
                    if (mo->power.rage)
                        mon_rage_fx(mo, 1);
                }
            }
        }
        if (getenv("RAMPAGE_TRACE") && mo->have_graph)
            printf("properties %s%s\n", mo->graph.props ? mo->graph.props : "-", monster_female(mo) ? ", female" : "");
    }
    if (v->extra >= 0 && !v->mon[v->extra].shown && mon_in(&v->mon[v->extra]))
        mon_in(&v->mon[v->extra])->hidden = 1;
    if (v->nmon && v->mon[0].have_graph)
        play_reset(v);
    if (v->have_scene && v->game_built && v->music_state) {   /* the game's next block: BeginBlock's music, its Music
                                                      * (sound.c music_for_state, 0x323CE0) */
        music_for_state(v->music_state, v->scene.music);
        v->music_state = 0;
    } else if (v->have_scene && v->scene.music && !have_monster(v)) {   /* no game: the block's music, looping
                                                      * (silent unless started); a game's comes with its states */
        music_stop(0);
        music_play(v->scene.music);
    }
    if (v->army) {
        army_free(v->army);
        v->army = NULL;
    }
    v->nfxa = 0;
    v->narmed_people = 0;
    v->nppfx = 0;
    if (v->have_scene && have_monster(v)) {          /* the army comes for the monsters (the block's AIDef) */
        static ArmyHost host;
        host.find = army_find;
        host.clip_seconds = army_clip_seconds;
        host.shape_box = army_shape_box;
        host.shape_sphere = model_sphere;
        host.visible = people_visible;
        host.node_world = army_node_world;
        host.rod = effect_rod;
        host.ctx = v;
        v->army = army_new(&host);
        if (v->army)
            army_set_collide(v->army, &v->cw);
        if (v->army) {
            army_load_block(v->army, v->scene.block);
            army_activate(v->army, !v->game);        /* the game's BeginBlockNI activates it (GE_AI_ACTIVATE): the
                                                      * next block is built as the results start (0x334C40 leaves
                                                      * gAIMgr's bit 8 off), its clocks still [R] */
        }
    }
    v->scene.army = v->army;
    if (v->game_built)                               /* the HiddenVehicle is Traffic's, not the block's: it drives on */
        vehicle_hidden_save(v->veh, &v->hidden);
    vehicle_free(v->veh);                            /* the traffic of the three block slots, the roof rockets */
    v->veh = NULL;
    v->nvfx = 0;
    for (int k = 0; k < v->nmon; k++)
        v->mon[k].held_vehicle = v->mon[k].held_rail = -1;
    if (v->have_scene) {
        static VehicleHost vh;
        vh.find = army_find;
        vh.shape_box = army_shape_box;
        vh.shape_sphere = model_sphere;
        vh.rod = effect_rod;
        vh.node_world = vehicle_node_world;
        vh.ctx = v;
        v->veh = vehicle_new(&vh);
        if (v->veh)
            vehicle_set_collide(v->veh, &v->cw);
        if (v->veh) {
            if (v->army)
                vehicle_bind_army(v->veh, v->army);
            for (int k = 0; k < 3; k++)
                if (v->scene.traffic_def[k].at)
                    vehicle_load_traffic(v->veh, k, v->scene.traffic_def[k]);
            vehicle_level_start(v->veh, v->hidden_x, &v->hidden);
            for (int k = 0; k < v->scene.nrockets; k++)
                vehicle_spawn(v->veh, v->scene.rockets[k].name, v->scene.rockets[k].pos, v->scene.rockets[k].facing,
                              v->scene.rockets[k].building);
        }
    }
    v->scene.traffic = v->veh;
    for (int k = 0; k < v->nweather_fx; k++)         /* the last block's weather: in the game its FollowEffect goes
                                                      * with its object at the block's end (0x2B66C0) and the next
                                                      * block's Weather starts at once (0x302AF0) [R]; here the old
                                                      * one runs on until the new block is built */
        if (v->fxw)
            fx_stop(v->fxw, v->weather_fx[k]);
    v->nweather_fx = 0;
    v->weather_due = v->have_scene;
    streetobj_free(v->sobj);                         /* the block's StreetObject.bsf and RoofItem.bsf objects */
    v->sobj = NULL;
    v->nsofx = 0;
    for (int k = 0; k < v->nmon; k++)
        v->mon[k].held_sobj = -1;
    if (v->have_scene) {
        static StreetHost sh;
        sh.shape_box = army_shape_box;
        sh.shape_sphere = model_sphere;
        sh.find = rubble_tmpl;                       /* a template by name: the geysers the fountains leave */
        sh.ctx = v;
        v->sobj = streetobj_new(&sh);
        for (int k = 0; k < SCENE_STREET_MAX; k++)   /* one a script makes has no scene entry */
            v->street_of[k] = -1;
        static StreetBuilding sbs[64];
        int nb = 0;
        for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
            memcpy(sbs[nb].min, v->scene.buildings[i].b.min, sizeof sbs[nb].min);
            memcpy(sbs[nb].max, v->scene.buildings[i].b.max, sizeof sbs[nb].max);
            sbs[nb].falling = v->scene.buildings[i].b.collapse >= 0;
        }
        for (int k = 0; k < v->scene.nstreet; k++) {
            const struct SceneStreetObj *so = &v->scene.street[k];
            v->sobj_of[k] = !v->sobj ? -1 : so->parent >= 0 ?   /* a billboard's attachment, after it */
                            streetobj_add_attachment(v->sobj, v->sobj_of[so->parent], so->t, so->world) :
                            streetobj_add(v->sobj, so->t, so->inst, so->world, so->building, so->attack);
            if (v->sobj_of[k] >= 0)
                v->street_of[v->sobj_of[k]] = k;
            if (v->sobj_of[k] >= 0 && so->nnode)     /* the crane's nodes */
                streetobj_nodes(v->sobj, v->sobj_of[k], (const float (*)[3])so->node, so->nnode);
        }
        if (v->sobj)
            streetobj_start(v->sobj, sbs, nb);
        if (getenv("RAMPAGE_TRACE"))
            printf("street objects: %d\n", v->sobj ? streetobj_count(v->sobj) : 0);
    }
    parade_free(v->parade);                          /* the block's balloons, bikers and food carts */
    v->parade = NULL;
    for (int k = 0; k < v->nmon; k++)
        v->mon[k].held_parade = -1;
    v->npfx = 0;
    if (v->have_scene) {
        static ParadeHost ph;
        ph.find = army_find;
        ph.shape_box = army_shape_box;
        ph.shape_sphere = model_sphere;
        ph.clip_seconds = parade_clip_seconds;
        ph.ctx = v;
        v->parade = parade_new(&ph);
        if (v->parade)
            parade_set_collide(v->parade, &v->cw);
        if (v->parade) {
            int n = parade_load_block(v->parade, v->scene.block);
            if (getenv("RAMPAGE_TRACE"))
                printf("parade: %d objects\n", n);
            parade_activate(v->parade, !v->game);    /* the game's BeginBlockNI activates it (GE_AI_ACTIVATE) */
        }
    }
    v->scene.parade = v->parade;
    rail_free(v->rail);                              /* the trains, cable cars and freighters (rail.c) */
    v->rail = NULL;
    v->nrfx = v->nrsnd = 0;
    for (int k = 0; k < v->nosnd; k++)              /* the objects go: their sounds with them (a new block) */
        sound_kill(v->osnd[k].h);
    v->nosnd = 0;
    if (v->have_scene) {
        static RailHost rh;
        rh.find = army_find;
        rh.shape_box = army_shape_box;
        rh.shape_sphere = model_sphere;
        rh.ctx = v;
        v->rail = rail_new(&rh);
        if (v->rail)
            rail_set_collide(v->rail, &v->cw);
        if (v->rail) {
            if (v->army)
                rail_bind_army(v->rail, v->army);
            rail_load_block(v->rail, v->scene.block);
            rail_activate(v->rail, !v->game);        /* (gAIMgr's record, the freighter's: as the army's) */
            static const char *const OBJ[] = { "ObjectInstance|", "AttackObjectInstance|", "XObjectInstance|" };
            for (int k = 0; k < 3; k++) {
                Group bd = k == 0 ? v->scene.block : v->scene.traffic_def[k];
                if (!bd.at)
                    continue;
                for (int j = 0; j < 3; j++) {
                    Group where;
                    Record rr[256];
                    uint32_t n = group_values(bd, OBJ[j], &where, rr, 256);
                    for (uint32_t i = 0; i < n; i++) {
                        Group o = record_group(where, rr[i]);
                        if (k && group_num(o, k == 1 ? "LoadWhenWaiting" : "LoadWhenDistant", 0) == 0)
                            continue;
                        const char *name = group_str(o, "Name");
                        if (!name || rail_kind_of(v->rail, name) < 0)
                            continue;
                        Group loc = group_sub(o, "Location");
                        Record lr[3];
                        uint32_t nl = loc.at ? group_items(loc, lr, 3) : 0;
                        float p[3];
                        for (uint32_t q = 0; q < 3; q++)
                            p[q] = q < nl ? record_num(lr[q]) : 0;
                        p[1] += 41.0f * (float)k;
                        int id = rail_spawn(v->rail, name, p, group_num(o, "ZRotation", 0) / 57.2957795f, o);
                        if (getenv("RAMPAGE_TRACE"))
                            printf("rail: %s at %.1f %.1f %.1f -> %d\n", name, p[0], p[1], p[2], id);
                    }
                }
            }
        }
    }
    v->scene.rail = v->rail;
    edible_free(v->edibles);                         /* the edibles behind the block's windows */
    v->edibles = NULL;
    v->nefx = 0;
    if (v->have_scene) {
        static EdibleHost eh;
        eh.find = army_find;
        eh.cell_state = edible_cell_state;
        eh.cell_place = edible_cell_where;
        eh.cell_follow = edible_cell_follow;
        eh.ctx = v;
        v->edibles = edible_new(&eh);
        if (v->edibles) {
            edible_load_block(v->edibles, v->scene.block);
            for (uint32_t i = 0; i < v->scene.nbuildings; i++) {   /* 0x3196F0: a building's window slots, then
                                                      * its edibles, the draws in that order */
                const SceneBuilding *sb = &v->scene.buildings[i];
                int before = edible_slots(v->edibles), own = 0;
                scene_occupant_slots(&v->scene, (int)i);
                edible_add_building(v->edibles, (int)i, sb->inst, sb->props, sb->b.width, sb->b.n);
                for (int k = before; k < edible_slots(v->edibles); k++) {   /* its edibles' new slots: those in a
                                                      * cell of no occupant's slot are slots of their own; the first
                                                      * occupant's slot of its cell takes its kind (+0xE, 0x319EAC) */
                    const EdibleSlot *es = edible_slot(v->edibles, k);
                    int shared = 0;
                    for (int j = 0; j < v->scene.nslots && !shared; j++) {
                        struct WindowSlot *ws = &v->scene.slots[j];
                        shared = ws->building == (int)i && ws->column == es->column &&
                                 building_live_of(&sb->b, ws->storey) == es->storey;
                        if (shared)
                            ws->edible = es->kind;
                    }
                    own += !shared;
                }
                scene_occupant_pad(&v->scene, own);
            }
            scene_occupant_sort(&v->scene);          /* 0x31A540: then the list sorted (0x31AE20) */            v->scene.slots_made = 1;
            if (getenv("RAMPAGE_TRACE")) {
                int nm = 0;
                for (int k = 0; k < v->scene.nslots; k++)
                    nm += v->scene.slots[k].military;
                printf("window slots: %d civilian, %d military; the list %d long, the soldiers' turn its first %d, "
                       "the civilians' the %d after\n", v->scene.nslots - nm, nm, v->scene.norder, v->scene.order_mil,
                       v->scene.order_civ);
            }
            add_tubes(v);                            /* a reveal's tubes, for the block now built */
            if (getenv("RAMPAGE_TRACE"))
                printf("edibles: %d kinds, %d slots\n", edible_kinds(v->edibles), edible_slots(v->edibles));
        }
    }
    v->scene.edibles = v->edibles;
    {                                                /* the buildings' shadows on the lot, cast at the block's start */
        static float boxes[SHADOW_MAX_BUILDINGS][6];
        int nb = 0;
        for (uint32_t i = 0; i < v->scene.nbuildings && nb < SHADOW_MAX_BUILDINGS; i++, nb++) {
            memcpy(boxes[nb], v->scene.buildings[i].b.min, 3 * sizeof(float));
            memcpy(boxes[nb] + 3, v->scene.buildings[i].b.max, 3 * sizeof(float));
        }
        shadow_ground_init(&v->sg, v->have_scene && group_num(v->scene.block, "EnableGroundShadow", 1) != 0, nb,
                           (const float (*)[6])boxes, NULL, NULL);
        for (int k = 0; k < v->nmon; k++)
            shadow_monster_init(&v->mon[k].smon);
    }
    for (int k = 0; k < v->nbsnd; k++)               /* their parts go: so do the sounds on them (0x288050) */
        sound_kill(v->bsnd[k].h);
    v->nbsnd = 0;
    boss_free(v->boss);                              /* the block's boss, and the ambient ones of the blocks behind */
    v->boss = NULL;
    v->nbfx = 0;
    if (v->have_scene && have_monster(v)) {
        static BossHost bh;
        bh.find = army_find;
        bh.clip_seconds = army_clip_seconds;
        bh.shape_box = army_shape_box;
        bh.shape_sphere = model_sphere;
        bh.visible = people_visible;
        bh.weapon_make = boss_weapon_make;
        bh.weapon_tick = boss_weapon_tick;
        bh.node_world = boss_node_world;
        bh.ctx = v;
        v->boss = boss_new(&bh);
        v->boss_block = 0;
        if (v->boss) {
            int n = boss_load_block(v->boss, v->scene.block);
            v->boss_block = n > 0;
            if (v->scene.traffic_def[1].at)
                n += boss_load_waiting(v->boss, v->scene.traffic_def[1], 41, 1);
            if (v->scene.traffic_def[2].at)
                n += boss_load_waiting(v->boss, v->scene.traffic_def[2], 82, 2);
            boss_activate(v->boss, !v->game);        /* (gAIMgr's record: as the army's) */
            if (getenv("RAMPAGE_TRACE"))
                printf("bosses: %d\n", n);
        }
    }
    v->scene.boss = v->boss;
    for (int k = 0; k < v->nmon; k++)
        v->mon[k].streaker = -1;
    if (!v->game_built) {                            /* the flow of the city from this block on: the play type, the
                                                      * players and the bots */
        game_free(v->game);
        v->game = NULL;
        if (v->have_scene && have_monster(v)) {
            static GameHost gh;
            gh.find = army_find;
            gh.all_revealed = game_all_revealed;
            gh.block_rubble = game_block_rubble;
            gh.rubble_point = game_rubble_point;
            gh.ctx = v;
            if (v->roster.n == 0) {                  /* ShellData's MonsterData: the 30, the profile's flags */
                reveal_roster_load(&v->roster, city_book_group(v->city, "ShellData", "MonsterData"));
                rubble_seed(&v->rubble, 1);
            }
            int pm[MONSTERS];                        /* 0x3236F0: the city's monster, not one of the players' */
            for (int k = 0; k < v->nmon; k++)
                pm[k] = reveal_roster_find(&v->roster, v->monsters[v->mon[k].monster]);
            for (int i = 0; i < v->roster.n; i++)    /* the roster's flags are the profile's (ShellData's order) */
                v->roster.flags[i] = profile_monster_flags(&v->prof, 0, i);
            int rm = reveal_load(&v->reveal, world_data(v), play_type(v) == GP_CAMPAIGN,
                                 v->nmon, pm, &v->roster);
            v->ntubes = 0;
            if (rm >= 0)                             /* with one player: the world's extra monster, hidden */
                extra_add(v, reveal_extra_slot(&v->reveal, v->nmon), rm);
            if (getenv("RAMPAGE_TRACE"))
                printf("reveal: %s%s\n", rm >= 0 ? v->roster.name[rm] : "-", v->extra >= 0 ? " (shown)" : "");
            GameConfig gc = { play_type(v), v->nmon, v->num_bots,
                              campaign_index(CITIES[v->city_index]), 7,
                              city_block_index(v->city, v->blocks[v->block]), v->prof.current.seed[0],
                              v->prof.cheats, v->prof.current.seed[1], { 0 } };
            memcpy(gc.cities_won, v->cities_won, sizeof gc.cities_won);   /* King of the World's so far */
            v->game = game_new(&gh, &gc);
            building_set_one_hit((v->prof.cheats & PCH_40000000) != 0);
            if (v->game && game_load_city(v->game, CITIES[v->city_index]) == 0) {
                game_start(v->game);
                /* the city's total and flags [R 0x33DB90]: a campaign city resumed at a later block (the start block
                 * the profile's status has) keeps its checkpoint's, a challenge missed before staying missed; any
                 * other start begins at 0 and "All Challenges Met" until one is not (the world's start clears them
                 * with the start block 0; the viewer's other starts, straight into a block, as a new city's) */
                int resumed = play_type(v) == GP_CAMPAIGN && v->prof.current.start_block > 0 &&
                              v->prof.current.start_block == game_start_block(v->game);
                uint32_t cf = resumed ? v->prof.current.city_flags : PC_AVAILABLE | PC_CHALLENGES;
                game_set_city_status(v->game, resumed ? v->prof.current.city_score : 0, cf);
                v->challenges_lost = !(cf & PC_CHALLENGES);
                /* [R] SinglePlayer's load (0x2B7F50) sets +0x610, so its first frame (0x2B7BC0) brings the music
                 * to the profile's volume over 250 ms (the loading screen stopped the menu's music, showLoading,
                 * and BeginBlock's music only starts at 0.05 or more, 0x323CE0) */
                v->music_now = v->music_vol = profile_music_volume(&v->prof) / 0.75f;
                sound_settings(v->sfx_vol * 0.75f, v->music_now * 0.75f, 250);
            } else {
                game_free(v->game);
                v->game = NULL;
            }
        }
    }
    if (v->have_scene && v->scene.folk)
        people_set_visible(v->scene.folk, people_visible, v);
    if (!v->game_built || !v->hud) {                 /* the game's next block keeps the HUDs (their results going) */
        huds_close(v);
        for (int k = 0; v->have_gui && v->have_scene && have_monster(v) && !v->no_hud && k < v->nmon; k++) {
            Mon *mo = &v->mon[k];                    /* each player's HUD (Hud<k>.ifc); player 1's has the screens */
            int pt = play_type(v);                   /* (GP_* and HUD_PLAY_* agree) the King modes: the blocks to win */
            HudConfig hc = { k, v->nmon, pt, v->game && (pt == GP_KING_CITY || pt == GP_KING_WORLD) ?
                             game_blocks_to_win(v->game) : 0, v->monsters[mo->monster] };
            mo->hud = hud_open(&v->gui_world, &v->gui_common, &hc);
            if (k == 0)
                v->hud = mo->hud;
            if (mo->hud) {
                /* three lives: the campaign's start [G] */
                hud_set(mo->hud, mo->hp, mon_hp_max(mo), mo->energy, 100, 0, 3, -1);
                hud_set_project(mo->hud, hud_project, v);
                if (v->game)                         /* the city's onPostInit: hidden till BeginBlockNI */
                    hud_hide(mo->hud);
                hud_set_language(mo->hud, v->lang);
                hud_set_settings(mo->hud, profile_sound_volume(&v->prof), profile_music_volume(&v->prof),
                                 profile_rumble(&v->prof));
                if (k == 0)                          /* the pads vibrate as the profile says [G] */
                    v->vibrate = profile_set_rumble(&v->prof, profile_rumble(&v->prof), 0);
                hud_add_bundle(mo->hud, v->scene.city);   /* <City>.loc: the block names, the challenge texts */
                if (v->have_fx_bundle)               /* the challenge icons that are fx.bdp's pictures (Chocolate_bar,
                                                      * Powerup_HotDog, moneybag, Ace_Sp ...: 20 of the 63 blocks') */
                    hud_add_bundle(mo->hud, &v->fx_bundle);
                hud_results_auto(mo->hud, v->unattended ? 2.0f : 0);
            }
        }
    }
    challenge_init(&v->challenge);                   /* the block's challenge (0x3468F0, at its activation) */
    challenge_upgrade_props(&v->challenge, city_template(v->city, "MonsterUpgrade"));
    if (v->have_scene && have_monster(v))           /* (player 1's monster: its upgrades [G with more players]) */
        challenge_load(&v->challenge, v->scene.block, v->game ? game_play_type(v->game) : GP_CAMPAIGN, v->boss_block,
                       v->monsters[v->mon[0].monster], v->upgrades[v->mon[0].monster & 63]);
    if (v->hud) {                                    /* Levels::onActivateBlock: ChallengeStatus (the count, or the
                                                      * boss bar on the boss block) and ChallengeText's card */
        HudChallenge ch;
        challenge_hud(&v->challenge, v->boss_block, &ch);
        ch.pause_scale = challenge_icon_scale(&v->challenge, 1);
        hud_challenge(v->hud, &ch);
    }
    if (v->game && !v->hud && !v->game_built)        /* no screens to answer the results: after 3 s */
        game_auto_results(v->game, 3.0f);
    Group tt;
    if (city_camera(v->city, &tt) == 0)
        cam_settings(&v->cam_settings, tt);
    if (!v->game_built) {                            /* the game's next block: the cameras go on */
        cam_init(&v->cam, &v->cam_settings);
        dircam_init(&v->cd, &v->cam);
        v->view = v->cd.out;
    }
    if (v->have_scene)
        rnd_scene(v->rnd, &v->scene);
    printf("%s %s%s: %u models drawn, %d people, %d rockets, %d not found\n", CITIES[v->city_index],
           v->blocks[v->block], v->night ? " (night)" : "", v->scene.nitems, v->scene.people, v->scene.nrockets,
           v->scene.missing);
}

/* block: the index in the city's block names (city_blocks); game_block >= 0 instead: a game's start block, a
 * BlockList index (the profile's saved one), its block found by name */
static int open_city(Viewer *v, int index, uint32_t block, int game_block)
{
    City *c = city_open(&v->vol, CITIES[index]);
    if (!c)
        return -1;
    if (v->have_scene) {
        scene_free(&v->scene);
        v->have_scene = 0;
    }
    city_close(v->city);
    v->city = c;
    v->city_index = index;
    if (v->roster.n)                                 /* its names are the old city's book's: read from this one's */
        reveal_roster_load(&v->roster, city_book_group(c, "ShellData", "MonsterData"));
    v->nblocks = city_blocks(c, v->blocks, 64);
    const char *at = game_block >= 0 ? city_game_block(c, game_block) : NULL;
    for (uint32_t i = 0; at && i < v->nblocks; i++)
        if (!_stricmp(v->blocks[i], at)) {
            block = i;
            break;
        }
    v->block = block < v->nblocks ? block : 0;
    build(v);
    return 0;
}

/* the viewer's answers to the graph's script checks (Monsters.bsf: there are no buildings, people or power-ups to
 * test against here, so only the monster's own abilities say yes) */
static int attach_world(Mon *mo, const char *name, Mat4 *out);
static int window_person(Mon *mo, const float *xyz);
static int window_soldier(Mon *mo, const float *xyz);

/* the middle of the players' x (0x336360, kept at gp - 0x6C70): the camera's reach is measured from it */
static float players_mid_x(const Viewer *v)
{
    float lo = 0, hi = 0;
    int n = 0;
    for (int k = 0; k < v->nmon; k++) {
        const Mon *o = &v->mon[k];
        if (!mon_in(o))
            continue;
        if (!n || o->mx < lo) lo = o->mx;
        if (!n || o->mx > hi) hi = o->mx;
        n++;
    }
    return n ? (lo + hi) / 2 : 0;
}

/* the monster as the climbing code sees it: the camera's reach (-9) is measured from the players' middle, and is
 * not asked of an AI monster (0x2DDC40); the other monsters in the world: their objects in the grid as the box query
 * (0x282D70) sees them, a WCISpheres' half sizes r, r, r (its bound's radius, 0x29A918 .. 0x29A924) turned by |M|
 * (0x101C20) about the bound's centre [R]; without a shape the box of their radius and height. Still different: the
 * query's sphere test before the box (0x282EE4: the query box's bounding sphere against the bound), which climb.c's
 * box test does not make [G: it would need the query's sphere in climb.c; only with more than one monster] */
static float mon_radius(const Mon *mo);
static float mon_half_height(const Mon *mo);
static void mon_shape(const Mon *mo, float c[3], float *r);
static void mon_cyl(const Mon *mo, float c[3], float *r, float *h);
/* isHoldlingEdible: hasGrabbedObject is 2 (0x2E5590) when the held object has flag 0x80000 [R]: the people and the
 * army's soldiers (actors), the vehicles Vehicle.bsf flags Animates or Edible (its Construct's setFlags) and the
 * parade's carts and bikers (GroundCart.bsf, parade.c's PAF_EDIBLE); no rail object, parked car or army vehicle
 * (no script sets it on them) */
static int held_anything(const Mon *mo)
{
    return mo->held >= 0 || mo->held_unit >= 0 || mo->held_vehicle >= 0 || mo->held_rail >= 0 || mo->held_parade >= 0 ||
           mo->held_sobj >= 0;
}

static int held_edible(const Mon *mo)
{
    const Viewer *v = mo->v;
    if (mo->held >= 0)
        return 1;
    if (mo->held_unit >= 0 && v->army) {
        const ArmyUnit *u = army_unit(v->army, mo->held_unit);
        return u && (u->kind == AK_TROOP || u->kind == AK_WINDOW);
    }
    if (mo->held_vehicle >= 0 && v->veh) {
        const VehObj *o = vehicle_obj(v->veh, mo->held_vehicle);
        return o && (o->oflags & VF_EDIBLE) != 0;
    }
    if (mo->held_parade >= 0 && v->parade)
        return parade_is_edible(v->parade, mo->held_parade);
    return 0;
}

/* the held object's IsShaker (getIntProp, 0 none) [R Monsters.bsf isShaker / addPeople / gainHealth]: 1 the buses,
 * the trams and the cable cars (People_Pour), 2 the garbage truck (Garbage_Pour, and it hurts) */
static int held_shaker(const Mon *mo)
{
    const Viewer *v = mo->v;
    Group t = { 0 }, in = { 0 };
    if (mo->held_vehicle >= 0 && v->veh) {
        const VehObj *o = vehicle_obj(v->veh, mo->held_vehicle);
        if (o)
            t = o->tmpl;
    } else if (mo->held_rail >= 0 && v->rail) {
        const RailObj *o = rail_obj(v->rail, mo->held_rail);
        if (o) {
            t = o->tmpl;
            in = o->inst;
        }
    }
    float k = in.at ? group_num(in, "IsShaker", -1) : -1;
    return (int)(k >= 0 ? k : t.at ? group_num(t, "IsShaker", 0) : 0);
}

/* addHitPoints(amount): + heals, - hurts (0 .. its most; nothing at 0 HP) */
static void mon_add_hp(Mon *mo, float amount)
{
    if (mo->hp > 0)
        mo->hp = fmaxf(0, fminf(mon_hp_max(mo), mo->hp + amount));
}

static Climber climber(Mon *mo)
{
    Climber c = { { mo->mx, mo->my, mo->mz }, mo->heading, mo->graph.box[0],
                  mo->v->nmon > 1 && !mo->is_ai ? mo->v->mid_x : mo->mx, { mo->graph.probe[0], mo->graph.probe[1] },
                  0, mo->mount.mounted, { { { 0 } } }, 0 };
    for (int k = 0; k < mo->v->nmon && c.nother < 4; k++) {
        Mon *o = &mo->v->mon[k];
        if (o == mo || !o->have_graph || !mon_in(o))
            continue;
        float r = mon_radius(o), hh = mon_half_height(o), *pt = c.other[c.nother].pt, sc[3], sr;
        if (o->mount.mounted)
            memcpy(pt, o->mount.at.pt, sizeof o->mount.at.pt);
        else {
            pt[0] = o->mx; pt[1] = o->my; pt[2] = o->mz;
        }
        float lo[3] = { o->mx - r, o->my - r, o->mz }, hi[3] = { o->mx + r, o->my + r, o->mz + 2 * hh };
        mon_shape(o, sc, &sr);
        if (sr >= 0) {                               /* its WCISpheres' box, turned with it */
            float h = o->heading / 57.2957795f, e[3] = { sr * (fabsf(cosf(h)) + fabsf(sinf(h))), 0, sr };
            e[1] = e[0];
            for (int k = 0; k < 3; k++) {
                lo[k] = sc[k] - e[k];
                hi[k] = sc[k] + e[k];
            }
        }
        memcpy(c.other[c.nother].lo, lo, sizeof lo);
        memcpy(c.other[c.nother].hi, hi, sizeof hi);
        c.other[c.nother++].mounted = o->mount.mounted;
    }
    return c;
}

/* the hop / swing scripts' way along the face: canClimb(0, -1) for 0 mirrored or > 0 not, else (0, 1) */
static int along(Mon *mo, float p0)
{
    return (mo->mirror && p0 == 0) || (!mo->mirror && p0 > 0) ? -1 : 1;
}

static int answer(void *ctx, const char *call, const float *a)
{
    Mon *mo = ctx;
    Viewer *v = mo ? mo->v : NULL;
    if (!mo)
        return 0;
    if (!strcmp(call, "Monsters::hasDoubleJump"))    /* Monsters.bsf: hasDoubleJumped() == 0 (+0xAB7) [R] */
        return !mo->double_jumped;
    if (!strcmp(call, "Monsters::canSpin")) {        /* Monsters.bsf: a jump's first spin, two more with MULTI-SPIN
                                                      * (upgrade flag 2), cheat 8 or the rage; each counted
                                                      * (incJumpSpinCount) [R] */
        if (mo->spins == 0 || (mo->spins <= 2 && ((mo->power.abilities & 2) || (mo->power.cheats & 8) || mo->power.rage))) {
            mo->spins++;
            return 1;
        }
        return 0;
    }
    if (!strcmp(call, "Eyegore::canSpin")) {         /* Eyegore.bsf's own: the first counted, then with MULTI-SPIN, */
        if (mo->spins == 0) {                        /* cheat 8 or the rage any number, uncounted [R] */
            mo->spins++;
            return 1;
        }
        return (mo->power.abilities & 2) || (mo->power.cheats & 8) || mo->power.rage;
    }
    int r = v ? edible_power_answer(&mo->power, call) : -1;   /* hasRoarEnergy, hasChargePunch, ... (edible.c) */
    if (r >= 0)
        return r;
    if (!v)
        return 0;
    Climber c = climber(mo);
    const Scene *sc = &v->scene;
    if (!strcmp(call, "Monsters::canMount")) {       /* (side, z offset, style): styles 2 / 3 swap the sides */
        int side = (int)a[0];
        if ((a[2] == 2 && mo->mirror) || (a[2] == 3 && !mo->mirror))
            side = side == 0 ? 1 : side == 1 ? 0 : side;
        return climb_can_mount(&mo->mount, sc, &c, side, a[1]) == 1;
    }
    if (!strcmp(call, "Monsters::mountIsOccupied")) {
        int r = climb_can_mount(&mo->mount, sc, &c, (int)a[0], 0);
        return r == -1 || r == -2 || r == -3 || r == -5 || r == -6;
    }
    if (!strcmp(call, "Monsters::canHop")) return climb_can_climb(&mo->mount, sc, &c, 0, along(mo, a[0]), 0) == 3;
    if (!strcmp(call, "Monsters::canSwing")) return climb_can_climb(&mo->mount, sc, &c, 0, along(mo, a[0]), 0) == 4;
    if (!strcmp(call, "Monsters::canMantle")) return climb_can_climb(&mo->mount, sc, &c, 1, 0, 0) == 5;
    if (!strcmp(call, "Monsters::canRespawn"))       /* 0x2D3D80: lives left, become human, its streaker not held */
        return v->game ? game_can_respawn(v->game, mo->slot, mo->streaker >= 0 && v->scene.folk &&
                                          people_get(v->scene.folk, mo->streaker)->phase == PP_HELD) : mo->defeated;
    if (!strcmp(call, "Monsters::canGrabPerson")) return window_person(mo, a + 1) >= 0 || window_soldier(mo, a + 1) >= 0;
    if (!strcmp(call, "Monsters::canDoPunch")) {     /* not at a person's or a power-up's window */
        int e = edible_call(mo, call, a + 1);
        return e >= 0 ? e : window_person(mo, a + 1) < 0 && window_soldier(mo, a + 1) < 0;
    }
    if (strstr(call, "::canGrabPowerup") || strstr(call, "::checkForHazard"))
        return edible_call(mo, call, a + 1) > 0;
    if (!strcmp(call, "Monsters::pickupSuccess") || !strcmp(call, "Monsters::isHoldlingEdible"))
        return call[10] == 'p' ? held_anything(mo) : held_edible(mo);                      /* hasGrabbedObject 2 (0x2E5590): the held object's flag 0x80000 */
    if (!strcmp(call, "Monsters::isShaker"))         /* Ground_Grab_Down -> Ground_Hold_Shake */
        return held_shaker(mo) != 0;
    if (!strcmp(call, "Monsters::canTransfer"))      /* (right): mirrored the other way */
        return climb_can_transfer(&mo->mount, sc, &c, mo->mirror ? a[0] == 0 : a[0] == 1) == 1;
    if (!strcmp(call, "Monsters::canSpiderTransfer"))   /* (right): mirrored always right */
        return climb_can_transfer(&mo->mount, sc, &c, mo->mirror || a[0] != 0) == 1;
    if (!strcmp(call, "Monsters::stopClimbUp") || !strcmp(call, "Monsters::stopSpiderClimbUp")) {
        int r = climb_can_climb(&mo->mount, sc, &c, 1, 0, !strcmp(call, "Monsters::stopSpiderClimbUp"));
        if (getenv("RAMPAGE_TRACE"))
            printf("  canClimb up %d (mount z %.1f, top %.1f)\n", r, mo->mount.at.pt[2],
                   mo->mount.at.building >= 0 ? sc->buildings[mo->mount.at.building].b.max[2] : 0);
        return r != 2;
    }
    if (!strcmp(call, "Monsters::stopClimbDown")) return climb_can_climb(&mo->mount, sc, &c, -1, 0, 0) != 2;
    if (!strcmp(call, "Monsters::shouldUnmountDown")) return climb_can_climb(&mo->mount, sc, &c, -1, 0, 0) == -1;
    if (!strcmp(call, "Monsters::hitElectricSign"))
        return climb_can_climb(&mo->mount, sc, &c, a[0] == 0 ? 1 : -1, 0, 0) == -2;
    return 0;                                        /* canGrabPerson, ...: not here */
}

/* the clip events the viewer acts on: the movement mode, and Monsters.bsf's velocity functions (jumpNorm and its kin
 * set the velocity, addVelocity adds to it, in the world or turned with the monster) */
/* Monsters.bsf's hitBuildingFront(kind, x, y, z): which face (by kind, the mirror state, or for kinds 6 and 7 the way
 * the monster faces), the point in the monster's frame, then hitBuildingPt(point, side, 1, 2.0): the point in the
 * world (0x2B3A80), the building at it (unless it is more than 1 behind that one's front) or else the nearest one that
 * way, not above its top, the point put on its face and kept only within 5 (0x2D54F0 with 0x2A3D20: collide.c's
 * collide_hit_point), and its hit (0x2BC150). Sides: 0 the building's left face (looking +x), 1 its right (-x), 2 its
 * front (+y), 5 its roof. */
/* setReaction(slot) (0x2E1AD0): taken unless the monster is defeated (+0xA59, set with its slot 67 at 0 HP, 0x2D95F4,
 * cleared by the respawn, 0x2D3898) or shrunk below 0.3 (+0xA78, the defeat's setDynamicScale); the interruptible
 * byte +0xAA2 it asks first is always set in play [R]. With the slot's entry there (+0x110 not 0) 0x2DA570(monster, 0),
 * let_go: what it holds thrown, its slot effects shut down, its feet and a prepared mount let go, for every caller (the
 * scripts' setReaction with a2 0 at 0x2ABBC8, the blows, the shots, the rage, the defeats) [R]. 1 when taken */
static void let_go(Mon *mo, int keep);
static int set_reaction(Mon *mo, int slot)
{
    if (mo->defeated || mo->scale < 0.3f)
        return 0;
    mo->pending_slot = slot;
    if (moves_slot(&mo->graph, slot, mo->column) >= 0)
        let_go(mo, 0);
    return 1;
}

/* ---- the objects' own 3D sounds (add3DSfx(name, keep): 0x287860) [R, BOSS_NOTES.md "Sounds"]: one of the object's 16
 * slots (2 bits each in +0x50: in use, keep), one whose sound has ended taken again (0x274CF0), none free: no sound;
 * the 3D sound (0x26D560, kept in the list by its id while out of range) at the object's own place (+0x2C), so it
 * follows it; remove3DSfx stops them; a deleted object (0x290480 -> 0x288050 -> 0x274710) kills those without keep
 * and leaves the others where they are */
enum { OS_ARMY, OS_PEOPLE, OS_VEHICLE, OS_STREET, OS_PARADE, OS_EDIBLE, OS_MONSTER, OS_PEOPLE_OBJECT };

/* where the object is now; 0 once it is gone (deleted, or its slot made over to another: serial) */
static int obj_where(const Viewer *v, int src, int obj, int serial, float pos[3])
{
    switch (src) {
    case OS_ARMY: {
        const ArmyUnit *u = v->army ? army_unit(v->army, obj) : NULL;
        if (!u || u->phase == AU_GONE || u->serial != serial)
            return 0;
        memcpy(pos, u->pos, 3 * sizeof(float));
        return 1;
    }
    case OS_PEOPLE: {
        const Person *p = v->scene.folk ? people_get(v->scene.folk, obj) : NULL;
        if (!p || p->phase == PP_GONE)
            return 0;
        memcpy(pos, p->pos, 3 * sizeof(float));
        return 1;
    }
    case OS_VEHICLE: {
        const VehObj *o = v->veh ? vehicle_obj(v->veh, obj) : NULL;
        if (!o || o->phase == VP_GONE)
            return 0;
        memcpy(pos, o->pos, 3 * sizeof(float));
        return 1;
    }
    case OS_STREET: {
        const StreetObj *o = v->sobj ? streetobj_get(v->sobj, obj) : NULL;
        if (!o || !o->alive)
            return 0;
        memcpy(pos, o->world + 12, 3 * sizeof(float));
        return 1;
    }
    case OS_PARADE: {
        const ParadeObj *o = v->parade ? parade_obj(v->parade, obj) : NULL;
        if (!o || o->phase == PAP_GONE)
            return 0;
        memcpy(pos, o->pos, 3 * sizeof(float));
        return 1;
    }
    case OS_EDIBLE: {
        const Edible *d = v->edibles ? edible_get(v->edibles, obj) : NULL;
        if (!d || d->phase == EP_GONE)
            return 0;
        memcpy(pos, d->pos, 3 * sizeof(float));
        return 1;
    }
    case OS_PEOPLE_OBJECT:
        return v->scene.folk && serial && people_object(v->scene.folk, obj, pos) == serial;
    case OS_MONSTER:
        if (obj < 0 || obj >= v->nmon)
            return 0;
        pos[0] = v->mon[obj].mx;
        pos[1] = v->mon[obj].my;
        pos[2] = v->mon[obj].mz;
        return 1;
    }
    return 0;
}

/* add3DSfx(name, keep) on that object; an object not (or no longer) there: the sound where it was asked for, once */
static void obj_sound(Viewer *v, int src, int obj, const char *name, int keep, const float at[3])
{
    float pos[3];
    int serial = src == OS_ARMY && v->army && army_unit(v->army, obj) ? army_unit(v->army, obj)->serial : 0;
    if (src == OS_PEOPLE_OBJECT)
        serial = v->scene.folk ? people_object(v->scene.folk, obj, pos) : 0;
    if (!name || !*name)
        return;
    if (!obj_where(v, src, obj, serial, pos)) {
        sound_play(name, at, 1);
        return;
    }
    int used = 0;
    for (int k = 0; k < v->nosnd; k++)
        if (v->osnd[k].src == src && v->osnd[k].obj == obj && v->osnd[k].serial == serial)
            used++;
    if (used >= 16 || v->nosnd >= 128)
        return;
    int h = sound_add3d(name, pos, 0, 1, 1);
    if (!h)
        return;
    v->osnd[v->nosnd].src = src;
    v->osnd[v->nosnd].obj = obj;
    v->osnd[v->nosnd].serial = serial;
    v->osnd[v->nosnd].h = h;
    v->osnd[v->nosnd].keep = keep != 0;
    v->osnd[v->nosnd++].name = name;
    if (getenv("RAMPAGE_TRACE"))
        printf("  sound %s on object %d.%d%s\n", name, src, obj, keep ? " (kept)" : "");
}

/* remove3DSfx: that object's sounds of that name (NULL: all of them) stopped */
static void obj_sound_stop(Viewer *v, int src, int obj, const char *name)
{
    for (int k = 0; k < v->nosnd; k++)
        if (v->osnd[k].src == src && v->osnd[k].obj == obj && (!name || !_stricmp(v->osnd[k].name, name))) {
            if (getenv("RAMPAGE_TRACE"))
                printf("  sound %s on object %d.%d stopped\n", v->osnd[k].name, src, obj);
            sound_kill(v->osnd[k].h);
            v->osnd[k--] = v->osnd[--v->nosnd];
        }
}

/* each frame: a sound that has ended frees its slot; the others follow their object, or with it gone die (keep 0) or
 * stay where it was (keep 1) */
static void obj_sounds_step(Viewer *v)
{
    for (int k = 0; k < v->nosnd; k++) {
        float pos[3];
        if (!sound_active(v->osnd[k].h)) {
            v->osnd[k--] = v->osnd[--v->nosnd];
        } else if (obj_where(v, v->osnd[k].src, v->osnd[k].obj, v->osnd[k].serial, pos)) {
            sound_move(v->osnd[k].h, pos);
        } else {
            if (getenv("RAMPAGE_TRACE"))
                printf("  sound %s gone with object %d.%d%s\n", v->osnd[k].name, v->osnd[k].src, v->osnd[k].obj,
                       v->osnd[k].keep ? " (kept: left where it was)" : "");
            if (!v->osnd[k].keep)
                sound_kill(v->osnd[k].h);
            v->osnd[k--] = v->osnd[--v->nosnd];
        }
    }
}

/* AlarmBuilding.bsf's DamageCell [R AlarmBuilding.scr]: Building::hit (0x2BC150) calls its script's DamageCell (slot
 * 0xC, 0x2BC8C0) at every hit past its early outs (the one-hit cheat, a Nuclear hit, no point; a ground-floor or roof
 * hit that damages no cell still calls it), whatever the building's state. The building's AlarmSound when a monster
 * hit it (coerceToMonster), AlarmDuration (4) after the last or at the first, only the first with PlayOnlyOnce: its
 * last one stopped (remove3DSfx(-1)), add3DSfx(sound, 0) at its place. The banks' Bank_Alarm (3 s, once), London's
 * ClockTower (GB08, every 8 s). [G: Building::hit's out for a standing building and an attacker of flag 0x10000000
 * (+0x4C, 0x2BC1B8) is not followed; no monster's was seen to have it] */
static void building_alarm(Viewer *v, SceneBuilding *sb)
{
    if (!sb->alarm_sound)
        return;
    if (sb->alarm_played && fabsf(v->play_now - sb->alarm_at) <= sb->alarm_duration)
        return;
    if (sb->alarm_once && sb->alarm_played)
        return;
    if (sb->alarm_h)
        sound_kill(sb->alarm_h);
    sb->alarm_h = sound_add3d(sb->alarm_sound, &sb->node.m[12], 0, 1, 1);
    sb->alarm_at = v->play_now;
    sb->alarm_played = 1;
    if (getenv("RAMPAGE_TRACE"))
        printf("  building %d alarm %s%s\n", (int)(sb - v->scene.buildings), sb->alarm_sound,
               sb->alarm_h ? "" : " (refused)");
}

/* a monster's hit on a building, its script's DamageCell (Building::hit 0x2BC150 -> slot 0xC at 0x2BC8C0 with the
 * hit's amount, its own f12 kept in f22 (0x2BC194), as famount: 2 for a punch or a roof hit (hitBuildingPt,
 * hitBuildingRoof), the roar's burst its weapon's DamagePerSec (RoarWeapon 10, 0x31BB84 -> 0x31BDC0)):
 * - AlarmBuilding.bsf: building_alarm;
 * - SchoolBuilding.bsf [R SchoolBuilding.scr]: an amount of 2.5 or more once its fStateTime (Process: less the step
 *   each frame) is out: 20 s again, and its Swarm (the BuildingInstance's OTRef|Swarm: ChildSwarm at NY01's
 *   school_back and SF04's SF04_3x6) made at its place (getPosition, +0x2C: the middle of its foot) and sent onInit
 *   (people_add_swarm: the bell, the recess, the children running out);
 * - ChickenRestaurant.bsf [R ChickenRestaurant.scr]: an amount of 2.5 or more, once a building: FollowEffect
 *   createAndStart Chickens_Run at (its x, -10, 0).
 * Only the roar reaches 2.5 (and "hitcell b floor col amount") */
static void building_script(Viewer *v, SceneBuilding *sb, float amount)
{
    building_alarm(v, sb);
    if (amount < 2.5f)
        return;
    const float *at = &sb->node.m[12];
    if (sb->school && v->play_now >= sb->swarm_next) {
        sb->swarm_next = v->play_now + 20.0f;
        Group t = sb->swarm ? city_template(v->city, sb->swarm) : (Group){ 0 };
        if (t.at && v->scene.folk)
            people_add_swarm(v->scene.folk, t, at);
        if (getenv("RAMPAGE_TRACE"))
            printf("  building %d school: swarm %s at %.1f %.1f %.1f%s\n", (int)(sb - v->scene.buildings),
                   sb->swarm ? sb->swarm : "-", at[0], at[1], at[2], t.at ? "" : " (none)");
    }
    if (sb->chicken && !sb->chickens_made) {
        float pos[3] = { at[0], -10.0f, 0.0f }, up[3] = { 0, 1, 0 };
        scene_fx(&v->scene, "Chickens_Run", pos, up);
        sb->chickens_made = 1;
        if (getenv("RAMPAGE_TRACE"))
            printf("  building %d chickens at %.1f %.1f %.1f\n", (int)(sb - v->scene.buildings), pos[0], pos[1], pos[2]);
    }
}

static void hit_building_front(Mon *mo, const float *p, float gain)
{
    Viewer *v = mo->v;
    int mirror = mo->mirror, side = 0, kind = (int)p[0];
    if (kind == 0 || kind == 1 || kind == 3 || kind == 4) side = mirror;
    else if (kind == 2 || kind == 5) side = 2;
    else if (kind == 6 || kind == 7) {
        float yaw = (mo->heading > 180 ? mo->heading - 360 : mo->heading) / 57.2957795f;
        if (yaw >= -0.7853982f && yaw <= 0.7853982f) side = 2;
        else if (yaw >= 0.7853982f && yaw <= 2.3561945f) side = 1;
        else if (yaw <= -0.7853982f && yaw >= -2.3561945f) side = 0;
        /* facing the street the script leaves side as it was declared: 0 */
    }
    float l[3] = { (mirror && side == 2) || side == 1 ? -p[1] : p[1], p[2], p[3] };
    /* 0x2B3A80: on a face (mode 5) the point is added to the monster's place as it is, in the world's axes
     * (0x2B3AC4 .. 0x2B3B08); otherwise turned with the monster [R]. So a side's punch (Side_L_Punch, kind 2) lands on
     * the front's corner column beside it */
    float c = mo->mode == MODE_BUILDING ? 1 : cosf(mo->heading / 57.2957795f),
          sn = mo->mode == MODE_BUILDING ? 0 : sinf(mo->heading / 57.2957795f);
    float w[3] = { mo->mx + c * l[0] - sn * l[1], mo->my + sn * l[0] + c * l[1], mo->mz + l[2] };
    float bq[3];                                     /* 0x2D54F0's search (collide.c, 0x2A3D20) [R] */
    int bi = collide_hit_point(&v->cw, w, side, bq);
    SceneBuilding *best = bi >= 0 && (uint32_t)bi < v->scene.nbuildings ? &v->scene.buildings[bi] : NULL;
    if (best)                                        /* Building::hit's +0x190: its Scoring pays this monster */
        best->hitter = mo->slot;
    if (best && mo->is_ai) {                         /* the AI's onHit (hitBuildingPt -> 0x2EED40) */
        float off[3] = { w[0] - mo->mx, w[1] - mo->my, w[2] - mo->mz };
        dir_ai_on_hit(&mo->ai, off, mo->mirror);
    }
    if (best && mo->power.rage) {                    /* Monsters.bsf: in the rage hitBuildingPt twice [R] */
        building_hit(&best->b, bq, 2.0f);
        scene_building_flags_punch(&v->scene, bi, bq);   /* 0x2D5824, after each hit */
    }
    int hit = best && building_hit(&best->b, bq, 2.0f);
    if (best) {
        scene_building_flags_punch(&v->scene, bi, bq);
        building_script(v, best, 2.0f);
    }
    if (hit && (kind <= 2 || kind == 6) &&             /* +2 energy (Monsters.bsf), the rage when full */
        edible_power_hit_building(&mo->power, 1, gain, mo->mode == MODE_AIRBORNE || mo->mode == MODE_TAKEOFF,
                                  others_full(mo))) {
        edible_power_start_rage(&mo->power, others_full(mo));
        set_reaction(mo, 54);
        rage_started(mo);
    }
    mo->energy = mo->power.energy;
    if (getenv("RAMPAGE_TRACE"))
        printf("  hitBuildingFront side %d at %.1f %.1f %.1f: %s", side, w[0], w[1], w[2],
               !best ? "no building\n" : hit ? "hit" : "no damage");
    if (getenv("RAMPAGE_TRACE") && best)
        printf(" (state %d, %d storeys, HP %d; energy %.1f)\n", best->b.state, best->b.n, best->b.hp, mo->energy);
}

/* the building under the monster's body (0x2D6150: the one at its probe point, its top no more than 5 above the
 * feet), -1 none */
static int mon_building_below(const Mon *mo)
{
    float s = mo->scale > 0 ? mo->scale : 1;
    CollideMonster cm = { { mo->mx, mo->my, mo->mz }, mo->heading, mo->mode,
                          (mo->graph.box[0] > 0 ? mo->graph.box[0] : 3) * s, (mo->graph.box[1] > 0 ? mo->graph.box[1] : 4.25f) * s,
                          { mo->graph.probe[0] * s, mo->graph.probe[1] * s }, 0, 0, mo->defeated || mo->hp <= 0,
                          { 0, 0, 0 }, { 0, 0, 0 } };
    return collide_building_below(&mo->v->cw, &cm);
}

/* Monsters.bsf's damageRoof (the roof punch, a jump's landing, a fall's, the butt slam's six beats; its arguments
 * unused): monster.hitBuildingRoof(bone, 1, 2.0), Monster method 0x6F (descriptor 0x42B9C0, dispatcher case 0x2AB620):
 * the bone's place in the world (vtable +0x48 0x2E54A0 by name, +0x4C 0x2E4FF0 its matrix), the building under the
 * monster's body (0x2D6150: the one at its probe point, its top no more than 5 above the feet), and its hit 0x2BBE80
 * (building_hit_roof) with kind 1 and radius 2.0. The script ignores the answer: no energy, no rage; the points are
 * Building::hit's (its hitter). The monsters with a damageRoof of their own (Cal, Eyegore, Jill, Natalie, Venus) take
 * "Bip01 Spine". */
static void hit_building_roof(Mon *mo, const char *bone)
{
    Viewer *v = mo->v;
    int bi = mon_building_below(mo);
    Mat4 w;
    float at[3] = { mo->mx, mo->my, mo->mz };
    if (scene_monster_node(&v->scene, mo->slot, bone, &w) == 0) {   /* the named bone, as the game's mirrored pose has it */
        at[0] = w.m[12]; at[1] = w.m[13]; at[2] = w.m[14];
    }
    if (bi < 0 || (uint32_t)bi >= v->scene.nbuildings) {
        if (getenv("RAMPAGE_TRACE"))
            printf("  hitBuildingRoof %s at %.1f %.1f %.1f: no building below\n", bone, at[0], at[1], at[2]);
        return;
    }
    SceneBuilding *sb = &v->scene.buildings[bi];
    if (sb->b.state == BUILDING_STANDING)            /* Building::hit's +0x190: its Scoring pays this monster */
        sb->hitter = mo->slot;
    int col, roof = sb->b.roof, hit = building_hit_roof(&sb->b, at, 2.0f, &col);
    if (col >= 0)                                    /* (0x2BBE80 went on to Building::hit) */
        building_script(v, sb, 2.0f);
    if (getenv("RAMPAGE_TRACE"))
        printf("  hitBuildingRoof %s at %.1f %.1f %.1f: building %d roof %d -> %d, top storey %d column %d %s "
               "(state %d, %d storeys, HP %d)\n", bone, at[0], at[1], at[2], bi, roof, sb->b.roof, sb->b.n - 1, col,
               hit ? "hit" : "no damage", sb->b.state, sb->b.n, sb->b.hp);
}

static float dist_to(const float a[3], const float b[3])
{
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* throwObject(eat, x, y, z) (0x2D9DC0): what the monster holds eaten, or smashed away with (x, y, z) in its frame */
static void throw_held(Mon *mo, int eat, float x, float y, float z)
{
    Viewer *v = mo->v;
    /* [R 0x2D9DC0] throwObject(vel, 0, eat): the velocity in the monster's frame, turned by its matrix (x
     * mirrored with it) and by its +0x2560 angle, always 0: the head animator's +0x68 (the animator at +0x24F8,
     * the animation player's +0x1A10, set by 0x3022DC), zeroed by 0x2FB734 and written only with one head bone
     * (0x2FC2D4); all 30 monsters have two (MONSTER_DATA +0xA14) [R] */
    float h = mo->heading / 57.2957795f, lx = mo->mirror ? -x : x;
    float vel[3] = { cosf(h) * lx - sinf(h) * y, sinf(h) * lx + cosf(h) * y, z };
    if (!eat && held_anything(mo))                   /* thrown, not eaten: +0xA94 = 1 (0x2DA284), the camera keeps
                                                      * its held object's distance a second more */
        mo->thrown_t = 1;
    if (mo->held_unit >= 0 && v->army) {
        army_throw(v->army, mo->held_unit, mo->slot, eat, vel, mo->heading / 57.2957795f);
        mo->held_unit = -1;
        return;
    }
    if (mo->held_vehicle >= 0 && v->veh) {
        vehicle_throw(v->veh, mo->held_vehicle, mo->slot, eat, vel, mo->heading / 57.2957795f);
        mo->held_vehicle = -1;
        return;
    }
    if (mo->held_parade >= 0 && v->parade) {
        parade_throw(v->parade, mo->held_parade, mo->slot, eat, mo->heading / 57.2957795f);
        mo->held_parade = -1;
        return;
    }
    if (mo->held_rail >= 0 && v->rail) {
        rail_throw(v->rail, mo->held_rail, mo->slot, eat, vel);
        mo->held_rail = -1;
        return;
    }
    if (mo->held_sobj >= 0 && v->sobj) {
        streetobj_fly(v->sobj, mo->held_sobj, mo->slot, eat, vel);
        mo->held_sobj = -1;
        return;
    }
    if (mo->held < 0 || !v->scene.folk)
        return;
    if (eat) {
        const Person *pp = people_get(v->scene.folk, mo->held);
        int first = pp && !(pp->flags & 0x40000000u);    /* addStats once per object */
        const char *name = pp ? pp->name : NULL;
        PeopleEaten got = people_eat(v->scene.folk, mo->held, mo->slot);
        monster_stats(mo, got.health, got.energy);
        if (got.fart)
            mon_fart_later(mo, 2.0f);
        if (got.eat_fx)                              /* the lightning on the priest's eater */
            mon_fx_named(mo, got.eat_fx, got.eat_node);
        if (first) {                                 /* gCommon.addStats(actor, slot, 1): civilian points */
            if (v->game && got.points != 0)
                game_add_points(v->game, mo->slot, GPT_CIVILIAN, (int)got.points);
            challenge_stats(v, name, (int)got.points);
        }
        if (getenv("RAMPAGE_TRACE"))
            printf("  ate: health %+.0f -> %.0f, energy %+.0f -> %.0f\n", got.health, mo->hp, got.energy, mo->energy);
    } else {
        people_throw(v->scene.folk, mo->held, mo->heading);
    }
    mo->held = -1;
}

/* 0x2DA570(monster, keep) [R], wherever a state is posted to start (+0x110 set: setReaction, playReaction 0x2D9710,
 * the blows 0x2D6A50 .. 0x2D89E0, the landing 0x2DC450, the knock-offs 0x2E176C / 0x2E185C / 0x2E19EC, the reset
 * 0x2DCB40): the kept sound +0xC5E0 stopped (0x2877A0; George's 240 sound events keep none), a held monster let go
 * (+0xB9C0 / +0xB9C4: none in this build), what it holds thrown unless keep (throwObject((0, 10, 10), not eaten),
 * 0x2DA698 .. 0x2DA6B8), every slot effect shut down and its slot emptied (0x2F56E0 on the record +0x7DFC: vtable
 * +0x30, as a slot's next event does; and its 4 swooshes, never started), the target offset +0xBD54 zeroed, with
 * foot IK (+0x258C) both feet let go (0x33BE90: no anchor, no planted record), and a mount only prepared
 * (+0xC640 set, +0xC644 not) cleared, its hands with it (0x322EC0; hands_step lets them go with no building). Not
 * ported: the matrix +0xBA58 made the identity and 0x2FF7A0 (each of the record +0x7D90's +0x14 vtable +0x10) */
static void let_go(Mon *mo, int keep)
{
    if (!keep)
        throw_held(mo, 0, 0, 10, 10);
    for (int k = 0; k < MON_FX_SLOTS && mo->v->fxw; k++)
        if (mo->fx_used >> k & 1)
            fx_stop(mo->v->fxw, mo->fx_slot[k]);
    mo->fx_used = 0;
    if (mo->feet.links) {
        foot_step(&mo->feet, 0, 0);
        foot_step(&mo->feet, 1, 0);
        mo->feet.anchor = -1;
    }
    if (mo->mount.at.building >= 0 && !mo->mount.mounted)
        climb_unmount(&mo->mount);
}

/* the monster as its foot object reads it (footstep.c): the feet's bones, its velocity, its ground, the water */
static void foot_pose(Mon *mo, FootPose *m)
{
    Viewer *v = mo->v;
    memset(m, 0, sizeof *m);
    m->place[0] = mo->mx; m->place[1] = mo->my; m->place[2] = mo->mz;
    float h = mo->heading / 57.2957795f;
    m->fwd[0] = -sinf(h); m->fwd[1] = cosf(h);
    for (int f = 0; f < 2; f++) {
        Mat4 w;
        if (scene_monster_node(&v->scene, mo->slot, foot_bone(f, mo->mirror), &w) == 0) {
            m->foot[f][0] = w.m[12]; m->foot[f][1] = w.m[13]; m->foot[f][2] = w.m[14];
        } else
            memcpy(m->foot[f], m->place, sizeof m->place);
    }
    for (int k = 0; k < 3; k++)
        m->vel[k] = mo->vel[k] + mo->air[k];
    m->ground = ground_at(mo);
    int wk = mon_water(mo);                          /* 0x2DC350 / 0x2DC3D0 -> 0x2D5F00 */
    m->in_water = wk >= 0;
    m->water_z = wk >= 0 ? water_surface(v->scene.water[wk]) : 0;
    m->node_flags = mo->have_graph ? mo->graph.states[mo->run.state].flags : 0;
    m->mirrored = mo->mirror;
    if (mo->have_graph && mon_in(mo)) {    /* the clip now started, on its own, at its time 0 (0x33C5A0) */
        const Instance *in = mon_in(mo);
        m->clip_ok = ik_clip_node(in, 0, foot_bone(0, mo->mirror), m->clip_foot[0]) == 0 &&
                     ik_clip_node(in, 0, foot_bone(1, mo->mirror), m->clip_foot[1]) == 0;
    }
}

static void clip_event(Mon *mo, const MoveEvent *e)
{
    Viewer *v = mo->v;
    float c = cosf(mo->heading / 57.2957795f), s = sinf(mo->heading / 57.2957795f);
    float mirror = mo->mirror ? -1.0f : 1.0f;        /* Monster.getMirrorState */
    float w[3] = { 0, 0, 0 }, l[3] = { 0, 0, 0 };   /* world / relative velocity */
    int set = 0, add = 0, relative = 0;
    if (e->type == 7) {                              /* 0x2E7DE0: the mode; any but the ground's lets both feet go with
                                                      * foot IK (+0x258C): 0x33BE90, no anchor [R]. So the foot lock of a
                                                      * standing monster (cancelFootStep) ends as it mounts a face or
                                                      * leaves the ground, and never pulls it off its rail */
        mo->mode = (int)e->value;
        if (mo->mode != MODE_GROUND && mo->feet.links) {
            foot_step(&mo->feet, 0, 0);
            foot_step(&mo->feet, 1, 0);
            mo->feet.anchor = -1;
        }
        return;
    }
    if (e->type == 15) {                             /* the record kept at +0xBA9C till the next clip (0x2E7F04,
                                                      * 0x2E8084): the bots' reference point and radius [R] */
        union { uint32_t u; float f; } r = { e->value };
        mo->ev15 = e->p[0];
        mo->ev15x = e->p[1];
        mo->ev15r = r.f;
        return;
    }
    if (e->type == 16) {                             /* the record kept at +0xBAA0 till the next clip (0x2E7F70) */
        mo->ev16 = e->p[0];
        mo->ev16x = e->p[1];
        return;
    }
    if (e->type == 6) {                              /* a sound on the monster ({6, at, ?, name}): 0x287860, its own */
        float at[3] = { mo->mx, mo->my, mo->mz };    /* place (+0x2C), following it [R] */
        obj_sound(v, OS_MONSTER, (int)(mo - v->mon), e->name, 0, at);
        return;
    }
    if (e->type == 9) {                              /* 0x2E7E18 -> 0x2F5AC0: the camera shakes, f32 strength at +8 */
        union { uint32_t u; float f; } k = { e->value };
        float at[3] = { mo->mx, mo->my, mo->mz };
        shake_start(mon_shake(mo), &SHAKE_MONSTER, at, k.f, 1, &v->shake_seed);
        return;
    }
    if (e->type == 12) {                             /* 0x2E7E90 -> 0x2F5800: an effect on or off in a slot */
        mon_fx_event(mo, e);
        return;
    }
    if (e->type == 8) {                              /* 0x2E7F18: the next clip the other way round */
        mo->mirror_queued = !mo->mirror_queued;
        return;
    }
    if (e->type == 23) {                             /* 0x2E7D38 */
        mo->column = (int32_t)e->value;
        return;
    }
    if (e->type == 0) {                              /* 0x2E7880: the parts' bones armed with this attack, or not */
        mo->armed = e->p[0] != 0 ? mo->armed | e->value : mo->armed & ~e->value;
        if (e->p[0] != 0) {                          /* its record, the parts' own (0x2E7880), its hit list empty */
            mo->atk_seq++;
            for (int p = 0; p < MOVES_PARTS; p++)
                if (e->value >> p & 1)
                    mo->atk[p] = (struct MonAtk){ e->p[1], e->p[2], e->p[3], e->level, e->aflags, e->victims, 0,
                                                  e->value, mo->atk_seq, e->kind, e->side, e->lift, e->turn,
                                                  { e->over[0], e->over[1], e->over[2], e->over[3] } };
        }
        if (getenv("RAMPAGE_TRACE"))
            printf("  attack %x %s (%g %g %g)\n", e->value, e->p[0] != 0 ? "on" : "off", e->p[1], e->p[2], e->p[3]);
        return;
    }
    if (e->type != 21 || !e->name)
        return;
    const char *n = e->name;
    if (getenv("RAMPAGE_TRACE"))
        printf("  script %s (%g %g %g %g)\n", n, e->p[0], e->p[1], e->p[2], e->p[3]);
    if (!strcmp(n, "Monsters::jumpNull"))        { w[0] = mirror * e->p[0]; w[1] = e->p[2]; set = 1; }
    else if (!strcmp(n, "Monsters::jumpNorm"))   { w[0] = mirror * e->p[0]; w[2] = 22; set = 1; }
    else if (!strcmp(n, "Monsters::jumpLow"))    { w[0] = mirror * e->p[0]; w[2] = 15; set = 1; }
    else if (!strcmp(n, "Monsters::jumpDrop"))   { w[2] = -10; set = 1; }
    else if (!strcmp(n, "Monsters::jumpHighMonster")) { l[1] = e->p[0]; l[2] = 32; set = relative = 1; }
    else if (!strcmp(n, "Monsters::addVelocity")) {
        relative = (int)e->p[0] != 0;               /* [R] Monsters.scr: (int) param0 */
        float *dst = relative ? l : w;
        dst[0] = e->p[1]; dst[1] = e->p[2]; dst[2] = e->p[3];
        add = 1;
    }
    else if (!strcmp(n, "Monsters::doFart"))     { mon_fart(mo); return; }
    else if (!strcmp(n, "Monsters::doBarf"))     { mon_function(mo, "doBarf"); return; }   /* Expression_Barf's */
    else if (!strcmp(n, "Monsters::addPeople")) {   /* Ground_Hold_Shake: what pours out of the held vehicle */
        int k = held_shaker(mo);
        if (k == 1 || k == 2)
            mon_fx_named(mo, k == 1 ? "People_Pour" : "Garbage_Pour", "fxNode_Belch");
        return;
    }
    else if (!strcmp(n, "Monsters::gainHealth")) {  /* (amount, garbage, last): the garbage truck's last hurts 40,
                                                        * with the barf and React_Ground_Heavy_Back */
        if (held_shaker(mo) == 2 && (int)e->p[1] == 1) {
            mon_add_hp(mo, -40);
            if ((int)e->p[2] == 1) {                 /* at 0 HP too: addHitPoints fells it at its next state */
                face_play(mo, "Expression_Barf", 0, 0, 1);
                force_state(mo, "React_Ground_Heavy_Back");
            }
        } else {
            mon_add_hp(mo, e->p[0]);
        }
        return;
    }
    else if (!strcmp(n, "Monsters::disableGravity")) { mo->no_gravity = 1; return; }
    else if (!strcmp(n, "Monsters::didDoubleJump")) { mo->double_jumped = 1; return; }   /* setHasDoubleJumped(1) */
    else if (!strcmp(n, "Monsters::footStep")) { foot_step(&mo->feet, (int)e->p[0], mo->mirror); return; }
    else if (!strcmp(n, "Monsters::footPlant") || !strcmp(n, "Monsters::cancelFootStep")) {
        FootPose m;                                  /* the footfall under a still monster's foot; the feet set down */
        FootFx fx;
        foot_pose(mo, &m);
        if (n[10] == 'c')
            foot_cancel(&mo->feet, (int)e->p[0], &m);
        else if (foot_plant(&mo->feet, &mo->feet_fx, (int)e->p[0], &m, &fx) && v->fxw) {
            fx_spawn_free(v->fxw, fx.name, fx.pos, NULL, 1);   /* outside the effects budget (0x2F4A20) */
            if (getenv("RAMPAGE_TRACE"))
                printf("  footfall %s at %.1f %.1f %.1f\n", fx.name, fx.pos[0], fx.pos[1], fx.pos[2]);
        }
        return;
    }
    else if (!strcmp(n, "Monsters::setMirrorState")) { mo->mirror_queued = e->p[0] != 0; return; }
    else if (!strcmp(n, "Monsters::faceAnim")) {     /* (expression, head layer, -, plays): never looping */
        int head, plays;                             /* (and a delay > 1: the belch that many seconds later) */
        const char *face = face_anim_event(&mo->faces, e->p, &head, &plays);
        if (face)
            face_play(mo, face, head, 0, plays);
        return;
    }
    else if (!strcmp(n, "Monsters::prepareToMount")) { climb_prepare(&mo->mount); return; }
    else if (!strcmp(n, "Monsters::mountBuilding")) {
        Climber cl = climber(mo);
        climb_mount(&mo->mount, &cl, e->p[0]);        /* and transitionToMount(param0) */
        if (mo->is_ai && mo->mount.at.building >= 0)  /* the AI's onMount (0x2EEC40) */
            dir_ai_on_mount(&mo->ai, mo->mount.at.building, mo->mount.at.rail,
                            v->scene.buildings[mo->mount.at.building].b.width);
        return;
    }
    else if (!strcmp(n, "Monsters::unmountBuilding")) { climb_unmount(&mo->mount); return; }
    else if (!strcmp(n, "Monsters::attachHand")) {   /* (hand, time, mirror): only on a building */
        int h = ((int)e->p[0] & 1) ^ (mo->mirror ? 1 : 0);
        Mat4 w;
        if (mo->mount.at.building < 0 || scene_monster_hand(&v->scene, mo->slot, h, &w) != 0)
            return;
        /* the place: the clip's hand now, held to the face; the rotation the clip's (the game's is the clip's hand
         * at the end of the blend, 0x321E80) */
        float pos[3] = { w.m[12], w.m[13], w.m[14] }, r[9], q[4];
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++)
                r[3 * c + k] = w.m[4 * c + k];
        ik_quat_of(r, q);
        climb_hand_snap(&mo->mount, &v->scene, h, mo->hand_thick, pos);
        if (hand_attach(&mo->hand[h], e->p[1], pos, q) && e->p[2] != 0)
            mo->mirror_queued = !mo->mirror_queued;
        return;
    }
    else if (!strcmp(n, "Monsters::detachHand")) {
        hand_detach(&mo->hand[((int)e->p[0] & 1) ^ (mo->mirror ? 1 : 0)], e->p[1]);
        return;
    }
    else if (!strcmp(n, "Monsters::transferMount")) { climb_transfer(&mo->mount); return; }
    else if (!strcmp(n, "Monsters::setScale")) {     /* (scale, time): setDynamicScale (0x2E8660 [R]), from its size
                                                      * now; a time of 0.001 or less at once */
        mo->scale_from = mo->scale;
        mo->scale_to = e->p[0] > 0 ? e->p[0] : 1;
        mo->scale_time = mo->scale_left = e->p[1] > 0.001f ? e->p[1] : 0;
        if (mo->scale_left <= 0)
            mo->scale = mo->scale_to;
        return;
    }
    else if (!strcmp(n, "Monsters::startRespawn")) { /* 0x2D3930: a life, back to full health where the streaker is
                                                        or dropping in from the view */
        mo->hp = mon_hp_max(mo);
        mo->defeated = 0;
        if (mo->nick == 3)                           /* Nick's fire and eyes on again (0x2D3D44 .. 0x2D3D58: the name
                                                      * +0xC89C is "Nick", 0x2F4FC0 starts the three) [R] */
            mo->nick = 1;
        if (!v->game) {                              /* (no game to place it: as dropping in) */
            mo->scale = mo->scale_to = 1;
            mo->scale_left = 0;
        }
        if (mon_in(mo))
            mon_in(mo)->hidden = 0;
        if (v->game) {
            GameRespawnIn in;
            GameRespawn out;
            memset(&in, 0, sizeof in);
            const Person *p = mo->streaker >= 0 && v->scene.folk ? people_get(v->scene.folk, mo->streaker) : NULL;
            if (p && p->phase != PP_GONE) {
                in.streaker = 1;
                in.on_line = p->mv.active && p->phase != PP_HELD;
                memcpy(in.streaker_pos, p->pos, sizeof in.streaker_pos);
                in.streaker_facing = p->heading / 57.2957795f;
            }
            memcpy(in.cam_pos, v->view.pos, sizeof in.cam_pos);
            memcpy(in.cam_dir, v->view.rows[1], sizeof in.cam_dir);
            in.cam_half_fov = v->view.fov / 2;
            game_start_respawn(v->game, mo->slot, &in, &out);
            mo->mx = out.pos[0]; mo->my = out.pos[1]; mo->mz = out.pos[2];
            mo->heading = out.facing * 57.2957795f;
            mo->human = 0;
            if (out.fall) {                          /* in the air, Jump_Butt_Slam_fall */
                /* [R 0x2D3BDC .. 0x2D3C08] dropping in, the scale is 1 at once (+0xA7C = +0xA78 = 1, its time 0): it
                 * leaves Faceup_Idle_Respawn before that state's setScale(1) at 0.513 and would stay the human's 0.2.
                 * Where the streaker ran it stays 0.2, and that setScale grows it back over 0.83 s */
                mo->scale = mo->scale_to = 1;
                mo->scale_left = 0;
                mo->mode = MODE_AIRBORNE;
                mo->dropping = 1;                    /* +0xAB5: the camera leaves it out until it lands */
                for (int k = 0; mo->have_graph && k < mo->graph.n; k++)
                    if (!_stricmp(mo->graph.states[k].name, "Jump_Butt_Slam_fall")) {
                        moves_force(&mo->run, k);
                        break;
                    }
            }
            if (out.delete_streaker && v->scene.folk && mo->streaker >= 0)
                people_remove(v->scene.folk, mo->streaker);
            if (getenv("RAMPAGE_TRACE"))
                printf("  respawn at %.1f %.1f %.1f%s, %d lives\n", out.pos[0], out.pos[1], out.pos[2],
                       out.fall ? ", falling" : "", game_lives(v->game, mo->slot));
        }
        mo->streaker = -1;
        return;
    }
    else if (!strcmp(n, "Monsters::becomeHuman")) {  /* while a block plays (state 6): createStreaker, and the monster
                                                        stops drawing itself (0x2D3ED0) */
        if (!v->scene.folk || !mon_in(mo))
            return;
        int human = v->game ? game_become_human(v->game, mo->slot) : 1;
        if (getenv("RAMPAGE_TRACE"))
            printf("  becomeHuman: %s (state %d)\n", human == 1 ? "a streaker" : "not in play", v->game ? game_state(v->game) : -1);
        if (human != 1)
            return;
        if (mo->nick == 2 && v->fxw)                 /* Nick's fire and eyes shut down (0x2F4F60) */
            for (int k = 0; k < 3; k++)
                fx_stop(v->fxw, mo->nick_fx[k]);
        if (mo->nick)
            mo->nick = 3;
        Group t = city_template(v->city, monster_female(mo) ? "NakedFemale" : "NakedMale");
        float at[3] = { mo->mx, mo->my, mo->mz };
        if (t.at && (mo->streaker = people_add_streaker(v->scene.folk, t, at, v->view.pos[0])) >= 0) {
            mon_in(mo)->hidden = 1;
            mo->human = 1;
        }
        return;
    }
    else if (!strcmp(n, "Monsters::doGrabPerson")) { /* grabBuildingOccupant(loc, 1) (0x2B3350): 0.25 s blend */
        int free = mo->held < 0 && mo->held_unit < 0, who = free ? window_person(mo, e->p + 1) : -1;
        if (who >= 0) {
            people_grab(v->scene.folk, who, 0.25f);
            mo->held = who;
        } else if (free && v->army && (who = window_soldier(mo, e->p + 1)) >= 0) {   /* a window soldier or cop:
                                                      * held, its onPickup; eaten or smashed by throwObject */
            scene_window_soldier_take(&v->scene, who);
            army_grab(v->army, who, mo->slot, 0.25f);
            mo->held_unit = who;
        }
        return;
    }
    else if (!strcmp(n, "Monsters::doGrabObject")) { /* (0x2D99C0): the nearest within 15 of the hand, in front */
        Mat4 h;
        if (mo->held >= 0 || !v->scene.folk || attach_world(mo, "Helper R Hand", &h) != 0)
            return;
        float hand[3] = { h.m[12], h.m[13], h.m[14] }, root[3] = { mo->mx, mo->my, mo->mz };
        float fwd[3] = { -sinf(mo->heading / 57.2957795f), cosf(mo->heading / 57.2957795f), 0 };
        /* [R] one query of the object manager (0x4D0900's +0x20: within the reach, flags 0x20840, at most 64) and
         * the nearest of them to the hand, of whatever kind: a cop beside a tourist is taken when he is nearer */
        float r = 15;                                /* each kind asked within the nearest so far */
        int kind = -1, who = -1, k;
        if ((k = people_nearest(v->scene.folk, hand, root, fwd, r)) >= 0)
            kind = 0, who = k, r = dist_to(people_get(v->scene.folk, k)->world + 12, hand);
        if (v->army && (k = army_nearest_pickup(v->army, hand, root, fwd, r)) >= 0)
            kind = 1, who = k, r = dist_to(army_unit(v->army, k)->pos, hand);
        if (v->parade && mo->held_parade < 0 && (k = parade_nearest_pickup(v->parade, hand, root, fwd, r)) >= 0)
            kind = 2, who = k, r = dist_to(parade_obj(v->parade, k)->world + 12, hand);
        if (v->veh && mo->held_vehicle < 0 && (k = vehicle_nearest_pickup(v->veh, hand, root, fwd, r)) >= 0)
            kind = 3, who = k, r = dist_to(vehicle_obj(v->veh, k)->pos, hand);
        if (v->rail && mo->held_rail < 0 && (k = rail_nearest_pickup(v->rail, hand, root, fwd, r)) >= 0)
            kind = 4, who = k, r = dist_to(rail_obj(v->rail, k)->pos, hand);
        if (v->sobj && mo->held_sobj < 0 && (k = streetobj_nearest_pickup(v->sobj, hand, root, fwd, r)) >= 0)
            kind = 5, who = k, r = dist_to(streetobj_get(v->sobj, k)->world + 12, hand);
        switch (kind) {
        case 0: people_grab(v->scene.folk, who, 0.1f); mo->held = who; break;
        case 1: army_grab(v->army, who, mo->slot, 0.1f); mo->held_unit = who; break;
        case 2: parade_grab(v->parade, who, mo->slot, 0.1f); mo->held_parade = who; break;
        case 3: vehicle_grab(v->veh, who, mo->slot, 0.1f); mo->held_vehicle = who; break;
        case 4: rail_grab(v->rail, who, mo->slot, 0.1f); mo->held_rail = who; break;
        case 5: streetobj_grab(v->sobj, who, 0.1f); mo->held_sobj = who; break;   /* a parked car */
        }
        return;
    }
    else if (!strcmp(n, "Monsters::grabBoulder")) {  /* Monsters.bsf: a Boulder torn out of the street, 6.5 up */
        float at[3] = { mo->mx, mo->my, mo->mz + 6.5f };
        int who = v->veh && mo->held < 0 && mo->held_unit < 0 && mo->held_vehicle < 0 ?
                  vehicle_spawn(v->veh, "Boulder", at, 0, -1) : -1;
        if (who >= 0) {
            vehicle_grab(v->veh, who, mo->slot, 0.1f);
            mo->held_vehicle = who;
        }
        return;
    }
    else if (!strcmp(n, "Monsters::throwObject")) {  /* (eat, x, y, z) (0x2D9DC0): eaten, or smashed away */
        throw_held(mo, e->p[0] != 0, e->p[1], e->p[2], e->p[3]);
        return;
    }
    else if (strstr(n, "::doGrabPowerup") || strstr(n, "::destroyPowerup") || strstr(n, "::checkForHazard")) {
        int r = edible_call(mo, n, e->p + 1);         /* grabBuildingOccupant: the edible at that window */
        if (getenv("RAMPAGE_TRACE"))
            printf("  %s: %d\n", n, r);
        return;
    }
    else if (strstr(n, "::useRoarEnergy")) {         /* Monsters', Furby's and Leon's costs */
        edible_power_use_roar(&mo->power, n);
        mo->energy = mo->power.energy;
        return;
    }
    else if (!strcmp(n, "Marco::eatPerson")) { mon_add_hp(mo, 10); return; }   /* Marco.bsf: addHitPoints(10) [R] */
    else if (!strcmp(n, "Monsters::fireWeapon")) {   /* Monsters.bsf: p0 == 0 "roar", else p1 == 1 "fart" [R] */
        if (e->p[0] == 0 || e->p[1] == 1)
            army_mon_weapon_fire(&mo->weapons, e->p[0] == 0 ? "roar" : "fart");
        return;
    }
    else if (!strcmp(n, "Monsters::changeMountRail")) {
        float d = mo->mirror ? -e->p[0] : e->p[0];
        climb_change_rail(&mo->mount, d < 0 ? -1 : 1);
        return;
    }
    else if (!strcmp(n, "Monsters::transitionFromBuildingTop")) { mo->mount.from_top = 1; return; }
    else if (strstr(n, "::hitBuildingFront")) { hit_building_front(mo, e->p, edible_power_hit_gain(n)); return; }
    else if (strstr(n, "::damageRoof")) {            /* Monsters.bsf the foot, Cal.bsf's kin the spine */
        hit_building_roof(mo, !strncmp(n, "Monsters::", 10) ? "Bip01 R Foot" : "Bip01 Spine");
        return;
    }
    else return;
    if (relative) {                                  /* the monster's frame (0x2E7330 / 0x2E74F0: by its matrix) */
        w[0] = c * l[0] - s * l[1];
        w[1] = s * l[0] + c * l[1];
        w[2] = l[2];
    }
    for (int k = 0; k < 3; k++)
        mo->vel[k] = set ? w[k] : mo->vel[k] + (add ? w[k] : 0);
}

static void clip_events(Mon *mo, const MoveState *st, float from, float to)
{
    const MoveEvent *e;
    int n = moves_events_between(st, from, to, &e);
    for (int i = 0; i < n; i++)
        clip_event(mo, &e[i]);
}

/* the stick (x right, y down) as a direction on the ground, in the camera's frame the monster keeps (0x2E8410) */
static void stick_world(Mon *mo, float lx, float ly, float w[3])
{
    if (mo->is_ai) {                                  /* the AI's stick is in world x, y (0x2C0AD0, 0x2E8410) */
        w[0] = mo->ai.stick[0];
        w[1] = mo->ai.stick[1];
        w[2] = 0;
        return;
    }
    for (int k = 0; k < 3; k++)
        w[k] = lx * mo->frame[0][k] - ly * mo->frame[1][k];
    w[2] = 0;
}

/* the monster's collision box (0x2E2C50): MONSTER_DATA +0x198 across (3 each way) and +0x19C up (4.25 each way from
 * its middle: 8.5 tall), against the buildings' boxes */
#define MONSTER_HALF_WIDTH 3.0f

/* the monster's radius and half height (MONSTER_DATA +0x198, +0x19C) x its dynamic scale (+0xA78 while +0xA74) */
static float mon_radius(const Mon *mo)
{
    return (mo->graph.box[0] > 0 ? mo->graph.box[0] : MONSTER_HALF_WIDTH) * (mo->scale > 0 ? mo->scale : 1);
}

static float mon_half_height(const Mon *mo)
{
    return (mo->graph.box[1] > 0 ? mo->graph.box[1] : 4.25f) * (mo->scale > 0 ? mo->scale : 1);
}

/* its body centre (0x2E4D30 with its matrix, 0x282B90: +0x8) [R]: the feet + MONSTER_DATA +0x1A0 plus the kept
 * type-16 record's +0xC along its x axis and +0x1A4 plus the record's +0x10 along its y, x its scale (+0xA78 while
 * +0xA74); z the feet's */
static void mon_body_centre(const Mon *mo, float out[3])
{
    float s = mo->scale > 0 ? mo->scale : 1, h = mo->heading / 57.2957795f, c = cosf(h), sn = sinf(h);
    float ax = (mo->graph.probe[0] + mo->ev16x) * s, ay = (mo->graph.probe[1] + mo->ev16) * s;
    out[0] = mo->mx + c * ax - sn * ay;
    out[1] = mo->my + sn * ax + c * ay;
    out[2] = mo->mz;
}

/* the water the monster is in (0x2D5F00), -1 none. Once a frame: the answer is kept (+0xC700, flag +0xC704, cleared
 * at the top of the update 0x2DE5BC) [R]. None on a building's face (+0xC644) unless its state's action code (node
 * +0x3C) is 7, climbing down; none off it in a state of action 1, the transitions onto a building. Else its box:
 * the body centre raised by the half height (3 more while airborne, mode 1, more than 3 up), half sizes radius,
 * radius, half height, against every water's box (0x2801A0, the list at 0x4D0AE0 / count 0x4D0AE8); the last that
 * touches [R]. So it is in a water standing in it, or on the street within its radius of the pool's edge. */
static int mon_water(Mon *mo)
{
    Viewer *v = mo->v;
    if (mo->water_tested) {
        for (int k = 0; mo->water_in && k < v->scene.nwater; k++)
            if (v->scene.water[k] == mo->water_in)
                return k;
        if (!mo->water_in)
            return -1;
    }                                                /* (a water no longer in the scene: asked again) */
    mo->water_tested = 1;
    mo->water_in = NULL;
    int act = mo->have_ag && mo->ag.action && mo->run.state >= 0 && mo->run.state < mo->ag.n ?
              mo->ag.action[mo->run.state] : DA_NONE;
    if (mo->mount.mounted ? act != DA_CLIMB_DOWN : act == DA_TRANSITION)
        return -1;
    float r = mon_radius(mo), hh = mon_half_height(mo), c[3], half[3] = { r, r, hh };
    mon_body_centre(mo, c);
    c[2] += hh;
    if (mo->mode == MODE_AIRBORNE && mo->mz > 3)    /* 0x2D603C: the same 3 as the landing's (0x2E29A4) */
        c[2] += 3;
    int found = -1;
    for (int k = 0; k < v->scene.nwater; k++)
        if (water_box_touches(v->scene.water[k], c, half))
            found = k;
    mo->water_in = found >= 0 ? v->scene.water[found] : NULL;
    if (mo->water_in != mo->water_was && getenv("RAMPAGE_TRACE")) {
        if (found >= 0)
            printf("  in water %d at %.1f %.1f %.1f\n", found, mo->mx, mo->my, mo->mz);
        else
            printf("  out of the water at %.1f %.1f %.1f\n", mo->mx, mo->my, mo->mz);
    }
    mo->water_was = mo->water_in;
    return found;
}

/* its ground (0x2E8530) [R]: the roof of the building under it (0x2D6150: none while its hit points, +0xA4C, are
 * gone; else the last building whose footprint holds its body centre, 0x2A3D20 at 0x2E4D30's point, kept only while
 * its top, +0x1F0, is no more than 5 above the feet, 0x2D6200; the ground its box's top +0xD4, a gone one's flat),
 * else in a water (0x2E859C, +0xA48 = 0x337E00) the water's floor under its body centre, else the street.
 * *under: that building, -1 none */
static int mon_under(Mon *mo);
static float ground_under(Mon *mo, int *under)
{
    Viewer *v = mo->v;
    float c[3];
    mon_body_centre(mo, c);
    *under = mon_under(mo);
    if (*under >= 0)
        return v->cw.b[*under].max[2];
    int wk = mon_water(mo);
    return wk >= 0 ? water_floor(v->scene.water[wk], c[0], c[1]) : 0;
}
/* 0x2D6150 itself: the building under the monster, once a frame (+0xC6F8; asked, +0xC6FC, cleared at the top of the
 * update 0x2DE5A4) [R]. Asked from the place the monster has then: the game's move (0x2E03A8) goes through a copy of
 * its matrix, which the box test (0x2E2C50) moves and only then sets (0x2DCCE0), so the frame's answer is from the
 * place before the move, and the box test keeps the moved probe on that roof (0x2A49C0) rather than losing the
 * building once the probe is past the edge */
static int mon_under(Mon *mo)
{
    Viewer *v = mo->v;
    if (mo->under_tested)
        return mo->under;
    mo->under_tested = 1;
    float c[3];
    mon_body_centre(mo, c);
    int b = mo->defeated || mo->hp <= 0 ? -1 : collide_building_at(&v->cw, c, 0, 0, NULL);
    mo->under = b >= 0 && (uint32_t)b < v->scene.nbuildings && v->cw.b[b].max[2] - mo->mz <= 5 ? b : -1;
    return mo->under;
}
static float ground_at(Mon *mo)
{
    int under;
    return ground_under(mo, &under);
}

/* the monster's collision spheres (MoveShape: a bone and an offset) in the world now: centre, radius; how many */
static int monster_spheres(Mon *mo, float out[][4], int max)
{
    if (!mo->have_graph || !mon_in(mo))
        return 0;
    const Instance *in = mon_in(mo);
    int ns = 0;
    for (int i = 0; i < mo->graph.nshapes && ns < max; i++) {
        const MoveShape *sh = &mo->graph.shapes[i];
        if (sh->bone < 0 || (uint32_t)sh->bone >= in->tree.n)
            continue;
        const float *m = in->node[sh->bone].m, *bm = in->base.m;
        float l[3];
        for (int r = 0; r < 3; r++)
            l[r] = m[r] * sh->off[0] + m[4 + r] * sh->off[1] + m[8 + r] * sh->off[2] + m[12 + r];
        if (mo->mirror)
            l[0] = -l[0];
        for (int r = 0; r < 3; r++)
            out[ns][r] = bm[r] * l[0] + bm[4 + r] * l[1] + bm[8 + r] * l[2] + bm[12 + r];
        out[ns++][3] = sh->radius;
    }
    return ns;
}

/* the frame's collision world (collide.c; the game's RGridMgr 0x4D0900 and RSceneryMgr 0x4D0B00), built once a frame
 * before anything moves: the block's buildings (tag COLLIDE_TAG(COLLIDE_OWN_BUILDING, scene index)), the monster's
 * spheres (flags 4 | COLLIDE_F_MONSTER: what the shots' rays hit; WCISpheres 0x449A20, one object each: its ray
 * 0x29A300 keeps the nearest sphere's hit, as the world's ray does across them [R]; their radii x the dynamic scale,
 * 0x2DAB60), then every module's objects with their shapes and flags (army, vehicles, rail, parade, street and roof
 * objects, the bosses' parts and VolumeTriggers). The monster (push_out), the shots and the flying objects ask it */
static void world_build(Viewer *v)
{
    CollideWorld *w = &v->cw;
    collide_begin(w, -1);
    /* [G] the street's y limits (-10 .. 21, 0x2A3638) left out while the block ends: Levels.scr's ResultsNI puts the
     * winners on the block left (13, -42) and EndBlockNI walks them from there to (0.7 x, -13.2), then the street
     * (x, -5.2); with the limits they would stand at the new street's front and walk at it until the 5 s are up. The
     * game's 0x2E2C50 reaches 0x2A35A0 in every mode but on a building, and how it lets them by is not found */
    int gs = v->game ? game_state(v->game) : -1;
    w->street_open = gs == GS_BLOCK_WON || gs == GS_END_BLOCK;
    for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
        const Building *b = &v->scene.buildings[i].b;
        if (v->scene.left) {                         /* the block left: rubble, flat (scene_block_left) */
            float top[3] = { b->max[0], b->max[1], b->min[2] };
            collide_add_building(w, b->min, top, b->row, BUILDING_GONE, 0, COLLIDE_TAG(COLLIDE_OWN_BUILDING, (int)i));
        } else
            collide_add_building(w, b->min, b->max, b->row, b->state, b->collapse != -1,
                                 COLLIDE_TAG(COLLIDE_OWN_BUILDING, (int)i));
    }
    for (int j = 0; j < v->nmon; j++) {              /* each monster's, tagged with its slot */
        Mon *mo = &v->mon[j];
        float sp[64][4];
        int ns = monster_spheres(mo, sp, 64);
        mo->msph0 = w->no;
        mo->nmsph = 0;
        for (int k = 0; k < ns; k++) {
            CollideShape sh;
            float m[12], zero[3] = { 0, 0, 0 };
            collide_shape_sphere(&sh, zero, sp[k][3] * (mo->scale > 0 ? mo->scale : 1));
            collide_matrix(m, 0, sp[k]);
            if (collide_add(w, &sh, m, COLLIDE_F_WORLD | COLLIDE_F_MONSTER, COLLIDE_TAG(COLLIDE_OWN_MONSTER, j), k) >= 0)
                mo->nmsph++;
        }
    }
    if (v->army) army_collide_add(v->army, w);
    if (v->veh) vehicle_collide_add(v->veh, w);
    if (v->rail) rail_collide_add(v->rail, w);
    if (v->parade) parade_collide_add(v->parade, w);
    if (v->sobj) streetobj_collide_add(v->sobj, w);
    if (v->boss) boss_collide_add(v->boss, w);
    scene_huts_collide_add(&v->scene, w);           /* the guards' huts (city.c) */
    collide_finish(w);
}

/* the monster's spheres moved with it: their objects in the frame's world follow (before the modules update). The
 * WCISpheres entries (0x2DA9E0) take the shape's radius x the dynamic scale (+0xA78 while +0xA74, 0x2DAB60 ..
 * 0x2DAB84: +0x10 it, +0xC its square) [R]; the touch test (0x2E22B0) the shape's own, as monster_spheres gives */
static void world_monster_moved(Mon *mo, const float (*sp)[4], int ns)
{
    Viewer *v = mo->v;
    float s = mo->scale > 0 ? mo->scale : 1;
    for (int k = 0; k < ns && k < mo->nmsph; k++) {
        CollideObj *o = &v->cw.o[mo->msph0 + k];
        float r = sp[k][3] * s;
        memcpy(o->m + 9, sp[k], 3 * sizeof(float));
        o->shape.radius = o->shape.half[0] = o->shape.half[1] = o->shape.half[2] = r;
        o->shape.raw[3] = r * r;
    }
}

/* TakeImpact on what a module's flying object met (0x287430, vtable +0x38 of the other): a building is hit at the
 * contact with radius Mass / 1000 of the mover (Building 0x2BC0F0 -> 0x2BC150) [R]; another module's object gets its
 * script's TakeImpact (Vehicle.bsf, ElevatedTrain.bsf, CableCar.bsf, StreetObject.bsf, Bomber.bsf); the monster,
 * the parade's and the bosses' scripts have none */
static void impact_route(Viewer *v, int tag, int kind, const float at[3], float mass)
{
    int i = COLLIDE_TAG_INDEX(tag), hit = 0;
    switch (COLLIDE_TAG_OWNER(tag)) {
    case COLLIDE_OWN_BUILDING:
        if ((uint32_t)i < v->scene.nbuildings) {    /* Building::hit (0x2BC150): +0x190 = the attacker if it is a */
            v->scene.buildings[i].hitter = -1;       /* monster, else none: a thrown object clears it */
            hit = building_hit(&v->scene.buildings[i].b, at, mass / 1000);
        }
        break;
    case COLLIDE_OWN_VEHICLE: if (v->veh) vehicle_impact(v->veh, i); break;
    case COLLIDE_OWN_RAIL: if (v->rail) rail_impact(v->rail, i); break;
    case COLLIDE_OWN_STREET: if (v->sobj) hit = streetobj_take_impact(v->sobj, i); break;
    case COLLIDE_OWN_ARMY: if (v->army) army_impact(v->army, i); break;
    default: break;
    }
    if (getenv("RAMPAGE_TRACE"))
        printf("  impact on %d.%d (kind %d) at %.1f %.1f %.1f, mass %.0f%s\n", COLLIDE_TAG_OWNER(tag), i, kind, at[0],
               at[1], at[2], mass, hit ? ": hit" : "");
}

/* the monster's box against the frame's world, as the game moves it (0x2E2C50): the PushOutMonsters objects and the
 * VolumeTriggers (their touches go to their owner: boss.c's barriers), then the buildings (fronts, alley walls and
 * corners), held on a roof, the street's limits; on a building's face (mode 5) only the triggers. Roofs are landed on
 * by ground_at */
/* 0x2E4660, before the box test (0x2E11A4) [R]: a player's monster (its slot under the players' count, 0x4D7E28),
 * not in mode 3 or 5, is pushed off every other player's monster it overlaps (not one hidden, +0x109, nor one it
 * holds or is held by: none here): each is a cylinder about its reference point (0x2DA900: its place + (+0x190 +
 * the type-15 record's +0xC, +0x194 + its +0x10) x its scale along its x and y axes), MONSTER_DATA +0x188 + the
 * record's +0x8 x its scale across, from its feet 2 x +0x18C up (in mode 1 or 2 about +0xB20 x its scale instead
 * [G: +0xB20 not traced: the feet's cylinder kept]); overlapping in z and nearer in x, y than the two radii, it is
 * moved away along their x, y by half of what they overlap, that overlap at most 50 x the frame's time (straight
 * along a default way when they are within 0.001), and with foot IK both its feet let go (0x33BE90) */
static void push_apart(Mon *mo, float dt)
{
    Viewer *v = mo->v;
    if (mo->slot >= v->nmon || mo->mode == 3 || mo->mode == MODE_BUILDING || !mo->have_graph)
        return;
    float a[3], ar, ah;
    mon_cyl(mo, a, &ar, &ah);
    for (int k = 0; k < v->nmon; k++) {
        Mon *o = &v->mon[k];
        if (o == mo || !o->have_graph || (mon_in(o) && mon_in(o)->hidden))
            continue;
        float b[3], br, bh;
        mon_cyl(o, b, &br, &bh);
        if (!(a[2] <= b[2] + 2 * bh && !(a[2] + 2 * ah < b[2])))
            continue;
        float d[2] = { a[0] - b[0], a[1] - b[1] }, r = ar + br, l2 = d[0] * d[0] + d[1] * d[1];
        if (l2 >= r * r)
            continue;
        float l = sqrtf(l2);
        if (l <= 0.001f) {                           /* the default way (0x40A008) [G: its value not read] */
            d[0] = 1;
            d[1] = 0;
        } else {
            d[0] /= l;
            d[1] /= l;
        }
        float push = r - l, most = 50 * dt;
        push = 0.5f * (push < most ? push : most);
        mo->mx += d[0] * push;
        mo->my += d[1] * push;
        a[0] += d[0] * push;
        a[1] += d[1] * push;
        if (mo->feet.links) {
            foot_step(&mo->feet, 0, 0);
            foot_step(&mo->feet, 1, 0);
            mo->feet.anchor = -1;
        }
    }
}

static void push_out(Mon *mo)
{
    Viewer *v = mo->v;
    if (mo->slot >= v->nmon)                         /* 0x2E2C6C: a slot (+0xFC) past the players' count (0x4D7E28),
                                                      * the revealed monster's: none of it, so it walks off the street */
        return;
    float s = mo->scale > 0 ? mo->scale : 1;
    int landed = mo->landed;                         /* the frame it landed: as it was in the air */
    mo->landed = 0;
    CollideMonster m = { { mo->mx, mo->my, landed ? mo->landed_z : mo->mz }, mo->heading, landed ? MODE_AIRBORNE : mo->mode,
                         (mo->graph.box[0] > 0 ? mo->graph.box[0] : 3) * s, (mo->graph.box[1] > 0 ? mo->graph.box[1] : 4.25f) * s,
                         { (mo->graph.probe[0] + mo->ev16x) * s, (mo->graph.probe[1] + mo->ev16) * s }, 0, mo->is_ai, 0,
                         { mo->vel[0], mo->vel[1], mo->vel[2] }, { mo->air[0], mo->air[1], mo->air[2] } };
    /* [R 0x2E4D30] its body centre with the kept type-16 record's offset: a Transition_Ground clip's (-5 .. -6.25)
     * puts it off the wall the monster has just let go of, so it is not pushed out of the building */
    m.below = mon_under(mo);                         /* the frame's building under it, from before the move */
    m.below_set = 1;
    CollideMonsterOut o;
    collide_monster(&v->cw, &m, &o);
    for (int k = 0; k < o.ntouch; k++) {             /* 0x335BF0: the trigger's FireFunc, by its owner */
        const CollideObj *t = &v->cw.o[o.touch[k]];
        if (COLLIDE_TAG_OWNER(t->tag) == COLLIDE_OWN_BOSS && t->kind == 1 && v->boss)
            boss_trigger_touch(v->boss, COLLIDE_TAG_INDEX(t->tag), mo->slot, o.touch_normal[k]);
    }
    if (getenv("RAMPAGE_TRACE")) {
        for (int k = 0; k < o.npushed; k++) {
            const CollideObj *t = &v->cw.o[o.pushed[k]];
            printf("  collide: pushed out of %d.%d (kind %d)\n", COLLIDE_TAG_OWNER(t->tag), COLLIDE_TAG_INDEX(t->tag),
                   t->kind);
        }
        for (int k = 0; k < o.ntouch; k++) {
            const CollideObj *t = &v->cw.o[o.touch[k]];
            printf("  collide: touched trigger %d.%d\n", COLLIDE_TAG_OWNER(t->tag), COLLIDE_TAG_INDEX(t->tag));
        }
        if (m.pos[0] != mo->mx || m.pos[1] != mo->my)
            printf("  collide: %.2f %.2f -> %.2f %.2f (below %d roof %d walled %d)\n", mo->mx, mo->my, m.pos[0], m.pos[1],
                   o.below, o.on_roof, o.walled);
    }
    mo->mx = m.pos[0];
    mo->my = m.pos[1];
    mo->vel[0] = m.vel[0]; mo->vel[1] = m.vel[1];
    mo->air[0] = m.stick[0]; mo->air[1] = m.stick[1];
    if (o.feet_lift) {                               /* 0x33BE90: both feet lifted, no anchor */
        foot_step(&mo->feet, 0, 0);
        foot_step(&mo->feet, 1, 0);
        mo->feet.anchor = -1;
    }
}

/* NODE_STEER (0x2E8190): the turn towards the stick this frame, in degrees. The stick in the monster's own frame
 * against the state's forward axis; beyond one frame's worth of turning it turns by the angle Ã— `turn` Ã— dt, so it
 * swings round fast and settles gently. */
static float steer(Mon *mo, const MoveState *st, float lx, float ly, float dt)
{
    float w[3], h = mo->heading / 57.2957795f, c = cosf(h), s = sinf(h);
    stick_world(mo, lx, ly, w);
    float l[2] = { c * w[0] + s * w[1], -s * w[0] + c * w[1] };   /* by the monster's matrix: right, forward */
    float n = l[0] * l[0] + l[1] * l[1];
    if (n <= 1e-6f)
        return 0;
    n = 1 / sqrtf(n);
    l[0] *= n;
    l[1] *= n;
    float fx = mo->mirror ? -st->fwd[0] : st->fwd[0];   /* mirrored: its forward axis' x the other way */
    float d = l[0] * fx + l[1] * st->fwd[1];
    float angle = acosf(d < -1 ? -1 : d > 1 ? 1 : d);
    if (angle <= st->turn * dt)
        return 0;
    float sign = l[0] * st->fwd[1] - l[1] * fx < 0 ? 1.0f : -1.0f;
    return sign * angle * st->turn * dt * 57.2957795f;
}

/* NODE_AIR_* (0x2E6580): the stick asks for `speed` along the monster's own axes the state allows; the velocity
 * moves towards that by MOVES_AIR_ACCEL (MOVES_AIR_BRAKE when slowing), each axis on its own, and is kept to the
 * state's speed */
static void air_control(Mon *mo, const MoveState *st, float lx, float ly, float dt)
{
    float goal[3] = { 0, 0, 0 }, w[3];
    if (st->flags & (NODE_AIR_X | NODE_AIR_Y | NODE_AIR_Z)) {
        stick_world(mo, lx, ly, w);
        float n = w[0] * w[0] + w[1] * w[1];
        if (n >= 1e-6f) {
            float h = mo->heading / 57.2957795f, c = cosf(h), s = sinf(h);
            float axis[2][3] = { { c, s, 0 }, { -s, c, 0 } };   /* the monster's x (right) and y (forward) */
            n = st->speed / sqrtf(n);
            for (int k = 0; k < 3; k++)
                w[k] *= n;
            for (int a = 0; a < 2; a++)
                if (st->flags & (a ? NODE_AIR_Y : NODE_AIR_X)) {
                    float p = w[0] * axis[a][0] + w[1] * axis[a][1];
                    for (int k = 0; k < 3; k++)
                        goal[k] += p * axis[a][k];
                }
        }
    }
    float cur = mo->air[0] * mo->air[0] + mo->air[1] * mo->air[1] + mo->air[2] * mo->air[2];
    float want = goal[0] * goal[0] + goal[1] * goal[1] + goal[2] * goal[2];
    float step = (cur <= want ? MOVES_AIR_ACCEL : MOVES_AIR_BRAKE) * dt;
    for (int k = 0; k < 3; k++) {
        float d = goal[k] - mo->air[k];
        mo->air[k] += d > step ? step : d < -step ? -step : d;
    }
    cur = mo->air[0] * mo->air[0] + mo->air[1] * mo->air[1] + mo->air[2] * mo->air[2];
    if (cur > st->speed * st->speed) {
        float k = st->speed / sqrtf(cur);
        for (int i = 0; i < 3; i++)
            mo->air[i] *= k;
    }
}

/* one frame of play. The left stick (x right, y down) and the buttons pressed this frame (MV_CROSS ...) or held.
 * The graph picks the state; the clip's root carries the monster (its travel added to where it stands, its turn
 * when a NODE_BAKE_ROOT state ends), the stick turns it (NODE_STEER) or drives it (NODE_AIR_*) in the camera's frame,
 * the clip's events set its movement mode and velocity, and in the air it falls by MOVES_GRAVITY until it meets the
 * ground (here the street at z = 0), which fires the landing slot. */
/* every collision sphere of the monster, armed or not, in the world (0x2DA9E0: the shape's offset by its bone's
 * matrix, then by the monster's; its count +0x42C, centres +0x6D4): what an unarmed touch tests (0x2E22B0) */
static int mon_spheres(const Mon *mo, float out[][4], int max)
{
    const Instance *in = mon_in(mo);
    if (!mo->have_graph || !in)
        return 0;
    const MoveGraph *g = &mo->graph;
    int n = 0;
    for (int i = 0; i < g->nshapes && n < max; i++) {
        const MoveShape *sh = &g->shapes[i];
        if (sh->bone < 0 || (uint32_t)sh->bone >= in->tree.n)
            continue;
        const float *m = in->node[sh->bone].m;
        float l[3];
        for (int r = 0; r < 3; r++)
            l[r] = m[r] * sh->off[0] + m[4 + r] * sh->off[1] + m[8 + r] * sh->off[2] + m[12 + r];
        if (mo->mirror)
            l[0] = -l[0];
        const float *bm = in->base.m;
        for (int r = 0; r < 3; r++)
            out[n][r] = bm[r] * l[0] + bm[4 + r] * l[1] + bm[8 + r] * l[2] + bm[12 + r];
        out[n++][3] = sh->radius;
    }
    return n;
}

/* the monster's WCISpheres bound (0x29A880: sphere 0 merged with the rest, over the entries 0x2DA9E0 fills, their
 * radii x the dynamic scale, 0x2DAB60): r -1 without a shape [R] */
/* the cylinder the monsters push each other off by (0x2E4660): about its reference point (0x2DA900), its radius
 * and half height (MONSTER_DATA +0x188 + the type-15 record's +0x8, +0x18C), x its scale; c[2] its feet */
static void mon_cyl(const Mon *mo, float c[3], float *r, float *h)
{
    float s = mo->scale > 0 ? mo->scale : 1, hd = mo->heading / 57.2957795f;
    float fx = (mo->graph.bot_off[0] + mo->ev15x) * s, fy = (mo->graph.bot_off[1] + mo->ev15) * s;
    c[0] = mo->mx + cosf(hd) * fx - sinf(hd) * fy;
    c[1] = mo->my + sinf(hd) * fx + cosf(hd) * fy;
    c[2] = mo->mz;
    *r = (mo->graph.bot_r + mo->ev15r) * s;
    *h = mo->graph.bot_h * s;
}

static void mon_shape(const Mon *mo, float c[3], float *r)
{
    float sp[64][4], s = mo->scale > 0 ? mo->scale : 1;
    int n = mon_spheres(mo, sp, 64);
    for (int i = 0; i < n; i++)
        sp[i][3] *= s;
    army_monster_shape((const float (*)[4])sp, n, c, r);
}

/* the armed part whose attack the collision shape i's bone holds (the newest arming of a part with that bone), or
 * -1 unarmed */
static int shape_part(const Mon *mo, int i)
{
    const MoveGraph *g = &mo->graph;
    int best = -1;
    for (int p = 0; p < g->nparts && p < MOVES_PARTS; p++)
        if (mo->armed >> p & 1)
            for (int k = 0; k < g->part_nbones[p]; k++)
                if (g->part_bone[p][k] == g->shapes[i].bone && (best < 0 || mo->atk[p].seq > mo->atk[best].seq))
                    best = p;
    return best;
}

/* the collision spheres an armed attack can hit with, in the world (0x2DA9E0: the shape's offset by its bone's
 * matrix, then by the monster's), into the scene's overlay */
/* RoyalGuardHut.bsf's TakeDamage ends gCommon.addStats(this.object, m.getSlot(), 0): the hut's Health, Energy and
 * Points (DefObjectProps' 10) for the monster whose blow sent it flying [R] */
static void hut_stats(Mon *mo, const float c[3], float r, const float from[3])
{
    Viewer *v = mo->v;
    Group hit[8];
    int n = scene_huts_hit(&v->scene, c, r, from, hit, 8);
    for (int i = 0; i < n; i++)
        if (v->army)
            army_add_stats(v->army, mo->slot, hit[i], 0);
        else
            monster_stats(mo, hit[i].at ? group_num(hit[i], "Health", 0) : 0,
                          hit[i].at ? group_num(hit[i], "Energy", 0) : 0);
}

static void hit_spheres(Mon *mo)
{
    Viewer *v = mo->v;
    if (!mo->have_graph || !mo->armed || !mon_in(mo))
        return;
    const Instance *in = mon_in(mo);
    const MoveGraph *g = &mo->graph;
    for (int i = 0; i < g->nshapes && v->scene.nspheres < 256; i++) {
        const MoveShape *sh = &g->shapes[i];
        int part = shape_part(mo, i);
        if (part < 0 || sh->bone < 0 || (uint32_t)sh->bone >= in->tree.n)
            continue;
        const struct MonAtk *atk = &mo->atk[part];
        const float *m = in->node[sh->bone].m;      /* in the monster's frame, then reflected if mirrored */
        float l[3];
        for (int r = 0; r < 3; r++)
            l[r] = m[r] * sh->off[0] + m[4 + r] * sh->off[1] + m[8 + r] * sh->off[2] + m[12 + r];
        if (mo->mirror)
            l[0] = -l[0];
        const float *bm = in->base.m;
        float *c = v->scene.spheres[v->scene.nspheres++];
        for (int r = 0; r < 3; r++)
            c[r] = bm[r] * l[0] + bm[4 + r] * l[1] + bm[8 + r] * l[2] + bm[12 + r];
        c[3] = sh->radius;
        scene_adornments_hit(&v->scene, c, c[3]);    /* 0x2D4390: it knocks adornments off */
        if (v->army)                                 /* and the army's units: the attack's damage, once an attack */
            army_hit(v->army, mo->slot, c, c[3], atk->damage, atk->seq);
        if (v->veh)                                  /* and the vehicles and rockets */
            vehicle_hit(v->veh, mo->slot, c, c[3], atk->damage);
        if (v->rail)                                 /* and the trains, cable cars and freighters */
            rail_hit(v->rail, mo->slot, c, c[3], atk->damage);
        if (v->boss)                                 /* and the bosses */
            boss_hit(v->boss, mo->slot, c, c[3], atk->damage, atk->seq);   /* the record's damage, once a record */
        if (v->parade)                               /* and the balloons, bikers and food carts */
            parade_hit(v->parade, mo->slot, c, c[3], atk->seq);
        upgrade_touch(mo, c, c[3]);                   /* and the MonsterUpgrade object */
        if (v->sobj) {                               /* and the street and roof objects (TakeDamage, type 0) */
            StreetMonster me = { { mo->mx, mo->my, mo->mz }, mo->heading / 57.2957795f,
                                 mo->graph.states[mo->run.state].name };
            streetobj_hit(v->sobj, mo->slot, &me, c, c[3], atk->damage, atk->seq);   /* the record's +0x2C (0x2E268C):
                                                      * Crane.bsf's HitPoints 5 take it */
        }
        if (v->scene.folk) {                         /* and people (TakeDamage), and the guards' huts */
            people_hit(v->scene.folk, mo->slot, c, c[3], mo->heading, mo->graph.states[mo->run.state].name);
            float from[3] = { mo->mx, mo->my, mo->mz };
            hut_stats(mo, c, c[3], from);
        }
    }
}

/* the squared distance of the origin from the segment a .. b (0x100230; t clamped to 0 .. 1, a point when
 * |b - a|^2 < 0.001) */
static float seg_dist2(const float a[3], const float b[3])
{
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    float t = dd < 0.001f ? 0 : -(a[0] * d[0] + a[1] * d[1] + a[2] * d[2]) / dd;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float p[3] = { a[0] + t * d[0], a[1] + t * d[1], a[2] + t * d[2] };
    return p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
}

/* every collision shape's sphere in the world by its shape index (-1 radius: no bone), as mon_spheres */
static int mon_shape_spheres(const Mon *mo, float out[64][4])
{
    const Instance *in = mon_in(mo);
    if (!mo->have_graph || !in)
        return 0;
    const MoveGraph *g = &mo->graph;
    int n = g->nshapes < 64 ? g->nshapes : 64;
    for (int i = 0; i < n; i++) {
        const MoveShape *sh = &g->shapes[i];
        out[i][3] = -1;
        if (sh->bone < 0 || (uint32_t)sh->bone >= in->tree.n)
            continue;
        const float *m = in->node[sh->bone].m, *bm = in->base.m;
        float l[3];
        for (int r = 0; r < 3; r++)
            l[r] = m[r] * sh->off[0] + m[4 + r] * sh->off[1] + m[8 + r] * sh->off[2] + m[12 + r];
        if (mo->mirror)
            l[0] = -l[0];
        for (int r = 0; r < 3; r++)
            out[i][r] = bm[r] * l[0] + bm[4 + r] * l[1] + bm[8 + r] * l[2] + bm[12 + r];
        out[i][3] = sh->radius;
    }
    return n;
}

/* monster against monster (0x2E35D0, each frame for each attacker): its armed spheres against the others' collision
 * spheres, each swept from its place last frame (0x100230 on the pair's offset then and now: the closest the two
 * came; unswept without last frame's), a pair meeting when that is within the radii; a victim within 40, not hidden,
 * not down, is hit by the closest pair of a part whose attack allows it (in front or behind, in its victims' mask,
 * not hit by that part's arming yet): 0x2D6A50 with that attack's record (monster_blow_hit: its damage, its knock
 * and reaction by look.c monster_blow). No points, no team [R] */
static void monsters_fight(Viewer *v)
{
    static float now[MONSTERS][64][4];
    int nnow[MONSTERS];
    for (int a = 0; a < v->nmon; a++)
        nnow[a] = mon_shape_spheres(&v->mon[a], now[a]);
    for (int a = 0; a < v->nmon; a++) {
        Mon *at = &v->mon[a];
        if (!at->nsph || at->defeated)
            continue;
        float h = at->heading / 57.2957795f, fwd[2] = { -sinf(h), cosf(h) };
        for (int j = 0; j < v->nmon; j++) {
            Mon *vi = &v->mon[j];
            if (j == a || vi->defeated || vi->hp <= 0 || !mon_in(vi) || mon_in(vi)->hidden)
                continue;
            float dx = vi->mx - at->mx, dy = vi->my - at->my, dz = vi->mz - at->mz;
            if (dx * dx + dy * dy + dz * dz > 1600)
                continue;
            int front = fwd[0] * dx + fwd[1] * dy >= 0, part = -1;
            float best = 3.4e38f;
            for (int i = 0; i < nnow[a]; i++) {        /* the closest pair of a part that may hit it */
                const float *c = now[a][i], *c0 = i < at->sph_prev_n ? at->sph_prev[i] : c;
                int p = c[3] < 0 ? -1 : shape_part(at, i);
                if (p < 0 || !(at->atk[p].victims >> j & 1) || (at->atk[p].hit >> j & 1) ||
                    !(at->atk[p].flags & (front ? 2u : 4u)))
                    continue;
                for (int k = 0; k < nnow[j]; k++) {
                    const float *s = now[j][k], *s0 = k < vi->sph_prev_n ? vi->sph_prev[k] : s;
                    if (s[3] < 0)
                        continue;
                    float d0[3] = { s0[0] - c0[0], s0[1] - c0[1], s0[2] - c0[2] };
                    float d1[3] = { s[0] - c[0], s[1] - c[1], s[2] - c[2] }, r = s[3] + c[3];
                    float d2 = seg_dist2(d0, d1);
                    if (d2 < best && d2 < r * r) {
                        best = d2;
                        part = p;
                    }
                }
            }
            if (part < 0)
                continue;
            struct MonAtk *atk = &at->atk[part];
            for (int q = 0; q < MOVES_PARTS; q++)      /* 0x2E33A0 on each of the record's parts: hit once */
                if (atk->parts >> q & 1)
                    at->atk[q].hit |= 1u << j;
            char name[16];
            snprintf(name, sizeof name, "monster %d", a + 1);
            monster_blow_hit(vi, at, atk, name);
        }
    }
    for (int a = 0; a < v->nmon; a++) {               /* this frame's places, for the next frame's sweep */
        Mon *mo = &v->mon[a];
        for (int i = 0; i < nnow[a]; i++)
            memcpy(mo->sph_prev[i], now[a][i], sizeof mo->sph_prev[i]);
        mo->sph_prev_n = nnow[a];
    }
}

static int take_damage_level(const Mon *mo, float amount);

/* the Shockwave's burst (0x31BAD0, once a burst) [R]: the RGridMgr's sphere query (vtable +0x3C / +0x40, 0x282BC0 /
 * 0x282A40: include -1, exclude 0) over the world's objects: each one whose shape's bounding sphere (its centre +0xC
 * through its matrix, radius +0x18) reaches the sphere of CollisionRadius about the weapon's place, but the monster
 * itself (+0xB0). Its way from the place, made 1 long ((1, 0, 0) when nearer than 1e-6 squared), then:
 * - a monster (flag 0x40): the knock, that way's x, y made 1 long times HorizontalKnockback and VerticalKnockback up
 *   (both 0: no knock), to 0x2D9650 -> 0x2D6A50 (type, the monster, no point, the knock, ReactionLevel, the amount:
 *   DamagePerSec as it is, once, not by time); a level < 0 would go to its TakeDamage (+0x34);
 * - anything else: its TakeDamage (+0x34: DamageType, the monster, no point, the way, DamagePerSec). Here the modules'
 *   sphere entries, which test their own shapes against the sphere, a fresh attack record so each is hit once [G:
 *   the game tests the shapes' bounding spheres (0x282BC0), so a box's corners reach a little short here; the
 *   modules' hit entries (army.c, vehicle.c, rail.c, boss.c, parade.c, streetobj.c, people.c) take a sphere, not
 *   the world's objects];
 * - a building (0x2BC150 with type 0, a monster, a way and no point, 0x2BC328): only a standing one with storeys
 *   (+0x1D0), its Scoring paying the monster (+0x190); the front cell under the monster's shape centre (0x287F00:
 *   column (x - its min x) / 5 and storey (z - 6) / 3, each clamped to the building's), hit at its middle (min x +
 *   2.5 + 5 column, the front, min z + 6 + 3 storey + 1.5) as Building::hit with the amount as its radius: 3 x
 *   round(10 / 2.5) dents and the cell's 2 HP. The roar cracks the buildings beside it */
static void roar_burst(Viewer *v, Mon *mo, const ArmyMonWeapon *w, const float c[3])
{
    float r = w->radius, amount = w->dps, mc[3], mr;
    char name[16];
    snprintf(name, sizeof name, "monster %d", mo->slot + 1);
    int hit = 0;
    for (int k = 0; k < v->nmon; k++) {              /* the other monsters: their WCISpheres' bounding sphere */
        Mon *vi = &v->mon[k];
        if (vi == mo || !vi->have_graph || !mon_in(vi) || mon_in(vi)->hidden)
            continue;
        float sc[3], sr;
        mon_shape(vi, sc, &sr);
        float d[3] = { sc[0] - c[0], sc[1] - c[1], sc[2] - c[2] };
        if (sr < 0 || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > (r + sr) * (r + sr))
            continue;
        float l = sqrtf(d[0] * d[0] + d[1] * d[1]), kx = l > 1e-4f ? d[0] / l : 1, ky = l > 1e-4f ? d[1] / l : 0;
        float knock[3] = { kx * w->h_knock, ky * w->h_knock, w->v_knock };
        float from[3] = { vi->mx - knock[0], vi->my - knock[1], vi->mz };   /* behind: the knock along its front
                                                                              * (0x2D6A50 without a point) */
        monster_hit(vi, name, amount, w->level >= 0 ? w->level : take_damage_level(vi, amount), knock, from);
        hit++;
    }
    mon_shape(mo, mc, &mr);
    if (mr < 0) {                                    /* without a shape its place (+0x2C, 0x2BC390) */
        mc[0] = mo->mx;
        mc[1] = mo->my;
        mc[2] = mo->mz;
    }
    for (uint32_t i = 0; i < v->scene.nbuildings; i++) {   /* the buildings: their box's bounding sphere */
        SceneBuilding *sb = &v->scene.buildings[i];
        Building *b = &sb->b;
        float d[3], br = 0;
        for (int k = 0; k < 3; k++) {
            d[k] = (b->min[k] + b->max[k]) / 2 - c[k];
            br += (b->max[k] - b->min[k]) * (b->max[k] - b->min[k]) / 4;
        }
        br = sqrtf(br);
        if (b->state != BUILDING_STANDING || b->n <= 0 || b->width <= 0 ||
            d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > (r + br) * (r + br))
            continue;
        int col = (int)((mc[0] - b->min[0]) / 5), s = (int)((mc[2] - 6) / 3);
        col = col > b->width - 1 ? b->width - 1 : col < 0 ? 0 : col;
        s = s > b->n - 1 ? b->n - 1 : s < 0 ? 0 : s;
        float at[3] = { b->min[0] + 2.5f + 5 * (float)col, b->min[1], b->min[2] + 6 + 3 * (float)s + 1.5f };
        sb->hitter = mo->slot;
        hit += building_hit(b, at, amount);
        building_script(v, sb, amount);
    }
    unsigned rec = ++mo->atk_seq;                    /* the rest: TakeDamage(monster, amount, type 0, the way) */
    if (v->army)
        hit += army_hit(v->army, mo->slot, c, r, amount, rec);
    if (v->veh)
        hit += vehicle_hit(v->veh, mo->slot, c, r, amount);
    if (v->rail)
        hit += rail_hit(v->rail, mo->slot, c, r, amount);
    if (v->boss)
        hit += boss_hit(v->boss, mo->slot, c, r, amount, rec);
    if (v->parade)
        hit += parade_hit(v->parade, mo->slot, c, r, rec);
    upgrade_touch(mo, c, r);
    if (v->sobj) {
        StreetMonster me = { { mo->mx, mo->my, mo->mz }, mo->heading / 57.2957795f,   /* (getMoveName: the roar's
                                                      * state, "Ground..." for MerryGoRound / DamagingEffect) */
                             mo->graph.states[mo->run.state].name };
        hit += streetobj_hit(v->sobj, mo->slot, &me, c, r, amount, rec);
    }
    if (v->scene.folk) {
        hit += people_hit(v->scene.folk, mo->slot, c, r, mo->heading, mo->graph.states[mo->run.state].name);
        float from[3] = { mo->mx, mo->my, mo->mz };
        hut_stats(mo, c, r, from);
    }
    if (getenv("RAMPAGE_TRACE"))
        printf("  %s's %s burst at %.1f %.1f %.1f (radius %g, %g): %d hit\n", name, w->key, c[0], c[1], c[2], r, amount,
               hit);
}

/* each monster's weapons each frame (the update 0x2E1400, army.c army_mon_weapon_step): the FireEffect at the
 * WeaponNode in its frame (the node's world, the weapon's +0xBC the identity) for the burst's Duration, and the
 * burst's damage the frame it starts [R] */
static void mon_weapons_step(Viewer *v, float dt)
{
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        if (!mo->have_graph || !mon_in(mo))
            continue;
        for (int i = 0; i < mo->weapons.n; i++) {
            ArmyMonWeapon *w = &mo->weapons.w[i];
            int what = army_mon_weapon_step(w, dt, game_weapons_on(v->game));
            if (!what)
                continue;
            Mat4 n;
            float at[3], R[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
            if (w->node && scene_monster_node(&v->scene, mo->slot, w->node, &n) == 0) {
                for (int a = 0; a < 3; a++) {
                    float l = sqrtf(n.m[4 * a] * n.m[4 * a] + n.m[4 * a + 1] * n.m[4 * a + 1] +
                                    n.m[4 * a + 2] * n.m[4 * a + 2]);
                    for (int j = 0; j < 3 && l > 0; j++)
                        R[a][j] = n.m[4 * a + j] / l;
                    at[a] = n.m[12 + a];
                }
            } else {                                 /* [G: no node; every monster has its fxNode_Belch, so the
                                                      * data never takes this] */
                mon_body_centre(mo, at);
                at[2] += mon_half_height(mo);
            }
            if ((what & AMW_STOP) && w->fx >= 0 && v->fxw) {
                fx_stop(v->fxw, w->fx);
                w->fx = -1;
            }
            if ((what & AMW_START) && w->fire_fx && v->fxw) {
                if (w->fx >= 0)
                    fx_stop(v->fxw, w->fx);
                w->fx = fx_spawn_frame(v->fxw, w->fire_fx, at, (const float (*)[3])R, 1);
                fx_set_of_block(v->fxw, w->fx, 0);   /* the weapon's own (0x295750 ..), and a monster's weapon
                                                      * stays at a block's end (flag 0x200000) [R] */
                if (getenv("RAMPAGE_TRACE"))
                    printf("  effect %s at %.1f %.1f %.1f -> %d (%g s)\n", w->fire_fx, at[0], at[1], at[2], w->fx,
                           w->duration);
            }
            if (what & AMW_BURST)
                roar_burst(v, mo, w, at);
        }
    }
}

/* every monster's armed spheres, after the frame's posing (drawn with H), and what they hit of each other */
static void hit_spheres_all(Viewer *v)
{
    v->scene.nspheres = 0;
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        mo->sph0 = v->scene.nspheres;
        hit_spheres(mo);
        mo->nsph = v->scene.nspheres - mo->sph0;
    }
    if (v->nmon > 1)
        monsters_fight(v);
    if (!v->show_hits)
        v->scene.nspheres = 0;
}

static void hands_step(Mon *mo, float dt);

/* the foot object's update (0x33D450), from ik_feet between the posing and the foot IK, once a game frame */
static void feet_update(void *ctx, IkFeet *f)
{
    Mon *mo = ctx;
    if (!mo->feet_due || !mon_in(mo))
        return;
    mo->feet_due = 0;
    const Instance *in = mon_in(mo);
    FootFrame m;
    memset(&m, 0, sizeof m);
    m.place[0] = mo->mx; m.place[1] = mo->my; m.place[2] = mo->mz;
    m.heading = mo->heading;
    m.ground = ground_at(mo);
    for (int r = 0; r < 2; r++) {
        m.ok[r] = f->ok[r];
        memcpy(m.foot[r], f->foot[r], sizeof m.foot[r]);
        memcpy(m.hip[r], f->hip[r], sizeof m.hip[r]);
        m.reach[r] = f->reach[r];
    }
    m.blend_left = in->blend_left;
    m.blend_time = in->blend_time;
    m.steps = mo->feet_dt / TIMESTEP;                /* the frames since its last update, in world steps */
    mo->feet_dt = 0;
    foot_update(&mo->feet, &m, f->w, f->pos);
}

/* the end of the frame's move: the foot object updates at the next posing */
static void feet_step(Mon *mo, float dt)
{
    Viewer *v = mo->v;
    if (!mon_in(mo))
        return;
    mo->feet_dt += dt;
    v->scene.mon[mo->slot].feet = &mo->feet_ik;
    mo->feet_ik.update = feet_update;
    mo->feet_ik.ctx = mo;
    mo->feet_due = 1;
}

/* the frame's shadows (shadow.c): the buildings' on the lot (redone where a top came down), every object whose
 * template has ShadowData, the monster's blobs on the ground or roof and on the wall it climbs */
static void shadow_frame(Viewer *v, float dt)
{
    if (!v->slight.monster_blob)
        shadow_lighting_default(&v->slight);
    shadow_list_clear(&v->shl);
    v->scene.shadows = &v->shl;
    v->scene.shadow_bundle = v->have_fx_bundle ? &v->fx_bundle : NULL;
    if (!v->have_scene)
        return;
    for (uint32_t i = 0; i < v->scene.nbuildings && i < SHADOW_MAX_BUILDINGS; i++) {
        const Building *b = &v->scene.buildings[i].b;
        if (b->max[2] != v->sg.top[i]) {
            float box[6] = { b->min[0], b->min[1], b->min[2], b->max[0], b->max[1], b->max[2] };
            shadow_ground_top(&v->sg, (int)i, box);
        }
    }
    shadow_ground_draw(&v->shl, &v->sg, &v->slight);
    for (uint32_t i = 0; i < v->scene.ninstances; i++) {
        const Instance *in = &v->scene.instances[i];
        if (!in->shadow.at || in->hidden)
            continue;
        ShadowData d;
        shadow_data_default(&d);
        Group g = in->shadow;
        const char *tex = group_str(g, "Texture|TextureName");
        if (tex) d.texture = tex;
        d.intensity = group_num(g, "Intensity", d.intensity);
        d.max_height = group_num(g, "MaxHeight", d.max_height);
        d.full_height = group_num(g, "FullHeight", d.full_height);
        d.ex_proj_height = group_num(g, "ExProjHeight", d.ex_proj_height);
        Group rc = group_sub(g, "ShadowRect");
        Record r[4];
        if (rc.at && group_items(rc, r, 4) == 4)
            for (int k = 0; k < 4; k++)
                d.rect[k] = record_num(r[k]);
        const float *m = in->base.m;                 /* rows: its x, y, z axes, then its place */
        float m12[12] = { m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10], m[12], m[13], m[14] };
        shadow_object(&v->shl, &v->slight, &d, m12);
    }
    for (int k = 0; k < mon_world(v); k++) {        /* each monster's */
        Mon *mo = &v->mon[k];
        if (mon_in(mo) && !mon_in(mo)->hidden && v->play) {
            float h = mo->heading / 57.2957795f, c = cosf(h), sn = sinf(h), radius = mon_radius(mo);
            /* the ground point: the body centre (0x2FDA50 -> 0x2E4D30); the wall's: the skeleton's first node
             * (0x2FDD00 -> 0x2E61E0: monster +0xB18, the animation player's node 0 place (+0xAE8 + 0x30), through
             * its matrix: Bip01 as posed); the radius MONSTER_DATA +0x198 x the scale (0x2E4D00) */
            float centre[3], body[3] = { mo->mx, mo->my, mo->mz };
            mon_body_centre(mo, centre);
            Mat4 root;
            const Instance *rin = mon_in(mo);
            if (rin->tree.n && scene_monster_node(&v->scene, mo->slot, rin->tree.nodes[0].name, &root) == 0) {
                body[0] = root.m[12]; body[1] = root.m[13]; body[2] = root.m[14];
            }
            float mbox[6], rbox[6], *roof = NULL;
            int mounted = mo->mode == MODE_BUILDING && mo->mount.at.building >= 0 &&
                          (uint32_t)mo->mount.at.building < v->scene.nbuildings;
            if (mounted) {
                const Building *b = &v->scene.buildings[mo->mount.at.building].b;
                memcpy(mbox, b->min, 3 * sizeof(float));
                memcpy(mbox + 3, b->max, 3 * sizeof(float));
            }
            int under = mon_building_below(mo);      /* the building it is over (0x2FDA98: 0x2D6150) */
            if (under >= 0 && (uint32_t)under < v->scene.nbuildings) {
                const Building *b = &v->scene.buildings[under].b;
                memcpy(rbox, b->min, 3 * sizeof(float));
                memcpy(rbox + 3, b->max, 3 * sizeof(float));
                roof = rbox;
            }
            shadow_monster_update(&mo->smon, dt, centre, body, radius, mounted, mbox, roof, 0);
            float rot[9] = { c, sn, 0, -sn, c, 0, 0, 0, 1 };
            shadow_monster_draw(&v->shl, &v->slight, &mo->smon, rot, radius);
        }
    }
}

/* an emitter's sounds (fx.c): add3DSfx at the effect's place as it starts, moved with it, killed as it ends. Its id is
 * the emitter's (0x1F84F8 / 0x1F8544 -> 0x26D560: the emitter's pointer, + 1 for the second sound), not one the list
 * makes (0x274840 counts its own down from 0xFFFFFFFF), so out of range it is kept and starts again on coming back
 * (0x273CF0: 0x274218 drops only the list's own ids) [R] */
static int fx_sound(void *ctx, int event, const char *name, const float pos[3], int handle)
{
    (void)ctx;
    if (event == FXS_START)
        return sound_add3d(name, pos, 0, 1, 1);
    if (event == FXS_MOVE)
        sound_move(handle, pos);
    else
        sound_kill(handle);
    return 0;
}

/* a composite effect's camerashake part (fx.c): camshake or camshake_collapse at strength 1 where it plays */
static void fx_shake(void *ctx, const char *name, const float pos[3])
{
    Viewer *v = ctx;
    int slot = 1;
    for (int j = 2; j < 8; j++)                      /* a free one, else the oldest */
        if (!v->shakes[j].def || v->shakes[j].age > v->shakes[slot].age)
            slot = j;
    const ShakeDef *d = name && _stricmp(name, "camshake_collapse") == 0 ? &SHAKE_COLLAPSE : &SHAKE_MONSTER;
    shake_start(&v->shakes[slot], d, pos, 1, 0, &v->shake_seed);
}

/* the army each frame (army.c): the monster as it sees it, the buildings, then what it makes happen */
/* a move state of the monster by name, now (playReaction) */
/* playReaction(name) from a module (a geyser, the merry-go-round, a boss): the state, started at the monster's next
 * step as a slot's start is (0x2C0DC0 .. 0x2C0E78), so the new state's events from its start fire (React_Geyser's
 * mode 1 and addVelocity(0, 0, 18) at 0); started here, between the steps, they were passed over. The binding
 * (0x2ABB18: the state by name, 0x2D9890, none found nothing) goes through 0x2D9710(monster, state, 0, 0), which takes
 * it as setReaction does (not defeated, not shrunk below 0.3) and lets go (0x2DA570(monster, 0): let_go) [R]: the
 * garbage truck's last shake (React_Ground_Heavy_Back) drops the truck */
static void force_state(Mon *mo, const char *name)
{
    if (mo->defeated || mo->scale < 0.3f)
        return;
    for (int k = 0; name && mo->have_graph && k < mo->graph.n; k++)
        if (!_stricmp(mo->graph.states[k].name, name)) {
            mo->force_to = k + 1;
            let_go(mo, 0);
            return;
        }
}

/* resetMonsterState (the Monster binding 0x97 -> 0x2DCB40) [R]: the scale 1 at once (+0xA78 = +0xA7C = 1, +0xA80 the
 * one before), the velocity 0 (+0xBD30), its goals cleared (0x2EE760, the caller's dir_ai_clear), and unless it is
 * defeated (+0xA59; +0xAA2, also asked, is always set in play) nor shrunk below 0.3 (+0xA78) the graph's first state
 * (+0xA54 -> +0x30, Ground_Idle) started as a slot's is (+0x110 = its own entry at +0x118: 1, the state, flags 0),
 * and let go (0x2DA570(monster, 0): let_go); then, with foot IK (+0x258C), both feet let go (0x33BE90: no anchor,
 * no planted record), the mount cleared (+0xC644, +0xC645, the building +0xC640) and, with the arm IK (+0x2588), the
 * hands let go of the face (0x322EC0, 0.1 s: hand_detach as the frame finds no building). Its respawn cancelled
 * (0x2D36A0) is left out: EndBlockNI, BeginBlockNI and ResultsNI, which ask it, find the monsters alive. So a taunt of
 * the results (React_Victory, 3 s) stops as the walk to the next block begins; let run, the walk ran past EndBlockNI's
 * 5 s, and the monster was put at its end facing pi as the next block began (a turn cut short, or one played on from
 * there); and a monster put at ResultsNI's place (13, -42) keeps no foot planted where it was before (its legs bent
 * by the foot IK towards the old place, a foot off the street) */
static void reset_monster_state(Mon *mo)
{
    mo->scale = mo->scale_to = 1;
    mo->scale_left = 0;
    memset(mo->vel, 0, sizeof mo->vel);
    mo->power.speed = 1;                             /* 0x2DCBAC .. 0x2DCBB0: the speed scale 1, no time left (the
                                                      * rage, if on, goes on) [R] */
    mo->power.speed_time = 0;
    if (!mo->defeated && mo->have_graph && mo->graph.n > 0) {   /* (the scale was just set to 1: not below 0.3) */
        mo->force_to = 1;
        let_go(mo, 0);
    }
    if (mo->feet.links) {
        foot_step(&mo->feet, 0, 0);
        foot_step(&mo->feet, 1, 0);
        mo->feet.anchor = -1;
    }
    climb_unmount(&mo->mount);
}

/* the bosses' host: the army's weapons */
static int boss_weapon_make(void *ctx, ArmyWeapon *w, Group g)
{
    Viewer *v = ctx;
    return v->army ? army_weapon_make(v->army, w, g) : 0;
}
static void boss_weapon_tick(void *ctx, ArmyWeapon *w, int may, const float from[3], const float dir[3],
                             const float frame[3][3], int target, float dt, int part)
{
    Viewer *v = ctx;
    if (v->army)
        army_weapon_tick(v->army, w, may, from, dir, frame, target, dt, COLLIDE_TAG(COLLIDE_OWN_BOSS, part));
}

/* the monster an event names (its slot), else player 1's (a module's event for "the monster") */
static Mon *ev_mon(Viewer *v, int slot)
{
    return &v->mon[slot >= 0 && slot < v->nmon ? slot : 0];
}

/* ChallengeDef.Points (or the boss's 5000) / players into each player's category 5 [R ChallengeStatus.scr] */
static void challenge_points_all(Viewer *v, int points)
{
    for (int p = 0; v->game && p < v->nmon; p++)
        game_add_points(v->game, p, GPT_CHALLENGE, points / v->nmon);
}

/* a boss effect's frame on its part (the FollowEffect's follow, 0x291440: the offset x the object's matrix, or its
 * node's: createAndStartAttachedToNodeByName's name, ...AttachedToNode's number): the part's look's node as drawn,
 * else the part's own matrix; R its axes made 1 long, at the offset along them; 0 = ok */
static void frame_of(const Mat4 *m, float pos[3], float R[3][3]);
static int boss_fx_frame(Viewer *v, int part, const char *node, int node_id, const float off[3], float at[3],
                         float R[3][3])
{
    const BossPart *p = v->boss ? boss_part(v->boss, part) : NULL;
    if (!p)
        return -1;
    Mat4 m;
    int found = (node || node_id >= 0) && scene_boss_node_at(&v->scene, part, node, node_id, m.m) == 0;
    if (!found)
        memcpy(m.m, p->world, sizeof m.m);
    if (getenv("RAMPAGE_NODES") && (node || node_id >= 0))
        printf("  %s effect node %s / %d: %s\n", p->name ? p->name : "?", node ? node : "-", node_id,
               found ? "found" : "not found");
    float pos[3];
    frame_of(&m, pos, R);
    for (int k = 0; k < 3; k++)
        at[k] = pos[k] + R[0][k] * off[0] + R[1][k] * off[1] + R[2][k] * off[2];
    return 0;
}

/* the bosses' frame: what they see of the monsters and the buildings, then what they did */
/* NewYork.scr's centre building (findBuildingByPosition(0, 10, 1)) isFalling() || isDestroyed(): a storey coming
 * down (0x2CE230) or the building gone (+0x14C 0) [R] */
static int ny_centre_down(Viewer *v)
{
    float cy = 10 + v->block_shift;
    int down = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
        const Building *bb = &v->scene.buildings[i].b;
        if (bb->min[0] <= 0 && bb->max[0] >= 0 && bb->min[1] <= cy && bb->max[1] >= cy)
            down = bb->collapse >= 0 || bb->state == BUILDING_GONE;
    }
    return down;
}

static void boss_step(Viewer *v, float dt, const ArmyMonster *am, int nm)
{
    if (!v->boss)
        return;
    static BossMonster m[MONSTERS];                  /* (static: boss_hit reads it after the frame's update) */
    for (int k = 0; k < nm; k++) {
        memset(&m[k], 0, sizeof m[k]);
        memcpy(m[k].pos, am[k].pos, sizeof m[k].pos);
        m[k].heading = am[k].heading;
        m[k].move = am[k].move;
        m[k].climbing = v->mon[k].mode == MODE_BUILDING && v->mon[k].mount.mounted;   /* [R 0x2AAAD8] */
        m[k].spheres = am[k].spheres;
        m[k].nspheres = am[k].nspheres;
        memcpy(m[k].box_min, am[k].box_min, sizeof m[k].box_min);
        memcpy(m[k].box_max, am[k].box_max, sizeof m[k].box_max);
        memcpy(m[k].target, am[k].target, sizeof m[k].target);
    }
    static BossBuilding bl[64];
    int nb = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
        const SceneBuilding *sb = &v->scene.buildings[i];
        const Building *b = &sb->b;
        BossBuilding *o = &bl[nb];
        memset(o, 0, sizeof *o);
        o->pos[0] = sb->node.m[12]; o->pos[1] = sb->node.m[13]; o->pos[2] = sb->node.m[14];
        memcpy(o->min, b->min, sizeof o->min);
        memcpy(o->max, b->max, sizeof o->max);
        o->width = b->width;
        o->storeys = b->storeys;
        o->height = b->max[2];
        /* [R] isDestroyed (0x2AD780): +0x14C 0, gone; isFalling (0x2ADE10 -> 0x2CE230): a storey coming down
         * (G+0x1CC set: a floor dropping off a standing building too, not the cracking or the shaking);
         * findBuildingNearMonster (0x318320) and getRandomBuilding (0x2AECF8) take any with +0x14C not 0, one coming
         * down too */
        o->destroyed = b->state == BUILDING_GONE;
        o->falling = b->collapse >= 0;
        o->standing = !o->destroyed;
        o->hp_frac = b->hp_max > 0 ? (float)b->hp / (float)b->hp_max : 0;
        o->inst = sb->inst;
    }
    boss_set_view(v->boss, v->view.pos[0] - 45, v->view.pos[0] + 45);   /* the view's x range [G], as the army's */
    boss_set_city_destroyed(v->boss, game_city_destroyed(v->game));   /* gCommon.isCityDestroyed */
    boss_update(v->boss, dt, m, nm, bl, nb);
    BossEvent ev[64];
    int ne;
    while ((ne = boss_events(v->boss, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const BossEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            switch (e->type) {
            case BE_EFFECT:
                if (v->fxw) {
                    const BossPart *p = e->obj >= 0 ? boss_part(v->boss, e->obj) : NULL;
                    float off[3], at[3], R[3][3];
                    for (int k = 0; k < 3; k++)      /* the part's: the script's offset, in its frame */
                        off[k] = e->pos[k] - (p ? p->pos[k] : k == 0 ? mo->mx : k == 1 ? mo->my : mo->mz);
                    int id = p && boss_fx_frame(v, e->obj, e->node, e->level, off, at, R) == 0 ?
                             fx_spawn_frame(v->fxw, e->name, at, (const float (*)[3])R, 1) :
                             fx_spawn(v->fxw, e->name, e->pos, e->dir, 1);
                    if (getenv("RAMPAGE_TRACE"))
                        printf("  boss effect %s on part %d node %s / %d at %.1f %.1f %.1f -> %d\n", e->name, e->obj,
                               e->node ? e->node : "-", e->level, p ? at[0] : e->pos[0], p ? at[1] : e->pos[1],
                               p ? at[2] : e->pos[2], id);
                    fx_set_of_block(v->fxw, id, 0);  /* the bosses stay here at a block's end (block_left) */
                    if (id >= 0 && v->nbfx < 64) {   /* to follow its part or the monster, or to stop by its id */
                        v->bfx[v->nbfx].id = e->id;
                        v->bfx[v->nbfx].fx = id;
                        v->bfx[v->nbfx].obj = p ? e->obj : -1;
                        v->bfx[v->nbfx].monster = !p && e->monster >= 0 ? mo->slot : -1;
                        v->bfx[v->nbfx].node = e->node;
                        v->bfx[v->nbfx].node_id = e->level;
                        memcpy(v->bfx[v->nbfx].off, off, sizeof off);
                        v->nbfx++;
                    }
                }
                break;
            case BE_EFFECT_STOP:
                for (int k = 0; k < v->nbfx; k++)
                    if (v->bfx[k].id == e->id) {
                        fx_stop(v->fxw, v->bfx[k].fx);
                        v->bfx[k] = v->bfx[--v->nbfx];
                        break;
                    }
                break;
            case BE_SOUND: {                         /* [R] add3DSfx(name, 0) is 0x287860: one of the part's 16
                                                      * slots, the sound at its place (+0x2C) as it moves; deleted, the
                                                      * part takes its sounds with it (0x288050 -> 0x274710) */
                const BossPart *p = e->obj >= 0 ? boss_part(v->boss, e->obj) : NULL;
                if (e->level != 0 && e->name) {
                    int on = 0;
                    for (int k = 0; k < v->nbsnd; k++)
                        on += v->bsnd[k].obj == e->obj;
                    int h = on < 16 ? sound_add3d(e->name, p ? p->pos : e->pos, 0, 1, 1) : 0;
                    if (h && p && v->nbsnd < 64) {
                        v->bsnd[v->nbsnd].obj = e->obj;
                        v->bsnd[v->nbsnd++].h = h;
                    }
                    if (getenv("RAMPAGE_TRACE"))
                        printf("  boss sound %s on part %d%s\n", e->name, e->obj, h ? "" : " (refused)");
                } else if (e->level == 0)
                    for (int k = 0; k < v->nbsnd; k++)
                        if (v->bsnd[k].obj == e->obj) {
                            sound_kill(v->bsnd[k].h);
                            v->bsnd[k--] = v->bsnd[--v->nbsnd];
                        }
                break;
            }
            case BE_REACTION: if (mo->hp > 0) force_state(mo, e->name); break;
            case BE_VEL: memcpy(mo->vel, e->v, sizeof mo->vel); break;
            case BE_HP: if (mo->hp > 0) mo->hp = fmaxf(0, fminf(mon_hp_max(mo), mo->hp + e->amount)); break;
            case BE_FACING: mo->heading = e->amount * 57.2957795f; break;
            case BE_PUSH: mo->mx += e->v[0]; mo->my += e->v[1]; break;
            case BE_STATS: if (v->army) army_add_stats(v->army, mo->slot, e->tmpl, e->level); break;
            case BE_BOSS_HEALTH:                     /* the boss bar; at 0 the challenge is met: 5000 points */
                if (v->hud && hud_boss_health(v->hud, e->amount))
                    challenge_points_all(v, 5000);
                break;
            case BE_DEFEATED:                        /* New York's callback finishes the block (isBossBlockFinished) */
                if (v->game && e->name && strstr(e->name, "NewYork"))
                    game_set_boss_finished(v->game, ny_centre_down(v));
                break;
            case BE_COLLAPSE:
                if (e->building >= 0 && (uint32_t)e->building < v->scene.nbuildings)
                    building_destroy(&v->scene.buildings[e->building].b);
                break;
            case BE_NO_AUTOCOLLAPSE:
                if (e->building >= 0 && (uint32_t)e->building < v->scene.nbuildings)
                    v->scene.buildings[e->building].b.no_autocollapse = 1;
                break;
            case BE_CELL_DAMAGE:                     /* New York's walls broken open before the generators */
                if (e->building >= 0 && (uint32_t)e->building < v->scene.nbuildings)
                    building_set_cell(&v->scene.buildings[e->building].b, e->level, e->a, e->b, e->c);
                break;
            case BE_FORCE_DAMAGE:                    /* RoofItem.bsf's forceDamage: the roof item there is knocked
                                                      * off (the adornment nearest pos, within 2 units [G]) */
                if (!v->sobj || streetobj_force_damage(v->sobj, e->name, e->pos) == -1)   /* (-2: it is gone) */
                    scene_adornments_hit(&v->scene, e->pos, 2);
                break;
            case BE_CAMERA_RMH:                      /* Levels::changeTwoTiltCameraRMH: how tall the camera frames */
                v->cam.s.rep_monster_height = e->amount;
                break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != BE_EFFECT && e->type != BE_EFFECT_STOP && e->type != BE_SOUND)
                printf("  boss event %d (%s) part %d, %.2f\n", e->type, e->name ? e->name : "-", e->obj, e->amount);
        }
    for (int k = 0; k < v->nbsnd; k++) {             /* the sounds that follow their part; ended, the slot is free */
        const BossPart *p = boss_part(v->boss, v->bsnd[k].obj);
        if (!sound_active(v->bsnd[k].h) || !p || !p->alive)
            v->bsnd[k--] = v->bsnd[--v->nbsnd];
        else
            sound_move(v->bsnd[k].h, p->pos);
    }
    for (int k = 0; k < v->nbfx; k++) {              /* the effects that follow their part or the monster */
        float at[3], R[3][3];
        if (v->bfx[k].obj >= 0) {
            const BossPart *p = boss_part(v->boss, v->bfx[k].obj);
            if (!p || !p->alive) {
                v->bfx[k].obj = -1;
                continue;
            }
            if (boss_fx_frame(v, v->bfx[k].obj, v->bfx[k].node, v->bfx[k].node_id, v->bfx[k].off, at, R) == 0)
                fx_place(v->fxw, v->bfx[k].fx, at, (const float (*)[3])R);
            continue;
        } else if (v->bfx[k].monster >= 0) {
            const Mon *mo = &v->mon[v->bfx[k].monster];
            at[0] = mo->mx + v->bfx[k].off[0]; at[1] = mo->my + v->bfx[k].off[1]; at[2] = mo->mz + v->bfx[k].off[2];
        } else {
            if (!fx_alive(v->fxw, v->bfx[k].fx))
                v->bfx[k--] = v->bfx[--v->nbfx];
            continue;
        }
        fx_move(v->fxw, v->bfx[k].fx, at);
    }
}

/* the game's frame (game.c): what it sees (the monster's HP, the buildings standing), then what it says. The intro and
 * walk-out choreography plays as the scripts give it: their moves and walks are the AI's PlayMove and WalkTo goals
 * (director.c; a move the flow waits for on a monster with no AI is done at once), their cameras the script camera
 * (dircam_game_event); a new block is built after the frame */
static void game_step(Viewer *v, float dt)
{
    if (!v->game || game_left(v->game))
        return;
    GameInput in;
    memset(&in, 0, sizeof in);
    for (int k = 0; k < v->nmon && k < GAME_PLAYERS; k++)
        in.hp[k] = v->mon[k].hp;
    for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
        const Building *bb = &v->scene.buildings[i].b;
        in.buildings += bb->state != BUILDING_GONE;  /* [R 0x2AEE00: +0x14C not 0, its fall included] */
    }
    in.centre_down = ny_centre_down(v);
    game_update(v->game, dt, &in);
    GameEvent ev[32];
    int ne;
    while ((ne = game_events(v->game, ev, 32)) > 0)
        for (int i = 0; i < ne; i++) {
            const GameEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->slot);            /* the monster slot it is for (the monster events) */
            switch (e->type) {
            case GE_BLOCK: {                         /* the next block: in the game at once, else when it begins */
                if (e->b)                            /* [R 0x2B6760] the teardown's end: every 3D sound killed */
                    sound_objects_clear();
                int k = -1;
                for (uint32_t i = 0; e->name && i < v->nblocks; i++)
                    if (!_stricmp(v->blocks[i], e->name))
                        k = (int)i;
                if (k >= 0 && k != (int)v->block && !e->b) {
                    v->next_block = k;
                    v->block_shift = 41;             /* [R 0x302F0C: slot 1 at y 41, 0x303BB0 the block left at -41] */
                }
                /* the reveal's tubes in this block (0x3235F0), now if it is the one built */
                v->ntubes = reveal_block(&v->reveal, e->a, 1 << 30, v->tubes, REVEAL_LOCS);
                if (k == (int)v->block)
                    add_tubes(v);
                if (k >= 0 && k != (int)v->block && e->b) {
                    /* [R 0x2B66C0, 0x302AF0] in the game the block left is torn down and the new one laid out at
                     * once, as the results start: built at slot 0 once this frame is over (the monsters and the
                     * cameras 41 back), the block left's ground and rubble at -41 */
                    block_left(v);
                    v->next_block = -1;
                    v->block_shift = 41;
                    v->game_block = k;
                    v->left_block = (int)v->block;
                } else if (k >= 0 && e->b) {           /* the same block again in the game (a tied King block
                                                      * 0x325054, Game Over's Continue 0x325258): torn down and
                                                      * loaded fresh at once */
                    v->next_block = -1;
                    v->block_shift = 0;
                    v->game_block = k;
                }
                break;
            }
            case GE_STATE:
                if (e->a == GS_BLOCK_WON) {          /* 0x3255B0: the clock's seconds to "Clock", the check */
                    int vis;
                    float left = game_clock(v->game, &vis);
                    if (!challenge_block_won(&v->challenge, game_play_type(v->game), left))
                        v->challenges_lost = 1;
                    if (getenv("RAMPAGE_TRACE"))
                        printf("  challenge %s: %d/%d\n", challenge_was_met(&v->challenge) ? "met" : "not met",
                               challenge_collected(&v->challenge), challenge_required(&v->challenge));
                }
                if (e->a == GS_EXIT)
                    v->exit_game = 1;
                if (e->a == GS_GAME_OVER)            /* [R 0x324584 .. 0x3245B0] every 3D sound killed */
                    sound_objects_clear();
                if (e->a == GS_BEGIN_BLOCK && v->next_block >= 0) {
                    v->game_block = v->next_block;   /* built once this frame is over */
                    v->next_block = -1;
                    v->music_state = GS_BEGIN_BLOCK; /* its music then */
                } else                               /* the state's music (0x3247A8) */
                    music_for_state(e->a, v->have_scene ? v->scene.music : NULL);
                break;
            case GE_EXIT:                            /* the world is left: the app's next package is the shell's
                                                      * (+0xA8 = 0x4D53F0), whatever the way (0 the menus, 1 King of
                                                      * the World's next city b, 2 the demo's end 0x324D18) [R] */
                v->exit_game = 1;
                if (e->a == 2)
                    v->attract_button = e->b == 1;
                for (int k = 0; k < GAME_PLAYERS && k < MONSTERS; k++)   /* GameStatus' players' cities won
                                                      * go on with it (0x4D7990, King of the World) */
                    v->cities_won[k] = game_cities_won(v->game, k);
                if (e->a == 1 && e->b >= 0) {        /* GameStatus' city is the next one, from its first block */
                    v->last_game.city_index = e->b;
                    v->last_game.city[0] = 0;
                    v->last_game.start_block = 0;
                } else                               /* GameStatus' start block (+0x210) is the shell's too: the
                                                      * block after the one won (activateNextBlock), the checkpoint's
                                                      * after a game over; Change Monster starts the city there */
                    v->last_game.start_block = game_start_block(v->game);
                break;
            case GE_RUBBLE_FX:                       /* at the point game_rubble_point found, 1.5 up */
                if (v->fxw && e->a >= 0 && e->a < 3) {
                    /* (the new block's frame already when the scene is rebuilt at this frame's end: the effects
                     * are not carried with it) */
                    float at[3] = { e->pos[0], e->pos[1] + (v->game_block >= 0 ? 0 : v->block_shift), e->pos[2] + 1.5f };
                    if (v->rubble_fx[e->a] >= 0)
                        fx_stop(v->fxw, v->rubble_fx[e->a]);
                    v->rubble_fx[e->a] = fx_spawn(v->fxw, e->name, at, NULL, 1);
                    if (getenv("RAMPAGE_TRACE"))
                        printf("  rubble effect %d: %s at %.1f %.1f %.1f\n", e->a, e->name, at[0], at[1], at[2]);
                }
                break;
            case GE_RUBBLE_FX_STOP:
                for (int k = 0; k < 3; k++) {
                    if (v->fxw && v->rubble_fx[k] >= 0)
                        fx_stop(v->fxw, v->rubble_fx[k]);
                    v->rubble_fx[k] = -1;
                }
                break;
            case GE_AI:                              /* setIsAI; a bot's destroyBuildings, the computer's players' */
                mo->is_ai = e->a || mo->drive == DRIVE_COMPUTER;
                if ((e->b || (!e->a && mo->drive == DRIVE_COMPUTER)) && mo->have_graph)
                    dir_ai_destroy(&mo->ai, &mo->graph, v->bot_seed = v->bot_seed * 69069u + 1u + (uint32_t)mo->slot);
                break;
            case GE_WALK:                            /* walkTo(pos, radius) and its way point */
                if (mo->have_ag) {
                    float to[3] = { e->pos[0], e->pos[1] + v->block_shift, e->pos[2] };
                    float wp[3] = { e->v[0], e->v[1] + v->block_shift, e->v[2] };
                    int w = dir_ai_walk_to(&mo->ai, to, e->f, 0);
                    if (w >= 0)
                        dir_ai_add_waypoint(&mo->ai, w, wp, e->f);
                }
                break;
            case GE_CAMERA: {
                float at[3] = { e->pos[0], e->pos[1] + v->block_shift, e->pos[2] };
                float to[3] = { e->v[0], e->v[1] + v->block_shift, e->v[2] };
                dircam_game_event(&v->cd, e->name, e->f, at, to, e->a);
                break;
            }
            case GE_RESET:
                if (e->type == GE_RESET) {
                    dir_ai_clear(&mo->ai);            /* clearAllGoals */
                    reset_monster_state(mo);          /* resetMonsterState */
                }
                /* fall through */
            case GE_PLACE:
                if (e->a == 1 && e->type == GE_PLACE) {  /* SkipResultsNI's -41: the block left; here it stays */
                    mo->mx += e->pos[0]; mo->my += e->pos[1] + v->block_shift; mo->mz += e->pos[2];
                } else if (e->a >= 1 || e->type == GE_PLACE) {
                    mo->mx = e->pos[0]; mo->my = e->pos[1] + v->block_shift; mo->mz = e->pos[2];
                    if (e->a >= 1) {                 /* setFacingZ, setVel(0, 0, 0) */
                        mo->heading = e->f * 57.2957795f;
                        memset(mo->vel, 0, sizeof mo->vel);
                    }
                }
                break;
            case GE_MOVE:                            /* playMove: a goal of the AI, queued whether the monster is AI
                                                      * yet or not (Levels::playMove sets it after: setIsAI(1));
                                                      * without a goal graph, done at once */
                if (mo->have_ag && mo->have_graph)
                    dir_ai_play_move(&mo->ai, e->name ? moves_find(&mo->graph, e->name) : -1, e->a);
                else if (e->a == 1)
                    game_move_done(v->game, e->slot);
                break;
            case GE_AI_ACTIVATE:
                if (v->army) army_activate(v->army, 1);
                if (v->boss) boss_activate(v->boss, 1);
                if (v->rail) rail_activate(v->rail, 1);
                parade_activate(v->parade, 1);
                break;
            case GE_HUD:                             /* every player's HUD */
                for (int k = 0; k < v->nmon; k++)
                    if (v->mon[k].hud)
                        hud_show(v->mon[k].hud, (float)e->a);
                break;
            case GE_GUI: if (v->hud && e->name) hud_load_interface(v->hud, e->name); break;
            case GE_RESULTS:                         /* WorldGuiDriver::startResults: filled now (the points are
                                                      * cleared once it is done) */
                if (v->hud) {
                    HudResults r;
                    memset(&r, 0, sizeof r);
                    r.play_type = game_play_type(v->game);   /* GP_* and HUD_PLAY_* agree */
                    r.num_players = v->nmon;
                    for (int p = 0; p < r.num_players; p++) {
                        for (int c = 0; c < 7; c++) {
                            r.points[p][c] = game_points(v->game, p, c);
                            r.kills[p][c] = game_kills(v->game, p, c);
                        }
                        r.monsters[p] = v->mon[p].monster >= 0 ? v->monsters[v->mon[p].monster] : NULL;
                        r.blocks_won[p] = game_blocks_won(v->game, p);
                        r.cities_won[p] = game_cities_won(v->game, p);
                    }
                    int sb = game_start_block(v->game);   /* getMonsterName(-1): the roster's monster of that number */
                    r.monster_none = sb >= 0 && sb < v->roster.n ? v->roster.name[sb] : NULL;
                    r.king_block = game_king(v->game, 0);
                    r.king_city = game_king(v->game, 1);
                    r.king_world = game_king(v->game, 2);
                    r.blocks_to_win = game_blocks_to_win(v->game);
                    r.cities_to_win = game_cities_to_win(v->game);
                    int ci = game_city_index(v->game);
                    r.next_city = ci >= 0 && ci < 6 ? CAMPAIGN[ci + 1] : NULL;
                    r.clock_time = game_clock(v->game, NULL);
                    r.clock_limit = game_clock_limit(v->game);
                    r.block_played = game_block_name(v->game, 0);
                    r.block_current = game_block_name(v->game, 1);
                    for (uint32_t k = 0; k < v->scene.nbuildings; k++)
                        r.buildings += v->scene.buildings[k].b.state != BUILDING_GONE;   /* [R 0x2AEE00: +0x14C not
                                                      * 0, its fall included] */
                    r.city_destroyed = game_city_destroyed(v->game);
                    r.city = CITIES[v->city_index];
                    r.city_score = game_city_total(v->game);
                    r.par_score = game_par_score(v->game);
                    r.city_flags = game_city_flags(v->game) | (v->challenges_lost ? 0 : CITY_FLAG_CHALLENGES);
                    r.monster_revealed = reveal_was_revealed(&v->reveal);
                    r.results_text = game_block_str(v->game, "ResultsNIText");   /* Results.scr:428 (NY10's
                                                      * ChallengeBossDefeat) */
                    r.show_victory = game_world_done(v->game);   /* LevelResults.scr:228: getShowVictory (0x2C3180,
                                                      * 0x4D7E4F) hides Change Monster and Quit */
                    v->results_update = e->a == GR_SCREEN_RESULTS && r.play_type == GP_CAMPAIGN;
                    if (v->results_update)           /* the block won; Results.bsf's updateProfile and saveProfile
                                                      * 1.5 s in (HUD_CALL_SAVE_PROFILE) */
                        profile_block_won(&v->prof, r.city_flags | PC_AVAILABLE, r.city_score, game_block(v->game, NULL),
                                          r.city_destroyed);
                    r.allow_save = v->prof.allow_save && v->prof_write;
                    r.loaded = v->prof.loaded;
                    hud_results(v->hud, e->a, &r);
                }
                break;
            case GE_REVIVE:                          /* cancelRespawn: full HP where it is */
                mo->hp = mon_hp_max(mo);
                mo->defeated = 0;
                mo->human = mo->dropping = 0;
                mo->scale = mo->scale_to = 1;
                if (mon_in(mo))
                    mon_in(mo)->hidden = 0;
                if (mo->streaker >= 0 && v->scene.folk)
                    people_remove(v->scene.folk, mo->streaker);
                mo->streaker = -1;
                if (e->a == 1)                       /* the continue: setEnergy(0) (0x325288 -> 0x2E5730) [R];
                                                      * a rage runs on till its clock and energy are out */
                    edible_power_set_energy(&mo->power, 0, others_full(mo));
                mo->energy = mo->power.energy;
                break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != GE_MOVE)
                printf("  game event %d (%s) a %d, state %d\n", e->type, e->name ? e->name : "-", e->a,
                       game_state(v->game));
        }
}

/* a Graphics page's values (the front end's; the Pause menu's HudGraphics copied into one) into the
 * settings, applied; keep: the page's Accept, written to rampage.cfg (not by --shot runs) */
static void graphics_from_menu(Viewer *v, const ShellGraphics *g, int keep)
{
    if (!v->gfx || !v->win)
        return;
    Settings to = *v->gfx;
    to.ao = g->ao; to.smaa = g->smaa; to.msaa = g->msaa; to.aniso = g->aniso; to.mips = g->mips;
    to.fullscreen = g->fullscreen; to.vsync = g->vsync;
    settings_graphics(v->gfx, v->win, &to);
    if (keep && v->gfx_write)
        settings_save(v->gfx, "rampage.cfg");
}
static void shell_give_graphics(Viewer *v);

/* after the screens ran: a results screen's answer to the game, the sounds they start and stop */
static void hud_frame(Viewer *v, int after)
{
    if (!after || !v->hud)
        return;
    int r = hud_result(v->hud);
    if (r == GR_CONTINUE && v->game && game_state(v->game) == GS_GAME_OVER) {   /* back to the checkpoint */
        profile_restore_checkpoint(&v->prof);
        profile_new_city(&v->prof);
    }
    if (r == GR_CONTINUE && v->game && game_state(v->game) == GS_GAME_OVER && reveal_was_revealed(&v->reveal))
        extra_hide(v, reveal_continue(&v->reveal));  /* 0x323450: the tube comes back, the monster hides */
    if (r && v->game)
        game_set_result(v->game, r);
    float sv, mv;
    int rumble;
    switch (hud_pause_result(v->hud)) {
    case HUD_PAUSE_QUIT:                             /* ConfirmQuit's Yes: gWorld.restartShell */
        if (v->game)
            game_restart_shell(v->game);
        else
            v->exit_game = 1;
        break;
    case HUD_PAUSE_SETTINGS:                         /* the profile's volumes (0 .. 0.75: the game's full one) */
        hud_settings(v->hud, &sv, &mv, &rumble);
        profile_set_sound_volume(&v->prof, sv);      /* written with the next Results */
        profile_set_music_volume(&v->prof, mv);
        v->vibrate = profile_set_rumble(&v->prof, rumble, 1);
        v->sfx_vol = sv / 0.75f;
        v->music_vol = v->music_now = mv / 0.75f;
        sound_settings(v->sfx_vol * 0.75f, v->music_now * 0.75f, 0);
        break;
    default: break;
    }
    HudCall calls[16];
    int nc = hud_calls(v->hud, calls, 16);
    shell_give_graphics(v);                          /* the settings in use, for the Pause menu's Graphics page */
    for (int i = 0; i < nc; i++)                     /* gSndDev's volumes */
        switch (calls[i].call) {
        case HUD_CALL_GRAPHICS: {                    /* the Pause menu's Graphics page: tried, put back or kept */
            const HudGraphics *hg = hud_graphics(v->hud);
            ShellGraphics g = { hg->ao, hg->smaa, hg->msaa, hg->aniso, hg->mips, hg->fullscreen, hg->vsync };
            graphics_from_menu(v, &g, calls[i].arg);
            break;
        }
        case HUD_CALL_MUSIC_VOLUME:                  /* setMusicVolume(v, ms): the music slots go there over ms
                                                      * (0x26B5E0, group 3) [R Pause.scr: 0 over 1 s at the menu's
                                                      * opening and back over 1 s at Continue] */
            v->music_now = calls[i].value / 0.75f;
            sound_settings(v->sfx_vol * 0.75f, v->music_now * 0.75f, calls[i].arg);
            break;
        case HUD_CALL_SFX_VOLUME: v->sfx_vol = calls[i].value / 0.75f; sound_settings(v->sfx_vol * 0.75f, v->music_now * 0.75f, 0); break;
        case HUD_CALL_SAVE_SETTINGS:
            hud_settings(v->hud, &sv, &mv, &rumble);
            profile_set_sound_volume(&v->prof, sv);
            profile_set_music_volume(&v->prof, mv);
            v->vibrate = profile_set_rumble(&v->prof, rumble, 1);
            v->sfx_vol = sv / 0.75f;
            v->music_vol = v->music_now = mv / 0.75f;
            sound_settings(v->sfx_vol * 0.75f, v->music_now * 0.75f, 0);
            break;
        case HUD_CALL_RUMBLE:                        /* setEnableRumble(on, save) */
            v->vibrate = profile_set_rumble(&v->prof, calls[i].value != 0, calls[i].arg);
            break;
        case HUD_CALL_RUMBLE_PULSE:                  /* gCommon.rumble(ms): pad 0 [R 0x2C34C8] */
            if (v->pads_live && v->vibrate)
                pad_rumble(0, 0xC0 / 255.0f, calls[i].arg / 1000.0f);
            break;
        case HUD_CALL_PLAY_MUSIC:
            if (!music_playing() && v->scene.music)
                music_play(v->scene.music);
            break;
        case HUD_CALL_SAVE_PROFILE:
            if (calls[i].arg && v->results_update)
                profile_update(&v->prof);
            save_profile(v);                         /* the SaveGame popup writes it (shell.c) */
            break;
        case HUD_CALL_ALLOW_SAVE: v->prof.allow_save = 1; break;   /* setAllowSave(1): flag 4 alone */
        default: break;                              /* the cheats: nothing here */
        }
    HudSound hs[16];
    int n = hud_sounds(v->hud, hs, 16);
    for (int i = 0; i < n; i++) {
        if (hs[i].kill)
            sound_kill_name(hs[i].name);
        else
            sound_play(hs[i].name, NULL, 1);
    }
}

/* Monsters::startRage on screen: the RAMPAGE banner (Rage.ifc) */
static void rage_started(Mon *mo)
{
    Viewer *v = mo->v;
    mon_rage_fx(mo, 1);
    if (v->hud)
        hud_banner_rage(v->hud);
    dircam_rage(&v->cd, mo->slot);                          /* the script camera on the monster for 1.5 s */
}

/* gCommon.addStats on the monster (0x2C51A0): HP and energy by the rules edible.c keeps (none at 0 HP; while raging
 * only a full Energy restarts it); a full one starts the rage: Monsters::startRage, slot 54 (its setReaction(54),
 * Monsters.bsf [R]) */
static void monster_stats(Mon *mo, float health, float energy)
{
    if (edible_power_add_stats(&mo->power, &mo->hp, mon_hp_max(mo), health, energy, others_full(mo))) {
        edible_power_start_rage(&mo->power, others_full(mo));
        set_reaction(mo, 54);
        rage_started(mo);
        if (getenv("RAMPAGE_TRACE"))
            printf("  rage\n");
    }
    mo->energy = mo->power.energy;
}

/* the cell a monster call's (x, y, z) names (getBuildingOccupant, people_probe): its building, storey as built and
 * column, and whether a person is at its window; the edibles' answer to the call, -1 none */
static int edible_call(Mon *mo, const char *call, const float *xyz)
{
    Viewer *v = mo->v;
    if (!v->edibles)
        return -1;
    PeopleBox boxes[64];
    int nb = scene_people_boxes(&v->scene, boxes, 64), b = -1, ls = -1, col = -1;
    float at[3] = { mo->mx, mo->my, mo->mz };
    int found = people_probe(boxes, nb, at, xyz, mo->mirror, &b, &ls, &col);
    int o = found && b >= 0 && ls >= 0 && ls < v->scene.buildings[b].b.n ? v->scene.buildings[b].b.live[ls] : -1;
    if (o >= 0 && edible_hides(v->edibles, b, o, col))
        o = -1;                                      /* 0x2B3900: no slot, neither its person nor its edible */
    int person = o >= 0 && ((v->scene.folk && people_window_at(v->scene.folk, b, o, col) >= 0) ||
                            scene_window_soldier(&v->scene, b, o, col) >= 0);   /* an occupant actor, either sort */
    return edible_monster_call(v->edibles, call, o >= 0 ? b : -1, o, col, person, mo->slot,
                               mo->have_graph ? mo->graph.states[mo->run.state].name : NULL);
}

/* the edibles' frame: their clocks, then what they did */
static void edible_step(Viewer *v, float dt)
{
    if (!v->edibles)
        return;
    edible_update(v->edibles, dt);
    for (int k = 0; k < v->nmon; k++) {              /* each monster's rage and speed */
        Mon *mo = &v->mon[k];
        if (edible_power_update(&mo->power, dt, mo->hp)) {   /* stopRage (0x2D4340): its effect released */
            mon_rage_fx(mo, 0);
            if (getenv("RAMPAGE_TRACE"))
                printf("  the rage ends\n");
        }
        mo->energy = mo->power.energy;
    }
    EdibleEvent ev[32];
    int ne;
    while ((ne = edible_events(v->edibles, ev, 32)) > 0)
        for (int i = 0; i < ne; i++) {
            const EdibleEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            switch (e->type) {
            case EE_EFFECT:
                if (v->fxw) {
                    /* one that follows its edible is on it, offset 0 (createAndStartAttachedWithOffset(actor,
                     * name, 0)): where the edible is now, not where it was made (the shake lifts it 1.5 the frame
                     * after, before this event is read) */
                    const Edible *it = e->edible >= 0 ? edible_get(v->edibles, e->edible) : NULL;
                    int id = fx_spawn(v->fxw, e->name, it ? it->pos : e->pos, NULL, 1);
                    if (id >= 0 && it && v->nefx < 64) {
                        v->efx[v->nefx].fx = id;
                        v->efx[v->nefx].edible = e->edible;
                        v->efx[v->nefx].id = e->id;
                        memset(v->efx[v->nefx].off, 0, sizeof v->efx[v->nefx].off);
                        v->nefx++;
                    }
                }
                break;
            case EE_EFFECT_STOP:                     /* e->id is edible.c's handle, not the effect world's */
                for (int k = 0; k < v->nefx; k++)
                    if (v->efx[k].id == e->id) {
                        if (v->fxw)
                            fx_stop(v->fxw, v->efx[k].fx);
                        v->efx[k] = v->efx[--v->nefx];
                        break;
                    }
                break;
            case EE_SOUND: obj_sound(v, OS_EDIBLE, e->edible, e->name, 0, e->pos); break;   /* add3DSfx */
            case EE_MONSTER_FUNCTION: mon_function(mo, e->name); break;
            case EE_REACTION:                        /* playReaction: the state of that name, only while alive; at
                                                      * the next step, its clip with it (forced here, the runner went
                                                      * on in the old clip from 0: Face_Eat_Item twice, no Yum) */
                if (mo->hp > 0)
                    force_state(mo, e->name);
                break;
            case EE_SPEED: edible_power_speed(&mo->power, e->amount, e->v[0]); break;
            case EE_STATS:                           /* gCommon.addStats once (0x2C51A0): through the army (its
                                                      * AE_STATS_* give the HP and energy), else here */
                if (v->army)
                    army_add_stats(v->army, mo->slot, e->tmpl, e->level);
                else
                    monster_stats(mo, e->v[0], e->v[1]);
                break;
            case EE_BANNER:
                if (v->hud)
                    hud_float(v->hud, (int)e->amount, e->pos, 2, 1.0f);
                break;
            case EE_MESSAGE:                         /* "Instant Rampage", "Speed Increase", ... (Rage.ifc) */
                if (v->hud)
                    hud_banner_text(v->hud, e->name, e->v, e->amount, e->name2);
                break;
            case EE_REVEAL: {                        /* the cryotube: gWorld.startReveal, "<monster> Unlocked" */
                int slot, i = reveal_start(&v->reveal, v->nmon, &v->roster, &slot);
                if (i >= 0)                          /* unlocked: the profile's monster flags */
                    profile_set_monster_flags(&v->prof, 0, i, v->roster.flags[i]);
                if (v->hud && i >= 0)
                    hud_banner_reveal(v->hud, v->roster.name[i]);
                if (getenv("RAMPAGE_TRACE"))
                    printf("  revealed: %s\n", i >= 0 ? v->roster.name[i] : "-");
                if (i >= 0 && slot >= 0 && slot == v->extra)   /* shown: the walk-on (0x2D40B0) */
                    reveal_walk_start(&v->rwalk, slot, e->pos);
                else
                    reveal_stop(&v->reveal);
                break;
            }
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != EE_EFFECT && e->type != EE_EFFECT_STOP)
                printf("  edible event %d (%s) from %d\n", e->type, e->name ? e->name : "-", e->edible);
        }
    for (int k = 0; k < v->nefx; k++) {              /* the effects that follow their edible, while it is out */
        const Edible *it = edible_get(v->edibles, v->efx[k].edible);
        if (!it || it->phase == EP_GONE || !fx_alive(v->fxw, v->efx[k].fx)) {
            v->efx[k--] = v->efx[--v->nefx];
            continue;
        }
        float at[3] = { it->pos[0] + v->efx[k].off[0], it->pos[1] + v->efx[k].off[1], it->pos[2] + v->efx[k].off[2] };
        fx_move(v->fxw, v->efx[k].fx, at);
    }
}

/* the edibles' host: a front cell's state and where its edible stands */
static int edible_cell_state(void *ctx, int building, int storey, int column)
{
    const Viewer *v = ctx;
    if (building < 0 || (uint32_t)building >= v->scene.nbuildings)
        return -1;
    const Building *b = &v->scene.buildings[building].b;
    int ci = building_cell_index(b, FACE_FRONT, column);
    return ci < 0 || storey < 0 || storey >= b->storeys || building_live_of(b, storey) < 0 ? -1 : b->cell[storey][ci].state;
}
static int edible_cell_where(void *ctx, int building, int storey, int column, float out[3])
{
    const Viewer *v = ctx;
    if (building < 0 || (uint32_t)building >= v->scene.nbuildings)
        return 0;
    const Building *b = &v->scene.buildings[building].b;
    int ls = storey >= 0 && storey < b->storeys ? building_live_of(b, storey) : -1;
    if (ls < 0)
        return 0;
    float bottom, height;
    building_storey(b, ls, &bottom, &height);
    edible_cell_place(b->min, column, bottom, out);
    return 1;
}
static int edible_cell_follow(void *ctx, int building, int storey, int column, float out[3])
{
    const Viewer *v = ctx;
    if (building < 0 || (uint32_t)building >= v->scene.nbuildings ||
        v->scene.buildings[building].b.shake_time <= 0 || !edible_cell_where(ctx, building, storey, column, out))
        return 0;
    out[0] += building_shake(&v->scene.buildings[building].b);   /* 0x2BAD50: along the world's x */
    return 1;
}

/* the monster's TakeDamage (vtable +0x34, 0x2D9680), where a weapon without a ReactionLevel goes (0x33BCA0): the
 * level by the amount against MONSTER_DATA +0x12C, +0x130, +0x134 (3, 15, 30): below the first 4 (no reaction), below
 * the second 0, below the third 1, else 2; then 0x2D6A50 [R] */
static int take_damage_level(const Mon *mo, float amount)
{
    const float *t = mo->have_graph ? mo->graph.react : NULL;
    return amount < (t ? t[0] : 3) ? 4 : amount < (t ? t[1] : 15) ? 0 : amount < (t ? t[2] : 30) ? 1 : 2;
}

/* 0x2D6A50's start: no hit at 0 HP (+0xA4C), in a grab (+0xB9C4: no monster grabs another) or with cheat 986960
 * (0x4D7E68 & 4); the interruptible byte (+0xAA2) is always set in play [R] */
static int monster_hittable(const Mon *mo)
{
    return !mo->defeated && mo->hp > 0 && !(mo->v->prof.cheats & PCH_4);
}

/* 0x2D6A50's end, after the knock: the reaction's slot (0x2D7F38 .. 0x2D8BB0; set_reaction: not while defeated or
 * shrunk below 0.3), the pad by the level (400 ms at 1, 800 at 2 [R 0x2D8BE4, 0x2D8C14]), the HP off (0x2D9364). At 0
 * HP with no reaction asked (+0x110) the defeat: slot 67 React_Defeated, the velocity 0, +0xA59 (0x2D9518 ..
 * 0x2D95F4); with one asked it plays, and the update's own test fells the monster once it has started and the
 * monster is out of the air (mon_step, 0x2E68D0) [R] */
static void monster_take(Mon *mo, const char *name, float amount, int level, int slot)
{
    if (slot >= 0)                                   /* (what it holds let go of: set_reaction) */
        set_reaction(mo, slot);
    if (level == 1 || level == 2)
        mon_rumble(mo, level == 1 ? 400 : 800);
    mo->hp = fmaxf(0, mo->hp - amount);
    if (mo->hp <= 0 && mo->pending_slot < 0) {
        set_reaction(mo, 67);
        mo->defeated = 1;
        memset(mo->vel, 0, sizeof mo->vel);
    }
    if (getenv("RAMPAGE_TRACE"))
        printf("  hit by %s: %.1f, HP %.0f, slot %d\n", name ? name : "?", amount, mo->hp, slot);
}

/* 0x2D6A50 without an attack record (a shot, the roar, TakeDamage): the knock at any level (0x2D7928: per axis the
 * larger of knock and velocity), then the reaction by its level and the velocity it gave */
static void monster_hit(Mon *mo, const char *name, float amount, int level, const float knock[3], const float from[3])
{
    if (!monster_hittable(mo))
        return;
    float heading = mo->heading / 57.2957795f, at[3] = { mo->mx, mo->my, mo->mz };
    mo->friction = monster_knock_shot(mo->vel, knock);
    int slot = army_reaction_slot(level, mo->vel, mo->mode, army_hit_behind(at, heading, from));
    monster_take(mo, name, amount, level, slot);
}

/* 0x2D6A50 with the attacker's attack record (the fight, 0x2E35D0): the knock and the reaction by look.c monster_blow
 * (the attacker's front and right are its +0x14 and +0x8 rows, its state's node its +0x10C), then the hit's end */
static void monster_blow_hit(Mon *mo, const Mon *by, const struct MonAtk *atk, const char *name)
{
    if (!monster_hittable(mo))
        return;
    float h = by->heading / 57.2957795f, vh = mo->heading / 57.2957795f;
    const MoveState *st = by->have_graph ? &by->graph.states[by->run.state] : NULL;
    MonsterBlow b = { atk->level, atk->kind, atk->side, atk->knock, atk->knock_time, atk->lift, atk->turn,
                      { atk->over[0], atk->over[1], atk->over[2], atk->over[3] }, { by->mx, by->my },
                      { -sinf(h), cosf(h) }, { cosf(h), sinf(h) }, st && st->aim, by->mirror };
    float at[3] = { mo->mx, mo->my, mo->mz }, from[3] = { by->mx, by->my, by->mz }, right[2] = { cosf(vh), sinf(vh) };
    int slot = monster_blow(&b, at, right, mo->mirror, mo->mode, army_hit_behind(at, vh, from), &mo->air_hits,
                            mo->vel, &mo->friction);
    if (getenv("RAMPAGE_TRACE"))
        printf("  blow by %s (kind %d, level %d): %s, velocity %.1f %.1f %.1f, friction %.1f\n", name, atk->kind,
               atk->level, slot == MONSTER_BLOW_BLOCKED ? "none (past the air hits)" : "knocked", mo->vel[0],
               mo->vel[1], mo->vel[2], mo->friction);
    if (slot != MONSTER_BLOW_BLOCKED)
        monster_take(mo, name, atk->damage, atk->level, slot);
}

/* the vehicles' frame (vehicle.c): what they see of the monster and the buildings, then what they did */
static void vehicle_step(Viewer *v, float dt, const ArmyMonster *m, int nm, const ArmyBox *boxes, int nb)
{
    if (!v->veh)
        return;
    vehicle_update(v->veh, dt, m, nm, boxes, nb);
    VehicleEvent ev[64];
    int ne;
    while ((ne = vehicle_events(v->veh, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const VehicleEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            switch (e->type) {
            case VE_EFFECT:
                if (v->fxw) {
                    const VehObj *o = e->obj >= 0 ? vehicle_obj(v->veh, e->obj) : NULL;
                    int beam = o && o->beam_fx == e->id;     /* the stream: in the nozzle's firing frame */
                    float R[3][3];                   /* one on the car: in its whole frame, at its origin [R 0x277100
                                                      * -> 0x291ED0: the identity offset x its matrix] */
                    for (int a = 0; a < 3 && o; a++)
                        for (int k = 0; k < 3; k++)
                            R[a][k] = o->world[4 * a + k];
                    int id = beam ? fx_spawn_frame(v->fxw, e->name, e->pos, o->beam.R, 1) :
                             o ? fx_spawn_frame(v->fxw, e->name, o->pos, (const float (*)[3])R, 1) :
                                 fx_spawn(v->fxw, e->name, e->pos, e->dir, 1);
                    if (id >= 0 && v->nvfx < 64) {   /* to follow its object, or to stop by its handle */
                        v->vfx[v->nvfx].id = e->id;
                        v->vfx[v->nvfx].fx = id;
                        v->vfx[v->nvfx].obj = o ? e->obj : -1;
                        for (int k = 0; k < 3; k++)   /* createAndStartAttached: on the object, offset 0 [R 0x277100
                                                       * -> 0x291ED0] (the place it was emitted at may be stale: a car
                                                       * constructed before it is put on its line); the beam its nozzle's */
                            v->vfx[v->nvfx].off[k] = beam ? e->pos[k] - o->pos[k] : 0;
                        v->nvfx++;
                    }
                }
                break;
            case VE_EFFECT_STOP:
                for (int k = 0; k < v->nvfx; k++)
                    if (v->vfx[k].id == e->id) {
                        fx_stop(v->fxw, v->vfx[k].fx);
                        v->vfx[k] = v->vfx[--v->nvfx];
                        break;
                    }
                break;
            case VE_SOUND: obj_sound(v, OS_VEHICLE, e->obj, e->name, e->level, e->pos); break;   /* add3DSfx */
            case VE_HIT: monster_hit(mo, e->name, e->amount, e->level, e->v, e->pos); break;
            case VE_BUILDING_HIT:                    /* the fire truck's stream: Building::hit, no one's */
                if ((uint32_t)e->obj < v->scene.nbuildings && v->scene.buildings[e->obj].b.state == BUILDING_STANDING) {
                    v->scene.buildings[e->obj].hitter = -1;
                    building_hit(&v->scene.buildings[e->obj].b, e->pos, e->amount);
                }
                break;
            case VE_REACTION: set_reaction(mo, e->level); break;
            case VE_FART: mon_fart_later(mo, e->amount); break;
            case VE_SET_VEL: memcpy(mo->vel, e->v, sizeof mo->vel); break;
            case VE_HP: if (mo->hp > 0) mo->hp = fmaxf(0, fminf(mon_hp_max(mo), mo->hp + e->amount)); break;
            case VE_STATS:                           /* id 1: the army applied it (its events follow) */
                if (e->id == 0) {
                    monster_stats(mo, e->v[0], e->v[1]);
                    challenge_stats(v, e->name, (int)e->amount);
                }
                break;
            case VE_REMOVED:                         /* out of whichever hand held it */
                for (int k = 0; k < v->nmon; k++)
                    if (e->obj == v->mon[k].held_vehicle)
                        v->mon[k].held_vehicle = -1;
                break;
            case VE_IMPACT: impact_route(v, e->id, e->level, e->pos, e->amount); break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != VE_EFFECT && e->type != VE_EFFECT_STOP)
                printf("  vehicle event %d (%s) from %d\n", e->type, e->name ? e->name : "-", e->obj);
        }
    for (int k = 0; k < v->nmon; k++) {              /* one in a monster's hand (before what follows it) */
        Mon *mo = &v->mon[k];
        Mat4 h;
        if (mo->held_vehicle >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
            vehicle_hold(v->veh, mo->held_vehicle, h.m, mo->mirror, dt);
    }
    for (int k = 0; k < v->nvfx; k++) {              /* the effects that follow their vehicle, while it is there */
        if (v->vfx[k].obj < 0) {
            if (!fx_alive(v->fxw, v->vfx[k].fx))
                v->vfx[k--] = v->vfx[--v->nvfx];
            continue;
        }
        const VehObj *o = vehicle_obj(v->veh, v->vfx[k].obj);
        if (!o || o->phase == VP_GONE) {
            v->vfx[k].obj = -1;                      /* it stays where it is */
            continue;
        }
        float w[16];                                 /* where it is drawn (between its steps) */
        vehicle_draw_world(v->veh, v->vfx[k].obj, w);
        float at[3] = { w[12] + v->vfx[k].off[0], w[13] + v->vfx[k].off[1], w[14] + v->vfx[k].off[2] };
        if (o->beam_fx == v->vfx[k].id)
            fx_place(v->fxw, v->vfx[k].fx, at, o->beam.R);
        else {                                       /* in the car's frame as it turns */
            float R[3][3];
            for (int a = 0; a < 3; a++)
                for (int c = 0; c < 3; c++)
                    R[a][c] = w[4 * a + c];
            fx_place(v->fxw, v->vfx[k].fx, at, (const float (*)[3])R);
        }
    }
}

/* the rail objects' frame (rail.c): what they see of the monster and the buildings, then what they did */
static void rail_step(Viewer *v, float dt, const ArmyMonster *m, int nm)
{
    if (!v->rail)
        return;
    static RailBox rb[64];
    int nb = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
        const Building *b = &v->scene.buildings[i].b;
        memcpy(rb[nb].min, b->min, sizeof rb[nb].min);
        memcpy(rb[nb].max, b->max, sizeof rb[nb].max);
        rb[nb].destroyed = b->state == BUILDING_GONE;
        rb[nb].falling = b->collapse != -1;
    }
    rail_update(v->rail, dt, m, nm, rb, nb);
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        if (mo->pending_slot < 0)
            mo->rail_knock = 0;
        else if (mo->rail_knock == 1)                 /* the frame after the knock: the repeats are dropped */
            mo->rail_knock = 2;
    }
    RailEvent ev[64];
    int ne;
    while ((ne = rail_events(v->rail, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const RailEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            const RailObj *o = e->obj >= 0 ? rail_obj(v->rail, e->obj) : NULL;
            switch (e->type) {
            case RE_EFFECT:
                if (v->fxw) {                        /* one on an object: in its whole frame, the offset in it */
                    float R[3][3], d[3] = { 0, 0, 0 };
                    for (int a = 0; a < 3 && o; a++)
                        for (int k = 0; k < 3; k++)
                            R[a][k] = o->rot[3 * a + k];
                    int id = o ? fx_spawn_frame(v->fxw, e->name, e->pos, (const float (*)[3])R, 1)
                               : fx_spawn(v->fxw, e->name, e->pos, e->dir, 1);
                    if (id >= 0 && v->nrfx < 64) {
                        v->rfx[v->nrfx].id = e->id;
                        v->rfx[v->nrfx].fx = id;
                        v->rfx[v->nrfx].obj = o ? e->obj : -1;
                        for (int k = 0; k < 3 && o; k++)
                            d[k] = e->pos[k] - o->pos[k];
                        for (int a = 0; a < 3; a++)
                            v->rfx[v->nrfx].off[a] = o ? d[0] * R[a][0] + d[1] * R[a][1] + d[2] * R[a][2] : 0;
                        v->nrfx++;
                    }
                }
                break;
            case RE_EFFECT_STOP:
                for (int k = 0; k < v->nrfx; k++)
                    if (v->rfx[k].id == e->id) {
                        fx_stop(v->fxw, v->rfx[k].fx);
                        v->rfx[k] = v->rfx[--v->nrfx];
                        break;
                    }
                break;
            case RE_SOUND: {                         /* (an object's add3DSfx: kept out of range, as obj_sound's) */
                int h = sound_add3d(e->name, e->pos, 0, 1, 1);
                if (h && v->nrsnd < 64) {
                    v->rsnd[v->nrsnd].id = e->id;
                    v->rsnd[v->nrsnd].h = h;
                    v->rsnd[v->nrsnd++].obj = e->obj;
                }
                break;
            }
            case RE_SOUND_STOP:
                for (int k = 0; k < v->nrsnd; k++)
                    if (v->rsnd[k].id == e->id) {
                        sound_kill(v->rsnd[k].h);
                        v->rsnd[k] = v->rsnd[--v->nrsnd];
                        break;
                    }
                break;
            /* a knock-down is asked again every frame until its slot starts (the viewer takes pending_slot at the
             * clip's next event): setReaction (0x2E1AD0) puts the slot's entry in +0x110 each time, the same one, so
             * one is enough [R] */
            case RE_REACTION: if (mo->pending_slot < 0 && set_reaction(mo, e->level)) mo->rail_knock = 1; break;
            case RE_SET_VEL: if (mo->rail_knock < 2) memcpy(mo->vel, e->v, sizeof mo->vel); break;
            case RE_HP: if (mo->hp > 0 && mo->rail_knock < 2) mo->hp = fmaxf(0, fminf(mon_hp_max(mo), mo->hp + e->amount)); break;
            case RE_PUSH: mo->mx += e->v[0]; mo->my += e->v[1]; break;
            case RE_IMPACT: impact_route(v, e->id, e->level, e->pos, e->amount); break;
            case RE_STATS:
                if (e->id == 0) {
                    monster_stats(mo, e->v[0], e->v[1]);
                    challenge_stats(v, e->name, (int)e->amount);
                }
                break;
            case RE_REMOVED:
                for (int k = 0; k < v->nmon; k++)
                    if (e->obj == v->mon[k].held_rail)
                        v->mon[k].held_rail = -1;
                break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE") && e->type != RE_PUSH && e->type != RE_SOUND && e->type != RE_SOUND_STOP)
                printf("  rail event %d (%s) from %d (%s) at %.1f %.1f %.1f\n", e->type, e->name ? e->name : "-", e->obj,
                       o && o->name ? o->name : "-", e->pos[0], e->pos[1], e->pos[2]);
        }
    for (int k = 0; k < v->nmon; k++) {              /* one in a monster's hand (before what follows it) */
        Mon *mo = &v->mon[k];
        Mat4 h;
        if (mo->held_rail >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
            rail_hold(v->rail, mo->held_rail, h.m, mo->mirror, dt);
    }
    for (int k = 0; k < v->nrfx; k++) {              /* the effects that follow their object, while it is there */
        if (v->rfx[k].obj < 0) {
            if (!fx_alive(v->fxw, v->rfx[k].fx))
                v->rfx[k--] = v->rfx[--v->nrfx];
            continue;
        }
        const RailObj *o = rail_obj(v->rail, v->rfx[k].obj);
        if (!o || o->phase == RP_GONE) {
            v->rfx[k].obj = -1;
            continue;
        }
        float R[3][3], at[3], w[16];                 /* [R 0x291ED0, 0x277100] the object's matrix x the offset (as
                                                      * drawn, between its steps) */
        rail_draw_world(v->rail, v->rfx[k].obj, w);
        for (int a = 0; a < 3; a++)
            for (int c = 0; c < 3; c++)
                R[a][c] = w[4 * a + c];
        for (int c = 0; c < 3; c++)
            at[c] = w[12 + c] + v->rfx[k].off[0] * R[0][c] + v->rfx[k].off[1] * R[1][c] + v->rfx[k].off[2] * R[2][c];
        fx_place(v->fxw, v->rfx[k].fx, at, (const float (*)[3])R);
    }
    for (int k = 0; k < v->nrsnd; k++) {             /* the sounds that follow theirs */
        const RailObj *o = rail_obj(v->rail, v->rsnd[k].obj);
        if (o && o->phase != RP_GONE)
            sound_move(v->rsnd[k].h, o->pos);
    }
}

/* an attached effect's frame on a parade object [R 0x291ED0 / 0x277100, as rail.c's]: the object's axes (its world
 * matrix's columns, made 1 long) */
static void parade_fx_frame(const ParadeObj *o, float R[3][3])
{
    for (int a = 0; a < 3; a++) {
        float l = sqrtf(o->world[4 * a] * o->world[4 * a] + o->world[4 * a + 1] * o->world[4 * a + 1] +
                        o->world[4 * a + 2] * o->world[4 * a + 2]);
        for (int c = 0; c < 3; c++)
            R[a][c] = l > 1e-6f ? o->world[4 * a + c] / l : (float)(a == c);
    }
}

/* the balloons', bikers' and food carts' frame (parade.c): what they see of the monster and the camera, then what
 * they did; the one in the monster's hand follows it */
static void parade_step(Viewer *v, float dt, const ArmyMonster *m, int nm)
{
    if (!v->parade)
        return;
    parade_set_world_state(v->parade, v->game ? game_state(v->game) : GS_PLAY);
    parade_update(v->parade, dt, m, m ? nm : 0, v->view.pos[0]);
    ParadeEvent ev[64];
    int ne;
    while ((ne = parade_events(v->parade, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const ParadeEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            switch (e->type) {
            case PAE_EFFECT:
                if (v->fxw) {                        /* attached: in its object's frame, following it while it is there */
                    const ParadeObj *o = e->obj >= 0 ? parade_obj(v->parade, e->obj) : NULL;
                    float R[3][3];
                    if (o)
                        parade_fx_frame(o, R);
                    int id = o ? fx_spawn_frame(v->fxw, e->name, e->pos, (const float (*)[3])R, 1)
                               : fx_spawn(v->fxw, e->name, e->pos, NULL, 1);
                    if (id >= 0 && o && v->npfx < 32) {
                        v->pfx[v->npfx].fx = id;
                        v->pfx[v->npfx].obj = e->obj;
                        v->pfx[v->npfx].marker = 0;
                        for (int k = 0; k < 3; k++)
                            v->pfx[v->npfx].off[k] = e->pos[k] - o->pos[k];
                        v->npfx++;
                    }
                }
                break;
            case PAE_CHALLENGE_FX: {                 /* the ChallengeEffect only on a goal, on the object where it is */
                const ParadeObj *o = parade_obj(v->parade, e->obj);
                if (e->level && v->fxw && o && o->phase != PAP_GONE && challenge_is_goal(&v->challenge, o->name) && v->npfx < 32) {
                    float R[3][3];
                    parade_fx_frame(o, R);
                    int id = fx_spawn_frame(v->fxw, e->name, o->pos, (const float (*)[3])R, 1);
                    if (id >= 0) {
                        v->pfx[v->npfx].fx = id;
                        v->pfx[v->npfx].obj = e->obj;
                        v->pfx[v->npfx].marker = 1;
                        memset(v->pfx[v->npfx].off, 0, sizeof v->pfx[v->npfx].off);
                        v->npfx++;
                    }
                } else if (!e->level)
                    for (int k = 0; k < v->npfx; k++)
                        if (v->pfx[k].obj == e->obj && v->pfx[k].marker) {
                            if (v->fxw)
                                fx_stop(v->fxw, v->pfx[k].fx);
                            v->pfx[k--] = v->pfx[--v->npfx];
                        }
                break;
            }
            case PAE_SOUND: obj_sound(v, OS_PARADE, e->obj, e->name, e->level, e->pos); break;   /* add3DSfx */
            case PAE_MONSTER_SOUND: {                /* monster.add3DSfx(name, 0): on the monster (0x287860) */
                float at[3] = { mo->mx, mo->my, mo->mz };
                obj_sound(v, OS_MONSTER, (int)(mo - v->mon), e->name, 0, at);
                break;
            }
            case PAE_STATS:                          /* gCommon.addStats: the score through the army (its AE_SCORE
                                                      * counts the challenge), HP and energy */
                if (v->army)
                    army_add_stats(v->army, mo->slot, e->tmpl, e->level);
                else {
                    monster_stats(mo, e->v[0], e->v[1]);
                    challenge_stats(v, e->name, (int)e->amount);
                }
                break;
            case PAE_POINTS:                         /* gCommon.addPoints: the category only */
                if (v->game)
                    game_add_points(v->game, mo->slot, e->level, (int)e->amount);
                break;
            case PAE_FART: mon_fart(mo); break;      /* Monsters::timedDoFart -> doFart */
            case PAE_WEAPON:                         /* the biker's bottle: the army's person weapons */
                if (v->army) {
                    const ParadeObj *o = parade_obj(v->parade, e->obj);
                    army_person_weapon(v->army, 1000 + e->obj, e->name && o ? group_sub(o->tmpl, "Weapon|RifleWeapon")
                                       : (Group){ 0 }, o ? o->world : NULL, e->monster >= 0 ? e->monster : 0);
                }
                break;
            case PAE_REMOVED:
                for (int k = 0; k < v->nmon; k++)
                    if (e->obj == v->mon[k].held_parade)
                        v->mon[k].held_parade = -1;
                break;
            case PAE_IMPACT: impact_route(v, e->id, e->level, e->pos, e->amount); break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE"))
                printf("  parade event %d (%s) from %d\n", e->type, e->name ? e->name : "-", e->obj);
        }
    for (int k = 0; k < v->nmon; k++) {              /* one in a monster's hand (before what follows it) */
        Mon *mo = &v->mon[k];
        Mat4 h;
        if (mo->held_parade >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
            parade_hold(v->parade, mo->held_parade, h.m, mo->mirror, dt);
    }
    for (int k = 0; k < v->npfx; k++) {              /* the effects that follow their object, while it is there */
        const ParadeObj *o = parade_obj(v->parade, v->pfx[k].obj);
        if (!o || o->phase == PAP_GONE || !fx_alive(v->fxw, v->pfx[k].fx)) {
            if (v->pfx[k].marker && v->fxw)          /* a ChallengeEffect goes with its actor (0x30DF80) */
                fx_stop(v->fxw, v->pfx[k].fx);
            v->pfx[k--] = v->pfx[--v->npfx];
            continue;
        }
        float at[3] = { o->pos[0] + v->pfx[k].off[0], o->pos[1] + v->pfx[k].off[1], o->pos[2] + v->pfx[k].off[2] }, R[3][3];
        parade_fx_frame(o, R);
        fx_place(v->fxw, v->pfx[k].fx, at, (const float (*)[3])R);
    }
    for (int i = 0, n = parade_count(v->parade); v->army && i < n; i++) {   /* the bikers' weapons go with them */
        const ParadeObj *o = parade_obj(v->parade, i);
        if (o->kind == PAK_BIKER && o->weapon_on)
            army_person_moved(v->army, 1000 + i, o->world, COLLIDE_TAG(COLLIDE_OWN_PARADE, i), o->anim_time, o->anim_len);
    }
}

/* an attached effect's frame on a street object: its axes (x, y, z: the 3 x 3's columns), made unit length */
static void street_fx_frame(const float rot[9], float R[3][3])
{
    for (int a = 0; a < 3; a++) {
        float l = sqrtf(rot[3 * a] * rot[3 * a] + rot[3 * a + 1] * rot[3 * a + 1] + rot[3 * a + 2] * rot[3 * a + 2]);
        for (int k = 0; k < 3; k++)
            R[a][k] = l > 1e-6f ? rot[3 * a + k] / l : (float)(a == k);
    }
}

/* the street and roof objects' frame (streetobj.c): the monster's body against them (TakeDamage, Collision), what
 * they see of it and of the buildings, then what they did; they are drawn where they are now */
static void street_step(Viewer *v, float dt)
{
    if (!v->sobj)
        return;
    static StreetMonster me[MONSTERS];               /* (static: kept for the hits after the update) */
    int nm = have_monster(v) ? v->nmon : 0;
    for (int k = 0; k < nm; k++) {
        Mon *mo = &v->mon[k];
        me[k] = (StreetMonster){ { mo->mx, mo->my, mo->mz }, mo->heading / 57.2957795f,
                                 mo->have_graph ? mo->graph.states[mo->run.state].name : NULL };
        if (mo->monster >= 0 && mo->have_graph) {    /* [R 0x2E22B0] each collision sphere against the shapes */
            float sp[32][4];
            int ns = mon_spheres(mo, sp, 32);
            for (int i = 0; i < ns; i++)
                streetobj_touch(v->sobj, k, &me[k], sp[i], sp[i][3]);
        }
    }
    static StreetBuilding sbs[64];
    int nb = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
        memcpy(sbs[nb].min, v->scene.buildings[i].b.min, sizeof sbs[nb].min);
        memcpy(sbs[nb].max, v->scene.buildings[i].b.max, sizeof sbs[nb].max);
        sbs[nb].falling = v->scene.buildings[i].b.collapse >= 0;
    }
    streetobj_update(v->sobj, dt, nm ? me : NULL, nm, sbs, nb);
    StreetEvent ev[64];
    int ne;
    while ((ne = streetobj_events(v->sobj, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const StreetEvent *e = &ev[i];
            int k = e->obj >= 0 ? v->street_of[e->obj] : -1;
            switch (e->type) {
            case SE_EFFECT:
                if (v->fxw) {
                    /* one on the object: in its whole frame [R 0x277100 -> 0x291ED0: the offset x its matrix], so a
                     * falling lamp's sparks (Sparks.edf's emitters 4 .. 5 up its z) come down with its head */
                    const StreetObj *so = e->level && e->obj >= 0 ? streetobj_get(v->sobj, e->obj) : NULL;
                    float R[3][3];
                    if (so)
                        street_fx_frame(so->rot, R);
                    int id = so ? fx_spawn_frame(v->fxw, e->name, e->pos, (const float (*)[3])R, 1) :
                                  fx_spawn(v->fxw, e->name, e->pos, NULL, 1);
                    if (id >= 0 && v->nsofx < 64) {
                        v->sofx[v->nsofx].id = e->id;
                        v->sofx[v->nsofx].fx = id;
                        v->sofx[v->nsofx].obj = e->level ? e->obj : -1;
                        v->sofx[v->nsofx].node = e->node;
                        v->sofx[v->nsofx].node_id = e->node_id;
                        v->sofx[v->nsofx].mnode[0] = 0;
                        memcpy(v->sofx[v->nsofx].off, e->off, sizeof e->off);
                        v->nsofx++;
                    }
                }
                break;
            case SE_EFFECT_STOP:
                for (int j = 0; j < v->nsofx; j++)
                    if (v->sofx[j].id == e->id) {
                        fx_stop(v->fxw, v->sofx[j].fx);
                        v->sofx[j] = v->sofx[--v->nsofx];
                        break;
                    }
                break;
            case SE_SOUND:                           /* add3DSfx: the object's, or a monster's (m.add3DSfx) */
                if (e->monster >= 0)
                    obj_sound(v, OS_MONSTER, e->monster, e->name, e->level, e->pos);
                else
                    obj_sound(v, OS_STREET, e->obj, e->name, e->level, e->pos);
                break;
            case SE_SHAPE: scene_street_shape(&v->scene, k, e->name); break;
            case SE_IMPACT: impact_route(v, e->id, e->level, e->pos, e->amount); break;   /* a thrown car met it */
            case SE_MONSTER_FACING:                  /* the MerryGoRound's rider: queueFacingZRequest (0x2E97D0) */
            case SE_MONSTER_PUSH: {                  /* queuePushRequest (0x2E9860): taken [R 0x2DEB04] */
                Mon *mo = ev_mon(v, e->monster);
                if (e->type == SE_MONSTER_FACING)
                    mo->heading = e->amount * 57.2957795f;
                else {
                    mo->mx = e->pos[0]; mo->my = e->pos[1]; mo->mz = e->pos[2];
                }
                foot_step(&mo->feet, 0, 0);          /* 0x33BE90: both feet let go */
                foot_step(&mo->feet, 1, 0);
                mo->feet.anchor = -1;
                break;
            }
            case SE_MONSTER_VEL: memcpy(ev_mon(v, e->monster)->vel, e->v, sizeof e->v); break;   /* setVel(v, 0) */
            case SE_MONSTER_REACTION:                /* playReaction, while alive */
                if (ev_mon(v, e->monster)->hp > 0)
                    force_state(ev_mon(v, e->monster), e->name);
                break;
            case SE_STATS:                           /* gCommon.addStats(obj, monster, 0) */
                if (v->army)
                    army_add_stats(v->army, e->monster, e->tmpl, 0);
                else
                    challenge_stats(v, e->name, (int)e->amount);
                break;
            case SE_MONSTER_EFFECT:                  /* on the monster's node: fxNode_Belch, fxNode_Shoulder1 .. */
                for (int n = 1; v->fxw && n <= (e->level ? 20 : 1); n++) {
                    char node[32];
                    Mat4 m;
                    if (e->level) snprintf(node, sizeof node, "%s%d", e->node, n);
                    else snprintf(node, sizeof node, "%s", e->node);
                    int mk = ev_mon(v, e->monster)->slot;
                    if (scene_monster_node(&v->scene, mk, node, &m) != 0)
                        break;
                    int id = fx_spawn(v->fxw, e->name, m.m + 12, NULL, 1);
                    if (id >= 0 && v->nsofx < 64) {
                        v->sofx[v->nsofx].id = 0;
                        v->sofx[v->nsofx].mon = mk;
                        v->sofx[v->nsofx].fx = id;
                        v->sofx[v->nsofx].obj = -1;
                        v->sofx[v->nsofx].node = NULL;
                        snprintf(v->sofx[v->nsofx].mnode, sizeof v->sofx[v->nsofx].mnode, "%s", node);
                        v->nsofx++;
                    }
                }
                break;
            default: break;
            }
            if (getenv("RAMPAGE_TRACE"))
                printf("  street event %d (%s) from %d\n", e->type, e->name ? e->name : "-", e->obj);
        }
    for (int k = 0; k < v->nmon; k++) {              /* a parked car in a monster's hand (before it is drawn and
                                                      * what follows it) */
        Mon *mo = &v->mon[k];
        Mat4 h;
        if (mo->held_sobj >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
            streetobj_hold(v->sobj, mo->held_sobj, h.m, mo->mirror, dt);
    }
    for (int i = 0, n = streetobj_count(v->sobj); i < n; i++) {   /* drawn where they are now */
        const StreetObj *o = streetobj_get(v->sobj, i);
        float w[16];
        if (o->moved) {                              /* (between its steps) */
            streetobj_draw_world(v->sobj, i, w);
            scene_street_place(&v->scene, v->street_of[i], w, !o->alive);
        }
    }
    for (int j = 0; j < v->nsofx; j++) {             /* the effects that follow, turning with what they follow */
        float at[3], R[3][3];
        int framed = 0;
        Mat4 m;
        if (v->sofx[j].mnode[0]) {
            if (scene_monster_node(&v->scene, v->sofx[j].mon, v->sofx[j].mnode, &m) != 0 || !fx_alive(v->fxw, v->sofx[j].fx)) {
                v->sofx[j--] = v->sofx[--v->nsofx];
                continue;
            }
            memcpy(at, m.m + 12, sizeof at);
        } else if (v->sofx[j].obj >= 0) {
            const StreetObj *o = streetobj_get(v->sobj, v->sofx[j].obj);
            int k = v->street_of[v->sofx[j].obj];
            if (!o || !o->alive) {
                v->sofx[j].obj = -1;                 /* it stays where it is */
                continue;
            }
            if ((v->sofx[j].node || v->sofx[j].node_id > 0) &&
                scene_instance_node(&v->scene, v->scene.street[k].model, v->sofx[j].node, v->sofx[j].node_id, m.m) == 0) {
                for (int r = 0; r < 3; r++)
                    at[r] = m.m[12 + r] + m.m[r] * v->sofx[j].off[0] + m.m[4 + r] * v->sofx[j].off[1] +
                            m.m[8 + r] * v->sofx[j].off[2];
                float nr[9] = { m.m[0], m.m[1], m.m[2], m.m[4], m.m[5], m.m[6], m.m[8], m.m[9], m.m[10] };
                street_fx_frame(nr, R);
            } else {
                streetobj_point(v->sobj, v->sofx[j].obj, v->sofx[j].off, at);
                street_fx_frame(o->rot, R);
            }
            framed = 1;
        } else {
            if (!fx_alive(v->fxw, v->sofx[j].fx))
                v->sofx[j--] = v->sofx[--v->nsofx];
            continue;
        }
        if (framed)
            fx_place(v->fxw, v->sofx[j].fx, at, (const float (*)[3])R);
        else
            fx_move(v->fxw, v->sofx[j].fx, at);
    }
}

/* what the modules see of the monsters this frame (army.h's ArmyMonster, index = the monster's slot): their places,
 * moves and collision spheres, which the frame's world follows too */
static int army_monsters(Viewer *v, ArmyMonster *m)
{
    static float spheres[MONSTERS][64][4];
    int nm = 0;
    for (int k = 0; k < v->nmon; k++, nm++) {
        Mon *mo = &v->mon[k];
        memset(&m[k], 0, sizeof m[k]);
        if (!mo->have_graph || !mon_in(mo))
            return k;                                /* (a monster without its graph: the ones before it) */
        int ns = monster_spheres(mo, spheres[k], 64);   /* every collision sphere, in the world */
        world_monster_moved(mo, (const float (*)[4])spheres[k], ns);
        m[k].pos[0] = mo->mx; m[k].pos[1] = mo->my; m[k].pos[2] = mo->mz;
        Mat4 sp;                                     /* the weapons' target: Spine1's x and y (0x2E6000) */
        if (scene_monster_node(&v->scene, mo->slot, "Bip01 Spine1", &sp) == 0) {
            m[k].target[0] = sp.m[12];
            m[k].target[1] = sp.m[13];
        } else {
            m[k].target[0] = mo->mx;
            m[k].target[1] = mo->my;
        }
        m[k].target[2] = mo->mz + 6;
        m[k].heading = mo->heading / 57.2957795f;
        m[k].move = mo->graph.states[mo->run.state].name;
        m[k].mode = mo->mode;
        m[k].spheres = (const float (*)[4])spheres[k];
        m[k].nspheres = ns;
        float hw = mo->graph.box[0] > 0 ? mo->graph.box[0] : 3, hh = mo->graph.box[1] > 0 ? mo->graph.box[1] : 4.25f;
        m[k].box_min[0] = mo->mx - hw; m[k].box_min[1] = mo->my - hw; m[k].box_min[2] = mo->mz;
        m[k].box_max[0] = mo->mx + hw; m[k].box_max[1] = mo->my + hw; m[k].box_max[2] = mo->mz + 2 * hh;
        v->scene.mon[k].move = m[k].move;
    }
    return nm;
}

/* an effect on a unit: its axes (the unit's matrix, or its model node's as drawn: node >= 0, by number) and where
 * the offset in that frame puts it */
static void frame_of(const Mat4 *m, float pos[3], float R[3][3]);
static void unit_fx_frame(Viewer *v, int unit, const ArmyUnit *u, int node, const float off[3], float R[3][3],
                          float at[3])
{
    Mat4 n;
    float o[3];
    int found = node >= 0 && scene_unit_node_at(&v->scene, unit, NULL, node, n.m) == 0;
    if (getenv("RAMPAGE_NODES") && node >= 0)
        printf("  %s effect node %d: %s\n", u->name ? u->name : "?", node, found ? "found" : "not found");
    if (found)
        frame_of(&n, o, R);
    else {
        for (int a = 0; a < 3; a++)
            for (int c = 0; c < 3; c++)
                R[a][c] = u->world[4 * a + c];
        memcpy(o, u->pos, sizeof o);
    }
    for (int c = 0; c < 3; c++)
        at[c] = o[c] + off[0] * R[0][c] + off[1] * R[1][c] + off[2] * R[2][c];
}

static void army_step(Viewer *v, float dt)
{
    if (!v->army || !have_monster(v) || !v->mon[0].have_graph || !mon_in(&v->mon[0]))
        return;
    static ArmyMonster m[MONSTERS];                  /* (static: the army's, vehicles', rail's and parade's hits read
                                                      * it after the frame's update) */
    int nm = army_monsters(v, m);
    static ArmyBox boxes[64];
    int nb = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
        const Building *b = &v->scene.buildings[i].b;
        memcpy(boxes[nb].min, b->min, sizeof boxes[nb].min);
        memcpy(boxes[nb].max, b->max, sizeof boxes[nb].max);
        boxes[nb].width = b->width;
        boxes[nb].depth = b->depth;
        boxes[nb].storeys = b->n;
        boxes[nb].gone = b->state == BUILDING_GONE;
        boxes[nb].falling = b->state >= BUILDING_CRACKING;
    }
    army_set_view(v->army, v->view.pos[0] - 45, v->view.pos[0] + 45);   /* the camera's x (FlybySound), and the x range unhosted */
    army_set_weapons(v->army, game_weapons_on(v->game));   /* the army's and the bosses' weapons (gp-0x772C) */
    army_airstrike(v->army, v->force_strike || (v->game && game_time_up(v->game)));   /* the time-up airstrike
                                                      * (gAIMgr 0x3320F0) */
    army_ai_cut(v->army, vehicle_ai_cut(v->veh));     /* (the traffic's release ends gAIMgr's update) */
    army_update(v->army, dt, m, nm, boxes, nb);
    vehicle_step(v, dt, m, nm, boxes, nb);
    parade_step(v, dt, m, nm);
    rail_step(v, dt, m, nm);
    edible_step(v, dt);
    boss_step(v, dt, m, nm);
    game_step(v, dt);
    ArmyEvent ev[64];
    int ne;
    while ((ne = army_events(v->army, ev, 64)) > 0)
        for (int i = 0; i < ne; i++) {
            const ArmyEvent *e = &ev[i];
            Mon *mo = ev_mon(v, e->monster);
            switch (e->type) {
            case AE_EFFECT:
                if (e->level == 1 && !(e->unit >= 0 && army_unit(v->army, e->unit) &&   /* a ChallengeEffect only on
                                                      * a goal (as the people's PE_FX_ON) */
                                       challenge_is_goal(&v->challenge, army_unit(v->army, e->unit)->name)))
                    break;
                if (v->fxw) {
                    const ArmyShot *bs = NULL;       /* a beam's look: in the weapon's firing frame (0x2975A4) */
                    for (int j = 0, n = e->shot >= 0 ? army_shots(v->army) : 0; j < n && !bs; j++)
                        if (army_shot(v->army, j)->id == e->shot && army_shot(v->army, j)->beam)
                            bs = army_shot(v->army, j);
                    const ArmyUnit *on = e->unit >= 0 && e->shot < 0 ? army_unit(v->army, e->unit) : NULL;
                    float R[3][3], at[3];            /* one on a unit: in its whole frame, the offset in it [R 0x27A990
                                                      * -> 0x291ED0, 0x291440: the offset x the unit's matrix] */
                    if (on)
                        unit_fx_frame(v, e->unit, on, e->node, e->v, R, at);
                    int id = bs ? fx_spawn_frame(v->fxw, e->name, e->pos, bs->R, 1) :
                             on ? fx_spawn_frame(v->fxw, e->name, at, (const float (*)[3])R, 1) :
                                  fx_spawn(v->fxw, e->name, e->pos, e->dir, 1);
                    if (id >= 0 && (e->unit >= 0 || e->shot >= 0) && v->nfxa < 128) {
                        v->fxa[v->nfxa].id = e->id;
                        v->fxa[v->nfxa].fx = id;
                        v->fxa[v->nfxa].unit = e->shot >= 0 ? -1 : e->unit;
                        v->fxa[v->nfxa].shot = e->shot;
                        v->fxa[v->nfxa].node = e->node;
                        v->fxa[v->nfxa].die_with = e->die_with;
                        FxRod rod;
                        int has_rod = bs && fx_rod(v->fxlib, e->name, &rod);
                        v->fxa[v->nfxa].head = 0;
                        v->fxa[v->nfxa].speed = has_rod ? rod.speed : 0;
                        v->fxa[v->nfxa].len = has_rod ? rod.length : 0;
                        const ArmyUnit *u = e->unit >= 0 ? army_unit(v->army, e->unit) : NULL;
                        for (int k = 0; k < 3; k++)
                            v->fxa[v->nfxa].off[k] = u && e->shot < 0 ? e->v[k] : 0;   /* (army.c's offset) */
                        v->nfxa++;
                    } else if (id >= 0 && v->nfxa < 128) {   /* a handle to stop it by */
                        v->fxa[v->nfxa].id = e->id;
                        v->fxa[v->nfxa].fx = id;
                        v->fxa[v->nfxa].unit = v->fxa[v->nfxa].shot = -1;
                        v->nfxa++;
                    }
                }
                break;
            case AE_EFFECT_MOVE:
                for (int k = 0; v->fxw && k < v->nfxa; k++)
                    if (v->fxa[k].id == e->id) {
                        fx_move(v->fxw, v->fxa[k].fx, e->pos);
                        break;
                    }
                break;
            case AE_EFFECT_STOP:
                for (int k = 0; k < v->nfxa; k++)
                    if (v->fxa[k].id == e->id) {
                        fx_stop(v->fxw, v->fxa[k].fx);
                        v->fxa[k] = v->fxa[--v->nfxa];
                        break;
                    }
                break;
            case AE_SOUND:                           /* a unit's add3DSfx (level 1) / remove3DSfx (0); 2: a 2D one */
                if (e->level == 2)
                    sound_play(e->name, NULL, 1);
                else if (e->unit >= 0 && e->level == 0)
                    obj_sound_stop(v, OS_ARMY, e->unit, e->name);
                else if (e->unit >= 0)               /* (amount: the script's keep) */
                    obj_sound(v, OS_ARMY, e->unit, e->name, e->amount != 0, e->pos);
                else if (e->level == 0)
                    sound_kill_name(e->name);
                else
                    sound_play(e->name, e->pos, 1);
                if (getenv("RAMPAGE_TRACE"))
                    printf("  army sound %s%s\n", e->name ? e->name : "(all the unit's)", e->level ? "" : " (stopped)");
                break;
            case AE_HIT:                             /* ReactionLevel < 0: through TakeDamage */
                monster_hit(mo, e->name, e->amount, e->level >= 0 ? e->level : take_damage_level(mo, e->amount), e->v,
                            e->pos);
                break;
            case AE_REACTION: set_reaction(mo, e->level); break;
            case AE_FART: mon_fart_later(mo, e->amount); break;
            case AE_PUSH:
                if (getenv("RAMPAGE_TRACE"))
                    printf("  army push: %.2f %.2f by %.2f %.2f\n", mo->mx, mo->my, e->v[0], e->v[1]);
                mo->mx += e->v[0]; mo->my += e->v[1];
                break;
            case AE_IMPACT: impact_route(v, e->id, e->level, e->pos, e->amount); break;
            case AE_STATS_HEALTH: mo->hp = mo->hp > 0 ? fmaxf(1, fminf(mon_hp_max(mo), mo->hp + e->amount)) : mo->hp; break;
            case AE_STATS_ENERGY: monster_stats(mo, 0, e->amount); break;
            case AE_FLOAT:                           /* a PointsBanner: 0.8 for the army's awards (0x2BAB2C) */
                if (v->hud)
                    hud_float(v->hud, (int)e->amount, e->pos, e->level, 0.8f);
                break;
            case AE_WINDOW_CLOSE: scene_window_close_at(&v->scene, e->pos); break;
            case AE_OBJECT_HIT: {                    /* a shot's or beam's TakeDamage on another module's object */
                int owner = COLLIDE_TAG_OWNER(e->id), idx = COLLIDE_TAG_INDEX(e->id);
                if (owner == COLLIDE_OWN_VEHICLE && v->veh)
                    vehicle_shot(v->veh, idx, e->dir);
                else if (owner == COLLIDE_OWN_STREET && v->sobj)
                    streetobj_shot(v->sobj, idx, e->dir);
                break;
            }
            case AE_BUILDING_HIT:                    /* a shot's or beam's Building::hit (0x2BC150), no one's */
                if ((uint32_t)e->id < v->scene.nbuildings &&
                    v->scene.buildings[e->id].b.state == BUILDING_STANDING) {
                    SceneBuilding *sb = &v->scene.buildings[e->id];
                    sb->hitter = -1;
                    if (e->level == 8)               /* Nuclear (the airstrike's bomb): it falls at once */
                        building_destroy(&sb->b);
                    else
                        building_hit(&sb->b, e->pos, e->amount);
                    if (getenv("RAMPAGE_TRACE"))
                        printf("  building %d %s by %s (%.1f)\n", e->id, e->level == 8 ? "nuked" : "hit",
                               e->name ? e->name : "?", e->amount);
                }
                break;
            case AE_SCORE:
                if (v->game)
                    game_add_points(v->game, mo->slot, e->level, (int)e->amount);
                if (e->name)                         /* the object's template counts for the challenge (0x346780) */
                    challenge_stats(v, e->name, (int)e->amount);
                if (getenv("RAMPAGE_TRACE"))
                    printf(mo->slot ? "  score %+.0f (category %d, %s), total %.0f, monster %d\n" :
                           "  score %+.0f (category %d, %s), total %.0f\n", e->amount, e->level, e->name ? e->name : "-",
                           army_stats(v->army, mo->slot)->points[6], mo->slot);
                break;
            default: break;
            }
        }
    for (int k = 0; k < v->nmon; k++) {              /* a unit in a monster's hand (before what follows it) */
        Mon *mo = &v->mon[k];
        Mat4 h;
        if (mo->held_unit >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
            army_hold(v->army, mo->held_unit, h.m, mo->mirror, dt);
    }
    for (int k = 0; k < v->nfxa; k++) {              /* the effects that follow their unit or shot */
        float at[3];
        if (v->fxa[k].shot >= 0) {
            const ArmyShot *sh = NULL;
            for (int j = 0, n = army_shots(v->army); j < n && !sh; j++)
                if (army_shot(v->army, j)->id == v->fxa[k].shot && army_shot(v->army, j)->alive)
                    sh = army_shot(v->army, j);
            if (!sh) {
                fx_stop(v->fxw, v->fxa[k].fx);
                v->fxa[k--] = v->fxa[--v->nfxa];
                continue;
            }
            if (sh->beam) {                          /* [R 0x2975A4] placed in the firing frame each frame */
                fx_place(v->fxw, v->fxa[k].fx, sh->pos, sh->R);
                /* a hit this frame (the stream's head short of its growth) is the effect's hit too: t from the tail
                 * (0x295938, before the stream's own cut 0x226DF0) */
                float grown = fminf(v->fxa[k].head + v->fxa[k].speed * dt, v->fxa[k].len);
                if (v->fxa[k].len > 0 && sh->head > 0 && sh->head < grown - 1e-3f)
                    fx_cut(v->fxw, v->fxa[k].fx, sh->head - sh->tail);
                v->fxa[k].head = sh->head;
                continue;
            }
            float sv[3] = { sh->dir[0] * sh->speed, sh->dir[1] * sh->speed, sh->dir[2] * sh->speed };
            fx_vel(v->fxw, v->fxa[k].fx, sv);        /* the shot's velocity is its look's [R 0x293B80, 0x294D24] */
            memcpy(at, sh->pos, sizeof at);
        } else if (v->fxa[k].unit >= 0) {
            const ArmyUnit *u = army_unit(v->army, v->fxa[k].unit);
            if (!u || u->phase == AU_GONE) {         /* its unit gone: a turn-off one stops, the others run
                                                      * on where they were (as the other modules' FollowEffects) */
                v->fxa[k].unit = -1;
                if (v->fxa[k].die_with) {
                    fx_stop(v->fxw, v->fxa[k].fx);
                    v->fxa[k--] = v->fxa[--v->nfxa];
                }
                continue;
            }
            float R[3][3];
            unit_fx_frame(v, v->fxa[k].unit, u, v->fxa[k].node, v->fxa[k].off, R, at);
            fx_place(v->fxw, v->fxa[k].fx, at, (const float (*)[3])R);
            continue;
        } else {
            if (!fx_alive(v->fxw, v->fxa[k].fx))
                v->fxa[k--] = v->fxa[--v->nfxa];
            continue;
        }
        fx_move(v->fxw, v->fxa[k].fx, at);
    }
}

/* Weather.bsf (an InvisShape in the block: rain, fog, wind) [R]: onInit starts its Effect on its node 1, kept on
 * it, not turned off when it dies; Process puts the object at the camera's place, 2 more in y, every frame */
static void weather_step(Viewer *v)
{
    if (!v->fxw)
        return;
    float at[3] = { v->view.pos[0], v->view.pos[1] + 2.0f, v->view.pos[2] };
    if (v->weather_due) {
        v->weather_due = 0;
        for (int k = 0; k < v->scene.nweather && v->nweather_fx < 4; k++) {
            int id = fx_spawn(v->fxw, v->scene.weather[k], at, NULL, 1);
            fx_set_of_block(v->fxw, id, 0);          /* (the game's goes at a block's end and the next block's starts
                                                      * at once, 0x302AF0; here once the new block is built) */
            if (id >= 0)
                v->weather_fx[v->nweather_fx++] = id;
            if (getenv("RAMPAGE_TRACE"))
                printf("  weather %s -> %d\n", v->scene.weather[k], id);
        }
    }
    for (int k = 0; k < v->nweather_fx; k++)
        fx_move(v->fxw, v->weather_fx[k], at);
}

/* the effects the scene and the people asked for, started; the world's particles moved on */
static void fx_step(Viewer *v, float dt)
{
    weather_step(v);
    for (int i = 0; i < v->scene.nfxreq; i++)
        if (v->fxw) {
            const struct SceneFx *f = &v->scene.fxreq[i];
            int id = fx_spawn(v->fxw, f->name, f->pos, f->dir, 1);
            if (getenv("RAMPAGE_TRACE"))
                printf("  effect %s at %.1f %.1f %.1f -> %d\n", f->name, f->pos[0], f->pos[1], f->pos[2], id);
            scene_fx_started(&v->scene, v->fxw, f, id);
        }
    v->scene.nfxreq = 0;
    if (v->fxw)                                      /* the buildings' spawners: their cells' debris on their storeys */
        scene_fx_follow(&v->scene, v->fxw);
    for (int k = 0; k < v->nmon; k++)                /* the timedDoFarts due */
        for (int j = 0; j < 4; j++)
            if (v->mon[k].fart_in[j] > 0 && (v->mon[k].fart_in[j] -= dt) <= 0) {
                v->mon[k].fart_in[j] = 0;
                mon_fart(&v->mon[k]);
            }
    for (int k = 0; k < v->nmon; k++)                /* the monsters' effects on their nodes */
        mon_fx_follow(&v->mon[k]);
    if (v->fxw)
        fx_update(v->fxw, dt, 0);
}

static int ends_with_i(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && _stricmp(s + a - b, tail) == 0;
}

/* an attachment node of MONSTER_DATA (+0xA84) in the world as drawn: its bone's place x its matrix (rows, then the
 * place); mirrored, the other side's node on the other side's bone, reflected; 0 = ok */
static int attach_world(Mon *mo, const char *name, Mat4 *out)
{
    if (!mo->have_graph || !mon_in(mo))
        return -1;
    const Instance *in = mon_in(mo);
    const struct MoveAttach *a = NULL;
    for (int i = 0; i < mo->graph.nattach && !a; i++)
        if (mo->graph.attach[i].name && ends_with_i(mo->graph.attach[i].name, name))
            a = &mo->graph.attach[i];
    if (!a || a->bone < 0 || (uint32_t)a->bone >= in->tree.n)
        return -1;
    if (mo->mirror) {                                 /* the partner bone's own attachment */
        int mb = in->tree.nodes[a->bone].mirror;
        for (int i = 0; i < mo->graph.nattach; i++)
            if (mo->graph.attach[i].bone == mb && mb != a->bone)
                a = &mo->graph.attach[i];
    }
    Mat4 t = mat_identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t.m[4 * i + j] = a->m[3 * i + j];
    for (int j = 0; j < 3; j++)
        t.m[12 + j] = a->m[9 + j];
    Mat4 m = mat_mul(in->node[a->bone], t);
    if (mo->mirror)
        for (int c = 0; c < 4; c++) {                /* S m S */
            m.m[4 * c] = -m.m[4 * c];
            m.m[c] = -m.m[c];
        }
    *out = mat_mul(in->base, m);
    return 0;
}

/* a node of the monster by name in the world as drawn: an attachment node of MONSTER_DATA's (fxNode_Fart,
 * fxNode_Belch), else a bone of its skeleton (Nick's FxFire, Bip01 Head); 0 = ok */
static int mon_node_world(Mon *mo, const char *name, Mat4 *out)
{
    return attach_world(mo, name, out) == 0 ? 0 : scene_monster_node(&mo->v->scene, mo->slot, name, out);
}

/* a world matrix as an effect's frame: its place, its axes made 1 long */
static void frame_of(const Mat4 *m, float pos[3], float R[3][3])
{
    for (int a = 0; a < 3; a++) {
        float l = sqrtf(m->m[4 * a] * m->m[4 * a] + m->m[4 * a + 1] * m->m[4 * a + 1] + m->m[4 * a + 2] * m->m[4 * a + 2]);
        for (int c = 0; c < 3; c++)
            R[a][c] = l > 1e-6f ? m->m[4 * a + c] / l : (float)(a == c);
    }
    pos[0] = m->m[12]; pos[1] = m->m[13]; pos[2] = m->m[14];
}

/* effect slot node `node`'s frame in the world (0x2F5800's start; then the FollowEffect's follow, 0x291440: the
 * offset x the object's node, vtable +0x4C): the node's matrix on its bone as the game poses it (a mirrored
 * monster's bone is the other side's node reflected, as scene_monster_node gives it), its place x the monster's
 * scale (+0xA78 when +0xA74, 0x2F58A4; the port's base matrix is scaled); R its axes made 1 long; 0 = ok */
static int mon_fx_frame(Mon *mo, int node, float pos[3], float R[3][3])
{
    const Instance *in = mon_in(mo);
    if (!in || !mo->have_graph || node < 0 || node >= mo->graph.nfxnode)
        return -1;
    const struct MoveFxNode *n = &mo->graph.fxnode[node];
    Mat4 w;
    if (n->bone < 0 || (uint32_t)n->bone >= in->tree.n ||
        scene_monster_node(&mo->v->scene, mo->slot, in->tree.nodes[n->bone].name, &w) != 0)
        return -1;
    Mat4 t = mat_identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            t.m[4 * i + j] = n->m[3 * i + j];
    for (int j = 0; j < 3; j++)
        t.m[12 + j] = n->m[9 + j];
    Mat4 m = mat_mul(w, t);
    frame_of(&m, pos, R);
    return 0;
}

/* clip event 12 (0x2E7E90 -> 0x2F5800, on the record +0x7DFC) [R]: effect `name` of MONSTER_DATA's list on in slot
 * k, or off. Either way what the slot holds, if it still lives (the FollowEffect's serial +0x4 is the slot's), shuts
 * down (its effect's vtable +0x30, 0x1F1C60 -> 0x1F2410: each emitter's 0x1F7CD0; the live particles finish) and the
 * slot is emptied. On: the new effect starts there (0x292010 -> 0x292050 -> 0x291A20: a FollowEffect on the
 * monster, the bone and offset of the slot's node, 0x2DACA0, the other side's while mirrored, kept for its life).
 * A state posted to start shuts them all down too (let_go, 0x2DA570 -> 0x2F56E0: a reaction, a landing, a knock-off,
 * the reset); no clip's own start, death or respawn does: a one-shot ends by itself, a looping one (Charged_Punch,
 * Roar_Weapon) until its slot's next event or such a state, past its clip if that is cut short.
 * Not ported: the started FollowEffect's +0x4C |= 0x80 (0x2F5A28) [G: no code tests +0x4C against 0x80 and no
 * query's mask found holds it (the shots' 4, 0x2000, 0x2040: 0x2953E0, 0x33B770); only a mask from a script could] */
static void mon_fx_event(Mon *mo, const MoveEvent *e)
{
    Viewer *v = mo->v;
    int k = (int)(int32_t)e->value;
    if (k < 0 || k >= MON_FX_SLOTS || !v->fxw)
        return;
    if (mo->fx_used >> k & 1) {
        int live = fx_alive(v->fxw, mo->fx_slot[k]);
        if (live)
            fx_stop(v->fxw, mo->fx_slot[k]);
        mo->fx_used &= ~(1u << k);
        if (getenv("RAMPAGE_TRACE"))
            printf("  effect slot %d: %d %s\n", k, mo->fx_slot[k], live ? "shut down" : "already over");
    }
    if (e->p[0] == 0 || !e->name)
        return;
    const struct MoveFxNode *n = moves_fx_node(&mo->graph, k, mo->mirror);
    int node = n ? (int)(n - mo->graph.fxnode) : -1, id = -1;
    float pos[3], R[3][3];
    if (mon_fx_frame(mo, node, pos, R) == 0 && (id = fx_spawn_frame(v->fxw, e->name, pos, R, 1)) >= 0) {
        mo->fx_slot[k] = id;
        mo->fx_node[k] = node;
        mo->fx_used |= 1u << k;
    }
    if (getenv("RAMPAGE_TRACE")) {
        int b = n && mon_in(mo) && n->bone >= 0 && (uint32_t)n->bone < mon_in(mo)->tree.n ? n->bone : -1;
        printf("  effect %s in slot %d (node %d, %s) at %.1f %.1f %.1f -> %d\n", e->name, k, node,
               b >= 0 ? mon_in(mo)->tree.nodes[b].name : "-", id >= 0 ? pos[0] : 0, id >= 0 ? pos[1] : 0,
               id >= 0 ? pos[2] : 0, id);
    }
}

/* each frame: the slots' effects follow their nodes (the FollowEffect's update, 0x290F60 -> 0x291440); a slot whose
 * effect has ended is free again (its serial no longer matches, 0x2F5830) */
static void mon_fx_follow(Mon *mo)
{
    Viewer *v = mo->v;
    for (int k = 0; k < MON_FX_SLOTS && v->fxw; k++) {
        if (!(mo->fx_used >> k & 1))
            continue;
        float pos[3], R[3][3];
        if (!fx_alive(v->fxw, mo->fx_slot[k]))
            mo->fx_used &= ~(1u << k);
        else if (mon_fx_frame(mo, mo->fx_node[k], pos, R) == 0)
            fx_place(v->fxw, mo->fx_slot[k], pos, (const float (*)[3])R);
    }
    float at[3];
    if (mo->rage_on && v->fxw) {                     /* the rage's, at the head on the camera's side */
        if (!fx_alive(v->fxw, mo->rage_fx))
            mo->rage_on = 0;
        else if (rage_fx_at(mo, at) == 0)
            fx_move(v->fxw, mo->rage_fx, at);
    }
    mon_nick_fx(mo);
    for (int k = 0; k < mo->nfx_named && v->fxw; k++) {
        float pos[3], R[3][3];
        Mat4 w;
        if (!fx_alive(v->fxw, mo->fx_named[k].fx)) {
            mo->fx_named[k] = mo->fx_named[--mo->nfx_named];
            k--;
        } else if (mon_node_world(mo, mo->fx_named[k].node, &w) == 0) {
            frame_of(&w, pos, R);
            fx_place(v->fxw, mo->fx_named[k].fx, pos, (const float (*)[3])R);
        }
    }
}

/* FollowEffect.createAndStartAttachedToNodeByName(monster, name, node) (Monsters.bsf): the effect on that node of
 * the monster's, in its frame, kept there while it lives (mon_fx_follow); its id, -1 when not started (no such
 * node, outside the effects budget) */
static int mon_fx_named(Mon *mo, const char *name, const char *node)
{
    Viewer *v = mo->v;
    float pos[3], R[3][3];
    Mat4 w;
    int id = -1;
    if (v->fxw && mon_node_world(mo, node, &w) == 0) {
        frame_of(&w, pos, R);
        id = fx_spawn_frame(v->fxw, name, pos, (const float (*)[3])R, 1);
    }
    if (id >= 0 && mo->nfx_named < MON_FX_NAMED) {
        mo->fx_named[mo->nfx_named].fx = id;
        snprintf(mo->fx_named[mo->nfx_named++].node, sizeof mo->fx_named[0].node, "%s", node);
    }
    if (getenv("RAMPAGE_TRACE"))
        printf("  effect %s on %s -> %d\n", name, node, id);
    return id;
}

/* Monsters::doFart (timedDoFart's, an eaten Beans' MonsterFunction) [R Monsters.bsf]: the smile, the Fart effect
 * on fxNode_Fart, a hop of 20 */
static void mon_fart(Mon *mo)
{
    face_play(mo, "Expression_Smile", 0, 0, 1);
    mon_fx_named(mo, "Fart", "fxNode_Fart");
    mo->vel[2] += 20;                                /* addVel((0, 0, 20), 1) */
}

/* the rage's effect's place (0x2F4710, each frame 0x2F5170) [R]: the head (0x2E63A0: the animation player's node
 * +0x72D0, Bip01 Head, through the monster's matrix) moved by the monster's radius (0x2E4FA0: MONSTER_DATA +0x188,
 * 3, x its scale) along the camera's forward (gWorld +0x66C, made 1 long; (1, 0, 0) when shorter than 1e-6): the
 * world's view block +0x660 is the camera's rows right, forward, up and its eye (+0x684), copied from the frame's
 * (0x285030); the renderer swaps rows 1 and 2 into its depth axis (0x2895F0 -> 0x101E90) [R]. So the effect sits a
 * radius beyond the head, away from the camera; 0 = ok */
static int rage_fx_at(Mon *mo, float at[3])
{
    Viewer *v = mo->v;
    Mat4 w;
    if (scene_monster_node(&v->scene, mo->slot, "Bip01 Head", &w) != 0)
        return -1;
    float d[3] = { v->view.rows[1][0], v->view.rows[1][1], v->view.rows[1][2] };
    float l2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], r = mon_radius(mo);
    if (l2 < 1e-6f) {
        d[0] = 1; d[1] = d[2] = 0;
    } else {
        float l = sqrtf(l2);
        for (int k = 0; k < 3; k++)
            d[k] /= l;
    }
    for (int k = 0; k < 3; k++)
        at[k] = w.m[12 + k] + d[k] * r;
    return 0;
}

/* the rage's effect, on (startRage 0x2D4360 -> 0x2F4710) or off (stopRage 0x2D4340 -> 0x2F46D0, and the update's
 * 0x2DE96C / 0x2DE9B0) [R]: the book's RageFx (Rage.edf: Rage_enter, Rage_loop and camshake_collapse) in the world's
 * frame, started outside the effects budget and only when none is on; addStats' restart stops the old one first. Off,
 * it is handed straight back to the FxManager (0x207A40, no shutdown first): gone at once, its particles too */
static void mon_rage_fx(Mon *mo, int on)
{
    Viewer *v = mo->v;
    if (mo->rage_on && v->fxw)
        fx_kill(v->fxw, mo->rage_fx);
    mo->rage_on = 0;
    float at[3];
    if (on && v->fxw && mo->rage_fx_name[0] && rage_fx_at(mo, at) == 0 &&
        (mo->rage_fx = fx_spawn_free(v->fxw, mo->rage_fx_name, at, NULL, 1)) >= 0)
        mo->rage_on = 1;
    if (getenv("RAMPAGE_TRACE") && on)
        printf("  rage effect %s -> %d\n", mo->rage_fx_name, mo->rage_on ? mo->rage_fx : -1);
}

/* Nick's own (0x2F5D80 for a monster named Nick, each frame 0x2F5170) [R]: Nick_Fire on its node FxFire in that
 * node's frame, Glowing_Eyeball on FxFireEyeL and on FxFireEyeR in the world's; made at the setup, outside the
 * budget, shut down by becomeHuman (0x2D3ED0 -> 0x2F4F60) until the next world's setup */
static void mon_nick_fx(Mon *mo)
{
    static const char *const NODE[3] = { "FxFire", "FxFireEyeL", "FxFireEyeR" };
    static const char *const NAME[3] = { "Nick_Fire", "Glowing_Eyeball", "Glowing_Eyeball" };
    static const float I[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    Viewer *v = mo->v;
    if ((mo->nick != 1 && mo->nick != 2) || !v->fxw || !mon_in(mo))
        return;
    for (int k = 0; mo->nick == 1 && k < 3; k++)
        mo->nick_fx[k] = -1;
    for (int k = 0; k < 3; k++) {
        Mat4 w;
        float pos[3], R[3][3];
        if (mon_node_world(mo, NODE[k], &w) != 0)
            continue;
        frame_of(&w, pos, R);
        if (mo->nick == 1) {
            mo->nick_fx[k] = fx_spawn_free(v->fxw, NAME[k], pos, NULL, 1);
            if (getenv("RAMPAGE_TRACE"))
                printf("  effect %s on %s at %.2f %.2f %.2f -> %d\n", NAME[k], NODE[k], pos[0], pos[1], pos[2],
                       mo->nick_fx[k]);
        }
        fx_place(v->fxw, mo->nick_fx[k], pos, k == 0 ? (const float (*)[3])R : I);
    }
    mo->nick = 2;
}

/* monster.callMonsterFunction(name, 0, 0, 0, 0): "<Monster>::name", else "Monsters::name" (Monsters.bsf); no
 * monster has its own of these. The books' MonsterFunction (Beans: doFart, the cats: doCatSound) */
static void mon_function(Mon *mo, const char *name)
{
    if (!name)
        return;
    if (!strcmp(name, "doFart"))
        mon_fart(mo);
    else if (!strcmp(name, "doBarf"))
        mon_fx_named(mo, "Barf", "fxNode_Belch");
    else if (!strcmp(name, "doCatSound"))           /* add3DSfx("Cat_1", 0): on the monster, following it */
        obj_sound(mo->v, OS_MONSTER, (int)(mo - mo->v->mon), "Cat_1", 0, &mo->mx);
}

/* installObjectTimer("Monsters::timedDoFart", delay) on the monster: one of eaten food in five (the people's,
 * soldiers' and vehicles' onThrow) */
static void mon_fart_later(Mon *mo, float delay)
{
    for (int k = 0; k < 4; k++)
        if (mo->fart_in[k] <= 0) {
            mo->fart_in[k] = delay > 0 ? delay : 1e-6f;
            return;
        }
}

/* a new monster in the slot: the old one's slot effects shut down, for the viewer's monster swap. In the game the
 * old Monster goes (0x2EACA0 -> 0x2F5AF0 releases the record's definitions but not the slots): its FollowEffects
 * lose their object, keep their own last matrix (0x290F60 -> 0x291440) and run on; they would shut down only with
 * +0xF4 bit 8 (0x291238), which 0x291A20 never sets [R] */
static void mon_fx_stop_all(Mon *mo)
{
    for (int k = 0; k < MON_FX_SLOTS; k++)
        if ((mo->fx_used >> k & 1) && mo->v->fxw)
            fx_stop(mo->v->fxw, mo->fx_slot[k]);
    mo->fx_used = 0;
    for (int k = 0; k < mo->nfx_named; k++)
        if (mo->v->fxw)
            fx_stop(mo->v->fxw, mo->fx_named[k].fx);
    mo->nfx_named = 0;
    for (int k = 0; mo->nick == 2 && k < 3; k++)     /* Nick's, as the slots' */
        if (mo->v->fxw)
            fx_stop(mo->v->fxw, mo->nick_fx[k]);
    mo->nick = 0;
}

/* canGrabPerson's / doGrabPerson's window person (getBuildingOccupant 0x2B37C0), -1 none */
static int window_soldier(Mon *mo, const float *xyz)   /* the army unit at that window, -1 none */
{
    Viewer *v = mo->v;
    PeopleBox boxes[64];
    int nb = scene_people_boxes(&v->scene, boxes, 64), b, st, col;
    float at[3] = { mo->mx, mo->my, mo->mz };
    if (!v->army || !people_probe(boxes, nb, at, xyz, mo->mirror, &b, &st, &col) || !boxes[b].live || st >= boxes[b].n)
        return -1;
    if (v->edibles && edible_hides(v->edibles, b, boxes[b].live[st], col))
        return -1;                                   /* 0x2B3900: an edible out behind its unbroken window */
    return scene_window_soldier(&v->scene, b, boxes[b].live[st], col);
}

static int window_person(Mon *mo, const float *xyz)
{
    Viewer *v = mo->v;
    PeopleBox boxes[64];
    int nb = scene_people_boxes(&v->scene, boxes, 64), b, st, col;
    float at[3] = { mo->mx, mo->my, mo->mz };
    if (v->edibles && people_probe(boxes, nb, at, xyz, mo->mirror, &b, &st, &col) && boxes[b].live &&
        st < boxes[b].n && edible_hides(v->edibles, b, boxes[b].live[st], col))
        return -1;                                   /* 0x2B3900: an edible out behind its unbroken window */
    return v->scene.folk ? people_can_grab(v->scene.folk, boxes, nb, at, xyz, mo->mirror) : -1;
}

/* ---- the bots' view of the world (director.h DirBotView): the climbing code's answers for this monster */
static int bot_cell(void *ctx, int bi, int storey, int col)
{
    const Viewer *v = ((Mon *)ctx)->v;
    if (bi < 0 || (uint32_t)bi >= v->scene.nbuildings)
        return -1;
    const Building *b = &v->scene.buildings[bi].b;
    int ci = building_cell_index(b, FACE_FRONT, col);
    return ci < 0 || storey < 0 || storey >= b->n ? -1 : b->cell[b->live[storey]][ci].state;
}
static int bot_can_mount(void *ctx, int side, int *building)
{
    Mon *mo = ctx;
    Climber c = climber(mo);
    int r = climb_can_mount(&mo->mount, &mo->v->scene, &c, side, 0);
    *building = r == 1 ? mo->mount.cand.building : -1;
    return r;
}
static int bot_can_climb(void *ctx, int dv, int dh)
{
    Mon *mo = ctx;
    Climber c = climber(mo);
    return climb_can_climb(&mo->mount, &mo->v->scene, &c, dv, dh, 0);
}
static int bot_place(void *ctx, int bi, const float pt[3])
{
    Mon *mo = ctx;
    Climber c = climber(mo);
    c.focus_x = pt[0];                               /* (an AI: no camera reach) */
    return climb_place_ok(&mo->v->scene, bi, pt, &c);
}
static void bot_collapse(void *ctx, int bi)          /* collapseBuilding, scored to the bot (+0x190) */
{
    Mon *mo = ctx;
    if (bi >= 0 && (uint32_t)bi < mo->v->scene.nbuildings) {
        mo->v->scene.buildings[bi].hitter = mo->slot;
        building_destroy(&mo->v->scene.buildings[bi].b);
        mo->v->bot_guard_falls++;
    }
}

/* what the AI monsters' DestroyBuildings goals see this frame: the buildings, the monsters, each one itself */
static void bot_views(Viewer *v, DirBuilding *db, DirMonster *dm, DirBotView *bv)
{
    int nb = 0;
    for (uint32_t i = 0; i < v->scene.nbuildings && nb < 64; i++, nb++) {
        const Building *b = &v->scene.buildings[i].b;
        DirBuilding *o = &db[nb];
        memcpy(o->min, b->min, sizeof o->min);
        memcpy(o->max, b->max, sizeof o->max);
        o->pos[0] = (b->min[0] + b->max[0]) / 2;
        o->pos[1] = (b->min[1] + b->max[1]) / 2;
        o->pos[2] = b->min[2];
        o->state = b->state;
        o->row = b->row;
        o->width = b->width;
        o->storeys = b->n;
    }
    for (int k = 0; k < v->nmon; k++) {
        const Mon *mo = &v->mon[k];
        DirMonster *o = &dm[k];
        o->pos[0] = mo->mx; o->pos[1] = mo->my; o->pos[2] = mo->mz;
        o->mounted = mo->mount.mounted ? mo->mount.at.building : -1;
        o->rail = mo->mount.at.rail;
        o->human = mo->human;
        o->ai = mo->is_ai;
        float s = mo->scale > 0 ? mo->scale : 1, h = mo->heading / 57.2957795f;
        /* [R 0x2E4FA0] (MONSTER_DATA +0x188 + the type-15 record's +0x8) x its scale */
        o->radius = (mo->graph.bot_r > 0 ? mo->graph.bot_r + mo->ev15r : MONSTER_HALF_WIDTH) * s;
        /* [R 0x2E4E90] its place + (+0x190 + the record's +0xC) along its x axis + (+0x194 + its +0x10) along its y,
         * x its scale */
        float fx = (mo->graph.bot_off[0] + mo->ev15x) * s, fy = (mo->graph.bot_off[1] + mo->ev15) * s;
        o->ref[0] = mo->mx + cosf(h) * fx - sinf(h) * fy;
        o->ref[1] = mo->my + sinf(h) * fx + cosf(h) * fy;
        o->ref[2] = mo->mz;
    }
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        DirBotView *b = &bv[k];
        memset(b, 0, sizeof *b);
        float h = mo->heading / 57.2957795f;
        b->pos[0] = mo->mx; b->pos[1] = mo->my; b->pos[2] = mo->mz;
        b->fwd[0] = -sinf(h); b->fwd[1] = cosf(h);
        b->mirror = mo->mirror;
        b->mirror_queued = mo->mirror_queued;
        b->mode = mo->mode;
        b->mounted = dm[k].mounted;
        b->rail = mo->mount.at.rail;
        b->storey = mo->mount.at.storey;
        b->smash = (mo->power.abilities & 8) != 0 || mo->power.rage != 0;
        b->hp = mo->hp;
        b->half_width = dm[k].radius;
        b->probe = ((mo->graph.probe[1]) + mo->ev16) * (mo->scale > 0 ? mo->scale : 1);
        b->self = k;
        b->b = db;
        b->nb = nb;
        b->m = dm;
        b->nm = v->nmon;
        b->under = -1;
        for (int i = 0; i < nb && b->under < 0; i++)   /* the building under its body (0x2D6150) */
            if (mo->mx >= db[i].min[0] && mo->mx <= db[i].max[0] && mo->my >= db[i].min[1] && mo->my <= db[i].max[1] &&
                mo->mz >= db[i].max[2] - 1 && b->mounted < 0)
                b->under = i;
        b->ctx = mo;
        b->cell = bot_cell;
        b->can_mount = bot_can_mount;
        b->can_climb = bot_can_climb;
        b->place = bot_place;
        b->collapse = bot_collapse;
        mo->ai.view = b;
    }
}

/* the TwoTilts camera frames this monster (0x2D69A0) [R]: one in the scene, not hidden, not dropping in, not human
 * unless its streaker still runs and is not held; alone too (0x312B50 holds its last view when it frames none) */
static int cam_follows(const Mon *mo)
{
    const Instance *in = mon_in(mo);
    if (!in)
        return 0;
    if (mo->dropping)                                /* 0x2D69A0: not dropping in, not human unless its streaker */
        return 0;                                    /* still runs and is not held [R] */
    if (mo->human) {
        const Person *p = mo->streaker >= 0 && mo->v->scene.folk ? people_get(mo->v->scene.folk, mo->streaker) : NULL;
        return p && p->phase != PP_GONE && p->phase != PP_HELD;
    }
    return !in->hidden;
}

/* one frame of a monster's own play, from its pad (Mon.pad). The graph picks the state; the clip's root carries the
 * monster (its travel added to where it stands, its turn when a NODE_BAKE_ROOT state ends), the stick turns it
 * (NODE_STEER) or drives it (NODE_AIR_*) in the camera's frame, the clip's events set its movement mode and velocity,
 * and in the air it falls by MOVES_GRAVITY until it meets the ground, which fires the landing slot. Then its place,
 * head, scale, hands and feet for the posing */
static void mon_step(Mon *mo, float dt)
{
    Viewer *v = mo->v;
    static const uint8_t none[MV_COUNT];
    float lx = mo->pad.lx, ly = mo->pad.ly;
    const uint8_t *pressed = mo->pad.pressed, *held = mo->pad.held;
    if (mo->thrown_t > 0)                            /* 0x2E1E88 .. 0x2E1EAC */
        mo->thrown_t = mo->thrown_t > dt ? mo->thrown_t - dt : 0;
    if (mo->is_ai) {                                  /* the game drives the monster: no pad */
        lx = mo->ai.stick[0];
        ly = mo->ai.stick[1];
        pressed = held = none;
    }
    if (mo->have_graph) {
        MoveInput *in = &mo->input;
        face_step(mo, dt);
        if (in->v[MV_LX] != lx) in->fresh[MV_LX] = 0.2f;
        if (in->v[MV_LY] != ly) in->fresh[MV_LY] = 0.2f;
        in->v[MV_LX] = lx;
        in->v[MV_LY] = ly;
        for (int k = MV_UP; k <= MV_L3; k++) {
            in->v[k] = held[k] || pressed[k];
            if (pressed[k]) in->fresh[k] = 0.2f;
        }
        moves_derive(in);
        {                                            /* the stick in the monster's frame (0x2C0AD0 -> 0x2C03E0) */
            float w[3], h = mo->heading / 57.2957795f, c = cosf(h), s = sinf(h);
            stick_world(mo, lx, ly, w);
            moves_sectors(in, c * w[0] + s * w[1], -s * w[0] + c * w[1]);
        }
        const MoveState *st = &mo->graph.states[mo->run.state];
        /* the camera frame the stick is read in: every frame (0x2E0738), or, in a NODE_HOLD_FRAME state, only when the
         * stick moves (0x2E8190), so a climb keeps its direction while the camera swings */
        if (!(st->flags & NODE_HOLD_FRAME) || lx != mo->stick[0] || ly != mo->stick[1])
            memcpy(mo->frame, v->view.rows, sizeof mo->frame);
        mo->stick[0] = lx;
        mo->stick[1] = ly;
        float t0 = mo->run.time, length = st->length;
        float cdt = dt * mo->power.speed;             /* the clips run at the speed scale x their rate (0x2EBFE0:
                                                      * setSpeedScale, the rage's 1.33, the speed power-up) */
        int ev = -1, was_mode = mo->mode;            /* the mode 0x2E2770 switches on (the landing sets 4) */
        if (mo->force_to > 0 && mo->force_to <= mo->graph.n) {   /* a played reaction (force_state) */
            moves_force(&mo->run, mo->force_to - 1);
            ev = 0;
        }
        mo->force_to = 0;
        mo->water_tested = 0;                        /* 0x2DE5BC: the water is asked anew each frame */
        mo->under_tested = 0;                        /* 0x2DE5A4: and the building under it */
        if (mo->mode == MODE_AIRBORNE) {             /* the air (0x2DEBD8, mode 1 of 0x2DEBB0's switch) */
            float pos[3] = { mo->mx, mo->my, mo->mz };  /* 0x2DEBD8: drag, the move, gravity */
            monster_air_velocity(mo->vel, pos, dt, mo->no_gravity);
            mo->mx = pos[0]; mo->my = pos[1]; mo->mz = pos[2];
            int under;
            float g = ground_under(mo, &under);
            /* [R 0x2E29A4 .. 0x2E2A9C, 0x2E2770's mode-1 case] more than 3 up, its body is 3 above its place (the
             * jump clips carry it so; as for the water, 0x2D603C, and the box, 0x2E2E18): it lands once z + 3 is down
             * to the ground and is put 3 under it, the ground follow (0x2E0330) lifting it the rest while the
             * landing crouches. No velocity in the test; none on a building coming down (0x2D6150's +0x14C >= 4,
             * 0x2E2A2C): it falls on through */
            int high = mo->mz > 3;
            if (mo->mz + (high ? 3 : 0) <= g &&
                !(under >= 0 && v->scene.buildings[under].b.state >= BUILDING_CRACKING)) {
                mo->landed = 1;
                mo->landed_z = mo->mz;
                mo->mz = high ? g - 3 : g;
                int wk = mon_water(mo);
                if (wk >= 0) {                       /* into the water (0x2DCA94 -> 0x2E2BD0): pressed down 1.25 x
                                                      * its radius across at its body centre, by its fall speed
                                                      * (+0xBD38) */
                    float s = fminf(fmaxf(-mo->vel[2] / 50, 0), 1), at[3];
                    mon_body_centre(mo, at);
                    water_disturb(v->scene.water[wk], at, 1.25f * mon_radius(mo), -(2 * s + 0.5f * (1 - s)));
                }
                if (v->fxw) {                        /* every landing (0x2DC450 -> 0x2F48D0): the big footfall
                                                      * under the spine, GroundImpactFx / WaterImpactFx */
                    Mat4 sp;
                    float spine[3] = { mo->mx, mo->my, mo->mz };
                    FootFx fx;
                    if (scene_monster_node(&v->scene, mo->slot, FOOT_IMPACT_BONE, &sp) == 0) {
                        spine[0] = sp.m[12]; spine[1] = sp.m[13];
                    }
                    if (foot_impact(&mo->feet_fx, spine, g, wk >= 0,
                                    wk >= 0 ? water_surface(v->scene.water[wk]) : 0, &fx)) {
                        fx_spawn_free(v->fxw, fx.name, fx.pos, NULL, 1);   /* outside the budget (0x2F48D0) */
                        if (getenv("RAMPAGE_TRACE"))
                            printf("  landing %s at %.1f %.1f %.1f%c", fx.name, fx.pos[0], fx.pos[1], fx.pos[2], 10);
                    }
                }
                mo->dropping = 0;                    /* (0x2E1A14: in the camera again) */
                /* no rumble: 0x2DC9F4's 400 ms is asked only in mode 1 or 2 (0x2DC9C4), and 0x2DC5DC has already set
                 * mode 4, nothing between writing +0xAD8 [R] */
                /* landed (0x2DC450): slot 56 in the current column, taken as setReaction does (not defeated, not
                 * shrunk below 0.3, 0x2DC5E0 .. 0x2DC610) and let go (0x2DA570, 0x2DC628) */
                int to = mo->defeated || mo->scale < 0.3f ? -1 : moves_slot(&mo->graph, SLOT_LAND, mo->column);
                if (to >= 0) {
                    let_go(mo, 0);
                    moves_force(&mo->run, to);
                    ev = 0;
                }
                mo->mode = MODE_PRONE;
                memset(mo->vel, 0, sizeof mo->vel);
            }
        } else if (mo->mode == MODE_BUILDING) {
            /* 0x2E03A8 (mode 5 of the switch, 0x431290 [R]): the velocity 0 unless a state was posted (+0x110), no
             * gravity and no ground follow, mounted or not (a knock-off's clip holds it where it is till its own
             * mode event, after unmountBuilding); only below a ground at or under the street it is lifted, 0.4 x
             * ground + 0.6 x z a world step (0x2E06E0 .. 0x2E0730) */
            if (ev != 0)
                memset(mo->vel, 0, sizeof mo->vel);
            float g = ground_at(mo);
            if (mo->mz < g && g <= 0)
                mo->mz = g + (mo->mz - g) * powf(0.6f, dt * 30);
        } else {                                     /* 0x2DF6F0 (modes 0, 2 and 4 of the switch, 0x431290 [R]:
                                                      * taking off it is on the ground till a velocity event sets
                                                      * mode 1): a knock slides it, friction stops it */
            float pos[3] = { mo->mx, mo->my, mo->mz };
            monster_ground_velocity(mo->vel, pos, mo->friction, dt, st->flags, mo->pending_slot >= 0);
            mo->mx = pos[0]; mo->my = pos[1]; mo->mz = pos[2];
            /* its z after its ground (0x2E0330, modes 0, 2 and 4) [R]: off a roof's edge onto a lower one, or onto the
             * street, it is eased down so in the state it is in; nothing in those modes makes it fall (+0xAD8 is
             * written only by the clips' mode events, 0x2E7DE4, their velocity ones, 0x2DB130, the clip's start,
             * 0x2E8028, and the landing, 0x2DC5DC). On a roof's free edge the box test holds it (0x2A49C0) */
            mo->mz = monster_ground_follow(mo->mz, ground_at(mo), mo->no_gravity, dt);
        }
        if ((was_mode == MODE_GROUND || was_mode == MODE_BUILDING) && (st->flags & 0x810D2)) {
            /* wading (0x2E2870, the case of modes 0 and 5 in 0x2E2770: 0x2E28A0 .. 0x2E2974): in a state that
             * travels (node +0x2C & 0x1080, 0x50 or 0x80002), the water pressed down at its body centre (0x2E2BD0)
             * half its radius across, to 0.9 x how far its ground (0x2E8530) is under the surface over its half
             * height (+0x19C x scale), that at most 1. Standing still, prone or in the air it presses nothing [R] */
            int wk = mon_water(mo);
            if (wk >= 0) {
                float at[3], hh = mon_half_height(mo);
                float deep = fminf(fmaxf(water_surface(v->scene.water[wk]) - ground_at(mo), 0), hh) / hh;
                mon_body_centre(mo, at);
                water_disturb(v->scene.water[wk], at, 0.5f * mon_radius(mo), -0.9f * deep);
            }
        }
        air_control(mo, st, lx, ly, dt);
        if (mo->mode != MODE_AIRBORNE)               /* 0x2E0C38: off the air the double jump and the spins are there
                                                      * again */
            mo->double_jumped = mo->spins = 0;
        if (mo->mode == MODE_GROUND || mo->mode == MODE_AIRBORNE || mo->mode == MODE_TAKEOFF || mo->mode == MODE_PRONE) {
            mo->mx += mo->air[0] * dt;                 /* 0x2E0C50: in modes 0, 1, 2 and 4 */
            mo->my += mo->air[1] * dt;
        }
        if (st->flags & NODE_STEER)
            mo->heading += steer(mo, st, lx, ly, dt);
        scene_monster_blend_step(&v->scene, mo->slot, dt);
        if (ev != 0 && mo->pending_slot >= 0) {       /* the army's hit: the reaction slot (0x2D6A50), its column's;
                                                      * the mount stays (0x2D6A50 does not unmount: a face's reaction
                                                      * holds on, any other lets go at the next mount check, mode 5 no
                                                      * more, 0x2E14F0) [R] */
            int to = moves_slot(&mo->graph, mo->pending_slot, mo->column);
            mo->pending_slot = -1;
            if (to >= 0) {
                moves_force(&mo->run, to);
                if (mo->vel[2] > 0)
                    mo->mode = MODE_AIRBORNE;
                ev = 0;
            }
        }
        if (ev != 0) {                               /* 0x2E14F0: held on the face, or knocked off it */
            Climber cl = climber(mo);
            cl.in_water = mon_water(mo) >= 0;        /* 0x2E1618, while mounted in mode 5 */
            int was = mo->mount.at.building;
            int slot = climb_check(&mo->mount, &v->scene, &cl, mo->mode);
            int to = slot >= 0 ? moves_slot(&mo->graph, slot, mo->column) : -1;
            if (slot < 0 && !mo->mount.mounted && mo->mode != MODE_AIRBORNE && mo->mode != MODE_TAKEOFF &&
                !mo->defeated && mo->scale >= 0.3f) {
                /* not mounted (0x2E14F4 -> 0x2E18D0) [R]: outside modes 1 and 2, the building under it (0x2D6150)
                 * falling (+0x14C >= 6) slips it off: slot 73 of its column (George's React_Ground_Slip_Heavy,
                 * addVelocity(0, -20, 15) into the fall), unless that state is the one playing (+0x10C), taken as
                 * setReaction does and let go (0x2E19EC); it does not ride the building down */
                int under = mon_under(mo), k;
                if (under >= 0 && v->scene.buildings[under].b.state >= BUILDING_FALLING &&
                    (k = moves_slot(&mo->graph, 73, mo->column)) >= 0 && k != mo->run.state) {
                    let_go(mo, 0);
                    moves_force(&mo->run, k);
                    ev = 0;
                }
            }
            /* knocked off (0x2E1680 .. 0x2E1888) [R]: the slot's state unless it is the one playing (+0x10C), taken
             * as setReaction does (not defeated, not shrunk below 0.3) and let go (0x2E176C, 0x2E185C); the mount and
             * the mode stay: the clip lets go of the face (unmountBuilding) and sets its own modes. Slot 31 (not
             * the falling building's) also has the AI keep off the building a while (0x2ED720 -> 0x2EEBE0) */
            if (to >= 0 && to != mo->run.state) {
                if (slot == 31 && mo->is_ai)
                    dir_ai_avoid(&mo->ai, was, NULL);
                if (!mo->defeated && mo->scale >= 0.3f) {
                    let_go(mo, 0);
                    moves_force(&mo->run, to);
                    ev = 0;
                }
            }
        }
        if (mo->dropping && mo->mode != MODE_AIRBORNE)   /* 0x2E19F8 .. 0x2E1A14: dropping in only while in the air */
            mo->dropping = 0;
        float before[3] = { mo->mx, mo->my, mo->mz };
        float root[3] = { mo->mx, mo->my, mo->mz };
        int switched = ev == 0 || (mo->is_ai && mo->have_ag ?   /* the AI picks the transitions (0x2E1AA0) */
                                   dir_ai_step(&mo->ai, &mo->ag, &mo->run, root, cdt, answer, mo) :
                                   moves_step(&mo->run, in, cdt, answer, mo));
        for (; mo->is_ai && mo->ai.done > 0; mo->ai.done--)   /* Levels::moveComplete */
            if (mo->reveal)                          /* Monsters::onRevealNIEnd: stopReveal */
                reveal_walk_arrived(&v->rwalk, &v->reveal);
            else if (v->game)
                game_move_done(v->game, mo->slot);
        float t1 = switched ? (ev == 0 ? t0 : (t0 + cdt < length ? t0 + cdt : length)) : mo->run.time;
        float a[3], b[3], q[4];                      /* the root's travel and turn this frame the state hands the monster */
        if (root_at(mo, clip_at(mo, st, t0), a, q) == 0 && root_at(mo, clip_at(mo, st, t1), b, q) == 0) {
            float c = cosf(mo->heading / 57.2957795f), s = sinf(mo->heading / 57.2957795f);
            float dx = st->flags & NODE_ROOT_X ? b[0] - a[0] : 0, dy = st->flags & NODE_ROOT_Y ? b[1] - a[1] : 0;
            float m = mo->mirror ? -1.0f : 1.0f;      /* mirrored: across the monster's x */
            mo->mx += c * m * dx - s * dy;
            mo->my += s * m * dx + c * dy;
            if (st->flags & NODE_ROOT_Z)
                mo->mz += b[2] - a[2];
            if (st->flags & NODE_ROOT_TURN)
                mo->heading += m * root_turn(mo, clip_at(mo, st, t0), clip_at(mo, st, t1));
        }
        clip_events(mo, st, t0, t1);                  /* the old clip's events up to now */
        if (mo->mode == MODE_BUILDING && mo->mount.at.building >= 0) {   /* held on the face (0x2D5870) */
            float pos[3] = { mo->mx, mo->my, mo->mz }, delta[3] = { mo->mx - before[0], mo->my - before[1], mo->mz - before[2] };
            float h = mo->heading;
            climb_place(&mo->mount, &v->scene, st->flags, delta, dt, pos, &h);
            /* [R 0x2E0D44 .. 0x2E0EE4] the monster goes 0.4 of the way there each frame of its update (a world step):
             * its place by a lerp, its turn by a slerp (0x167DE0), so a new place (getting on, over the top, off the
             * roof) is eased into; here 0.6^(dt / step) of the way is left */
            float k = 1 - powf(0.6f, dt / TIMESTEP);
            mo->mx += (pos[0] - mo->mx) * k;
            mo->my += (pos[1] - mo->my) * k;
            mo->mz += (pos[2] - mo->mz) * k;
            mo->heading += (fmodf(h - mo->heading + 540, 360) - 180) * k;
        }
        if (switched) {
            /* a clip that bakes: its root, as drawn where it ended (the axes it handed over were held at its start, a
             * turn it handed over taken out), in the monster's frame as seen (mirrored if it was) */
            int bake = (st->flags & NODE_BAKE_ROOT) && root_at(mo, clip_at(mo, st, t1), b, q) == 0;
            float bake_p[3], bake_r[3][3];
            if (bake) {
                for (int k = 0; k < 3; k++)
                    bake_p[k] = st->flags & (k == 0 ? NODE_ROOT_X : k == 1 ? NODE_ROOT_Y : NODE_ROOT_Z) ?
                                mo->entry_root[k] : b[k];
                float zero[3] = { 0, 0, 0 }, tw = st->flags & NODE_ROOT_TURN ? root_turn(mo, mo->entry_t, clip_at(mo, st, t1)) : 0;
                Mat4 r = mat_mul(mat_trs_z(0, 0, 0, -tw), mat_tq(zero, q));
                seen_root(mo->mirror, bake_p, &r, bake_r);
            }
            const MoveState *to = &mo->graph.states[mo->run.state];
            if (mo->run.from != mo->run.state && mo->slot)   /* (the other monsters' marked with their slot) */
                printf(getenv("RAMPAGE_TRACE") ? "[%d] %s -> %s (%s) by %d\n" : "[%d] %s -> %s (%s)\n", mo->slot + 1,
                       st->name ? st->name : "?", to->name ? to->name : "?", to->clip ? to->clip : "-",
                       ev == 0 ? -2 : mo->run.via);
            else if (mo->run.from != mo->run.state)
                printf(getenv("RAMPAGE_TRACE") ? "%s -> %s (%s) by %d\n" : "%s -> %s (%s)\n", st->name ? st->name : "?",
                       to->name ? to->name : "?", to->clip ? to->clip : "-", ev == 0 ? -2 : mo->run.via);
            if (getenv("RAMPAGE_TRACE"))
                printf("  stick %.2f %.2f size %.2f\n", lx, ly, in->v[MV_L_SIZE]);
            /* the transition says whether the new clip blends in (over the new state's blend time) or cuts; a start
             * the game forces (a slot: 0x2C0DC0 .. 0x2C0E78, +0x110) takes no transition and blends in when the new
             * state's blend time is over 0.001, else cuts [R], which is what blending over its time does; it starts
             * at 0, as a start list entry's flag 8 (keep the phase) is set in none of the 30 monsters' lists */
            int blend = ev == 0 ? BLEND_IN : mo->run.via >= 0 ? st->trans[mo->run.via].blend : st->end_blend;
            scene_monster_blend(&v->scene, mo->slot, blend == BLEND_IN ? to->blend : 0);
            foot_command(&mo->feet, st->flags);      /* 0x33CCA0: the foot object keeps the state left */
            play_state(mo);
            if (bake && root_at(mo, mo->entry_t, b, q) == 0) {
                /* 0x2E6580: the monster moves and turns so that the new clip's root, where it starts, is where the old
                 * one's was: the old root's place and turn become the monster's */
                float zero[3] = { 0, 0, 0 }, np[3] = { b[0], b[1], b[2] }, nr[3][3];
                Mat4 r = mat_tq(zero, q);
                seen_root(mo->mirror, np, &r, nr);
                float h = atan2f(bake_r[1][0] * nr[0][0] + bake_r[1][1] * nr[0][1] + bake_r[1][2] * nr[0][2],
                                 bake_r[0][0] * nr[0][0] + bake_r[0][1] * nr[0][1] + bake_r[0][2] * nr[0][2]);
                float ch = cosf(h), sh = sinf(h);
                float d[3] = { bake_p[0] - (ch * np[0] - sh * np[1]), bake_p[1] - (sh * np[0] + ch * np[1]),
                               bake_p[2] - np[2] };
                float c = cosf(mo->heading / 57.2957795f), s = sinf(mo->heading / 57.2957795f);
                mo->mx += c * d[0] - s * d[1];
                mo->my += s * d[0] + c * d[1];
                mo->mz += d[2];
                mo->heading += h * 57.2957795f;
                scene_monster_hold_root(&v->scene, mo->slot, b, q);   /* 0x300FE0: the blend keeps the new root */
                if (getenv("RAMPAGE_ROOT"))
                    printf("  bake: old root %.2f %.2f %.2f, new %.2f %.2f %.2f, turn %.0f\n", bake_p[0], bake_p[1],
                           bake_p[2], np[0], np[1], np[2], h * 57.2957795f);
            }
            mo->column = -1;                         /* 0x2E7F70: a new state starts without the old one's effects */
            mo->no_gravity = 0;
            /* and the new one's up to where it starts: the game's event player (0x2F45D0) begins at the clip's first
             * event and fires, in order, every one at or before the start, so a state entered part-way (phase
             * kept) still gets its set-up (mode, column) */
            clip_events(mo, to, -1, mo->run.time);
            if (ev != 0 && !mo->defeated && mo->hp <= 0 && mo->pending_slot < 0 && mo->mode != MODE_AIRBORNE) {
                /* [R 0x2E68A0 .. 0x2E6A4C] the state command's own test, as a state starts that is not a reaction
                 * (the command's +0xC; a forced start, ev 0 here) with none asked (+0x110): at 0 HP, not in the air
                 * (+0xAD8 1) and not yet defeated (+0xA59), the defeat as TakeDamage's (0x2D9500 .. 0x2D95F4): the
                 * velocity 0, its slot (67, setReaction's rules: what it holds let go of), +0xA59 = 1. So the
                 * reaction to the blow that took the last HP plays out first, and the HP the scripts take
                 * (addHitPoints 0x2DA530: the bosses, the trains, the rockets) fells it too at its next state */
                memset(mo->vel, 0, sizeof mo->vel);
                set_reaction(mo, 67);
                mo->defeated = 1;
            }
        }
        if (mon_in(mo) && mo->feet.anchor >= 0) {   /* the foot lock (0x33BF10): the root eases onto
                                                      * the planted foot as the clip has it at its time 0 */
            const Instance *in = mon_in(mo);
            float feet[2][3], place[3] = { mo->mx, mo->my, mo->mz };
            if (ik_clip_node(in, 0, "Bip01 L Foot", feet[0]) == 0 && ik_clip_node(in, 0, "Bip01 R Foot", feet[1]) == 0 &&
                foot_lock(&mo->feet, mo->graph.states[mo->run.state].flags, (const float (*)[3])feet, place, mo->heading,
                          dt / TIMESTEP)) {
                mo->mx = place[0];
                mo->my = place[1];
                if (mo->is_ai)                       /* 0x2E0F54: the goal's +0x38 */
                    dir_ai_cooldown(&mo->ai);
            }
        }
        if (!mo->reveal) {                           /* (the revealed one walks off to x 100 [R]) */
            if (mo->mx < -75) mo->mx = -75;
            if (mo->mx > 75) mo->mx = 75;
        }
        push_apart(mo, dt);
        push_out(mo);                                 /* on a face (mode 5) only the triggers: it is held by the face */
        if (!mo->is_ai && !mo->mount.mounted) {   /* the camera's reach (0x336290): a player off a face stays
                                                      * at most 70 up and 30 along x from the players' middle */
            if (mo->mz > 70) {
                mo->mz = 70;
                if (mo->vel[2] > 0) mo->vel[2] = 0;
                if (mo->air[2] > 0) mo->air[2] = 0;
            }
            if (fabsf(mo->mx - v->mid_x) > 30) {
                mo->mx = v->mid_x + (mo->mx > v->mid_x ? 30 : -30);
                mo->vel[0] = mo->air[0] = 0;
            }
        }
        mo->heading = fmodf(mo->heading + 360, 360);
        if (getenv("RAMPAGE_TRACE") && mo->slot)
            printf("  [%d] %s %.2f at %.1f %.1f %.1f heading %.0f mode %d vz %.1f\n", mo->slot + 1,
                   mo->graph.states[mo->run.state].name, mo->run.time, mo->mx, mo->my, mo->mz, mo->heading, mo->mode,
                   mo->vel[2]);
        else if (getenv("RAMPAGE_TRACE"))
            printf("  %s %.2f at %.1f %.1f %.1f heading %.0f mode %d vz %.1f\n", mo->graph.states[mo->run.state].name,
                   mo->run.time, mo->mx, mo->my, mo->mz, mo->heading, mo->mode, mo->vel[2]);
    }
    scene_monster_place(&v->scene, mo->slot, mo->mx, mo->my, mo->mz, mo->heading);
    {                                                /* the head (0x2E70C0, 0x2FBE30, 0x2FBC90): in a look state the
                                                        right stick's deflection is the target, 81 degrees of yaw and
                                                        45 of pitch (MONSTER_DATA +0xA08 .. +0xA10, the same in all
                                                        30); eased at pi / 2 a second (MD +0x174, +0x178), half that
                                                        while the stick is held; back to 0 outside */
        uint32_t fl = mo->have_graph ? mo->graph.states[mo->run.state].flags : 0;
        look_stick(&mo->head, &mo->look_lim, (fl & NODE_LOOK) != 0, mo->pad.rx, mo->pad.ry);
        look_update(&mo->head, &mo->look_lim, 0, fl); /* no target: tracking is off in this build (look.c) */
        look_step(&mo->head, &mo->look_lim, dt);
        mo->look_yaw = mo->head.yaw;
        mo->look_pitch = mo->head.pitch;
        /* [R] the game turns the head in the mirrored pose (0x2FFF10 mirrors the keys, 0x194BA0, before
         * MonsterXFormTree 0x301610 builds the model and runs the animators' +0xC, 0x30182C): the drawn turn is the
         * yaw as it is; the viewer turns the unmirrored pose and reflects it, so mirrored it turns by -yaw */
        v->scene.mon[mo->slot].look_yaw = mo->mirror ? -mo->look_yaw : mo->look_yaw;
        v->scene.mon[mo->slot].look_pitch = mo->look_pitch;
    }
    mo->tail.dt += dt;                               /* the tail's springs, at the next posing (0x2DEA78) */
    v->scene.mon[mo->slot].tail = &mo->tail;
    if (mo->scale_left > 0) {                         /* the dynamic scale (0x2E1CF0 .. 0x2E1D74 [R]): the old one's
                                                      * share sin(pi / 2 x the time left / its time), +0xA84 x +0xA88;
                                                      * the new one once the time is out */
        mo->scale_left -= dt;
        float f = mo->scale_left > 0 ? sinf(1.5707963f * mo->scale_left / mo->scale_time) : 0;
        mo->scale = mo->scale_to + (mo->scale_from - mo->scale_to) * f;
    }
    if (mo->scale != 1 && mon_in(mo))
        for (int k = 0; k < 11; k++)
            if ((k & 3) != 3)
                mon_in(mo)->base.m[k] *= mo->scale;
    /* [R 0x2D6680] the building it is drawn swaying with: the one it climbs (+0xC640), else on the ground (+0xAD8
     * 0) the one under its body (0x2D6150) */
    scene_monster_sway(&v->scene, mo->slot, mo->mount.at.building >= 0 ? mo->mount.at.building :
                       mo->mode == MODE_GROUND ? mon_building_below(mo) : -1);
    hands_step(mo, dt);
    feet_step(mo, dt);
}

/* the soak test's world half (cheat 082864: hud.c hud_soak) in the world's update, running or paused */
static void soak_step(Viewer *v)
{
    if (!(v->prof.cheats & PCH_10000) || !v->hud)
        return;
    int popup = v->popup && shell_popup_active(v->popup) ? (shell_popup_fade(v->popup) >= 1.0f ? 1 : -1) : 0;
    int ev = hud_soak(v->hud, popup, game_block(v->game, NULL));
    if (ev)
        shell_input(v->popup, ev, 0);
}

/* one frame of play: the monsters (each from its pad: Mon.pad), then the world around them, the HUDs and the camera */
/* a window occupant's droppable (people_set_drop), with the army's last frame of monsters and buildings: the people
 * go before the army in the port's frame [G: the game's object order] */
static void people_drop(void *ctx, const char *name, const float pos[3])
{
    Viewer *v = ctx;
    if (v->army && name)
        army_drop(v->army, name, pos);
}

static void play_step(Viewer *v, float dt, float aspect)
{
    soak_step(v);
    if (v->hud && hud_paused(v->hud)) {              /* GameOver's pauseGame(1): only the screen runs */
        hud_update(v->hud, dt);
        hud_frame(v, 1);
        popup_step(v, dt);
        return;
    }
    v->play_now += dt;
    world_build(v);                                  /* the collision world as the frame begins */
    scene_cloth_step(&v->scene, dt);                 /* the cloths' time, simulated at the next posing */
    fx_budget(v->fxw, v->nmon + (v->extra >= 0));    /* the effects budget, by the monsters in the world (0x2B72B0) */
    v->mid_x = players_mid_x(v);                     /* 0x336360, once a frame (0x2B6CE0) */
    static DirBuilding db[64];
    static DirMonster dm[MONSTERS];
    static DirBotView bv[MONSTERS];
    bot_views(v, db, dm, bv);
    if (v->extra >= 0) {                             /* the revealed monster's 0.25 s timer, its calls */
        reveal_walk_update(&v->rwalk, dt, v->mon[v->extra].mx, v->view.pos[0]);
        reveal_events(v);
    }
    for (int k = 0; k < mon_world(v); k++)
        mon_step(&v->mon[k], dt);
    if (v->extra >= 0)                               /* (it arrived: hidden) */
        reveal_events(v);
    if (v->scene.folk) {                             /* the people, the ones in their hands, what they make happen */
        PeopleMonster pm[MONSTERS];
        for (int k = 0; k < v->nmon; k++) {
            const Mon *mo = &v->mon[k];
            pm[k] = (PeopleMonster){ { mo->mx, mo->my, mo->mz },
                                     mo->have_graph ? mo->graph.states[mo->run.state].name : NULL };
        }
        {                                            /* the occupants' clock and reach (0x318650) */
            float lo = 1000, hi = -1000;             /* [R 0x318390] the players' monsters' x (gWorld +0x40) */
            for (int k = 0; k < v->nmon; k++) {
                lo = fminf(lo, v->mon[k].mx);
                hi = fmaxf(hi, v->mon[k].mx);
            }
            if (fabsf(hi - lo) < 20) {               /* at least 30 wide about their middle */
                float c = (hi + lo) / 2;
                lo = c - 15;
                hi = c + 15;
            }
            v->scene.view_lo = lo - 10;              /* and 10 more on each side */
            v->scene.view_hi = hi + 10;
            /* 0x347DC0, the time left; without a game, as an unstarted clock (all its limit: the range's end) */
            v->scene.spawn_left = v->game ? game_clock(v->game, NULL) : v->scene.military_range;
        }
        scene_people_spawn(&v->scene, dt);
        people_set_world_state(v->scene.folk, v->game ? game_state(v->game) : GS_PLAY);   /* (the chains in play) */
        people_set_drop(v->scene.folk, people_drop, v);
        people_update(v->scene.folk, dt, pm, v->nmon);
        scene_huts_update(&v->scene, dt);           /* (no touch flies them: OnlyHitWithEffNodes, scene_huts_hit) */
        for (int k = 0; k < v->nmon; k++) {
            Mon *mo = &v->mon[k];
            Mat4 h;
            if (mo->held >= 0 && people_get(v->scene.folk, mo->held)->phase != PP_HELD)
                mo->held = -1;
            if (mo->held >= 0 && attach_world(mo, "Helper R Hand", &h) == 0)
                people_hold(v->scene.folk, mo->held, h.m, mo->mirror, dt);
        }
        PeopleEvent ev[32];
        int ne = people_events(v->scene.folk, ev, 32);
        for (int i = 0; i < ne; i++) {               /* what the army takes over: the punks' bottles */
            const Person *p = people_get(v->scene.folk, ev[i].who);
            if (!v->army || !p)
                continue;
            if (ev[i].type == PE_WEAPON) {            /* enableWeapon(1) with its name, enableWeapon(0) without */
                int on = ev[i].on && ev[i].name, k;
                army_person_weapon(v->army, ev[i].who, on ? group_sub(p->tmpl, "Weapon|RifleWeapon") : (Group){ 0 },
                                   p->world, 0);
                for (k = 0; k < v->narmed_people && v->armed_people[k] != ev[i].who; k++)
                    ;
                if (on && k == v->narmed_people && v->narmed_people < 32)
                    v->armed_people[v->narmed_people++] = ev[i].who;
            }
        }
        for (int i = 0; v->army && i < v->narmed_people; i++) {
            const Person *p = people_get(v->scene.folk, v->armed_people[i]);
            if (p && p->phase != PP_GONE)
                army_person_moved(v->army, v->armed_people[i], p->world, 0, p->anim_time, p->anim_len);
            else {                                   /* [R] a deleted actor's weapon goes with it */
                army_person_forget(v->army, v->armed_people[i]);
                v->armed_people[i--] = v->armed_people[--v->narmed_people];
            }
        }
        for (int i = 0; i < ne; i++) {
            if (ev[i].type == PE_SOUND && ev[i].name) {   /* the person's add3DSfx / remove3DSfx (on 0) */
                if (ev[i].who < 0 && ev[i].object >= 0)
                    obj_sound(v, OS_PEOPLE_OBJECT, ev[i].object, ev[i].name, 0, ev[i].v);
                else if (ev[i].who < 0)
                    sound_play(ev[i].name, ev[i].v, 1);
                else if (ev[i].on)                   /* (amount: the script's keep) */
                    obj_sound(v, OS_PEOPLE, ev[i].who, ev[i].name, ev[i].amount != 0, ev[i].v);
                else
                    obj_sound_stop(v, OS_PEOPLE, ev[i].who, ev[i].name);
            }
            if (ev[i].type == PE_EFFECT && !ev[i].node && v->fxw) {   /* a script's createAndStart: a FollowEffect */
                float up[3] = { 0, 1, 0 };
                fx_spawn(v->fxw, ev[i].name, ev[i].v, up, 1);
            }
            if (ev[i].type == PE_EFFECT && ev[i].node && v->fxw) {   /* createAndStartAttachedToNodeByName(actor,
                                                      * name, node): the tourist's CameraFlash on its Camera, in the
                                                      * node's frame (a flash: not followed) */
                Mat4 n;
                float pos[3], R[3][3];
                int on = scene_person_node(&v->scene, ev[i].who, ev[i].node, n.m) == 0;
                if (on) {
                    frame_of(&n, pos, R);
                    fx_spawn_frame(v->fxw, ev[i].name, pos, (const float (*)[3])R, 1);
                } else {
                    fx_spawn(v->fxw, ev[i].name, ev[i].v, NULL, 1);
                }
                if (getenv("RAMPAGE_TRACE"))
                    printf("  person %d effect %s on %s%s\n", ev[i].who, ev[i].name, ev[i].node, on ? "" : " (no node)");
            }
            if (ev[i].type == PE_WINDOW_CLOSE)       /* back in: its window shuts */
                scene_window_close_at(&v->scene, ev[i].v);
            if (ev[i].type == PE_MESSAGE && v->hud && ev[i].name) {   /* ActorSwarm's RECESS (createAndStartText) */
                float rgba[4] = { ev[i].v[0], ev[i].v[1], ev[i].v[2], ev[i].amount };
                hud_banner_text(v->hud, ev[i].name, rgba, 0.5f, "");
            }
            if (ev[i].type == PE_FX_ON && v->fxw && ev[i].name &&   /* the ChallengeEffect only on a goal */
                (ev[i].on != PFX_CHALLENGE || challenge_is_goal(&v->challenge, people_get(v->scene.folk, ev[i].who)->name))) {
                /* attached (0x291ED0(effect, actor, node -1)): on the person where it is now, in its whole frame (the
                 * effect's offsets turn with it: the Singer's and the Doomsayer's em_pt_offset (0, 1, 3) is 1 in front
                 * of the mouth, 0x290F60 -> 0x291440); the place it was started at may be stale (a SpawnsItem mime is
                 * made at its spot, then sent to walk in from x -80 before the host sees this: its NPC_Target ran 40
                 * ahead of it) [R] */
                const Person *p = people_get(v->scene.folk, ev[i].who);
                int follow = ev[i].amount != 0 && p;
                float at[3] = { ev[i].v[0], ev[i].v[1], ev[i].v[2] }, R[3][3];
                if (follow) {
                    memcpy(at, &p->world[12], sizeof at);
                    for (int a = 0; a < 3; a++)
                        for (int c = 0; c < 3; c++)
                            R[a][c] = p->world[4 * a + c];
                }
                int id = follow ? fx_spawn_frame(v->fxw, ev[i].name, at, (const float (*)[3])R, 1) :
                                  fx_spawn(v->fxw, ev[i].name, at, NULL, 1);
                if (id >= 0 && p && v->nppfx < 64) {
                    v->ppfx[v->nppfx].fx = id;
                    v->ppfx[v->nppfx].who = ev[i].who;
                    v->ppfx[v->nppfx].slot = ev[i].on;
                    v->ppfx[v->nppfx].follow = follow;
                    memset(v->ppfx[v->nppfx].off, 0, sizeof v->ppfx[v->nppfx].off);
                    v->nppfx++;
                }
            }
            if (ev[i].type == PE_FX_OFF)
                for (int k = 0; k < v->nppfx; k++)
                    if (v->ppfx[k].who == ev[i].who && v->ppfx[k].slot == ev[i].on) {
                        if (v->fxw)
                            fx_stop(v->fxw, v->ppfx[k].fx);
                        v->ppfx[k--] = v->ppfx[--v->nppfx];
                    }
            Mon *mo = ev_mon(v, ev[i].monster);
            if (ev[i].type == PE_MONSTER_HP)
                mo->hp = fmaxf(0, fminf(mon_hp_max(mo), mo->hp + ev[i].amount));
            else if (ev[i].type == PE_MONSTER_KNOCK) {   /* setVel(v, world), setReaction(2) (Person_Child.bsf,
                                                          * Monsters.bsf doPriestReaction) [R] */
                memcpy(mo->vel, ev[i].v, sizeof mo->vel);   /* (setVel 0x2E74F0 leaves the mode: the reaction's) */
                set_reaction(mo, 2);
            }
            if (getenv("RAMPAGE_TRACE"))
                printf("  people event %d (%s) from %d\n", ev[i].type, ev[i].name ? ev[i].name : "-", ev[i].who);
        }
        for (int k = 0; k < v->nppfx; k++) {          /* the ones that follow their person */
            const Person *p = people_get(v->scene.folk, v->ppfx[k].who);
            if (!p || !v->fxw || !fx_alive(v->fxw, v->ppfx[k].fx)) {
                v->ppfx[k--] = v->ppfx[--v->nppfx];
                continue;
            }
            if (v->ppfx[k].follow) {                 /* in the person's frame as it turns */
                float at[3] = { p->world[12] + v->ppfx[k].off[0], p->world[13] + v->ppfx[k].off[1],
                                p->world[14] + v->ppfx[k].off[2] }, R[3][3];
                for (int a = 0; a < 3; a++)
                    for (int c = 0; c < 3; c++)
                        R[a][c] = p->world[4 * a + c];
                fx_place(v->fxw, v->ppfx[k].fx, at, (const float (*)[3])R);
            }
        }
    }
    uint8_t was[256];                                /* the buildings' states before their step (the pads only) */
    uint32_t nbw = v->pads_live ? (v->scene.nbuildings < 256 ? v->scene.nbuildings : 256) : 0;
    for (uint32_t i = 0; i < nbw; i++)
        was[i] = (uint8_t)v->scene.buildings[i].b.state;
    scene_buildings_update(&v->scene, dt);
    for (uint32_t i = 0; i < nbw; i++)               /* the front has cracked (0x2BD7D0: state 5, Building_Pre_Collapse):
                                                      * 2 s on the pads of the monsters on it, or on its face [R 0x2BD9A0] */
        if (was[i] == BUILDING_CRACKING && v->scene.buildings[i].b.state == BUILDING_SHAKING)
            for (int k = 0; k < v->nmon; k++) {
                Mon *mo = &v->mon[k];
                if (mon_in(mo) && !mo->is_ai && (mon_building_below(mo) == (int)i ||
                                                 (mo->mount.mounted && mo->mount.at.building == (int)i)))
                    mon_rumble(mo, 2000);
            }
    mon_weapons_step(v, dt);                         /* the roar (army.c's monster weapons) */
    street_step(v, dt);
    army_step(v, dt);
    obj_sounds_step(v);                              /* the objects' sounds follow them, or go with them */
    fx_step(v, dt);
    shake_step(v->shakes, 8 + MONSTERS - 1, dt);
    for (int k = v->nmon - 1; k >= 0; k--) {         /* Hud.bsf: health, rage, the score (ArmyStats points[6]); player
                                                      * 1's last, as it has the screens */
        Mon *mo = &v->mon[k];
        if (!mo->hud)
            continue;
        int vis = 0;
        float clock = v->game ? game_clock(v->game, &vis) : 0;
        hud_set(mo->hud, mo->hp, mon_hp_max(mo), mo->energy, 100, v->game ? game_points(v->game, k, GPT_TOTAL) :
                v->army ? (int)army_stats(v->army, k)->points[6] : 0, v->game ? game_lives(v->game, k) : -1,
                v->game && (game_play_type(v->game) == GP_KING_CITY || game_play_type(v->game) == GP_KING_WORLD) ?
                game_blocks_won(v->game, k) : -1);   /* (the King modes: the blocks won by the block icon) */
        hud_set_clock(mo->hud, vis, clock);
        if (k == 0 && hud_challenge_items(mo->hud, challenge_collected(&v->challenge), challenge_required(&v->challenge)))
            challenge_points_all(v, challenge_points(&v->challenge));   /* CHALLENGE MET!: ChallengeDef.Points / players */
        hud_update(mo->hud, dt);
        if (k == 0)
            hud_frame(v, 1);
    }
    popup_step(v, dt);                               /* the SaveGame popup over the screens */
    for (int k = 0; k < v->nmon && v->challenge.up_state == CU_WAITING; k++) {   /* a monster's touch (TakeDamage,
                                                      * Collision): its collision spheres, armed or not [R 0x2E22B0] */
        Mon *mo = &v->mon[k];
        if (!mon_in(mo))
            continue;
        float sp[32][4];
        int ns = mon_spheres(mo, sp, 32);
        for (int i = 0; i < ns && v->challenge.up_state == CU_WAITING; i++)
            upgrade_touch(mo, sp[i], sp[i][3]);
    }
    switch (challenge_upgrade_tick(&v->challenge, dt)) {   /* gAIMgr: the MonsterUpgrade object, its last banner */
    case CUE_SPAWN:
        if (v->fxw && v->challenge.up.effect)
            v->upgrade_fx = fx_spawn(v->fxw, v->challenge.up.effect, v->challenge.up.pos, NULL, 1);
        if (getenv("RAMPAGE_TRACE"))
            printf("  upgrade on offer: %s\n", challenge_upgrade_move(v->challenge.upgrade));
        break;
    case CUE_BANNER:
        if (v->hud) {
            static const float green[4] = { 0, 1, 0, 1 };
            hud_banner_text2(v->hud, challenge_upgrade_key(v->challenge.up_taken),
                             challenge_upgrade_inst(v->challenge.up_taken), green, 1, 3, "");
        }
        break;
    default: break;
    }
    float at[MONSTERS + 2][3];                       /* the camera's points: every monster's root (2 more: cam_interest) */
    DirObject me[MONSTERS];                          /* the script camera's objects: the monsters by slot */
    int nat = 0, cam_held = 0;
    for (int k = 0; k < mon_world(v); k++) {
        Mon *mo = &v->mon[k];
        if (mo->have_graph) {                         /* the pose keeps what the monster did not take over (0x301080:
                                                       * x with 0x40, y with 0x80, z with 0x80000 [R]) */
            const MoveState *st = &mo->graph.states[mo->run.state];
            float turn = st->flags & NODE_ROOT_TURN ?
                root_turn(mo, mo->entry_t, clip_at(mo, st, mo->run.time)) : 0;
            scene_monster_root_motion(&v->scene, mo->slot, (st->flags & NODE_ROOT_X ? 1 : 0) |
                                      (st->flags & NODE_ROOT_Y ? 2 : 0) | (st->flags & NODE_ROOT_Z ? 4 : 0), mo->entry_root,
                                      turn);
        }
        if (k >= v->nmon)                            /* (the revealed monster: not the camera's) */
            continue;
        me[k] = (DirObject){ { mo->mx, mo->my, mo->mz }, mo->heading / 57.2957795f };
        if (cam_follows(mo)) {
            at[nat][0] = mo->mx; at[nat][1] = mo->my; at[nat][2] = mo->mz;
            nat++;
            /* [R 0x312CB0 .. 0x312D1C] holding what is not edible (0x2E5590), or a second from a throw (+0xA94) */
            cam_held |= (held_anything(mo) && !held_edible(mo)) || mo->thrown_t > 0;
        }
    }
    float interest[2][3];                            /* and the camera manager's interest objects: a tank on screen */
    nat = cam_interest(at, nat, (const float (*)[3])interest, v->army ? army_interest(v->army, interest) : 0);
    cam_update(&v->cam, (const float (*)[3])at, nat, cam_held, dt, aspect);
    dircam_update(&v->cd, dt, me, v->nmon, &v->view);   /* the modes (the script camera) and their blend */
    /* [R Traffic.scr onEndFrame] the world's tick (WorldBase +0x28, 0x284DA0) ends in WorldScript +0x38 (0x284E20:
     * slot 4), after its objects' update (+0x24): fNextExitTime less the step, and a new 5 + 5 frand when it is out,
     * which nothing uses (createRunners is never called) but the world's stream */
    for (v->traffic_acc += v->have_scene ? dt : 0; v->traffic_acc >= TIMESTEP; v->traffic_acc -= TIMESTEP) {
        v->traffic_exit -= TIMESTEP;
        if (v->traffic_exit < 0)
            v->traffic_exit = 5.0f + wrand_frand() * 5.0f;
    }
}

/* the hands each frame (0x323130, 0x3230F0): let go off a building (unmountBuilding's 0x322EC0, 0.1 s), the timers and
 * targets, then the IK's targets in the monster's frame */
static void hands_step(Mon *mo, float dt)
{
    Viewer *v = mo->v;
    /* 0x322360 (from 0x2E0FEC; not in states whose node flags have 0x1080, 0x50, 0x80002 or 0x40000): a planted hand
     * the arm cannot reach from below lifts the monster just enough, h = dz - sqrt(dz^2 - (|d|^2 - L^2)), at most
     * 0.4, the larger of the two hands' */
    const MoveState *st = mo->have_graph ? &mo->graph.states[mo->run.state] : NULL;
    if (st && !(st->flags & (0x1080 | 0x50 | 0x80002 | 0x40000))) {
        float lift = 0;
        for (int h = 0; h < 2; h++) {
            float sh[3], reach, d[3], d2 = 0;
            if (!mo->hand[h].planted || scene_monster_shoulder(&v->scene, mo->slot, h, sh, &reach) != 0)
                continue;
            for (int k = 0; k < 3; k++) {
                d[k] = mo->hand[h].tp[k] - sh[k];
                d2 += d[k] * d[k];
            }
            float disc = d[2] * d[2] - (d2 - reach * reach);
            if (d[2] < 0 || d2 < reach * reach || disc < 0)
                continue;
            float up = d[2] - sqrtf(disc);
            if (up <= 0.4f && up > lift)
                lift = up;
        }
        mo->mz += lift;
        if (lift > 0 && mo->is_ai)                   /* 0x2E1044: the goal's +0x38 */
            dir_ai_cooldown(&mo->ai);
    }
    float h0 = mo->heading / 57.2957795f, c = cosf(h0), sn = sinf(h0);
    for (int h = 0; h < 2; h++) {
        Hand *hd = &mo->hand[h];
        if (mo->mount.at.building < 0 && hd->target > 0)
            hand_detach(hd, 0.1f);
        Mat4 w;
        float anim[3] = { hd->tp[0], hd->tp[1], hd->tp[2] };
        if (scene_monster_hand(&v->scene, mo->slot, h, &w) == 0) {
            anim[0] = w.m[12];
            anim[1] = w.m[13];
            anim[2] = w.m[14];
        }
        hand_update(hd, h, &mo->mount, &v->scene, mo->hand_thick, anim, dt);
        float wt = hand_weight(hd);
        if (getenv("RAMPAGE_HANDS") && (wt > 0 || hd->target > 0))
            printf("  hand %d: w %.2f%s%s, clip's at %.2f %.2f %.2f, held at %.2f %.2f %.2f\n", h, wt,
                   hd->attaching ? " attaching" : "", hd->planted ? " planted" : "", anim[0], anim[1], anim[2],
                   hd->sp[0], hd->sp[1], hd->sp[2]);
        v->scene.mon[mo->slot].ik_w[h] = hd->valid ? wt : 0;
        if (!hd->valid)
            continue;
        /* model space: the world less the monster's place, turned back by its heading */
        float d[3] = { hd->sp[0] - mo->mx, hd->sp[1] - mo->my, hd->sp[2] - mo->mz }, r[9], rz[9] = { c, -sn, 0, sn, c, 0, 0, 0, 1 };
        v->scene.mon[mo->slot].ik_pos[h][0] = c * d[0] + sn * d[1];
        v->scene.mon[mo->slot].ik_pos[h][1] = -sn * d[0] + c * d[1];
        v->scene.mon[mo->slot].ik_pos[h][2] = d[2];
        ik_rot_of(hd->sq, r);
        for (int col = 0; col < 3; col++)            /* Rz(-heading) x r */
            for (int k = 0; k < 3; k++)
                v->scene.mon[mo->slot].ik_rot[h][3 * col + k] = rz[k] * r[3 * col] + rz[3 + k] * r[3 * col + 1] + rz[6 + k] * r[3 * col + 2];
    }
}

/* gCommon.addStats's award to the block's challenge (0x346780): the object's template counts */
static void challenge_stats(Viewer *v, const char *name, int points)
{
    if (challenge_add_stats(&v->challenge, name, points) > 0 && getenv("RAMPAGE_TRACE"))
        printf("  challenge: %s counted, %d/%d%c", name, challenge_collected(&v->challenge),
               challenge_required(&v->challenge), 10);
}

/* a monster's damage on the MonsterUpgrade object (StreetObject.bsf TakeDamage: any type, so a blow or its body's
 * touch): once its delay is over, the move is player 1's monster's, whoever touched it (aquireUpgrade 0x2A62B8: into
 * GameStatus +0x234 + 4 x player 1's monster), the "Monster Upgrade" banner */
static void upgrade_touch(Mon *toucher, const float centre[3], float radius)
{
    Viewer *v = toucher->v;
    Mon *mo = &v->mon[0];
    if (v->challenge.up_state != CU_WAITING || mo->monster < 0)
        return;
    uint32_t mv = challenge_upgrade_hit(&v->challenge, centre, radius, &v->upgrades[mo->monster & 63]);
    if (!mv)
        return;
    if (v->fxw && v->upgrade_fx >= 0)
        fx_stop(v->fxw, v->upgrade_fx);
    v->upgrade_fx = -1;
    if (v->hud) {                                    /* gWorld.hideChallengeCompletionUI, then its banner */
        hud_hide_challenge_completion(v->hud);
        hud_banner_text(v->hud, v->challenge.up.text, v->challenge.up.colour, 1, "v_upgrade");
    }
    mo->power.abilities |= mv;
    int pm = profile_find_monster(&v->prof, v->monsters[mo->monster]);
    if (pm >= 0)                                     /* aquireUpgrade: into the profile's monster flags */
        profile_set_monster_flags(&v->prof, 0, pm, profile_monster_flags(&v->prof, 0, pm) | mv);
    if (getenv("RAMPAGE_TRACE"))
        printf("  upgrade taken: %s\n", challenge_upgrade_move(mv));
}

/* gCommon.getMonsterFlags: the moves the profile has for the monster (a new one: none; cheat 011235 all four) */
static void power_from_profile(Mon *mo)
{
    Viewer *v = mo->v;
    int pm = mo->monster >= 0 ? profile_find_monster(&v->prof, v->monsters[mo->monster]) : -1;
    if (pm < 0)
        return;                                      /* not in the roster: edible.c's all four */
    mo->power.abilities = profile_moves(&v->prof, mo->slot, pm);   /* [R Monsters.scr canSpin, hasRoarEnergy:
                                                      * getMonsterFlags(monster.getSlot(), ..): a player's own flags
                                                      * when it has them (player_flags), else the profile's */
    mo->power.cheats = profile_cheat_flags(&v->prof);
    v->upgrades[mo->monster & 63] = profile_monster_flags(&v->prof, 0, pm) & 0xF;
}

/* Start on pad `pad` (0 = player 1): the Pause menu (WorldGuiDriver::process, event 128) */
static int pause_open(Viewer *v, int pad)
{
    HudPause p = { CITIES[v->city_index], v->game ? game_block_name(v->game, 1) : NULL };
    return v->hud && hud_pause_open(v->hud, pad, &p);
}

/* gApp.pauseGame (0x28BC90) pauses the sound events with the world (the Pause menu's, Game Over's): the sounds
 * playing then (the water's, a police car's siren) stop where they are until it lets go; the music plays on */
static void sound_follow_pause(const Viewer *v)
{
    static int held;
    int now = v->hud && hud_paused(v->hud);
    if (now != held)
        sound_pause(now);
    held = now;
}

/* a pad's button went down in the attract mode's demo (RControllerMgr 0x2C1BF4): the game leaves at its next update */
static void attract_press(Viewer *v)
{
    if (v->attract && v->game)
        game_attract_button(v->game);
}

/* this frame's buttons for the demo: any controller's d-pad or button (not the sticks: RControllerMgr's controls 4
 * and on [R 0x2C1BE0]), or a key the viewer gives a button: Enter, Space, Backspace, P, J, K, L, R and the second
 * player's keypad buttons [G: the keys' share] */
static int attract_buttons(const Viewer *v, const Window *w)
{
    static const int KEYS[] = { VK_RETURN, VK_SPACE, VK_BACK, 'P', 'J', 'K', 'L', 'R', VK_NUMPAD0, VK_NUMPAD1,
                                VK_NUMPAD3, VK_NUMPAD7, VK_NUMPAD9 };
    for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++)
        if (w->pressed[KEYS[k]])
            return 1;
    for (int k = 0; v->pads_live && k < PAD_SLOTS; k++)
        if ((v->pads_on >> k & 1) && (v->pads[k].buttons & ~v->pads_prev[k].buttons))
            return 1;
    return 0;
}

/* each monster's clip at its state's time (clip_at), for a posing at time t (the scene's clock); how many have their
 * move graph */
static int pose_monsters(Viewer *v, double t)
{
    int n = 0;
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        Instance *in = mon_in(mo);
        if (!in)
            continue;
        in->offset = (float)((mo->have_graph ? clip_at(mo, &mo->graph.states[mo->run.state], mo->run.time) :
                              mo->run.time) - t);
        n += mo->have_graph;
    }
    return n;
}

/* the game's next block begins: the scene is rebuilt around it, the monster and the cameras carried along (the block
 * left was 41 towards -y of it) */
/* the block left behind, as activateNextBlock tears it down (0x2B66C0 from block_won 0x325924, as the results
 * start) [R]: every vehicle, person, street, parade, rail and army object goes (the monsters' weapons stay, flag
 * 0x200000), the buildings are freed and only their rubble is left (0x302800, 0x303C90), and the next block is
 * loaded. Here the scene's frame stays the old one until the next block begins (GS_BEGIN_BLOCK, the new block at
 * slot 1, +41): its moving objects are freed now, the effects that follow them stopped, its buildings left as
 * rubble, and the traffic of the blocks ahead (slots 1, 2) loaded again. The bosses stay (the ambient ones belong
 * to the blocks behind) [G: the game reloads them with those blocks] */
static void block_left(Viewer *v)
{
    for (int k = 0; k < v->nmon; k++) {
        Mon *mo = &v->mon[k];
        mo->held = mo->held_unit = mo->held_vehicle = mo->held_parade = mo->held_rail = mo->held_sobj = -1;
    }
    /* the block's effects go with its objects and buildings, at once (0x2B66C0 -> 0x292270, 0x302800 -> 0x2BF490 ->
     * 0x310810; 0x207A40) [R]: the army's, the vehicles', the street objects', the parade's, the trains', the edibles',
     * the people's, the scripts' one-shots, the monsters' clip events' and the buildings' debris; not the monster's own,
     * the monsters' weapons', the bosses' (kept here) and the weather (the new block's at once in the game; here the
     * old one until the new block is built) */
    int before = fx_particles(v->fxw);
    fx_drop_block(v->fxw);
    if (getenv("RAMPAGE_TRACE"))
        printf("  block left: its effects dropped, particles %d -> %d\n", before, fx_particles(v->fxw));
    v->nfxa = v->nvfx = v->nsofx = v->npfx = v->nrfx = v->nrsnd = v->nefx = 0;
    for (int k = 0; k < v->nosnd; k++)
        sound_kill(v->osnd[k].h);
    v->nosnd = 0;
    if (v->veh) {
        vehicle_hidden_save(v->veh, &v->hidden);
        vehicle_clear(v->veh);
        for (int k = 1; k < 3; k++)
            if (v->scene.traffic_def[k].at)
                vehicle_load_traffic(v->veh, k, v->scene.traffic_def[k]);
        vehicle_level_start(v->veh, v->hidden_x, &v->hidden);
    }
    if (v->army) {                                   /* its units and shots; the army stays, the scores' way */
        army_clear(v->army);
        army_activate(v->army, 0);
    }
    streetobj_free(v->sobj);
    v->sobj = NULL;
    for (int k = 0; k < SCENE_STREET_MAX; k++)
        v->sobj_of[k] = -1;
    parade_free(v->parade);
    v->parade = v->scene.parade = NULL;
    rail_free(v->rail);
    v->rail = NULL;
    v->scene.rail = NULL;
    edible_free(v->edibles);
    v->edibles = v->scene.edibles = NULL;
    if (v->scene.folk) {
        people_set_world_state(v->scene.folk, -1);   /* (no chains from the teardown) */
        for (int i = 0; i < people_count(v->scene.folk); i++)
            people_remove(v->scene.folk, i);
    }
    scene_block_left(&v->scene);
}

static void game_next_block(Viewer *v)
{
    if (v->game_block < 0)
        return;
    for (int k = 0; k < v->nmon; k++)
        v->mon[k].my -= v->block_shift;
    v->cam.pos[1] -= v->block_shift;
    dircam_shift_y(&v->cd, -v->block_shift);       /* the script camera's places too (ResultsNI's) */
    v->view.pos[1] -= v->block_shift;
    v->block_shift = 0;
    v->block = (uint32_t)v->game_block;
    v->game_block = -1;
    v->game_built = 1;
    build(v);
    v->game_built = 0;
    v->left_block = -1;
}

/* --do: "right 1, cross 0.5, wait 1.5": the stick held one way or a button held down, for that many seconds of play
 * (no seconds: one frame, a tap); "collapse" destroys the building in front of the monster (or under it), as collapseBuilding does;
 * "mirror" flips the mirror state for the next clip; "at x y [heading]" puts the monster there, facing the buildings (or that way);
 * "square+up 0.3": a button pressed with the stick held that way (a throw up, down);
 * "fx name" starts that effect 6 units in front of the monster, 3 up; "rage" fills its energy (the rage starts);
 * "airstrike" starts the time-up airstrike;
 * "monfn name" calls that monster function (doFart, doBarf); "hitcell b floor col [amount]" hits that front cell of
 * building b as an attack of that amount would (2, the roar's 10); "bosshit n" is one blow of n on the block's boss;
 * "spawn name" makes vehicle.c's object of that template (the FireTruck); "soldier name" a window soldier on the front ahead; "unit name" an army unit
 * of that template 2.5 ahead on the street; "strike k" a blow on street object k; "p2" (p1 .. p4): the words after it
 * play that player's monster (the others' keys idle meanwhile, the computer's monsters go on). Once the game goes back
 * to the shell the words stop: the ones left are returned (NULL none), for the shell */
static void attract_press(Viewer *v);
static const char *play_script(Viewer *v, const char *script, float aspect)
{
    Mon *mo = &v->mon[0];                            /* the monster the words play */
    static const struct { const char *name; int button; float x, y; } WORDS[] = {
        { "left", -1, -1, 0 }, { "right", -1, 1, 0 }, { "up", -1, 0, -1 }, { "down", -1, 0, 1 }, { "wait", -1, 0, 0 },
        { "cross", MV_CROSS, 0, 0 }, { "square", MV_SQUARE, 0, 0 }, { "triangle", MV_TRIANGLE, 0, 0 },
        { "circle", MV_CIRCLE, 0, 0 }, { "r2", MV_R2, 0, 0 },
        { "rleft", -2, -1, 0 }, { "rright", -2, 1, 0 }, { "rup", -2, 0, -1 }, { "rdown", -2, 0, 1 },
        { "rcenter", -2, 0, 0 } };
    char word[32];
    const char *p = script;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (v->exit_game)                            /* the shell's turn: the rest is its */
            return *p ? p : NULL;
        int n = 0;
        while (*p && *p != ' ' && *p != ',' && n < 31) word[n++] = *p++;
        word[n] = 0;
        float seconds = 0;
        while (*p == ' ') p++;
        if ((*p >= '0' && *p <= '9') || *p == '.' || *p == '-') {
            char *end;
            seconds = (float)strtod(p, &end);
            p = end;
        }
        if ((word[0] == 'p' || word[0] == 'P') && word[1] >= '1' && word[1] <= '4' && !word[2]) {   /* p2: player 2 */
            int k = word[1] - '1';
            if (k < v->nmon)
                mo = &v->mon[k];
            else
                printf("no player %d\n", k + 1);
            continue;
        }
        if (_stricmp(word, "mirror") == 0) {         /* the next clip mirrored, as an event of type 8 would */
            mo->mirror_queued = !mo->mirror_queued;
            continue;
        }
        if (_stricmp(word, "at") == 0) {             /* at x y: the monster stands there, facing the buildings */
            char *end;
            mo->mx = seconds;
            mo->my = (float)strtod(p, &end);
            p = end;
            mo->mz = 0;
            mo->heading = 0;
            while (*p == ' ') p++;
            if ((*p >= '0' && *p <= '9') || *p == '-') {   /* at x y heading */
                mo->heading = (float)strtod(p, &end);
                p = end;
            }
            continue;
        }
        if (_stricmp(word, "fx") == 0) {             /* fx name: an effect in front of the monster */
            char name[64];
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 63) name[k++] = *p++;
            name[k] = 0;
            float h = mo->heading / 57.2957795f, at[3] = { mo->mx - sinf(h) * 6, mo->my + cosf(h) * 6, mo->mz + 3 };
            char *end;                               /* (fx name x y z: there) */
            for (int c = 0; c < 3; c++) {
                float f = strtof(p, &end);
                if (end == p)
                    break;
                at[c] = f;
                p = end;
            }
            if (v->fxw && fx_spawn(v->fxw, name, at, NULL, 1) < 0)
                printf("no effect %s\n", name);
            continue;
        }
        if (_stricmp(word, "airstrike") == 0) {      /* the time-up airstrike now, as if the clock were out */
            v->force_strike = 1;
            continue;
        }
        if (_stricmp(word, "rage") == 0) {           /* the energy full through addStats: the rage starts */
            monster_stats(mo, 0, 100);
            continue;
        }
        if (_stricmp(word, "eatperson") == 0) {      /* eatperson name: the first living person of that template
                                                      * grabbed and eaten (as the monster's grab and its eat) */
            char name[64];
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 63) name[k++] = *p++;
            name[k] = 0;
            for (int who = 0; v->scene.folk && mo->held < 0 && who < people_count(v->scene.folk); who++) {
                const Person *q = people_get(v->scene.folk, who);
                if (q->phase != PP_GONE && q->name && _stricmp(q->name, name) == 0) {
                    people_grab(v->scene.folk, who, 0.1f);
                    mo->held = who;
                    throw_held(mo, 1, 0, 0, 0);
                }
            }
            continue;
        }
        if (_stricmp(word, "hurt") == 0) {           /* hurt amount: TakeDamage on the monster (no knock), its level
                                                      * by the amount */
            float knock[3] = { 0, 0, 0 }, from[3] = { mo->mx, mo->my - 5, mo->mz };
            monster_hit(mo, "hurt", seconds, take_damage_level(mo, seconds), knock, from);
            continue;
        }
        if (_stricmp(word, "monfn") == 0) {          /* monfn name: callMonsterFunction (doFart, doBarf) */
            char name[64];
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 63) name[k++] = *p++;
            name[k] = 0;
            mon_function(mo, name);
            continue;
        }
        if (_stricmp(word, "award") == 0) {          /* award name: gCommon.addStats of an object of that template */
            char name[64];
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 63) name[k++] = *p++;
            name[k] = 0;
            challenge_stats(v, name, 0);
            continue;
        }
        if (_stricmp(word, "spawn") == 0) {          /* spawn name: vehicle.c's object of that template 6 in front of
                                                      * the monster (FireTruck, which no block spawns, places itself) */
            static char name[64];                    /* the object keeps the name */
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 63) name[k++] = *p++;
            name[k] = 0;
            float h = mo->heading / 57.2957795f, at[3] = { mo->mx - sinf(h) * 6, mo->my + cosf(h) * 6, 0 };
            if (!v->veh || vehicle_spawn(v->veh, name, at, h, -1) < 0)
                printf("no vehicle %s\n", name);
            continue;
        }
        if (_stricmp(word, "strike") == 0) {         /* strike k: a blow (an attack record of its own) on street
                                                      * object k (RAMPAGE_STREET lists them), from the monster */
            const StreetObj *o = v->sobj ? streetobj_get(v->sobj, (int)seconds) : NULL;
            static unsigned rec = 0x7F000000;
            if (o) {
                StreetMonster me = { { mo->mx, mo->my, mo->mz }, mo->heading / 57.2957795f,
                                     mo->graph.states[mo->run.state].name };
                float c[3];
                for (int k = 0; k < 3; k++)
                    c[k] = o->world[12 + k] + (o->box_min[k] + o->box_max[k]) / 2;
                streetobj_hit(v->sobj, mo->slot, &me, c, 0.5f, 20, rec++);
            } else
                printf("no street object %d\n", (int)seconds);
            continue;
        }
        if (_stricmp(word, "unit") == 0) {           /* unit name: an army unit of that template 2.5 in front of the
                                                      * monster, on the street, facing it */
            static char uname[64];                   /* the unit keeps the name */
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 63) uname[k++] = *p++;
            uname[k] = 0;
            float h = mo->heading / 57.2957795f, at[3] = { mo->mx - sinf(h) * 2.5f, mo->my + cosf(h) * 2.5f, 0 };
            if (!v->army || army_spawn(v->army, uname, at, h + 3.14159265f) < 0)
                printf("no unit %s\n", uname);
            continue;
        }
        if (_stricmp(word, "soldier") == 0) {        /* soldier name: a window soldier of that template on the front
                                                      * of the nearest building ahead of the monster, 9 up */
            static char name[64];                    /* the unit keeps the name */
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 63) name[k++] = *p++;
            name[k] = 0;
            const Building *front = NULL;
            for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
                const Building *b = &v->scene.buildings[i].b;
                if (mo->mx >= b->min[0] && mo->mx <= b->max[0] && mo->my < b->min[1] &&
                    (!front || b->min[1] < front->min[1]))
                    front = b;
            }
            float at[3] = { mo->mx, front ? front->min[1] : 0, 9 };
            int u = front && v->army ? army_spawn_window(v->army, name, at) : -1;
            if (u < 0)
                printf("no soldier %s\n", name);
            else if (getenv("RAMPAGE_TRACE"))
                printf("soldier %s (unit %d) at %.1f %.1f %.1f\n", name, u, at[0], at[1], at[2]);
            continue;
        }
        if (_stricmp(word, "pause") == 0) {          /* Start: the Pause menu (the demo: its end) */
            attract_press(v);
            pause_open(v, 0);
            continue;
        }
        if (_stricmp(word, "menu") == 0) {           /* menu up / down / left / right / accept / back / start, b0 /
                                                      * b1 (the Results' Change Monster / Save) */
            static const struct { const char *name; int ev, button; } EV[] = {
                { "up", HUD_EV_UP, 0 }, { "down", HUD_EV_DOWN, 0 }, { "left", HUD_EV_LEFT, 0 },
                { "right", HUD_EV_RIGHT, 0 }, { "accept", HUD_EV_ACCEPT, 0 }, { "back", HUD_EV_BACK, 0 },
                { "start", HUD_EV_START, 0 }, { "b0", HUD_EV_BUTTON, 0 }, { "b1", HUD_EV_BUTTON, 1 } };
            char name[16];
            int k = 0;
            while (*p == ' ') p++;
            while (*p && *p != ' ' && *p != ',' && k < 15) name[k++] = *p++;
            name[k] = 0;
            for (size_t i = 0; i < sizeof EV / sizeof *EV; i++)
                if (_stricmp(name, EV[i].name) == 0) {
                    if (shell_popup_active(v->popup))    /* isSaveGameActive: the popup has the pad */
                        shell_input(v->popup, EV[i].ev, EV[i].button);
                    else if (v->hud)
                        hud_input(v->hud, EV[i].ev, EV[i].button);
                }
            continue;
        }
        if (_stricmp(word, "bosshit") == 0) {        /* bosshit n: one blow of n (1 without) on the block's boss, by
                                                      * this monster, as an armed attack sphere at its middle would */
            const BossPart *bp = v->boss ? boss_part(v->boss, boss_main(v->boss)) : NULL;
            static unsigned record = 0x40000000u;    /* a record of its own each time (one blow a record) */
            if (bp && bp->alive)
                boss_hit(v->boss, mo->slot, bp->pos, 1, seconds > 0 ? seconds : 1, record++);
            else
                printf("no boss to hit\n");
            continue;
        }
        if (_stricmp(word, "collapseall") == 0) {    /* every building of the block falls (the end of a block) */
            for (uint32_t i = 0; i < v->scene.nbuildings; i++)
                building_destroy(&v->scene.buildings[i].b);
            continue;
        }
        if (_stricmp(word, "hitcell") == 0) {        /* hitcell b floor col: a hit on that front cell (player 1's) */
            char *end;
            int bi = (int)seconds, o = (int)strtol(p, &end, 10);
            p = end;
            int col = (int)strtol(p, &end, 10);
            p = end;
            float amount = strtof(p, &end);          /* an amount after them (the roar's 10), else 2 */
            if (end != p)
                p = end;
            else
                amount = 2.0f;
            if (bi >= 0 && (uint32_t)bi < v->scene.nbuildings) {
                SceneBuilding *sb = &v->scene.buildings[bi];
                float w = (sb->b.max[0] - sb->b.min[0]) / (sb->b.width > 0 ? sb->b.width : 1);
                /* the cell's middle: the ground floor 6 high, the storeys 3 (building.c) */
                float at[3] = { sb->b.min[0] + w * ((float)col + 0.5f), sb->b.min[1], 6.0f + 3.0f * ((float)o + 0.5f) };
                if (sb->b.state == BUILDING_STANDING)
                    sb->hitter = 0;
                int hit = building_hit(&sb->b, at, amount);   /* (a monster attack's radius) */
                building_script(v, sb, amount);
                if (getenv("RAMPAGE_TRACE"))
                    printf("  hitcell %d %d %d at %.1f %.1f %.1f: %s (%d storeys, %.1f .. %.1f)\n", bi, o, col, at[0],
                           at[1], at[2], hit ? "hit" : "no damage", sb->b.n, sb->b.min[2], sb->b.max[2]);
            }
            continue;
        }
        if (_stricmp(word, "collapse") == 0) {       /* collapseBuilding on the building in front of the monster, or
                                                      * the one it stands on */
            for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
                Building *b = &v->scene.buildings[i].b;
                if (mo->mx >= b->min[0] && mo->mx <= b->max[0] && mo->my < b->max[1] && mo->my > b->min[1] - 10)
                    building_destroy(b);
            }
            continue;
        }
        char *plus = strchr(word, '+');              /* button+direction: the stick held with it */
        float sx = 0, sy = 0;
        if (plus) {
            *plus = 0;
            for (size_t i = 0; i < sizeof WORDS / sizeof *WORDS; i++)
                if (WORDS[i].button == -1 && _stricmp(plus + 1, WORDS[i].name) == 0) {
                    sx = WORDS[i].x;
                    sy = WORDS[i].y;
                }
        }
        for (size_t i = 0; n && i < sizeof WORDS / sizeof *WORDS; i++) {
            if (_stricmp(word, WORDS[i].name) != 0)
                continue;
            uint8_t pressed[MV_COUNT] = { 0 }, held[MV_COUNT] = { 0 };
            if (WORDS[i].button >= 0) pressed[WORDS[i].button] = 1;
            int frames = (int)(seconds * 60 + 0.5f);
            if (frames < 1) frames = 1;
            if (WORDS[i].button >= 0)                /* a button: the demo's end */
                attract_press(v);
            for (int k = 0; k < frames && !v->exit_game; k++) {
                int right = WORDS[i].button == -2;  /* the right stick: held from here until rcenter */
                if (right) {
                    mo->pad.rx = WORDS[i].x;
                    mo->pad.ry = WORDS[i].y;
                }
                for (int j = 0; j < v->nmon; j++)    /* the keys' pads: this monster's from the words, the rest idle */
                    if (v->mon[j].drive != DRIVE_COMPUTER) {
                        Pad *pd = &v->mon[j].pad;
                        float rx = pd->rx, ry = pd->ry;
                        memset(pd, 0, sizeof *pd);
                        pd->rx = rx;
                        pd->ry = ry;
                    }
                mo->pad.lx = right ? 0 : WORDS[i].x + sx;
                mo->pad.ly = right ? 0 : WORDS[i].y + sy;
                memcpy(mo->pad.pressed, pressed, sizeof pressed);
                memcpy(mo->pad.held, held, sizeof held);
                play_step(v, 1.0f / 60, aspect);
                if (pose_monsters(v, 0)) {          /* posed each frame, as the main loop does */
                    scene_update(&v->scene, 0);
                    hit_spheres_all(v);
                }
                if (WORDS[i].button >= 0)
                    held[WORDS[i].button] = 1;
                memset(pressed, 0, sizeof pressed);
                game_next_block(v);                  /* the game moved on, as the main loop does */
            }
        }
    }
    return NULL;
}

/* ---- the front end (shell.c): --shell starts there, and a game's Quit goes back */

/* the profile as the shell reads it */
static void shell_profile(Viewer *v, ShellProfile *sp)
{
    memset(sp, 0, sizeof *sp);
    sp->loaded = v->prof.loaded;
    sp->allow_save = v->prof.allow_save && v->prof_write;
    sp->was_saving = v->prof.was_saving;
    sp->language = v->lang;
    sp->region = v->region;
    sp->profile_shown = v->prof.profile_shown;
    sp->offer_create = v->profile_autoload && !v->prof_on_card && v->prof_write;
    snprintf(sp->name, sizeof sp->name, "%s", v->prof.block.name);
    for (int p = 0; p < 4; p++)
        for (int m = 0; m < SHELL_MONSTERS; m++)
            sp->monster_flags[p][m] = (uint8_t)profile_monster_flags(&v->prof, p, m);
    for (int c = 0; c < SHELL_CITIES; c++) {
        sp->city_flags[c] = (uint8_t)profile_city_flags(&v->prof, c);
        sp->start_block[c] = profile_start_block(&v->prof, c);
    }
    sp->sound = profile_sound_volume(&v->prof);
    sp->music = profile_music_volume(&v->prof);
    sp->rumble = profile_rumble(&v->prof);
    sp->cheats = profile_cheat_flags(&v->prof);
    sp->changed = profile_needs_save(&v->prof);
    sp->nprofiles = v->prof_on_card ? 1 : 0;         /* one profile file: the "card" holds it or nothing */
    if (sp->nprofiles)
        snprintf(sp->profiles[0], sizeof sp->profiles[0], "%s", v->card_name[0] ? v->card_name : "Profile");
}

/* ---- the front end's 3D objects (shell_3d): each shape's model from its interface's bundle, every mesh on its node at
 * the rest pose (the RawAnim of the model's name: the globe's is one key); each effect played by fx.c over that bundle
 * (then fx.bdp), one FxWorld per bundle, the GuiEffect's instance its own [R 0x2377E0: not in the world's list] */
typedef struct {
    const Bundle *b; char name[40];
    Tree tree; Mat4 *node;
    SceneMesh mesh[24]; int mesh_node[24], nmesh;
} GuiModel;
typedef struct { const Bundle *b; FxLib *lib; FxWorld *w; } GuiFxLib;
typedef struct { const void *key; int gen, lib, fx, started, stopped; } GuiFx;
#define GUI_ITEMS 64
typedef struct GuiDraw {
    GuiModel model[4]; int nmodel;
    GuiFxLib lib[4]; int nlib;
    GuiFx fx[48]; int nfx;
    Shell3D item[GUI_ITEMS]; int nitem;
    SceneGui3D layer[GUI_ITEMS]; SceneGuiMesh mesh[256]; FxQuad quad[6144];
} GuiDraw;

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static GuiModel *gui_model(GuiDraw *g, const Bundle *b, const char *file)
{
    char key[40];
    snprintf(key, sizeof key, "%s", file ? file : "");
    size_t n = strlen(key);
    if (n > 4 && !_stricmp(key + n - 4, ".pwk"))
        key[n - 4] = 0;
    for (int i = 0; i < g->nmodel; i++)
        if (g->model[i].b == b && !_stricmp(g->model[i].name, key))
            return &g->model[i];
    if (!b || !key[0] || g->nmodel >= 4)
        return NULL;
    GuiModel *m = &g->model[g->nmodel++];
    memset(m, 0, sizeof *m);
    m->b = b;
    snprintf(m->name, sizeof m->name, "%s", key);
    const BdpEntry *te = NULL, *raw = NULL;
    for (uint32_t i = 0; i < b->nentries; i++) {
        const BdpEntry *e = &b->entries[i];
        if (e->type == ET_TREE && !_stricmp(bdp_name(b, e->name), key))
            te = e;
        else if (e->type == ET_ANIM && e->size >= 0x44 && le32(e->data + 0x20) == 1 &&
                 !_stricmp(bdp_name(b, (uint32_t)(e->data[0x28] | e->data[0x29] << 8)), key))
            raw = e;                                 /* its RawAnim: the rest pose */
    }
    if (te && tree_read(b, te, raw, &m->tree) == 0 && m->tree.n) {
        m->node = calloc(m->tree.n, sizeof *m->node);
        for (uint32_t i = 0; m->node && i < m->tree.n; i++) {
            const TreeNode *nd = &m->tree.nodes[i];
            Mat4 local = mat_tq(nd->t, nd->q);
            m->node[i] = nd->parent >= 0 && (uint32_t)nd->parent < i ? mat_mul(m->node[nd->parent], local) : local;
        }
    }
    for (uint32_t i = 0; i < b->nentries && m->nmesh < 24; i++) {   /* <model>|<object> on the node <object> */
        const BdpEntry *e = &b->entries[i];
        const char *en = bdp_name(b, e->name);
        size_t kl = strlen(key);
        if (e->type != ET_MESH || _strnicmp(en, key, kl) || en[kl] != '|')
            continue;
        SceneMesh *sm = &m->mesh[m->nmesh];
        memset(sm, 0, sizeof *sm);
        if (mesh_read(e, &sm->mesh) != 0)
            continue;
        sm->b = b;
        m->mesh_node[m->nmesh] = -1;
        for (uint32_t k = 0; m->node && k < m->tree.n; k++)
            if (!_stricmp(m->tree.nodes[k].name, en + kl + 1))
                m->mesh_node[m->nmesh] = (int)k;
        m->nmesh++;
    }
    return m;
}

static int gui_lib(Viewer *v, GuiDraw *g, const Bundle *b)
{
    for (int i = 0; i < g->nlib; i++)
        if (g->lib[i].b == b)
            return g->lib[i].w ? i : -1;
    if (!b || g->nlib >= 4)
        return -1;
    GuiFxLib *l = &g->lib[g->nlib];
    const Bundle *bl[2] = { b, &v->fx_bundle };
    l->b = b;
    l->lib = v->data_path[0] ? fx_open(v->data_path, bl, v->have_fx_bundle ? 2 : 1) : NULL;
    l->w = l->lib ? fx_world(l->lib) : NULL;
    return l->w ? g->nlib++ : (g->nlib++, -1);
}

static void gui3d_free(Viewer *v)
{
    GuiDraw *g = v->gd;
    if (!g)
        return;
    for (int i = 0; i < g->nmodel; i++) {
        for (int k = 0; k < g->model[i].nmesh; k++)
            mesh_free(&g->model[i].mesh[k].mesh);
        tree_free(&g->model[i].tree);
        free(g->model[i].node);
    }
    for (int i = 0; i < g->nlib; i++) {
        fx_world_free(g->lib[i].w);
        fx_close(g->lib[i].lib);
    }
    free(g);
    v->gd = NULL;
}

/* where an effect is: its world, or on its shape's node (the node's frame x the shape's world) */
static Mat4 gui_effect_world(GuiDraw *g, const Shell3D *it)
{
    Mat4 w;
    memcpy(w.m, it->world, sizeof w.m);
    if (it->attach && it->parent >= 0 && it->parent < g->nitem) {
        const Shell3D *sh = &g->item[it->parent];
        GuiModel *m = gui_model(g, sh->b, sh->name);
        for (uint32_t k = 0; m && m->node && k < m->tree.n; k++)
            if (!_stricmp(m->tree.nodes[k].name, it->attach)) {
                Mat4 sw;
                memcpy(sw.m, sh->world, sizeof sw.m);
                w = mat_mul(sw, m->node[k]);
                break;
            }
    }
    return w;
}

/* the effects follow their GuiEffects: one running is started (once) and kept where it is drawn, one that stops
 * running is shut down, one whose edf ended says so; then every effect world steps */
static void gui3d_step(Viewer *v, Shell *sh, float dt)
{
    if (!sh)
        return;
    if (!v->gd && !(v->gd = calloc(1, sizeof *v->gd)))
        return;
    GuiDraw *g = v->gd;
    g->nitem = shell_3d(sh, g->item, GUI_ITEMS);
    for (int i = 0; i < g->nitem; i++) {
        const Shell3D *it = &g->item[i];
        if (it->kind != SHELL_3D_EFFECT)
            continue;
        GuiFx *x = NULL;
        for (int k = 0; k < g->nfx && !x; k++)
            if (g->fx[k].key == it->key)
                x = &g->fx[k];
        if (!x) {
            if (g->nfx >= 48)
                continue;
            x = &g->fx[g->nfx++];
            memset(x, 0, sizeof *x);
            x->key = it->key;
            x->gen = it->gen;
            x->fx = -1;
            x->lib = gui_lib(v, g, it->b);
        }
        FxWorld *w = x->lib >= 0 ? g->lib[x->lib].w : NULL;
        if (!w) {                                    /* no effects here: one stopping is over at once */
            if (it->stopping)
                shell_effect_ended(sh, it->key);
            continue;
        }
        if (x->gen != it->gen) {                     /* setEffect: the old one is freed */
            fx_kill(w, x->fx);
            x->fx = -1;
            x->started = 0;
            x->gen = it->gen;
        }
        Mat4 m = gui_effect_world(g, it);
        float pos[3] = { m.m[12], m.m[13], m.m[14] }, R[3][3], scale = 0;
        for (int r = 0; r < 3; r++) {
            float l = sqrtf(m.m[4 * r] * m.m[4 * r] + m.m[4 * r + 1] * m.m[4 * r + 1] + m.m[4 * r + 2] * m.m[4 * r + 2]);
            if (r == 0) scale = l;
            for (int c = 0; c < 3; c++)
                R[r][c] = l > 0 ? m.m[4 * r + c] / l : (r == c);
        }
        if (it->running) {
            if (!x->started) {                       /* the frame's scale scales the effect's places, not its
                                                      * billboards (0x2376E0 -> +0x6C, kept at +0x2C) [R] */
                x->fx = fx_spawn_frame(w, it->name, pos, R, scale > 0 ? scale : 1);
                x->started = 1;
                x->stopped = 0;
                if (getenv("RAMPAGE_TRACE"))
                    printf("  gui effect %s at %.2f %.2f %.2f%s\n", it->name, pos[0], pos[1], pos[2],
                           x->fx < 0 ? ": none" : "");
                if (x->fx < 0)
                    shell_effect_ended(sh, it->key);
            } else if (fx_alive(w, x->fx)) {
                if (it->stopping && !x->stopped) {   /* stopEffect: its particles play out */
                    fx_stop(w, x->fx);
                    x->stopped = 1;
                }
                if (it->layer >= 0)
                    fx_place(w, x->fx, pos, R);
            } else {                                 /* it is over */
                shell_effect_ended(sh, it->key);
                x->started = 0;
                x->fx = -1;
            }
        } else if (x->started) {
            fx_kill(w, x->fx);
            x->fx = -1;
            x->started = 0;
        }
    }
    for (int i = 0; i < g->nlib; i++) {              /* the adapter's step: the world's in play (the popup), the front
                                                      * end's 1 / the display rate there [R 0x2852E0, 0x309210] */
        fx_set_step(g->lib[i].w, sh == v->popup ? TIMESTEP : 1.0f / 60);
        fx_update(g->lib[i].w, dt, -1e9f);
    }
}

/* this frame's 3D layers for the renderer, one per HUD_TEX_3D quad */
/* the wider window [no PS2 meaning]: an interface's own effect (MainMenu's two fires, which lean left with their
 * smoke: on the PS2 the screen's left edge cuts the drift) fades out past the screen's edges, alike each side: a
 * particle at the edge as it is, gone 40 of the screen's pixels (a sixteenth) out, by a smoothstep on the place it
 * is drawn at (the GUI camera: eye (0, -1, 0) along +y, tan(fov / 2) = 0.375, so x pixels = 320 + 640 x / (y + 1)).
 * Alpha blended: its alpha; additive: its colour too */
static void fade_sides(FxQuad *q, int n, float side)
{
    const float out = fminf(40.0f, side);
    for (int i = 0; i < n; i++) {
        float d = q[i].pos[1] + 1;
        if (d <= 0.01f)
            continue;
        float px = 320.0f + 640.0f * q[i].pos[0] / d, past = px < 0 ? -px : px > 640 ? px - 640 : 0;
        if (past <= 0)
            continue;
        float u = fminf(past / out, 1), k = 1 - u * u * (3 - 2 * u);
        q[i].col[3] *= k;
        if (q[i].additive)
            for (int c = 0; c < 3; c++)
                q[i].col[c] *= k;
    }
}

static void gui3d_layers(Viewer *v, Shell *sh, Scene *s)
{
    s->gui3d = NULL;
    s->ngui3d = 0;
    if (!sh)
        return;
    if (!v->gd && !(v->gd = calloc(1, sizeof *v->gd)))
        return;
    GuiDraw *g = v->gd;
    g->nitem = shell_3d(sh, g->item, GUI_ITEMS);
    int nl = 0, nm = 0, nq = 0;
    for (int i = 0; i < g->nitem; i++) {
        const Shell3D *it = &g->item[i];
        if (it->layer < 0)
            continue;
        while (nl <= it->layer && nl < GUI_ITEMS)
            memset(&g->layer[nl++], 0, sizeof g->layer[0]);
        if (it->layer >= GUI_ITEMS)
            continue;
        SceneGui3D *l = &g->layer[it->layer];
        if (it->kind == SHELL_3D_SHAPE) {
            GuiModel *m = gui_model(g, it->b, it->name);
            l->meshes = &g->mesh[nm];
            for (int k = 0; m && k < m->nmesh && nm < 256; k++) {
                Mat4 w;
                memcpy(w.m, it->world, sizeof w.m);
                if (m->mesh_node[k] >= 0)
                    w = mat_mul(w, m->node[m->mesh_node[k]]);
                g->mesh[nm].mesh = &m->mesh[k];
                g->mesh[nm++].world = w;
                l->nmeshes++;
            }
            l->lit = it->lit;
            memcpy(l->ambient, it->ambient, sizeof l->ambient);
            l->nlights = it->nlights;
            memcpy(l->light_dir, it->light_dir, sizeof l->light_dir);
            memcpy(l->light_diffuse, it->light_diffuse, sizeof l->light_diffuse);
            memcpy(l->light_ambient, it->light_ambient, sizeof l->light_ambient);
        } else {
            for (int k = 0; k < g->nfx; k++) {
                const GuiFx *x = &g->fx[k];
                if (x->key != it->key || x->lib < 0)
                    continue;
                l->fx = &g->quad[nq];
                FxWorld *w = g->lib[x->lib].w;
                if (x->fx >= 0)
                    nq += fx_quads_of(w, x->fx, g->quad + nq, 6144 - nq);
                l->nfx = (int)(&g->quad[nq] - l->fx);
                if (it->sides && v->gui_side > 0)
                    fade_sides(g->quad + (l->fx - g->quad), l->nfx, v->gui_side);
            }
        }
    }
    s->gui3d = g->layer;
    s->ngui3d = nl;
}

static void popup_free(Viewer *v);
static void shell_enter(Viewer *v, const ShellStart *st)
{
    popup_free(v);                                   /* (before the textures are dropped: its bundle goes) */
    sound_effect_stop_all();                         /* [R] the package changes: the scripts' SoundEffects stop
                                                      * (0x28BE1C) */
    ShellProfile sp;
    shell_profile(v, &sp);
    v->shell = shell_open(&v->vol, st, &sp);
    v->card_poll = 0;
    static Scene empty;                              /* nothing but the screens, black around their 4:3 */
    memset(&empty, 0, sizeof empty);
    empty.fog_end = 1;
    empty.hud_clip = 1;                              /* Shell's backgrounds overhang x 0 (MonsterSelect's left tiles) */
    v->shell_scene = &empty;
    if (v->shell)
        rnd_scene(v->rnd, v->shell_scene);           /* (the texture cache is keyed by the bundles: anew) */
}

/* the movie's sound, pulled by the mixer */
static int movie_pull(void *ctx, int16_t *out, int frames) { return movie_audio(ctx, out, frames); }

static void movie_stop(Viewer *v)
{
    if (!v->movie)
        return;
    sound_stream(NULL, NULL);
    rnd_video(v->rnd, NULL, 0, 0);
    movie_close(v->movie);
    v->movie = NULL;
    if (v->shell)
        shell_movie_picture(v->shell, 0);
}

/* SHELL_EVT_MOVIE: playVideo(name), the file by the game's pick (movie.c): the European disc's on a PAL console, at
 * 50 Hz (its flags +0x50 = 0xC never ask for 60, 0x169020): <name>_<lang>_pal, else <name>_pal; the USA disc's on an
 * NTSC console, at 60 Hz: <name>_<lang>, else <name>; the language the .loc column (hud.h's + 1) */
static void movie_start(Viewer *v, const char *name)
{
    char picked[96];
    movie_stop(v);
    v->movie = movie_pick(&v->vol, name, v->lang + 1, v->region != 0, picked, sizeof picked);
    if (!v->movie) {
        printf("no movie %s\n", name);
        return;
    }
    shell_movie_hold(v->shell);
    sound_stream(movie_pull, v->movie);
    if (getenv("RAMPAGE_TRACE"))
        printf("  movie %s (%s)\n", name, picked);
}

/* the movie at the shell's time: its picture to the Video quad; at its end the shell goes on; skipped: stopped */
static void movie_step(Viewer *v)
{
    if (!v->movie)
        return;
    float t = v->shell ? shell_movie_clock(v->shell) : -1;
    if (t < 0) {
        movie_stop(v);
        return;
    }
    int w, h, changed;
    const uint8_t *pic = movie_video(v->movie, t, &changed);
    movie_size(v->movie, &w, &h);
    if (pic) {
        if (changed)
            rnd_video(v->rnd, pic, w, h);
        shell_movie_picture(v->shell, 1);
    }
    if (movie_done(v->movie, t)) {
        if (getenv("RAMPAGE_TRACE"))
            printf("  movie over: %d pictures\n", movie_pictures(v->movie));
        shell_movie_end(v->shell);
    }
}

/* the SaveGame popup's card jobs on the profile's file (shell.h SHELL_CARD_*): the read of the list's profile (a
 * missing file NOT_FOUND, a bad one CORRUPT; the profile is kept unless it was read), the write of a new one (the
 * name entry; written before it becomes the profile) and of the profile as it is; each answered at once. A run
 * that writes nothing (a --shot run without --profile) answers OK [G: as a card that took it] */
static int card_status(int written) { return written >= 0 ? SHELL_CARD_OK : written == -2 ? SHELL_CARD_NO_SPACE :
                                             SHELL_CARD_IO; }
static int card_job(Viewer *v, Shell *sh, const ShellEvent *e)
{
    ShellProfile sp;
    int st = SHELL_CARD_OK;
    uint64_t now = (uint64_t)time(NULL) * 1000;
    if (e->kind == SHELL_EVT_PROFILE_LOAD) {         /* the one profile file */
        Profile p = v->prof;
        int r = v->prof_write ? profile_read(&p, v->prof_path) : -1;
        st = r > 0 ? SHELL_CARD_OK : r < 0 ? SHELL_CARD_NOT_FOUND : SHELL_CARD_CORRUPT;
        if (st == SHELL_CARD_OK) {
            v->prof = p;
            v->prof.loaded = 1;
            v->prof.allow_save = v->prof.was_saving = 1;   /* [R 0x34559C: close(1) in op 3] */
            profile_restore_checkpoint(&v->prof);
        }
    } else if (e->kind == SHELL_EVT_PROFILE_CREATE) {
        Profile p = v->prof;
        profile_create(&p, e->name);
        p.loaded = 1;
        if (v->prof_write)
            st = card_status(profile_write(&p, v->prof_path, 1, now));
        if (st == SHELL_CARD_OK) {
            v->prof = p;
            if (v->prof_write) {
                v->prof_on_card = 1;
                snprintf(v->card_name, sizeof v->card_name, "%s", v->prof.block.name);
            }
        }
    } else if (e->kind == SHELL_EVT_PROFILE_SAVE) {  /* saveProfile's write (0x33F230 clears the changed bytes) */
        if (v->prof_write)
            st = card_status(profile_write(&v->prof, v->prof_path, 1, now));
        if (st == SHELL_CARD_OK)
            v->prof.was_saving = 1;                  /* [R 0x34559C: close(1) in op 7 sets 4 and 8] */
    } else
        return 0;
    if (st != SHELL_CARD_OK)
        printf("the profile %s: %s\n", v->prof_path, st == SHELL_CARD_NO_SPACE ? "no room to write it" :
               st == SHELL_CARD_IO ? "cannot write it" : st == SHELL_CARD_NOT_FOUND ? "not found" : "corrupt");
    shell_profile(v, &sp);
    shell_set_profile(sh, &sp);
    shell_card_result(sh, st);
    return 1;
}

/* gCommon.saveProfile (0x33E2D0) from the game's screens: nothing unless the profile changed, then the SaveGame popup
 * (opened over the screens once, kept for the game), which writes only with autosave on [R 0x345950] */
static void save_profile(Viewer *v)
{
    if (!profile_needs_save(&v->prof) || !v->prof.allow_save || !v->prof_write)
        return;
    ShellProfile sp;
    shell_profile(v, &sp);
    if (!v->popup && !v->shell)
        v->popup = shell_open_popup(&v->vol, v->have_gui ? &v->gui_common : NULL, &sp);
    Shell *sh = v->shell ? v->shell : v->popup;
    if (!sh) {                                       /* no MemoryCard.bdp: written at once */
        if (profile_write(&v->prof, v->prof_path, 0, (uint64_t)time(NULL) * 1000) < 0)
            printf("could not write the profile %s\n", v->prof_path);
        return;
    }
    shell_set_profile(sh, &sp);
    shell_save_profile(sh);
}

static void popup_free(Viewer *v)
{
    if (!v->popup)
        return;
    gui3d_free(v);
    shell_close(v->popup);
    v->popup = NULL;
}

/* the game's popup each frame: its time, its sounds and card jobs, its effects; the Results wait while it is up
 * (isSaveGameActive) */
static void popup_step(Viewer *v, float dt)
{
    if (!v->popup)
        return;
    shell_update(v->popup, dt);
    gui3d_step(v, v->popup, dt);
    ShellEvent ev[16];
    int n;
    while ((n = shell_events(v->popup, ev, 16)) > 0)
        for (int i = 0; i < n; i++) {
            if (ev[i].kind == SHELL_EVT_SOUND)
                sound_play(ev[i].name, NULL, 1);
            else if (ev[i].kind == SHELL_EVT_PROFILE_NONE) {   /* warn-continue's Don't Save: autosave off */
                v->prof.allow_save = v->prof.was_saving = 0;
                hud_set_allow_save(v->hud, 0);
            }
            else
                card_job(v, v->popup, &ev[i]);
        }
    if (v->hud)
        hud_set_save_active(v->hud, shell_popup_active(v->popup));
}

/* what the shell asked for since the last call; 1 when a game is to start */
static int shell_answer(Viewer *v)
{
    ShellEvent ev[32];
    int n, start = 0;
    float sv, mv;
    int rumble;
    while ((n = shell_events(v->shell, ev, 32)) > 0)
        for (int i = 0; i < n; i++) {
            const ShellEvent *e = &ev[i];
            if (getenv("RAMPAGE_TRACE"))
                printf("  shell event %d (%s) %d %.2f\n", e->kind, e->name, e->arg, e->value);
            switch (e->kind) {
            case SHELL_EVT_SOUND: sound_play(e->name, NULL, 1); break;
            case SHELL_EVT_MUSIC:
                if (e->name[0])
                    music_replace(e->name, 0.5f, 0.5f);
                else
                    music_stop(0.5f);
                break;
            case SHELL_EVT_MUSIC_VOLUME: v->music_now = e->value / 0.75f; sound_settings(v->sfx_vol * 0.75f, v->music_now * 0.75f, 0); break;
            case SHELL_EVT_SFX_VOLUME: v->sfx_vol = e->value / 0.75f; sound_settings(v->sfx_vol * 0.75f, v->music_now * 0.75f, 0); break;
            case SHELL_EVT_RUMBLE: v->vibrate = profile_set_rumble(&v->prof, e->arg != 0, 0); break;
            case SHELL_EVT_RUMBLE_PULSE:             /* gCommon.rumble(ms): pad 0 [R 0x2C34C8] */
                if (v->pads_live && v->vibrate)
                    pad_rumble(0, 0xC0 / 255.0f, e->arg / 1000.0f);
                break;
            case SHELL_EVT_SAVE_SETTINGS:            /* Options' Accept: into the profile, saved */
                shell_settings(v->shell, &sv, &mv, &rumble);
                profile_set_sound_volume(&v->prof, sv);
                profile_set_music_volume(&v->prof, mv);
                v->vibrate = profile_set_rumble(&v->prof, rumble, 1);
                v->sfx_vol = sv / 0.75f;
                v->music_vol = v->music_now = mv / 0.75f;
                sound_settings(v->sfx_vol * 0.75f, v->music_now * 0.75f, 0);
                break;                               /* (saveProfile: the popup asks for the write) */
            case SHELL_EVT_PROFILE_LOAD:
            case SHELL_EVT_PROFILE_CREATE:
            case SHELL_EVT_PROFILE_SAVE:
                card_job(v, v->shell, e);
                break;
            case SHELL_EVT_PROFILE_NONE: v->prof.allow_save = v->prof.was_saving = 0; break;
            case SHELL_EVT_BUDDY_PROFILE: shell_buddy_profile(v->shell, -1); break;   /* no second profile here */
            case SHELL_EVT_CHEATS: v->prof.cheats = (uint32_t)e->arg; break;
            case SHELL_EVT_START: start = 1; break;
            case SHELL_EVT_GRAPHICS:                 /* the Options' Graphics page: tried, put back or kept */
                if (shell_graphics(v->shell))
                    graphics_from_menu(v, shell_graphics(v->shell), e->arg);
                break;
            case SHELL_EVT_MOVIE: movie_start(v, e->name); break;
            case SHELL_EVT_VICTORY_SHOWN: profile_set_victory_shown(&v->prof); break;
            case SHELL_EVT_ATTRACT_GAME:             /* the demo (shell_game(), play type 0): its loading screen,
                                                      * then SHELL_EVT_START as any game */
                printf("attract mode demo %d: %s\n", e->arg, e->name);
                break;
            default: break;                          /* the emulators: skipped */
            }
        }
    movie_step(v);
    return start;
}

/* the index of a monster by name (Viewer.monsters), -1 none */
static int monster_index(const Viewer *v, const char *name)
{
    for (uint32_t i = 0; name && i < v->nmonsters; i++)
        if (!_stricmp(name, v->monsters[i]))
            return (int)i;
    return -1;
}

/* the players and bots of a game: n monsters (monster[k] of Viewer.monsters each), the last `bots` of them the
 * computer's. Player 1 plays on the first keys, player 2 (not a bot) on the second, players 3 and 4 on their
 * controllers (pad.h slots 2, 3) when there are ones as the game starts; the computer drives the rest.
 * They stand in the street as BeginLevelNI puts them: x = -17 (n - 1) / 2 + 17 i, facing the camera */
static void setup_players(Viewer *v, int n, int bots, const int *monster)
{
    v->nmon = n < 1 ? 1 : n > MONSTERS ? MONSTERS : n;
    int soak = (v->prof.cheats & PCH_10000) != 0;    /* [R 0x2B7DC0] the world's start with the soak test's cheat:
                                                      * bots = players (0x2B7D18: none again at the world's end) */
    if (soak)
        bots = v->nmon;
    int most = v->attract || soak ? v->nmon : v->nmon - 1;   /* the demo's are all the computer's */
    v->num_bots = bots < 0 ? 0 : bots > most ? most : bots;
    for (int k = 0; k < MONSTERS; k++) {
        Mon *mo = &v->mon[k];
        mo->v = v;
        mo->slot = k;
        mo->monster = k < v->nmon ? monster[k] : -1;
        mo->bot = k < v->nmon && k >= v->nmon - v->num_bots;
        mo->drive = mo->bot || v->ap ? DRIVE_COMPUTER : k == 0 ? DRIVE_KEYS1 : k == 1 ? DRIVE_KEYS2 :
                    v->pads_on >> k & 1 ? DRIVE_PAD : DRIVE_COMPUTER;   /* (--autoplay: all the computer's) */
        mo->mx = -17.0f * (float)(v->nmon - 1) / 2 + 17.0f * (float)k;
        mo->my = -18;
        mo->heading = 180;
        memset(&mo->pad, 0, sizeof mo->pad);
        mo->reveal = mo->shown = 0;
    }
    v->extra = -1;
}

/* gShell.startGame: the game the shell chose (GameStatus), the city opened at its block. The demo (play type 0) keeps
 * the seeds it drew for the world; for the other play types the shell draws new ones (0x308890 [R]): each (s >> 8) &
 * 0xFFFF of a step of 0x106BD0 on gShell +0x1C, drawn again while 0, into GameStatus +0x0 and +0x4 (the current
 * record's), before the profile takes the status (0x33E290). The state's start is not found (gShell is in .bss and no
 * traced code writes +0x1C; a Park-Miller state of 0 would stay 0): 1 here [G] */
static void shell_start_game(Viewer *v)
{
    const ShellGame *g = shell_game(v->shell);
    int city = v->city_index;
    for (int i = 0; i < NCITIES; i++)
        if (!_stricmp(g->city, CITIES[i]))
            city = i;
    v->attract = g->play_type == GP_ATTRACT;
    v->attract_button = 0;
    v->play_type = v->attract ? 0 : g->play_type;
    v->night = g->night;
    if (v->attract) {                                /* GameStatus +0x0 / +0x4: the current record's */
        v->prof.current.seed[0] = g->seed[0];
        v->prof.current.seed[1] = g->seed[1];
    } else {
        if (!v->world_rng)
            v->world_rng = 1;
        for (int k = 0; k < 2; k++)
            do
                v->prof.current.seed[k] = wrand_pm_next(&v->world_rng) >> 8 & 0xFFFF;
            while (!v->prof.current.seed[k]);
    }
    memset(&v->kept, 0, sizeof v->kept);
    shell_keep(v->shell, &v->kept);
    int pick[MONSTERS], n = g->num_players < 1 ? 1 : g->num_players > MONSTERS ? MONSTERS : g->num_players;
    for (int k = 0; k < n; k++)                      /* the players' monsters, then the bots' (GameStatus +0x14) */
        pick[k] = monster_index(v, g->monster_name[k]);
    for (int k = 1; k < n; k++)                      /* a name the viewer does not know: player 1's monster (a guard:
                                                      * the shell fills every player's, GameStatus +0x14 + 4 i) */
        if (pick[k] < 0)
            pick[k] = pick[0];
    setup_players(v, n, g->num_bots, pick);
    v->clip = -1;
    v->play = have_monster(v);
    v->last_game = *g;
    movie_stop(v);
    gui3d_free(v);
    v->prof.profile_shown = shell_profile_shown(v->shell);
    shell_close(v->shell);
    v->shell = NULL;
    v->exit_game = 0;
    /* the start block is the BlockList's index (Profile.getStartBlock: the block after the last one won); the names'
     * list keeps the repeated first block once, so taken as their index it began one block on, the block after the
     * one to play again skipped */
    open_city(v, city, 0, v->last_game.start_block > 0 ? v->last_game.start_block : 0);   /* (g is gone) */
    const ShellGame *lg = &v->last_game;
    ap_event(v->ap, "what=game_start city=%s type=%d players=%d bots=%d start_block=%d night=%d monsters=%s,%s,%s,%s",
             CITIES[city], lg->play_type, lg->num_players, lg->num_bots, lg->start_block, lg->night, lg->monster_name[0],
             n > 1 ? lg->monster_name[1] : "-", n > 2 ? lg->monster_name[2] : "-", n > 3 ? lg->monster_name[3] : "-");
}

/* the demo is over, the shell's again (0x308950): with the button's end its idle timer is 120 (0x324CC8), so the main
 * menu; else still 0, the attract mode. +0x20 set: restoreProfile (0x308A00 -> 0x33D910), the current game is the
 * profile's again (its city, night, start block, monsters, seeds; three lives); the play type stays 0 and the players
 * and bots the demo's (0x4D7E44, 0x4D7E28 / 2C are not in the record) [R] */
static void attract_end(Viewer *v, ShellStart *st)
{
    if (v->attract_button)
        st->idle = SHELL_IDLE_SECONDS;
    profile_restore(&v->prof);
    const ProfileStatus *c = &v->prof.current;
    ShellGame *g = &st->game;
    g->night = c->night != 0;
    g->city_index = c->city >= 0 && c->city < v->prof.ncities ? c->city : 0;
    snprintf(g->city, sizeof g->city, "%s", v->prof.city_name[g->city_index] ? v->prof.city_name[g->city_index] : "");
    g->start_block = c->start_block;
    g->seed[0] = c->seed[0];
    g->seed[1] = c->seed[1];
    for (int i = 0; i < 4; i++) {
        int m = c->monster[i] >= 0 && c->monster[i] < v->prof.nmonsters ? c->monster[i] : 0;
        g->monster[i] = m;
        snprintf(g->monster_name[i], sizeof g->monster_name[i], "%s",
                 v->prof.monster_name[m] ? v->prof.monster_name[m] : "");
    }
    v->attract = v->attract_button = 0;
}

/* the "card"'s free room for the shell's room tests: --card-free-kb, else the drive the profile file is on, a look a
 * second; nothing written (a --shot run): room enough. Only the tests against 140 read it (no text shows it), so a
 * drive's room needs no card's cap */
static void card_free(Viewer *v, float dt)
{
    if (!v->shell)
        return;
    if (v->card_free_kb >= 0) {
        shell_set_card_free(v->shell, v->card_free_kb);
        return;
    }
    if (!v->prof_write || (v->card_poll -= dt) > 0)
        return;
    v->card_poll = 1;
    long long kb = plat_free_kb(v->prof_path);
    if (kb >= 0)
        shell_set_card_free(v->shell, kb > 0x7FFFFFFF ? 0x7FFFFFFF : (int)kb);
}

/* ---- the controllers (pad.h) as the PS2's pads ---- */

/* an axis through the dead zone, the rest scaled to 0 .. 1 (the PS2 pack does it per axis, 0x1A6050 [R]; its zone
 * and curve sit in the pack object, not traced: 0.2 and straight here [G]) */
static float pad_axis(float v)
{
    const float DZ = 0.2f;
    return v > DZ ? (v - DZ) / (1 - DZ) : v < -DZ ? (v + DZ) / (1 - DZ) : 0;
}

/* a controller onto the game's pad, over what the keys put there (keys_pad): each button in the slot the PS2 pack's
 * table gives it [R 0x3B18B0], held, and pressed when it was up the frame before; each stick the larger of the keys'
 * and the pad's. The pressures (28 R2, 29 L2) are not read by the port's move graphs */
static void pad_to_pad(const PadState *p, const PadState *prev, Pad *pd)
{
    static const struct { int pad, mv; } MAP[] = {
        { PAD_UP, MV_UP }, { PAD_DOWN, MV_DOWN }, { PAD_LEFT, MV_LEFT }, { PAD_RIGHT, MV_RIGHT },
        { PAD_START, MV_START }, { PAD_BACK, MV_SELECT }, { PAD_RT, MV_R2 }, { PAD_LT, MV_L2 }, { PAD_RB, MV_R1 },
        { PAD_LB, MV_L1 }, { PAD_A, MV_CROSS }, { PAD_B, MV_CIRCLE }, { PAD_X, MV_SQUARE }, { PAD_Y, MV_TRIANGLE },
        { PAD_RS, MV_R3 }, { PAD_LS, MV_L3 } };
    for (size_t k = 0; k < sizeof MAP / sizeof *MAP; k++) {
        int down = p->buttons >> MAP[k].pad & 1, was = prev->buttons >> MAP[k].pad & 1;
        pd->held[MAP[k].mv] |= (uint8_t)down;
        pd->pressed[MAP[k].mv] |= (uint8_t)(down && !was);
    }
    float lx = pad_axis(p->lx), ly = pad_axis(p->ly), rx = pad_axis(p->rx), ry = pad_axis(p->ry);
    if (lx * lx + ly * ly > pd->lx * pd->lx + pd->ly * pd->ly) {
        pd->lx = lx;
        pd->ly = ly;
    }
    if (rx * rx + ry * ry > pd->rx * pd->rx + pd->ry * pd->ry) {
        pd->rx = rx;
        pd->ry = ry;
    }
}

/* ---- the menus' pad: MenuEventHandler (0x169880 .. 0x16A870) [R]. Each pad event becomes an event code (0x16A430):
 * a button that went down by its logical slot (0x3F67C0, the table 0x40F130): Up 1, Down 2, Left 3, Right 4, A (Cross)
 * 5, B (Circle) 6, Start 7, X (Square) 8, Y (Triangle) 9, Z 10, LTrigger (L2) 11, RTrigger (R2) 12, W 13; Back
 * (Select), the shoulders and the thumbs none; the left stick by its flicks (0x16A5B0). GuiManager::input (0x22F7A0,
 * its table 0x422410) makes them the GuiEvents: 1 .. 4 up, down, left, right, 5 Accept, 6 Back, 7 Start, 8 .. 13
 * Button code - 8. */
enum { MC_UP = 1, MC_DOWN, MC_LEFT, MC_RIGHT, MC_A, MC_B, MC_START, MC_X, MC_Y, MC_Z, MC_LT, MC_RT, MC_W };
static const struct { int bit, ev, button; } PAD_MENU_EV[] = {
    { 1 << MC_UP, HUD_EV_UP, 0 }, { 1 << MC_DOWN, HUD_EV_DOWN, 0 }, { 1 << MC_LEFT, HUD_EV_LEFT, 0 },
    { 1 << MC_RIGHT, HUD_EV_RIGHT, 0 }, { 1 << MC_A, HUD_EV_ACCEPT, 0 }, { 1 << MC_B, HUD_EV_BACK, 0 },
    { 1 << MC_START, HUD_EV_START, 0 }, { 1 << MC_X, HUD_EV_BUTTON, 0 }, { 1 << MC_Y, HUD_EV_BUTTON, 1 },
    { 1 << MC_Z, HUD_EV_BUTTON, 2 }, { 1 << MC_LT, HUD_EV_BUTTON, 3 }, { 1 << MC_RT, HUD_EV_BUTTON, 4 },
    { 1 << MC_W, HUD_EV_BUTTON, 5 } };

/* [R 0x16A5B0] a stick axis (a: 0 x, 1 y) moved to x: inside 0.15 (or coming from the other side) it rests at 0.15;
 * 0.45 past its rest (so first at 0.6) it flicks, the code of that way, and is armed; armed, it follows the stick out
 * and is disarmed 0.45 back from there; disarmed, it follows it back in (0.45 out again is the next flick) */
static int menu_flick(struct MenuPad *m, int a, float x)
{
    float s = x < 0 ? -1.0f : 1.0f, d = s * x, r = s * m->ref[a];   /* as on the + side */
    if (d < 0.15f || r < 0.15f) {
        m->ref[a] = s * 0.15f;
        m->armed[a] = 0;
        return 0;
    }
    if (d - r > 0.45f && !m->armed[a]) {
        m->armed[a] = 1;
        m->ref[a] = x;
        return a ? (x < 0 ? MC_UP : MC_DOWN) : (x < 0 ? MC_LEFT : MC_RIGHT);
    }
    if (d - r < -0.45f && m->armed[a])
        m->armed[a] = 0;
    if (m->armed[a] ? d > r : d < r)
        m->ref[a] = x;
    return 0;
}

/* [R 0x169E90] the code held is still held: its d-pad button, or the stick armed that way (up / down past 0.15,
 * left / right past 0); the others never are */
static int menu_still_held(const struct MenuPad *m, const PadState *p, int code)
{
    switch (code) {
    case MC_UP:    return (p->buttons >> PAD_UP & 1) || (m->armed[1] && m->ref[1] < -0.15f);
    case MC_DOWN:  return (p->buttons >> PAD_DOWN & 1) || (m->armed[1] && m->ref[1] > 0.15f);
    case MC_LEFT:  return (p->buttons >> PAD_LEFT & 1) || (m->armed[0] && m->ref[0] < 0);
    case MC_RIGHT: return (p->buttons >> PAD_RIGHT & 1) || (m->armed[0] && m->ref[0] > 0);
    }
    return 0;
}

/* pad slot k's menu events this frame, a bit (1 << code) each: its events as they come (the stick's, then the buttons
 * that went down [G: the order within a frame]), the held one again (0x169910's frame, 0x169E90) once it has been held
 * 0.5 s (+0x30) and 0.125 s (+0x34) since it last came, until it is let go */
static int pad_menu(Viewer *v, int k, float dt)
{
    static const struct { int pad, code; } BUTTONS[] = {   /* in the logical slots' order */
        { PAD_UP, MC_UP }, { PAD_DOWN, MC_DOWN }, { PAD_LEFT, MC_LEFT }, { PAD_RIGHT, MC_RIGHT },
        { PAD_START, MC_START }, { PAD_RT, MC_RT }, { PAD_LT, MC_LT }, { PAD_A, MC_A }, { PAD_B, MC_B },
        { PAD_X, MC_X }, { PAD_Y, MC_Y } };
    struct MenuPad *m = &v->menu_pad[k];
    if (!v->pads_live || !(v->pads_on >> k & 1)) {   /* a pad not there (0x45AA48): nothing held */
        m->held = 0;
        return 0;
    }
    const PadState *p = &v->pads[k], *was = &v->pads_prev[k];
    int codes[2 + sizeof BUTTONS / sizeof *BUTTONS], n = 0, out = 0;
    codes[n++] = menu_flick(m, 0, pad_axis(p->lx));
    codes[n++] = menu_flick(m, 1, pad_axis(p->ly));
    for (size_t i = 0; i < sizeof BUTTONS / sizeof *BUTTONS; i++)
        if ((p->buttons >> BUTTONS[i].pad & 1) && !(was->buttons >> BUTTONS[i].pad & 1))
            codes[n++] = BUTTONS[i].code;
    for (int i = 0; i < n; i++)
        if (codes[i]) {                              /* [R 0x16999C] a new code is held from now */
            if (codes[i] != m->held) {
                m->held = codes[i];
                m->start = m->now;
            }
            m->last = m->now;
            out |= 1 << codes[i];
        }
    m->now += dt;                                    /* [R 0x169A60] the frame's step */
    if (m->held) {
        int due = m->now - m->start >= 0.5f && m->now - m->last >= 0.125f;
        if (!menu_still_held(m, p, m->held)) {
            m->held = 0;
            m->start = m->last = m->now;
        } else if (due) {
            m->last = m->now;
            out |= 1 << m->held;
        }
    }
    return out;
}

/* pad slot k's button went down this frame */
static int pad_went_down(const Viewer *v, int k, int button)
{
    return v->pads_live && (v->pads_on >> k & 1) && (v->pads[k].buttons >> button & 1) &&
           !(v->pads_prev[k].buttons >> button & 1);
}

/* pad 0 held (gGuiMgr.getController(0)): its stick across, and getButtons' bits from the d-pad, Start, Select and the
 * triggers (shell.h SHELL_HELD_*) */
static float pad0_x(const Viewer *v)
{
    return v->pads_live && (v->pads_on & 1) ? pad_axis(v->pads[0].lx) : 0;
}
static int pad0_held(const Viewer *v)
{
    static const struct { int pad, held; } MAP[] = {
        { PAD_UP, SHELL_HELD_UP }, { PAD_DOWN, SHELL_HELD_DOWN }, { PAD_LEFT, SHELL_HELD_LEFT },
        { PAD_RIGHT, SHELL_HELD_RIGHT }, { PAD_START, SHELL_HELD_START }, { PAD_BACK, SHELL_HELD_BACK },
        { PAD_RT, SHELL_HELD_R2 }, { PAD_LT, SHELL_HELD_L2 }, { PAD_RB, SHELL_HELD_R1 }, { PAD_LB, SHELL_HELD_L1 },
        { PAD_A, SHELL_HELD_CROSS }, { PAD_B, SHELL_HELD_CIRCLE }, { PAD_X, SHELL_HELD_SQUARE },
        { PAD_Y, SHELL_HELD_TRIANGLE }, { PAD_RS, SHELL_HELD_R3 }, { PAD_LS, SHELL_HELD_L3 } };
    int h = 0;
    for (size_t k = 0; v->pads_live && (v->pads_on & 1) && k < sizeof MAP / sizeof *MAP; k++)
        if (v->pads[0].buttons >> MAP[k].pad & 1)
            h |= MAP[k].held;
    return h;
}

/* the graphics settings in use, for the Options' Graphics page as it opens (the front end's, the Pause menu's) */
static void shell_give_graphics(Viewer *v)
{
    if (!v->gfx)
        return;
    ShellGraphics g = { v->gfx->ao, v->gfx->smaa, v->gfx->msaa, v->gfx->aniso, v->gfx->mips, v->gfx->fullscreen,
                        v->gfx->vsync };
    if (v->shell)
        shell_set_graphics(v->shell, &g);
    HudGraphics hg = { g.ao, g.smaa, g.msaa, g.aniso, g.mips, g.fullscreen, g.vsync };
    if (v->hud)
        hud_set_graphics(v->hud, &hg);
}

/* the shell's frame: the pad (Enter / Space accept, Backspace back, the arrows, P start, C / V / R the buttons,
 * letters and digits into the name entry, F2 L2 + R2 held for the cheat entry; each controller as its own pad), its
 * time, its answers */
static void shell_frame(Viewer *v, const Window *w, float dt)
{
    static const struct { int key, ev, button; } KEYS[] = {
        { VK_RETURN, HUD_EV_ACCEPT, 0 }, { VK_SPACE, HUD_EV_ACCEPT, 0 }, { VK_BACK, HUD_EV_BACK, 0 },
        { VK_UP, HUD_EV_UP, 0 }, { VK_DOWN, HUD_EV_DOWN, 0 }, { VK_LEFT, HUD_EV_LEFT, 0 }, { VK_RIGHT, HUD_EV_RIGHT, 0 },
        { 'P', HUD_EV_START, 0 }, { 'C', HUD_EV_BUTTON, 0 }, { 'V', HUD_EV_BUTTON, 1 }, { 'R', HUD_EV_BUTTON, 4 } };
    static const struct { int key, ev, button; } KEYS2[] = {   /* the keypad: 0 Cross, 1 Circle, 7 Square, 9 Triangle,
                                                      * 3 R2 (the controller's A, B, X, Y, RT), 8 2 4 6 the d-pad */
        { VK_NUMPAD0, HUD_EV_ACCEPT, 0 }, { VK_NUMPAD1, HUD_EV_BACK, 0 }, { VK_NUMPAD8, HUD_EV_UP, 0 },
        { VK_NUMPAD2, HUD_EV_DOWN, 0 }, { VK_NUMPAD4, HUD_EV_LEFT, 0 }, { VK_NUMPAD6, HUD_EV_RIGHT, 0 },
        { VK_NUMPAD7, HUD_EV_BUTTON, 0 }, { VK_NUMPAD9, HUD_EV_BUTTON, 1 }, { VK_NUMPAD3, HUD_EV_BUTTON, 4 } };
    const char *focus = shell_focus(v->shell);
    int typing = focus && !strcmp(focus, "MemoryCard");
    unsigned char took[256] = { 0 };                 /* the keys the name entry took (the popup's other panels
                                                      * take none: Enter / Backspace answer them) */
    for (int c = 'A'; typing && c <= 'Z'; c++)
        if (w->pressed[c])
            took[c] = (unsigned char)shell_type(v->shell, c);
    for (int c = '0'; typing && c <= '9'; c++)
        if (w->pressed[c])
            took[c] = (unsigned char)shell_type(v->shell, c);
    if (typing && w->pressed[VK_RETURN])             /* the name entry: Enter is DONE, Backspace rubs out */
        took[VK_RETURN] = (unsigned char)shell_type(v->shell, '\r');
    if (typing && w->pressed[VK_BACK])
        took[VK_BACK] = (unsigned char)shell_type(v->shell, '\b');
    for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++)
        if (w->pressed[KEYS[k].key] && !took[KEYS[k].key & 255])
            shell_input(v->shell, KEYS[k].ev, KEYS[k].button);
    for (size_t k = 0; k < sizeof KEYS2 / sizeof *KEYS2; k++)   /* player 2's keypad: pad 1, as in play */
        if (w->pressed[KEYS2[k].key])
            shell_input_pad(v->shell, KEYS2[k].ev, KEYS2[k].button, 1);
    for (int k = 0; k < PAD_SLOTS; k++) {            /* GuiEvent.getController(): the controller's slot */
        int m = pad_menu(v, k, dt);
        for (size_t i = 0; m && i < sizeof PAD_MENU_EV / sizeof *PAD_MENU_EV; i++)
            if (m & PAD_MENU_EV[i].bit)
                shell_input_pad(v->shell, PAD_MENU_EV[i].ev, PAD_MENU_EV[i].button, k);
    }
    int held = (w->keys[VK_LEFT] ? SHELL_HELD_LEFT : 0) | (w->keys[VK_RIGHT] ? SHELL_HELD_RIGHT : 0) |
               (w->keys[VK_F2] ? SHELL_HELD_L2 | SHELL_HELD_R2 : 0) | pad0_held(v);
    shell_pad_held(v->shell, pad0_x(v), held);
    shell_give_graphics(v);
    card_free(v, dt);
    shell_update(v->shell, dt);
    gui3d_step(v, v->shell, dt);
    if (shell_answer(v))
        v->shell_go = 1;                             /* after this frame: the loading screen is drawn once */
}

/* --shell with --shot: the --do words for the shell (wait S at 1/60 s, menu <ev>, button N, type TEXT, done; p2 ..
 * p4 the menu words after it from that player's pad, p1 again player 1's); the words left once a game starts (they
 * play in it), NULL none */
static const char *shell_script(Viewer *v, const char *script)
{
    static const struct { const char *name; int ev; } EV[] = {
        { "up", HUD_EV_UP }, { "down", HUD_EV_DOWN }, { "left", HUD_EV_LEFT }, { "right", HUD_EV_RIGHT },
        { "accept", HUD_EV_ACCEPT }, { "back", HUD_EV_BACK }, { "start", HUD_EV_START } };
    const char *p = script;
    char word[32], arg[32];
    int pad = 0;
    while (*p && v->shell && !v->shell_go) {
        shell_give_graphics(v);
        while (*p == ' ' || *p == ',') p++;
        int n = 0, k = 0;
        while (*p && *p != ' ' && *p != ',' && n < 31) word[n++] = *p++;
        word[n] = 0;
        while (*p == ' ') p++;
        if ((word[0] == 'p' || word[0] == 'P') && word[1] >= '1' && word[1] <= '4' && !word[2]) {
            pad = word[1] - '1';                     /* (no argument: what follows is the next word) */
            continue;
        }
        while (*p && *p != ',' && k < 31) arg[k++] = *p++;
        while (k > 0 && arg[k - 1] == ' ') k--;
        arg[k] = 0;
        if (!_stricmp(word, "wait"))
            for (int f = 0, frames = (int)(atof(arg) * 60 + 0.5); f < frames && !v->shell_go; f++) {
                card_free(v, 1.0f / 60);
                shell_update(v->shell, 1.0f / 60);
                gui3d_step(v, v->shell, 1.0f / 60);
                if (shell_answer(v))
                    v->shell_go = 1;
            }
        else if (!_stricmp(word, "menu"))
            for (size_t i = 0; i < sizeof EV / sizeof *EV; i++) {
                if (!_stricmp(arg, EV[i].name))
                    shell_input_pad(v->shell, EV[i].ev, 0, pad);
            }
        else if (!_stricmp(word, "button"))
            shell_input_pad(v->shell, HUD_EV_BUTTON, atoi(arg), pad);
        else if (!_stricmp(word, "done"))            /* the name entry's DONE */
            shell_type(v->shell, '\r');
        else if (!_stricmp(word, "type"))
            for (const char *c = arg; *c; c++)
                shell_type(v->shell, toupper((unsigned char)*c));
        card_free(v, 1.0f / 60);
        shell_update(v->shell, 1.0f / 60);
        gui3d_step(v, v->shell, 1.0f / 60);
        if (shell_answer(v))
            v->shell_go = 1;
    }
    while (*p == ' ' || *p == ',') p++;
    return *p ? p : NULL;
}

/* ---- --autoplay (autoplay.h): the port's own test, a whole playthrough with nobody at the pad */

/* the shell's pad for the player, one press every 0.4 s at most: the boot's notice and the popup's messages taken
 * (a new profile's name typed), the main menu to the mode, players and bots asked for, every monster card readied,
 * the city select to the city asked for or the campaign's next one (the last one open that is not destroyed), the
 * screens it has no business on left with Back; nothing while the loading screen or the Victory's movies play, and
 * nothing in the menus while it waits for the demos (the attract mode comes after 120 s) */
static void autopilot_shell(Viewer *v, float dt)
{
    if ((v->ap_wait -= dt) > 0)
        return;
    v->ap_wait = 0.4f;
    ShellAutoView sv;
    shell_auto_view(v->shell, &sv);
    const char *f = shell_focus(v->shell);
    int ev = 0, pad = 0;
    if (!strcmp(f, "MemoryCard")) {
        if (sv.mc_mode == 2) {                       /* the name entry: a name, DONE */
            for (const char *c = "AUTOPLAY"; *c; c++)
                shell_type(v->shell, *c);
            shell_type(v->shell, '\r');
            return;
        }
        ev = HUD_EV_ACCEPT;
    } else if (!strcmp(f, "AttractMode"))
        ev = v->ap_demos ? 0 : HUD_EV_ACCEPT;
    else if (!strcmp(f, "MainMenu")) {
        int want = v->ap_type ? v->ap_type : GP_CAMPAIGN, np = v->ap_players < 1 ? 1 : v->ap_players > 2 ? 2 :
                   v->ap_players;                    /* (the PS2's boxes offer 1 or 2) */
        if (v->ap_demos)
            ev = 0;
        else if (sv.mm_state == 0)
            ev = sv.mm_play_mode != want ? HUD_EV_RIGHT : HUD_EV_ACCEPT;
        else if (sv.mm_state == 1)
            ev = sv.mm_players < np - 1 ? HUD_EV_DOWN : sv.mm_players > np - 1 ? HUD_EV_UP : HUD_EV_ACCEPT;
        else if (sv.mm_state == 2)
            ev = sv.mm_bots < v->ap_bots ? HUD_EV_DOWN : sv.mm_bots > v->ap_bots ? HUD_EV_UP : HUD_EV_ACCEPT;
        else
            ev = HUD_EV_BACK;
    } else if (!strcmp(f, "MonsterSelect")) {
        ev = HUD_EV_ACCEPT;
        int humans = v->shell ? shell_game(v->shell)->num_players - shell_game(v->shell)->num_bots : 1;
        for (int k = 0; k < sv.ms_players && k < 4; k++)
            if (!(sv.ms_ready >> k & 1)) {
                pad = k < humans ? k : 0;            /* (the bots' cards: player 1's pad) */
                break;
            }
    } else if (!strcmp(f, "CitySelect")) {
        int target = v->ap_city;
        if (target < 0) {                            /* the campaign's next city */
            for (int i = 0; i < 7; i++) {
                int fl = profile_city_flags(&v->prof, i);
                if ((fl & 0x80) && (target < 0 || !(fl & 1)))
                    target = i;
            }
            if (target < 0)
                target = 0;
        }
        ev = sv.cs_index < target ? HUD_EV_RIGHT : sv.cs_index > target ? HUD_EV_LEFT : sv.cs_locked ? HUD_EV_BACK :
             HUD_EV_ACCEPT;
    } else if (!strcmp(f, "MonsterSheet") || !strcmp(f, "Options"))
        ev = HUD_EV_BACK;
    if (ev)
        shell_input_pad(v->shell, ev, 0, pad);
}

/* the frame for the watchdog, and what it asks: a block that never ends felled (and its boss beaten), a results
 * screen answered, the shell's screen left with Back; the popup over the game's screens answered */
static int autoplay_watch(Viewer *v)
{
    static ApFrame f;
    memset(&f, 0, sizeof f);
    f.t = v->ap_t;
    f.in_game = v->shell == NULL;
    if (v->shell) {
        f.focus = shell_focus(v->shell);
        f.shell_state = shell_state(v->shell);
        f.movie = shell_movie_clock(v->shell) >= 0;
    } else {
        const char *bn = NULL;
        f.state = game_state(v->game);
        f.play_type = play_type(v);
        f.city = CITIES[v->city_index];
        f.block_index = game_block(v->game, &bn);
        f.block = bn;
        f.results = v->hud && hud_results_showing(v->hud);
        f.clock = game_clock(v->game, NULL);
        f.city_score = game_city_total(v->game);
        f.guard_falls = v->bot_guard_falls;
        f.nmon = v->nmon < AP_MONSTERS ? v->nmon : AP_MONSTERS;
        for (int k = 0; k < f.nmon; k++) {
            Mon *mo = &v->mon[k];
            ApMonster *m = &f.mon[k];
            m->pos[0] = mo->mx; m->pos[1] = mo->my; m->pos[2] = mo->mz;
            m->hp = mo->hp;
            m->lives = game_lives(v->game, k);
            m->state = mo->have_graph ? mo->graph.states[mo->run.state].name : NULL;
            m->mounted = mo->mount.mounted;
            m->ground = mo->have_graph && mon_in(mo) ? ground_at(mo) : 0;
            m->out = mo->human || mo->dropping || mo->defeated || !mon_in(mo) || mon_in(mo)->hidden;
            f.points[k] = game_points(v->game, k, GPT_TOTAL);
        }
        if (v->boss_block && v->boss) {
            const BossPart *bp = boss_part(v->boss, boss_main(v->boss));
            f.boss = bp && bp->alive && !boss_defeated(v->boss) ? 1 : 2;
            f.boss_hp = bp ? bp->hp_frac : 0;
        }
        f.nbuildings = (int)v->scene.nbuildings;
        for (uint32_t i = 0; i < v->scene.nbuildings; i++) {
            const Building *b = &v->scene.buildings[i].b;
            int up = b->state == BUILDING_STANDING || b->state == BUILDING_FLOOR_FALLING;
            f.standing += up;
            if (i < AP_BUILDINGS) {
                memcpy(f.b[i].min, b->min, sizeof f.b[i].min);
                memcpy(f.b[i].max, b->max, sizeof f.b[i].max);
                f.b[i].standing = up;
            }
        }
        for (int i = 0; v->scene.folk && i < people_count(v->scene.folk); i++)   /* the live ones */
            f.people += people_get(v->scene.folk, i)->phase != PP_GONE;
        f.vehicles = v->veh ? vehicle_count(v->veh) : 0;
        f.army = v->army ? army_count(v->army) : 0;
        f.particles = v->fxw ? fx_particles(v->fxw) : 0;
        f.street = v->sobj ? streetobj_count(v->sobj) : 0;
        f.parade = v->parade ? parade_count(v->parade) : 0;
        f.rail = v->rail ? rail_count(v->rail) : 0;
        f.edibles = v->edibles ? edible_count(v->edibles) : 0;
        if (v->popup && shell_popup_active(v->popup) && (v->ap_wait -= 1.0f / 60) <= 0) {
            v->ap_wait = 0.5f;                       /* the SaveGame popup over the screens: its messages taken */
            shell_input(v->popup, HUD_EV_ACCEPT, 0);
        }
    }
    int r = ap_frame(v->ap, &f);
    if ((r & AP_FORCE_BLOCK) && !v->shell) {
        for (uint32_t i = 0; i < v->scene.nbuildings; i++)
            if (v->scene.buildings[i].b.state == BUILDING_STANDING)
                building_destroy(&v->scene.buildings[i].b);
        const BossPart *bp = v->boss ? boss_part(v->boss, boss_main(v->boss)) : NULL;
        if (bp && bp->alive)
            boss_hit(v->boss, 0, bp->pos, 1, 1e6f, 0x7F000000u);
    }
    if ((r & AP_FORCE_RESULT) && v->game) {
        if (v->hud)
            hud_input(v->hud, HUD_EV_ACCEPT, 0);
        game_set_result(v->game, game_state(v->game) == GS_GAME_OVER ? GR_CONTINUE : GR_DONE);
    }
    if ((r & AP_FORCE_SHELL) && v->shell)
        shell_input(v->shell, HUD_EV_BACK, 0);
    return r;
}

/* a player's pad from the keys. Player 1: W A S D / the left stick, the arrows the right stick, Space Cross (jump), J
 * Square (punch), K Triangle (kick), L Circle (grab), R R2 (roar). Player 2: the numeric keypad (Num Lock on): 8 4 2
 * 6 (or 5) the left stick, 0 Cross, 7 Square, 9 Triangle, 1 Circle, 3 R2, / and * the right stick left and right */
static void keys_pad(const Window *w, int drive, Pad *pd)
{
    static const struct { int key, button; } PAD1[] = {
        { VK_SPACE, MV_CROSS }, { 'J', MV_SQUARE }, { 'K', MV_TRIANGLE }, { 'L', MV_CIRCLE }, { 'R', MV_R2 } };
    static const struct { int key, button; } PAD2[] = {
        { VK_NUMPAD0, MV_CROSS }, { VK_NUMPAD7, MV_SQUARE }, { VK_NUMPAD9, MV_TRIANGLE }, { VK_NUMPAD1, MV_CIRCLE },
        { VK_NUMPAD3, MV_R2 } };
    memset(pd->pressed, 0, sizeof pd->pressed);
    memset(pd->held, 0, sizeof pd->held);
    if (drive == DRIVE_KEYS1) {
        pd->lx = (float)(w->keys['D'] - w->keys['A']);
        pd->ly = (float)(w->keys['S'] - w->keys['W']);
        pd->rx = (float)(w->keys[VK_RIGHT] - w->keys[VK_LEFT]);
        pd->ry = (float)(w->keys[VK_DOWN] - w->keys[VK_UP]);
        for (size_t k = 0; k < sizeof PAD1 / sizeof *PAD1; k++) {
            pd->pressed[PAD1[k].button] = w->pressed[PAD1[k].key];
            pd->held[PAD1[k].button] = w->keys[PAD1[k].key];
        }
    } else if (drive == DRIVE_KEYS2) {
        pd->lx = (float)(w->keys[VK_NUMPAD6] - w->keys[VK_NUMPAD4]);
        pd->ly = (float)((w->keys[VK_NUMPAD2] || w->keys[VK_NUMPAD5]) - w->keys[VK_NUMPAD8]);
        pd->rx = (float)(w->keys[VK_MULTIPLY] - w->keys[VK_DIVIDE]);
        pd->ry = 0;
        for (size_t k = 0; k < sizeof PAD2 / sizeof *PAD2; k++) {
            pd->pressed[PAD2[k].button] = w->pressed[PAD2[k].key];
            pd->held[PAD2[k].button] = w->keys[PAD2[k].key];
        }
    } else if (drive == DRIVE_PAD)                   /* no keys: the controller alone (pad_to_pad) */
        pd->lx = pd->ly = pd->rx = pd->ry = 0;
}

#if defined RAMPAGE_GUI && defined _WIN32
/* ---- the player build's own start (main below): where its files go, the log */
#define LOG_MAX (32L << 20)                          /* rampage.log starts again past this size */
static int g_gui_log;                                /* stdout and stderr go to rampage.log */

static int dir_ok(void)                              /* the current folder takes files and is not a temporary one */
{
    char cur[MAX_PATH + 2], tmp[MAX_PATH + 2];
    DWORD c = GetCurrentDirectoryA(MAX_PATH, cur), t = GetTempPathA(MAX_PATH, tmp);   /* tmp ends with a backslash */
    if (c && c < MAX_PATH && t > 3 && t < MAX_PATH) {     /* Explorer runs an exe opened inside a zip from %TEMP% */
        if (cur[c - 1] != '\\')
            strcat(cur, "\\");
        if (_strnicmp(cur, tmp, t) == 0)
            return 0;
    }
    HANDLE h = CreateFileA("rampage.tmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    CloseHandle(h);
    return 1;
}

static void gui_start(int argc, char **argv)
{
    int shot = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--shot") == 0)
            shot = 1;
    const char *base = getenv("LOCALAPPDATA");
    if (argc < 2 && !dir_ok() && base && base[0]) {  /* started from a read-only folder (Program Files) or from inside
                                                      * a zip: rampage.cfg, the profile and the log in the user's folder */
        char home[MAX_PATH];
        snprintf(home, sizeof home, "%s\\RampageRE", base);
        CreateDirectoryA(home, NULL);
        SetCurrentDirectoryA(home);
    }
    if (shot || getenv("RAMPAGE_CONSOLE"))           /* a --shot run keeps its streams (the tests read them) */
        return;
    FILE *f = fopen("rampage.log", "w");             /* a new log each start; both streams append to it */
    if (!f)
        return;
    fclose(f);
    if (freopen("rampage.log", "a", stdout))
        setvbuf(stdout, NULL, _IOLBF, 1024);
    if (freopen("rampage.log", "a", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
    g_gui_log = 1;
}

static void gui_log_bound(void)                      /* a bound for whatever still prints a lot (hours of play) */
{
    if (g_gui_log && ftell(stdout) > LOG_MAX && freopen("rampage.log", "w", stdout)) {
        setvbuf(stdout, NULL, _IOLBF, 1024);
        freopen("rampage.log", "a", stderr);
        printf("log: rampage.log passed %d MB and starts again\n", (int)(LOG_MAX >> 20));
    }
}
#define DEBUG_TITLE (getenv("RAMPAGE_DEBUGTITLE") != NULL)   /* the player build: a plain window title */
#else
#define DEBUG_TITLE 1                                /* city, block, clip, triangles, fps and effects in the title */
#endif

static int viewer_main(int argc, char **argv)
{
    static Viewer v;
    if (getenv("RAMPAGE_TRACE"))                     /* a trace survives a crash */
        setvbuf(stdout, NULL, _IONBF, 0);
    const char *shot = NULL, *monster[MONSTERS] = { NULL }, *pos[3] = { NULL, "Chicago", "1" };
    int npos = 0, night = 0, clip = -1, have_view = 0, mute = 0, players = 1, bots = 0;
    float view[5] = { 0 };
    v.sfx_vol = v.music_vol = v.music_now = 1;      /* the Options page: as the game until changed */
    v.card_free_kb = -1;
    v.left_block = -1;
    int newprofile = 0;
    const char *cheat = NULL;
    double shot_time = 0;
    const char *script = NULL;
    char walk_script[48];
    int autoplay = 0;                                /* --autoplay and its --ap-* (autoplay.h) */
    const char *ap_log = NULL, *ap_shots = NULL, *ap_city = NULL;
    ApLimits ap_lim;
    memset(&ap_lim, 0, sizeof ap_lim);
    uint32_t ap_seed = 0;
    Settings gfx, gfx_flags;                         /* the graphics settings (settings.h), the flags among them */
    settings_unset(&gfx_flags);
#ifdef __SWITCH__
    plat_switch_home();                              /* sdmc:/switch/rampagere: the data, rampage.cfg, the profile,
                                                      * rampage.log */
    if (argc < 2)                                    /* from the Homebrew Menu: the front end, played on the pads */
        v.shell_mode = 1;
#elif defined __ANDROID__
    android_init();                                  /* logcat; the app folder the current one: the data, rampage.cfg,
                                                      * the profile (RampageActivity starts the front end, --shell) */
#elif defined RAMPAGE_GUI
    if (argc < 2)                                    /* RampageRE.exe started as it is: the front end */
        v.shell_mode = 1;
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) {   /* no window, no sound, no message boxes */
            shot = argv[++i];
            v.unattended = plat_quiet = 1;
        }
        else if (strcmp(argv[i], "--monster") == 0 && i + 1 < argc) monster[0] = argv[++i];
        else if (!strncmp(argv[i], "--monster", 9) && argv[i][9] >= '1' && argv[i][9] <= '4' && !argv[i][10] &&
                 i + 1 < argc)               /* --monster2 Lizzie: player 2's (--monster1 is --monster) */
        {
            int k = argv[i][9] - '1';
            monster[k] = argv[++i];
        }
        else if (strcmp(argv[i], "--players") == 0 && i + 1 < argc) players = atoi(argv[++i]);
        else if (strcmp(argv[i], "--bots") == 0 && i + 1 < argc) bots = atoi(argv[++i]);
        else if (strcmp(argv[i], "--clip") == 0 && i + 1 < argc) clip = atoi(argv[++i]);
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) shot_time = atof(argv[++i]);
        else if (strcmp(argv[i], "--walk") == 0 && i + 1 < argc) {
            snprintf(walk_script, sizeof walk_script, "right %s", argv[++i]);
            script = walk_script;
        }
        else if (strcmp(argv[i], "--do") == 0 && i + 1 < argc) script = argv[++i];
        else if (strcmp(argv[i], "--night") == 0) night = 1;
        else if (strcmp(argv[i], "--nofog") == 0) v.fog = -1;
        else if (strcmp(argv[i], "--mute") == 0) mute = 1;
        else if (strcmp(argv[i], "--dumptex") == 0) tp_set_dump(1);   /* mods/dump/<scope>/ (texpack.c) */
        else if (strcmp(argv[i], "--nohud") == 0) v.no_hud = 1;
        else if (strcmp(argv[i], "--newprofile") == 0) newprofile = 1;
        else if (strcmp(argv[i], "--shell") == 0) v.shell_mode = 1;
        else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) v.lang = atoi(argv[++i]);
        else if (strcmp(argv[i], "--card-free-kb") == 0 && i + 1 < argc) v.card_free_kb = atoi(argv[++i]);
        else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) v.prof_path = argv[++i];
        else if (strcmp(argv[i], "--cheat") == 0 && i + 1 < argc) cheat = argv[++i];
        else if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) {   /* campaign, kingcity, kingworld, timed */
            const char *m = argv[++i];
            v.play_type = !_stricmp(m, "kingcity") ? GP_KING_CITY : !_stricmp(m, "kingworld") ? GP_KING_WORLD :
                          !_stricmp(m, "timed") ? GP_TIMED_RUN : GP_CAMPAIGN;
        }
        else if (strcmp(argv[i], "--cam") == 0 && i + 5 < argc) {   /* x y z yaw pitch (degrees): a fixed view */
            for (int k = 0; k < 5; k++)
                view[k] = (float)atof(argv[++i]);
            have_view = 1;
        }
        else if (strcmp(argv[i], "--autoplay") == 0) autoplay = 1;   /* the playthrough test (docs/NATIVE.md) */
        else if (strcmp(argv[i], "--ap-log") == 0 && i + 1 < argc) ap_log = argv[++i];
        else if (strcmp(argv[i], "--ap-shots") == 0 && i + 1 < argc) ap_shots = argv[++i];
        else if (strcmp(argv[i], "--ap-minutes") == 0 && i + 1 < argc) ap_lim.minutes = atof(argv[++i]);
        else if (strcmp(argv[i], "--ap-blocks") == 0 && i + 1 < argc) ap_lim.blocks = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ap-cities") == 0 && i + 1 < argc) ap_lim.cities = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ap-demos") == 0 && i + 1 < argc) ap_lim.demos = v.ap_demos = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ap-stuck") == 0 && i + 1 < argc) ap_lim.stuck = atof(argv[++i]);
        else if (strcmp(argv[i], "--ap-block-time") == 0 && i + 1 < argc) ap_lim.block = atof(argv[++i]);
        else if (strcmp(argv[i], "--ap-city") == 0 && i + 1 < argc) ap_city = argv[++i];
        else if (strcmp(argv[i], "--ap-seed") == 0 && i + 1 < argc) ap_seed = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (settings_arg(&gfx_flags, argc, argv, &i)) { }   /* --ao --smaa N --msaa N --aniso N --mips ... */
        else if (npos < 3) pos[npos++] = argv[i];
    }
    settings_resolve(&gfx, &gfx_flags, !shot);       /* a --shot run: the flags alone, never rampage.cfg */
    char found[1024], msg[1024];
    for (int i = 0; pos[0] && npos < 3 && i < NCITIES; i++)   /* "rampage Chicago 1": no path, a city */
        if (_stricmp(pos[0], CITIES[i]) == 0 && !plat_exists(pos[0])) {
            pos[2] = npos > 1 ? pos[1] : pos[2];
            pos[1] = pos[0];
            pos[0] = NULL;
            break;
        }
    if (!pos[0]) {                                   /* no path given: look for the data (iso.h) */
        if (!data_find(found, sizeof found)) {
#ifdef __ANDROID__                                   /* the first start: the disc image through the file picker */
            if (!android_data_setup(found, sizeof found))
                return 1;
#else
            int asked = 0;
#ifdef _WIN32
            if (!plat_quiet)                         /* the first start: the disc image in a file dialog, remembered
                                                      * (a --shot run never asks) */
                asked = data_ask(found, sizeof found);
            if (!asked && plat_told)                 /* cancelled */
                return 1;
#endif
            if (!asked) {
                snprintf(msg, sizeof msg, "Found no game data: no RAMPAGE.VOL, nor a disc image (.iso) with it, in %s.\n"
                         "Pass the path to RAMPAGE.VOL or to the game's disc image, or set RAMPAGE_VOL.", data_where());
                plat_message(msg, 1);
                return 1;
            }
#endif
        }
        pos[0] = found;
        printf("game data: %s\n", found);
    }
    int vol_err = vol_open(&v.vol, pos[0]);
    if (vol_err != 0) {
        if (vol_err == VOL_NOT_IN_IMAGE)
            snprintf(msg, sizeof msg, "%s is a disc image without RAMPAGE.VOL: not Rampage - Total Destruction.", pos[0]);
        else
            snprintf(msg, sizeof msg, "Cannot read %s.\nPass the path to RAMPAGE.VOL or to the game's disc image (.iso), "
                     "or run build.ps1 (it extracts the disc).", pos[0]);
        plat_message(msg, 1);
        return 1;
    }
    if (!data_check(pos[0], &v.vol, msg, sizeof msg))   /* another release: say so, carry on */
        plat_message(msg, 1);
    v.region = data_region(pos[0], &v.vol);
    int city = 0;
    for (int i = 0; i < NCITIES; i++)
        if (_stricmp(pos[1], CITIES[i]) == 0)
            city = i;
    uint32_t block = (uint32_t)(atoi(pos[2]) > 0 ? atoi(pos[2]) - 1 : 0);
    Window w;
    if (win_open(&w, "RampageRE", gfx.width, gfx.height, shot != NULL) != 0) {   /* a --shot run never shows a window */
        plat_message("Cannot open an OpenGL window.", 1);
        return 1;
    }
    settings_apply(&gfx, &w);
    v.gfx = &gfx;
    v.win = &w;
    v.gfx_write = !shot;
    if (!shot) {                                     /* the controllers: never for a --shot run (no light, no motor,
                                                      * the --do words alone drive it) */
        v.pads_live = 1;
        v.pads_on = pad_poll(v.pads, w.focused, (unsigned)w.dev_changes);
#ifdef __ANDROID__
        v.pads_on |= touch_pad(v.pads, v.pads_on);
#endif
    }
    v.fog = v.fog < 0 ? 0 : 1;                       /* the game's fog (LightingData FogStart / FogEnd), F toggles it */
    v.rnd = rnd_create();
    /* the game's sound (sound.c: Rampage.uber / .samp, the .mic music); the output only for a window of one's own,
     * never for a --shot run, nor with --mute */
    if (sound_init(&v.vol) > 0 && !shot && !mute && sound_start() != 0)
        printf("no sound output\n");
    {                                                /* the effects: fx.bdp, its words from RAMPAGE.ELF beside the VOL */
        uint32_t size = 0;
        uint8_t *data = vol_read(&v.vol, "fx.bdp", &size);
        uint32_t gsize = 0, csize = 0;                /* the HUD's: WorldGui.bdp and CommonGUI.bdp */
        uint8_t *gui = vol_read(&v.vol, "WorldGui.bdp", &gsize), *common = vol_read(&v.vol, "CommonGUI.bdp", &csize);
        v.have_gui = gui && common && bdp_load(&v.gui_world, gui, gsize) == 0 && bdp_load(&v.gui_common, common, csize) == 0;
        v.gui_world.scope = v.gui_common.scope = "GUI";   /* texture packs: the dump folders (texpack.c) */
        if (data && bdp_load(&v.fx_bundle, data, size) == 0) {
            v.fx_bundle.scope = "fx";
            const Bundle *bl[1] = { &v.fx_bundle };
            snprintf(v.data_path, sizeof v.data_path, "%s", pos[0]);
            v.have_fx_bundle = 1;
            v.fxlib = fx_open(v.data_path, bl, 1);
            v.fxw = v.fxlib ? fx_world(v.fxlib) : NULL;
            if (v.fxw) {
                fx_set_step(v.fxw, TIMESTEP);        /* the world's effects: its step (0x2852E0 -> 0x282260) [R] */
                fx_on_shake(v.fxw, fx_shake, &v);
                fx_on_sound(v.fxw, fx_sound, &v);
            }
            for (int i = 0; v.fxlib && i < fx_shape_count(v.fxlib) && i < 32; i++) {   /* the debris models */
                const BdpEntry *me[8];
                int nm = fx_shape_meshes(v.fxlib, i, me, 8);
                for (int k = 0; k < nm; k++)
                    if (mesh_read(me[k], &v.fx_shapes[i][v.fx_nshapes[i]].mesh) == 0)
                        v.fx_shapes[i][v.fx_nshapes[i]++].b = fx_shape_bundle(v.fxlib, i);
            }

        } else
            free(data);
    }
    v.night = night;
    v.show_hits = 1;
    profile_init(&v.prof);                           /* the profile: rampage_profile.bin (a --shot run: a new one,
                                                      * not written, unless --profile names one) */
    v.prof_write = !shot || v.prof_path;
    if (!v.prof_path)
        v.prof_path = "rampage_profile.bin";
    v.profile_autoload = gfx.profile_autoload;
    int prof_read = !newprofile && v.prof_write ? profile_read(&v.prof, v.prof_path) : -1;
    if (prof_read > 0) {
        printf("profile %s\n", v.prof_path);
        v.prof_on_card = 1;
        snprintf(v.card_name, sizeof v.card_name, "%s", v.prof.block.name);
        if (v.shell_mode && !gfx.profile_autoload)   /* the game's way: the shell's popup loads it */
            profile_init(&v.prof);
        else if (v.shell_mode) {                     /* [port extra, profile_autoload] as if picked from the list
                                                      * (SHELL_EVT_PROFILE_LOAD): loaded, saving, its checkpoint, and
                                                      * shown, so the main menu opens no list (close(1): 0x300) */
            v.prof.loaded = 1;
            v.prof.allow_save = v.prof.was_saving = 1;
            v.prof.profile_shown = 1;
            profile_restore_checkpoint(&v.prof);
            printf("profile %s loaded at launch (profile_autoload)\n", v.prof.block.name);
        } else                                       /* straight into a game: as if picked from the list */
            v.prof.allow_save = v.prof.was_saving = 1;
    } else if (prof_read == 0) {                     /* there but not a profile: the list still has it (its load
                                                      * shows "Corrupt Profile"), by the header's name (+0x30) */
        FILE *f = fopen(v.prof_path, "rb");
        uint8_t head[PROFILE_FILE_HEADER] = { 0 };
        if (f) {
            if (fread(head, 1, sizeof head, f) != sizeof head)
                head[0x30] = 0;
            fclose(f);
        }
        head[0x30 + 15] = 0;
        int ok = head[0x30] != 0;
        for (int i = 0x30; ok && head[i]; i++)
            ok = head[i] >= 32 && head[i] < 127;
        v.prof_on_card = 1;
        snprintf(v.card_name, sizeof v.card_name, "%s", ok ? (const char *)head + 0x30 : "Profile");
        profile_init(&v.prof);
    }
    profile_restore_checkpoint(&v.prof);              /* MainMenu's campaign start */
    for (const char *c = cheat; c && *c; c += c[6] == ',' ? 7 : 6) {   /* "986960,071767": several */
        char code[7];
        snprintf(code, sizeof code, "%s", c);
        if (!profile_cheat_code(&v.prof, code))
            printf("no cheat %s\n", code);
        if (strlen(c) <= 6)
            break;
    }
    v.vibrate = profile_set_rumble(&v.prof, profile_rumble(&v.prof), 0);   /* the pads vibrate as the profile says [G] */
    v.sfx_vol = profile_sound_volume(&v.prof) / 0.75f;
    v.music_vol = v.music_now = profile_music_volume(&v.prof) / 0.75f;
    sound_settings(v.sfx_vol * 0.75f, v.music_now * 0.75f, 0);
    v.nmonsters = monster_names(&v.vol, v.monsters, 64);
    v.clip = clip;
    if (autoplay) {                                  /* --autoplay: offscreen alone, in simulated time */
        char lp[1024], what[1024] = "";
        if (!shot) {
            plat_message("--autoplay runs with --shot (no window).", 1);
            return 1;
        }
        snprintf(lp, sizeof lp, "%s.log", shot);
        for (int i = 1; i < argc; i++)
            snprintf(what + strlen(what), sizeof what - strlen(what), "%s%s", i > 1 ? " " : "", argv[i]);
        ap_lim.stop_at_menu = !ap_lim.demos;         /* back at the main menu after a game: over */
        v.ap = ap_open(ap_log ? ap_log : lp, ap_shots, &ap_lim, what);
        if (!v.ap) {
            plat_message("--autoplay: cannot write its log.", 1);
            return 1;
        }
        v.ap_type = v.play_type;
        v.ap_players = players;
        v.ap_bots = bots;
        v.ap_city = ap_city ? campaign_index(ap_city) : v.play_type && v.play_type != GP_CAMPAIGN && !v.shell_mode ?
                    campaign_index(CITIES[city]) : -1;
        if (ap_seed) {                               /* the world's seeds, gShell's state they are drawn from, the bots' */
            uint32_t s = ap_seed;
            v.world_rng = v.bot_seed = ap_seed;
            for (int k = 0; k < 2; k++)
                do
                    v.prof.current.seed[k] = wrand_pm_next(&s) >> 8 & 0xFFFF;
                while (!v.prof.current.seed[k]);
        }
    }
    {                                                /* --players N (1 .. 4, --bots B of them the computer's) */
        static const char *const OTHERS[] = { "George", "Lizzie", "Ralph", "Boosy" };   /* the ones not named */
        int pick[MONSTERS];
        players = players < 1 ? 1 : players > MONSTERS ? MONSTERS : players;
        for (int k = 0; k < players; k++) {
            pick[k] = monster_index(&v, monster[k]);
            for (int j = 0; (k || players > 1 || autoplay) && pick[k] < 0 && j < 4; j++) {   /* one of the classic
                                                      * three (and Boosy) not taken yet; alone without --monster: none
                                                      * (--autoplay: George) */
                int c = monster_index(&v, OTHERS[j]), taken = 0;
                for (int q = 0; q < k; q++)
                    taken |= pick[q] == c;
                if (!taken)
                    pick[k] = c;
            }
        }
        if (pick[0] < 0)                             /* no monster: the free camera only */
            players = 1;
        setup_players(&v, players, bots, pick);
    }
    v.play = have_monster(&v) && clip < 0;
    if (v.shell_mode) {                              /* the front end first: the city once a game starts */
        shell_enter(&v, NULL);
        if (!v.shell) {
            plat_message("The shell's bundles are not in RAMPAGE.VOL.", 1);
            return 1;
        }
        if (shot && script)                          /* its words, then the shot (of the shell, or of the game, which
                                                      * plays the words left) */
            script = shell_script(&v, script);
        if (!v.shell_go)
            script = NULL;
        if (v.shell_go) {
            v.shell_go = 0;
            shell_start_game(&v);
        }
    } else if (open_city(&v, city, block, -1) != 0) {
        plat_message("The city's bundles are not in RAMPAGE.VOL.", 1);
        return 1;
    } else if (v.ap) {                               /* --autoplay straight into a city: the shell takes over after it,
                                                      * with this game as the one it chose */
        ShellGame *g = &v.last_game;
        g->play_type = play_type(&v);
        g->num_players = v.nmon;
        g->num_bots = v.num_bots;
        for (int k = 0; k < v.nmon && k < 4; k++) {
            int r = reveal_roster_find(&v.roster, v.monsters[v.mon[k].monster]);   /* MonsterData's index */
            g->monster[k] = r >= 0 ? r : 0;
            snprintf(g->monster_name[k], sizeof g->monster_name[k], "%s", v.monsters[v.mon[k].monster]);
        }
        g->city_index = campaign_index(CITIES[city]);
        snprintf(g->city, sizeof g->city, "%s", CITIES[city]);
        g->start_block = city_block_index(v.city, v.blocks[v.block]);   /* (a BlockList index, as the profile's) */
        g->night = night;
        v.kept.idle = SHELL_IDLE_SECONDS;            /* (no shell before it: not the attract mode after it) */
    }
    for (uint32_t i = 0; getenv("RAMPAGE_TRACE") && i < v.scene.nbuildings; i++) {
        const Building *b = &v.scene.buildings[i].b;
        printf("building %u: x %.0f .. %.0f, y %.0f .. %.0f, top %.1f, row %d, %d wide\n", i, b->min[0], b->max[0],
               b->min[1], b->max[1], b->max[2], b->row, b->width);
    }
    for (uint32_t i = 0; getenv("RAMPAGE_TRACE") && i < v.scene.nobstacles; i++) {
        const float *o = v.scene.obstacles[i];
        printf("obstacle %u: %.1f %.1f %.1f .. %.1f %.1f %.1f\n", i, o[0], o[1], o[2], o[3], o[4], o[5]);
    }
    Camera cam = { { 0, -110, 45 }, 3.14159265f / 2, -0.3f, 60 };
    if (have_monster(&v)) {                                /* closer, the monster in front */
        cam.pos[1] = -55; cam.pos[2] = 18; cam.pitch = -0.2f;
    }
    float speed = 30;
    double last = win_time(), title_at = last, start = last;
    /* the world's clock: the scene's clips that run on it (a soldier's run, a helicopter's rotor: a model's own
     * animation) and the texture animations (.ifl: the neon) go by it; pauseGame stops the world and its clock (App
     * +0xA0 & 0x10 skips the world's update, 0x288340), the GUI going on with the frame's time [R] */
    double world_t = 0;
    int frames = 0, settled = 0;                     /* a shot's game: its words played */
    while (!w.quit) {
        win_poll(&w);
        double now = win_time();
        float dt = (float)(now - last);
        last = now;
        int apf = 0;                                 /* --autoplay: the watchdog's asks this frame */
        if (v.ap) {                                  /* simulated time, 60 frames a second */
            dt = 1.0f / 60;
            v.ap_t += dt;
            now = start + v.ap_t;
        }
        if (!(v.hud && hud_paused(v.hud)))           /* (paused: the Pause menu, GameOver's pauseGame(1)) */
            world_t += dt;
        if (v.pads_live) {                           /* this frame's controllers, the vibration on them */
            memcpy(v.pads_prev, v.pads, sizeof v.pads);
            v.pads_on = pad_poll(v.pads, w.focused, (unsigned)w.dev_changes);
#ifdef __ANDROID__
            v.pads_on |= touch_pad(v.pads, v.pads_on);   /* the on-screen pad: slot 0's */
#endif
            pad_set_strength(v.vibrate ? 1.0f : 0.0f);
        }
        /* Esc (Android's back button): Backspace (back) in the menus, else P (Start: the Pause menu), as a game's;
         * the game is left from its menus (an app with Home). Only the bare viewer, with neither, quits on it */
        if (w.pressed[VK_ESCAPE]) {
            if (v.shell || v.game)
                w.pressed[v.shell || (v.hud && (hud_results_showing(v.hud) || hud_pause_showing(v.hud))) ? VK_BACK : 'P'] = 1;
#ifndef __ANDROID__
            else
                w.quit = 1;
#endif
        }
        settings_keys(&gfx, &w);                     /* F5 .. F9 the graphics, F11 / Alt+Enter fullscreen */
        if (v.shell) {                               /* the front end has the screen and the pad */
            if (v.shell_go) {                        /* the loading screen was drawn: the game */
                v.shell_go = 0;
                shell_start_game(&v);
                continue;
            }
            if (v.ap) {                              /* the autopilot's press, the shell's frame, the watchdog */
                autopilot_shell(&v, dt);
                shell_frame(&v, &w, dt);
                if (!((apf = autoplay_watch(&v)) & (AP_SHOT | AP_STOP)))
                    continue;                        /* nothing to draw */
            } else if (!shot)
                shell_frame(&v, &w, dt > 0.1f ? 0.1f : dt);
            static HudQuad sq[4096];
            v.shell_scene->hud = sq;
            v.gui_side = w.height ? fmaxf(0, (480.0f * (float)w.width / (float)w.height - 640) / 2) : 0;
            shell_set_wide(v.shell, v.gui_side);
            v.shell_scene->nhud = shell_quads(v.shell, sq, 4096);
            gui3d_layers(&v, v.shell, v.shell_scene);
            Camera none = { { 0, 0, 0 }, 0, 0, 60 };
            sound_update(shot ? 0 : (dt > 0.1f ? 0.1f : dt));
            rnd_hud_time(v.rnd, now - start);
            rnd_frame(v.rnd, &w, &none, now - start, 0);
            if (shot) {
                const char *to = apf & AP_STOP ? shot : v.ap ? ap_shot_path(v.ap) : shot;
                int ok = shot_png(to, w.width, w.height) == 0;
                printf(ok ? "wrote %s\n" : "cannot write %s\n", to);
                if (v.ap && !(apf & AP_STOP)) {
                    ap_shot_done(v.ap, ok);
                    continue;
                }
                break;
            }
            win_swap(&w);
            continue;
        }
        sound_follow_pause(&v);
        int menu_had_pad = v.hud && (hud_results_showing(v.hud) || hud_pause_showing(v.hud));
        if (v.hud && hud_results_showing(v.hud)) {   /* a results screen has the pad: Enter / Space go on,
                                                      * Backspace quits, the arrows choose, C changes monster, V
                                                      * saves */
            static const struct { int key, ev, button; } KEYS[] = {
                { VK_RETURN, HUD_EV_ACCEPT, 0 }, { VK_SPACE, HUD_EV_ACCEPT, 0 }, { VK_BACK, HUD_EV_BACK, 0 },
                { VK_UP, HUD_EV_UP, 0 }, { VK_DOWN, HUD_EV_DOWN, 0 }, { VK_LEFT, HUD_EV_LEFT, 0 },
                { VK_RIGHT, HUD_EV_RIGHT, 0 }, { 'C', HUD_EV_BUTTON, 0 }, { 'V', HUD_EV_BUTTON, 1 } };
            int popup = shell_popup_active(v.popup); /* isSaveGameActive: the popup has the pad (pad 0) */
            for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++)
                if (w.pressed[KEYS[k].key]) {
                    if (popup)
                        shell_input(v.popup, KEYS[k].ev, KEYS[k].button);
                    else
                        hud_input(v.hud, KEYS[k].ev, KEYS[k].button);
                }
            for (int k = 0; k < PAD_SLOTS; k++) {    /* and the controllers, each as its own pad */
                int m = pad_menu(&v, k, dt > 0.1f ? 0.1f : dt);
                for (size_t i = 0; m && i < sizeof PAD_MENU_EV / sizeof *PAD_MENU_EV; i++)
                    if (m & PAD_MENU_EV[i].bit) {
                        if (popup)
                            shell_input_pad(v.popup, PAD_MENU_EV[i].ev, PAD_MENU_EV[i].button, k);
                        else
                            hud_input_pad(v.hud, PAD_MENU_EV[i].ev, PAD_MENU_EV[i].button, k);
                    }
            }
        } else if (v.hud && hud_pause_showing(v.hud)) {   /* the Pause menu has the pad: the arrows choose, Enter /
                                                      * Space take, Backspace back, P (Start) takes too */
            static const struct { int key, ev; } KEYS[] = {
                { VK_RETURN, HUD_EV_ACCEPT }, { VK_SPACE, HUD_EV_ACCEPT }, { VK_BACK, HUD_EV_BACK },
                { VK_UP, HUD_EV_UP }, { VK_DOWN, HUD_EV_DOWN }, { VK_LEFT, HUD_EV_LEFT }, { VK_RIGHT, HUD_EV_RIGHT },
                { 'P', HUD_EV_START } };
            for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++)
                if (w.pressed[KEYS[k].key])
                    hud_input(v.hud, KEYS[k].ev, 0);
            for (int k = 0; k < PAD_SLOTS; k++) {    /* and the controllers (its Options page answers pad 0) */
                int m = pad_menu(&v, k, dt > 0.1f ? 0.1f : dt);
                for (size_t i = 0; m && i < sizeof PAD_MENU_EV / sizeof *PAD_MENU_EV; i++)
                    if (m & PAD_MENU_EV[i].bit)
                        hud_input_pad(v.hud, PAD_MENU_EV[i].ev, PAD_MENU_EV[i].button, k);
            }
            int held = pad0_held(&v);
            hud_pad_held(v.hud, pad0_x(&v), w.keys[VK_LEFT] || (held & SHELL_HELD_LEFT),
                         w.keys[VK_RIGHT] || (held & SHELL_HELD_RIGHT));
        } else if (v.hud && v.play && (w.pressed['P'] || w.pressed[VK_RETURN]))   /* Start: the Pause menu */
            pause_open(&v, 0);
        else if (v.hud && v.play)                    /* or a controller's Start, that pad's */
            for (int k = 0; k < PAD_SLOTS; k++)
                if (pad_went_down(&v, k, PAD_START) && pause_open(&v, k))
                    break;
        if (v.attract && attract_buttons(&v, &w))     /* the demo: a button ends it */
            attract_press(&v);
        if (v.exit_game) {                           /* back to the shell (--shell), or the viewer ends */
            v.exit_game = 0;
            /* [R 0x2B7CB0, RWorldPackage's vtable 0x44A820 +0x18] the world package's end: every 3D sound killed
             * (0x26D500) and the sound's frame once (0x26D960), then setState(15): the block's loops (a broken
             * hydrant's Geyser, a fire's Fire Loop) and the objects' sounds do not play on in the menus */
            sound_objects_clear();
            if (v.shell_mode || v.ap) {              /* (--autoplay: the shell goes on after a city) */
                ShellStart st = v.kept;              /* gShell's idle timer, attract state, demo byte */
                ap_game_left(v.ap, v.attract);
                st.from = SHELL_FROM_GAME;           /* and the flags the world left in gCommon [R] (no game: the
                                                      * main menu) */
                st.quit = v.game ? game_quitting(v.game) : 1;
                st.choose_monster = game_choose_monster(v.game);
                st.city_destroyed = game_city_destroyed(v.game);
                st.show_victory = game_world_done(v.game);
                st.king_of_world = game_king(v.game, 2);
                st.game = v.last_game;
                if (v.attract)
                    attract_end(&v, &st);
                v.play = 0;
                shell_enter(&v, &st);
                if (shot && script) {                /* the words left play on in the shell */
                    script = shell_script(&v, script);
                    if (v.shell_go) {
                        v.shell_go = 0;
                        settled = 0;
                        shell_start_game(&v);
                    }
                }
                continue;
            }
            w.quit = 1;
        }
        game_next_block(&v);                         /* the game moved on to the next block */
        if (w.pressed[VK_NEXT] && v.nblocks) { v.block = (v.block + 1) % v.nblocks; build(&v); }
        if (w.pressed[VK_PRIOR] && v.nblocks) { v.block = (v.block + v.nblocks - 1) % v.nblocks; build(&v); }
        if (w.pressed['N']) { v.night = !v.night; build(&v); }
        if (w.pressed['F']) v.fog = !v.fog;
        if (w.pressed['H']) v.show_hits = !v.show_hits;
        if (w.pressed['M']) {                        /* player 1's monster: the next one */
            v.mon[0].monster = v.mon[0].monster + 1 < (int)v.nmonsters ? v.mon[0].monster + 1 : -1;
            for (int k = 1; k < v.nmon; k++)         /* (the others keep theirs; with none, none at all) */
                if (v.mon[k].monster < 0)
                    v.mon[k].monster = v.mon[0].monster;
            v.clip = -1;
            build(&v);
        }
        if (w.pressed['G'] && have_monster(&v)) {
            v.play = !v.play;
            cam_init(&v.cam, &v.cam_settings);
            dircam_init(&v.cd, &v.cam);
            v.view = v.cd.out;
            for (int k = 0; v.play && k < v.nmon; k++)
                if (v.mon[k].have_graph)
                    play_state(&v.mon[k]);
        }
        if (have_monster(&v) && !v.play && (w.pressed['K'] || w.pressed['L'])) {
            v.clip = w.pressed['L'] ? v.clip + 1 : (v.clip > 0 ? v.clip - 1 : 0);
            build(&v);
        }
        for (int i = 0; i < NCITIES; i++)
            if (w.pressed['1' + i] && i != v.city_index)
                open_city(&v, i, 0, -1);
        if (w.wheel) speed *= powf(1.25f, (float)w.wheel);
        cam.yaw -= w.mouse_dx * 0.005f;
        cam.pitch -= w.mouse_dy * 0.005f;
        if (cam.pitch > 1.5f) cam.pitch = 1.5f;
        if (cam.pitch < -1.5f) cam.pitch = -1.5f;
        int playing = v.play && have_monster(&v) && v.have_scene && mon_in(&v.mon[0]);
        if (playing) {
            float aspect = w.width ? (float)w.height / w.width : 0.75f;
            if (shot && !settled) {                     /* a shot: the camera settles, then the script */
                if (!v.ap)                           /* (--autoplay: its words first, then it plays on) */
                    play_script(&v, "wait 2", aspect);
                if (script)
                    script = play_script(&v, script, aspect);
            }
            settled = 1;
            if (v.exit_game)                         /* the script quit the game: the shell's turn */
                continue;
            for (int k = 0; k < v.nmon; k++)         /* the pads of the players on keys and controllers (player k
                                                      * on slot k) */
                if (v.mon[k].drive != DRIVE_COMPUTER) {
                    keys_pad(&w, v.mon[k].drive, &v.mon[k].pad);
                    if (v.pads_live && k < PAD_SLOTS && (v.pads_on >> k & 1))
                        pad_to_pad(&v.pads[k], &v.pads_prev[k], &v.mon[k].pad);
                    if (menu_had_pad)                /* the press that took the Pause menu's Continue was the
                                                      * menu's: not a jump too (one on a wall lets go of it) */
                        memset(v.mon[k].pad.pressed, 0, sizeof v.mon[k].pad.pressed);
                }
            play_step(&v, shot && !v.ap ? 0 : (dt > 0.1f ? 0.1f : dt), aspect);
            pose_monsters(&v, shot && !v.ap ? shot_time : world_t);   /* the clips at the states' times (clip_at),
                                                      * against the clock the scene poses by (scene_update's t) */
            memcpy(cam.pos, v.view.pos, sizeof cam.pos);
            cam_shaken(v.view.pos, (const float (*)[3])v.view.rows, v.shakes, 8 + MONSTERS - 1, cam.rows);   /* after the
                                                      * smoothing */
            cam.use_rows = 1;
            cam.fov_deg = v.view.fov * 57.2957795f;
        } else {
            if (v.have_scene) {
                world_build(&v);                     /* the traffic's and the parade's world */
                scene_buildings_update(&v.scene, dt > 0.1f ? 0.1f : dt);
                v.scene.cloth_dt += dt > 0.1f ? 0.1f : dt;   /* the flags fly in the free camera too */
                if (v.veh)                           /* and the traffic drives on, with no monster about */
                    vehicle_update(v.veh, dt > 0.1f ? 0.1f : dt, NULL, 0, NULL, 0);
                if (v.parade)                        /* and the balloons drift, the bikers ride */
                    parade_update(v.parade, dt > 0.1f ? 0.1f : dt, NULL, 0, cam.pos[0]);
            }
            if (cam.use_rows) {                        /* back to the free camera where the game camera was */
                cam.use_rows = 0;
                cam.fov_deg = 60;
                cam.yaw = atan2f(cam.rows[1][1], cam.rows[1][0]);
                cam.pitch = asinf(cam.rows[1][2]);
            }
            float step = speed * dt * (w.keys[VK_SHIFT] ? 4.0f : 1.0f);
            float fx = cosf(cam.yaw), fy = sinf(cam.yaw);
            if (w.keys['W']) { cam.pos[0] += fx * step; cam.pos[1] += fy * step; }
            if (w.keys['S']) { cam.pos[0] -= fx * step; cam.pos[1] -= fy * step; }
            if (w.keys['A']) { cam.pos[0] -= fy * step; cam.pos[1] += fx * step; }
            if (w.keys['D']) { cam.pos[0] += fy * step; cam.pos[1] -= fx * step; }
            if (w.keys['E']) cam.pos[2] += step;
            if (w.keys['Q']) cam.pos[2] -= step;
        }
        double t = shot && !v.ap ? shot_time : world_t;
        rnd_hud_time(v.rnd, shot && !v.ap ? shot_time : now - start);   /* the GUI's 3D layers: the frame's time */
        sound_listener(cam.pos, (const float (*)[3])cam.rows);   /* the camera (its rows when it is the game's) */
        sound_update(shot ? 0 : (dt > 0.1f ? 0.1f : dt));
        if (v.have_scene) {
            scene_update(&v.scene, t);
            if (playing)
                hit_spheres_all(&v);
            else
                v.scene.nspheres = 0;
        }
        if (v.ap && !v.shell && !((apf = autoplay_watch(&v)) & (AP_SHOT | AP_STOP)))
            continue;                                /* --autoplay: nothing to draw */
        {                                            /* this frame's particles */
            static FxQuad quads[8192];
            v.scene.fx = quads;
            v.scene.nfx = v.fxw ? fx_quads(v.fxw, quads, 8192) : 0;
            shadow_frame(&v, shot ? 1.0f : (dt > 0.1f ? 0.1f : dt));
            if (shot && getenv("RAMPAGE_TRACE")) {
                int np[3] = { 0, 0, 0 };
                for (int k = 0; k < v.shl.nb; k++)
                    np[v.shl.b[k].pass] += v.shl.b[k].count / 3;
                printf("shadows: %d ground, %d decal, %d wall triangles; grid %s, blob fade %.2f\n", np[0], np[1], np[2],
                       v.sg.enabled ? "on" : "off", v.mon[0].smon.ground_fade);
            }
            static FxMesh fxm[1024];
            v.scene.fxm = fxm;
            v.scene.nfxm = v.fxw ? fx_meshes(v.fxw, fxm, 1024) : 0;
            v.scene.fx_shapes = (const SceneMesh (*)[8])v.fx_shapes;
            v.scene.fx_nshapes = v.fx_nshapes;
            if (shot && getenv("RAMPAGE_TRACE") && v.fxw)
                printf("particles %d, quads %d (first at %.1f %.1f %.1f, size %.2f, alpha %.2f)\n", fx_particles(v.fxw),
                       v.scene.nfx, v.scene.nfx ? quads[0].pos[0] : 0, v.scene.nfx ? quads[0].pos[1] : 0,
                       v.scene.nfx ? quads[0].pos[2] : 0, v.scene.nfx ? quads[0].size[0] : 0, v.scene.nfx ? quads[0].col[3] : 0);
        }
        {                                            /* the HUD, through this frame's camera */
            static HudQuad hq[8192];
            if (have_view) {
                Camera fixed = { { view[0], view[1], view[2] }, view[3] / 57.2957795f, view[4] / 57.2957795f, 60 };
                v.hud_cam = fixed;
            } else
                v.hud_cam = cam;
            v.scene.hud = hq;
            hud_set_aspect(w.height ? (float)w.width / w.height : 0);   /* (wider than 4:3: the play HUD to the
                                                      * window's sides, the dimmers across it) */
            int nq = 0;                              /* the other players' HUDs, then player 1's (the screens) */
            for (int k = 1; k < v.nmon; k++)
                if (v.mon[k].hud && v.mon[k].hud != v.hud)
                    nq += hud_quads(v.mon[k].hud, hq + nq, 8192 - nq);
            nq += v.hud ? hud_quads(v.hud, hq + nq, 8192 - nq) : 0;
            if (shell_popup_shown(v.popup))          /* the SaveGame popup over the screens */
                nq += shell_quads(v.popup, hq + nq, 8192 - nq);
            v.scene.nhud = nq;
            gui3d_layers(&v, shell_popup_shown(v.popup) ? v.popup : NULL, &v.scene);
        }
        if (have_view) {
            Camera fixed = { { view[0], view[1], view[2] }, view[3] / 57.2957795f, view[4] / 57.2957795f, 60 };
            rnd_frame(v.rnd, &w, &fixed, t, v.fog);
        } else
            rnd_frame(v.rnd, &w, &cam, t, v.fog);
        if (shot) {
            if (cam.use_rows)
                printf("camera at %.2f %.2f %.2f, forward %.3f %.3f %.3f, up %.3f %.3f %.3f, tilt %.3f, state %d\n",
                       cam.pos[0], cam.pos[1], cam.pos[2], cam.rows[1][0], cam.rows[1][1], cam.rows[1][2],
                       cam.rows[2][0], cam.rows[2][1], cam.rows[2][2], v.cam.tilt, v.cam.state);
            const char *to = apf & AP_STOP ? shot : v.ap ? ap_shot_path(v.ap) : shot;
            int ok = shot_png(to, w.width, w.height) == 0;
            printf(ok ? "wrote %s\n" : "cannot write %s\n", to);
            if (v.ap && !(apf & AP_STOP)) {
                ap_shot_done(v.ap, ok);
                continue;
            }
            break;
        }
        if (w.pressed[VK_F12]) {
            static int n;
            char name[32];
            snprintf(name, sizeof name, "shot_%03d.png", ++n);
            if (shot_png(name, w.width, w.height) == 0)
                printf("wrote %s\n", name);
        }
        win_swap(&w);
        frames++;
#if defined RAMPAGE_GUI && defined _WIN32
        if (now - title_at > 0.5)
            gui_log_bound();
#endif
        if (now - title_at > 0.5 && DEBUG_TITLE) {
            char title[256], fx[96];
            settings_describe(&gfx, fx, sizeof fx);
            snprintf(title, sizeof title, "RampageRE - %s, %s%s%s%s - %u triangles, %.0f fps%s%s", CITIES[v.city_index],
                     v.nblocks ? v.blocks[v.block] : "-", v.night ? ", night" : "",
                     have_monster(&v) ? ", " : "", have_monster(&v) && v.scene.mon[0].clip ? v.scene.mon[0].clip : "",
                     rnd_triangles(v.rnd), frames / (now - title_at), v.fog ? ", fog" : "", fx);
            win_title(&w, title);
            title_at = now;
            frames = 0;
        }
    }
    popup_free(&v);
    rnd_free(v.rnd);
    for (int k = 0; k < MONSTERS; k++)
        if (v.mon[k].have_graph) {
            moves_stop(&v.mon[k].run);
            moves_free(&v.mon[k].graph);
        }
    if (v.have_scene)
        scene_free(&v.scene);
    city_close(v.city);
    sound_end();
    sound_free();
    win_close(&w);
    return v.ap ? ap_close(v.ap, NULL) : 0;          /* --autoplay: 3 when it saw problems */
}

int main(int argc, char **argv)                      /* (Android: SDL_main, android/app/CMakeLists.txt) */
{
#if defined RAMPAGE_GUI && defined _WIN32
    gui_start(argc, argv);
    int r = viewer_main(argc, argv);
    fflush(stdout);
    if (r != 0 && !plat_quiet && !plat_told)         /* nothing said yet: say where to look */
        MessageBoxA(NULL, g_gui_log ? "RampageRE could not start. rampage.log (beside rampage.cfg) says why." :
                    "RampageRE could not start.", "RampageRE", MB_ICONERROR);
    return r;
#else
    return viewer_main(argc, argv);
#endif
}
