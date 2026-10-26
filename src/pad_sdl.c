/* pad_sdl.c - pad.h on SDL2's game controller API, for the builds outside Windows (WoodyRE's src/pad_sdl.c with a slot
 * per pad; the Windows build has src/pad.c). SDL knows the DualSense / DualShock 4 (rumble, light bar and player lights
 * over USB and Bluetooth), Xbox pads, Nintendo's and the Steam Deck's controls, all with the same button names as pad.h.
 * Nintendo pads go by place, not label (SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS 0): the bottom button is A, where the
 * PS2's Cross is. A pad's player index is its slot, which SDL shows on the player lights and the Sony light bar
 * (RAMPAGE_PADNOLIGHT=1 leaves them be). Input and rumble pause while the window is not in front, as in pad.c.
 * On the Switch SDL lists all eight players' controllers, there or not, with its mapping by place too (A = the bottom
 * button, a single Joy-Con turned sideways the same); libnx says which are there, and a single Joy-Con's stick, which
 * SDL hands over as the d-pad, is made the left stick. */
#ifndef _WIN32
#include "pad.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __SWITCH__
#include <switch/services/hid.h>                 /* not switch.h: its runtime/pad.h has another PadState */
static int nx_there(SDL_JoystickID id)           /* SDL's joystick k is player k's controller (0: also handheld) */
{
    return hidGetNpadStyleSet((HidNpadIdType)id) != 0 || (id == 0 && hidGetNpadStyleSet(HidNpadIdType_Handheld) != 0);
}
static int nx_single(SDL_JoystickID id)          /* one Joy-Con held sideways, a player of its own */
{
    u32 s = hidGetNpadStyleSet((HidNpadIdType)id);
    return (s & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight)) && !(s & HidNpadStyleTag_NpadJoyDual);
}
#elif defined __vita__
static unsigned vita_rear(void)                  /* the handheld has no L2 / R2: the rear touch pad's halves are */
{
    unsigned m = 0;
    for (int d = 0; d < SDL_GetNumTouchDevices(); d++) {
        SDL_TouchID t = SDL_GetTouchDevice(d);
        const char *name = SDL_GetTouchName(d);
        if (!name || strcmp(name, "Back") != 0) continue;   /* (SDL's names: "Front", "Back") */
        for (int f = 0; f < SDL_GetNumTouchFingers(t); f++) {
            SDL_Finger *g = SDL_GetTouchFinger(t, f);
            if (g) m |= 1u << (g->x < 0.5f ? PAD_LT : PAD_RT);
        }
    }
    return m;
}
#endif

static const char *penv(const char *n) { const char *v = getenv(n); return v && *v ? v : NULL; }
static double pad_clock(void) { return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

/* ---- rumble: per slot up to 8 running effects, the strongest one is what the motor gets (as pad.c) ------------------ */
static struct { float s[PAD_SLOTS][8]; double until[PAD_SLOTS][8]; float scale; int log; } g_rum = { .scale = 1.0f, .log = -1 };
void pad_set_strength(float s) { g_rum.scale = s < 0 ? 0 : s > 1 ? 1 : s; }
void pad_rumble(int slot, float strength, float seconds)
{
    if (slot < 0 || slot >= PAD_SLOTS) return;
    if (g_rum.log < 0) g_rum.log = penv("RAMPAGE_PADLOG") != NULL;
    if (g_rum.log) printf("pad: slot %d rumble %.2f for %.2f s (x %.2f)\n", slot, strength, seconds, g_rum.scale);
    double now = pad_clock(); int k = 0;
    for (int i = 1; i < 8; i++) if (g_rum.until[slot][i] < g_rum.until[slot][k]) k = i;
    g_rum.s[slot][k] = strength; g_rum.until[slot][k] = now + seconds;
}
static int rum_level(int slot, double now)       /* 0 .. 255 */
{
    float m = 0;
    for (int i = 0; i < 8; i++) if (now < g_rum.until[slot][i] && g_rum.s[slot][i] > m) m = g_rum.s[slot][i];
    return (int)(m * g_rum.scale * 255.0f + 0.5f);
}

/* ---- the pads -------------------------------------------------------------------------------------------------------- */
#define NPADS 8                                  /* open at once; the first four to come have a slot */
static struct {
    int inited; unsigned seen, slots;
    struct { SDL_GameController *c; SDL_JoystickID id; int kind, slot, sent; double sent_at; PadState st; } p[NPADS];
} P;

const char *pad_kind_name(int kind) { return kind == PADK_DS5 ? "DualSense" : kind == PADK_DS4 ? "DualShock 4" : kind == PADK_XBOX ? "Xbox controller" : kind == PADK_SWITCH ? "Switch controller" : "none"; }

static void take_slot(int i)                     /* the lowest free slot, the player lights to match */
{
    for (int k = 0; k < PAD_SLOTS && P.p[i].slot < 0; k++)
        if (!(P.slots >> k & 1)) {
            P.slots |= 1u << k; P.p[i].slot = k;
            if (!penv("RAMPAGE_PADNOLIGHT")) SDL_GameControllerSetPlayerIndex(P.p[i].c, k);
            printf("pad: %s is player %d's\n", pad_kind_name(P.p[i].kind), k + 1);
        }
}
static void open_new(void)
{
    for (int j = 0; j < SDL_NumJoysticks(); j++) {
        if (!SDL_IsGameController(j)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j); int have = 0, i = -1;
        for (int k = 0; k < NPADS; k++) { if (P.p[k].c && P.p[k].id == id) have = 1; if (!P.p[k].c && i < 0) i = k; }
        if (have || i < 0) continue;
#ifdef __SWITCH__
        if (!nx_there(id)) continue;
#endif
        SDL_GameController *c = SDL_GameControllerOpen(j); if (!c) continue;
        int t = SDL_GameControllerGetType(c), kind = t == SDL_CONTROLLER_TYPE_PS5 ? PADK_DS5 : t == SDL_CONTROLLER_TYPE_PS4 ? PADK_DS4 : PADK_XBOX;
        if (t == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO) kind = PADK_SWITCH;
#if SDL_VERSION_ATLEAST(2, 24, 0)
        if (t == SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR) kind = PADK_SWITCH;
#endif
#ifdef __SWITCH__
        kind = PADK_SWITCH;                      /* the Joy-Cons, handheld, a Pro Controller */
#elif defined __vita__
        kind = PADK_DS4;                         /* the handheld's own buttons (or a PS TV's DualShock): Cross, Circle */
#endif
        memset(&P.p[i], 0, sizeof P.p[i]);
        P.p[i].c = c; P.p[i].id = id; P.p[i].kind = kind; P.p[i].sent = -1; P.p[i].slot = -1;
        printf("pad: %s connected (\"%s\", SDL)\n", pad_kind_name(kind), SDL_GameControllerName(c));
    }
}
static void close_gone(void)
{
    for (int i = 0; i < NPADS; i++) if (P.p[i].c && (!SDL_GameControllerGetAttached(P.p[i].c)
#ifdef __SWITCH__
                                                     || !nx_there(P.p[i].id)
#endif
                                                     )) {
        printf("pad: %s disconnected\n", pad_kind_name(P.p[i].kind));
        if (P.p[i].slot >= 0) P.slots &= ~(1u << P.p[i].slot);
        SDL_GameControllerClose(P.p[i].c); memset(&P.p[i], 0, sizeof P.p[i]);
    }
}
static float ax(SDL_GameController *c, SDL_GameControllerAxis a) { int v = SDL_GameControllerGetAxis(c, a); return v < 0 ? v / 32768.0f : v / 32767.0f; }
static void read_pad(int i)
{
    static const struct { int sdl, b; } map[] = {
        { SDL_CONTROLLER_BUTTON_A, PAD_A }, { SDL_CONTROLLER_BUTTON_B, PAD_B }, { SDL_CONTROLLER_BUTTON_X, PAD_X }, { SDL_CONTROLLER_BUTTON_Y, PAD_Y },
        { SDL_CONTROLLER_BUTTON_LEFTSHOULDER, PAD_LB }, { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, PAD_RB }, { SDL_CONTROLLER_BUTTON_BACK, PAD_BACK },
        { SDL_CONTROLLER_BUTTON_START, PAD_START }, { SDL_CONTROLLER_BUTTON_LEFTSTICK, PAD_LS }, { SDL_CONTROLLER_BUTTON_RIGHTSTICK, PAD_RS },
        { SDL_CONTROLLER_BUTTON_DPAD_UP, PAD_UP }, { SDL_CONTROLLER_BUTTON_DPAD_DOWN, PAD_DOWN }, { SDL_CONTROLLER_BUTTON_DPAD_LEFT, PAD_LEFT },
        { SDL_CONTROLLER_BUTTON_DPAD_RIGHT, PAD_RIGHT }, { SDL_CONTROLLER_BUTTON_GUIDE, PAD_GUIDE }, { SDL_CONTROLLER_BUTTON_TOUCHPAD, PAD_TOUCH } };
    SDL_GameController *c = P.p[i].c; PadState *st = &P.p[i].st; uint32_t m = 0;
    for (unsigned k = 0; k < sizeof map / sizeof map[0]; k++) if (SDL_GameControllerGetButton(c, (SDL_GameControllerButton)map[k].sdl)) m |= 1u << map[k].b;
    st->lx = ax(c, SDL_CONTROLLER_AXIS_LEFTX); st->ly = ax(c, SDL_CONTROLLER_AXIS_LEFTY);       /* SDL: y down = +, as pad.h */
    st->rx = ax(c, SDL_CONTROLLER_AXIS_RIGHTX); st->ry = ax(c, SDL_CONTROLLER_AXIS_RIGHTY);
    st->lt = ax(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT); st->rt = ax(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (st->lt >= 0.25f) m |= 1u << PAD_LT;
    if (st->rt >= 0.25f) m |= 1u << PAD_RT;
#ifdef __vita__
    if (P.p[i].id == 0) {                        /* the handheld's own controls: the rear touch pad as L2 / R2 */
        unsigned r = vita_rear();
        if (r & 1u << PAD_LT) st->lt = 1.0f;
        if (r & 1u << PAD_RT) st->rt = 1.0f;
        m |= r;
    }
#endif
#ifdef __SWITCH__
    if (nx_single(P.p[i].id)) {                  /* SDL reads no axes of it: its stick comes as the d-pad */
        float x = (float)((m >> PAD_RIGHT & 1) - (m >> PAD_LEFT & 1)), y = (float)((m >> PAD_DOWN & 1) - (m >> PAD_UP & 1));
        float k = x != 0 && y != 0 ? 0.7071f : 1.0f;
        st->lx = x * k; st->ly = y * k;
        m &= ~(1u << PAD_UP | 1u << PAD_DOWN | 1u << PAD_LEFT | 1u << PAD_RIGHT);
    }
#endif
    st->kind = P.p[i].kind;
    st->buttons = m;
}
static void rumble(int i, int v, double now)        /* SDL rumble effects run for a duration: send changes, and refresh a running one */
{
    if (v == P.p[i].sent && (v == 0 || now - P.p[i].sent_at < 0.5)) return;
    Uint16 a = (Uint16)(v * 257);
    if (SDL_GameControllerRumble(P.p[i].c, a, 0, v ? 1000 : 0) == 0 || v == 0) { P.p[i].sent = v; P.p[i].sent_at = now; }
}
void pad_close(void)
{
    for (int i = 0; i < NPADS; i++) if (P.p[i].c) { SDL_GameControllerRumble(P.p[i].c, 0, 0, 0); SDL_GameControllerClose(P.p[i].c); }
    memset(P.p, 0, sizeof P.p);
    P.slots = 0;
}
unsigned pad_poll(PadState st[PAD_SLOTS], int focused, unsigned devchanges)
{
    memset(st, 0, sizeof *st * PAD_SLOTS);
    if (!P.inited) {
        P.inited = 1;
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1"); SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");   /* rumble over Bluetooth too */
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");   /* Nintendo pads by place: the bottom button is A */
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)) { printf("pad: SDL game controllers unavailable: %s\n", SDL_GetError()); P.inited = 2; return 0; }
        atexit(pad_close); open_new(); P.seen = devchanges;
    }
    if (P.inited == 2) return 0;
#ifdef __SWITCH__
    devchanges = P.seen + 1;                     /* SDL sends no device events there: look every frame (libnx's
                                                  * shared memory, cheap) */
#endif
    if (devchanges != P.seen) { P.seen = devchanges; close_gone(); open_new(); }
    double now = pad_clock();
    SDL_GameControllerUpdate();
    unsigned on = 0;
    for (int i = 0; i < NPADS; i++) {
        if (!P.p[i].c) continue;
        take_slot(i);
        int k = P.p[i].slot; if (k < 0) continue;
        on |= 1u << k;
        rumble(i, focused ? rum_level(k, now) : 0, now);
        st[k].kind = P.p[i].kind;
        if (!focused) continue;
        read_pad(i);
        st[k] = P.p[i].st;
    }
    return on;
}
#endif
