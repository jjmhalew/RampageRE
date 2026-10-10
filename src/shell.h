/* shell.h - the game's front end ("the shell", RShellPackage), natively: the ControlRoom and its interfaces from
 * Shell.bdp (AttractMode, MainMenu with its number-of-players and number-of-bots boxes, the cheat entry and the retro
 * games box, MonsterSelect, MonsterSheet, CitySelect, Options), the profile select / create popup (MemoryCard.ifc of
 * MemoryCard.bdp, the SaveGame class' native code), the loading screen (Loading.ifc of Loading_<City>.bdp) and the
 * Victory interface (VShell.bdp), run by the logic of their scripts (build/scripts/ControlRoom.scr, AttractMode.scr,
 * MainMenu.scr, MonsterSelect.scr, MonsterSheet.scr, CitySelect.scr, Options.scr, Loading.scr, Victory.scr, Shell.scr)
 * and the gShell / gCommon / GameStatus bindings they call. docs/SHELL_NOTES.md has the details ([R] read in the code
 * or the files, [G] a guess or a stand-in).
 *
 * The movies (.pss) are not played: where one would play, an event SHELL_EVT_MOVIE names it and the shell goes on
 * after shell_set_movie_time's seconds (0: at once). The arcade emulators (DESI/JARVOS.ELF, M1.ELF) are skipped the same way
 * (SHELL_EVT_EMULATOR). The 3D parts of the interfaces (CitySelect's globe, the .edf particle effects: the main menu's
 * fires and searchlights, the cards' glow, the lava lamp) are not drawn.
 *
 * Use: shell_open() with the volume, how the shell starts (the boot, or back from a game) and the profile as the viewer
 * knows it; every frame shell_input*() for the pad, shell_update(dt), shell_events() for what it asks of the viewer
 * (sounds, music, profile load / create / save, the movies, the start of a game) and shell_quads() for what to draw.
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
    int allow_save;                 /* gCommon.getAllowSave() */
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
    int nprofiles;                  /* the profiles saved (the popup's list; getSaveFilesExist = nprofiles > 0) */
    char profiles[SHELL_PROFILES][16];
    int language;                   /* hud.h's languages: 0 English .. 8 Portuguese */
} ShellProfile;

/* gCommon's play state and gCommon.getGameStatus() as the shell hands them to the game [R GameStatus bindings, lookup
 * 0x2C4C30: setMonsterIdx, setCityIdx, setStartBlock, setNight; the Common bindings setPlayType, setNumPlayersAndBots] */
typedef struct {
    int play_type;                  /* game.h GP_*: 2 campaign, 3 King of the City, 4 King of the World, 5 Timed Run */
    int num_players, num_bots;      /* 1 .. 4; the last num_bots of the players are the computer's */
    int monster[4];                 /* MonsterData.Monster<n> per player */
    char monster_name[4][16];       /* its Name ("George") */
    int city_index;                 /* CityData.City<n> (the campaign's order, above) */
    char city[16];                  /* its City ("LasVegas") */
    int start_block;                /* GameStatus +0x210: 0 = the city's BlockStart, else that block */
    int night;                      /* GameStatus.getNight */
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
} ShellStart;

typedef struct Shell Shell;
/* vol: RAMPAGE.VOL, read for Shell.bdp, VShell.bdp, ShellData.bdp, CommonGUI.bdp, MemoryCard.bdp, Loading.bdp and
 * Loading_<City>.bdp; it must outlive the Shell. st NULL: the boot. NULL when the bundles are missing. */
Shell *shell_open(const Vol *vol, const ShellStart *st, const ShellProfile *p);
void shell_close(Shell *s);
/* the profile changed (after SHELL_EVT_PROFILE_LOAD / _CREATE, or a buddy's for player n): the shell takes a copy */
void shell_set_profile(Shell *s, const ShellProfile *p);
/* seconds a movie "plays" (a black screen) where the game would play a .pss; 0 (default): skipped at once */
void shell_set_movie_time(Shell *s, float seconds);

/* the pad, as GuiEvent ids: hud.h HUD_EV_* (2 right, 4 left, 8 up, 16 down, 32 back, 64 accept, 128 start, 256 a
 * button). The buttons [R the scripts' use and the layouts' pictures]: 0 = CONTROLLER_B0 (square: the main menu's
 * Retro games, MonsterSelect's flip card, CitySelect's stats), 1 = CONTROLLER_B1 (triangle: Options, the card sheet,
 * MonsterSheet's zoom), 4 = TRIGGER_RIGHT (MonsterSelect's "load monsters" of another player's profile) */
void shell_input(Shell *s, int ev, int button);
void shell_input_pad(Shell *s, int ev, int button, int pad);   /* GuiEvent.getController(): 0 = player 1 */
/* pad 0 held, every frame (gGuiMgr.getController(0): getX(0), getButtons()): the stick across -1 .. 1 and the held
 * buttons, SHELL_HELD_*. Options' sliders move while left / right are held; Start and Cross held together on the main
 * menu open the cheat entry [R MainMenu::updateCheatActivation; G: the bits] */
enum { SHELL_HELD_LEFT = 4, SHELL_HELD_RIGHT = 8, SHELL_HELD_CROSS = 64, SHELL_HELD_START = 128 };
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
    SHELL_EVT_EMULATOR,             /* gShell.startEmulator(arg): 0 Rampage, 1 Rampage World Tour (skipped) */
    SHELL_EVT_ATTRACT_GAME,         /* gShell.startAttractGame(): the attract mode's demo game (skipped) */
    SHELL_EVT_PROFILE_LOAD,         /* the popup chose saved profile arg (ShellProfile.profiles[arg]): load it and
                                     * call shell_set_profile */
    SHELL_EVT_PROFILE_CREATE,       /* the popup's name entry: create profile `name`, then shell_set_profile */
    SHELL_EVT_PROFILE_NONE,         /* the popup's "continue without a profile": nothing is saved (allow_save 0) */
    SHELL_EVT_BUDDY_PROFILE,        /* gShell.loadBuddyProfile(arg): player arg's own monsters from another profile;
                                     * answer with shell_buddy_profile */
    SHELL_EVT_CHEATS,               /* gCommon.setCheatFlags / setCheatFlagsI / clearCheatFlags: arg the new flags,
                                     * value the parameter of flag 0x80000000 (the start block) or 16 (the video) */
    SHELL_EVT_START                 /* gShell.startGame() (shell state 4): play shell_game() */
};
typedef struct { int kind; int arg; float value; char name[32]; } ShellEvent;
int shell_events(Shell *s, ShellEvent *out, int max);
const ShellGame *shell_game(const Shell *s);
void shell_settings(const Shell *s, float *sound, float *music, int *rumble);
/* MonsterSelect::onGetBuddyProfileCallback(player): the buddy profile for player n was loaded (its monster flags in
 * the profile's monster_flags[n], set them with shell_set_profile first), or -1: none */
void shell_buddy_profile(Shell *s, int player);

/* gShell.getState() [R 0x4D51F0, set by 0x308950 and the bindings]: 1 the boot (memory card check), 2 the attract
 * mode, 3 the menus, 4 a game starts, 6 the Victory movies */
int shell_state(const Shell *s);
/* the interface that has the pad: "AttractMode", "MainMenu", "MonsterSelect", "MonsterSheet", "CitySelect",
 * "Options", "MemoryCard" (the profile popup), "Loading", "Victory" */
const char *shell_focus(const Shell *s);

typedef struct {
    float x[4], y[4];               /* the corners in the reference screen: the picture's top-left, top-right,
                                     * bottom-right, bottom-left */
    float u[4], v[4];               /* texture coordinates at them (over the whole decoded texture) */
    float col[4];
    const Bundle *b;                /* NULL: untextured */
    uint32_t tex;
} ShellQuad;
int shell_quads(const Shell *s, HudQuad *out, int max);
int shell_quads_turned(const Shell *s, ShellQuad *out, int max);
#endif
