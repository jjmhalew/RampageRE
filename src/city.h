/* city.h - a city block put together from the property books (city.c, the port of tools/city.py; docs/FORMAT_BDP.md
 * "City layout", "Spawns", "Lighting"), with what moves in it: the props' own loops, the people's clips, traffic and a
 * monster. Game space: Z up, the street towards -Y. Matrices are column-major 4 x 4 (OpenGL order). */
#ifndef RAMPAGE_CITY_H
#define RAMPAGE_CITY_H
#include "data.h"
#include "building.h"
#include "people.h"
#include "fx.h"
#include "cloth.h"
#include "army.h"
#include "hud.h"
#include "vehicle.h"
#include "water.h"
#include "edible.h"
#include "boss.h"
#include "parade.h"
#include "shadow.h"

typedef struct { float m[16]; } Mat4;

typedef struct { Mesh mesh; const Bundle *b; } SceneMesh;   /* a mesh entry of bundle b, or a generated facade */

/* the front end's 3D layers (shell.h shell_3d): the k-th is drawn where the k-th HudQuad with tex HUD_TEX_3D stands,
 * through the GUI camera; meshes depth tested against the layers before them (the 2D quads write no depth) */
typedef struct { const SceneMesh *mesh; Mat4 world; } SceneGuiMesh;
typedef struct {
    const SceneGuiMesh *meshes; int nmeshes;
    const FxQuad *fx; int nfx;
    int lit; float ambient[3]; int nlights;           /* the light set: each light a direction it travels */
    float light_dir[3][3], light_diffuse[3][3], light_ambient[3][3];
} SceneGui3D;

typedef struct {                                     /* a skinned mesh's vertices, posed every frame */
    uint8_t (*bones)[4]; float (*weights)[4];         /* per vertex of all parts, in part order */
    float *pos, *nrm;                                 /* the posed vertices */
    uint32_t nverts;
    /* a cloth mesh (hair, fur, whiskers): its vertices follow the simulation (cloth.c), each on its particle */
    Cloth *cloth; int32_t *particle; int cloth_started;
    ClothWind wind; int has_wind;                     /* a city flag's wind (its object's ClothBit group) */
    /* a building's flag (BuildingFlag): a force for its next step besides the wind (its building's shake x 500,
     * 0x346F88 .. 0x346FCC) and the plane n.x = d of its wall, which its particles are kept out of after each step
     * (0x1D6630 from 0x347534) */
    float push[3]; int has_plane; float plane[4];
} Skin;

typedef struct {
    Tree tree;                                        /* n = 0: a model without a node tree */
    Mat4 base;                                        /* where it stands */
    Clip clip; int has_clip; float offset;            /* a looping clip, started offset seconds in */
    int vehicle;                                      /* -1, or the vehicle it rides on */
    Mat4 *node;                                       /* per node, relative to base */
    /* root motion the monster takes over (0x301080): the root's axes in still_root (bit 0 x, 1 y, 2 z) are held
     * at root_hold, and its turn about Z is taken out by root_turn degrees */
    int still_root; float root_hold[3], root_turn;
    float (*pose)[7];                                 /* the monster: each node's last local pose (t, q) */
    /* the monster's face layer (0x2E9350, 0x2FFF10): a clip on its own 0 .. 1 clock whose bones replace the body
     * clip's in face_mask (the nodes under Bip01 Head, the head itself too for a head layer) */
    Clip face; int has_face; double face_t; uint8_t *face_mask;
    float (*face_held)[7]; float face_blend_left, face_blend_time;   /* the face bones' own cross-fade (AP +0x7298) */
    float (*held)[7]; float blend_left, blend_time;   /* the pose blended from while blend_left > 0 (0x2FFF10) */
    /* an army unit's weapon clusters (WWeaponProcAnimator 0x298980): node turn[k].node's own pose is turned first,
     * about its own z (axis 2, the RotationControlNode) or x (axis 0, the ElevationControlNode) by angle radians */
    struct { int node, axis; float angle; } turn[8]; int nturn;
    int hidden;                                       /* not drawn (a building's interior until it is damaged) */
    int dynamic;                                      /* its base moves (a building falling): refreshed every frame */
    int mirrored;                                     /* skinned mirror-wise: left and right swapped, x reflected */
    Group shadow;                                     /* its template's ShadowData (shadow.c), at NULL: casts none */
    int own_ambient; float ambient[3];                /* drawn with this ambient, not the scene's (an adornment's: the
                                                       * building light's ramp at its height, 0x2CD060) */
} Instance;

typedef struct {
    uint32_t mesh;                                    /* index into Scene.meshes */
    int instance, node;                               /* node -1: the instance's root */
    Mat4 world;                                       /* updated by scene_update for moving instances */
    Skin *skin;                                       /* skinned meshes: draw skin->pos / nrm instead of the mesh's */
} DrawItem;


/* an adornment on a building (record 0x60 bytes at building +0x250): a fire escape, an AC unit, a roof's trim */
typedef struct {
    int inst;                                         /* its instance */
    int side, record;                                 /* +0x40: 0 left, 1 right, 2 front, 4 ground, 5 roof; +0x38 storey */
    int cell, flags, detach;                          /* +0x48; AdornmentFlags (+0x3C); CellDetachDamage (+0x50) */
    float x, y, deg, zoff;                            /* in the building's frame; +0x34 */
    float lo[3], hi[3];                               /* its model's box, in its own frame */
    const char *fx;                                   /* DislodgedFx (+0x54): played when it is knocked off */
    int gone;                                         /* flag 0x10: off the building */
} Adornment;

/* a building's flag: a ClothInstance| of its BuildingInstance, a ClothObject (0x2BD200 -> 0x347580) hung where its
 * BuildingAttachment says, as an adornment is (0x30FAA0, 0x30F710), on the building's list for the punches (0x2CFA20) */
typedef struct {
    int inst; Skin *skin;                             /* its instance and the cloth drawn there */
    Adornment at;                                     /* side, storey (record), x, y, deg, zoff */
    float wall;                                       /* WallOffset (+0x14C): out from the face */
    Mat4 off;                                         /* ClothOffset (+0x118): the model in the attachment's frame */
    int gone;                                         /* deleted, sunk below the ground (0x3473B0) */
} BuildingFlag;

typedef struct {                                      /* a building's cells and its facade quads */
    Building b;
    int mesh;                                         /* the facade's scene mesh, -1 none */
    int32_t quad[BUILDING_STOREYS][BUILDING_CELLS];   /* per cell (building.h order) its quad's first vertex, -1 */
    /* per storey its interior models (instances, -1 none): Skeleton once a cell is damaged, DamagedSkeleton once
     * FloorHP is down to DamagedSkeletonHP (half of FloorHP unless set; drawn around 0x2CD898) */
    int skeleton[BUILDING_STOREYS], damaged_skeleton[BUILDING_STOREYS], skeleton_hp[BUILDING_STOREYS];
    int ground, facade, top, rubble;                  /* its instances (-1 none) */
    /* the TopFloorShapes models by the roof's damage state (G+0x65C + 4 x state, 0x28D890; drawn 0x2CD5E0: the state's
     * or the nearest lower one there is); roof_inst[0] is top. The cities list two: intact, damaged */
    int roof_inst[4];
    int first, last; Mat4 *rest;                      /* all its instances, first .. last - 1, where they were built */
    Mat4 node;                                        /* its place */
    int moving;                                       /* it moved last frame */
    /* the facade's other passes (FORMAT_BDP.md "Facade geometry"), parts 1 .. 3 of its mesh: the window glass, the
     * cracks, the inner corner walls. Per sheet column its window outline in cell units (x0, z0, x1, z1; x0 < 0
     * none), the sheet's InnerWallColumn, 1 / CrackScale and CrackTint */
    float outline[4][4]; int inner_wall; float crack_k, crack_tint[3];
    /* per sheet column its Openable| windows by LRPos (0x2D0FB0: ColumnDef +0x14 there, +0x16 OpensH, +0x18 the rect,
     * x0, z0, x1, z1 in cell units as the outline; x0 < 0 none): the occupants' windows (building_window_*) */
    float openable[4][2][4]; uint8_t opens_h[4][2];
    int open_part;                                    /* its facade mesh's part of open windows (part 4), 0 none */
    Adornment *adorn; int nadorn;
    BuildingFlag *flag; int nflag;
    uint8_t broke[BUILDING_STOREYS][BUILDING_CELLS];  /* a front cell's state (2 or more) its slots were told of */
    int fronts_told;                                 /* its first update's 0x2BE300 made (a back-row building's) */
    int seen_falls, seen_state;
    Group props;                                      /* its template (DefBuildingProps' Scoring, ...) */
    int hitter;                                       /* the monster slot that hit it last (its Scoring's points) */
    /* AlarmBuilding.bsf's (its template's ScriptName; alarm_sound NULL for any other): AlarmSound, AlarmDuration,
     * PlayOnlyOnce; when it last sounded (the viewer's play time; alarm_played 0 never: prevTimeStamp 0, flags & 2)
     * and that 3D sound (main.c building_alarm) */
    const char *alarm_sound; float alarm_duration, alarm_at; int alarm_once, alarm_played, alarm_h;
    /* SchoolBuilding.bsf's (school 1): its Swarm (getStringProp: the BuildingInstance's OTRef|Swarm, else the
     * template's; NULL none) and when its fStateTime of 20 s runs out (the viewer's play time); ChickenRestaurant.bsf's
     * (chicken 1): bChickenRunnersCreated (main.c building_script) */
    int school, chicken, chickens_made; const char *swarm; float swarm_next;
    /* its effects (DefBuildingProps: Glass_Small / _Large, Wall_Small / _Large, Floor_Small / _Large) */
    const char *fx_glass, *fx_wall_small, *fx_wall_large, *fx_floor;
    /* its effect spawner's records (+0x8F4, 0x58 bytes each) that the port follows: the front cell's debris started
     * with flags 1 or 2 (the cells' hits): the effect, the cell's key (template storey o, column; +0x4), the storey it
     * sits on (+0x44, 0x30F690), its place (+0x34) and the flags (+0x0) [R 0x310260, 0x310660] */
    struct { int id; uint8_t o, col, storey, flags; float at[3]; } spawn[32]; int nspawn;
    Group inst;                                       /* its BuildingInstance| (EdibleInstance| rows, ...) */
    /* its light (docs/BUILDING_LIGHT_NOTES.md): per live vertex row, face and column the grid vertex's colour
     * (building_light_grid, 0x2CE300), which the facade quads (times their storey's FloorTint) and the cracks (times
     * CrackTint) are drawn with; per storey (template order) FloorTintR/G/B (the cells' tint, 0x2CBC78) */
    BuildingLightRow *light; float floor_tint[BUILDING_STOREYS][3];
    /* the neighbours' shadows on its front and right face (+0x914, 0x31C8C0): the shape (0x31D6B0), the two
     * neighbours (buildings of the block, -1 none) and their tops when it was made (+0xCC, +0xD4), the faces still
     * shaded (G+0x1EC) */
    ShadowFacade shade; int shade_nb[2]; float shade_top[2]; uint8_t shade_faces[3];
    /* its ground-floor and top models' draw items, given meshes of their own whose vertex colours are the building
     * light and that have no normals (0x31CCA0: lit per vertex, less the shadows), and the shared meshes they were
     * copied from (the normals the light is made from) */
    int lit_item[16], lit_src[16], nlit_items;
    /* its place in the work queue (0x4D7870) for a relight: the frame it was queued + 1 (0 not queued), and a change
     * of its own still waiting to be served (+0x154, kept until a grant) */
    int light_queued, light_pending;
    /* the ground floor's crack overlay (G+0x674, city.c ground_cracks_make / ground_cracks): the ground model's draw
     * item whose (lit) mesh carries it as its part gf_part (-1 none), its triangles while shown (+0x61), per vertex
     * the bottom-row grid vertex it follows, its height factor and its damage (+0x58: 3 bytes each), and its own place
     * in the work queue (+0x16C) */
    int gf_item, gf_part, gf_ntris, gf_queued; uint8_t (*gf_vert)[3];
} SceneBuilding;

struct IkFeet;
struct TailAnim;
/* a monster in the scene: the game has up to four (the players and the bots), each with its own skeleton, clips,
 * head turn, arm IK and foot IK; Scene.mon[k] is monster slot k */
#define SCENE_MONSTERS 4
typedef struct {
    int inst;                                         /* its instance, -1 = none */
    const char *clip;                                 /* the name of the clip it plays */
    const Bundle *anims;                              /* <Monster>_anims.cmp */
    const Bundle *bundle;                             /* <Monster>.cmp: its meshes, skeleton and MONSTER_DATA */
    const char *move;                                 /* its move state's name now (the buildings' Scoring) */
    /* its head turn (MonsterHeadAnimator, 0x2FBF90): the neck yaws about the model's up, the head pitches about its
     * own z; its bones, -1 none */
    float look_yaw, look_pitch; int look_neck, look_head;
    /* its arm IK (ik.c), per hand (0 left, 1 right, as drawn): its share, where the hand goes (model space, as drawn:
     * rotation 3 x 3 column-major), the chain (upper arm, forearm, hand; -1 none), and the hand as the clips posed it
     * before the IK (model space, as drawn) */
    float ik_w[2], ik_pos[2][3], ik_rot[2][9];
    int ik_chain[2][3], ik_ready;
    Mat4 ik_anim[2];
    float ik_shoulder[2][3], ik_reach[2];             /* the upper arm's place (model space, as drawn), the arm's length */
    struct IkFeet *feet;                              /* its foot IK (ik.c ik_feet), NULL none */
    struct TailAnim *tail;                            /* its tail's springs (look.c tail_pose), NULL none */
    int sway_building;                                /* the building whose sway moves it as drawn (0x2D6680), -1 */
} SceneMonster;

typedef struct {
    const Bundle *city;
    Bundle *own[2 * SCENE_MONSTERS]; int nown;        /* bundles loaded for this scene (the monsters and their clips) */
    SceneMesh *meshes; uint32_t nmeshes, meshes_cap;
    int32_t *mesh_of_entry;                           /* per city bundle entry: its SceneMesh, -1 = not loaded */
    Instance *instances; uint32_t ninstances, instances_cap;
    DrawItem *items; uint32_t nitems, items_cap;
    /* the traffic, the fire truck and the rockets (vehicle.c, the viewer's): the TrafficDef blocks of the three block
     * slots (0 this one, 1 and 2 behind it) and the roof rockets the buildings carry, for it to make */
    Vehicles *traffic; Group traffic_def[3];
    struct Rail *rail;                               /* the trains, cable cars and freighters (rail.c), drawn here */
    Water *water[16]; int nwater; float water_acc, flag_acc;
    Edibles *edibles;                                 /* the power-ups and hazards behind the windows (edible.c) */
    Parade *parade;                                   /* the balloons, bikers and food carts (parade.c), driven by main */
    Boss *boss;                                       /* the block's boss and the ambient ones (boss.c) */   /* the block's WaterObject|s (water.c), stepped at 1/60 */
    struct { const char *name; float pos[3], facing; int building; } rockets[16]; int nrockets;
    const char *weather[4]; int nweather;            /* the block's Weather.bsf objects' effects (their Effect|Effect:
                                                      * Rain, Weather_Fog, Weather_wind, ...), main's to play */
    int placing_building;                             /* the playable building whose objects are placed, -1 none */
    float ambient[3]; float light_colour[4][3], light_dir[4][3]; int nlights;
    /* the buildings' light (building_light_*: the light file's ambient and lights, LightingData's Ramp*) and the
     * facade darkening's MShadowR/G/B (shadow_facade_*) */
    BuildingLight building_light; ShadowLighting shade_light;
    float fog_colour[3], fog_start, fog_end;
    float cube_tint[3];                               /* LightingData's CubeTint (the window glass), 0 .. 255 */
    /* LightingData's CubeMap|EnvMap (0x2B92F0): a texture of the city bundle (its name index, 0 none), the sphere map
     * the world's BUILDING_CUBEMAP and RCUBEMAP* materials take (render_gl.c draw_part) */
    uint32_t env_map;
    int sky;                                          /* the SkyBox's instance, -1 none: drawn around the camera */
    int ground_inst;                                  /* the block's GroundShape, -1 none: the shadow grid goes on it */
    const ShadowList *shadows; const Bundle *shadow_bundle;   /* this frame's shadows (shadow.c), their textures */
    float cloth_dt;                                   /* play time not yet simulated by the cloths */
    float cloth_time;                                 /* the flags' wind clock */
    const char *music;                                /* the block's Music (a .mic stream, sound.c) */
    Group block;                                      /* the block's BlockDef (its AIDef, ...) */
    /* the army (army.c, driven by main): its units' looks are posed with the people's; the buildings' damage scores
     * (for the monster that hit the building last, SceneBuilding.hitter) */
    Army *army;
    /* the people (people.c): each one's looks (ActorAnimShape|<anim>: a model and its clip), made as it needs
     * them; the block's window occupants (Occupants: OccupantInstance| templates, FillRate) */
    People *folk; struct City *owner;
    /* effects to start (fx.c, by main) and the particles to draw this frame; a building cell's through its spawner
     * (building >= 0: its storey o, column and the spawner's flags, scene_fx_started) */
    struct SceneFx { const char *name; float pos[3], dir[3]; int building, o, col, flags; } fxreq[64]; int nfxreq;
    const FxQuad *fx; int nfx;
    const FxMesh *fxm; int nfxm;                     /* the effects' mesh debris (XForm particles) */
    const SceneMesh (*fx_shapes)[8]; const int *fx_nshapes;   /* their meshes, by fx_shape_* */
    const HudQuad *hud; int nhud;                     /* the HUD and the floating scores, over everything (hud.c) */
    int hud_clip;                                    /* the HUD cut at its 640 x 480 screen, as the PS2's frame cuts
                                                      * it (the front end, black around its 4:3) */
    const SceneGui3D *gui3d; int ngui3d;              /* the HUD's 3D layers (the front end's globe and effects) */
    struct PersonLook *looks; int nlooks;
    Group occupant[16]; float occupant_freq[16]; int noccupants; float fill_rate, spawn_timer, spawn_rate;
    /* a slot: its front cell (template storey, column), the Openable it opens (LRPos, +0xC), its occupant's kind */
    struct WindowSlot { int building, storey, column, occupant, military, unit, lr, retired; float win_x, win_floor; }
        *slots;                                      /* retired: its soldier grabbed (0x318140 with 1: +0xD = 0xFF) or
     * its floor fallen (0x317A70), never filled again; win_x, win_floor: its soldier's window x and its storey's floor
     * when last placed (the soldier follows it as people_window_move's people do) */
    int nslots, slots_made;
    /* the game's one list of slots (+0xC, 0x3195E0 appends) in its order: per building its soldiers' slots, its
     * civilians', then the edibles' own (here -1: the edibles keep theirs in edible.c); order_mil (+0x40) and
     * order_civ (+0x3C) the soldiers' and civilians' counts, the spawn's two ranges of it (scene_occupant_pad) */
    int *order, norder, order_mil, order_civ;
    /* the Occupants group's soldiers (MilitaryInstance|: Military_Window.bsf templates, the army's window soldiers)
     * and its rates (0x31A540: +0x48 MilitaryStartRate 0.1, +0x4C MilitaryEndRate 0.9, +0x50 MilitaryTimeRange 240) */
    const char *military[16]; float military_freq[16]; int nmilitary;
    float nonmilitary_rate, military_start, military_end, military_range;
    /* the host's each frame before scene_people_spawn: the clock's time left (0x347DC0) and the x range of the
     * buildings that may take an occupant (0x318390: the players' monsters' x, at least 30 wide, and 10 on each side);
     * occ_setup: the block has an Occupants and an Edibles group (0x31A540 makes no slots without both) */
    float spawn_left, view_lo, view_hi; int occ_setup;
    /* the objects whose template Flags have IsObstacle (object flag 0x40000: the electric side signs): their boxes
     * in the world (min, max: AutoCollisionBox, the model's bounds), for the climbing's -2 (0x2801A0, mask 0x40040) */
    int *obstacle_of; float (*obstacles)[6]; uint32_t nobstacles;
    /* the block's scripted objects for people.c: Band.bsf (a band marching in) and RoyalGuardHut.bsf (a guard's hut) */
    /* a hut: its object in the frame's collision world (cobj, -1 none; hut_cw that world), and sent flying its
     * SpherePhysicsMover (cm, its matrix cm_m, made at the first flying frame: cm_on) */
    struct { Group t; float pos[3]; int instance, hut, hut_id; float vel[3]; CollideMover cm; float cm_m[12];
             int cm_on, cobj; } scripted[8]; int nscripted;
    CollideWorld *hut_cw;
    /* the street objects (StreetObject.bsf, RoofItem.bsf, Sign.bsf, the billboards, ...) of the block and its
     * buildings as placed, for streetobj.c (main adds them): the template, its ObjectInstance| group, its instances
     * first .. last - 1 (the model, then its flags' cloths), where it stands, the building whose ObjectInstance| it
     * is (-1 the block's own); model: the instance of its model now (scene_street_shape), rel: its cloths' places
     * relative to it (scene_street_place); attack: an AttackObjectInstance| of the block (an AttackObject,
     * 0x3050F0); parent: for a billboard's attachment (made where its onInit makes it), the billboard's index here,
     * else -1 */
#define SCENE_STREET_MAX 256
    /* node: its model's nodes 1 .. nnode in its own frame (the crane's, streetobj_nodes) */
    struct SceneStreetObj { Group t, inst; int first, last, building, model, nrel, attack, parent, nnode; float world[16];
                            Mat4 rel[4]; float node[2][3]; } street[SCENE_STREET_MAX];
    int nstreet;
    /* the block's AttackObjectInstance|s that army.c makes units of (the PirateShip, StationaryAttackObject.bsf,
     * army_load_block): their model as placed (instances first .. last - 1: the model, then its flags' cloths), shown
     * until the unit whose instance group is inst is drawn; then the cloths ride on the unit (rel: their places
     * relative to the model) and go with it (seen: it has been drawn) */
    struct { Group inst; int first, last, seen; Mat4 rel[4]; } army_obj[4]; int narmy_obj;
    /* the rail objects' flags (their template's ClothBit|: the freighter's flagpole_red), made when the object is
     * first drawn: instances first .. last - 1 riding on the rail object of that uid at rel (its ClothOffset) */
    struct { uint32_t uid; int first, last, seen; Mat4 rel[4]; } rail_flag[16]; int nrail_flag;
    int left;                                        /* the game has moved on (scene_block_left): the block is behind */
    int placing_block;                                /* placing the block's own objects (not those behind) */
    int placing_attack;                               /* ... its AttackObjectInstance| list */
    uint32_t cloud_material; float cloud_rate[2];    /* the material whose texture drifts (AnimatingSkyData) */
    int people, missing;
    SceneMonster mon[SCENE_MONSTERS]; int nmon;      /* the monsters, slot 0 .. nmon - 1 (city_monster adds one) */
    float spheres[256][4]; int nspheres;              /* drawn over the scene as wire spheres: centre, radius */
    SceneBuilding *buildings; uint32_t nbuildings;
    int light_frame, light_granted;                  /* the work queue's frame count (+0x10), a grant this frame */
    /* the next two blocks of the city's BlockList, drawn behind this one at y + 41 and y + 82 (the block manager
     * 0x4D4E80's slots 1 and 2, 0x302AF0): their buildings are scenery, out of the play */
    SceneBuilding *scenery; uint32_t nscenery;
} Scene;

typedef struct City City;
City *city_open(const Vol *vol, const char *name);   /* NULL if the city's bundles are not in the archive */
void city_close(City *c);
uint32_t city_blocks(City *c, const char **names, uint32_t max);   /* the block list, without repeats */
int city_build(City *c, const char *block, int night, Scene *s);    /* 0 = ok */
/* the world starts (its scripts' onInit): the next city_build with occupants makes Traffic's two crowds and their
 * Crowd_Panic; a city_build without this before it (the game's next block) makes none */
void city_new_world(City *c);
/* the block left behind, after city_build of the next (0x303BB0, 0x303C90): its GroundShape at (0, -41, 0) and the
 * RubbleShape of each of its buildings with -50 <= LeftEdgeXPos < 50 where the building stood, as models only */
void city_build_left(City *c, const char *block, Scene *s);
/* a monster in the street at (x, y), turned deg about Z, playing its clip number `clip` (-1: its idle), in the next
 * slot (Scene.mon[Scene.nmon]); its slot, -1 = not made. The scene_monster_* functions take the slot k */
int city_monster(City *c, Scene *s, const char *monster, int clip, float x, float y, float deg);
void scene_update(Scene *s, double t);               /* pose everything that moves at time t (seconds) */
/* the monster plays the clip whose name ends in suffix ("_WALK2", "_IDLE"); 0 = ok. *speed: how fast its root
 * travels in that clip (units per second), for moving it by hand */
int scene_monster_clip(Scene *s, int k, const char *suffix, float *speed);
void scene_monster_place(Scene *s, int k, float x, float y, float z, float deg);
/* the building (scene index, -1 none) the monster climbs or stands on: while it shakes, the monster and its hair are
 * drawn moved by its sway (0x2D6680), the monster's own matrix (and the places of its bones) staying where they are */
void scene_monster_sway(Scene *s, int k, int building);
/* the root's axes held (bit 0 x, 1 y, 2 z) at hold, and turn degrees about Z taken out of it: the motion the
 * monster itself makes */
void scene_monster_root_motion(Scene *s, int k, int axes, const float hold[3], float turn);
int scene_monster_play(Scene *s, int k, const char *clip);
int scene_monster_slot(const Scene *s, int inst);  /* the monster slot whose instance inst is, -1 none */
/* the face layer plays clip (head: Bip01 Head itself too), at face_t 0 .. 1 (Instance.face_t); 0 = ok */
int scene_monster_face(Scene *s, int k, const char *clip, int head);
/* the face bones blend over seconds from where they are now (a face clip starting, 0x2FF910) */
void scene_monster_face_blend(Scene *s, int k, float seconds);
/* the monster's hand (0 left, 1 right, as drawn) in the world, as the clips posed it (before the IK); 0 = ok */
int scene_monster_hand(const Scene *s, int k, int hand, Mat4 *world);
/* where that arm's shoulder (upper arm) is in the world, as posed, and how far the arm reaches; 0 = ok */
int scene_monster_shoulder(const Scene *s, int k, int hand, float at[3], float *reach);
/* 0x2D4390: a monster's attack sphere knocks off the adornments it reaches (0.75 of its radius; not the structural
 * ones, flag 4, nor obstacles, flag 2); how many */
int scene_adornments_hit(Scene *s, const float centre[3], float radius);
/* a monster's punch reaching building `building`'s face at `at` (hitBuildingPt): its flags pushed off it */
void scene_building_flags_punch(Scene *s, int building, const float at[3]);
/* street object k (Scene.street[k], streetobj.c's object) drawn at world (its model's base: StreetObj.world), or
 * hidden: its model and its flags' cloths move together (the cloths kept where they were relative to the model) */
void scene_street_place(Scene *s, int k, const float world[16], int hidden);
/* activateNextBlock's teardown of this block (0x2B66C0, 0x302800): its playable buildings are left as their rubble
 * shapes (a standing one too) and no longer move, its street objects (this block's, y below 20.5) are gone */
void scene_block_left(Scene *s);
/* street object k's model is now `shape` (streetobj.c's SE_SHAPE: setShape(DamageShape)), placed where the old one
 * is, which is no longer drawn; 0 = ok, -1 no such model (the old one stays) */
int scene_street_shape(Scene *s, int k, const char *shape);
/* instance inst's model node `name` (or, name NULL, node number id) in the world now (base x node); 0 = ok */
int scene_instance_node(const Scene *s, int inst, const char *name, int id, float out[16]);
/* RoyalGuardHut's TakeDamage: a monster's sphere (centre, radius) from the monster at `from` sends the huts it reaches
 * flying (people_hut_hit): an armed attack sphere, or any of its spheres touching it (TakeDamage 2.0, Collision, every
 * frame: the hut has no OnlyHitWithEffNodes, 0x2E22B0); what the monster gets, summed */
PeopleEaten scene_huts_hit(Scene *s, const float centre[3], float radius, const float from[3]);
void scene_huts_update(Scene *s, float dt);         /* the flying huts move; a deleted one is no longer drawn */
/* the huts as world objects (WSimpleObject: the guardshack's sphere, 0x28FD70) in the frame's collision world, between
 * collide_begin and collide_finish: what a flying hut meets, and what meets it */
void scene_huts_collide_add(Scene *s, CollideWorld *w);
Group city_template(City *c, const char *name);     /* an object template by name (the city's template books) */
/* one play of a look's clip (its AnimName; NULL: the shape's own RawAnim) in seconds; 0 = unknown */
float city_clip_seconds(City *c, const char *anim, const char *shape);
void scene_fx(Scene *s, const char *name, const float pos[3], const float dir[3]);   /* an effect to start */
/* the effect of request f started as id (main.c, fx_spawn's answer): a building cell's goes on its spawner's records */
void scene_fx_started(Scene *s, FxWorld *w, const struct SceneFx *f, int id);
void scene_fx_follow(Scene *s, FxWorld *w);          /* the spawners' update: their effects on their storeys */
/* a node of the monster's skeleton by name, in the world as drawn (mirrored: the other side's, reflected); 0 = ok */
int scene_monster_node(const Scene *s, int k, const char *name, Mat4 *world);
/* the window occupants (0x318650): every SpawnRate seconds (0.75) one comes to a random free slot of a building in
 * view; the slots are the block's windows, a FillRate share of them, each with an occupant picked by Frequency */
/* building bi's window slots (0x3196F0's first half): the host makes them building by building just before the
 * building's edibles (edible_add_building), as the game's draws come; any left are made at the first spawn */
void scene_occupant_slots(Scene *s, int bi);
/* n slots of the building's edibles made after its occupants' (where no occupant's slot was in the cell: 0x3196F0 has
 * an edible take an occupant's slot of its cell, else a new one), in the game's list as edible slots */
void scene_occupant_pad(Scene *s, int n);
void scene_people_spawn(Scene *s, float dt);
/* closeWindow (Actor binding, 0x30C3F0): the window an occupant came out of shuts over 0.5 s (openWindow(.., 0,
 * 0.5)), the slot's Openable; building and storey as people_add_window got them (the live storey then), or, _at, the
 * cell at a point (the occupant's place: the building whose front it is on, column (x - min x) / 5, storey (z - foot
 * - 6) / 3). Call it when Person_Window's or Military_Window's WindowReEnter ends, not when the occupant falls out
 * (leaveBuilding), is grabbed or is smashed: those leave the window open, as the game does. 1 if one was shut. */
int scene_window_close(Scene *s, int building, int storey, int column);
int scene_window_close_at(Scene *s, const float pos[3]);
/* getBuildingOccupant's actor when it is a soldier (0x2B37C0: the slot's occupant, Military_Window.bsf's as well as
 * the people's): the army unit in that window of the building (storey as built), -1 none; _take: grabBuildingOccupant
 * (0x2B3350 -> 0x318140(.., 1)) takes it out, the slot retired */
int scene_window_soldier(const Scene *s, int building, int storey, int column);
void scene_window_soldier_take(Scene *s, int unit);
/* the playable buildings as people.c sees them */
int scene_people_boxes(const Scene *s, PeopleBox *out, int max);
/* the buildings' next dt seconds (collapses, destruction, shaking: building_update), and their facades, interiors,
 * storeys and rubble drawn as they now are, lit again (the building light, its neighbours' shadows) when they moved,
 * changed or a neighbour's top did */
void scene_buildings_update(Scene *s, float dt);
/* the next clip blends in over `seconds` from the pose the monster has now, as the game's animation player does
 * (0x2FFA00): call before scene_monster_play; scene_monster_blend_step counts it down */
void scene_monster_blend(Scene *s, int k, float seconds);
void scene_monster_blend_step(Scene *s, int k, float dt);   /* the clip of that name (any case, as MONSTER_DATA names it) */
void scene_cloth_step(Scene *s, float dt);          /* play time for the cloths, simulated at the next posing */
/* the root node's translation and rotation in the monster's clip at `seconds`; 0 = ok */
int scene_monster_root(const Scene *s, int k, double seconds, float t[3], float q[4]);
/* a bake (0x300FE0): the root of the pose kept for the blend (and the last one) set to this (the new clip's at its start) */
void scene_monster_hold_root(Scene *s, int k, const float t[3], const float q[4]);
/* the side it is drawn on (mirrored or not); a blend under way keeps the pose it had as it was drawn */
void scene_monster_mirror(Scene *s, int k, int mirrored);
int city_camera(City *c, Group *two_tilts);
/* a group of a property book by the book's name ("Objects1_Props", "Weapons": <name>.bdp in the Vol) */
Group city_book_group(City *c, const char *book, const char *name);
/* a block's index in the city's BlockList as the game counts it (the first at or after WorldData BlockStart), -1 */
int city_block_index(City *c, const char *name);
/* the block a game with that start block (GameStatus +0x210, a BlockList index) begins with: the list's entry at
 * max(start block, BlockStart) (0x2B74D0); NULL past the list. The BlockList repeats its first block (BlockStart 1),
 * which city_blocks' names keep once */
const char *city_game_block(City *c, int start_block);
/* a model's bounds in its own frame (its meshes' vertices); 0 = ok */
int city_shape_box(City *c, const char *shape, float min[3], float max[3]);
/* the sphere the model keeps (the union of its meshes' header spheres); 0 = ok */
int city_shape_sphere(City *c, const char *shape, float centre[3], float *radius);
/* an army unit's node (a weapon's WeaponNode) in the world as its look is drawn now; 0 = ok */
int scene_unit_node(const Scene *s, int unit, const char *node, float out[16]);           /* camera.bdp's TwoTilts group; 0 = ok */
/* the same on the look of what unit u plays now, made and posed at once when it has not been drawn yet; 0 = ok */
int scene_unit_node_now(Scene *s, int unit, const ArmyUnit *u, const char *node, float out[16]);
/* the same for boss part `part` (boss.c) and vehicle object `obj` (vehicle.c) as drawn; 0 = ok */
int scene_unit_node_at(const Scene *s, int unit, const char *node, int id, float out[16]);   /* node NULL: number id */
int scene_boss_node(const Scene *s, int part, const char *node, float out[16]);
int scene_boss_node_at(const Scene *s, int part, const char *node, int id, float out[16]);   /* node NULL: number id */
int scene_vehicle_node(const Scene *s, int obj, const char *node, float out[16]);
int scene_person_node(const Scene *s, int who, const char *node, float out[16]);   /* person who's look (people.c) */
void scene_free(Scene *s);
uint32_t monster_names(const Vol *vol, const char **names, uint32_t max);   /* the 30 monsters (NAME_anims.cmp) */

/* Mat4 helpers */
Mat4 mat_identity(void);
Mat4 mat_mul(Mat4 a, Mat4 b);
Mat4 mat_trs_z(float x, float y, float z, float deg);
Mat4 mat_tq(const float t[3], const float q[4]);   /* q in the game's row-vector convention */

#endif
