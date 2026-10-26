/* iso.c - the game's data straight from the disc image (or from an APK of make_android_bundle.bat, which carries the
 * disc's files), and where the data is (iso.h). */
#include <stdlib.h>
#include <string.h>
#include "plat.h"
#include "iso.h"
#ifdef _WIN32
#define SEP "\\"
#else
#include <dirent.h>
#include <sys/stat.h>
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

/* ---- an APK of make_android_bundle.bat (tools/native/apkbundle.c): a ZIP whose entries assets/RAMPAGE.VOL,
 * assets/<the ELF SYSTEM.CNF boots> and assets/SYSTEM.CNF are the disc's files, stored (not compressed), so they are read
 * in place as from the image: path "RAMPAGE.VOL" is the entry assets/RAMPAGE.VOL. No ZIP64 (the bundle stays below 2 GB) */
static int zip_find(FILE *f, const char *path, int64_t *offset, uint32_t *size)
{
    uint8_t tail[22 + 1024], *cd = NULL;
    char want[128];
    int64_t end;
    if (_fseeki64(f, 0, SEEK_END) != 0 || (end = _ftelli64(f)) < 22 || snprintf(want, sizeof want, "assets/%s", path) >=
        (int)sizeof want)
        return 0;
    for (char *c = want; *c; c++)
        if (*c == '\\')
            *c = '/';
    size_t tn = end < (int64_t)sizeof tail ? (size_t)end : sizeof tail, e = tn - 22;   /* the end record, a short comment */
    if (!read_at(f, end - (int64_t)tn, tail, tn))
        return 0;
    while (e > 0 && !(rd32(tail + e) == 0x06054b50 && e + 22 + rd16(tail + e + 20) == tn))
        e--;
    uint32_t cd_size = rd32(tail + e + 12), cd_at = rd32(tail + e + 16);
    if (rd32(tail + e) != 0x06054b50 || cd_at == 0xffffffffu || cd_size > (16u << 20) || (int64_t)cd_at + cd_size > end ||
        !(cd = malloc(cd_size ? cd_size : 1)) || !read_at(f, cd_at, cd, cd_size)) {
        free(cd);
        return 0;
    }
    size_t wl = strlen(want);
    int found = 0;
    for (uint32_t o = 0; o + 46 <= cd_size && rd32(cd + o) == 0x02014b50; ) {
        uint32_t nl = rd16(cd + o + 28), len = 46 + nl + rd16(cd + o + 30) + rd16(cd + o + 32);
        if (o + len > cd_size)
            break;
        if (nl == wl && _strnicmp((const char *)cd + o + 46, want, wl) == 0) {
            uint8_t lh[30];
            uint32_t at = rd32(cd + o + 42), n = rd32(cd + o + 24);
            found = rd16(cd + o + 10) == 0 && rd32(cd + o + 20) == n && read_at(f, at, lh, sizeof lh) &&
                    rd32(lh) == 0x04034b50;                  /* stored, and its local header */
            if (found) {
                *offset = (int64_t)at + 30 + rd16(lh + 26) + rd16(lh + 28);
                *size = n;
                found = *offset + n <= end;
            }
            break;
        }
        o += len;
    }
    free(cd);
    return found;
}

int iso_is(FILE *f)
{
    uint32_t lba, len, n;
    int64_t at;
    return iso_root(f, &lba, &len) || zip_find(f, "RAMPAGE.VOL", &at, &n);
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
        return zip_find(f, path, offset, size);
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
        if (!elf)                                        /* an APK of make_android_bundle.bat keeps it as SLES_538.00 */
            elf = iso_slurp(f, "SLES_538.00", size);
        free(cnf);
    }
    if (f)
        fclose(f);
    const char *names[3] = { "RAMPAGE.ELF", "SLES_538.00", "SLUS_213.23" };
    for (int i = 0; i < 3 && !elf; i++) {
        char p[1024];
        beside(p, sizeof p, path, names[i]);
        elf = slurp(p, size);
    }
    return elf;
}

static int64_t file_size(FILE *f)
{
    return _fseeki64(f, 0, SEEK_END) == 0 ? _ftelli64(f) : -1;
}

int data_parts(const char *path, DataPart part[3])
{
    static const char *const names[3] = { "SYSTEM.CNF", "SLES_538.00", "RAMPAGE.VOL" };
    memset(part, 0, 3 * sizeof *part);
    for (int i = 0; i < 3; i++)
        snprintf(part[i].name, sizeof part[i].name, "%s", names[i]);
    uint8_t head[4];
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    char *cnf = NULL;
    uint32_t n = 0;
    int64_t at;
    int ok = 0;
    if (fread(head, 1, 4, f) == 4 && memcmp(head, "PVOL", 4) == 0) {   /* RAMPAGE.VOL, the rest beside it */
        int64_t size = file_size(f);
        fclose(f);
        if (size < 16 || size > 0xffffffffll)
            return 0;
        snprintf(part[2].file, sizeof part[2].file, "%s", path);
        part[2].size = (uint32_t)size;
        beside(part[0].file, sizeof part[0].file, path, "SYSTEM.CNF");
        if ((cnf = (char *)slurp(part[0].file, &n)) != NULL)
            part[0].size = n;
        const char *elf[3] = { "RAMPAGE.ELF", "SLES_538.00", "SLUS_213.23" };   /* as data_elf: build.ps1's split
                                                                                  * copy first */
        for (int i = 0; i < 3 && !part[1].size; i++) {
            beside(part[1].file, sizeof part[1].file, path, elf[i]);
            FILE *e = fopen(part[1].file, "rb");
            int64_t es = e ? file_size(e) : -1;
            if (e)
                fclose(e);
            if (es > 0 && es < (1 << 30))
                part[1].size = (uint32_t)es;
        }
        ok = 1;
    } else {
        if (iso_is(f) && iso_find(f, "RAMPAGE.VOL", &part[2].at, &part[2].size)) {
            for (int i = 0; i < 3; i++)
                snprintf(part[i].file, sizeof part[i].file, "%s", path);
            if (iso_find(f, "SYSTEM.CNF", &part[0].at, &part[0].size))
                cnf = (char *)iso_slurp(f, "SYSTEM.CNF", &n);
            ok = 1;
        }
        char boot[64];
        if (ok && iso_find(f, boot_name(cnf, boot, sizeof boot) ? boot : "SLES_538.00", &at, &n)) {
            part[1].at = at;
            part[1].size = n;
        }
        fclose(f);
    }
    char bn[64];
    if (boot_name(cnf, bn, sizeof bn))                   /* the name an image holds the ELF under */
        snprintf(part[1].name, sizeof part[1].name, "%s", bn);
    free(cnf);
    return ok;
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

int data_usable(const char *path)
{
    uint8_t head[4];
    FILE *f = fopen(path, "rb");
    int64_t at;
    uint32_t n;
    int ok = f && fread(head, 1, 4, f) == 4 && (memcmp(head, "PVOL", 4) == 0 ||
             (iso_find(f, "RAMPAGE.VOL", &at, &n) && n >= 16 && read_at(f, at, head, 4) && memcmp(head, "PVOL", 4) == 0));
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
#elif defined __SWITCH__ || defined __vita__
    (void)cap;                                   /* no /proc: SWITCH_HOME / VITA_HOME stands for it */
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

/* the disc image or RAMPAGE.VOL chosen at an earlier start (data_ask): its path, one line in <user folder>/disc.txt */
static void disc_txt(char *out, size_t cap)
{
    char user[1024];
    user_dir(user, sizeof user);
    if (user[0])
        join(out, cap, user, "disc.txt");
    else
        out[0] = 0;
}

static int remembered(char *out, size_t cap)
{
    char p[1024], line[1024];
    disc_txt(p, sizeof p);
    FILE *f = p[0] ? fopen(p, "rb") : NULL;
    size_t n = f ? fread(line, 1, sizeof line - 1, f) : 0;
    if (f)
        fclose(f);
    line[n] = 0;
    line[strcspn(line, "\r\n")] = 0;
#ifdef _WIN32
    UINT em = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);   /* a disc drive left empty: no box */
#endif
    int ok = line[0] && data_usable(line);
#ifdef _WIN32
    SetErrorMode(em);
#endif
    if (ok)
        snprintf(out, cap, "%s", line);
    return ok;
}

int data_remember(const char *path)
{
    char user[1024], p[1024];
    user_dir(user, sizeof user);
    if (!user[0])
        return 0;
#ifdef _WIN32
    CreateDirectoryA(user, NULL);
#else
    mkdir(user, 0755);
#endif
    disc_txt(p, sizeof p);
    FILE *f = fopen(p, "wb");
    if (!f)
        return 0;
    fprintf(f, "%s\n", path);
    return fclose(f) == 0;
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
#ifdef __SWITCH__
    if (vol_in(SWITCH_HOME, out, cap) || iso_in(SWITCH_HOME, out, cap))   /* the SD card's sdmc:/switch/rampagere */
        return 1;
#elif defined __vita__
    if (vol_in(VITA_HOME, out, cap) || iso_in(VITA_HOME, out, cap))       /* the memory card's ux0:data/rampagere */
        return 1;
#elif defined __ANDROID__
    if (vol_in(android_home(), out, cap) || iso_in(android_home(), out, cap))   /* the app folder (android.c) */
        return 1;
    if (android_apk()[0] && has_vol(android_apk())) {    /* a bundle (make_android_bundle.bat): the disc's files are in
                                                          * the installed APK itself, read in place */
        snprintf(out, cap, "%s", android_apk());
        return 1;
    }
#endif
    return vol_in("build" SEP "disc", out, cap) || vol_in(".", out, cap) || vol_in("data", out, cap) ||
           vol_in(exe, out, cap) || iso_in(exe, out, cap) || iso_in(".", out, cap) || iso_in("data", out, cap) ||
           vol_in(user, out, cap) || iso_in(user, out, cap) || remembered(out, cap);
}

const char *data_where(void)
{
#ifdef _WIN32
    return "build\\disc, the current folder, data\\, beside the program, %LOCALAPPDATA%\\RampageRE (and the disc image "
           "chosen at an earlier start)";
#elif defined __SWITCH__
    return SWITCH_HOME " (the disc image, or RAMPAGE.VOL with RAMPAGE.ELF or SLES_538.00)";
#elif defined __vita__
    return VITA_HOME " (the disc image, or RAMPAGE.VOL with RAMPAGE.ELF or SLES_538.00)";
#elif defined __ANDROID__
    return "the app folder, Android/data/io.github.jjmhalew.rampagere/files (the disc image, or RAMPAGE.VOL with "
           "RAMPAGE.ELF or SLES_538.00), and the APK itself (make_android_bundle.bat)";
#else
    return "build/disc, the current folder, data/, beside rampage, $XDG_DATA_HOME/RampageRE (~/.local/share/RampageRE; "
           "and the path in its disc.txt)";
#endif
}

/* SYSTEM.CNF: an image's own, else the one beside the VOL; malloc'd, NULL if none */
static char *data_cnf(const char *path, const Vol *v)
{
    uint32_t n = 0;
    char *cnf = NULL;
    FILE *f = v->base ? fopen(path, "rb") : NULL;
    if (f) {
        cnf = (char *)iso_slurp(f, "SYSTEM.CNF", &n);
        fclose(f);
    } else {
        char p[1024];
        beside(p, sizeof p, path, "SYSTEM.CNF");
        cnf = (char *)slurp(p, &n);
    }
    return cnf;
}

int data_region(const char *path, const Vol *v)
{
    char *cnf = data_cnf(path, v), boot[64];
    int region = !boot_name(cnf, boot, sizeof boot) ? 1 : strstr(boot, "SLUS") ? 0 : strstr(boot, "SLES") ? 1 : 2;
    free(cnf);
    return region;
}

int data_check(const char *path, const Vol *v, char *msg, size_t cap)
{
    char *cnf = data_cnf(path, v), boot[64] = "";
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
    /* the USA disc (SLUS_213.23, version 1.01) holds the same ELF and RAMPAGE.VOL, byte for byte; only its SYSTEM.CNF
     * (and the ELF's name) differ */
    int usa = boots && _stricmp(boot, "SLUS_213.23") == 0;
    if (boots && !usa && _stricmp(boot, "SLES_538.00") != 0) {
        snprintf(text, sizeof text, "SYSTEM.CNF boots %s", boot);
        why = text;
    } else if (boots && ver && strncmp(ver, usa ? "1.01" : "1.00", 4) != 0) {
        snprintf(text, sizeof text, "SYSTEM.CNF says version %.8s", ver);
        why = text;
    } else if (v->count != 352 || end != 1342827563u)
        why = "its RAMPAGE.VOL is not theirs";
    free(cnf);
    if (!why)
        return 1;
    snprintf(msg, cap, "%s is not a supported release, Rampage - Total Destruction (Europe) SLES-53800 version 1.00 "
             "or (USA) SLUS-21323 version 1.01: "
             "%s.\nThe viewer may not work with it.", path, why);
    return 0;
}

#if defined _WIN32 && !defined RAMPAGE_TOOL
/* ---- the first start on Windows: the disc image chosen in a file dialog (never a search of the drives), remembered */
#include <commdlg.h>

/* a wide path as the ANSI one fopen takes, else its short (8.3) name; 0 = neither spells it */
static int ansi_path(const wchar_t *w, char *out, size_t cap)
{
    wchar_t s[1024];
    BOOL bad = FALSE;
    int utf8 = GetACP() == CP_UTF8;                      /* (the UTF-8 code page: every name fits, the flags must be 0) */
    if (WideCharToMultiByte(CP_ACP, utf8 ? 0 : WC_NO_BEST_FIT_CHARS, w, -1, out, (int)cap, NULL, utf8 ? NULL : &bad) &&
        !bad)
        return 1;
    DWORD k = GetShortPathNameW(w, s, 1024);
    bad = FALSE;
    return k && k < 1024 && WideCharToMultiByte(CP_ACP, utf8 ? 0 : WC_NO_BEST_FIT_CHARS, s, -1, out, (int)cap, NULL,
                                                utf8 ? NULL : &bad) && !bad;
}

int data_ask(char *out, size_t cap)
{
    char where[1024], m[2048];
    disc_txt(where, sizeof where);
    snprintf(m, sizeof m, "RampageRE needs the files of your own game disc: Rampage - Total Destruction (Europe, "
             "PlayStation 2, SLES-53800).\n\nChoose your disc image of it (.iso) in the next window, or the RAMPAGE.VOL "
             "of a copy of the disc (with SLES_538.00 beside it). It is read where it is, nothing is copied, and "
             "RampageRE remembers it (in %s).\n\nLooked in: %s.", where[0] ? where : "disc.txt", data_where());
    if (MessageBoxA(NULL, m, "RampageRE", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) {
        plat_told = 1;
        return 0;
    }
    HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int ok = 0;
    for (;;) {
        wchar_t file[1024] = L"";
        OPENFILENAMEW o;
        memset(&o, 0, sizeof o);
        o.lStructSize = sizeof o;
        o.lpstrFilter = L"Disc image or RAMPAGE.VOL (*.iso, RAMPAGE.VOL)\0*.iso;RAMPAGE.VOL\0All files (*.*)\0*.*\0";
        o.lpstrFile = file;
        o.nMaxFile = 1024;
        o.lpstrTitle = L"Your Rampage - Total Destruction disc image (.iso), or its RAMPAGE.VOL";
        o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&o)) {
            plat_told = 1;                               /* cancelled: nothing more to say */
            break;
        }
        if (!ansi_path(file, out, cap)) {
            MessageBoxA(NULL, "RampageRE cannot open a file of that name (its letters do not fit this system's code "
                        "page): move or rename it.", "RampageRE", MB_ICONWARNING);
            continue;
        }
        if (!data_usable(out)) {
            snprintf(m, sizeof m, "%s is not the game's disc image (an ISO 9660 .iso with RAMPAGE.VOL in it) nor its "
                     "RAMPAGE.VOL.", out);
            MessageBoxA(NULL, m, "RampageRE", MB_ICONWARNING);
            continue;
        }
        if (!data_remember(out))
            printf("data: could not write %s; the disc image is asked for again at the next start\n", where);
        ok = 1;
        break;
    }
    if (SUCCEEDED(co))
        CoUninitialize();
    return ok;
}
#endif
