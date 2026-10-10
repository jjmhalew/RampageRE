/* plat.h - what the viewer takes from the operating system outside the window, as in WoodyRE: on Windows <windows.h>
 * itself (the window: render_gl.c), elsewhere the same names on SDL2 (src/plat_sdl.c), so the rest keeps one spelling:
 * the Windows virtual-key codes (Window.keys / pressed), Sleep, the case-insensitive compares, and fopen made
 * case-insensitive for reading, with \ taken as / (the default build\disc\RAMPAGE.VOL). */
#ifndef RAMPAGE_PLAT_H
#define RAMPAGE_PLAT_H
extern int plat_quiet;                           /* set by --shot: plat_message only prints (stderr), never a box */
#ifdef _WIN32
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define plat_gl_proc(name) ((void (*)(void))wglGetProcAddress(name))
#define plat_exists(path) (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
static inline void plat_message(const char *text, int warn)
{
    if (plat_quiet)
        fprintf(stderr, "%s\n", text);
    else
        MessageBoxA(NULL, text, "RampageRE", warn ? MB_ICONWARNING : MB_ICONINFORMATION);
}
/* the free room in KB on the drive that holds path's folder (the current drive for a bare name), -1 unknown */
static inline long long plat_free_kb(const char *path)
{
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", path ? path : ".");
    char *cut = strrchr(dir, '\\'), *cut2 = strrchr(dir, '/');
    if (cut2 > cut) cut = cut2;
    if (cut) cut[1] = 0;
    else snprintf(dir, sizeof dir, ".\\");
    ULARGE_INTEGER avail;
    return GetDiskFreeSpaceExA(dir, &avail, NULL, NULL) ? (long long)(avail.QuadPart / 1024) : -1;
}
#else
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/statvfs.h>
#ifndef APIENTRY
#define APIENTRY
#endif
#define WINAPI
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
static inline void Sleep(unsigned ms) { usleep(ms * 1000u); }
#define _fseeki64 fseeko                         /* 64-bit offsets (RAMPAGE.VOL is 1.34 GB): -D_FILE_OFFSET_BITS=64 */
#define _ftelli64 ftello
FILE *plat_fopen(const char *path, const char *mode);   /* fopen; to read, a path not there is looked up per part
                                                         * ignoring case, \ as / */
#define fopen plat_fopen
int plat_exists(const char *path);
void (*plat_gl_proc(const char *name))(void);
void plat_message(const char *text, int warn);
/* the free room in KB on the file system that holds path's folder, -1 unknown */
static inline long long plat_free_kb(const char *path)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s", path ? path : ".");
    char *cut = strrchr(dir, '/'), *cut2 = strrchr(dir, '\\');
    if (cut2 > cut) cut = cut2;
    if (cut) cut[1] = 0;
    else snprintf(dir, sizeof dir, ".");
    for (char *c = dir; *c; c++)
        if (*c == '\\') *c = '/';
    struct statvfs st;
    return statvfs(dir, &st) == 0 ? (long long)((unsigned long long)st.f_bavail * st.f_frsize / 1024) : -1;
}
enum {                                           /* the Windows virtual-key codes (winuser.h) */
    VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_PAUSE = 0x13,
    VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24,
    VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_SNAPSHOT = 0x2C, VK_INSERT = 0x2D, VK_DELETE = 0x2E,
    VK_LWIN = 0x5B, VK_RWIN = 0x5C, VK_APPS = 0x5D, VK_NUMPAD0 = 0x60, VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD4,
    VK_NUMPAD5, VK_NUMPAD6, VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, VK_MULTIPLY = 0x6A, VK_ADD = 0x6B, VK_SUBTRACT = 0x6D,
    VK_DECIMAL = 0x6E, VK_DIVIDE = 0x6F, VK_F1 = 0x70, VK_F2, VK_F3, VK_F4, VK_F5, VK_F6, VK_F7, VK_F8, VK_F9, VK_F10,
    VK_F11, VK_F12, VK_F24 = 0x87, VK_NUMLOCK = 0x90, VK_SCROLL = 0x91, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1,
    VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3, VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_OEM_1 = 0xBA, VK_OEM_PLUS = 0xBB,
    VK_OEM_COMMA = 0xBC, VK_OEM_MINUS = 0xBD, VK_OEM_PERIOD = 0xBE, VK_OEM_2 = 0xBF, VK_OEM_3 = 0xC0, VK_OEM_4 = 0xDB,
    VK_OEM_5 = 0xDC, VK_OEM_6 = 0xDD, VK_OEM_7 = 0xDE, VK_OEM_102 = 0xE2
};
#endif
#endif
