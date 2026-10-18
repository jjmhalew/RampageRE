/* shell.h - the game's front end ("the shell", RShellPackage), natively: the ControlRoom and its interfaces from
 * Shell.bdp (AttractMode, MainMenu with its number-of-players and number-of-bots boxes, the cheat entry and the retro
 * games box, MonsterSelect, MonsterSheet, CitySelect, Options), the profile select / create popup (MemoryCard.ifc of
 * MemoryCard.bdp, the SaveGame class' native code), the loading screen (Loading.ifc of Loading_<City>.bdp) and the
 * Victory interface (VShell.bdp), run by the logic of their scripts (build/scripts/ControlRoom.scr, AttractMode.scr,
 * MainMenu.scr, MonsterSelect.scr, MonsterSheet.scr, CitySelect.scr, Options.scr, Loading.scr, Victory.scr, Shell.scr)
 * and the gShell / gCommon / GameStatus bindings they call. docs/SHELL_NOTES.md has the details ([R] read in the code
 * or the files, [G] a guess or a stand-in).
 *
 * The movies (.pss) are the viewer's to play (movie.c): where one plays, an event SHELL_EVT_MOVIE names it; the viewer
 * says it plays it (shell_movie_hold), gives its picture to the Video quad (shell_movie_picture, the HudQuad's tex
 * HUD_TEX_VIDEO) by shell_movie_clock's time and ends it (shell_movie_end). One nobody holds lasts
 * shell_set_movie_time's seconds of black (0: skipped at once). The arcade emulators (DESI/JARVOS.ELF, M1.ELF) are
 * skipped (SHELL_EVT_EMULATOR). The 3D parts of the interfaces (CitySelect's globe and its cities' rings, the .edf
 * particle effects: the main menu's fires, the cards' glow, the lava lamp) are the viewer's to draw: shell_3d() lists
 * them, each where a HudQuad with tex HUD_TEX_3D stands in shell_quads(), with its world in the GUI camera's.
 *
 * Use: shell_open() with the volume, how the shell starts (the boot, or back from a game) and the profile as the viewer
 * knows it; every frame shell_input*() for the pad, shell_update(dt), shell_events() for what it asks of the viewer
 * (sounds, music, profile load / create / save, the movies, the start of a game) and shell_quads() for what to draw.
 * The profile's file is the viewer's: the popup asks for each read and write (SHELL_EVT_PROFILE_LOAD / _CREATE /
 * _SAVE) as the memory card's job, and the viewer answers each with shell_card_result() before the next update. The
 * game's screens use the popup alone (shell_open_popup): Results' saveProfile, the SaveIcon while it writes.
 * When SHELL_EVT_START comes, shell_game() is the game to play (gCommon.getGameStatus(): the play type, players,
 * bots, each player's monster, the city, the start block, day or night); the loading screen stays drawn until the
 * viewer closes the shell. Nothing here touches OpenGL or plays sounds.
 *
 * Drawing: as hud.h (the GUI's 640 x 480 reference screen, y down, back to front, alpha blended, the texture times
 * col). shell_quads() gives HudQuads (axis-aligned: a turned picture is drawn upright in its bounds, as hud.c does);
 * shell_quads_turned() gives the exact four corners of each (the loading screen's picture is turned 27 degrees, the
 * cards squash while they flip, the "LOCKED" stamps and the cheat box's "Code:" are turned). */
#ifndef RAMPAGE_SHELL_H
#define RAMPAGE_SHELL_H
#include <stdint.h>
#include "data.h"
#include "hud.h"

#define SHELL_MONSTERS 30           /* MonsterData.Monster0 .. 29 of ShellData.bdp */
#define SHELL_CITIES 7              /* CityData.City0 .. 6: LasVegas SanFrancisco London LosAngeles Chicago HongKong
                                     * NewYork (the campaign's order) */
#define SHELL_PROFILES 16           /* profiles on the "memory card" the popup lists */

/* the profile, as the viewer knows it (src/profile.c fills it; the shell keeps a copy). The layout of the game's
 * profile is not needed here [R the bindings that read it: gCommon.getMonsterFlags 0x33D760, Profile.getCityFlags,
 * getStartBlock, gCommon.getSoundVolume / getMusicVolume / getEnableRumble, getCheatFlags, hasLoadedProfile,
 * getAllowSave, getSaveFilesExist] */
typedef struct {
    int loaded;                     /* gCommon.hasLoadedProfile(): one was loaded or created */
    int allow_save;                 /* gCommon.getAllowSave(): the popup's flag 4 */
    int was_saving;                 /* its flag 8: set with 4 by a load (and a save the card id changed for), cleared
                                     * only by warn-continue's Continue / Don't Save */
    int profile_shown;              /* 0x4D7FF8 & 0x200: a popup closed with a result but 3 (getShowProfile() 0) */
    int offer_create;               /* port extra (profile_autoload): no profile file yet, so the main menu's first focus
                                     * opens the list's Create New Profile, once a launch */
    char name[16];                  /* its name (the popup shows it) */
    /* gCommon.getMonsterFlags(player, monster): 0x80 unlocked, 1 2 4 8 the four upgrades (Charge Punch, Double Punch,
     * Roar Attack, Jump Smash); player 0 the profile, 1 .. 3 a buddy's profile when loaded, else the profile's [R
     * 0x33D760: GameStatus +0x2B7 + 31 n says whether player n has its own] */
    uint8_t monster_flags[4][SHELL_MONSTERS];
    uint8_t city_flags[SHELL_CITIES];   /* Profile.getCityFlags(i): 0x80 unlocked, a star each for 1 all blocks, 2 the
                                         * par score, 4 the challenges, 8 the monsters revealed */
    int start_block[SHELL_CITIES];      /* Profile.getStartBlock(i): the campaign's checkpoint in that city */
    float sound, music;             /* 0 .. 0.75 (the sliders' 0 .. 100) */
    int rumble;
    uint32_t cheats;                /* gCommon.getCheatFlags() */
    int changed;                    /* saveProfile's test (0x33E2D0): the block or its status changed */
    int nprofiles;                  /* the profiles saved (the popup's list; getSaveFilesExist = nprofiles > 0) */
    char profiles[SHELL_PROFILES][16];
    int language;                   /* hud.h's languages: 0 English .. 8 Portuguese */
} ShellProfile;

/* gCommon's play state and gCommon.getGameStatus() as the shell hands them to the game [R GameStatus bindings, lookup
 * 0x2C4C30: setMonsterIdx, setCityIdx, setStartBlock, setNight; the Common bindings setPlayType, setNumPlayersAndBots] */
typedef struct {
    int play_type;                  /* game.h GP_*: 2 campaign, 3 King of the City, 4 King of the World, 5 Timed Run;
                                     * 0 the attract mode's demo (gShell.startAttractGame) */
    int num_players, num_bots;      /* 1 .. 4; the last num_bots of the players are the computer's (the demo: all) */
    int monster[4];                 /* MonsterData.Monster<n> per player */
    char monster_name[4][16];       /* its Name ("George") */
    int city_index;                 /* CityData.City<n> (the campaign's order, above) */
    char city[16];                  /* its City ("LasVegas") */
    int start_block;                /* GameStatus +0x210: 0 = the city's BlockStart, else that block */
    int night;                      /* GameStatus.getNight */
    uint32_t seed[2];               /* GameStatus +0x0 / +0x4, the world's seeds: the shell draws new ones at each
                                     * world's start, but not for the demo, whose own it keeps (0x308890) [R] */
} ShellGame;

/* how the shell starts [R Shell.scr, ControlRoom.scr's choice of the first interface, RShellPackage 0x308950] */
enum { SHELL_FROM_BOOT = 0, SHELL_FROM_GAME = 1 };
typedef struct {
    int from;
    /* back from a game: the flags the world left in gCommon (game.h GR_* and its exits) */
    int quit;                       /* isPlayerQuiting (a confirmed quit): the main menu */
    int choose_monster;             /* isChooseMonster (GameOver's / Results' "select"): the monster select */
    int city_destroyed;             /* isCityDestroyed: the city select (the next city) */
    int king_of_world;              /* getKingOfWorld(): -1 none; King of the World with a destroyed city and no
                                     * King: straight to the loading screen of the next city */
    int show_victory;               /* the campaign was won: the Victory and Credits movies, then the main menu */
    ShellGame game;                 /* the game status kept from before (the shell starts with these choices) */
    /* what gShell (0x4D51F0) keeps while a game plays, from shell_keep() at the start: the shell is closed here */
    float idle;                     /* +0x18, the idle timer: 0 or less and the attract mode comes back (0x3089AC);
                                     * the demo's end by a button sets it to SHELL_IDLE_SECONDS (0x324CC8) */
    int attract_state;              /* +0x14, AttractMode.scr's state */
    int attract_game;               /* +0x20: the game was the attract mode's demo (the viewer restores the profile,
                                     * restoreProfile 0x33D910 at 0x308A00) */
} ShellStart;
#define SHELL_IDLE_SECONDS 120.0f   /* [R 0x308240: 120.0 into gShell +0x18] */

typedef struct Shell Shell;
/* vol: RAMPAGE.VOL, read for Shell.bdp, VShell.bdp, ShellData.bdp, CommonGUI.bdp, MemoryCard.bdp, Loading.bdp and
 * Loading_<City>.bdp; it must outlive the Shell. st NULL: the boot. NULL when the bundles are missing. */
Shell *shell_open(const Vol *vol, const ShellStart *st, const ShellProfile *p);
void shell_close(Shell *s);
/* when a game starts: ShellStart's idle, attract_state and attract_game as gShell has them, for the return */
void shell_keep(const Shell *s, ShellStart *st);
/* the profile changed (after SHELL_EVT_PROFILE_LOAD / _CREATE, or a buddy's for player n): the shell takes a copy */
void shell_set_profile(Shell *s, const ShellProfile *p);
/* ShellProfile.profile_shown as the shell has it now (keep it for the next shell_open) */
int shell_profile_shown(const Shell *s);
/* seconds a movie "plays" (a black screen) where the game would play a .pss; 0 (default): skipped at once */
void shell_set_movie_time(Shell *s, float seconds);
/* the memory card's free room in KB (PS2MemoryCardManager +0x4C: sceMcGetInfo's free), 0 none; a save needs 140
 * (SHELL_CARD_NEED_KB). Unset: room enough */
void shell_set_card_free(Shell *s, int kb);
#define SHELL_CARD_NEED_KB 140
/* the card's answer to the last SHELL_EVT_PROFILE_LOAD / _CREATE / _SAVE, as PS2MemoryCardManager's statuses [R the
 * sceMcRead / sceMcWrite results 0x1A1550, 0x1A1B98: -4 -> 6, below -9 -> 7, -3 -> 11; the data's CRC -> 18,
 * 0x1A19B0]: OK; NOT_FOUND the file is not there (a read); IO it could not be read or written; NO_SPACE no room for
 * it; CORRUPT a file that is not a profile (its sizes, magic or CRC). The popup shows the game's message for each */
enum { SHELL_CARD_OK = 0, SHELL_CARD_NOT_FOUND = 6, SHELL_CARD_IO = 7, SHELL_CARD_NO_SPACE = 11,
       SHELL_CARD_CORRUPT = 18 };
void shell_card_result(Shell *s, int status);
/* gCommon.saveProfile() for a profile that changed (the viewer's test, 0x33E2D0) [R 0x345950]: with autosave off
 * (allow_save 0) nothing; else the popup's save (state 7): the SaveIcon alone, then SHELL_EVT_PROFILE_SAVE */
void shell_save_profile(Shell *s);
/* the SaveGame popup alone, for the game's screens (Results' saveProfile): MemoryCard.bdp read from vol, the fonts
 * and frames of common (the viewer's CommonGUI.bdp; it must outlive the Shell). Nothing but the popup runs and draws;
 * NULL when MemoryCard.bdp is missing */
Shell *shell_open_popup(const Vol *vol, const Bundle *common, const ShellProfile *p);
/* gCommon.isSaveGameActive() [R 0x2C2F30: the popup's state 0x4D813C is not 1]: the popup has the pad */
int shell_popup_active(const Shell *s);
/* the popup is still drawn: active, or closed and fading out with its SaveIcon playing out [R 0x341CD8] */
int shell_popup_shown(const Shell *s);
float shell_popup_fade(const Shell *s);   /* the popup's interface fade (+0x5C): 1 once it has faded in */
/* the movie SHELL_EVT_MOVIE named is played: it lasts until shell_movie_end (or the pad skips it) */
void shell_movie_hold(Shell *s);
void shell_movie_end(Shell *s);              /* it is over (Video.status() 3): the shell goes on */
float shell_movie_clock(const Shell *s);     /* seconds since the playing movie started, -1 none (ended, skipped) */
void shell_movie_picture(Shell *s, int on);  /* the Video quad shows the movie's picture (1) or black (0) */

/* the pad, as GuiEvent ids: hud.h HUD_EV_* (2 right, 4 left, 8 up, 16 down, 32 back, 64 accept, 128 start, 256 a
 * button). The buttons [R the scripts' use and the layouts' pictures]: 0 = CONTROLLER_B0 (square: the main menu's
 * Retro games, MonsterSelect's flip card, CitySelect's stats), 1 = CONTROLLER_B1 (triangle: Options, the card sheet,
 * MonsterSheet's zoom), 4 = TRIGGER_RIGHT (MonsterSelect's "load monsters" of another player's profile) */
void shell_input(Shell *s, int ev, int button);
void shell_input_pad(Shell *s, int ev, int button, int pad);   /* GuiEvent.getController(): 0 = player 1 */
/* pad 0 held, every frame (gGuiMgr.getController(0): getX(0), getButtons()): the stick across -1 .. 1 and the held
 * buttons, SHELL_HELD_*. Options' sliders move while the d-pad's left / right are held; L2 and R2 held together, and
 * nothing else, on the main menu open the cheat entry [R MainMenu::updateCheatActivation; the bits: getButtons
 * 0x16A180 over the names at 0x3F67C0 (Up 1, Down 2, Left 4, Right 8, Start 16, Back 32, RTrigger 64, LTrigger 128,
 * ...), the PS2 pad's L2 / R2 the triggers (0x1A5220, table 0x3B18B0)] */
enum { SHELL_HELD_UP = 1, SHELL_HELD_DOWN = 2, SHELL_HELD_LEFT = 4, SHELL_HELD_RIGHT = 8, SHELL_HELD_START = 16,
       SHELL_HELD_BACK = 32, SHELL_HELD_R2 = 64, SHELL_HELD_L2 = 128, SHELL_HELD_R1 = 256, SHELL_HELD_L1 = 512,
       SHELL_HELD_CROSS = 1024, SHELL_HELD_CIRCLE = 2048, SHELL_HELD_SQUARE = 4096, SHELL_HELD_TRIANGLE = 8192,
       SHELL_HELD_R3 = 65536, SHELL_HELD_L3 = 131072 };   /* getButtons' (0x16A180) bits: the names at 0x3F67C0, in
                                                      * turn (16384 W and 32768 Z: no PS2 button) */
void shell_pad_held(Shell *s, float stick_x, int buttons);
/* a convenience the game does not have: a character typed into the profile name entry ('\b' rubs one out, '\r' is
 * DONE) when it shows; returns 1 when it took it */
int shell_type(Shell *s, int ch);

void shell_update(Shell *s, float dt);   /* seconds */

/* what the shell asks of the viewer since the last call */
enum {
    SHELL_EVT_SOUND,                /* gSndDev.playSound(name) */
    SHELL_EVT_MUSIC,                /* gSndDev.replaceMusic(name, 0.5, 0.5): "Oingo" in the menus, "" none */
    SHELL_EVT_MUSIC_VOLUME,         /* gSndDev.setMusicVolume(value, arg ms) */
    SHELL_EVT_SFX_VOLUME,           /* gSndDev.setSfxVolume(value, arg ms) */
    SHELL_EVT_RUMBLE,               /* gCommon.setEnableRumble(arg != 0, 0) */
    SHELL_EVT_RUMBLE_PULSE,         /* gCommon.rumble(arg ms) */
    SHELL_EVT_SAVE_SETTINGS,        /* Options' Accept: gCommon.setSoundVolume / setMusicVolume / setEnableRumble(.., 1)
                                     * and saveProfile: read them with shell_settings */
    SHELL_EVT_MOVIE,                /* a .pss would play here (name: "midway", "pipeworks", "digital", "intro",
                                     * "Victory", "Credits", "victory", "credits", "ballers") */
    SHELL_EVT_VICTORY_SHOWN,        /* gCommon.setVictoryShown() (0x2C3610): Victory.scr, as its first movie starts */
    SHELL_EVT_EMULATOR,             /* gShell.startEmulator(arg): 0 Rampage, 1 Rampage World Tour (skipped) */
    SHELL_EVT_ATTRACT_GAME,         /* gShell.startAttractGame() (0x308460): the attract mode's demo game, chosen at
                                     * random into shell_game() (play type 0, every player the computer's); the
                                     * loading screen follows, then SHELL_EVT_START */
    SHELL_EVT_PROFILE_LOAD,         /* the popup reads saved profile arg (ShellProfile.profiles[arg], name): load it,
                                     * call shell_set_profile and answer shell_card_result */
    SHELL_EVT_PROFILE_CREATE,       /* the popup writes a new profile `name` (the name entry; an overwrite): create
                                     * it, write it, shell_set_profile and answer shell_card_result */
    SHELL_EVT_PROFILE_SAVE,         /* the popup writes the profile as it is (saveProfile): write it and answer
                                     * shell_card_result */
    SHELL_EVT_PROFILE_NONE,         /* the popup's "continue without a profile": nothing is saved (allow_save 0) */
    SHELL_EVT_BUDDY_PROFILE,        /* gShell.loadBuddyProfile(arg): player arg's own monsters from another profile;
                                     * answer with shell_buddy_profile */
    SHELL_EVT_CHEATS,               /* gCommon.setCheatFlags / setCheatFlagsI / clearCheatFlags: arg the new flags,
                                     * value the parameter of flag 0x80000000 (the start block) or 16 (the video) */
    SHELL_EVT_START,                /* gShell.startGame() (shell state 4): play shell_game() */
    SHELL_EVT_GRAPHICS              /* port extra, Options' Graphics page: shell_graphics() changed (arg 0: to try, or
                                     * back to what the page opened with; 1: the page's Accept, to keep) */
};
typedef struct { int kind; int arg; float value; char name[32]; } ShellEvent;
int shell_events(Shell *s, ShellEvent *out, int max);
const ShellGame *shell_game(const Shell *s);
void shell_settings(const Shell *s, float *sound, float *music, int *rumble);
/* port extra (docs/NATIVE.md "Graphics settings"): the graphics settings the Options' Graphics page shows, in
 * settings.h's values (smaa 0 .. 4, msaa 0 2 4 8, aniso 1 2 4 8 16, the rest 0 / 1). The viewer gives the ones in use
 * every frame (the page takes them as it opens); shell_graphics is the page's, after SHELL_EVT_GRAPHICS */
typedef struct { int ao, smaa, msaa, aniso, mips, fullscreen, vsync; } ShellGraphics;
void shell_set_graphics(Shell *s, const ShellGraphics *g);
const ShellGraphics *shell_graphics(const Shell *s);
/* MonsterSelect::onGetBuddyProfileCallback(player): the buddy profile for player n was loaded (its monster flags in
 * the profile's monster_flags[n], set them with shell_set_profile first), or -1: none */
void shell_buddy_profile(Shell *s, int player);

/* gShell.getState() [R 0x4D51F0, set by 0x308950 and the bindings]: 1 the boot (memory card check), 2 the attract
 * mode, 3 the menus, 4 a game starts, 6 the Victory movies */
int shell_state(const Shell *s);
/* the interface that has the pad: "AttractMode", "MainMenu", "MonsterSelect", "MonsterSheet", "CitySelect",
 * "Options", "MemoryCard" (the profile popup), "Loading", "Victory" */
const char *shell_focus(const Shell *s);
/* where the menus stand, for the viewer's autopilot (--autoplay; no game meaning): MainMenu's box (0 the play modes,
 * 1 the players, 2 the bots, 4 the retro games, 5 the cheat entry), its play mode (2 .. 5) and the two lists'
 * selections; MonsterSelect's players ready (bit n: player n) of ms_players; CitySelect's city (the campaign's order)
 * and whether it is locked; the popup's panel (0 none, 1 the list, 2 the name entry, 3 a message) and message;
 * AttractMode.scr's state */
typedef struct {
    int mm_state, mm_play_mode, mm_players, mm_bots;
    int ms_ready, ms_players;
    int cs_index, cs_locked;
    int mc_mode, mc_msg;
    int attract_state;
} ShellAutoView;
void shell_auto_view(const Shell *s, ShellAutoView *out);

typedef struct {
    float x[4], y[4];               /* the corners in the reference screen: the picture's top-left, top-right,
                                     * bottom-right, bottom-left */
    float u[4], v[4];               /* texture coordinates at them (over the whole decoded texture) */
    float col[4];
    const Bundle *b;                /* NULL: untextured */
    uint32_t tex;
    int additive;                   /* M 0x80: drawn additively (HudQuad's) */
} ShellQuad;
int shell_quads(const Shell *s, HudQuad *out, int max);

/* ---- the 3D objects: GuiShape (<s>, a model) and GuiEffect (<e>, an .edf). They are drawn through the GUI camera: eye
 * at (0, -1, 0) of its world looking along +y, z up, a vertical field of view of 2 atan(0.375) (41.1 degrees), near 1,
 * far 10000 [R 0x227AD0]. A shape draws its whole model (each mesh on its node of the model's tree, at the rest pose)
 * at world x its node's place, lit by its light set (or unlit); an effect plays its .edf with its frame at world (the
 * effect's x, y, z axes; on a shape's node: that node's frame x the shape's world). An effect that is running is
 * started when it is not playing and kept where it is; one stopping is shut down (its particles play out, still
 * kept where it is); when it is over, the viewer says so (shell_effect_ended). A new gen: another edf (the old one is
 * gone at once). Effects not drawn now (layer -1) run on unseen, where they were. */
enum { SHELL_3D_SHAPE, SHELL_3D_EFFECT };
typedef struct {
    int kind;
    const void *key;                /* the object, while its interface is loaded */
    int layer;                      /* its HUD_TEX_3D quad: the layer-th of shell_quads(); -1 not drawn now */
    const char *name;               /* the model ("CITYSELECT_GLOBE.PWK") or the .edf ("MAP_HIGHLIGHT.EDF") */
    const Bundle *b;                /* the bundle of its interface, where they are */
    float world[16];                /* column-major, world = m (x, y, z, 1); the model's or the effect's axes */
    int parent;                     /* an effect on a shape's node: the shape's index here, and the node; -1 / NULL */
    const char *attach;
    int running, stopping, gen;     /* an effect: playing; stopped, playing out (shut it down once) */
    int lit;                        /* a shape: its light set (ambient, up to three lights, each a direction it travels) */
    float ambient[3]; int nlights;
    float light_dir[3][3], light_diffuse[3][3], light_ambient[3][3], light_specular[3][3];
} Shell3D;
/* the 3D objects in their markers' order, then the effects that run unseen; marks the drawn effects drawn (an effect
 * starts on the first update after it was drawn once [R 0x237590]) */
int shell_3d(Shell *s, Shell3D *out, int max);
void shell_effect_ended(Shell *s, const void *key);
int shell_quads_turned(const Shell *s, ShellQuad *out, int max);
#endif
