/* touch.h - an on-screen pad for touch screens (port extra, WoodyRE's src/touch.h with Rampage's buttons; the Android
 * build only, docs/NATIVE.md "Android"). The left half of the screen is a floating stick (move), the right has the
 * PS2's face buttons, Cross (jump), Square (punch), Triangle (kick) and Circle (grab), R2 (roar) above them and Start
 * (pause) in the corner. They reach the game as pad slot 0 (player 1's, pad.h), so the menus take them as a real pad's:
 * Cross accepts, Circle goes back. A real pad's buttons or a key hide the controls until the screen is touched again. */
#ifndef RAMPAGE_TOUCH_H
#define RAMPAGE_TOUCH_H
#include "pad.h"

void touch_event(const void *sdl_event, int width, int height);   /* an SDL_Event (fingers, keys); width / height = the drawable */
/* merge into this frame's pads (pad_poll's st and its slots on): 1 when the on-screen pad is up (it is then slot 0's) */
unsigned touch_pad(PadState st[PAD_SLOTS], unsigned on);
void touch_draw(int width, int height);                            /* over the finished frame, before the swap */
#endif
