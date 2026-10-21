/* camera.h - the game's play camera ("TwoTilts", camera.c; docs/CAMERA.md). */
#ifndef RAMPAGE_CAMERA_H
#define RAMPAGE_CAMERA_H
#include "data.h"

typedef struct {                       /* camera.bdp, group TwoTilts (0x3134F0 stores them at 0x4D5C80 + 4 ..) */
    float fov;                         /* +0x04 vertical, radians */
    float smoothing;                   /* +0x08 how much of the last frame's camera stays */
    float near_tilt, far_extra_tilt;   /* +0x0C, +0x10 */
    float far_border_scale;            /* +0x14 */
    float above_tilt;                  /* +0x18 */
    float state_trans_time;            /* +0x1C seconds the near / above state must hold before it switches */
    float max_angular_vel;             /* +0x20 tilt change per second */
    float min_cam_dist, min_cam_height;/* +0x24, +0x28 */
    float rep_monster_height;          /* +0x2C */
    float held_obj_min_cam_dist_offset;/* +0x30 */
    float yaw;                         /* +0x34 */
    float show_ground_height;          /* +0x38 */
} CamSettings;

typedef struct {
    CamSettings s;
    float inv_sin_h, sin_h, cos_h, sin_hh, cos_hh, inv_sin_hh, cot_hh;   /* +0x3C .. +0x54 */
    int first, state;                  /* +0x5C, +0x7C (1 = near the ground) */
    float timer, tilt;                 /* +0x80, +0x84 */
    float q[4], pos[3];                /* +0x60, +0x70: the smoothed camera */
    float rot[3][3];                   /* its rows: right, forward, up */
} GameCamera;

void cam_settings(CamSettings *s, Group g);
void cam_init(GameCamera *c, const CamSettings *s);
/* 0x313A50, the mode (re)entered (gCameraMgr.setCamMode, director.c): the next frame is taken as it is (no
 * smoothing, the tilt at once), state 2 (neither near nor above) and its timer 1e20, so the first state counts at once */
void cam_activate(GameCamera *c);
/* follow the monsters (root positions); held: one of them holds what is not edible or threw a second ago (the
 * minimums HeldObjMinCamDistOffset further, 0x312B50's sp+0xC0); aspect_hw = the view's height / width */
void cam_update(GameCamera *c, const float (*monsters)[3], int n, int held, float dt, float aspect_hw);
/* [R 0x312B50, after the monsters' box] the camera manager's interest objects (army.h army_interest: a tank on screen)
 * added to the points after the n monsters' (pts has room for 2 more): each place held to x within 30 of the monsters'
 * middle, y -10 .. 21, z 0 .. 70. Returns the new count */
int cam_interest(float (*pts)[3], int n, const float (*obj)[3], int nobj);

/* camera shake (CameraShakeFx, vtable 0x446750; its definition 0x1E8C70): an effect whose yaw and pitch, summed over
 * the live shakes with a falloff by distance, turn the finished camera (CamShakeApplier 0x27F550, after the TwoTilts
 * smoothing, so it does not feed back). Degree-1 graphs of t = age / duration: a + b t. */
typedef struct { float duration, mag[2][2], theta[2][2]; } ShakeDef;
extern const ShakeDef SHAKE_MONSTER;                 /* camshake.edf: the monsters' clip events, a floor falling */
extern const ShakeDef SHAKE_COLLAPSE;                /* camshake_collapse.edf: a toppling billboard, the rage */
typedef struct { const ShakeDef *def; float age, strength, phase[2], pos[3]; } CamShake;
/* 0x1E7ED0: (re)start at pos with strength; with check, a stronger shake still going is kept (0x1E8420) */
void shake_start(CamShake *s, const ShakeDef *def, const float pos[3], float strength, int check, uint32_t *seed);
void shake_step(CamShake *s, int n, float dt);       /* 0x1E7E90: they age */
/* 0x207050: the camera's rows (right, forward, up) turned by the shakes, its place kept */
void cam_shaken(const float pos[3], const float rows[3][3], const CamShake *s, int n, float out[3][3]);

#endif
