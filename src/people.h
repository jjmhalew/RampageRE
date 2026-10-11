/* people.h - the people of a block and the monster's hold on them (docs/PEOPLE_NOTES.md).
 *
 * [R] The people are Actors (vtable 0x44B300; the constructor 0x30E3E0 sets object flag 0x80000, "an actor", which
 * is what makes them edible) driven by their template's script; this is the port of those scripts:
 * Person_Window.bsf (the window occupants), Person_Runner.bsf (the crowds of Traffic.bsf), WalkingAroundPerson.bsf
 * (street punks, dog walkers, old ladies), StationaryPerson.bsf (readers, priests, beggars), Tourist.bsf and
 * Person_Child.bsf, with the native pieces they lean on: the LineMover (update 0x315E10: points, speed, the
 * callbacks 2 end / 3 a point reached / 4 a monster within MonsterResponseDist), the actor's anim records
 * (ActorAnimShape|<state>, 0x30AAD0: Duration, AnimRepeat), the window occupants (the fall 0x317DC0, the spawn
 * 0x318650, the window's place 0x2BE660) and the SpherePhysicsMover a smashed person flies with (0x28DE70: Gravity
 * 9.81 unless the template says otherwise).
 *
 * [R] The monster side, the Monster script bindings behind Monsters.bsf (looked up by 0x2AC3B0, dispatched by
 * 0x2AA920 through the jump table 0x42CEE0):
 *   canGrabPerson(-, x, y, z)  = getBuildingOccupant (id 113, 0x2B37C0) == 1   -> people_can_grab
 *   doGrabPerson(-, x, y, z)   = grabBuildingOccupant(loc, 1) (id 115, 0x2B3350) -> people_grab(.., 0.25)
 *   doGrabObject("Helper R Hand") (id 120, 0x2D99C0 + 0x2DA4A0)                  -> people_nearest + people_grab(.., 0.1)
 *   hasGrabbedObject (id 121): 2 while it holds an actor (flag 0x80000, 0x2E5590), so isHoldlingEdible is 1 for people
 *   throwObject(eat, x, y, z) (id 122, 0x2D9DC0): eat = 1 -> people_eat, eat = 0 -> people_throw
 *   the hold each frame (HeldObjectMover 0x3109E0 at the LiftNodeName node)    -> people_hold
 *   an armed attack sphere reaching a person (TakeDamage)                       -> people_hit
 * Positions are the game's: z up, the street at z = 0, the buildings' fronts towards +y; headings in degrees, 0
 * facing +y and growing counter-clockwise (the game's facing z in radians x 180 / pi; FACE_RIGHT = -90 faces +x). */
#ifndef RAMPAGE_PEOPLE_H
#define RAMPAGE_PEOPLE_H
#include <stdint.h>
#include "data.h"
#include "collide.h"

typedef struct People People;

enum {                                               /* by the template's Script|ScriptName */
    PK_WINDOW, PK_RUNNER, PK_WALKER, PK_STATIONARY, PK_TOURIST, PK_CHILD,
    PK_OTHER,                                        /* any other script: its DefaultAnim only */
    PK_MIME, PK_BAND, PK_GUARD                       /* Mime.bsf, BandMember.bsf, RoyalGuard.bsf */
};
enum {
    PP_SCRIPT,                                       /* doing what its script says */
    PP_HELD,                                         /* in the monster's hand (object flag 0x20000 BeingHeld) */
    PP_FLYING,                                       /* smashed: flying off until its script deletes it */
    PP_GONE                                          /* deleted (eaten, back inside its window, run off, landed) */
};

typedef struct {
    const char *name;                                /* template name */
    Group tmpl;
    int kind, phase;
    int state;                                       /* the script's own state number (Def.state / nState) */
    float pos[3], heading;                           /* on the street, in a window, or falling (PP_SCRIPT) */
    /* the look: ActorAnimShape|<anim> of the template gives the model (Shape|ShapeName) and the clip (Anim|AnimName) */
    char anim[32];
    float anim_time, anim_len;                       /* seconds into the play, one play's length (its Duration or
                                                      * the clip's); the clip time is anim_time / anim_len x clip */
    int anim_loop;                                   /* AnimRepeat */
    int anim_done;                                   /* a play that does not repeat has ended (onAnimComplete sent) */
    float world[16];                                 /* column-major; always kept: pos and heading, or the hand / the flight */
    int building, storey, column;                    /* a window occupant's cell (-1 none) */
    uint32_t flags;                                  /* object flags: 0x10000 Pickupable, 0x20000 BeingHeld, 0x80000 actor */
    /* the script's variables */
    int sub, has_idle, is_grenadier, thrown, is_child;
    float state_time, delay_time, fly_duration, walk_range, walk_speed, reload, weapon_off;
    float start[3], run_heading;
    float pulse_delay, pulse_time;                   /* onPulse every pulse_delay seconds (< 0: never) */
    /* the LineMover */
    struct { float pts[4][3]; int n, seg, active; float t, speed, resp2; uint32_t flags; } mv;
    /* the hold and the flight */
    float blend, blend_left, from_q[4], from_p[3];   /* HeldObjectMover: blending in from where it was */
    float off_q[4], off_p[3];                        /* its +0x68 / +0x78: where it was, in the hold's frame */
    int off_set;
    float vel[3], spin[3];                           /* smashObject's velocity and turn rates */
    CollideMover cm;                                 /* the SpherePhysicsMover it flies on (0x28DE70) */
    float cmat[12];                                  /* its matrix while it flies (collide.h's 3 x 4) */
    /* Mime (curAnim, numAnimReps, endLoc, Stationary), RoyalGuard (marchDirection, idleTime, hutDestroyed, its hut) */
    int cur_anim, reps, stationary, hut_destroyed, hut;
    float end_loc[3], march_dir, idle_time;
    int anim_reverse;                                /* setAnimPlayDir(-1): anim_time runs backwards */
    int fx;                                          /* its PE_FX_ON effects still on: 1 << PFX_* */
} Person;

typedef struct {                                     /* what the people see of a monster */
    float pos[3];
    const char *move;                                /* its state name (Person_Child asks for "Ground...") */
} PeopleMonster;

/* what the people make happen elsewhere (sounds, effects, the monster's reactions) */
enum {
    PE_SOUND,                                        /* name: a 3D sound at the person (on = 0: stop that person's
                                                      * looping sound of that name, remove3DSfx) */
    PE_EFFECT,                                       /* name at v (node: on the monster, by that node; NULL: at v) */
    PE_DROP,                                         /* a droppable (the block's DroppableDef) falls from v */
    PE_WEAPON,                                       /* the grenadier's weapon (name: WeaponName) on / off at the monster */
    PE_MONSTER_KNOCK,                                /* the monster: setVel(v, world) and setReaction(2) */
    PE_MONSTER_HP,                                   /* the monster: addHitPoints(amount) */
    PE_MONSTER_FART,                                 /* Monsters::doFart: Expression_Smile, the Fart effect, +20 up */
    PE_WINDOW_CLOSE,                                 /* closeWindow (0x30C3F0, WindowReEnter's end): the window at v
                                                      * shuts over 0.5 s (city.h scene_window_close_at) */
    PE_FX_ON,                                        /* an effect of the person's own that can be stopped: name at v;
                                                      * on: its slot (PFX_*); amount 1: it follows the person
                                                      * (FollowEffect attached), 0: it stays at v */
    PE_FX_OFF                                        /* that slot's effect stops (on: the slot) */
};
enum {                                               /* PE_FX_ON's slots */
    PFX_AMBIENT,                                     /* the Actor's AmbientEffect (+0x108; the priests' halo) */
    PFX_CHALLENGE,                                   /* its ChallengeEffect (+0x110, NPC_Target): only when its
                                                      * template is one of the block's challenge goals (0x3466A0,
                                                      * challenge_is_goal), which the viewer decides */
    PFX_SCRIPT                                       /* the script's own (StationaryPerson's idleEffect, a kid's
                                                      * BalloonFX) */
};
/* monster: the monster slot it is about (PE_MONSTER_*: the one whose blow it was; otherwise 0) */
typedef struct { int type, who, on; const char *name, *node; float v[3], amount; int monster; } PeopleEvent;
typedef struct { float health, energy, points; int priest; } PeopleEaten;

/* the clip length the viewer knows: the clip anim (NULL: the shape's own RawAnim) of a look; 0 = none */
typedef float (*PeopleClipSeconds)(void *ctx, const char *anim, const char *shape);

People *people_new(PeopleClipSeconds clip_seconds, void *ctx);
void people_free(People *p);
/* the template of a name (gWorld.createActor / OTRef): Band.bsf's members, the huts' guards */
typedef Group (*PeopleFindTemplate)(void *ctx, const char *name);
void people_set_templates(People *p, PeopleFindTemplate find, void *ctx);
/* gWorld.isSphereVisible(pos, r): the streakers leave once out of view; unset, everything counts as visible */
typedef int (*PeopleVisible)(void *ctx, const float pos[3], float radius);
void people_set_visible(People *p, PeopleVisible visible, void *ctx);
void people_set_droppable_rate(People *p, float rate);   /* the block's DroppableDef rate (getDroppableRate) */

/* a person as the block or a script creates it (ActorInstance|, createActor): its script's Construct */
int people_add(People *p, Group tmpl, const float pos[3], float heading_deg);
/* a window occupant (0x318650): at the window's bottom middle on the front (0x2BE660: the grid's min x + 5 x
 * column + the middle of its window's Openable, the front's y, the storey's height + the Openable's bottom) */
int people_add_window(People *p, Group tmpl, const float at[3], int building, int storey, int column);
/* Traffic::createCrowd: 4 .. 8 people of the given templates (the block's occupants' "<Name>Runner") around centre,
 * running at 10 to 50 below it; Traffic.bsf makes two, at (-14, 0.5) and (8, -3), when the first block activates */
int people_crowd(People *p, const Group *tmpls, int ntmpls, const float centre[3]);
/* a LineMover for a person (gWorld.createLineMover, init, addPoint, setFlags): 1 face the way it goes, 2 loop,
 * 8 face the nearest monster */
void people_set_path(People *p, int who, const float (*pts)[3], int n, float speed, uint32_t flags);
/* Band.bsf (an ObjectInstance, MarchingBand1 in New York): 0.25 s after it starts its OTRef|BandMember1 .. n march
 * in at 4 from x = 50 + their file offset to dest (its Location) less the offset, then play facing the street; when
 * one is grabbed or hit, the whole band runs off at 15 */
void people_add_band(People *p, Group band, const float dest[3]);
/* RoyalGuardHut.bsf (London's StationRoyalGuardLeft / Right): a guard (OTRef|Guard) comes out after 4 s, then
 * whenever none is out, every 15 s (6 s re-checks); it marches 3 out, 5 along MarchDir, performs, waits 2 s, marches
 * back and in. Returns the hut's id */
int people_add_hut(People *p, Group hut, const float pos[3]);
/* the hut's TakeDamage (a monster hit it): it flies at -25 x the hit's direction in x and y (vel, the viewer's to
 * apply), is deleted at its next pulse, and its guard marches 80 off instead of going back in. addStats(hut, 0) */
PeopleEaten people_hut_hit(People *p, int hut, const float direction[3], float vel[3]);
int people_hut_state(const People *p, int hut);     /* 0 standing, 1 hit (flying), 2 deleted */
/* Monsters::createStreaker, from becomeHuman (the Faceup_Idle_Shrinking clip at 0.763, a defeated monster), only
 * while gWorld.getState() == 6 (a block in play). tmpl: NakedFemale if the monster isFemale, else NakedMale;
 * camera_x the camera's x. Out at 2.241 to y = -3 -+ 1, then scurrying off along it (Person_Runner.bsf), deleted
 * once out of view. The monster's becomeHuman (0x2D3ED0) keeps the actor (+0xAAC) and stops drawing itself. */
int people_add_streaker(People *p, Group tmpl, const float monster_pos[3], float camera_x);
void people_remove(People *p, int who);              /* requestDelete: gone at once (the respawn's streaker) */

void people_update(People *p, float dt, const PeopleMonster *m, int nm);
int people_count(const People *p);
const Person *people_get(const People *p, int who);
int people_events(People *p, PeopleEvent *out, int max);   /* takes the events since the last call */

/* a front cell at (building, storey) of a building broke (state 2 or more; 0x317DC0): the window people of that
 * building within a storey of it fall (onFallFromWindow) */
void people_cell_broken(People *p, int building, int storey);

/* ---- the monster's grab */
typedef struct { float min[3], max[3]; int width; int gone; } PeopleBox;   /* a building's box in the world */
/* getBuildingOccupant's cell (0x2B37C0, 0x2A3D20, 0x2B3900): the script's (x, y, z) (x negated when the monster
 * plays mirrored) added to the monster's place, y less 1; the nearest building whose front lies at or ahead of that
 * point (+y) over its x; its column (x - min x) / 5 and storey (z - foot - 6) / 3. 0 = none */
int people_probe(const PeopleBox *b, int nb, const float monster_pos[3], const float xyz[3], int mirrored,
                 int *building, int *storey, int *column);
int people_window_at(const People *p, int building, int storey, int column);   /* the occupant there, -1 none */
/* a window whose occupant was grabbed from it: grabBuildingOccupant's 0x318140(mgr, actor, 1) retires its slot (+0xD
 * = 0xFF), which the spawner (0x318650) never fills again */
int people_window_retired(const People *p, int building, int storey, int column);
/* canGrabPerson(-, x, y, z): the window person the probe finds, -1 none */
int people_can_grab(const People *p, const PeopleBox *b, int nb, const float monster_pos[3], const float xyz[3],
                    int mirrored);
/* doGrabObject's search (0x2D99C0): within radius (MONSTER_DATA +0x1B8, 15 for George, x the monster's scale; + 10
 * when the last search had found one) of the hand, a pickupable person not held nor dying, in front of the
 * monster's root (forward = its +y axis), the nearest by its place; -1 none */
int people_nearest(const People *p, const float hand[3], const float root[3], const float forward[3], float radius);
/* hold it (0x2DA310): BeingHeld, onPickup; blend 0.25 from a window (grabBuildingOccupant), 0.1 from the ground */
void people_grab(People *p, int who, float blend);
/* each frame it is held: the hand's (LiftNodeName, "Helper R Hand") world matrix, column-major */
void people_hold(People *p, int who, const float hand_world[16], int mirrored, float dt);
/* throwObject(1, ...): onThrow(monster, eaten); gone. What the monster gets (gCommon.addStats, 0x2C51A0) */
PeopleEaten people_eat(People *p, int who);
/* addStats on the monster: hit points to max(1, hp + health) within hp_max (MONSTER_DATA +0x138, 200), only while
 * hp > 0; energy within 0 .. 100 (100 starts its rage) */
void people_apply_eaten(const PeopleEaten *e, float *hp, float hp_max, float *energy);
/* throwObject(0, ...): onThrow(monster, 0) -> smash -> smashObject (0x2B2F50); the clip's velocity is not used for
 * an actor (0x2D9DC0 gives it only to objects without flag 0x80000). monster_heading in degrees */
void people_throw(People *p, int who, float monster_heading);
/* an attack sphere of monster slot `monster` reaching people (TakeDamage): how many it sent flying */
int people_hit(People *p, int monster, const float centre[3], float radius, float monster_heading,
               const char *monster_move);

#endif
