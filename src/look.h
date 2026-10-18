/* look.h - three small monster behaviours (docs/LOOK_NOTES.md):
 *   1. the face queue: setFacialAnim / queueFacialAnim and what plays when a face clip ends (0x2E9350, 0x2E8FB0,
 *      0x2E95D0), plus Monsters.bsf's faceAnim and its delayed belch;
 *   2. the head turn (MonsterHeadAnimator, vtable 0x44B020): the right stick in a look state, and the target
 *      tracking the game has but never switches on;
 *   3. the velocity's friction and drag in the monster update (0x2DE530: modes 0 / 2 / 4 at 0x2DF6F0, mode 1 at
 *      0x2DEBD8) and the knocks that set it (0x2D6A50);
 *   4. the tail (MonsterTailAnimator, vtable 0x44AFE0): the "Bip01 Tail" links on springs while walking.
 * Positions are the game's: z up. Matrices as city.h's Mat4 (column vectors, column-major, m[12..14] the place). */
#ifndef RAMPAGE_LOOK_H
#define RAMPAGE_LOOK_H
#include <stdint.h>
#include "city.h"

/* ---- 1. the face ---------------------------------------------------------------------------------------------- */

#define FACE_RECORDS 8                               /* the pool at monster +0xBAB8, 8 x 0x24 bytes (0x2EA0E0) */
typedef struct {
    const void *node;                                /* the state node played on the face (MoveNode *), never NULL */
    const char *name;
    int head;                                        /* the head layer: the mask with the head bone itself (+0xC) */
    int loop;                                        /* +0x1C */
    int plays;                                       /* +0x20: plays left (counted down at each end, 0 = once) */
} FaceRec;
typedef struct {
    FaceRec list[FACE_RECORDS]; int n;               /* +0xBBD8 .. +0xBBDC: list[0] plays, the rest wait in order */
    float belch[4]; int nbelch;                      /* Monsters::appendBelch timers (installObjectTimer, once) */
} FaceQueue;

enum { FACE_NONE = 0, FACE_START, FACE_RESTART, FACE_DEFAULT };

void face_init(FaceQueue *q);
/* setFacialAnim (0x2E9350): list[0] is replaced and must start now (FACE_START: its clip from 0 on the face layer,
 * blended in over its node's blend when > 0, and its clip events restarted); what waits behind stays. node NULL (no
 * state node of that name) or no free record: nothing happens (FACE_NONE). */
int face_set(FaceQueue *q, const void *node, const char *name, int head, int loop, int plays);
/* queueFacialAnim (0x2E8FB0): appended behind; nothing starts. 1 = queued */
int face_queue(FaceQueue *q, const void *node, const char *name, int head, int loop, int plays);
/* the face clip reached its end (0x2E95D0): FACE_RESTART list[0] again (looping or plays left), FACE_START the next
 * queued one (now list[0]), FACE_DEFAULT nothing left: run Monsters.bsf's DefaultFacialAnim, i.e.
 * face_set(Expression_Idle, 0, 1, 0) and start it */
int face_clip_end(FaceQueue *q);

/* Monsters.bsf faceAnim(expression, head layer, delay, plays), a clip event: the expression's state name (NULL for a
 * number it does not know), never looping; a delay > 1 installs appendBelch, which plays Expression_Burp (0, 0, 1)
 * that many seconds later. Call face_set(name, *head, 0, *plays) with the result. */
const char *face_anim_event(FaceQueue *q, const float p[4], int *head, int *plays);
/* the belch timers; 1 when one fired this frame: face_set(Expression_Burp, 0, 0, 1) */
int face_tick(FaceQueue *q, float dt);

/* ---- 2. the head ---------------------------------------------------------------------------------------------- */

typedef struct {                                     /* MONSTER_DATA settings, the same in all 30 monsters */
    float yaw_rate;                                  /* +0x174: pi / 2 rad / s (the neck) */
    float pitch_rate;                                /* +0x178: pi / 2 (the head) */
    float yaw_max;                                   /* +0xA08: 81 degrees either way */
    float pitch_pos, pitch_neg;                      /* +0xA0C, +0xA10: 45 degrees */
} LookLimits;
void look_limits_default(LookLimits *l);

typedef struct {
    float yaw, yaw_to;                               /* +0xBC, +0xC0: the neck ("Bip01 Head"'s parent) about the
                                                        model's up */
    float pitch, pitch_to;                           /* +0xE8, +0xEC: "Bip01 Head" about its own z (applied -pitch) */
    int enabled;                                     /* +0x118: tracking a target (never on in this build) */
    int manual;                                      /* +0x119: the right stick holds it (halves the rates) */
} HeadLook;

/* the monster's command (0x2E70C0 -> 0x2FBE30), each frame: in a state whose node has NODE_LOOK (0x200) the right
 * stick (rx, ry: -1 .. 1 as the pad gives them, y down) is the target, held while it is off centre; outside, look 0 */
void look_stick(HeadLook *h, const LookLimits *l, int look_state, float rx, float ry);
/* the update (0x2DEA08 -> 0x2FBF80, 0x2FBC90), each frame: tracking is on only with a target and a node flag 0x20
 * (no monster's node has it, and nothing sets the target: off in this build). Off and not held: both targets 0. */
int look_update(HeadLook *h, const LookLimits *l, int has_target, uint32_t node_flags);   /* 1: call look_track */
/* 0x2FB950: the targets towards a point (tracking on and not held); all in the model's frame: the target, the
 * neck's and the head's places, fwd the Tracking_Node's x axis (the way the face looks) */
void look_track(HeadLook *h, const LookLimits *l, const float target[3], const float neck[3], const float head[3],
                const float fwd[3]);
/* 0x2FBF90, each frame as the pose is built: the angles move towards their targets at their rates (half while held) */
void look_step(HeadLook *h, const LookLimits *l, float dt);
/* the turn on a posed skeleton (model-space node matrices, as ik_arm takes them): the neck turns about the model's
 * up by yaw (its place kept), the head about its own z by -pitch, and everything below them follows. neck < 0 or
 * neck == head (MONSTER_DATA +0xA14 = 0, one bone): the head takes both. */
void look_pose(Mat4 *node, const Tree *tree, int neck, int head, float yaw, float pitch);

/* ---- 3. the velocity ------------------------------------------------------------------------------------------ */

#define MONSTER_FRICTION 50.0f                       /* +0xF4 (units / s^2): at spawn (0x2E98C0), after an army knock
                                                        (0x2D79E0) or a hit in the air (0x2D77C8) */
#define MONSTER_AIR_DRAG 0.002f                      /* x |v|^2 (0x2DECC4) */
#define MONSTER_FRICTION_Z 0x80002u                  /* node flags (+0x2C): the friction and travel take z too */

/* modes 0, 2 and 4 (0x2DF6F0): the up velocity is dropped (unless a slot's state is waiting to start, +0x110), the
 * monster moves by the velocity (x, y; z with MONSTER_FRICTION_Z), then the velocity loses friction x dt along its
 * own direction and stops when that would turn it round. pos may be NULL. */
void monster_ground_velocity(float vel[3], float pos[3], float friction, float dt, uint32_t node_flags,
                             int slot_pending);
/* mode 1 (0x2DEBD8): drag MONSTER_AIR_DRAG x |v|^2 x dt against the velocity, the move, then gravity (60) unless
 * setDisableGravity. pos may be NULL. */
void monster_air_velocity(float vel[3], float pos[3], float dt, int no_gravity);
/* modes 0, 2, 4 after the move (0x2E0330): the height eases onto the ground under it, 0.3 of the way a world step
 * (1/30 s; dt the frame's), none while gravity is off, at once within 0.001 */
float monster_ground_follow(float z, float ground, int no_gravity, float dt);

/* the knocks of a hit (0x2D6A50). A blow's slide: the monster slides `dist` in `time` and stops: the velocity
 * dir x 2 dist / time, the returned friction 2 dist / time^2 (keep it until the next knock). dir 1 long. */
float monster_knock_ground(float vel[3], const float dir[3], float dist, float time);
/* a blow's flight: dir x dist / time, up tan(angle) x that speed; friction back to MONSTER_FRICTION */
float monster_knock_air(float vel[3], const float dir[3], float dist, float time, float angle);
/* a shot's knock (army, 0x2D9650): per axis the larger of the knock and the velocity; friction MONSTER_FRICTION */
float monster_knock_shot(float vel[3], const float knock[3]);

/* a monster's blow on another monster: 0x2D6A50 with the attacker's attack record (the fight, 0x2E35D0; the clip's
 * attack event, docs/FORMAT_MONSTER.md "Clip events" type 0) */
typedef struct {
    int level, kind, side;                           /* the record's ReactionLevel (+0x24), the reaction's kind (+0x20:
                                                        1 the street's, 3 from above, 4 from below, 0 and 2 by the
                                                        side; no attack has 0 or 2) and its side (+0x1C, 0 in all) */
    float dist, time, lift, turn;                    /* +0x30 the knock's distance, +0x34 its time, +0x38 the lift's
                                                        angle, +0x3C the knock's turn about z (0 in all) */
    int over[4];                                     /* +0x64 / +0x68 the slot in front / behind on the ground, +0x6C /
                                                        +0x70 in the air; MONSTER_BLOW_NONE none */
    float from[2], fwd[2], right[2];                 /* the attacker: its place, its front and its right (the +0x14 and
                                                        +0x8 rows of its matrix) */
    int aim, mirror;                                 /* its state's node aims the blow (+0x62 has 1 and 2), it plays
                                                        mirrored (+0x7DA1) */
} MonsterBlow;
#define MONSTER_BLOW_NONE 74                         /* an override slot left empty (as the record's default, 0x2D6B38) */
#define MONSTER_BLOW_BLOCKED (-2)                    /* monster_blow: no hit at all (no damage, knock or reaction) */
#define MONSTER_AIR_HITS 3                           /* the blows a monster takes in the air (+0xA9C, 0x2D7198) */
/* the blow on the victim (its place at[2], right[2], mirrored, its mode and whether the attacker is behind it): its
 * velocity and friction knocked; returns the reaction slot (-1 none) or MONSTER_BLOW_BLOCKED for a blow past the
 * air hits (*air_hits counts the blows taken in the air, mode 1) */
int monster_blow(const MonsterBlow *b, const float at[2], const float right[2], int mirror, int mode, int behind,
                 int *air_hits, float vel[3], float *friction);

/* ---- 4. the tail ---------------------------------------------------------------------------------------------- */

/* MonsterTailAnimator (vtable 0x44AFE0, 0x468 bytes, two in the animation player at +0x113C; made by its setup
 * 0x301AE0 for the node named "Bip01 Tail", constructor 0x2FABC0): the links below it ("Bip01 Tail1", "Bip01 Tail2"
 * .. or "Bip01 Tail01" ..) hang on springs in the world, and while its weight is up they replace the clip's tail. The
 * settings are MONSTER_DATA's, the same in all 30 monsters. Quaternions are the game's (x, y, z, w; row vectors, as
 * mat_tq takes them). */
#define TAIL_LINKS 31                                /* the arrays hold 32 with the base's */
#define TAIL_STEP (1 / 30.0f)                        /* +0xA04: the springs' fixed step */
#define TAIL_PULL 0.05f                              /* +0x9FC: the share of the way to its parent's turn a link takes */
#define TAIL_DAMP 0.2f                               /* +0xA00: the share of its spin a link loses, each step */
#define TAIL_COS 0.7071068f                          /* +0x9F8, +0x9F4: cos and sin of 45 degrees: a link turns */
#define TAIL_SIN 0.7071068f                          /* at most 90 degrees from its parent */
typedef struct TailAnim {
    int ready;                                       /* the chain was looked for (tail_find) */
    int base, n;                                     /* +0x18 the "Bip01 Tail" node, +0x20 the links (n = 0: none);
                                                        the links are the nodes base + 1 .. base + n (+0x1C) */
    float q[TAIL_LINKS + 1][4];                      /* +0x24: each link's turn in the world, [0] the base's */
    float v[TAIL_LINKS + 1][4];                      /* +0x234: each link's spin per step */
    float weight;                                    /* +0x454: the share of the springs in the pose (0 .. 1) */
    float left, inv, rate, target;                   /* +0x44C, +0x450, +0x458, +0x45C: the weight's blend */
    float acc;                                       /* +0x460: time not yet stepped */
    int reset;                                       /* +0x464: the links start again from the base's turn */
    float dt;                                        /* play time since the last posing (the caller adds to it) */
} TailAnim;
/* 0x2FABC0: the chain below the node named "Bip01 Tail" (each with one child named "Bip01 Tail<k>" or
 * "Bip01 Tail0<k>"); ready, the springs to start from the base. The weight and its blend are kept (zero the struct
 * for a new monster: weight 0) */
void tail_find(TailAnim *t, const Tree *tree);
/* 0x2FAB00 (from the monster's command 0x2E6580, as a state starts): the weight goes to `target` (the new state's node
 * +0x10) over `seconds` (its blend time, node +0x14); 0.001 or less leaves the weight where it is */
void tail_blend(TailAnim *t, float target, float seconds);
/* the update (vtable +0x8, 0x2FAB70, from the monster update at 0x2DEA78 after the pose is built): on a posed
 * skeleton (model-space node matrices as drawn, the monster's world matrix `world`), the weight's blend, the springs
 * stepped by t->dt (then 0) and, with the weight above 0, the links' model matrices rewritten */
void tail_pose(TailAnim *t, Mat4 *node, const Tree *tree, const Mat4 *world);

#endif
