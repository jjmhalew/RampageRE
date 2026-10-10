/* pad.h - today's game controllers (port extra, docs/NATIVE.md "Input"), the WoodyRE way: src/pad.c on Windows (Sony
 * DualShock 4 and DualSense over raw HID, everything else through XInput), src/pad_sdl.c elsewhere (SDL's game
 * controllers). Up to four pads, each in a slot of its own: a pad takes the lowest free slot when it comes and keeps it
 * until it goes, and slot k is player k + 1's. main.c maps them to the game's pad (MV_* of moves.h) and to the menus.
 * A --shot run never calls any of this: no pad is opened, no light or motor touched. */
#ifndef RAMPAGE_PAD_H
#define RAMPAGE_PAD_H
#include <stdint.h>

/* buttons by their Xbox names; on a Sony pad A = Cross, B = Circle, X = Square, Y = Triangle, LB / RB = L1 / R1,
 * LT / RT = L2 / R2, BACK = Share / Create (Select), START = Options, LS / RS = L3 / R3, GUIDE = PS, TOUCH = the
 * touchpad's click; on a Nintendo pad by place, not label (A = the bottom button, where Cross is) */
enum { PAD_A, PAD_B, PAD_X, PAD_Y, PAD_LB, PAD_RB, PAD_LT, PAD_RT, PAD_BACK, PAD_START, PAD_LS, PAD_RS,
       PAD_UP, PAD_DOWN, PAD_LEFT, PAD_RIGHT, PAD_GUIDE, PAD_TOUCH, PAD_NBUTTONS };
enum { PADK_NONE, PADK_XBOX, PADK_DS4, PADK_DS5, PADK_SWITCH };   /* PADK_SWITCH: SDL builds only */
#define PAD_SLOTS 4

typedef struct {
    int kind;                    /* PADK_*; PADK_NONE: no pad in the slot */
    uint32_t buttons;            /* 1 << PAD_*; LT / RT count as pressed from a quarter of their travel */
    float lx, ly, rx, ry;        /* the sticks, -1 .. 1 with y down = + (as MV_LY), no dead zone */
    float lt, rt;                /* the triggers, 0 .. 1 */
} PadState;

/* once a frame: every slot's pad (all zero but the kind while the window is not in front: input and rumble pause);
 * devchanges = Window.dev_changes, a new value makes it look for pads that came or went. Returns the slots that have
 * a pad (bit k: slot k) */
unsigned pad_poll(PadState st[PAD_SLOTS], int focused, unsigned devchanges);
/* the big (low-frequency) motor of slot's pad at strength 0 .. 1 for that long; the strongest running one wins */
void pad_rumble(int slot, float strength, float seconds);
void pad_set_strength(float s);  /* 0 .. 1 over every motor: the vibration (gCommon.setEnableRumble) */
void pad_close(void);            /* motors off, the DualSense light back to the system's (also at exit) */
const char *pad_kind_name(int kind);
#endif
