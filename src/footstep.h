/* footstep.h - the monster's footfalls and landings (docs/FOOTSTEP_NOTES.md):
 *   1. the effect names: the property book's MonsterFx group (read by 0x2F5D80 into the monster's effect record at
 *      +0x7DFC: GroundFootStepFx +0x140, WaterFootStepFx +0x144, GroundImpactFx +0x150, WaterImpactFx +0x154);
 *   2. the clip events Monsters::footStep / footPlant / cancelFootStep (script methods 0x88 / 0x87 / 0x89, the
 *      monster's foot object at +0xC81C: 0x33CDA0, 0x33CCC0, 0x33CA10): footPlant puts a footfall under the foot,
 *      footStep lifts the foot, cancelFootStep plants the feet where they stand;
 *   2b. the foot lock (0x33BF10: the root follows the planted foot) and the foot IK's targets (0x33D450: the planted
 *      foot held on the ground, the dragged one on its arc; ik.c's ik_feet bends the legs);
 *   3. the landing (0x2DC450): the big footfall under the monster's middle.
 * Engine-free: the caller looks the bones up, knows the ground and the water, and starts the effects itself
 * (fx_spawn(w, fx.name, fx.pos, NULL, 1)). Positions are the game's: z up. Foot 0 is the clip's left foot. */
#ifndef RAMPAGE_FOOTSTEP_H
#define RAMPAGE_FOOTSTEP_H
#include <stdint.h>
#include "data.h"

/* ---- 1. the names --------------------------------------------------------------------------------------------- */

typedef struct {                                     /* "" = none: that effect is never started (0x2F4A20 ...) */
    char ground_step[64];                            /* GroundFootStepFx: "footfall.edf" in all seven books */
    char water_step[64];                             /* WaterFootStepFx: "footfall_water.edf" */
    char ground_impact[64];                          /* GroundImpactFx: "footfall_big.edf" */
    char water_impact[64];                           /* WaterImpactFx: "footfall_big_water.edf" */
} FootProps;

/* the names every book gives (Amanda, Cal, George, Jill, Lizzie, Ralph, Rhett all agree) */
void foot_props_default(FootProps *p);
/* the book's MonsterFx group (city_book_group(city, "George_prop", "MonsterFx")): each key it has replaces the name,
 * each it lacks is none, as the game reads it; no group (at = 0): the defaults stay. How many keys were found. */
int foot_props_read(FootProps *p, Group monster_fx);

/* the bones (the animation player's setup 0x301AE0 finds them by these names, not through HandFootList): a clip's
 * foot 0 is "Bip01 L Foot" and 1 "Bip01 R Foot"; mirrored they swap (0x33CCC0: foot = foot + 1 & 1), which with
 * scene_monster_node (the drawn side's node, reflected) is the clip's foot as drawn. */
const char *foot_bone(int foot, int mirrored);
#define FOOT_IMPACT_BONE "Bip01 Spine1"              /* the landing's place (0x2E6000: anim +0x72C8, gp-0x71E4) */

/* ---- 2. the steps --------------------------------------------------------------------------------------------- */

/* the monster as the foot object reads it, this frame */
typedef struct {
    float foot[2][3];                                /* the feet's places in the world: foot_bone(f, mirrored) */
    float place[3];                                  /* the monster's place (+0x2C) */
    float fwd[3];                                    /* its forward axis (its model y: "forward 0 1"), 1 long */
    float vel[3];                                    /* getVelocity (0x2E7460): +0xBD30 plus the stick's +0xA68 */
    float ground;                                    /* 0x2E8530: the height it stands on (its roof's top on a
                                                      * building, the world's ground, the floor under water) */
    int in_water;                                    /* 0x2D5F00: its box is in a water's box */
    float water_z;                                   /* that water's surface (the water's +0x34: Origin z) */
    uint32_t node_flags;                             /* not read: cancelFootStep tests the state left (FootState
                                                      * node_flags, foot_command) */
    int mirrored;                                    /* +0x7DA1 */
    /* for the drag (cancelFootStep's 0x33C5A0): the feet in the model's frame as the clip now started poses them on
     * its own at its time 0 (the animation player's clip query, ik_clip_node(in, 0, ..); indexed as foot[]); clip_ok
     * 0: not given, no lift */
    float clip_foot[2][3];
    int clip_ok;
} FootPose;

/* the foot record (0x28 bytes from foot object +0, one per foot, indexed after the mirror swap: foot r is the one
 * drawn on the left for 0, on the right for 1) */
typedef struct {
    float weight;                                    /* +0x0: 1 held (planted or dragged), 0 free with the clip */
    float shown;                                     /* +0x4: the IK's share, eased towards weight (FOOT_EASING) */
    float lift;                                      /* +0x8: a dragged foot's arc, its height (0x33C5A0) */
    float at[3];                                     /* +0xC: planted: where, z the ground + the ankle; else the
                                                      * foot as posed (a dragged one at least on its arc) */
    float held[3];                                   /* +0x18: the IK's target, eased towards at (FOOT_HELD) */
    uint32_t flags;                                  /* +0x24: FOOT_PLANTED, FOOT_DRAGGED, FOOT_HELD, FOOT_EASING */
} FootRec;
#define FOOT_PLANTED 1u
#define FOOT_DRAGGED 2u
#define FOOT_HELD 4u                                 /* held[] has started (0x33CDE0, the first update) */
#define FOOT_EASING 0x10u                            /* shown has started (0x33CDE0) */
typedef struct {
    FootRec rec[2];
    int anchor;                                      /* +0x6C: the planted foot the root follows, -1 none */
    uint32_t node_flags;                             /* +0x74: the flags of the state the monster last left
                                                      * (foot_command), 0 before any */
    int no_drag;                                     /* +0x69: MiscData|DisableFootDrag (Lizzie, Ralph) */
    float ankle;                                     /* +0x50: added to the ground for a planted foot: the right
                                                      * foot's height in the bind pose (0x33D4E0; foot_ik_props) */
    /* the foot IK's settings (foot_init's defaults, foot_ik_props) */
    float keep[4];                                   /* +0x54 .. +0x60: the share kept a frame of the target's
                                                      * place and of the share: planted 0.5, 0.5; else 0.7, 0.7 */
    float rtw;                                       /* +0x64: MiscData|RTWSmoothFactor (0.7; Lizzie, Ralph 0.85) */
    float drag_scale;                                /* monster +0xAC4: MiscData|FootIKDragHeightScale (1; 0.5) */
    int links;                                       /* the IK: 3 the 2-link legs, 4 MiscData|Use3LinkFootIK (Lizzie,
                                                      * Ralph); 0 none (no MiscData: 0x2E5640 makes no IK) */
    float lift_cap; int lift_cap_set;                /* gp-0x6C68: 3 x the ankle, kept from the first drag */
} FootState;

/* an effect to start: fx_spawn(w, name, pos, NULL, 1) (the game: no direction, the identity frame, no scale) */
typedef struct {
    const char *name;                                /* a FootProps name, never "" */
    float pos[3];
    int water;                                       /* the water's one (the step does not press the water: the
                                                      * wading does that, every frame, on its own) */
    int slot;                                        /* the game's emitter: 0 / 1 a foot's (+0x148 / +0x14C, after
                                                      * the mirror swap), 2 the landing's (+0x158) */
} FootFx;

/* the records cleared, no anchor, DisableFootDrag; the IK's settings at the game's defaults (2-link, RTW 0.7,
 * drag height scale 1, ankle 0) until foot_ik_props */
void foot_init(FootState *s, int disable_foot_drag);
/* the monster's command to a new state (0x2E6580) first hands the foot object the state node it still has, the one
 * being left (0x33CCA0: +0x74 = monster +0x10C, before 0x2E6848 sets the new one): call it with the old state's node
 * flags at every state change, before the new clip's events */
void foot_command(FootState *s, uint32_t leaving_flags);
/* Monsters::footStep(foot) (0x33CDA0): the foot lifts: free, neither planted nor dragged. No effect. */
void foot_step(FootState *s, int foot, int mirrored);
/* Monsters::footPlant(foot) (0x33CCC0): only while the monster does not move (|vel|^2 < 1e-6), the footfall at the
 * foot's (x, y) on the ground it stands on, or on the water's surface (WaterFootStepFx). The foot's record is not
 * touched. 1 = *fx is to be started. */
int foot_plant(FootState *s, const FootProps *p, int foot, const FootPose *m, FootFx *fx);
/* Monsters::cancelFootStep(foot) (0x33CA10): the clip's start in a standing state (every such clip sends it for 0
 * and 1; only foot 0 does anything). Coming from a state that travels (the state left, s->node_flags, has 0x50 or
 * 0x1080: NODE_AIR_X | NODE_ROOT_X, NODE_AIR_Y | NODE_ROOT_Y) and not moving: the foot further forward is planted
 * where it stands and becomes the anchor; the other is dragged (its arc's height from m->clip_foot: 0x33C5A0), or let
 * go with DisableFootDrag. No effect. Returns the anchor, -1 when nothing happened. */
int foot_cancel(FootState *s, int foot, const FootPose *m);

/* ---- 2b. the foot lock and the foot IK's targets --------------------------------------------------------------- */

/* after foot_init: the book's MiscData (city_book_group(city, book, "MiscData"); at = 0: no foot IK, as the game
 * makes none) for Use3LinkFootIK, FootIKDragHeightScale and RTWSmoothFactor (DisableFootDrag is foot_init's), and
 * the ankle from the skeleton (foot_ankle; NULL: 0) */
void foot_ik_props(FootState *s, Group misc, const Tree *skeleton);
/* [R] 0x33D4E0's +0x50: the height of "Bip01 R Foot" in the bind pose (its inverse bind matrix inverted): George
 * 0.528, Rhett 0.648, Ralph and Lizzie 0.514; 0 without the node */
float foot_ankle(const Tree *skeleton);

/* the foot lock (0x33BF10, from the monster update at 0x2E0EFC once the frame's move is known, before the matrix is
 * set): while there is an anchor, the root follows the planted foot: the anchor foot as its clip alone poses it at its
 * time 0 (foot: per record, model space as drawn: ik_clip_node(in, 0, ..)), put through the monster's matrix
 * (place, heading degrees about z), is short of where it was planted by a slip (x, y); the place moves by
 * (1 - RTWSmoothFactor) of it. The
 * anchor is dropped as soon as the state now (node_flags: the current state's) travels (0x50, 0x1080) or the anchor
 * is no longer planted (footStep), and once the slip is within 1e-3. 1 = place moved. */
int foot_lock(FootState *s, uint32_t node_flags, const float foot[2][3], float place[3], float heading);

/* the monster this frame as the foot object's update reads it (0x33D450, after the matrix is set) */
typedef struct {
    float place[3], heading;                         /* its matrix: the place, the heading (degrees about z) */
    float ground;                                    /* 0x2E8530 */
    /* per record (0 drawn on the left, 1 on the right), model space as drawn, as the clips pose them before the IK
     * (IkFeet's foot, hip, reach, ok) */
    int ok[2];
    float foot[2][3];                                /* the foot (0x2E5C40) */
    float hip[2][3];                                 /* its leg's first node (0x2F8F50, link 0) */
    float reach[2];                                  /* its leg's length (0x2F9060) */
    float blend_left, blend_time;                    /* the clip blend (animation player +0x4E58, 1 / +0x4E5C):
                                                      * a drag lasts as long, on an arc of sin(pi x done) */
    float steps;                                     /* this frame in world steps (dt x 30; 0: one): the shares are
                                                      * per 1/30 s step (RWorld 0x2B7800) */
} FootFrame;
/* 0x33D450, every frame: each record's share eased (0x33CF90), a planted foot on the ground under the monster (+ the
 * ankle) and within its leg's reach, a free one where the clip has it, a dragged one at least on its arc until the
 * blend is over; the target eased (0x33CF90) and handed to the IK in the model's frame with the share (0x33CDE0).
 * w, target: per record (drawn left, right), IkFeet's w and pos. Nothing (shares 0) when s->links is 0. */
void foot_update(FootState *s, const FootFrame *m, float w[2], float target[2][3]);

/* ---- 3. the landing ------------------------------------------------------------------------------------------- */

/* 0x2DC450, every landing from the air: the big footfall (GroundImpactFx / WaterImpactFx) at FOOT_IMPACT_BONE's
 * (x, y), on the ground or the water's surface. 1 = *fx is to be started. */
int foot_impact(const FootProps *p, const float spine[3], float ground, int in_water, float water_z, FootFx *fx);

#endif
