/* movie.h - the game's movies (*.pss): Sony's MPEG-2 program streams, played through the front end's full-screen
 * Video quad (AttractMode's midway, pipeworks, digital and intro; Victory's victory and credits; the cheat videos and
 * ballers), decoded by hand (docs/MOVIE_NOTES.md).
 *
 * [R] What is in them: packs of 4096 bytes; the video (stream 0xE0) is MPEG-2 Main Profile at Main Level, 512 x 384,
 * 4:2:0, 29.97 frames a second (the _pal ones 25); all but digital.pss are I pictures alone. The audio is
 * private stream 1 (0xBD): four bytes (FF A1 00 00) before each packet's data, the first one opening with Sony's
 * "SShd" header (0x10: SPU ADPCM, 48000 Hz, 2 channels, 0x20 bytes interleaved) and "SSbd" (the data's size).
 *
 * The file is read from the Vol as it plays (intro.pss is 100 MB). movie_video() decodes up to the picture shown t
 * seconds in; movie_audio() gives the sound as it is pulled (the mixer, sound_stream). Nothing here is threaded. */
#ifndef RAMPAGE_MOVIE_H
#define RAMPAGE_MOVIE_H
#include <stdint.h>
#include "data.h"

typedef struct Movie Movie;

Movie *movie_open(const Vol *v, const char *name);  /* "intro" -> intro.pss (case-insensitive); NULL if absent */
void movie_close(Movie *m);
void movie_size(const Movie *m, int *w, int *h);    /* the picture's size (0 until the first sequence header) */
double movie_fps(const Movie *m);
/* the picture shown t seconds in, RGBA (w x h x 4, the first row on top), NULL before the first one; *changed 1
 * when it is another picture than the last call's. Valid until the next call or movie_close. */
const uint8_t *movie_video(Movie *m, double t, int *changed);
/* the same picture's planes (Y, Cb, Cr: w x h and w/2 x h/2), for tests; 0 none */
int movie_planes(const Movie *m, const uint8_t *plane[3], int stride[3]);
int movie_done(const Movie *m, double t);           /* the last picture is shown and its time is over */
/* the next frames of the sound, stereo at 48 kHz; the frames that had sound (the rest are silence) */
int movie_audio(Movie *m, int16_t *out, int frames);
int movie_pictures(const Movie *m);                 /* pictures decoded so far */
#endif
