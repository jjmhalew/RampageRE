/* sound.h - the game's sound: the sound banks (Rampage.uber/.samp, RShell.uber/.samp), their event programs, the 3D
 * sounds around the camera, the music streams (*.mic), and a small mixer feeding the audio output (waveOut
 * on Windows, SDL2 elsewhere; docs/FORMAT_SOUND.md).
 *
 * [R] What the game does (SoundDevice gSndDev 0x4CF1B0, its event player 0x4CFAB0, its 3D sound list 0x4CFB40,
 * MultiStream V7.1 on the IOP for the voices):
 *   gSndDev.playSound(name)          0x26CFC0 -> sound_play(name, NULL, 1)
 *   object.add3DSfx(name, keep)      0x26D560 -> sound_add3d(name, pos, 0, 1, 1) (a sound that follows an object)
 *   the monsters' clip sounds        0x287860 -> sound_play(name, pos, 1) (3D, range 0 = 1000 x the event's scale)
 *   gSndDev.killSound(name)          0x26B960 -> sound_kill_name(name)
 *   gSndDev.playMusic(name)          0x26C4D0 -> music_play(name)  ("bbop" -> bbop.mic; a BlockDef's Music)
 *   gSndDev.replaceMusic(n, out, in) 0x26BFC0 -> music_replace(n, out, in)
 *   the camera each frame            0x288550 -> sound_listener(pos, rows)
 * Names are looked up case-insensitively (0x273480), first in Rampage.uber, then in RShell.uber (the front end's
 * "Cancel" is only there). Positions are the game's: z up.
 *
 * Nothing opens the audio device until sound_start(); without it sound_update() still runs the programs and moves
 * the voices on (into nothing), so the game's timing (waits for a voice to end) holds. */
#ifndef RAMPAGE_SOUND_H
#define RAMPAGE_SOUND_H
#include <stdint.h>
#include "data.h"

#define SOUND_RATE 48000                 /* [R] the SPU2's rate; the output is 16-bit stereo at this rate */

int  sound_init(const Vol *v);           /* read the banks from the Vol; the number of events, -1 if none */
void sound_free(void);
int  sound_start(void);                  /* open the audio output (waveOut from winmm.dll loaded at run
                                          * time; SDL2 outside Windows); 0 = ok */
void sound_end(void);                    /* close it */
void sound_update(float dt);             /* a game frame: the 3D list, the event programs, fades, the output */
int  sound_mix(int16_t *out, int frames);/* render frames (stereo) by hand instead of the output (tests, capture);
                                          * once called, sound_update stops moving the voices on by itself */

/* a sound by its event name; pos NULL = a 2D sound (playSound), else a one-shot 3D sound at pos (dropped when out of
 * its range). volume scales it on top of the game's own volumes [G] (1 = as the game). The handle, 0 if refused. */
int  sound_play(const char *name, const float pos[3], float volume);
/* a 3D sound object (add3DSfx): range 0 = 1000 x the event's scale; keep = stays in the list while out of range and
 * starts again when the listener comes back (the game keeps the objects scripts add by id). */
int  sound_add3d(const char *name, const float pos[3], float range, int keep, float volume);
void sound_move(int h, const float pos[3]);   /* a 3D sound's new place (each frame for things that move) */
void sound_kill(int h);                  /* 0x2725C0: ends a loop's wait, or fades the sound out in 100 ms */
void sound_kill_name(const char *name);
int  sound_active(int h);                /* still playing (or waiting to come into range) */
/* the listener: the camera's place and rows (right, forward, up as in camera.h) */
void sound_listener(const float pos[3], const float rows[3][3]);

int  music_play(const char *name);       /* a music stream, looping; replaces the one playing at once */
int  music_replace(const char *name, float fade_out, float fade_in);   /* seconds */
void music_stop(float fade_out);
int  music_playing(void);

void sound_volume(float sfx, float music);   /* the viewer's own gains [G], 1 = as the game */
/* a stream the mixer pulls (a movie's sound, movie.c): pull(ctx, out, frames) fills stereo 48 kHz frames; at the
 * music's gain [G]. NULL stops it. */
void sound_stream(int (*pull)(void *ctx, int16_t *out, int frames), void *ctx);
/* [R] op 0D multiplies the instance's volume byte (group 0's 0x6A to start) by op[1] / 128 and keeps the low 8 bits;
 * a byte of 128 or more is an SPU volume of 0x4000 or more, which the SPU2 plays phase-inverted and quieter. On (the
 * default) does as the game; off keeps such volumes at 127 [G], to compare by ear. */
void sound_wrap_volumes(int on);

/* introspection (tools, tests) */
int  sound_event_count(void);
const char *sound_event_name(int i);
/* a sample of a bank (0 Rampage, 1 RShell) by its SDIR index, decoded: PCM, length, rate, loop start (-1 none) */
int  sound_sample_pcm(int bank, int index, const int16_t **pcm, int *n, int *rate, int *loop);

#endif
