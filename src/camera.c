/* camera.c - the game's play camera, "TwoTilts" (RTTCamera, update 0x312B50; docs/CAMERA.md), transcribed from the
 * PS2 code. Its settings are camera.bdp's property book, group TwoTilts, read by 0x3134F0 into the object at 0x4D5C80.
 *
 * Each frame: the box around the monsters (their root positions), its centre raised by RepMonsterHeight / 2 is what the
 * camera looks at. Near the ground (the lowest monster at or below ShowGroundHeight) it tilts NearTilt + FarExtraTilt x
 * spread (spread = (box width / 110)^2, at most 1), higher up AboveTilt; the state only changes after holding for
 * StateTransTime and the tilt turns at most MaxAngularVel per second. Then it is placed (0x312950 near, 0x312790 above),
 * kept inside the street's ends (0x312600), turned about Z by Yaw x x / 60 (at most +-Yaw) around the centre, and
 * blended with the last frame: SmoothingFactor of the old camera stays (0x103FB0).
 * Game space: Z up; the camera's matrix rows are its right, forward and up axes. */
#include <math.h>
#include <string.h>
#include "camera.h"

#define HALF_PI 1.57079633f

static float maxf(float a, float b) { return a > b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }

void cam_settings(CamSettings *s, Group g)
{
    s->fov = group_num(g, "FOV", 0.5f);
    s->smoothing = group_num(g, "SmoothingFactor", 0.9f);
    s->near_tilt = group_num(g, "NearTilt", 0);
    s->far_extra_tilt = group_num(g, "FarExtraTilt", -0.2f);
    s->far_border_scale = group_num(g, "FarBorderScale", 2);
    s->above_tilt = group_num(g, "AboveTilt", -0.3f);
    s->state_trans_time = group_num(g, "StateTransTime", 0.3f);
    s->max_angular_vel = group_num(g, "MaxAngularVel", 2);
    s->min_cam_dist = group_num(g, "MinCamDist", 75);
    s->min_cam_height = group_num(g, "MinCamHeight", 10);
    s->rep_monster_height = group_num(g, "RepMonsterHeight", 15);
    s->held_obj_min_cam_dist_offset = group_num(g, "HeldObjMinCamDistOffset", 5);
    s->yaw = group_num(g, "Yaw", 0.4f);
    s->show_ground_height = group_num(g, "ShowGroundHeight", 30);
}

/* the derived values 0x3134F0 keeps next to the settings: half the (vertical) FOV and the horizontal half angle */
static void derive(GameCamera *c, float aspect_hw)
{
    float h = 0.5f * c->s.fov;
    c->sin_h = sinf(h);
    c->cos_h = cosf(h);
    c->inv_sin_h = 1 / c->sin_h;
    float x = c->sin_h / aspect_hw, y = c->cos_h, n = 1 / sqrtf(x * x + y * y);   /* 0x4D32B4: the window's h / w here */
    c->sin_hh = x * n;
    c->cos_hh = y * n;
    c->inv_sin_hh = 1 / c->sin_hh;
    c->cot_hh = c->inv_sin_hh * c->cos_hh;
}

void cam_init(GameCamera *c, const CamSettings *s)
{
    memset(c, 0, sizeof *c);
    c->s = *s;
    c->first = 1;
}

void cam_activate(GameCamera *c)                     /* [R 0x313A50: +0x5C = 1, +0x7C = 2, +0x80 = 0x60AD78EC] */
{
    c->first = 1;
    c->state = 2;
    c->timer = 1e20f;
}

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* 0x313C10: how far back the camera must be for every point to fit (the up extent through the vertical half angle,
 * the x extent through the horizontal one) */
static float fit_distance(const GameCamera *c, const float B[3][3], const float C[3], const float (*p)[3], int n, float k)
{
    float best = -1e20f;
    for (int i = 0; i < n; i++) {
        float d[3] = { p[i][0] - C[0], p[i][1] - C[1], p[i][2] - C[2] };
        float up = dot3(B[2], d);
        best = maxf(best, c->inv_sin_h * fabsf(up));
        up += B[2][2] * c->s.rep_monster_height * k;
        best = maxf(best, c->inv_sin_h * fabsf(up));
        best = maxf(best, (5 * k + fabsf(d[0])) * c->inv_sin_hh);
    }
    return best;
}

/* 0x313A70: the height and depth from which the view's lower and upper edges just hold every point */
static void fit_edges(const float (*p)[3], int n, float tilt, float fov, float rmh, float k, float out[3])
{
    float half = 0.5f * fov;
    float sa = sinf(tilt + half + HALF_PI), ca = cosf(tilt + half + HALF_PI);
    float sb = sinf(tilt - half + HALF_PI), cb = cosf(tilt - half + HALF_PI);
    float f6 = -1e20f;
    for (int i = 0; i < n; i++)
        f6 = maxf(f6, ca * p[i][1] + sa * p[i][2]);
    float f5 = cb * (-15 * k);
    f6 += k * sa * rmh;
    float det = sb * ca - cb * sa, inv = 1 / det;
    out[1] = inv * (-(f5 * sa - sb * f6));
    out[2] = inv * (-(-(f5 * ca) + cb * f6));
}

/* 0x312950: near the ground */
static void place_near(const GameCamera *c, const float C[3], const float bmin[3], const float bmax[3],
                       const float (*p)[3], int n, int held, float k, float out[3])
{
    float lower = c->tilt - 0.5f * c->s.fov;
    fit_edges(p, n, c->tilt, c->s.fov, c->s.rep_monster_height, k, out);
    out[0] = C[0];
    float half_width = 0.5f * ((bmax[0] - bmin[0]) + 20 * k);
    float need = half_width * c->cot_hh;
    float d[3] = { out[0] - C[0], out[1] - C[1], out[2] - C[2] }, dist2 = dot3(d, d);
    if (need * need > dist2) {                         /* back off along the lower edge until the width fits */
        float back = (need - sqrtf(dist2)) / c->cos_h;
        out[1] -= back * cosf(lower);
        out[2] -= back * sinf(lower);
    }
    float min_z = c->s.min_cam_height + (held ? c->s.held_obj_min_cam_dist_offset : 0);
    float below = min_z - out[2];
    if (below > 0) {
        out[2] = min_z;
        out[1] -= (-1 / tanf(lower)) * below;
    }
}

/* 0x312790: above the ground */
static void place_above(const GameCamera *c, const float B[3][3], const float C[3], const float (*p)[3], int n, int held,
                        float k, float out[3])
{
    float dist = fit_distance(c, B, C, p, n, k);
    for (int i = 0; i < 3; i++)
        out[i] = C[i] - dist * B[1][i];
    float more = c->s.min_cam_dist + (held ? c->s.held_obj_min_cam_dist_offset : 0) - dist;
    if (more > 0) {
        float a = c->tilt + 0.5f * c->s.fov;
        more /= c->cos_h;
        out[2] -= sinf(a) * more;
        out[1] -= cosf(a) * more;
    }
}

/* 0x312600: keep the side of the view inside x = +-60 at y = -15 */
static void keep_in_street(const GameCamera *c, const float B[3][3], float k, float P[3])
{
    float v[3];
    for (int i = 0; i < 3; i++)
        v[i] = c->cos_hh * B[0][i] - c->sin_hh * B[1][i];
    float n2 = dot3(v, v);
    if (n2 < 1e-6f) { v[0] = 1; v[1] = v[2] = 0; }
    else { float n = 1 / sqrtf(n2); v[0] *= n; v[1] *= n; v[2] *= n; }
    float dist = (P[0] < 0 ? -v[0] : v[0]) * P[0] + v[1] * P[1] + v[2] * P[2];
    float e = dist - (60 * v[0] - 15 * v[1]) * k;
    if (e > 0) {
        e /= v[0];
        P[0] += P[0] < 0 ? e : -e;
    }
}

static void quat_from_rows(const float m[3][3], float q[4])
{
    float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        float s = sqrtf(tr + 1) * 2;
        q[3] = 0.25f * s; q[0] = (m[1][2] - m[2][1]) / s; q[1] = (m[2][0] - m[0][2]) / s; q[2] = (m[0][1] - m[1][0]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[1][2] - m[2][1]) / s; q[0] = 0.25f * s; q[1] = (m[1][0] + m[0][1]) / s; q[2] = (m[2][0] + m[0][2]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[2][0] - m[0][2]) / s; q[0] = (m[1][0] + m[0][1]) / s; q[1] = 0.25f * s; q[2] = (m[2][1] + m[1][2]) / s;
    } else {
        float s = sqrtf(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[0][1] - m[1][0]) / s; q[0] = (m[2][0] + m[0][2]) / s; q[1] = (m[2][1] + m[1][2]) / s; q[2] = 0.25f * s;
    }
}

static void rows_from_quat(const float q[4], float m[3][3])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1 - 2 * (y * y + z * z); m[0][1] = 2 * (x * y + z * w); m[0][2] = 2 * (x * z - y * w);
    m[1][0] = 2 * (x * y - z * w); m[1][1] = 1 - 2 * (x * x + z * z); m[1][2] = 2 * (y * z + x * w);
    m[2][0] = 2 * (x * z + y * w); m[2][1] = 2 * (y * z - x * w); m[2][2] = 1 - 2 * (x * x + y * y);
}

void cam_update(GameCamera *c, const float (*monsters)[3], int n, float dt, float aspect_hw)
{
    if (n <= 0)
        return;
    derive(c, aspect_hw);
    float bmin[3] = { 1e30f, 1e30f, 1e30f }, bmax[3] = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            bmin[k] = minf(bmin[k], monsters[i][k]);
            bmax[k] = maxf(bmax[k], monsters[i][k]);
        }
    float C[3] = { 0.5f * (bmin[0] + bmax[0]), 0.5f * (bmin[1] + bmax[1]), 0.5f * (bmin[2] + bmax[2]) };
    C[2] += 0.5f * c->s.rep_monster_height;
    float spread = minf((bmax[0] - bmin[0]) / 110, 1);
    spread *= spread;
    int near = bmin[2] <= c->s.show_ground_height;
    if (near != c->state) {
        c->timer += dt;
        if (c->timer > c->s.state_trans_time) {
            c->state = near;
            c->timer = 0;
        }
    } else {
        c->timer = 0;
    }
    float target = c->state == 1 ? c->s.near_tilt + c->s.far_extra_tilt * spread : c->s.above_tilt;
    if (c->first) {
        c->tilt = target;
    } else {
        float diff = target - c->tilt, step = dt * c->s.max_angular_vel;
        c->tilt += diff > step ? step : diff < -step ? -step : diff;
    }
    float st = sinf(c->tilt), ct = cosf(c->tilt);
    float B[3][3] = { { 1, 0, 0 }, { 0, ct, st }, { 0, -st, ct } };   /* right, forward, up */
    float k = 1 + spread * (c->s.far_border_scale - 1), P[3];
    if (c->state == 1)
        place_near(c, C, bmin, bmax, monsters, n, 0, k, P);
    else
        place_above(c, B, C, monsters, n, 0, k, P);
    keep_in_street(c, B, k, P);
    float yaw = c->s.yaw * P[0] * (1.0f / 60);
    yaw = maxf(-c->s.yaw, minf(c->s.yaw, yaw));
    float cy = cosf(yaw), sy = sinf(yaw);
    float R[3][3] = { { cy, sy, 0 }, { -sy, cy, 0 }, { 0, 0, 1 } }, M[3][3];
    for (int r = 0; r < 3; r++)
        for (int col = 0; col < 3; col++)
            M[r][col] = B[r][0] * R[0][col] + B[r][1] * R[1][col] + B[r][2] * R[2][col];
    float d[3] = { C[0] - P[0], C[1] - P[1], C[2] - P[2] };
    for (int col = 0; col < 3; col++)                    /* turn the offset about the centre: P = C - d R */
        P[col] = C[col] - (d[0] * R[0][col] + d[1] * R[1][col] + d[2] * R[2][col]);
    float q[4];
    quat_from_rows(M, q);
    if (c->first) {
        memcpy(c->q, q, sizeof q);
        memcpy(c->pos, P, sizeof P);
        c->first = 0;
    } else {
        if (q[0] * c->q[0] + q[1] * c->q[1] + q[2] * c->q[2] + q[3] * c->q[3] < 0)
            for (int i = 0; i < 4; i++) q[i] = -q[i];
        float t = maxf(0, minf(1, c->s.smoothing)), n2 = 0;
        for (int i = 0; i < 4; i++) { c->q[i] = q[i] + t * (c->q[i] - q[i]); n2 += c->q[i] * c->q[i]; }
        n2 = 1 / sqrtf(n2);
        for (int i = 0; i < 4; i++) c->q[i] *= n2;
        for (int i = 0; i < 3; i++) c->pos[i] = P[i] + t * (c->pos[i] - P[i]);
    }
    rows_from_quat(c->q, c->rot);
}

/* ---------------------------------------------------------------- shake */

const ShakeDef SHAKE_MONSTER = { 0.75f, { { 0.005f, -0.005f }, { 0.005f, -0.005f } }, { { 160, -40 }, { 135, -50 } } };
const ShakeDef SHAKE_COLLAPSE = { 0.5f, { { 0.014f, -0.014f }, { 0.014f, -0.014f } }, { { 160, -80 }, { 135, -100 } } };

static float shake_mag(const CamShake *s, float t)   /* the larger of the two magnitudes at t */
{
    float a = s->def->mag[0][0] + s->def->mag[0][1] * t, b = s->def->mag[1][0] + s->def->mag[1][1] * t;
    return a > b ? a : b;
}

void shake_start(CamShake *s, const ShakeDef *def, const float pos[3], float strength, int check, uint32_t *seed)
{
    if (check && s->def && s->age < s->def->duration) {
        CamShake n = { def, 0, strength, { 0, 0 }, { 0, 0, 0 } };
        if (s->strength * shake_mag(s, s->age / s->def->duration) > strength * shake_mag(&n, 0))
            return;
    }
    s->def = def;
    s->age = 0;
    s->strength = strength;
    for (int k = 0; k < 2; k++) {                    /* Park-Miller (0x106BD0) x 2^-31 x 2 pi */
        *seed = (uint32_t)((uint64_t)(*seed ? *seed : 1) * 16807 % 2147483647u);
        s->phase[k] = (float)*seed / 2147483648.0f * 6.2831853f;
    }
    memcpy(s->pos, pos, sizeof s->pos);
}

void shake_step(CamShake *s, int n, float dt)
{
    for (int i = 0; i < n; i++)
        if (s[i].def && s[i].age < s[i].def->duration)
            s[i].age += dt;
}

void cam_shaken(const float pos[3], const float rows[3][3], const CamShake *s, int n, float out[3][3])
{
    float X = 0, Y = 0;
    memcpy(out, rows, sizeof(float) * 9);
    for (int i = 0; i < n; i++) {
        const ShakeDef *d = s[i].def;
        if (!d || s[i].age >= d->duration)
            continue;
        float t = s[i].age / d->duration, dd = 0, v[2];
        for (int k = 0; k < 3; k++)
            dd += (s[i].pos[k] - pos[k]) * (s[i].pos[k] - pos[k]);
        float dist = sqrtf(dd), f = dist < 100 ? 1 : dist <= 800 ? 1 - (dist - 100) * 0.00114286f : 0.2f;
        for (int k = 0; k < 2; k++)                  /* strength x mag(t) x sin(phase + theta(t)) */
            v[k] = s[i].strength * (d->mag[k][0] + d->mag[k][1] * t) *
                   sinf(s[i].phase[k] + d->theta[k][0] + d->theta[k][1] * t);
        X += v[0] * f;
        Y += v[1] * f;
    }
    if (X == 0 && Y == 0)
        return;
    float sx = sinf(X), cx = cosf(X), sy = sinf(Y), cy = cosf(Y);
    float S[3][3] = { { cx, sx, 0 }, { -cy * sx, cy * cx, sy }, { sy * sx, -sy * cx, cy } };   /* 0x101330 */
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++)
            out[i][k] = S[i][0] * rows[0][k] + S[i][1] * rows[1][k] + S[i][2] * rows[2][k];
    if (fabsf(out[0][2]) > 0.001f) {                 /* no roll: right stays level, the rest made square again */
        out[0][2] = 0;
        float l = sqrtf(out[0][0] * out[0][0] + out[0][1] * out[0][1]);
        out[0][0] /= l;
        out[0][1] /= l;
        float d = out[1][0] * out[0][0] + out[1][1] * out[0][1];
        for (int k = 0; k < 3; k++)
            out[1][k] -= d * out[0][k];
        l = sqrtf(out[1][0] * out[1][0] + out[1][1] * out[1][1] + out[1][2] * out[1][2]);
        for (int k = 0; k < 3; k++)
            out[1][k] /= l;
        out[2][0] = out[0][1] * out[1][2] - out[0][2] * out[1][1];
        out[2][1] = out[0][2] * out[1][0] - out[0][0] * out[1][2];
        out[2][2] = out[0][0] * out[1][1] - out[0][1] * out[1][0];
    }
}
