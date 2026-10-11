/* iso.c - the game's data straight from the disc image, and where the data is (iso.h). */
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "iso.h"
#ifdef _WIN32
#define SEP "\\"
#else
#include <dirent.h>
#define SEP "/"
#endif

static int read_at(FILE *f, int64_t at, void *buf, size_t n)
{
    return _fseeki64(f, at, SEEK_SET) == 0 && fread(buf, 1, n, f) == n;
}

/* the root directory's extent from the primary volume descriptor (sector 16 on, up to the terminator) */
static int iso_root(FILE *f, uint32_t *lba, uint32_t *len)
{
    uint8_t d[2048];
    for (int s = 16; s < 32; s++) {
        if (!read_at(f, (int64_t)s * 2048, d, sizeof d) || memcmp(d + 1, "CD001", 5) != 0 || d[0] == 255)
            return 0;
        if (d[0] == 1) {                                 /* the root's directory record at +156 */
            *lba = rd32(d + 156 + 2);
            *len = rd32(d + 156 + 10);
            return 1;
        }
    }
    return 0;
}

int iso_is(FILE *f)
{
    uint32_t lba, len;
    return iso_root(f, &lba, &len);
}

/* the entry name[0..n) in the directory at lba (len bytes): a directory or a file as dir says */
static int dir_find(FILE *f, uint32_t *lba, uint32_t *len, const char *name, size_t n, int dir)
{
    uint8_t s[2048];
    for (uint32_t at = 0; at < *len; at += 2048) {
        if (!read_at(f, ((int64_t)*lba + at / 2048) * 2048, s, sizeof s))
            return 0;
        for (uint32_t i = 0; i + 34 <= sizeof s && s[i]; i += s[i]) {   /* records never cross a sector; 0 = its end */
            const uint8_t *r = s + i;
            uint32_t k = 0, nl = r[32];
            if (r[0] < 34 || i + r[0] > sizeof s || 33 + nl > r[0])
                break;
            const char *e = (const char *)r + 33;
            while (k < nl && e[k] != ';')                /* "NAME.EXT;1" */
                k++;
            if (k && e[k - 1] == '.')                    /* "NAME." for a file without extension */
                k--;
            if (k == n && _strnicmp(e, name, n) == 0 && ((r[25] & 2) != 0) == (dir != 0)) {
                *lba = rd32(r + 2);
                *len = rd32(r + 10);
                return 1;
            }
        }
    }
    return 0;
}

int iso_find(FILE *f, const char *path, int64_t *offset, uint32_t *size)
{
    uint32_t lba, len;
    if (!iso_root(f, &lba, &len))
        return 0;
    while (*path) {
        const char *end = path;
        while (*end && *end != '/' && *end != '\\')
            end++;
        if (end > path && !dir_find(f, &lba, &len, path, (size_t)(end - path), *end != 0))
            return 0;
        path = *end ? end + 1 : end;
    }
    *offset = (int64_t)lba * 2048;
    *size = len;
    return 1;
}

static uint8_t *slurp_at(FILE *f, int64_t at, uint32_t n)
{
    uint8_t *b = malloc(n + 1u);
    if (b && !read_at(f, at, b, n)) {
        free(b);
        return NULL;
    }
    if (b)
        b[n] = 0;
    return b;
}

/* a whole file, 0-terminated */
static uint8_t *slurp(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *b = NULL;
    if (_fseeki64(f, 0, SEEK_END) == 0) {
        int64_t n = _ftelli64(f);
        if (n > 0 && n < (1 << 30) && (b = slurp_at(f, 0, (uint32_t)n)) != NULL)
            *size = (uint32_t)n;
    }
    fclose(f);
    return b;
}

/* a file of the image, 0-terminated (NULL: not there) */
static uint8_t *iso_slurp(FILE *f, const char *path, uint32_t *size)
{
    int64_t at;
    uint32_t n;
    uint8_t *b = iso_find(f, path, &at, &n) && n < (1u << 30) ? slurp_at(f, at, n) : NULL;
    if (b)
        *size = n;
    return b;
}

/* the ELF SYSTEM.CNF boots: BOOT2 = cdrom0:\SLES_538.00;1 */
static int boot_name(const char *cnf, char *out, size_t cap)
{
    const char *p = cnf ? strstr(cnf, "BOOT2") : NULL;
    if (!p || !(p = strstr(p, "cdrom0:")))
        return 0;
    p += 7;
    while (*p == '\\' || *p == '/')
        p++;
    size_t n = 0;
    while (p[n] && p[n] != ';' && p[n] != '\r' && p[n] != '\n' && n + 1 < cap) {
        out[n] = p[n] == '\\' ? '/' : p[n];
        n++;
    }
    out[n] = 0;
    return n > 0;
}

/* the file name beside path (in its folder) */
static void beside(char *out, size_t cap, const char *path, const char *name)
{
    const char *s = strrchr(path, '\\'), *s2 = strrchr(path, '/');
    if (s2 > s) s = s2;
    size_t n = s ? (size_t)(s + 1 - path) : 0;
    snprintf(out, cap, "%.*s%s", (int)n, path, name);
}

uint8_t *data_elf(const char *path, uint32_t *size)
{
    uint8_t *elf = NULL;
    FILE *f = fopen(path, "rb");
    if (f && iso_is(f)) {
        uint32_t n;
        char boot[64];
        char *cnf = (char *)iso_slurp(f, "SYSTEM.CNF", &n);
        elf = iso_slurp(f, boot_name(cnf, boot, sizeof boot) ? boot : "SLES_538.00", size);
        free(cnf);
    }
    if (f)
        fclose(f);
    const char *names[2] = { "RAMPAGE.ELF", "SLES_538.00" };
    for (int i = 0; i < 2 && !elf; i++) {
        char p[1024];
        beside(p, sizeof p, path, names[i]);
        elf = slurp(p, size);
    }
    return elf;
}

/* ---- the finder */
static int readable(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static void join(char *out, size_t cap, const char *dir, const char *name)
{
    if (!dir[0] || strcmp(dir, ".") == 0)
        snprintf(out, cap, "%s", name);
    else
        snprintf(out, cap, "%s%s%s", dir, strchr("\\/", dir[strlen(dir) - 1]) ? "" : SEP, name);
}

static int vol_in(const char *dir, char *out, size_t cap)
{
    if (!dir[0])
        return 0;
    join(out, cap, dir, "RAMPAGE.VOL");
    return readable(out);
}

static int has_vol(const char *path)
{
    FILE *f = fopen(path, "rb");
    int64_t at;
    uint32_t n;
    int ok = f && iso_find(f, "RAMPAGE.VOL", &at, &n);
    if (f)
        fclose(f);
    return ok;
}

/* name is the first (by name) image in dir with RAMPAGE.VOL so far */
static void consider(const char *dir, const char *name, char *best, size_t cap, char *best_name)
{
    size_t n = strlen(name);
    char p[1024];
    if (n < 5 || _stricmp(name + n - 4, ".iso") != 0 || (best_name[0] && strcmp(name, best_name) >= 0))
        return;
    join(p, sizeof p, dir, name);
    if (has_vol(p)) {
        snprintf(best, cap, "%s", p);
        snprintf(best_name, 256, "%s", name);
    }
}

static int iso_in(const char *dir, char *out, size_t cap)
{
    char best_name[256] = "";
    if (!dir[0])
        return 0;
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    char pat[1024];
    join(pat, sizeof pat, dir, "*.iso");
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            consider(dir, fd.cFileName, out, cap, best_name);
    while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *de;
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL)
        consider(dir, de->d_name, out, cap, best_name);
    closedir(d);
#endif
    return best_name[0] != 0;
}

static void exe_dir(char *out, size_t cap)
{
    out[0] = 0;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)cap);
    if (n == 0 || n >= cap)
        out[0] = 0;
#else
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    out[n > 0 ? n : 0] = 0;
#endif
    char *s = strrchr(out, SEP[0]);
    if (s)
        *s = 0;
    else
        out[0] = 0;
}

static void user_dir(char *out, size_t cap)
{
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    if (base && base[0])
        snprintf(out, cap, "%s\\RampageRE", base);
    else
        out[0] = 0;
#else
    const char *base = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (base && base[0])
        snprintf(out, cap, "%s/RampageRE", base);
    else if (home && home[0])
        snprintf(out, cap, "%s/.local/share/RampageRE", home);
    else
        out[0] = 0;
#endif
}

int data_find(char *out, size_t cap)
{
    char exe[1024], user[1024];
    const char *env = getenv("RAMPAGE_VOL");
    if (env && env[0]) {                                 /* used as it is, there or not */
        snprintf(out, cap, "%s", env);
        return 1;
    }
    exe_dir(exe, sizeof exe);
    user_dir(user, sizeof user);
    return vol_in("build" SEP "disc", out, cap) || vol_in(".", out, cap) || vol_in("data", out, cap) ||
           vol_in(exe, out, cap) || iso_in(exe, out, cap) || iso_in(".", out, cap) || iso_in("data", out, cap) ||
           vol_in(user, out, cap) || iso_in(user, out, cap);
}

const char *data_where(void)
{
#ifdef _WIN32
    return "build\\disc, the current folder, data\\, beside rampage.exe, %LOCALAPPDATA%\\RampageRE";
#else
    return "build/disc, the current folder, data/, beside rampage, $XDG_DATA_HOME/RampageRE (~/.local/share/RampageRE)";
#endif
}

int data_check(const char *path, const Vol *v, char *msg, size_t cap)
{
    uint32_t n = 0;
    char *cnf = NULL, boot[64] = "";
    FILE *f = v->base ? fopen(path, "rb") : NULL;        /* an image: its SYSTEM.CNF; else the one beside the VOL */
    if (f) {
        cnf = (char *)iso_slurp(f, "SYSTEM.CNF", &n);
        fclose(f);
    } else {
        char p[1024];
        beside(p, sizeof p, path, "SYSTEM.CNF");
        cnf = (char *)slurp(p, &n);
    }
    int boots = boot_name(cnf, boot, sizeof boot);
    const char *ver = cnf ? strstr(cnf, "VER") : NULL;
    if (ver)
        ver = strchr(ver, '=');
    while (ver && (*ver == '=' || *ver == ' '))
        ver++;
    uint64_t end = 0;                                    /* the VOL: 352 files up to 1,342,827,563 */
    for (uint32_t i = 0; i < v->count; i++)
        if ((uint64_t)v->files[i].offset + v->files[i].size > end)
            end = (uint64_t)v->files[i].offset + v->files[i].size;
    const char *why = NULL;
    char text[96];
    if (boots && _stricmp(boot, "SLES_538.00") != 0) {
        snprintf(text, sizeof text, "SYSTEM.CNF boots %s", boot);
        why = text;
    } else if (boots && ver && strncmp(ver, "1.00", 4) != 0) {
        snprintf(text, sizeof text, "SYSTEM.CNF says version %.8s", ver);
        why = text;
    } else if (v->count != 352 || end != 1342827563u)
        why = "its RAMPAGE.VOL is not that release's";
    free(cnf);
    if (!why)
        return 1;
    snprintf(msg, cap, "%s is not the supported release, Rampage - Total Destruction (Europe) SLES-53800 version 1.00: "
             "%s.\nThe viewer may not work with it.", path, why);
    return 0;
}
