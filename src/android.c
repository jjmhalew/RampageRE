/* android.c - what the viewer needs on Android besides plat_sdl.c (port extra, docs/NATIVE.md "Android"; only in the
 * Android build, android/app/CMakeLists.txt): stdout and stderr to logcat, the app's own folder as the current
 * directory, and the first start's data setup. The app folder (Android/data/io.github.jjmhalew.rampagere/files on the
 * shared storage: a USB cable reaches it, no permission needed) holds rampage.cfg, rampage_profile.bin and the game's
 * data, where data_find looks first (iso.h). Content URIs of the system's file picker do not
 * stay readable across starts, so the picked disc image is not read in place as on a PC: RAMPAGE.VOL, the ELF that
 * SYSTEM.CNF boots (as RAMPAGE.ELF) and SYSTEM.CNF are copied out of it once (1.35 GB), after the free room is checked,
 * each as NAME.part renamed when its size is right, RAMPAGE.VOL last (the folder is complete once it is there). A folder
 * that already holds them is copied by RampageActivity.java. A disc image put into the app folder by hand is read in
 * place (data_find), nothing copied. A bundle of make_android_bundle.bat (tools/native/apkbundle.c) carries the disc's
 * files inside the APK (stored assets): data_find reads them in place from the installed APK (android_apk), so such an
 * app neither asks nor copies anything. */
#ifdef __ANDROID__
#include "plat.h"
#include "iso.h"
#include <SDL.h>
#include <jni.h>
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>

/* ---- stdout and stderr also to logcat: adb logcat -s RampageRE */
static int g_logpipe[2];
static void *log_pump(void *arg)
{
    (void)arg;
    char b[1024];
    size_t n = 0;
    ssize_t k;
    while ((k = read(g_logpipe[0], b + n, sizeof b - 1 - n)) > 0) {
        n += (size_t)k;
        b[n] = 0;
        char *s = b, *e;
        while ((e = strchr(s, '\n')) != NULL) {
            *e = 0;
            __android_log_write(ANDROID_LOG_INFO, "RampageRE", s);
            s = e + 1;
        }
        n = strlen(s);
        memmove(b, s, n + 1);
        if (n == sizeof b - 1) {                         /* a line longer than the buffer: in pieces */
            __android_log_write(ANDROID_LOG_INFO, "RampageRE", b);
            n = 0;
        }
    }
    return NULL;
}

static char g_home[1024], g_apk[1024];
const char *android_home(void) { return g_home; }
const char *android_apk(void) { return g_apk; }

/* the installed APK's path: Context.getPackageCodePath() (/data/app/.../base.apk, readable by the app itself) */
static void find_apk(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (!env || !act)
        return;
    jclass c = (*env)->GetObjectClass(env, act);
    jmethodID m = (*env)->GetMethodID(env, c, "getPackageCodePath", "()Ljava/lang/String;");
    jstring s = m ? (jstring)(*env)->CallObjectMethod(env, act, m) : NULL;
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        s = NULL;
    }
    const char *p = s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
    if (p) {
        snprintf(g_apk, sizeof g_apk, "%s", p);
        (*env)->ReleaseStringUTFChars(env, s, p);
    }
    if (s)
        (*env)->DeleteLocalRef(env, s);
    (*env)->DeleteLocalRef(env, c);
    (*env)->DeleteLocalRef(env, act);
}

void android_init(void)
{
    pthread_t t;
    if (pipe(g_logpipe) == 0) {
        dup2(g_logpipe[1], fileno(stdout));
        dup2(g_logpipe[1], fileno(stderr));
        setvbuf(stdout, NULL, _IOLBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        if (pthread_create(&t, NULL, log_pump, NULL) == 0)
            pthread_detach(t);
    }
    const char *home = SDL_AndroidGetExternalStoragePath();   /* getExternalFilesDir(null), made if not there */
    if (!home || chdir(home) != 0) {
        home = SDL_AndroidGetInternalStoragePath();
        if (home && chdir(home) != 0)
            home = NULL;
    }
    snprintf(g_home, sizeof g_home, "%s", home ? home : "");
    printf("app folder: %s\n", home ? home : "(none)");
    find_apk();
    printf("APK: %s\n", g_apk[0] ? g_apk : "(unknown)");
}

/* ---- RampageActivity's static methods */
static jclass activity_class(JNIEnv **env)
{
    *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject act = (jobject)SDL_AndroidGetActivity();
    if (!*env || !act)
        return NULL;
    jclass c = (**env)->GetObjectClass(*env, act);
    (**env)->DeleteLocalRef(*env, act);
    return c;
}

/* pickGameData(kind, dest): kind 1 = a disc image, returns its file descriptor; kind 2 = a folder, its game files copied
 * into dest by the Java side, returns 0. -1 = cancelled, -2 = failed, -3 = not enough room */
static int java_pick(int kind, const char *dest)
{
    JNIEnv *env;
    jclass c = activity_class(&env);
    int r = -2;
    if (!c)
        return -2;
    jmethodID m = (*env)->GetStaticMethodID(env, c, "pickGameData", "(ILjava/lang/String;)I");
    if (m) {
        jstring s = (*env)->NewStringUTF(env, dest ? dest : "");
        r = (*env)->CallStaticIntMethod(env, c, m, kind, s);
        (*env)->DeleteLocalRef(env, s);
    }
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        r = -2;
    }
    (*env)->DeleteLocalRef(env, c);
    return r;
}

static void progress(const char *text)                   /* progress(text): a dialog with it, NULL closes it */
{
    JNIEnv *env;
    jclass c = activity_class(&env);
    if (!c)
        return;
    jmethodID m = (*env)->GetStaticMethodID(env, c, "progress", "(Ljava/lang/String;)V");
    if (m) {
        jstring s = text ? (*env)->NewStringUTF(env, text) : NULL;
        (*env)->CallStaticVoidMethod(env, c, m, s);
        if (s)
            (*env)->DeleteLocalRef(env, s);
    }
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, c);
}

/* ---- the copy out of the disc image */
typedef struct { const char *name; int64_t at; uint32_t size; } Part;

static int copy_part(FILE *iso, const Part *p, uint8_t *buf, size_t cap, uint64_t *done, uint64_t total)
{
    char part[64];
    snprintf(part, sizeof part, "%s.part", p->name);
    FILE *o = fopen(part, "wb");
    int ok = o && _fseeki64(iso, p->at, SEEK_SET) == 0;
    for (uint32_t left = p->size; ok && left; ) {
        size_t n = left < cap ? left : cap;
        ok = fread(buf, 1, n, iso) == n && fwrite(buf, 1, n, o) == n;
        left -= (uint32_t)n;
        if ((*done >> 23) != ((*done + n) >> 23)) {   /* every 8 MB */
            char m[160];
            snprintf(m, sizeof m, "Copying the game files out of the disc image (once)...\n%u of %u MB",
                     (unsigned)((*done + n) >> 20), (unsigned)(total >> 20));
            progress(m);
        }
        *done += n;
    }
    if (o && fclose(o) != 0)
        ok = 0;
    struct stat st;
    ok = ok && stat(part, &st) == 0 && (uint64_t)st.st_size == p->size && rename(part, p->name) == 0;   /* its size */
    if (!ok)
        remove(part);
    printf("data: %s, %u bytes: %s\n", p->name, p->size, ok ? "copied" : "FAILED");
    return ok;
}

/* fd: the picked file (closed here). 1 = RAMPAGE.VOL and the rest are in the app folder */
static int iso_copy(int fd)
{
    FILE *f = fdopen(fd, "rb");
    char m[512];
    if (!f) {
        close(fd);
        plat_message("Could not open that file.", 1);
        return 0;
    }
    Part p[3] = { { "SYSTEM.CNF", 0, 0 }, { "RAMPAGE.ELF", 0, 0 }, { "RAMPAGE.VOL", 0, 0 } };   /* RAMPAGE.VOL last */
    struct stat fs;
    if (fstat(fd, &fs) == 0)
        printf("data: the picked file: %lld bytes%s\n", (long long)fs.st_size, S_ISREG(fs.st_mode) ? "" : " (not a plain file)");
    if (!iso_is(f)) {
        printf("data: no ISO 9660 volume descriptor read (%s)\n", strerror(errno));
        fclose(f);
        plat_message("That file is not a disc image (an ISO 9660 .iso with 2048-byte sectors), or it cannot be read "
                     "from where it is (copy it onto the device first).", 1);
        return 0;
    }
    if (!iso_find(f, "RAMPAGE.VOL", &p[2].at, &p[2].size)) {
        fclose(f);
        plat_message("That disc image holds no RAMPAGE.VOL: not Rampage - Total Destruction.", 1);
        return 0;
    }
    char boot[64] = "SLES_538.00";                       /* the ELF that SYSTEM.CNF boots (cdrom0:\SLES_538.00;1) */
    if (iso_find(f, "SYSTEM.CNF", &p[0].at, &p[0].size) && p[0].size < 4096) {
        char cnf[4096];
        const char *b = NULL;
        if (_fseeki64(f, p[0].at, SEEK_SET) == 0 && fread(cnf, 1, p[0].size, f) == p[0].size) {
            cnf[p[0].size] = 0;
            b = strstr(cnf, "cdrom0:");
        }
        if (b) {
            size_t n = 0;
            for (b += 7; *b == '\\' || *b == '/'; b++) { }
            while (b[n] && b[n] != ';' && b[n] != '\r' && b[n] != '\n' && n + 1 < sizeof boot) {
                boot[n] = b[n] == '\\' ? '/' : b[n];
                n++;
            }
            boot[n] = 0;
        }
    } else
        p[0].size = 0;
    if (!iso_find(f, boot, &p[1].at, &p[1].size))
        p[1].size = 0;
    uint64_t total = (uint64_t)p[0].size + p[1].size + p[2].size, done = 0;
    for (int i = 0; i < 3; i++) {                        /* what an earlier copy left */
        snprintf(m, sizeof m, "%s.part", p[i].name);
        remove(m);
    }
    long long kb = plat_free_kb("RAMPAGE.VOL");          /* the app folder (the current directory) */
    if (kb >= 0 && (uint64_t)kb < total / 1024 + 16 * 1024) {
        fclose(f);
        snprintf(m, sizeof m, "Not enough free room for the game files: they take %u MB, there are %lld MB free.",
                 (unsigned)(total >> 20), kb / 1024);
        plat_message(m, 1);
        return 0;
    }
    size_t cap = 4u << 20;
    uint8_t *buf = malloc(cap);
    int ok = buf != NULL;
    printf("data: copying %s, %s and RAMPAGE.VOL (%u MB) out of the disc image\n", p[0].size ? "SYSTEM.CNF" : "(no SYSTEM.CNF)",
           p[1].size ? boot : "(no ELF)", (unsigned)(total >> 20));
    for (int i = 0; ok && i < 3; i++)
        if (p[i].size)
            ok = copy_part(f, &p[i], buf, cap, &done, total);
    free(buf);
    fclose(f);
    progress(NULL);
    if (!ok)
        plat_message("Could not copy the game files out of the disc image. Is there room on the device, and is the "
                     "image complete?", 1);
    else if (!p[1].size) {
        snprintf(m, sizeof m, "The disc image holds no %s: the viewer runs without the effects' words.", boot);
        plat_message(m, 1);
    }
    return ok;
}

int android_data_setup(char *out, size_t cap)
{
    char home[1024] = "", m[2048];
    if (!getcwd(home, sizeof home))
        snprintf(home, sizeof home, "the app's folder");
    for (;;) {
        snprintf(m, sizeof m, "RampageRE needs the files of your own game disc: Rampage - Total Destruction (Europe, "
                 "PlayStation 2, SLES-53800, or USA, SLUS-21323).\n\nChoose a disc image of it (.iso), or a folder "
                 "with RAMPAGE.VOL and the game's ELF (SLES_538.00, SLUS_213.23 or RAMPAGE.ELF). They are copied once (1.35 GB) into the app's folder.\n\n"
                 "Or copy them, or the disc image (which is then read where it is), with a USB cable into\n%s\nand "
                 "start RampageRE again.", home);
        int r = plat_dialog(m, "Disc image", "Folder", "Cancel");
        if (r < 0)
            return 0;
        if (r == 1) {
            int fd = java_pick(1, NULL);
            if (fd == -2)
                plat_message("Could not open that file.", 1);
            if (fd < 0 || !iso_copy(fd))
                continue;
        } else {
            int k = java_pick(2, home);
            if (k == -3)
                plat_message("Not enough free room on the device for the game files (1.35 GB).", 1);
            else if (k == -2)
                plat_message("Could not copy the game files: that folder needs RAMPAGE.VOL (and SLES_538.00, "
                             "SLUS_213.23 or RAMPAGE.ELF) in it.", 1);
            if (k < 0)
                continue;
            if (!plat_exists("RAMPAGE.ELF") && !plat_exists("SLES_538.00") && !plat_exists("SLUS_213.23"))
                plat_message("That folder holds no SLES_538.00, SLUS_213.23 or RAMPAGE.ELF: the viewer runs without "
                             "the effects' words.", 1);
        }
        if (data_find(out, cap))
            return 1;
        plat_message("The game files are still not there.", 1);
    }
}
#endif
