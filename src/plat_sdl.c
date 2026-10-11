/* plat_sdl.c - the window, keyboard, mouse and OS bits of render_gl.h / plat.h on SDL2, for the builds outside Windows
 * (Linux: build.sh), after WoodyRE's. The Windows build keeps its own Win32 + WGL window in render_gl.c. Keys
 * arrive as SDL scancodes and are stored as the Windows virtual-key codes the rest uses: letters by the layout (as VK
 * letters are on Windows), everything else by position; Ctrl / Shift / Alt set both their side and the plain code.
 * A hidden window (--shot) is never shown and draws into a framebuffer object; headless, SDL_VIDEODRIVER=offscreen.
 * Built with GLES=1 the context is OpenGL ES 3.0 (else 2.0) and the renderer draws through src/gles's shim. The
 * Switch (build_switch.sh, __SWITCH__) is that ES build: the whole screen at 1280x720, messages in the system's error
 * dialog, its folder sdmc:/switch/rampagere the current one and stdout / stderr in rampage.log there. Android
 * (android/, __ANDROID__) too: the whole screen in landscape, messages in RampageActivity's dialog, the back button as
 * Esc, and touch.c's on-screen pad fed every event and drawn over every frame. */
#ifndef _WIN32
#include "plat.h"
#undef fopen
#include "render_gl.h"
#ifdef __ANDROID__
#include "touch.h"                               /* the on-screen pad: every event goes by it, it draws over the frame */
#define TOUCH_EVENT(e, w) touch_event((e), (w)->width, (w)->height)
#define TOUCH_DRAW(w) touch_draw((w)->width, (w)->height)
#else
#define TOUCH_EVENT(e, w) ((void)0)
#define TOUCH_DRAW(w) ((void)0)
#endif
#include <SDL.h>
#include <GL/gl.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

/* ---- files */
int plat_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }

/* path with \ as / and every part matched ignoring case against what is on disk; 0 = some part is not there */
static int resolve(const char *in, char *out, size_t cap)
{
    char buf[1024];
    size_t n = strlen(in);
    if (n >= sizeof buf || n + 1 > cap)
        return 0;
    for (size_t i = 0; i <= n; i++)
        buf[i] = in[i] == '\\' ? '/' : in[i];
    size_t o = 0;
    out[0] = 0;
    char *p = buf;
    if (*p == '/') { out[o++] = '/'; out[o] = 0; while (*p == '/') p++; }
    while (*p) {
        char *e = strchr(p, '/');
        if (e) *e = 0;
        char test[2048];
        snprintf(test, sizeof test, "%s%s", out, p);
        if (!strcmp(p, ".") || !strcmp(p, "..") || plat_exists(test))
            o += (size_t)snprintf(out + o, cap - o, "%s", p);
        else {
            DIR *d = opendir(o ? out : ".");
            struct dirent *de;
            int found = 0;
            if (d) {
                while (!found && (de = readdir(d)))
                    if (!strcasecmp(de->d_name, p)) {
                        o += (size_t)snprintf(out + o, cap - o, "%s", de->d_name);
                        found = 1;
                    }
                closedir(d);
            }
            if (!found)
                return 0;
        }
        if (o >= cap - 1)
            return 0;
        if (!e)
            break;
        out[o++] = '/';
        out[o] = 0;
        p = e + 1;
        while (*p == '/') p++;
    }
    return 1;
}

FILE *plat_fopen(const char *path, const char *mode)
{
    FILE *f = fopen(path, mode);
    if (f || mode[0] != 'r' || errno != ENOENT)
        return f;
    char real[1024];
    return resolve(path, real, sizeof real) ? fopen(real, mode) : NULL;
}

#ifdef RAMPAGE_GLES                             /* the shim of src/gles (GLES=1): the entry points it emulates or has */
void (*plat_gl_proc(const char *name))(void) { return gles_proc(name); }   /* (eglGetProcAddress may hand out stubs) */
#else
void (*plat_gl_proc(const char *name))(void) { return (void (*)(void))SDL_GL_GetProcAddress(name); }
#endif

static int g_hidden;                             /* the window is hidden (--shot): no boxes either */

#ifdef __SWITCH__
#include <switch/applets/error.h>                /* not all of switch.h: its PadState is not pad.h's */
#include <switch/services/applet.h>
/* SDL has no message box on the Switch: the system's error dialog shows the text with one button */
void plat_message(const char *text, int warn)
{
    (void)warn;
    fprintf(stderr, "%s\n", text);               /* rampage.log */
    if (!plat_quiet && !g_hidden) {
        ErrorApplicationConfig c;
        errorApplicationCreate(&c, text, NULL);
        errorApplicationShow(&c);
    }
}

void plat_switch_home(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(SWITCH_HOME, 0777);
    if (chdir(SWITCH_HOME) != 0)
        return;
    FILE *f = fopen("rampage.log", "w");         /* a new log each start; both streams append to it */
    if (!f)
        return;
    fclose(f);
    if (freopen("rampage.log", "a", stdout))
        setvbuf(stdout, NULL, _IOLBF, 0);
    if (freopen("rampage.log", "a", stderr))
        setvbuf(stderr, NULL, _IONBF, 0);
    AppletType t = appletGetAppletType();
    if (t != AppletType_Application && t != AppletType_SystemApplication)   /* the Album: about 400 MB */
        printf("started as an applet, with less memory: hold R while starting a game for all of it\n");
}
#elif defined __ANDROID__
#include <jni.h>
/* RampageActivity.dialog: a scrolling message with up to three buttons that stay on the screen (SDL's message box does
 * not scroll, and in landscape its buttons end up below a phone's screen). Blocks until one is pressed. */
int plat_dialog(const char *text, const char *b1, const char *b2, const char *b3)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    int r = -1;
    if (!env || !act) { fprintf(stderr, "%s\n", text); return -1; }
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetStaticMethodID(env, c, "dialog",
                                            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)I");
    if (m) {
        jstring s[4] = { (*env)->NewStringUTF(env, text), b1 ? (*env)->NewStringUTF(env, b1) : NULL,
                         b2 ? (*env)->NewStringUTF(env, b2) : NULL, b3 ? (*env)->NewStringUTF(env, b3) : NULL };
        r = (*env)->CallStaticIntMethod(env, c, m, s[0], s[1], s[2], s[3]);
        for (int i = 0; i < 4; i++) if (s[i]) (*env)->DeleteLocalRef(env, s[i]);
    }
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); r = -1; }
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
    return r;
}

void plat_message(const char *text, int warn)    /* always in logcat too (stderr, android.c) */
{
    (void)warn;
    fprintf(stderr, "%s\n", text);
    if (!plat_quiet && !g_hidden)
        plat_dialog(text, "OK", NULL, NULL);
}
#else
void plat_message(const char *text, int warn)
{
    if (plat_quiet || g_hidden ||
        SDL_ShowSimpleMessageBox(warn ? SDL_MESSAGEBOX_WARNING : SDL_MESSAGEBOX_INFORMATION, "RampageRE", text, NULL))
        fprintf(stderr, "%s\n", text);
}
#endif

/* ---- window */
static int vk_of(const SDL_Keysym *k)
{
    if (k->sym >= 'a' && k->sym <= 'z') return k->sym - 'a' + 'A';   /* letters follow the layout, like VK_A..VK_Z */
    int s = k->scancode;
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return 'A' + s - SDL_SCANCODE_A;
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_9) return '1' + s - SDL_SCANCODE_1;
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F12) return VK_F1 + s - SDL_SCANCODE_F1;
    if (s >= SDL_SCANCODE_F13 && s <= SDL_SCANCODE_F24) return VK_F1 + 12 + s - SDL_SCANCODE_F13;
    if (s >= SDL_SCANCODE_KP_1 && s <= SDL_SCANCODE_KP_9) return VK_NUMPAD1 + s - SDL_SCANCODE_KP_1;
    switch (s) {
    case SDL_SCANCODE_0: return '0';                 case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
    case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;      case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB: return VK_TAB;            case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;    case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
    case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;  case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
    case SDL_SCANCODE_BACKSLASH: case SDL_SCANCODE_NONUSHASH: return VK_OEM_5;
    case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;    case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
    case SDL_SCANCODE_GRAVE: return VK_OEM_3;        case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
    case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;  case SDL_SCANCODE_SLASH: return VK_OEM_2;
    case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;   case SDL_SCANCODE_PRINTSCREEN: return VK_SNAPSHOT;
    case SDL_SCANCODE_SCROLLLOCK: return VK_SCROLL;  case SDL_SCANCODE_PAUSE: return VK_PAUSE;
    case SDL_SCANCODE_INSERT: return VK_INSERT;      case SDL_SCANCODE_HOME: return VK_HOME;
    case SDL_SCANCODE_PAGEUP: return VK_PRIOR;       case SDL_SCANCODE_DELETE: return VK_DELETE;
    case SDL_SCANCODE_END: return VK_END;            case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
    case SDL_SCANCODE_RIGHT: return VK_RIGHT;        case SDL_SCANCODE_LEFT: return VK_LEFT;
    case SDL_SCANCODE_DOWN: return VK_DOWN;          case SDL_SCANCODE_UP: return VK_UP;
    case SDL_SCANCODE_NUMLOCKCLEAR: return VK_NUMLOCK; case SDL_SCANCODE_KP_DIVIDE: return VK_DIVIDE;
    case SDL_SCANCODE_KP_MULTIPLY: return VK_MULTIPLY; case SDL_SCANCODE_KP_MINUS: return VK_SUBTRACT;
    case SDL_SCANCODE_KP_PLUS: return VK_ADD;        case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
    case SDL_SCANCODE_KP_PERIOD: return VK_DECIMAL;  case SDL_SCANCODE_NONUSBACKSLASH: return VK_OEM_102;
    case SDL_SCANCODE_APPLICATION: return VK_APPS;
    case SDL_SCANCODE_AC_BACK: return VK_ESCAPE;     /* Android's back button / gesture (main.c: back, the Pause menu) */
    case SDL_SCANCODE_LCTRL: return VK_LCONTROL;     case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
    case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;      case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
    case SDL_SCANCODE_LALT: return VK_LMENU;         case SDL_SCANCODE_RALT: return VK_RMENU;
    case SDL_SCANCODE_LGUI: return VK_LWIN;          case SDL_SCANCODE_RGUI: return VK_RWIN;
    default: return 0;
    }
}

static void drawable(Window *w)
{
    int dw, dh;
    SDL_GL_GetDrawableSize((SDL_Window *)w->hwnd, &dw, &dh);
    if (dw > 0 && dh > 0) { w->width = dw; w->height = dh; }
}

int win_open(Window *w, const char *title, int width, int height, int hidden)
{
    memset(w, 0, sizeof *w);
    g_hidden = hidden;
#ifdef __ANDROID__                               /* the whole screen in landscape; the fingers are touch.c's pad */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
    SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
#endif
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return -1; }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1); SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
#ifdef RAMPAGE_GLES                             /* OpenGL ES 3.0, else 2.0; src/gles emulates the fixed function on it */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    /* SDL's default context is a compatibility one: the renderer is GL 1.1's fixed function */
    Uint32 flags = SDL_WINDOW_OPENGL | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
#ifdef __SWITCH__
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");   /* no mouse: the touch screen is not one */
    if (!hidden) {                               /* the whole screen, docked (scaled up) or handheld */
        flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN;
        width = 1280;
        height = 720;
    }
#elif defined __ANDROID__
    if (!hidden)
        flags |= SDL_WINDOW_FULLSCREEN;
#endif
    SDL_Window *sw = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
#ifdef RAMPAGE_GLES
    SDL_GLContext gc = sw ? SDL_GL_CreateContext(sw) : NULL;
    if (!gc) {                                   /* no ES 3.0 (no config for its window, or no context): ES 2.0 */
        if (sw) SDL_DestroyWindow(sw);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        sw = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
        gc = sw ? SDL_GL_CreateContext(sw) : NULL;
    }
    if (!sw) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); SDL_QuitSubSystem(SDL_INIT_VIDEO); return -1; }
#else
    if (!sw) { fprintf(stderr, "SDL window: %s\n", SDL_GetError()); SDL_QuitSubSystem(SDL_INIT_VIDEO); return -1; }
    SDL_GLContext gc = SDL_GL_CreateContext(sw);
#endif
    if (!gc) {
        fprintf(stderr, "SDL OpenGL: %s\n", SDL_GetError());
        SDL_DestroyWindow(sw);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return -1;
    }
    w->hwnd = sw; w->hglrc = gc;
    w->width = width; w->height = height;
    w->hidden = hidden;
    SDL_GL_SetSwapInterval(hidden ? 0 : 1);
    if (hidden) {                                /* never shown: without a framebuffer object, no window at all */
        if (gl_offscreen(width, height) != 0) {
            fprintf(stderr, "no OpenGL framebuffer object for an offscreen frame\n");
            win_close(w);
            return -1;
        }
    } else {
        w->focused = 1;
        drawable(w);
    }
    printf("OpenGL: %s / %s (SDL %d.%d.%d, %s)\n", (const char *)glGetString(GL_RENDERER),
           (const char *)glGetString(GL_VERSION), SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL,
           SDL_GetCurrentVideoDriver());
    return 0;
}

static void set_key(Window *w, int vk, int down)
{
    if (down && !w->keys[vk])
        w->pressed[vk] = 1;
    w->keys[vk] = down;
}

/* the plain Ctrl / Shift / Alt, as Windows reports them, from their sides */
static void plain_keys(Window *w)
{
    set_key(w, VK_CONTROL, w->keys[VK_LCONTROL] || w->keys[VK_RCONTROL]);
    set_key(w, VK_SHIFT, w->keys[VK_LSHIFT] || w->keys[VK_RSHIFT]);
    set_key(w, VK_MENU, w->keys[VK_LMENU] || w->keys[VK_RMENU]);
}

static unsigned char g_down_now[256], g_up_late[256];   /* a key pressed and released within one poll stays down a frame */
void win_poll(Window *w)
{
    memset(w->pressed, 0, sizeof w->pressed);
    w->mouse_dx = w->mouse_dy = w->wheel = 0;
    for (int k = 0; k < 256; k++) {
        if (g_up_late[k]) { w->keys[k] = 0; g_up_late[k] = 0; }
        g_down_now[k] = 0;
    }
    plain_keys(w);
    SDL_Event e;
    while (SDL_PollEvent(&e)) switch (TOUCH_EVENT(&e, w), e.type) {
    case SDL_QUIT: w->quit = 1; break;
    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || e.window.event == SDL_WINDOWEVENT_RESIZED) {
            if (!g_hidden)                       /* a hidden window's frame is its framebuffer object's size */
                drawable(w);
        } else if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
            w->focused = 1;
        else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            w->focused = 0;
            memset(w->keys, 0, sizeof w->keys);
            memset(g_up_late, 0, sizeof g_up_late);
            w->mouse_right = 0;
        } else if (e.window.event == SDL_WINDOWEVENT_CLOSE)
            w->quit = 1;
        break;
    case SDL_KEYDOWN: case SDL_KEYUP: {
        int vk = vk_of(&e.key.keysym), down = e.type == SDL_KEYDOWN;
        if (!vk || e.key.repeat)
            break;
        if (!down && g_down_now[vk]) { g_up_late[vk] = 1; break; }   /* the game polls once a frame: let it see the tap */
        if (down) { g_down_now[vk] = 1; g_up_late[vk] = 0; }
        set_key(w, vk, down);
        plain_keys(w);
        break; }
    case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_RIGHT) w->mouse_right = 1; break;
    case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_RIGHT) w->mouse_right = 0; break;
    case SDL_MOUSEMOTION:
        if (w->mouse_right) { w->mouse_dx += e.motion.xrel; w->mouse_dy += e.motion.yrel; }
        break;
    case SDL_MOUSEWHEEL:
        w->wheel += e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
        break;
    case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED: case SDL_JOYDEVICEADDED:
    case SDL_JOYDEVICEREMOVED:
        w->dev_changes++;
        break;
    }
}

void win_swap(Window *w) { TOUCH_DRAW(w); SDL_GL_SwapWindow((SDL_Window *)w->hwnd); }
void win_title(Window *w, const char *title) { SDL_SetWindowTitle((SDL_Window *)w->hwnd, title); }

/* port extra (WoodyRE's): fullscreen on the desktop's resolution, or a window centred and shrunk to fit */
void win_mode(Window *w, int width, int height, int full)
{
    SDL_Window *sw = (SDL_Window *)w->hwnd;
    if (w->hidden)                               /* --shot: never shown, its size is the framebuffer object's */
        return;
#if defined __SWITCH__ || defined __ANDROID__
    (void)width; (void)height; (void)full; (void)sw;
    drawable(w);                                 /* always the whole screen */
    return;
#endif
    if (full) {
        SDL_SetWindowFullscreen(sw, SDL_WINDOW_FULLSCREEN_DESKTOP);
        drawable(w);
        return;
    }
    SDL_SetWindowFullscreen(sw, 0);
    SDL_Rect r;
    int d = SDL_GetWindowDisplayIndex(sw);
    if (!SDL_GetDisplayUsableBounds(d < 0 ? 0 : d, &r) && (width > r.w || height > r.h)) {
        float s = (float)r.w / width < (float)r.h / height ? (float)r.w / width : (float)r.h / height;
        printf("window %dx%d does not fit the screen: %dx%d\n", width, height, (int)(width * s), (int)(height * s));
        width = (int)(width * s);
        height = (int)(height * s);
    }
    SDL_SetWindowSize(sw, width, height);
    SDL_SetWindowPosition(sw, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    w->width = width;
    w->height = height;
    drawable(w);
}

int win_vsync(Window *w, int interval) { return !w->hidden && SDL_GL_SetSwapInterval(interval) == 0 ? 0 : -1; }

void win_close(Window *w)
{
    if (w->hglrc) SDL_GL_DeleteContext((SDL_GLContext)w->hglrc);
    if (w->hwnd) SDL_DestroyWindow((SDL_Window *)w->hwnd);
    w->hglrc = w->hwnd = NULL;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

double win_time(void)
{
    static double freq;
    if (!freq)
        freq = (double)SDL_GetPerformanceFrequency();
    return (double)SDL_GetPerformanceCounter() / freq;
}
#endif
