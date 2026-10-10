/* wrand.c - the world's two random states (gWorld +0x828, +0x82C), see wrand.h */
#include "wrand.h"

static uint32_t pm = 0x2545F491u;

uint32_t *wrand_pm_state(void) { return &pm; }
void wrand_seed_pm(uint32_t seed) { pm = seed; }

uint32_t wrand_pm(void)
{
    uint64_t x = (uint64_t)pm * 16807u;
    x = (x & 0x7FFFFFFFu) + (x >> 31);
    x = (x & 0x7FFFFFFFu) + (x >> 31);
    pm = x == 0x7FFFFFFFu ? 0 : (uint32_t)x;
    return pm;
}

float wrand_frand(void) { return (float)wrand_pm() * (1.0f / 2147483648.0f); }
int wrand_rand(void) { return (int)(wrand_pm() >> 8 & 0xFFFF); }

/* 0x106950: v = t[i] + t[j], i on (16 wraps to 0), t[j] = v, j on. Seeded (0x1069C0) i = 12, j = 0, t[0] = 0, t[k]
 * the Park-Miller run from the seed | 1, t[6] |= 1 */
static struct { uint32_t i, j, t[17]; } lf = { 12, 0, { 0 } };

void wrand_seed_lf(uint32_t seed)
{
    uint32_t x = seed;
    lf.i = 12;
    lf.j = 0;
    lf.t[0] = 0;
    for (int k = 1; k < 17; k++) {
        uint64_t y = (uint64_t)x * 16807u;
        y = (y & 0x7FFFFFFFu) + (y >> 31);
        y = (y & 0x7FFFFFFFu) + (y >> 31);
        x = y == 0x7FFFFFFFu ? 0 : (uint32_t)y;
        lf.t[k] = x | 1;
    }
    lf.t[6] |= 1;
}

uint32_t wrand_lf(void)
{
    uint32_t v = lf.t[lf.i] + lf.t[lf.j];
    lf.i = lf.i == 16 ? 0 : lf.i + 1;
    lf.t[lf.j] = v;
    lf.j = lf.j == 16 ? 0 : lf.j + 1;
    return v;
}
