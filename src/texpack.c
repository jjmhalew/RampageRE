/* texpack.c - texture packs (port extra, docs/TEXTURES.md; WoodyRE's texpack.c).
 * A texture is known by a 64-bit FNV-1a hash of the RGBA tex_decode makes of its mip 0 (tag 'T', w, h): the palette is
 * part of the picture and the PSMT8 swizzle and CSM1 order are undone, so the same texture in two bundles or cities has
 * the same hash, and one PNG replaces it everywhere. A replacement may be any size: the game keeps every size and
 * texture coordinate of the original and only samples the bigger image. Files: mods/textures/ (any subfolders) in the
 * current directory, next to rampage_profile.bin, named <anything>_<16 hex digits>.png - the dump names them
 * <w>x<h>_<hash>.png. Without a mods/ folder (and no --dumptex) nothing is hashed and the game is drawn as before. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "texpack.h"

#if defined(__GNUC__)                /* stb with only PNG leaves helpers unused (gcc, clang) */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb/stb_image_write.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* paths: wide on Windows (a user name the ANSI code page cannot spell), UTF-8 bytes elsewhere */
#ifdef _WIN32
typedef wchar_t PathC;
#define PMAX MAX_PATH
#define PF "%ls"
static void p_join(PathC *out, const PathC *a, const char *b) { _snwprintf(out, PMAX, L"%ls\\%hs", a, b); out[PMAX - 1] = 0; }
static void p_home(PathC *out) { DWORD n = GetCurrentDirectoryW(PMAX, out); if (!n || n >= PMAX - 8) wcscpy(out, L"."); }
static FILE *p_open(const PathC *p, const char *mode) { wchar_t m[8]; mbstowcs(m, mode, 8); return _wfopen(p, m); }
static int p_exists(const PathC *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static void p_mkdir(const PathC *p) { CreateDirectoryW(p, NULL); }
#define p_dup _wcsdup
#else
typedef char PathC;
#define PMAX 4096
#define PF "%s"
static void p_join(PathC *out, const PathC *a, const char *b) { snprintf(out, PMAX, "%s/%s", a, b); }
static void p_home(PathC *out) { if (!getcwd(out, PMAX - 8)) strcpy(out, "."); }
static FILE *p_open(const PathC *p, const char *mode) { return fopen(p, mode); }
static int p_exists(const PathC *p) { struct stat st; return stat(p, &st) == 0; }
static void p_mkdir(const PathC *p) { mkdir(p, 0755); }
#define p_dup strdup
#endif

typedef struct { uint64_t hash; PathC *path; } Entry;
typedef struct { Entry *tab; uint32_t cap, n; } Table;   /* open addressing, hash 0 = empty */
static struct {
    int scanned, dump, ndumped, nreplaced; char scope[64];
    PathC root[PMAX];                /* <current directory>/mods */
    Table pack, dumped;              /* mods/textures by hash; the hashes in mods/dump or written this run */
} T = { .scope = "Common" };

uint64_t tp_hash(char kind, const void *data, uint32_t bytes, int w, int h)
{
    uint64_t x = 0xcbf29ce484222325ull; const uint8_t *p = (const uint8_t *)data;
    uint8_t head[9] = { (uint8_t)kind, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)(w >> 16), (uint8_t)(w >> 24), (uint8_t)h, (uint8_t)(h >> 8), (uint8_t)(h >> 16), (uint8_t)(h >> 24) };
    for (int i = 0; i < 9; i++) { x ^= head[i]; x *= 0x100000001b3ull; }
    for (uint32_t i = 0; i < bytes; i++) { x ^= p[i]; x *= 0x100000001b3ull; }
    return x ? x : 1;
}
void tp_scope(const char *name) { snprintf(T.scope, sizeof T.scope, "%s", name && *name ? name : "Common"); }
const char *tp_scope_get(void) { return T.scope; }
void tp_set_dump(int on) { T.dump = on; }

static void put(Table *t, uint64_t hash, const PathC *path)
{
    if ((t->n + 1) * 2 > t->cap) {                         /* grow at half full */
        Entry *old = t->tab; uint32_t oc = t->cap; t->cap = oc ? oc * 2 : 256; t->tab = (Entry *)calloc(t->cap, sizeof *t->tab); t->n = 0;
        for (uint32_t i = 0; i < oc; i++) if (old[i].hash) { uint32_t k = (uint32_t)old[i].hash & (t->cap - 1); while (t->tab[k].hash) k = (k + 1) & (t->cap - 1); t->tab[k] = old[i]; t->n++; }
        free(old);
    }
    uint32_t k = (uint32_t)hash & (t->cap - 1);
    while (t->tab[k].hash) { if (t->tab[k].hash == hash) return; k = (k + 1) & (t->cap - 1); }   /* two files for one texture: the first one found wins */
    t->tab[k].hash = hash; t->tab[k].path = path ? p_dup(path) : NULL; t->n++;
}
static const Entry *find(const Table *t, uint64_t hash)
{
    if (!t->cap) return NULL;
    for (uint32_t k = (uint32_t)hash & (t->cap - 1); t->tab[k].hash; k = (k + 1) & (t->cap - 1)) if (t->tab[k].hash == hash) return &t->tab[k];
    return NULL;
}
static int name_hash(const PathC *name, uint64_t *out)   /* <anything>_<16 hex>.png or <16 hex>.png */
{
    size_t n = 0; while (name[n]) n++;
    if (n < 20) return 0;
    const PathC *e = name + n - 4;
    if (e[0] != '.' || (e[1] | 0x20) != 'p' || (e[2] | 0x20) != 'n' || (e[3] | 0x20) != 'g') return 0;
    const PathC *h = name + n - 20; if (h > name && h[-1] != '_') return 0;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        int c = (int)h[i], d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = v << 4 | (uint64_t)d;
    }
    *out = v ? v : 1; return 1;
}
#ifdef _WIN32
static void scan_dir(Table *t, const PathC *dir, int depth)
{
    wchar_t pat[MAX_PATH]; WIN32_FIND_DATAW fd;
    if (_snwprintf(pat, MAX_PATH, L"%ls\\*", dir) >= MAX_PATH) return;
    HANDLE h = FindFirstFileW(pat, &fd); if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.') continue;
        wchar_t p[MAX_PATH]; if (_snwprintf(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) >= MAX_PATH) continue;
        uint64_t hv;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 8) scan_dir(t, p, depth + 1); }
        else if (name_hash(fd.cFileName, &hv)) put(t, hv, p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
#else
static void scan_dir(Table *t, const PathC *dir, int depth)
{
    DIR *d = opendir(dir); struct dirent *de; if (!d) return;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char p[PMAX]; if (snprintf(p, PMAX, "%s/%s", dir, de->d_name) >= PMAX) continue;
        struct stat st; uint64_t hv; if (stat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 8) scan_dir(t, p, depth + 1); }
        else if (name_hash(de->d_name, &hv)) put(t, hv, p);
    }
    closedir(d);
}
#endif
static void scan(void)
{
    T.scanned = 1;
    PathC home[PMAX]; p_home(home);                      /* the current directory, as rampage_profile.bin */
    p_join(T.root, home, "mods");
    if (!p_exists(T.root)) return;                       /* no mods/: nothing to read */
    PathC dir[PMAX]; p_join(dir, T.root, "textures");
    scan_dir(&T.pack, dir, 0);
    if (T.pack.n) printf("texture pack: %u replacement textures in " PF "\n", T.pack.n, dir);
    if (T.dump) { PathC d[PMAX]; p_join(d, T.root, "dump"); scan_dir(&T.dumped, d, 0); }   /* dumped before: not again */
}
int tp_active(void)
{
    if (!T.scanned) scan();
    return T.pack.n || T.dump;
}

static uint8_t *read_file(const PathC *path, int *size)
{
    FILE *f = p_open(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = n > 0 ? (uint8_t *)malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f); *size = (int)n; return d;
}
uint8_t *tp_pixels(uint64_t hash, int alpha_mode, int *pw, int *ph)
{
    if (!T.scanned) scan();
    const Entry *en = find(&T.pack, hash); if (!en) return NULL;
    const PathC *path = en->path;
    int size, w, h, ch; uint8_t *file = read_file(path, &size); if (!file) return NULL;
    uint8_t *px = stbi_load_from_memory(file, size, &w, &h, &ch, 4); free(file);   /* stbi's malloc: free() frees it */
    GLint max = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max);
    if (!px || w > max || h > max) { printf("texture pack: cannot use " PF " (%s)\n", path, px ? "too big for this card" : stbi_failure_reason()); if (px) stbi_image_free(px); return NULL; }
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = px + i * 4;
        if (alpha_mode == TP_OPAQUE) p[3] = 255;
        else if (alpha_mode == TP_KEY) { if (p[3] < 128) p[0] = p[1] = p[2] = p[3] = 0; else p[3] = 255; }   /* transparent texels black (no fringe) */
    }
    *pw = w; *ph = h; T.nreplaced++;
    return px;
}
void tp_upload(uint8_t *px, int w, int h, int alpha_mode)
{
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    int level = 0;
    while (w > 1 || h > 1) {                               /* the full chain, 2x2 box filter; a colour key level keeps its alpha on/off */
        int dw = w > 1 ? w / 2 : 1, dh = h > 1 ? h / 2 : 1;
        for (int y = 0; y < dh; y++) for (int x = 0; x < dw; x++) {
            int x0 = 2 * x < w ? 2 * x : w - 1, x1 = 2 * x + 1 < w ? 2 * x + 1 : x0, y0 = 2 * y < h ? 2 * y : h - 1, y1 = 2 * y + 1 < h ? 2 * y + 1 : y0;
            const uint8_t *s[4] = { px + ((size_t)y0 * w + x0) * 4, px + ((size_t)y0 * w + x1) * 4, px + ((size_t)y1 * w + x0) * 4, px + ((size_t)y1 * w + x1) * 4 };
            uint8_t *d = px + ((size_t)y * dw + x) * 4;           /* in place: d never passes the sources still to be read */
            unsigned c[4] = { 0, 0, 0, 0 }, on = 0;
            for (int k = 0; k < 4; k++) { for (int q = 0; q < 4; q++) c[q] += s[k][q]; on += s[k][3] >= 128; }
            if (alpha_mode == TP_KEY && on) { unsigned r = 0, g = 0, b = 0; for (int k = 0; k < 4; k++) if (s[k][3] >= 128) { r += s[k][0]; g += s[k][1]; b += s[k][2]; } c[0] = r * 4 / on; c[1] = g * 4 / on; c[2] = b * 4 / on; }
            for (int q = 0; q < 4; q++) d[q] = (uint8_t)((c[q] + 2) / 4);
            if (alpha_mode == TP_KEY) { if (on >= 2) d[3] = 255; else d[0] = d[1] = d[2] = d[3] = 0; }
        }
        w = dw; h = dh; level++;
        glTexImage2D(GL_TEXTURE_2D, level, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);                 /* GL's default, as the other uploads expect */
    glTexParameteri(GL_TEXTURE_2D, 0x813D /* GL_TEXTURE_MAX_LEVEL */, level);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
}
int tp_replace(uint64_t hash, int alpha_mode)
{
    int w, h; uint8_t *px = tp_pixels(hash, alpha_mode, &w, &h); if (!px) return 0;
    tp_upload(px, w, h, alpha_mode);
    stbi_image_free(px);
    return 1;
}

static void write_cb(void *ctx, void *data, int size) { fwrite(data, 1, (size_t)size, (FILE *)ctx); }
void tp_dump(uint64_t hash, const uint8_t *rgba, int w, int h)
{
    if (!T.dump) return;
    if (!T.scanned) scan();
    if (find(&T.dumped, hash)) return;                     /* once per hash, whatever the scope */
    put(&T.dumped, hash, NULL);
    PathC d1[PMAX], dir[PMAX], path[PMAX]; char name[64];
    p_join(d1, T.root, "dump"); p_join(dir, d1, T.scope);
    snprintf(name, sizeof name, "%dx%d_%016llx.png", w, h, (unsigned long long)hash); p_join(path, dir, name);
    if (p_exists(path)) return;
    p_mkdir(T.root); p_mkdir(d1); p_mkdir(dir);
    FILE *f = p_open(path, "wb"); if (!f) return;
    stbi_write_png_to_func(write_cb, f, w, h, 4, rgba, w * 4);
    fclose(f);
    if (++T.ndumped % 100 == 1) printf("texture dump: %d files so far (" PF ")\n", T.ndumped, dir);
}
