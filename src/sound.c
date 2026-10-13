/* sound.c - the game's sound (sound.h; docs/FORMAT_SOUND.md).
 * [R] marks what is read from the game's code or data, [G] what is a guess or a stand-in.
 *
 * The game's layers, and what stands for them here:
 *   SoundDevice 0x4CF1B0 (script interface gSndDev, 32 named sound slots +0x54, 2 music slots +0x3D4, 64 volume
 *     groups +0x440, the listener)                                        -> Track, the group table, the listener
 *   the event player 0x4CFAB0 (instances of 0x3C bytes at +0x4C, run by 0x2716E0 every frame)  -> Inst, sfx_tick
 *   the 3D sound list 0x4CFB40 (objects of 0x40 bytes, sorted by distance, 16 nearest, 0x273CF0)  -> objects_tick
 *   the voice layer 0x4CF800 (start 0x26E3E0, 3D volume/pan/pitch 0x26E8F0, volume 0x26EE90)  -> Voice
 *   MultiStream V7.1 (STREAM.IRX on the IOP: SPU2 voices, sample cache, disc streams)  -> the mixer below: the
 *     volume and pitch units the EE sends it [R]; how the IOP mixes them is not in the ELF
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "sound.h"

#define PI 3.14159265f
#define TWO_PI 6.2831853f
/* [R] 0x270F20: the player holds 0x2E - (2 x streams + 2) instances, the voice layer's constructor (0x270FC0) giving
 * 2 streams (+0x20, the two music slots): 40. They are the SPU2 voices 2 .. 41 marked for the sound effects
 * (0x270B60; the streams have the top six of the 48, 46 .. 47 and 44 .. 45 the music's), and each instance holds at
 * most one voice (0x26E4E0), so a voice is never short and MultiStream's stealing (+0x1E, 0x26F280) never comes. */
#define MAX_INST 40
#define MAX_NAMED 32                     /* [R] the SoundDevice's named sounds (+0x54, 0x1C each): playSound is refused
                                          * when all 32 are taken (0x26D0F0) */
#define MAX_OBJECTS 128                  /* [R] the 3D list's objects (0x40 bytes each, 0x274FE0) */
#define MAX_TRACKS (MAX_NAMED + MAX_OBJECTS)
#define NEAREST 16                       /* [R] 0x273CF0 looks after the 16 nearest 3D objects */
#define NGROUPS 64                       /* [R] 0x26E0D8 */
#define GROUP_MUSIC 3                    /* [R] setMusicVolume = group 3 (0x26B5E0) */
#define MAX_BANKS 2

static inline int be16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static inline int pack_vol(int v) { return (v & 0xFF) << 7; }        /* [R] 0x26FC80 */
static inline int unpack_vol(int v) { return (v >> 7) & 0xFF; }      /* [R] 0x26FC70 */
/* [R] an SPU2 volume register: 15 bits, 0x4000..0x7FFF negative (phase inverted); MultiStream keeps that meaning
 * (0x131990 scales 0x7FFF - v for those). 0x3FFF is full volume. */
static inline float spu_amp(int v) { v &= 0x7FFF; return (float)(v & 0x4000 ? v - 0x8000 : v) / 16384.0f; }
/* [R] 0x135320: the SPU2 pitch for a rate in Hz, rate x 4096 / 48000 cut to an integer (0x1000 plays at 48 kHz); the
 * step through the sample per output frame is that / 4096 */
static inline double spu_step(int rate)
{
    uint32_t p = (uint32_t)(((uint64_t)((uint32_t)rate << 12) * 0x057619F1u) >> 32) >> 10;
    return p / 4096.0;
}

/* ---- the banks ([R] FORMAT_SOUND.md: UBER = PROJ, POOL, SDIR, group counts, group names, names) */
typedef struct { int16_t *pcm; int n, loop, rate, tried; } Sample;
typedef struct {
    uint8_t *uber, *samp;
    uint32_t uber_size, samp_size;
    uint8_t *proj, *pool, *sdir;         /* their headers; entries from +0x10 */
    int nproj, npool, nsdir;
    const char **names;                  /* one per PROJ entry */
    Sample *smp;
} Bank;

/* ---- an instance of an event program (0x3C bytes at 0x4CFAB0 +0x4C) */
typedef struct {
    int handle;                          /* +0x0C, -1 = free */
    int parent;                          /* +0x10: started by op 08 of that instance */
    Bank *bank;                          /* +0x00 */
    uint8_t *prog;                       /* +0x04: the program being run (op 06/13 switch it) */
    int track;                           /* +0x08: the 3D object, -1 = a 2D sound */
    int event;                           /* +0x14 */
    int pc;                              /* +0x16, in 8-byte instructions */
    int running;                         /* +0x18: 1 until killed (a wait with byte 1 set ends then) */
    int is3d;                            /* +0x19 */
    int notify;                          /* +0x1A: remove the 3D object when done (cleared by a kill) */
    int paused;                          /* +0x1D: pauseGame's (0x272780): its program waits, its voice is still */
    int steal;                           /* +0x1E: PROJ flags & 8 */
    int vol;                             /* +0x20: packed volume (byte << 7) */
    int group;                           /* +0x24 */
    int loops;                           /* +0x26: op 05's counter */
    uint32_t t_instr;                    /* +0x28: when the current instruction started */
    uint32_t fade_t0, fade_t1;           /* +0x2C, +0x30 (0 = no fade) */
    int fade_to;                         /* +0x34, packed */
    float pos[3], vel[3], range;         /* the 3D object's, copied each frame (0x26D2F0) */
    float gain;                          /* the viewer's own scale on top (sound_play's volume: 1 everywhere) */
} Inst;

/* ---- a voice (the instance's voice record at +0x38, 0x26E3E0; the SPU2 voice behind it) */
typedef struct {
    int on;
    Sample *s;
    double pos, step;
    int rate;                            /* +0x08: the sample's rate */
    int base;                            /* +0x04: a 3D voice's volume before distance and pan, -1 = the instance's */
    int l, r;                            /* the volume registers */
    int is3d;                            /* +0x13 */
    int sign;                            /* +0x12: the surround phase, +-1: 1 when the instance slot's voice record is
                                          * made (0x26F20C, from 0x2729C4), kept from one play to the next [R] */
    float angle;                         /* +0x00: the last pan angle, 2 pi = none yet */
} Voice;

/* ---- the named sounds and 3D objects (SoundDevice slots +0x54 and the list 0x4CFB40) */
typedef struct {
    int id;                              /* 0 = free */
    int obj;                             /* 1 = a 3D object */
    char name[48];
    int inst;                            /* the instance handle, -1 = none */
    int keep;                            /* [R] an object added with an id stays while out of range */
    int doppler;                         /* [R] PROJ flags & 2: the velocity comes from the moves */
    float pos[3], prev[3], vel[3], range, range2, dist2;
    float gain;
} Track;

/* a music slot (SoundDevice +0x3D4, 0x14 each) and its stream */
typedef struct {
    uint8_t *data;
    uint32_t size;
    int on, loop, ch, rate, inter, blocks;   /* on: +0x00 (the stream's handle) not -1 */
    int block, frame;                    /* the next ADPCM frame */
    int16_t pcm[2][28];
    int at, have;
    int hist[2][2];
    double phase;
    int vol, to, from, stop_after;       /* bytes +0xD (now), +0xE, +0xF; +0xC: stop it when the fade is over */
    uint32_t f1, f0;                     /* +0x04 the fade's end, +0x08 its start (ms) */
    char name[32];
} Music;

static struct {
    int ready;
    const Vol *vol;
    Bank bank[MAX_BANKS];
    int nbanks;
    Inst inst[MAX_INST];
    Voice voice[MAX_INST];               /* one per instance slot */
    Track track[MAX_TRACKS];
    Music music[2];
    int music_last;                      /* SoundDevice +0x434: the slot the last music went into */
    int next_handle;                     /* +0x40 of the player */
    int next_id;
    uint32_t rng;                        /* +0x50 of the player */
    int flags[32];                       /* op 32 */
    uint8_t group_vol[NGROUPS];          /* +0x444 + 8 g */
    int enable_3d;                       /* SoundDevice +0x3 (enable3DSfx) */
    float lis_pos[3], lis_r0[3], lis_r1[3], lis_r2[3];
    uint32_t now;                        /* ms, 0x26F230 */
    double ms_acc;
    float sfx_gain, music_gain;
    int wrap;                            /* 1: op 0D's volume wraps as the game's does (0: the viewer's option) */
    int manual_mix;
    float pending;                       /* seconds of audio to move on by when nothing pulls */
    int (*pull)(void *ctx, int16_t *out, int frames);   /* sound_stream */
    void *pull_ctx;
    struct {                             /* [R] the scripts' SoundEffects (0x4CFB90: 16 of 0x20 bytes) */
        int h;                           /* +0x00: the 3D object, 0 = a free slot */
        float pos[3];                    /* +0x04: its place (the object follows it), +0x10 the velocity: none set */
        int auto_stop, seen;             /* +0x1C: start's second argument, +0x1D: a frame has gone by */
    } se[16];
} S;

/* ---- PS-ADPCM ([R] the SPU's format: 16-byte frames, shift | filter << 4, flags, 28 nibbles) */
static const int FILT[5][2] = { { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 } };

static void adpcm_frame(const uint8_t *f, int16_t *out, int *h1, int *h2)
{
    int shift = f[0] & 15, filter = f[0] >> 4 & 15;
    if (filter > 4)
        filter = 0;
    if (shift > 12)
        shift = 9;
    for (int i = 0; i < 28; i++) {
        int nib = f[2 + i / 2] >> (i & 1 ? 4 : 0) & 15;
        int s = (int16_t)(nib << 12) >> shift;
        s += (*h1 * FILT[filter][0] + *h2 * FILT[filter][1] + 32) >> 6;
        s = s < -32768 ? -32768 : s > 32767 ? 32767 : s;
        out[i] = (int16_t)s;
        *h2 = *h1;
        *h1 = s;
    }
}

static Sample *sample_get(Bank *b, int idx)
{
    if (idx <= 0 || idx >= b->nsdir)
        return NULL;
    Sample *s = &b->smp[idx];
    if (s->tried)
        return s->pcm ? s : NULL;
    s->tried = 1;
    const uint8_t *e = b->sdir + 0x10 + 16 * idx;
    uint32_t off = rd32(e), size = rd32(e + 4), flags = rd32(e + 8);
    if (off == 0xFFFFFFFFu || off > b->samp_size || size > b->samp_size - off)
        return NULL;
    int frames = (int)(size / 16);
    s->pcm = malloc(sizeof(int16_t) * 28 * (frames ? frames : 1));
    if (!s->pcm)
        return NULL;
    s->rate = (int)rd32(e + 12);
    s->loop = -1;
    int h1 = 0, h2 = 0, n = 0, start = 0;
    for (int i = 0; i < frames; i++) {
        const uint8_t *f = b->samp + off + 16 * i;
        if (f[1] & 4)                    /* [R] loop start */
            start = n;
        adpcm_frame(f, s->pcm + n, &h1, &h2);
        n += 28;
        if (f[1] & 1)                    /* [R] the end (with bit 1: jump to the loop start) */
            break;
    }
    s->n = n;
    if (flags & 1)                       /* [R] SDIR flags bit 0: a looping sample (0x26E670 passes it on) */
        s->loop = start;
    return s;
}

/* ---- banks */
static int bank_load(Bank *b, const Vol *v, const char *name)
{
    char path[64];
    snprintf(path, sizeof path, "%s.uber", name);
    b->uber = vol_read(v, path, &b->uber_size);
    snprintf(path, sizeof path, "%s.samp", name);
    b->samp = vol_read(v, path, &b->samp_size);
    if (!b->uber || !b->samp || b->uber_size < 0x20 || memcmp(b->uber, "UBER", 4) != 0)
        goto bad;
    uint32_t o[6];
    for (int i = 0; i < 6; i++) {
        o[i] = rd32(b->uber + 8 + 4 * i);
        if (o[i] + 16 > b->uber_size)
            goto bad;
    }
    b->proj = b->uber + o[0];            /* [R] 0x272AB0 */
    b->pool = b->uber + o[1];
    b->sdir = b->uber + o[2];
    b->nproj = (int)rd32(b->proj + 8);
    b->npool = (int)rd32(b->pool + 8);
    b->nsdir = (int)rd32(b->sdir + 12);
    uint32_t nnames = rd32(b->uber + o[5]);
    if (nnames != (uint32_t)b->nproj || o[0] + 16 + 12u * b->nproj > b->uber_size ||
        o[2] + 16 + 16u * b->nsdir > b->uber_size || o[1] + 16 + 4u * b->npool > b->uber_size)
        goto bad;
    b->names = calloc(nnames, sizeof *b->names);
    b->smp = calloc(b->nsdir, sizeof *b->smp);
    if (!b->names || !b->smp)
        goto bad;
    for (uint32_t i = 0; i < nnames; i++) {
        uint32_t at = rd32(b->uber + o[5] + 4 + 4 * i);
        b->names[i] = at < b->uber_size ? (const char *)b->uber + at : "";
    }
    for (int i = 0; i < b->nproj; i++) {  /* [R] 0x272BC0: a group of 0x40 or more becomes 0 (all are 0xFF) */
        uint8_t *g = b->proj + 0x10 + 12 * i + 10;
        if (*g >= NGROUPS)
            *g = 0;
    }
    return 0;
bad:
    free(b->uber);
    free(b->samp);
    memset(b, 0, sizeof *b);
    return -1;
}

static uint8_t *program(Bank *b, int event)  /* [R] the POOL offset table; a negative offset is no program */
{
    if (event < 0 || event >= b->npool)
        return NULL;
    int32_t off = (int32_t)rd32(b->pool + 0x10 + 4 * event);
    return off < 0 ? NULL : b->pool + off;
}

static uint8_t *find_event(const char *name, Bank **bank)  /* [R] 0x272F20: the PROJ entry by name */
{
    for (int k = 0; k < S.nbanks; k++)
        for (int i = 0; i < S.bank[k].nproj; i++)
            if (_stricmp(S.bank[k].names[i], name) == 0) {
                *bank = &S.bank[k];
                return S.bank[k].proj + 0x10 + 12 * i;
            }
    return NULL;
}

/* ---- the listener and the 3D voice ([R] 0x26E8F0) */
static void voice_3d(const Inst *in, Voice *vc, float *pitch, int *pl, int *pr)
{
    float d[3], dist2 = 0;
    for (int k = 0; k < 3; k++) {
        d[k] = in->pos[k] - S.lis_pos[k];
        dist2 += d[k] * d[k];
    }
    float dist = sqrtf(dist2), range = in->range;
    float k = dist <= range && range > 0 ? (range - dist) / range : 0;   /* [R] linear to nothing at the range */
    int base = vc->base >= 0 ? vc->base : in->vol;
    int v = (int)((float)base * k);
    float a = d[0] * S.lis_r0[0] + d[1] * S.lis_r0[1] + d[2] * S.lis_r0[2];
    float b = d[0] * S.lis_r2[0] + d[1] * S.lis_r2[1] + d[2] * S.lis_r2[2];
    float th;
    if (b != 0)
        th = atan2f(a, b);               /* [R] VU0 atan with the quadrant fixed up after */
    else
        th = a > 0 ? PI / 2 : a < 0 ? -PI / 2 : 0;
    float prev = vc->angle;
    if (prev != TWO_PI) {                /* [R] the pan moves a tenth of the way each update */
        float p = prev < 0 ? prev + TWO_PI : prev, t = th < 0 ? th + TWO_PI : th, diff = t - p;
        if (!(diff <= PI))
            diff -= TWO_PI;
        if (diff < -PI)
            diff += TWO_PI;
        th -= 0.9f * diff;
        if (th < -PI)
            th += TWO_PI;
        if (!(th <= PI))
            th -= TWO_PI;
        if ((prev < 0) != (th < 0) && !(fabsf(th) < PI / 2))
            vc->sign = -vc->sign;        /* [R] crossing behind the listener turns the surround phase over */
    }
    vc->angle = th;
    float x = (th > 0 ? PI - th : -PI - th) * 0.5f;
    *pl = (vc->sign * (int)((float)v * cosf(x))) & 0x7FFF;
    *pr = (vc->sign * (int)((float)v * sinf(x))) & 0x7FFF;
    /* [R] Doppler: the change of distance over the next 10 ms, sound at 350 units a second */
    float f[3], fd2 = 0;
    for (int j = 0; j < 3; j++) {
        f[j] = in->pos[j] + 0.01f * in->vel[j] - S.lis_pos[j];
        fd2 += f[j] * f[j];
    }
    float vr = (sqrtf(fd2) - dist) / 0.01f;
    *pitch = vr <= 350.0f && !(vr < -192.5f) ? 350.0f / (350.0f + vr) : 1.0f;
}

void sound_listener(const float pos[3], const float rows[3][3])
{
    /* [R] 0x288550 asks the camera manager (RCameraMgr 0x4D3100, its controller's camera 0x27F500) for its place and
     * two of its rows, the ones the view matrix (0x167D10: right, up, forward) keeps second and third: forward as row
     * 0, up as row 1 (its fifth argument, 0x409FF8, is not read). 0x26FCB0 scales the place by setVelocityScalar's
     * +0xC (1, 0x26E390; no script sets it) and makes row 2 = row 0 x row 1 normalised (forward x up = right; (1, 0,
     * 0) if under 1e-6): the pan's angle is atan2(d . forward, d . right), right 0, ahead pi / 2, left pi, behind
     * -pi / 2 (0x26E8F0). */
    const float *f = rows[1], *u = rows[2];
    float c[3] = { f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0] };
    float n2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
    for (int k = 0; k < 3; k++) {
        S.lis_pos[k] = pos[k];
        S.lis_r0[k] = f[k];
        S.lis_r1[k] = u[k];
        S.lis_r2[k] = n2 < 1e-6f ? (k == 0) : c[k] / sqrtf(n2);
    }
}

/* ---- voices */
static void voice_stop(Inst *in) { S.voice[in - S.inst].on = 0; }    /* [R] 0x26EF60 */

static void voice_set_volume(Inst *in, int v)  /* [R] 0x26EE90 */
{
    Voice *vc = &S.voice[in - S.inst];
    if (vc->is3d && S.enable_3d)
        vc->base = v;
    else if (vc->on)
        vc->l = vc->r = v;
}

static void voice_start(Inst *in, int idx)    /* [R] 0x26E3E0 (op 10) */
{
    Voice *vc = &S.voice[in - S.inst];
    if (vc->on)                          /* [R] the instance's voice is still busy: the play is skipped */
        return;
    Sample *s = sample_get(in->bank, idx);   /* (a voice is always free: MAX_INST) */
    if (!s || s->n == 0)
        return;
    vc->s = s;
    vc->rate = s->rate;
    vc->angle = TWO_PI;
    vc->base = -1;
    vc->is3d = in->is3d;
    float pitch = 1;
    if (in->is3d)
        voice_3d(in, vc, &pitch, &vc->l, &vc->r);
    else
        vc->l = vc->r = in->vol;
    vc->pos = 0;
    vc->step = spu_step((int)((float)vc->rate * pitch));
    vc->on = 1;
}

static void voice_3d_update(Inst *in)  /* [R] 0x26E840, every frame for a 3D instance */
{
    Voice *vc = &S.voice[in - S.inst];
    if (!in->is3d || !S.enable_3d || !vc->on)
        return;
    float pitch;
    voice_3d(in, vc, &pitch, &vc->l, &vc->r);
    vc->step = spu_step((int)((float)vc->rate * pitch));
}

/* ---- the event player */
static uint32_t rng_next(void)          /* [R] 0x106BD0: Park-Miller, x * 16807 mod 2^31 - 1 */
{
    uint64_t x = (uint64_t)S.rng * 16807u;
    x = (x & 0x7FFFFFFF) + (x >> 31);
    x = (x & 0x7FFFFFFF) + (x >> 31);
    if (x == 0x7FFFFFFF)
        x = 0;
    S.rng = (uint32_t)x;
    return S.rng;
}

static Inst *inst_find(int h)
{
    if (h < 0)
        return NULL;
    for (int i = 0; i < MAX_INST; i++)
        if (S.inst[i].handle == h)
            return &S.inst[i];
    return NULL;
}

static void inst_free(Inst *in)         /* [R] 0x272A30 (its voice stops, 0x26F0A0) */
{
    voice_stop(in);
    memset(in, 0, sizeof *in);
    in->handle = in->parent = in->track = -1;
    in->notify = 1;
}

/* [R] 0x272170 */
static int spawn(Bank *b, int event, int pc, int group, int steal, int track, int parent, float gain)
{
    for (int i = 0; i < MAX_INST; i++) {
        Inst *in = &S.inst[i];
        if (in->handle != -1)
            continue;
        int h = S.next_handle++;
        if (S.next_handle < 0)
            S.next_handle = 100;
        inst_free(in);
        in->handle = h;
        in->parent = parent;
        in->bank = b;
        in->prog = program(b, event);
        in->track = track;
        in->event = event;
        in->pc = pc;
        in->group = group;
        in->steal = steal;
        in->vol = pack_vol(S.group_vol[group]);   /* [R] 0x26B550: the group's volume */
        in->running = 1;
        in->t_instr = S.now;
        in->is3d = track >= 0;
        in->gain = gain;
        if (track >= 0) {
            const Track *t = &S.track[track];
            memcpy(in->pos, t->pos, sizeof in->pos);
            memcpy(in->vel, t->vel, sizeof in->vel);
            in->range = t->range;
        }
        return h;
    }
    return -1;
}

/* [R] 0x272050: start an event by name; obj = the 3D object (kept only if the event's PROJ flags have bit 0) */
static int start_event(const char *name, int track, float gain)
{
    Bank *b;
    uint8_t *p = find_event(name, &b);
    if (!p)
        return -1;
    uint32_t flags = rd32(p + 4);
    int event = p[8] | p[9] << 8, max = p[11];
    uint8_t *prog = program(b, event);
    int count = 0;
    for (int i = 0; i < MAX_INST; i++)
        count += S.inst[i].handle != -1 && S.inst[i].prog == prog;
    if (count > max)                     /* [R] PROJ +0xB: how many may play at once (less one) */
        return -1;
    if (!(flags & 1))
        track = -1;
    return spawn(b, event, 0, p[10], (flags & 8) != 0, track, -1, gain);
}

static void inst_kill(int h)            /* [R] 0x2725C0 */
{
    for (int i = 0; i < MAX_INST; i++) {
        Inst *in = &S.inst[i];
        if (in->handle == -1)
            continue;
        if (in->handle == h) {
            const uint8_t *op = in->prog ? in->prog + 8 * in->pc : NULL;
            if (op && op[0] == 0x07 && op[1])
                in->running = 0;         /* a loop waiting to be stopped goes on to its own ending */
            else {
                in->fade_to = pack_vol(0);   /* otherwise: down to nothing in 100 ms */
                in->fade_t0 = S.now;
                in->fade_t1 = S.now + 100;
            }
        } else if (in->parent == h)
            inst_kill(in->handle);
    }
}

/* [R] 0x2716E0: each instance runs its program until an instruction has to wait */
static void sfx_tick(void)
{
    for (int i = 0; i < MAX_INST; i++) {
        Inst *in = &S.inst[i];
        if (in->handle == -1)
            continue;
        if (in->track >= 0 && S.track[in->track].id) {      /* 0x26D2F0: the object's place */
            const Track *t = &S.track[in->track];
            memcpy(in->pos, t->pos, sizeof in->pos);
            memcpy(in->vel, t->vel, sizeof in->vel);
            in->range = t->range;
        }
        voice_3d_update(in);
        int done = 0, more, guard = 0;
        if (!in->paused)                 /* [R 0x271754]: a paused one goes straight on to its fade */
        do {
            more = 0;
            if (!in->prog) {
                done = 1;
                break;
            }
            uint8_t *op = in->prog + 8 * in->pc;
            switch (op[0]) {
            case 0x00:                   /* end */
                done = 1;
                break;
            case 0x05: {                 /* loop to op[4..5], op[6..7] times (0xFFFF = always); op[1]: not once killed */
                in->loops = (int16_t)(in->loops + 1);
                int target = be16(op + 4), times = be16(op + 6), go = 1;
                if (op[1] && !in->running)
                    go = 0;
                if (times != 0xFFFF && !((int16_t)in->loops < times))
                    go = 0;
                if (go)
                    in->pc = target;
                else
                    more = 1;
                break;
            }
            case 0x06:                   /* goto event op[2..3] at op[4..5] */
                in->prog = program(in->bank, be16(op + 2));
                in->pc = be16(op + 4);
                break;
            case 0x07: {                 /* wait: op[1] for a kill, op[3] for the voice's end, op[4] for op[6..7] ms */
                if (op[1] && !in->running) {
                    in->notify = 0;
                    more = 1;
                }
                if (op[3] && !S.voice[i].on) {
                    voice_stop(in);
                    more = 1;
                }
                int ms = be16(op + 6);
                if (op[4] && ms != 0xFFFF && S.now - in->t_instr >= (uint32_t)ms) {
                    in->t_instr = S.now;
                    more = 1;
                }
                break;
            }
            case 0x08:                   /* start event op[2..3] at op[4..5] beside this one (2D, a child) */
                spawn(in->bank, be16(op + 2), be16(op + 4), in->group, in->steal, -1, in->handle, in->gain);
                more = 1;
                break;
            case 0x0D: {                 /* volume x op[1] / 128 ([R] in bytes: it wraps past 255) */
                int cur = unpack_vol(in->vol);
                int nv = (int)(128.0f * ((float)op[1] / 128.0f * ((float)cur / 128.0f)));
                if (!S.wrap && nv > 127)
                    nv = 127;                /* sound_wrap_volumes(0), the viewer's: kept at full instead */
                in->vol = pack_vol(nv);
                voice_set_volume(in, in->vol);
                more = 1;
                break;
            }
            case 0x0F:                   /* fade to op[1] over op[6..7] ms */
                in->fade_to = pack_vol(op[1]);
                in->fade_t0 = S.now;
                in->fade_t1 = S.now + (uint32_t)be16(op + 6);
                more = 1;
                break;
            case 0x10:                   /* play sample op[1..2] (an SDIR index) */
                voice_start(in, be16(op + 1));
                more = 1;
                break;
            case 0x11:                   /* stop the voice */
                voice_stop(in);
                more = 1;
                break;
            case 0x13:                   /* if a random byte > op[1]: goto event op[2..3] at op[4..5] */
                if (op[1] < (int)(rng_next() & 0xFF)) {
                    in->prog = program(in->bank, be16(op + 2));
                    in->pc = be16(op + 4);
                } else
                    more = 1;
                break;
            case 0x14: {                 /* the next of the listed instructions, in turn (op[7] keeps the turn) */
                int at = op[7] % 6, to = 0;
                for (int k = 0; k < 6 && !(to = (int8_t)op[1 + at]); k++)
                    at = (at + 1) % 6;
                if (to) {
                    in->pc = to;
                    op[7] = (uint8_t)((at + 1) % 6);
                } else
                    done = 1;
                break;
            }
            case 0x32:                   /* set flag op[1] = op[2] (querySoundFlag) */
                S.flags[op[1] & 31] = op[2];
                more = 1;
                break;
            default:
                more = 1;
                break;
            }
            if (more) {
                in->pc++;
                in->t_instr = S.now;
            }
        } while (more && ++guard < 256);
        if (done) {
            inst_free(in);
            continue;
        }
        if (in->fade_t1) {               /* [R] 0x271DC8 */
            uint32_t span = in->fade_t1 - in->fade_t0;
            float f = span ? (float)(S.now - in->fade_t0) / (float)span : 1.0f;
            if (f < 0)
                f = 0;
            if (!(f < 1.0f)) {
                f = 1.0f;
                in->fade_t1 = in->fade_t0 = 0;
                in->vol = in->fade_to;
            }
            int v = (int)((float)in->fade_to + (1.0f - f) * (float)(in->vol - in->fade_to));
            if (v == 0) {
                voice_stop(in);
                in->running = 0;
            } else
                voice_set_volume(in, v);
        }
    }
}

/* ---- named sounds and the 3D list */
static int track_new(const char *name, int obj, const float pos[3], float range, int keep, float gain)
{
    int used = 0;
    for (int i = 0; i < MAX_TRACKS; i++)
        used += S.track[i].id && S.track[i].obj == obj;
    if (used >= (obj ? MAX_OBJECTS : MAX_NAMED))   /* [R] full: refused */
        return -1;
    for (int i = 0; i < MAX_TRACKS; i++) {
        Track *t = &S.track[i];
        if (t->id)
            continue;
        memset(t, 0, sizeof *t);
        t->id = ++S.next_id;
        if (S.next_id >= 0x7FFFFFF0)
            S.next_id = 0;
        t->obj = obj;
        snprintf(t->name, sizeof t->name, "%s", name);
        t->inst = -1;
        t->keep = keep;
        t->gain = gain;
        if (pos) {
            memcpy(t->pos, pos, sizeof t->pos);
            memcpy(t->prev, pos, sizeof t->prev);
        }
        Bank *b;
        const uint8_t *p = find_event(name, &b);
        float scale = p ? rdf(p) : 1;
        if (p)
            t->doppler = (rd32(p + 4) & 2) != 0;
        if (range == 0)                  /* [R] 0x274B10: range 0 = 1000, times the event's PROJ scale */
            range = 1000;
        range *= scale;
        if (range == 0)
            range = 1000;
        t->range = range;
        t->range2 = range * range;
        return i;
    }
    return -1;
}

static Track *track_find(int h)
{
    if (h <= 0)
        return NULL;
    for (int i = 0; i < MAX_TRACKS; i++)
        if (S.track[i].id == h)
            return &S.track[i];
    return NULL;
}

static void track_drop(Track *t)
{
    if (t->inst >= 0)
        inst_kill(t->inst);
    for (int i = 0; i < MAX_INST; i++)
        if (S.inst[i].track == t - S.track)
            S.inst[i].track = -1;        /* the fading instance keeps the last place it was given */
    t->id = 0;
}

/* [R] 0x273CF0: the objects in order of their distance (in x, y) from the listener; the nearest 16 are seen to:
 * started when in range, killed when out of it (and dropped unless kept), dropped when their sound has ended */
static void objects_tick(float dt)
{
    int order[MAX_TRACKS], n = 0;
    for (int i = 0; i < MAX_TRACKS; i++) {
        Track *t = &S.track[i];
        if (!t->id || !t->obj)
            continue;
        if (t->doppler && dt > 0)       /* [R] only events with PROJ flags bit 1 get a velocity */
            for (int k = 0; k < 3; k++) {
                t->vel[k] = 0.9f * t->vel[k] + 0.1f * (t->pos[k] - t->prev[k]) / dt;
                t->prev[k] = t->pos[k];
            }
        float dx = S.lis_pos[0] - t->pos[0], dy = S.lis_pos[1] - t->pos[1];
        t->dist2 = dx * dx + dy * dy;
        int j = n++;
        while (j > 0 && S.track[order[j - 1]].dist2 > t->dist2) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    for (int k = 0; k < n && k < NEAREST; k++) {
        Track *t = &S.track[order[k]];
        if (t->dist2 > t->range2) {
            if (t->inst >= 0) {
                inst_kill(t->inst);
                t->inst = -1;
            }
            if (!t->keep)
                track_drop(t);
        } else if (t->inst < 0) {
            t->inst = start_event(t->name, order[k], t->gain);   /* refused (too many): tried again next frame */
        } else if (!inst_find(t->inst)) {
            t->inst = -1;
            track_drop(t);
        }
    }
}

/* ---- the script type SoundEffect ([R] 0x2750F0 its methods: start, stop, setPoint, ...; the table 0x4CFB90) */
static int effect_playing(int h)         /* [R] 0x274CF0 finds the object, 0x26BC90 asks after its sound (+0x3C) */
{
    Track *t = track_find(h);
    return t && t->inst >= 0 && inst_find(t->inst) != NULL;
}

/* [R] 0x2765C0: every frame, an effect started with auto_stop goes once its sound is not playing (ended, or never
 * started: out of range, refused), a frame after its start */
static void effects_tick(void)
{
    for (int i = 0; i < 16; i++) {
        if (!S.se[i].h || !S.se[i].auto_stop)
            continue;
        int playing = effect_playing(S.se[i].h);
        if (!S.se[i].seen)
            S.se[i].seen = 1;
        else if (!playing) {
            if (getenv("RAMPAGE_TRACE"))
                printf("sound effect %d has played\n", i);
            Track *t = track_find(S.se[i].h);   /* 0x26D530: the 3D object goes */
            if (t)
                track_drop(t);
            S.se[i].h = 0;
        }
    }
}

/* ---- music ([R] 0x26FF60: "<name>.mic", 2 slots (SoundDevice +0x3D4), group 3's volume) */
static void music_off(Music *m)
{
    free(m->data);
    memset(m, 0, sizeof *m);
}

static int music_open(Music *m, const char *name)
{
    char path[48];
    snprintf(path, sizeof path, "%s%s", name, strchr(name, '.') ? "" : ".mic");   /* [R] 0x26FF98 */
    uint32_t size;
    uint8_t *d = S.vol ? vol_read(S.vol, path, &size) : NULL;
    if (!d || size < 0x40)
        goto bad;
    int head = (int)rd32(d), ch = (int)rd32(d + 8), rate = (int)rd32(d + 12), inter = (int)rd32(d + 16),
        blocks = (int)rd32(d + 20);
    if (head < 0x18 || ch < 1 || ch > 2 || rate <= 0 || inter < 16 || inter % 16 || blocks <= 0 ||
        (uint64_t)head + (uint64_t)blocks * inter * ch > size)
        goto bad;
    free(m->data);
    m->data = d;
    m->size = size;
    m->ch = ch;
    m->rate = rate;
    m->inter = inter;
    m->blocks = blocks;
    m->block = m->frame = m->at = m->have = 0;
    memset(m->hist, 0, sizeof m->hist);
    m->phase = 0;
    /* [R] it loops: setMusicRepeat's flag is 1 from the start (SoundDevice +0x43C, 0x26E384; the voice layer's copy
     * +0xBC, 0x271118) and no script changes it, so MultiStream is told not to stop at the end (0x2700B4) and the
     * ended stream's slot is not freed (0x26DCC8) */
    m->loop = 1;
    m->on = 1;
    snprintf(m->name, sizeof m->name, "%s", name);
    if (getenv("RAMPAGE_TRACE"))
        printf("music %s (slot %d, group 3 at %d)\n", name, (int)(m - S.music), S.group_vol[GROUP_MUSIC]);
    return 0;
bad:
    free(d);
    return -1;
}

static int music_next_frame(Music *m)
{
    if (m->block >= m->blocks) {
        if (!m->loop)
            return 0;
        m->block = m->frame = 0;
        memset(m->hist, 0, sizeof m->hist);
    }
    int head = (int)rd32(m->data);
    for (int c = 0; c < m->ch; c++) {
        const uint8_t *f = m->data + head + ((size_t)m->block * m->ch + c) * m->inter + 16 * m->frame;
        adpcm_frame(f, m->pcm[c], &m->hist[c][0], &m->hist[c][1]);
    }
    if (++m->frame == m->inter / 16) {
        m->frame = 0;
        m->block++;
    }
    m->at = 0;
    m->have = 28;
    return 1;
}

static void music_fade(Music *m, int to, uint32_t ms, int stop_after)   /* a slot from its byte now to `to` */
{
    m->f0 = S.now;
    m->from = m->vol;
    m->f1 = S.now + ms;
    m->to = to & 0xFF;
    m->stop_after = stop_after;
}

static void music_tick(void)           /* [R] 0x26DA50, each frame for each slot in use */
{
    for (int i = 0; i < 2; i++) {
        Music *m = &S.music[i];
        if (!m->on || (!m->stop_after && m->to == m->vol))
            continue;
        if ((int32_t)(m->f1 - S.now) <= 0 || m->f1 == m->f0) {
            m->vol = m->to;                  /* the fade is over: stopped when it was one going out */
            if (m->stop_after)
                music_off(m);
        } else {
            float f = (float)(S.now - m->f0) / (float)(m->f1 - m->f0);
            m->vol = (m->from + (int)(f * (float)(m->to - m->from))) & 0xFF;
        }
    }
}

/* [R] 0x26C4D0 / 0x26BFC0: the slot after the last one used, unless one is free (the first free then); a stream still
 * in it is cut */
static Music *music_slot(void)
{
    int k = (S.music_last + 1) % 2;
    for (int i = 0; i < 2; i++)
        if (!S.music[i].on) {
            k = i;
            break;
        }
    S.music_last = k;
    music_off(&S.music[k]);
    return &S.music[k];
}

static int music_target(float f)       /* f x group 3's byte (0x26C07C) */
{
    float v = f * (float)S.group_vol[GROUP_MUSIC];
    return v > 0 ? (int)v & 0xFF : 0;
}

int music_replace(const char *name, float fade_out, float fade_in)
{
    /* [R] 0x26BFC0 replaceMusic(name, in, out) (the script's seconds x 1000, 0x275680): the new stream comes up from 0
     * to group 3's volume over `in` while every other one goes down to 0 over `out` and is then stopped; "" only
     * sends them all down */
    uint32_t in_ms = fade_in > 0 ? (uint32_t)(fade_in * 1000.0f) & 0xFFFF : 0;
    uint32_t out_ms = fade_out > 0 ? (uint32_t)(fade_out * 1000.0f) & 0xFFFF : 0;
    if (!name || !*name) {
        for (int i = 0; i < 2; i++)
            if (S.music[i].on)
                music_fade(&S.music[i], music_target(0), out_ms, 1);
        return 0;
    }
    Music *m = music_slot();
    if (music_open(m, name) != 0)
        return -1;
    m->vol = 0;
    music_fade(m, music_target(1), in_ms, 0);
    for (int i = 0; i < 2; i++)
        if (&S.music[i] != m && S.music[i].on)
            music_fade(&S.music[i], 0, out_ms, 1);
    return 0;
}

int music_play(const char *name)
{
    /* [R] 0x26C4D0 playMusic: into a slot at group 3's volume at once; the other slot plays on */
    Music *m = music_slot();
    if (music_open(m, name) != 0)
        return -1;
    m->vol = m->to = S.group_vol[GROUP_MUSIC];
    m->f0 = m->f1 = 0;
    m->stop_after = 0;
    return 0;
}

void music_stop(float fade_out) { music_replace("", fade_out, 0); }

int music_playing(void) { return S.music[0].on || S.music[1].on; }   /* [R] 0x26BDB0: a slot in use */

void sound_group_volume(int group, float v, int ms)
{
    /* [R] 0x26B5E0 setMusicVolume (group 3) / setSfxVolume (1) / setVoiceVolume (2): the group's byte = 127 v; the
     * music slots (group 3) go to it over ms, otherwise the running instances of the group (0x2724B0; group 0: all) */
    if (group < 0 || group >= NGROUPS)
        return;
    float f = 127.0f * v;
    int b = f > 0 ? (int)f & 0xFF : 0;
    S.group_vol[group] = (uint8_t)b;
    if (group == GROUP_MUSIC) {
        for (int i = 0; i < 2; i++) {
            Music *m = &S.music[i];
            if (!m->on)
                continue;
            if (ms <= 0)
                m->vol = m->to = b;          /* 0x26B698: at once */
            else {
                m->from = m->vol;
                m->to = b;
                m->f0 = S.now;
                m->f1 = S.now + (uint32_t)(ms & 0xFFFF);
            }
        }
        return;
    }
    for (int i = 0; i < MAX_INST; i++) {
        Inst *in = &S.inst[i];
        if (in->handle == -1 || !in->running || (group && in->group != group))
            continue;
        in->fade_to = pack_vol(b);
        in->fade_t0 = S.now;
        in->fade_t1 = S.now + (uint32_t)(ms & 0xFFFF);
    }
}

float sound_group_volume_get(int group)   /* [R] 0x26B570: the byte / 127 */
{
    return group >= 0 && group < NGROUPS ? (float)S.group_vol[group] / 127.0f : 0;
}

void sound_settings(float sfx, float music, int music_ms)
{
    /* [R] the profile's and the Options page's volumes (0..0.75) as the game sets them (0x33EFD8, 0x2C2FA4):
     * setSfxVolume on groups 1, 9, 10 and 11 (which no event is in: every PROJ group byte is 0xFF, made 0 by
     * 0x272BC0, so the sound effects stay at group 0's 0x6A whatever the slider says), setMusicVolume on group 3 */
    static const int sfx_groups[] = { 1, 9, 10, 11 };
    for (int i = 0; i < 4; i++)
        sound_group_volume(sfx_groups[i], sfx, 0);
    sound_group_volume(GROUP_MUSIC, music, music_ms);
}

void music_for_state(int state, const char *block_music)
{
    /* [R] 0x323CE0, each time the world enters a state (0x3247A8): nothing while the music volume is under 0.05;
     * the state's name (4 BeginLevel / ContinueLevel, 5 BeginBlock, 7 Results, 8 GameOver, 9 ContinueBlock,
     * 10 EndLevel, 11 Victory; 6 none) looked up in the city book's and the block's "Music" groups (none in the
     * data), the block's Music for BeginBlock, else "Wilder" for BeginBlock and "Oingo" for Results; then
     * replaceMusic(name, 0.5, 0.5) */
    if (sound_group_volume_get(GROUP_MUSIC) < 0.05f)
        return;
    const char *name = state == 5 ? (block_music && *block_music ? block_music : "Wilder") :
                       state == 7 ? "Oingo" : NULL;
    if (name)
        music_replace(name, 0.5f, 0.5f);
}

/* ---- the mixer: the voices' volume registers as the SPU2 takes them (spu_amp) and the master volume at full: the
 * boot logos' sound start (0x1AF608) sets MVOL to 0x3FFF on both cores and nothing sets it again [R]. [G: the
 * SPU2 interpolates a voice with its 4-point gaussian table, a hardware matter, not in the ELF: linear here] */
static float mixbuf[2 * 1024];

static void mix_block(float *out, int frames)
{
    memset(out, 0, sizeof(float) * 2 * frames);
    for (int i = 0; i < MAX_INST; i++) {
        Voice *vc = &S.voice[i];
        if (!vc->on || S.inst[i].paused)
            continue;
        const Sample *s = vc->s;
        float g = S.sfx_gain * S.inst[i].gain, al = spu_amp(vc->l) * g, ar = spu_amp(vc->r) * g;
        for (int f = 0; f < frames; f++) {
            int k = (int)vc->pos;
            if (k >= s->n) {
                if (s->loop < 0 || s->loop >= s->n) {
                    vc->on = 0;
                    break;
                }
                vc->pos -= s->n - s->loop;
                k = (int)vc->pos;
            }
            int k1 = k + 1 < s->n ? k + 1 : s->loop >= 0 ? s->loop : k;
            float fr = (float)(vc->pos - k), x = (float)s->pcm[k] + fr * (float)(s->pcm[k1] - s->pcm[k]);
            out[2 * f] += x * al;
            out[2 * f + 1] += x * ar;
            vc->pos += vc->step;
        }
    }
    for (int i = 0; i < 2; i++) {
        Music *m = &S.music[i];
        if (!m->on)
            continue;
        float a = spu_amp(pack_vol(m->vol)) * S.music_gain;   /* [R] 0x26FE00: byte << 7 on its two voices */
        double step = (double)m->rate / SOUND_RATE;   /* 1: the streams are at 48 kHz [R] */
        for (int f = 0; f < frames; f++) {
            if (m->at >= m->have && !music_next_frame(m)) {
                music_off(m);
                break;
            }
            out[2 * f] += m->pcm[0][m->at] * a;
            out[2 * f + 1] += m->pcm[m->ch - 1][m->at] * a;
            for (m->phase += step; m->phase >= 1; m->phase -= 1)
                m->at++;
        }
    }
    if (S.pull) {                        /* a movie's sound */
        static int16_t st[2 * 1024];
        S.pull(S.pull_ctx, st, frames);
        for (int i = 0; i < 2 * frames; i++)
            out[i] += st[i];                         /* full scale (0x1AF5C0: MVOL 0x3FFF, BVOL 0x7FFF) [R] */
    }
}

void sound_stream(int (*pull)(void *ctx, int16_t *out, int frames), void *ctx)
{
    S.pull = pull;
    S.pull_ctx = ctx;
}

int sound_mix(int16_t *out, int frames)
{
    S.manual_mix = 1;
    int done = 0;
    while (done < frames) {
        int n = frames - done > 1024 ? 1024 : frames - done;
        mix_block(mixbuf, n);
        if (out)
            for (int i = 0; i < 2 * n; i++) {
                float v = mixbuf[i];
                out[2 * done + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        done += n;
    }
    return frames;
}

/* ---- the output: on Windows waveOut, from winmm.dll loaded when asked for (so the viewer links nothing new) */
#ifdef _WIN32
#include <mmsystem.h>
#define NBUF 12
#define BUF_FRAMES 1024
#define AHEAD 5                          /* buffers queued: about 107 ms */
typedef MMRESULT (WINAPI *WaveOpen)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD_PTR, DWORD_PTR, DWORD);
typedef MMRESULT (WINAPI *WaveHdr)(HWAVEOUT, LPWAVEHDR, UINT);
typedef MMRESULT (WINAPI *WaveSimple)(HWAVEOUT);
static struct {
    HMODULE dll;
    HWAVEOUT wo;
    WaveOpen open;
    WaveHdr prepare, unprepare, write;
    WaveSimple reset, close;
    WAVEHDR hdr[NBUF];
    int used[NBUF], next;
    int16_t pcm[NBUF][BUF_FRAMES * 2];
} W;

static void out_feed(void)
{
    if (!W.wo)
        return;
    int queued = 0;
    for (int i = 0; i < NBUF; i++)
        queued += W.used[i] && !(W.hdr[i].dwFlags & WHDR_DONE);
    while (queued < AHEAD) {
        WAVEHDR *h = &W.hdr[W.next];
        if (W.used[W.next] && !(h->dwFlags & WHDR_DONE))
            break;
        int manual = S.manual_mix;
        sound_mix(W.pcm[W.next], BUF_FRAMES);
        S.manual_mix = manual;
        h->dwFlags &= ~WHDR_DONE;
        if (W.write(W.wo, h, sizeof *h) != MMSYSERR_NOERROR)
            break;
        W.used[W.next] = 1;
        W.next = (W.next + 1) % NBUF;
        queued++;
    }
}

int sound_start(void)
{
    if (W.wo)
        return 0;
    if (!W.dll && !(W.dll = LoadLibraryA("winmm.dll")))
        return -1;
    W.open = (WaveOpen)(void *)GetProcAddress(W.dll, "waveOutOpen");
    W.prepare = (WaveHdr)(void *)GetProcAddress(W.dll, "waveOutPrepareHeader");
    W.unprepare = (WaveHdr)(void *)GetProcAddress(W.dll, "waveOutUnprepareHeader");
    W.write = (WaveHdr)(void *)GetProcAddress(W.dll, "waveOutWrite");
    W.reset = (WaveSimple)(void *)GetProcAddress(W.dll, "waveOutReset");
    W.close = (WaveSimple)(void *)GetProcAddress(W.dll, "waveOutClose");
    if (!W.open || !W.prepare || !W.unprepare || !W.write || !W.reset || !W.close)
        return -1;
    WAVEFORMATEX f;
    memset(&f, 0, sizeof f);
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = SOUND_RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = SOUND_RATE * 4;
    if (W.open(&W.wo, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        W.wo = NULL;
        return -1;
    }
    for (int i = 0; i < NBUF; i++) {
        memset(&W.hdr[i], 0, sizeof W.hdr[i]);
        W.hdr[i].lpData = (LPSTR)W.pcm[i];
        W.hdr[i].dwBufferLength = sizeof W.pcm[i];
        W.prepare(W.wo, &W.hdr[i], sizeof W.hdr[i]);
        W.used[i] = 0;
    }
    W.next = 0;
    out_feed();
    return 0;
}

void sound_end(void)
{
    if (!W.wo)
        return;
    W.reset(W.wo);
    for (int i = 0; i < NBUF; i++)
        W.unprepare(W.wo, &W.hdr[i], sizeof W.hdr[i]);
    W.close(W.wo);
    W.wo = NULL;
}

static int out_open(void) { return W.wo != NULL; }
#else
/* SDL2's audio queue, pushed from the main thread as waveOut is (no callback, no thread): the same 5 x 1024 frames
 * ahead */
#include <SDL.h>
#define BUF_FRAMES 1024
#define AHEAD 5
static SDL_AudioDeviceID g_dev;
static int16_t g_pcm[BUF_FRAMES * 2];

static void out_feed(void)
{
    if (!g_dev)
        return;
    while (SDL_GetQueuedAudioSize(g_dev) < AHEAD * sizeof g_pcm) {
        int manual = S.manual_mix;
        sound_mix(g_pcm, BUF_FRAMES);
        S.manual_mix = manual;
        if (SDL_QueueAudio(g_dev, g_pcm, sizeof g_pcm) != 0)
            break;
    }
}

int sound_start(void)
{
    if (g_dev)
        return 0;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return -1;
    SDL_AudioSpec want, have;
    memset(&want, 0, sizeof want);
    want.freq = SOUND_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = BUF_FRAMES;
    want.callback = NULL;                        /* SDL_QueueAudio */
    if (!(g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0))) {   /* SDL converts to what the device takes */
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return -1;
    }
    out_feed();
    SDL_PauseAudioDevice(g_dev, 0);
    return 0;
}

void sound_end(void)
{
    if (!g_dev)
        return;
    SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static int out_open(void) { return g_dev != 0; }
#endif

/* ---- the frame */
void sound_update(float dt)
{
    if (!S.ready)
        return;
    if (dt < 0)
        dt = 0;
    S.ms_acc += dt * 1000.0;
    uint32_t ms = (uint32_t)S.ms_acc;
    S.ms_acc -= ms;
    S.now += ms;
    sfx_tick();                          /* [R] 0x26D960: the event player, then the 3D list, the named sounds, */
    objects_tick(dt);                    /* the music slots */
    for (int i = 0; i < MAX_TRACKS; i++)  /* a named 2D sound's slot is free once its instance has ended (0x26D9C8) */
        if (S.track[i].id && !S.track[i].obj && !inst_find(S.track[i].inst))
            S.track[i].id = 0;
    music_tick();
    effects_tick();                      /* [R] 0x2765C0, right after the device's tick in PackageApp's frame (0x28BE84) */
    if (out_open())
        out_feed();
    else if (!S.manual_mix) {            /* nobody listens: the voices still move on, so their ends come */
        S.pending += dt;
        int frames = (int)(S.pending * SOUND_RATE);
        S.pending -= (float)frames / SOUND_RATE;
        for (int done = 0; done < frames;) {
            int n = frames - done > 1024 ? 1024 : frames - done;
            mix_block(mixbuf, n);
            done += n;
        }
    }
}

/* ---- the interface */
int sound_init(const Vol *v)
{
    sound_free();
    memset(&S, 0, sizeof S);
    S.vol = v;
    static const char *const banks[MAX_BANKS] = { "Rampage", "RShell" };   /* [R] 0x28C270 loads <name>.uber/.samp */
    for (int i = 0; i < MAX_BANKS; i++)
        if (bank_load(&S.bank[S.nbanks], v, banks[i]) == 0)
            S.nbanks++;
    for (int i = 0; i < MAX_INST; i++) {
        inst_free(&S.inst[i]);
        S.voice[i].sign = 1;             /* [R] 0x26F20C */
    }
    for (int g = 0; g < NGROUPS; g++)
        S.group_vol[g] = 0x6A;           /* [R] 0x26E0CC: every group at 0x6A, music (3) at 0x3F */
    S.group_vol[GROUP_MUSIC] = 0x3F;
    S.next_handle = 100;                 /* [R] 0x44086C */
    S.rng = 12345;                       /* [G: 0x106C80 seeds it at start-up with the EE timer's count (0x10D4D0,
                                          * which starts the timer on its first call) | 1: unknowable] */
    S.enable_3d = 1;                     /* [R] SoundDevice +0x3 = 1 (0x26E368); only a script could change it */
    S.music_last = 0;                    /* +0x434: the static's 0 */
    S.sfx_gain = S.music_gain = 1;
    S.wrap = 1;
    S.lis_r0[1] = 1;
    S.ready = S.nbanks > 0;
    int n = 0;
    for (int i = 0; i < S.nbanks; i++)
        n += S.bank[i].nproj;
    return S.ready ? n : -1;
}

void sound_free(void)
{
    sound_end();
    for (int i = 0; i < 2; i++)
        music_off(&S.music[i]);
    for (int k = 0; k < S.nbanks; k++) {
        Bank *b = &S.bank[k];
        for (int i = 0; b->smp && i < b->nsdir; i++)
            free(b->smp[i].pcm);
        free(b->smp);
        free((void *)b->names);
        free(b->uber);
        free(b->samp);
        memset(b, 0, sizeof *b);
    }
    S.nbanks = 0;
    S.ready = 0;
}

int sound_play(const char *name, const float pos[3], float volume)
{
    Bank *b;
    if (!S.ready || !name || !*name || !find_event(name, &b))
        return 0;
    int t = track_new(name, pos != NULL, pos, 0, 0, volume);
    if (t < 0)
        return 0;
    Track *tr = &S.track[t];
    if (!pos) {                          /* [R] 0x26CFC0 */
        tr->inst = start_event(name, -1, volume);
        if (tr->inst < 0) {
            tr->id = 0;
            return 0;
        }
    }
    return tr->id;                       /* a 3D one starts at the next update if in range (0x273CF0) */
}

int sound_add3d(const char *name, const float pos[3], float range, int keep, float volume)
{
    if (!S.ready || !name || !*name || !pos)
        return 0;
    Bank *b;
    if (!find_event(name, &b))
        return 0;
    int t = track_new(name, 1, pos, range, keep, volume);
    return t < 0 ? 0 : S.track[t].id;
}

void sound_move(int h, const float pos[3])
{
    Track *t = track_find(h);
    if (t && pos)
        memcpy(t->pos, pos, sizeof t->pos);
}

void sound_kill(int h)
{
    Track *t = track_find(h);
    if (t)
        track_drop(t);
}

/* [R] 0x26D500 -> 0x274610: every object of the 3D list (SoundEffect slots included) killed (0x26BB30 -> 0x2725C0)
 * and freed. Its callers (the block's teardown 0x2B6760, Game Over 0x324584) put it between enableSfx(0) and
 * enableSfx(as it was), with the sound's frame (0x26D960) run once in between with the event player paused (+0x48):
 * nothing of that frame is heard but the kills */
void sound_objects_clear(void)
{
    for (int i = 0; i < MAX_TRACKS; i++)
        if (S.track[i].id && S.track[i].obj)
            track_drop(&S.track[i]);
}

/* [R] gApp.pauseGame(on) (0x28BC90) -> SoundDevice 0x26B7F0(on, mask 3) -> the event player's 0x272780: every
 * instance playing then is paused or let go (+0x1D, its voice paused, 0x26F050); mask 3 leaves the streams (bit 4,
 * 0x26FB80): the music plays on. A sound started while paused (the menu's) plays */
void sound_pause(int on)
{
    for (int i = 0; i < MAX_INST; i++)
        if (S.inst[i].handle != -1)
            S.inst[i].paused = on != 0;
}

void sound_kill_name(const char *name)  /* [R] 0x26B960 */
{
    for (int i = 0; i < MAX_TRACKS; i++)
        if (S.track[i].id && _stricmp(S.track[i].name, name) == 0)
            sound_kill(S.track[i].id);
}

int sound_active(int h)
{
    Track *t = track_find(h);
    if (t && !t->obj && !inst_find(t->inst))
        t->id = 0;
    return t && t->id;
}

int sound_effect_start(const char *name, int auto_stop)
{
    int i = 15;
    while (i >= 0 && S.se[i].h)          /* [R] 0x276490: the free slot from the last down */
        i--;
    if (i < 0)
        return -1;
    memset(&S.se[i], 0, sizeof S.se[i]);
    /* [R] add3DSfx (0x26D560 -> 0x274840) with the slot as the object, its place (zeroed: the world's origin until a
     * setPoint), no velocity (all zero), range 0 (1000 x the event's scale); the list keeps it while out of range */
    S.se[i].h = sound_add3d(name, S.se[i].pos, 0, 1, 1);
    if (!S.se[i].h)
        return -1;
    S.se[i].auto_stop = auto_stop != 0;
    if (getenv("RAMPAGE_TRACE"))
        printf("sound effect %d: %s\n", i, name);
    return i;
}

/* [R] the methods by the dispatcher 0x275180 (its table 0x427060): start 0x2751C0 (the name, its second argument
 * != 0), stop 0x2751E8, setPoint 0x2752B8 (0x1CB020 copies the point into the slot's +0x04), isValid 0x2753A8 (a slot) */
void sound_effect_set_point(int slot, const float pos[3])
{
    if (slot < 0 || slot >= 16 || !pos)
        return;
    memcpy(S.se[slot].pos, pos, sizeof S.se[slot].pos);
    sound_move(S.se[slot].h, pos);       /* (the game's object reads the slot's place each frame) */
}

void sound_effect_stop(int slot)         /* [R] 0x2751E8 (stop): 0x26D530 takes the object out of the list */
{
    if (slot < 0 || slot >= 16 || !S.se[slot].h)
        return;
    sound_kill(S.se[slot].h);
    S.se[slot].h = 0;
}

void sound_effect_stop_all(void)         /* [R] 0x276430, from PackageApp's frame when the package changes (0x28BE1C) */
{
    for (int i = 0; i < 16; i++)
        sound_effect_stop(i);
}

void sound_volume(float sfx, float music)
{
    S.sfx_gain = sfx;
    S.music_gain = music;
}

void sound_wrap_volumes(int on) { S.wrap = on; }

int sound_event_count(void)
{
    int n = 0;
    for (int i = 0; i < S.nbanks; i++)
        n += S.bank[i].nproj;
    return n;
}

const char *sound_event_name(int i)
{
    for (int k = 0; k < S.nbanks; k++) {
        if (i < S.bank[k].nproj)
            return S.bank[k].names[i];
        i -= S.bank[k].nproj;
    }
    return NULL;
}

int sound_sample_pcm(int bank, int index, const int16_t **pcm, int *n, int *rate, int *loop)
{
    if (bank < 0 || bank >= S.nbanks)
        return -1;
    Sample *s = sample_get(&S.bank[bank], index);
    if (!s)
        return -1;
    *pcm = s->pcm;
    *n = s->n;
    *rate = s->rate;
    *loop = s->loop;
    return 0;
}
