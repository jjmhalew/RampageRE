/* wiifiles.h - the Wii's files for the packers (docs/WII_PLAN.md): pack.c (the standalone exe) and apkbundle.c (the
 * Android bundle) take the files of Game, Shell and Audio of the player's extracted Wii disc (the folder with DATA, its
 * DATA\files, or the folder with Game in it) as wii/Game/NAME ... Include after <windows.h>, <stdio.h>, <stdlib.h>,
 * <string.h> and a die(fmt, ...) that does not return. */
#ifndef RAMPAGE_WIIFILES_H
#define RAMPAGE_WIIFILES_H

typedef struct { wchar_t path[MAX_PATH * 2]; char rel[112]; uint32_t size; } WiiFile;   /* rel: wii/Game/NAME */
static WiiFile *g_wii;
static unsigned g_nwii;

static int wii_is_dir(const wchar_t *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* 1 = dir holds the Wii's Game (or DATA/files/Game, files/Game): its files listed into g_wii */
static int wii_collect(const wchar_t *dir)
{
    static const wchar_t *const UNDER[] = { L"", L"\\DATA\\files", L"\\files" };
    static const wchar_t *const SUB[] = { L"Game", L"Shell", L"Audio" };
    wchar_t root[MAX_PATH * 2], p[MAX_PATH * 2];
    for (int k = 0; k < 3; k++) {
        _snwprintf(root, MAX_PATH * 2, L"%ls%ls", dir, UNDER[k]);
        root[MAX_PATH * 2 - 1] = 0;
        _snwprintf(p, MAX_PATH * 2, L"%ls\\Game", root);
        p[MAX_PATH * 2 - 1] = 0;
        if (!wii_is_dir(p))
            continue;
        for (int i = 0; i < 3; i++) {
            WIN32_FIND_DATAW fd;
            _snwprintf(p, MAX_PATH * 2, L"%ls\\%ls\\*", root, SUB[i]);
            p[MAX_PATH * 2 - 1] = 0;
            HANDLE h = FindFirstFileW(p, &fd);
            if (h == INVALID_HANDLE_VALUE)
                continue;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                if (fd.nFileSizeHigh)
                    die("%ls is 4 GB or more: not the Wii's", fd.cFileName);
                WiiFile *w = realloc(g_wii, sizeof *g_wii * (g_nwii + 1));
                if (!w)
                    die("out of memory");
                g_wii = w;
                w += g_nwii++;
                _snwprintf(w->path, MAX_PATH * 2, L"%ls\\%ls\\%ls", root, SUB[i], fd.cFileName);
                w->path[MAX_PATH * 2 - 1] = 0;
                if (snprintf(w->rel, sizeof w->rel, "wii/%ls/%ls", SUB[i], fd.cFileName) >= (int)sizeof w->rel)
                    die("%ls: too long a name", fd.cFileName);
                for (const char *c = w->rel; *c; c++)
                    if ((unsigned char)*c < 32 || (unsigned char)*c > 126)
                        die("%ls: not a Wii file name (letters outside ASCII)", fd.cFileName);
                w->size = fd.nFileSizeLow;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        return 1;
    }
    return 0;
}

/* "auto": RAMPAGE_WII_DATA, else rampage.cfg's wii_data= here (src/settings.c reads them so), else a wii folder */
static void wii_auto(wchar_t *out, size_t cap)
{
    char v[1024] = "", line[1200];
    const char *env = getenv("RAMPAGE_WII_DATA");
    FILE *f = env && env[0] ? NULL : fopen("rampage.cfg", "r");
    if (env && env[0])
        snprintf(v, sizeof v, "%s", env);
    while (f && fgets(line, sizeof line, f)) {
        char *k = line, *e;
        while (*k == ' ' || *k == '\t') k++;
        if (_strnicmp(k, "wii_data", 8) != 0)
            continue;
        for (k += 8; *k == ' ' || *k == '\t'; k++) { }
        if (*k++ != '=')
            continue;
        while (*k == ' ' || *k == '\t') k++;
        if ((e = strchr(k, '#')) != NULL) *e = 0;
        e = k + strlen(k);
        while (e > k && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        snprintf(v, sizeof v, "%s", k);
    }
    if (f)
        fclose(f);
    if (!v[0] || !MultiByteToWideChar(CP_ACP, 0, v, -1, out, (int)cap))
        _snwprintf(out, cap, L"wii");
    out[cap - 1] = 0;
}

/* arg: the extracted Wii disc, "auto" (wii_auto) or "" (none): its files into g_wii, said; a folder that is not the
 * Wii's dies, "auto" without one goes on without */
static void wii_take(const wchar_t *arg)
{
    if (!arg[0])
        return;
    wchar_t wd[MAX_PATH * 2];
    int any = wcscmp(arg, L"auto") == 0;
    if (any)
        wii_auto(wd, MAX_PATH * 2);
    else
        _snwprintf(wd, MAX_PATH * 2, L"%ls", arg);
    wd[MAX_PATH * 2 - 1] = 0;
    if (wii_collect(wd)) {
        uint64_t n = 0;
        for (unsigned i = 0; i < g_nwii; i++)
            n += g_wii[i].size;
        printf("Wii data: %ls, %u files, %u MB\n", wd, g_nwii, (unsigned)(n >> 20));
    } else if (!any)
        die("%ls holds no Game folder (nor DATA\\files\\Game): not your extracted Wii disc", wd);
    else
        printf("(no Wii data: give your extracted Wii disc for its monsters and Dallas)\n");
}
#endif
