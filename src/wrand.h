/* wrand.h - the world's two random states, shared by every system as in the game:
 *   gWorld +0x828: Park-Miller (0x106BD0: s = 16807 s mod 2^31 - 1, 2^31 - 1 becomes 0), seeded from GameStatus
 *     seed[0]: gWorld.frand() (s / 2^31, 0x279F98), gWorld.rand() ((s >> 8) & 0xFFFF, 0x279F58), findPointInRubble;
 *   gWorld +0x82C: the lagged Fibonacci 0x106950 (seeded by 0x1069C0 from seed[1]): the bots' rand01, the edibles'
 *     rand(), the smashes. [R] */
#ifndef WRAND_H
#define WRAND_H

#include <stdint.h>

uint32_t *wrand_pm_state(void);                      /* +0x828 itself (findPointInRubble draws on it) */
void wrand_seed_pm(uint32_t seed);
uint32_t wrand_pm(void);                             /* 0x106BD0: the next state */
float wrand_frand(void);                             /* gWorld.frand(): [0, 1) */
int wrand_rand(void);                                /* gWorld.rand(): 0 .. 65535 */
void wrand_seed_lf(uint32_t seed);                   /* 0x1069C0 */
uint32_t wrand_lf(void);                             /* 0x106950 */

#endif
