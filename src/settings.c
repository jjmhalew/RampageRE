/* settings.c - the graphics settings (port extra, settings.h; docs/NATIVE.md "Graphics settings"): rampage.cfg, the
 * environment, the command line and the viewer's keys for them. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "plat.h"
#include "settings.h"
#include "postfx.h"
#include "gtao.h"

#define UNSET (-1)
static const char *const SMAA_NAMES[5] = { "off", "low", "medium", "high", "ultra" };

static void defaults(Settings *s)
{
    s->width = 1280; s->height = 720; s->fullscreen = 0; s->vsync = 1;
    s->ao = 0; s->ao_radius = 4;
    s->aniso = 1; s->mips = 0;
    s->smaa = 0; s->msaa = 0;
    s->profile_autoload = 1;
}

void settings_unset(Settings *f)
{
    f->width = f->height = f->fullscreen = f->vsync = f->ao = f->aniso = f->mips = f->smaa = f->msaa = UNSET;
    f->profile_autoload = UNSET;
    f->ao_radius = UNSET;
}

/* the values kept to what the passes take */
static int clamp_msaa(int n) { return n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 0; }
static int clamp_aniso(int n) { return n >= 16 ? 16 : n >= 8 ? 8 : n >= 4 ? 4 : n >= 2 ? 2 : 1; }
static int smaa_of(const char *v)                    /* 0..4 or off / low / medium / high / ultra */
{
    for (int k = 0; k < 5; k++)
        if (!_stricmp(v, SMAA_NAMES[k]))
            return k;
    int n = atoi(v);
    return n < 0 ? 0 : n > 4 ? 4 : n;
}
static int size_of(const char *v, int *w, int *h)    /* WxH */
{
    int a, b;
    if (sscanf(v, "%dx%d", &a, &b) != 2 && sscanf(v, "%dX%d", &a, &b) != 2)
        return 0;
    if (a < 160 || b < 120 || a > 16384 || b > 16384)
        return 0;
    *w = a; *h = b;
    return 1;
}

/* one key = value (rampage.cfg, or RAMPAGE_<KEY>): 0 = not a key */
static int set(Settings *s, const char *key, const char *v)
{
    if (!_stricmp(key, "window")) { if (!size_of(v, &s->width, &s->height)) printf("settings: window=%s is not WxH\n", v); }
    else if (!_stricmp(key, "fullscreen")) s->fullscreen = atoi(v) != 0;
    else if (!_stricmp(key, "vsync")) s->vsync = atoi(v) != 0;
    else if (!_stricmp(key, "ao")) s->ao = atoi(v) != 0;
    else if (!_stricmp(key, "ao_radius")) { float r = (float)atof(v); if (r > 0) s->ao_radius = r; }
    else if (!_stricmp(key, "aniso")) s->aniso = clamp_aniso(atoi(v));
    else if (!_stricmp(key, "mips")) s->mips = atoi(v) != 0;
    else if (!_stricmp(key, "smaa")) s->smaa = smaa_of(v);
    else if (!_stricmp(key, "msaa")) s->msaa = clamp_msaa(atoi(v));
    else if (!_stricmp(key, "profile_autoload")) s->profile_autoload = atoi(v) != 0;
    else return 0;
    return 1;
}

static void read_cfg(Settings *s, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char line[256];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (isspace((unsigned char)*k)) k++;
        while (isspace((unsigned char)*v)) v++;
        for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); ) *--e = 0;
        for (char *e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); ) *--e = 0;
        if (set(s, k, v)) n++;
        else printf("settings: %s has no key %s\n", path, k);
    }
    fclose(f);
    printf("settings: %s (%d keys)\n", path, n);
}

static void read_env(Settings *s)
{
    static const char *const KEYS[] = { "window", "fullscreen", "vsync", "ao", "ao_radius", "aniso", "mips", "smaa", "msaa",
                                        "profile_autoload" };
    for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++) {
        char name[32];
        snprintf(name, sizeof name, "RAMPAGE_%s", KEYS[k]);
        for (char *c = name; *c; c++)
            *c = (char)toupper((unsigned char)*c);
        const char *v = getenv(name);
        if (v && *v)
            set(s, KEYS[k], v);
    }
}

int settings_arg(Settings *f, int argc, char **argv, int *i)
{
    const char *a = argv[*i], *v = *i + 1 < argc ? argv[*i + 1] : NULL;
    if (!strcmp(a, "--ao")) f->ao = 1;
    else if (!strcmp(a, "--noao")) f->ao = 0;
    else if (!strcmp(a, "--mips")) f->mips = 1;
    else if (!strcmp(a, "--nomips")) f->mips = 0;
    else if (!strcmp(a, "--fullscreen")) f->fullscreen = 1;
    else if (!strcmp(a, "--windowed")) f->fullscreen = 0;
    else if (!strcmp(a, "--autoload")) f->profile_autoload = 1;
    else if (!strcmp(a, "--noautoload")) f->profile_autoload = 0;
    else if (v && !strcmp(a, "--ao-radius")) { float r = (float)atof(v); if (r > 0) f->ao_radius = r; ++*i; }
    else if (v && !strcmp(a, "--smaa")) { f->smaa = smaa_of(v); ++*i; }
    else if (v && !strcmp(a, "--msaa")) { f->msaa = clamp_msaa(atoi(v)); ++*i; }
    else if (v && !strcmp(a, "--aniso")) { f->aniso = clamp_aniso(atoi(v)); ++*i; }
    else if (v && !strcmp(a, "--vsync")) { f->vsync = atoi(v) != 0; ++*i; }
    else if (v && !strcmp(a, "--window")) {
        if (!size_of(v, &f->width, &f->height)) printf("--window %s is not WxH\n", v);
        ++*i;
    }
    else return 0;
    return 1;
}

void settings_resolve(Settings *s, const Settings *f, int use_cfg)
{
    defaults(s);
    if (use_cfg)
        read_cfg(s, "rampage.cfg");
    read_env(s);
    if (f->width != UNSET) { s->width = f->width; s->height = f->height; }
    if (f->fullscreen != UNSET) s->fullscreen = f->fullscreen;
    if (f->vsync != UNSET) s->vsync = f->vsync;
    if (f->ao != UNSET) s->ao = f->ao;
    if (f->ao_radius > 0) s->ao_radius = f->ao_radius;
    if (f->aniso != UNSET) s->aniso = f->aniso;
    if (f->mips != UNSET) s->mips = f->mips;
    if (f->smaa != UNSET) s->smaa = f->smaa;
    if (f->msaa != UNSET) s->msaa = f->msaa;
    if (f->profile_autoload != UNSET) s->profile_autoload = f->profile_autoload;
}

static void apply_render(const Settings *s)
{
    postfx_set(s->msaa, s->smaa);
    gtao_enable(s->ao, s->ao_radius);
    rnd_set_textures(s->aniso, s->mips);
}

/* what is on, and what this GL cannot do of it */
static void print(const Settings *s)
{
    int msmax = s->msaa ? postfx_msaa_max() : 0, amax = s->aniso > 1 ? rnd_aniso_max() : 1;
    printf("graphics: ao=%d%s ao_radius=%g smaa=%d (%s)%s msaa=%d%s aniso=%d%s mips=%d\n", s->ao,
           s->ao && !gtao_supported() ? " (not supported)" : "", s->ao_radius, s->smaa, SMAA_NAMES[s->smaa],
           s->smaa && !postfx_smaa_supported() ? " (not supported)" : "", s->msaa,
           s->msaa && msmax < 2 ? " (not supported)" : s->msaa > msmax ? " (more than this GL can)" : "", s->aniso,
           s->aniso > amax ? " (more than this GL can)" : "", s->mips);
}

void settings_apply(const Settings *s, Window *w)
{
    apply_render(s);
    if (!w->hidden) {
        win_mode(w, s->width, s->height, s->fullscreen);
        win_vsync(w, s->vsync);
    }
    if (s->ao || s->smaa || s->msaa || s->aniso > 1 || s->mips)
        print(s);
}

int settings_keys(Settings *s, Window *w)
{
    int changed = 1;
    if (w->pressed[VK_F5])
        s->ao = !s->ao;
    else if (w->pressed[VK_F6])
        s->smaa = (s->smaa + 1) % 5;
    else if (w->pressed[VK_F7]) {                    /* 0, 2, 4, 8 as far as this GL goes (none: 0, 2 "not
                                                      * supported") */
        int max = postfx_msaa_max() < 2 ? 2 : postfx_msaa_max();
        s->msaa = s->msaa ? 2 * s->msaa : 2;
        if (s->msaa > max)
            s->msaa = 0;
    } else if (w->pressed[VK_F8]) {                  /* 1, 2, 4, 8, 16 likewise */
        int max = rnd_aniso_max() < 2 ? 2 : rnd_aniso_max();
        s->aniso = 2 * s->aniso;
        if (s->aniso > max)
            s->aniso = 1;
    } else if (w->pressed[VK_F9])
        s->mips = !s->mips;
    else if (w->pressed[VK_F11]) {
        s->fullscreen = !s->fullscreen;
        win_mode(w, s->width, s->height, s->fullscreen);
        return 1;
    } else
        changed = 0;
    if (changed) {
        char what[128], title[160];
        apply_render(s);
        print(s);
        settings_describe(s, what, sizeof what);
        snprintf(title, sizeof title, "RampageRE - graphics%s", what[0] ? what : ": all off");
        win_title(w, title);
    }
    return changed;
}

void settings_describe(const Settings *s, char *buf, size_t cap)
{
    size_t n = 0;
    buf[0] = 0;
    #define ADD(...) do { if (n < cap) n += (size_t)snprintf(buf + n, cap - n, __VA_ARGS__); } while (0)
    if (s->ao) ADD(", AO");
    if (s->smaa) ADD(", SMAA %s", SMAA_NAMES[s->smaa]);
    if (s->msaa) ADD(", MSAA %dx", s->msaa);
    if (s->aniso > 1) ADD(", aniso %dx", s->aniso);
    if (s->mips) ADD(", mips");
    #undef ADD
}

void settings_graphics(Settings *s, Window *w, const Settings *to)
{
    int mode = to->fullscreen != s->fullscreen, sync = to->vsync != s->vsync;
    s->ao = to->ao;
    s->smaa = to->smaa < 0 ? 0 : to->smaa > 4 ? 4 : to->smaa;
    s->msaa = clamp_msaa(to->msaa);
    s->aniso = clamp_aniso(to->aniso);
    s->mips = to->mips;
    s->fullscreen = to->fullscreen;
    s->vsync = to->vsync;
    apply_render(s);
    if (!w->hidden && mode)
        win_mode(w, s->width, s->height, s->fullscreen);
    if (!w->hidden && sync)
        win_vsync(w, s->vsync);
    print(s);
}

int settings_save(const Settings *s, const char *path)
{
    static const char *const KEYS[] = { "fullscreen", "vsync", "ao", "smaa", "msaa", "aniso", "mips" };
    enum { NK = sizeof KEYS / sizeof *KEYS, MAXL = 256 };
    char val[NK][16], *lines[MAXL], line[512];
    int n = 0, done[NK] = { 0 };
    snprintf(val[0], sizeof val[0], "%d", s->fullscreen);
    snprintf(val[1], sizeof val[1], "%d", s->vsync);
    snprintf(val[2], sizeof val[2], "%d", s->ao);
    snprintf(val[3], sizeof val[3], "%s", SMAA_NAMES[s->smaa < 0 ? 0 : s->smaa > 4 ? 4 : s->smaa]);
    snprintf(val[4], sizeof val[4], "%d", s->msaa);
    snprintf(val[5], sizeof val[5], "%d", s->aniso);
    snprintf(val[6], sizeof val[6], "%d", s->mips);
    FILE *f = fopen(path, "r");
    if (f) {
        while (n < MAXL && fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!(lines[n] = strdup(line)))
                break;
            n++;
        }
        fclose(f);
    }
    f = fopen(path, "w");
    for (int i = 0; i < n; i++) {
        const char *k = lines[i];
        while (isspace((unsigned char)*k)) k++;
        size_t len = strcspn(k, " \t=#");
        const char *after = k + len;
        while (*after == ' ' || *after == '\t') after++;
        int key = -1;
        for (int j = 0; *after == '=' && j < NK; j++)
            if (strlen(KEYS[j]) == len && !_strnicmp(k, KEYS[j], len))
                key = j;
        if (f && key < 0)
            fprintf(f, "%s\n", lines[i]);
        else if (f && !done[key]) {                  /* the key's line, its comment kept; a second one dropped */
            const char *hash = strchr(lines[i], '#');
            fprintf(f, "%s=%s%s%s\n", KEYS[key], val[key], hash ? " " : "", hash ? hash : "");
            done[key] = 1;
        }
        free(lines[i]);
    }
    for (int j = 0; f && j < NK; j++)
        if (!done[j])
            fprintf(f, "%s=%s\n", KEYS[j], val[j]);
    if (!f || fclose(f) != 0) {
        printf("settings: could not write %s\n", path);
        return -1;
    }
    printf("settings: %s written\n", path);
    return 0;
}
