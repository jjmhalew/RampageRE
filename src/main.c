/* main.c - RampageRE's native viewer: draws the city blocks straight from RAMPAGE.VOL (the game's own archive),
 * with the game's layout, lights, traffic, people and a monster (city.c, anim.c, render_gl.c).
 *   rampage [RAMPAGE.VOL] [City] [block number] [--night] [--monster NAME] [--clip N] [--shot FILE.png] [--time T]
 * default build\disc\RAMPAGE.VOL, Chicago, 1. --shot draws one frame (at T seconds) as a PNG and quits.
 * Keys: W A S D move, Q E down and up, Shift faster, right mouse button look, wheel speed; Page Up / Page Down the
 * block, 1-7 the city, N day / night, F fog, M the next monster (none, then the 30), K / L its clips, F12 a screenshot
 * (shot_NNN.png), Esc quit. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "render_gl.h"

static const char *const CITIES[] = { "Chicago", "NewYork", "SanFrancisco", "LosAngeles", "London", "HongKong", "LasVegas" };
#define NCITIES 7

typedef struct {
    Vol vol;
    City *city; int city_index;
    const char *blocks[64]; uint32_t nblocks, block;
    int night, fog;
    const char *monsters[64]; uint32_t nmonsters; int monster, clip;   /* monster -1 = none; clip -1 = its idle */
    Scene scene; int have_scene;
    Renderer *rnd;
} Viewer;

static void build(Viewer *v)
{
    if (v->have_scene)
        scene_free(&v->scene);
    v->have_scene = city_build(v->city, v->blocks[v->block], v->night, &v->scene) == 0;
    if (v->have_scene && v->monster >= 0 &&
        city_monster(v->city, &v->scene, v->monsters[v->monster], v->clip, 0, -18, 180) == 0)
        printf("%s: %s\n", v->monsters[v->monster], v->scene.monster_clip ? v->scene.monster_clip : "-");
    if (v->have_scene)
        rnd_scene(v->rnd, &v->scene);
    printf("%s %s%s: %u models drawn, %d people, %u vehicles, %d not found\n", CITIES[v->city_index],
           v->blocks[v->block], v->night ? " (night)" : "", v->scene.nitems, v->scene.people, v->scene.nvehicles,
           v->scene.missing);
}

static int open_city(Viewer *v, int index, uint32_t block)
{
    City *c = city_open(&v->vol, CITIES[index]);
    if (!c)
        return -1;
    if (v->have_scene) {
        scene_free(&v->scene);
        v->have_scene = 0;
    }
    city_close(v->city);
    v->city = c;
    v->city_index = index;
    v->nblocks = city_blocks(c, v->blocks, 64);
    v->block = block < v->nblocks ? block : 0;
    build(v);
    return 0;
}

int main(int argc, char **argv)
{
    static Viewer v;
    const char *shot = NULL, *monster = NULL, *pos[3] = { "build\\disc\\RAMPAGE.VOL", "Chicago", "1" };
    int npos = 0, night = 0, clip = -1;
    double shot_time = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--monster") == 0 && i + 1 < argc) monster = argv[++i];
        else if (strcmp(argv[i], "--clip") == 0 && i + 1 < argc) clip = atoi(argv[++i]);
        else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) shot_time = atof(argv[++i]);
        else if (strcmp(argv[i], "--night") == 0) night = 1;
        else if (npos < 3) pos[npos++] = argv[i];
    }
    if (vol_open(&v.vol, pos[0]) != 0) {
        char msg[512];
        snprintf(msg, sizeof msg, "Cannot read %s.\nRun build.ps1 first (it extracts the disc), or pass the path to "
                 "RAMPAGE.VOL.", pos[0]);
        plat_message(msg, 1);
        return 1;
    }
    int city = 0;
    for (int i = 0; i < NCITIES; i++)
        if (_stricmp(pos[1], CITIES[i]) == 0)
            city = i;
    uint32_t block = (uint32_t)(atoi(pos[2]) > 0 ? atoi(pos[2]) - 1 : 0);
    Window w;
    if (win_open(&w, "RampageRE", 1280, 720) != 0) {
        plat_message("Cannot open an OpenGL window.", 1);
        return 1;
    }
    v.rnd = rnd_create();
    v.night = night;
    v.nmonsters = monster_names(&v.vol, v.monsters, 64);
    v.monster = -1;
    v.clip = clip;
    for (uint32_t i = 0; monster && i < v.nmonsters; i++)
        if (_stricmp(monster, v.monsters[i]) == 0)
            v.monster = (int)i;
    if (open_city(&v, city, block) != 0) {
        plat_message("The city's bundles are not in RAMPAGE.VOL.", 1);
        return 1;
    }
    Camera cam = { { 0, -110, 45 }, 3.14159265f / 2, -0.3f, 60 };
    if (v.monster >= 0) {                                  /* closer, the monster in front */
        cam.pos[1] = -55; cam.pos[2] = 18; cam.pitch = -0.2f;
    }
    float speed = 30;
    double last = win_time(), title_at = last, start = last;
    int frames = 0;
    while (!w.quit) {
        win_poll(&w);
        double now = win_time();
        float dt = (float)(now - last);
        last = now;
        if (w.pressed[VK_ESCAPE]) w.quit = 1;
        if (w.pressed[VK_NEXT] && v.nblocks) { v.block = (v.block + 1) % v.nblocks; build(&v); }
        if (w.pressed[VK_PRIOR] && v.nblocks) { v.block = (v.block + v.nblocks - 1) % v.nblocks; build(&v); }
        if (w.pressed['N']) { v.night = !v.night; build(&v); }
        if (w.pressed['F']) v.fog = !v.fog;
        if (w.pressed['M']) { v.monster = v.monster + 1 < (int)v.nmonsters ? v.monster + 1 : -1; v.clip = -1; build(&v); }
        if (v.monster >= 0 && (w.pressed['K'] || w.pressed['L'])) {
            v.clip = w.pressed['L'] ? v.clip + 1 : (v.clip > 0 ? v.clip - 1 : 0);
            build(&v);
        }
        for (int i = 0; i < NCITIES; i++)
            if (w.pressed['1' + i] && i != v.city_index)
                open_city(&v, i, 0);
        if (w.wheel) speed *= powf(1.25f, (float)w.wheel);
        cam.yaw -= w.mouse_dx * 0.005f;
        cam.pitch -= w.mouse_dy * 0.005f;
        if (cam.pitch > 1.5f) cam.pitch = 1.5f;
        if (cam.pitch < -1.5f) cam.pitch = -1.5f;
        float step = speed * dt * (w.keys[VK_SHIFT] ? 4.0f : 1.0f);
        float fx = cosf(cam.yaw), fy = sinf(cam.yaw);
        if (w.keys['W']) { cam.pos[0] += fx * step; cam.pos[1] += fy * step; }
        if (w.keys['S']) { cam.pos[0] -= fx * step; cam.pos[1] -= fy * step; }
        if (w.keys['A']) { cam.pos[0] -= fy * step; cam.pos[1] += fx * step; }
        if (w.keys['D']) { cam.pos[0] += fy * step; cam.pos[1] -= fx * step; }
        if (w.keys['E']) cam.pos[2] += step;
        if (w.keys['Q']) cam.pos[2] -= step;
        double t = shot ? shot_time : now - start;
        if (v.have_scene)
            scene_update(&v.scene, t);
        rnd_frame(v.rnd, &w, &cam, t, v.fog);
        if (shot) {
            int ok = shot_png(shot, w.width, w.height) == 0;
            printf(ok ? "wrote %s\n" : "cannot write %s\n", shot);
            break;
        }
        if (w.pressed[VK_F12]) {
            static int n;
            char name[32];
            snprintf(name, sizeof name, "shot_%03d.png", ++n);
            if (shot_png(name, w.width, w.height) == 0)
                printf("wrote %s\n", name);
        }
        win_swap(&w);
        frames++;
        if (now - title_at > 0.5) {
            char title[256];
            snprintf(title, sizeof title, "RampageRE - %s, %s%s%s%s - %u triangles, %.0f fps%s", CITIES[v.city_index],
                     v.nblocks ? v.blocks[v.block] : "-", v.night ? ", night" : "",
                     v.monster >= 0 ? ", " : "", v.monster >= 0 && v.scene.monster_clip ? v.scene.monster_clip : "",
                     rnd_triangles(v.rnd), frames / (now - title_at), v.fog ? ", fog" : "");
            win_title(&w, title);
            title_at = now;
            frames = 0;
        }
    }
    rnd_free(v.rnd);
    if (v.have_scene)
        scene_free(&v.scene);
    city_close(v.city);
    win_close(&w);
    return 0;
}
