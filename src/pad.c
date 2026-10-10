/* pad.c - game controllers on Windows (pad.h; WoodyRE's src/pad.c with a slot per pad).
 *
 * Sony DualShock 4 and DualSense (Edge), over USB and Bluetooth, read as raw HID with hid.dll / setupapi: no driver, no
 * Steam, no DS4Windows. The report layouts follow SDL's hidapi drivers (SDL_hidapi_ps4.c / _ps5.c) and Linux's
 * hid-playstation:
 * - DualSense input 0x01 over USB (64 B) and 0x31 over Bluetooth (78 B, CRC-32 with the hidp header 0xa1): sticks, triggers,
 *   counter, three button bytes. Over Bluetooth the pad sends the short "simple" 0x01 report (DS4 layout) until a feature
 *   report is read: 0x09 (serial) and 0x20 (firmware) switch it to 0x31, which rumble needs.
 * - DualShock 4 input 0x01 (USB, and the short Bluetooth one) and 0x11..0x19 (Bluetooth, data from byte 3; feature 0x05
 *   switches to them).
 * - Rumble / light: DualSense output 0x02 (USB, 48 B) or 0x31 (Bluetooth, 78 B: 0x00, tag 0x10, the 47-byte effects block,
 *   CRC-32 with header 0xa2); DualShock 4 output 0x05 (USB, 32 B) or 0x11 (Bluetooth, 78 B with CRC).
 * Xbox pads and everything that speaks XInput (most other pads, Steam Input, DS4Windows): xinput1_4.dll (or 1_3 / 9_1_0),
 * loaded at run time, the four slots; ordinal 100 (XInputGetStateEx) also gives the Guide button. An empty slot is slow to
 * ask, so those are only tried every 2 s and when Windows reports a device change.
 * Steam Input and DS4Windows show a Sony pad twice (its HID device and an XInput one), which would be two players here:
 * while a Sony pad is open, XInput is not read (RAMPAGE_PADXINPUT=1 reads both). Light: the player's colour on the light
 * bar (PlayStation's blue, red, green, pink) and the DualSense's player lights (RAMPAGE_PADNOLIGHT=1 leaves them be). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "pad.h"

static const char *penv(const char *n) { const char *v = getenv(n); return v && *v ? v : NULL; }
static double pad_clock(void)
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c); return (double)c.QuadPart / (double)f.QuadPart;
}
static uint32_t crc32_of(uint32_t crc, const unsigned char *p, size_t n)   /* zlib's CRC-32 (reflected 0xedb88320) */
{
    crc = ~crc;
    while (n--) { crc ^= *p++; for (int k = 0; k < 8; k++) crc = crc >> 1 ^ (0xedb88320u & (0u - (crc & 1))); }
    return ~crc;
}
static uint32_t bt_crc(unsigned char hdr, const unsigned char *p, size_t n) { return crc32_of(crc32_of(0, &hdr, 1), p, n); }

/* ---- slots: a pad takes the lowest free one and keeps it until it goes ---------------------------------------------- */
static unsigned g_slots;
static int slot_take(void)
{
    for (int k = 0; k < PAD_SLOTS; k++) if (!(g_slots >> k & 1)) { g_slots |= 1u << k; return k; }
    return -1;
}
static void slot_free(int k) { if (k >= 0) g_slots &= ~(1u << k); }
static const unsigned char COLOUR[PAD_SLOTS][3] = { { 0x00, 0x00, 0x40 }, { 0x40, 0x00, 0x00 }, { 0x00, 0x40, 0x00 }, { 0x20, 0x00, 0x20 } };
static const unsigned char PLAYER_LEDS[PAD_SLOTS] = { 0x04, 0x0a, 0x15, 0x1b };   /* the DualSense's five lights */

/* ---- rumble: per slot up to 8 running effects, the strongest one is what the motor gets ----------------------------- */
static struct { float s[PAD_SLOTS][8]; double until[PAD_SLOTS][8]; float scale; int log; } g_rum = { .scale = 1.0f, .log = -1 };
void pad_set_strength(float s) { g_rum.scale = s < 0 ? 0 : s > 1 ? 1 : s; }
void pad_rumble(int slot, float strength, float seconds)
{
    if (slot < 0 || slot >= PAD_SLOTS) return;
    if (g_rum.log < 0) g_rum.log = penv("RAMPAGE_PADLOG") != NULL;
    if (g_rum.log) printf("pad: slot %d rumble %.2f for %.2f s (x %.2f)\n", slot, strength, seconds, g_rum.scale);
    double now = pad_clock(); int k = 0;
    for (int i = 1; i < 8; i++) if (g_rum.until[slot][i] < g_rum.until[slot][k]) k = i;   /* the one that ends first makes room */
    g_rum.s[slot][k] = strength; g_rum.until[slot][k] = now + seconds;
}
static int rum_level(int slot, double now)       /* 0 .. 255 */
{
    float m = 0;
    if (slot < 0) return 0;
    for (int i = 0; i < 8; i++) if (now < g_rum.until[slot][i] && g_rum.s[slot][i] > m) m = g_rum.s[slot][i];
    return (int)(m * g_rum.scale * 255.0f + 0.5f);
}

/* ---- Sony over HID -------------------------------------------------------------------------------------------------- */
#define SONY_VID 0x054c
#define IOCTL_HID_GET_FEATURE_ 0xb0192          /* hidclass.h: HID_OUT_CTL_CODE(100) */
typedef struct {
    HANDLE h; WCHAR path[300]; int kind, bt, rumble2, full, seen; unsigned pid, fw;
    DWORD in_len, out_len, feat_len;
    OVERLAPPED rov, wov; int reading, writing;
    unsigned char rb[1024], wb[1024], fb[1024];
    PadState st;
    int slot;                                    /* its player slot, -1 none (all taken) */
    int sent;                                    /* the motor value last sent, -1 = nothing yet */
    int led;                                     /* the light: 0 not set yet, 1 set */
    uint32_t stamp;                              /* DualSense sensor clock of the last full report (the Bluetooth light waits for it) */
    unsigned bad_crc;
} Sony;
static Sony g_sony[4];

static const char *sony_name(const Sony *d) { return d->kind == PADK_DS5 ? (d->pid == 0x0df2 ? "DualSense Edge" : "DualSense") : "DualShock 4"; }
const char *pad_kind_name(int kind) { return kind == PADK_DS5 ? "DualSense" : kind == PADK_DS4 ? "DualShock 4" : kind == PADK_XBOX ? "Xbox controller" : kind == PADK_SWITCH ? "Switch controller" : "none"; }

static int sony_feature(Sony *d, int id)         /* reads feature report id into d->fb; the byte count, 0 on failure */
{
    DWORD len = d->feat_len, n = 0; if (!len || len > sizeof d->fb) return 0;
    memset(d->fb, 0, len); d->fb[0] = (unsigned char)id;
    OVERLAPPED ov = {0}; ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BOOL ok = DeviceIoControl(d->h, IOCTL_HID_GET_FEATURE_, d->fb, len, d->fb, len, &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 500) == WAIT_OBJECT_0) ok = GetOverlappedResult(d->h, &ov, &n, FALSE);
        else { CancelIo(d->h); GetOverlappedResult(d->h, &ov, &n, TRUE); ok = FALSE; }
    }
    CloseHandle(ov.hEvent);
    return ok ? (int)n : 0;
}
static void sony_free(Sony *d)
{
    if (d->h) {
        CancelIo(d->h); DWORD n;
        if (d->reading) GetOverlappedResult(d->h, &d->rov, &n, TRUE);
        if (d->writing) GetOverlappedResult(d->h, &d->wov, &n, TRUE);
        CloseHandle(d->h); slot_free(d->slot);
    }
    if (d->rov.hEvent) CloseHandle(d->rov.hEvent);
    if (d->wov.hEvent) CloseHandle(d->wov.hEvent);
    memset(d, 0, sizeof *d);
}
static int sony_read_start(Sony *d)
{
    ResetEvent(d->rov.hEvent);
    if (ReadFile(d->h, d->rb, d->in_len, NULL, &d->rov) || GetLastError() == ERROR_IO_PENDING) { d->reading = 1; return 1; }
    d->reading = 0; return 0;
}
static void sony_open(const WCHAR *path)
{
    Sony *d = NULL;
    for (int i = 0; i < 4; i++) if (!g_sony[i].h) { d = &g_sony[i]; break; }
    if (!d) return;
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    HIDD_ATTRIBUTES at = { sizeof at }; PHIDP_PREPARSED_DATA pp = NULL; HIDP_CAPS caps;
    int kind = 0;
    if (HidD_GetAttributes(h, &at) && at.VendorID == SONY_VID) {
        unsigned p = at.ProductID;
        kind = p == 0x0ce6 || p == 0x0df2 ? PADK_DS5 : p == 0x05c4 || p == 0x09cc || p == 0x0ba0 ? PADK_DS4 : 0;
    }
    if (kind && HidD_GetPreparsedData(h, &pp)) {
        if (HidP_GetCaps(pp, &caps) != HIDP_STATUS_SUCCESS || caps.UsagePage != 1 || (caps.Usage != 4 && caps.Usage != 5)) kind = 0;   /* the game pad collection */
        HidD_FreePreparsedData(pp);
    } else kind = 0;
    if (!kind || caps.InputReportByteLength > sizeof d->rb || caps.OutputReportByteLength > sizeof d->wb) { CloseHandle(h); return; }
    memset(d, 0, sizeof *d);
    d->h = h; wcsncpy(d->path, path, 299); d->kind = kind; d->pid = at.ProductID; d->seen = 1; d->sent = -1; d->slot = -1;
    d->in_len = caps.InputReportByteLength; d->out_len = caps.OutputReportByteLength; d->feat_len = caps.FeatureReportByteLength;
    d->bt = d->in_len > 64;                      /* USB: 64; Bluetooth: 78 (DualSense) / 547 (DualShock 4) */
    d->rov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL); d->wov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (kind == PADK_DS5) {
        sony_feature(d, 0x09);                   /* serial: also switches Bluetooth to the full 0x31 reports */
        if (sony_feature(d, 0x20) >= 46) d->fw = d->fb[44] | d->fb[45] << 8;
        d->rumble2 = d->pid == 0x0df2 || d->fw == 0 || d->fw >= 0x0224;   /* "improved rumble emulation" of firmware 2.24 on (SDL) */
    } else if (d->bt) sony_feature(d, 0x05);     /* DualShock 4: the Bluetooth calibration switches to the 0x11 reports */
    d->full = !d->bt;                            /* USB is full from the start; Bluetooth once the first full report came in */
    if (!sony_read_start(d)) { sony_free(d); return; }
    printf("pad: %s connected (%s)\n", sony_name(d), d->bt ? "Bluetooth" : "USB");
    if (kind == PADK_DS5 && d->fw) printf("pad: firmware %x.%02x, %s rumble\n", d->fw >> 8, d->fw & 0xff, d->rumble2 ? "improved" : "classic");
}
static void sony_scan(void)
{
    GUID g; HidD_GetHidGuid(&g);
    HDEVINFO di = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (di == INVALID_HANDLE_VALUE) return;
    for (int i = 0; i < 4; i++) g_sony[i].seen = 0;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
    for (DWORD k = 0; SetupDiEnumDeviceInterfaces(di, NULL, &g, k, &ifd); k++) {
        union { SP_DEVICE_INTERFACE_DETAIL_DATA_W d; char b[sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) + 600 * sizeof(WCHAR)]; } u;
        u.d.cbSize = sizeof u.d;
        if (!SetupDiGetDeviceInterfaceDetailW(di, &ifd, &u.d, sizeof u, NULL, NULL)) continue;
        WCHAR low[300]; size_t n = wcslen(u.d.DevicePath); if (n >= 300) continue;
        for (size_t i = 0; i <= n; i++) low[i] = towlower(u.d.DevicePath[i]);
        if (!wcsstr(low, L"054c")) continue;     /* Sony's vendor id is in the path (USB "vid_054c", Bluetooth "vid&0002054c"): no need to open the rest */
        int have = 0;
        for (int i = 0; i < 4; i++) if (g_sony[i].h && !_wcsicmp(g_sony[i].path, u.d.DevicePath)) { g_sony[i].seen = have = 1; }
        if (!have) sony_open(u.d.DevicePath);
    }
    SetupDiDestroyDeviceInfoList(di);
    for (int i = 0; i < 4; i++) if (g_sony[i].h && !g_sony[i].seen) { printf("pad: %s disconnected\n", sony_name(&g_sony[i])); sony_free(&g_sony[i]); }
}
static float axis(unsigned char v) { float f = ((float)v - 128.0f) / 127.0f; return f < -1 ? -1 : f > 1 ? 1 : f; }
static void sony_state(Sony *d, const unsigned char *s, int full)   /* s = the packet from LX on; full = the DualSense's long layout */
{
    unsigned b0, b1, b2, l, r;
    if (full) { l = s[4]; r = s[5]; b0 = s[7]; b1 = s[8]; b2 = s[9]; d->stamp = s[27] | s[28] << 8 | s[29] << 16 | (uint32_t)s[30] << 24; }
    else { b0 = s[4]; b1 = s[5]; b2 = s[6]; l = s[7]; r = s[8]; }
    static const uint32_t hat[9] = { 1u << PAD_UP, 1u << PAD_UP | 1u << PAD_RIGHT, 1u << PAD_RIGHT, 1u << PAD_DOWN | 1u << PAD_RIGHT, 1u << PAD_DOWN,
                                     1u << PAD_DOWN | 1u << PAD_LEFT, 1u << PAD_LEFT, 1u << PAD_UP | 1u << PAD_LEFT, 0 };
    uint32_t m = hat[(b0 & 15) < 8 ? b0 & 15 : 8];
    if (b0 & 0x10) m |= 1u << PAD_X;    if (b0 & 0x20) m |= 1u << PAD_A;    if (b0 & 0x40) m |= 1u << PAD_B;     if (b0 & 0x80) m |= 1u << PAD_Y;
    if (b1 & 0x01) m |= 1u << PAD_LB;   if (b1 & 0x02) m |= 1u << PAD_RB;   if (b1 & 0x10) m |= 1u << PAD_BACK;  if (b1 & 0x20) m |= 1u << PAD_START;
    if (b1 & 0x40) m |= 1u << PAD_LS;   if (b1 & 0x80) m |= 1u << PAD_RS;   if (b2 & 0x01) m |= 1u << PAD_GUIDE; if (b2 & 0x02) m |= 1u << PAD_TOUCH;
    PadState *st = &d->st;
    st->lt = l / 255.0f; st->rt = r / 255.0f;
    if (st->lt >= 0.25f || (!l && (b1 & 0x04))) m |= 1u << PAD_LT;   /* the digital bit alone: a pad without analog triggers (SDL) */
    if (st->rt >= 0.25f || (!r && (b1 & 0x08))) m |= 1u << PAD_RT;
    st->lx = axis(s[0]); st->ly = axis(s[1]); st->rx = axis(s[2]); st->ry = axis(s[3]);
    st->kind = d->kind;
    st->buttons = m;
}
static void sony_report(Sony *d, const unsigned char *r, DWORD n)
{
    if (d->kind == PADK_DS5) {
        if (r[0] == 0x01 && n >= 10) sony_state(d, r + 1, !d->bt);     /* Bluetooth 0x01 = the simple report */
        else if (r[0] == 0x31 && n >= 78) {
            uint32_t c = r[74] | r[75] << 8 | r[76] << 16 | (uint32_t)r[77] << 24;
            if (bt_crc(0xa1, r, 74) != c) { if (!d->bad_crc++) puts("pad: DualSense Bluetooth report with a bad CRC (ignored)"); return; }
            if (!d->full) { d->full = 1; puts("pad: DualSense full reports on"); }
            sony_state(d, r + 2, 1);
        }
    } else {
        if (r[0] == 0x01 && n >= 10) sony_state(d, r + 1, 0);
        else if (r[0] >= 0x11 && r[0] <= 0x19 && n >= 12) { if (!d->full) d->full = 1; sony_state(d, r + 3, 0); }
    }
}
static void sony_pump(Sony *d)                   /* every report that came in since the last frame; the last one counts */
{
    for (int guard = 0; d->h && d->reading && guard < 64; guard++) {
        DWORD n = 0;
        if (!GetOverlappedResult(d->h, &d->rov, &n, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) return;
            printf("pad: %s disconnected\n", sony_name(d)); sony_free(d); return;
        }
        d->reading = 0;
        if (n) sony_report(d, d->rb, n);
        if (!sony_read_start(d)) { printf("pad: %s disconnected\n", sony_name(d)); sony_free(d); return; }
    }
}
/* effects: the big motor at v (0 .. 255), the small one off, and light 1 = the slot's colour (and the DualSense's player
 * lights), 2 = the DualSense's light given back to the system */
static int sony_write(Sony *d, int v, int light)
{
    if (d->writing) {
        DWORD n; if (!GetOverlappedResult(d->h, &d->wov, &n, FALSE) && GetLastError() == ERROR_IO_INCOMPLETE) return 0;   /* the last one is still on its way */
        d->writing = 0;
    }
    unsigned char *w = d->wb; DWORD size; memset(w, 0, sizeof d->wb);
    const unsigned char *rgb = COLOUR[d->slot >= 0 ? d->slot : 0];
    if (d->kind == PADK_DS5) {
        unsigned char *e;
        if (d->bt) { w[0] = 0x31; w[1] = 0x00; w[2] = 0x10; e = w + 3; size = 78; } else { w[0] = 0x02; e = w + 1; size = 48; }
        /* the motor bits always go along, also with 0: then the motors stop for sure */
        if (d->rumble2) { e[38] |= 0x04; e[3] = (unsigned char)v; }   /* +38 valid flag 2: improved rumble; +3 left (low), +2 right (high) */
        else { e[0] |= 0x01; e[3] = (unsigned char)(v >> 1); }       /* classic emulation, halved like SDL */
        e[0] |= 0x02;                                                  /* no audio haptics */
        if (light == 1) {                                              /* the colour, the player lights */
            e[1] |= 0x04 | 0x10; e[44] = rgb[0]; e[45] = rgb[1]; e[46] = rgb[2];
            e[43] = (unsigned char)(PLAYER_LEDS[d->slot >= 0 ? d->slot : 0] | 0x20);
        }
        if (light == 2) e[1] |= 0x08;                                  /* reset the light */
    } else {
        unsigned char *e, *flags;
        if (d->bt) { w[0] = 0x11; w[1] = 0xc4; w[3] = 0x01; e = w + 6; flags = w + 3; size = 78; } else { w[0] = 0x05; w[1] = 0x01; e = w + 4; flags = w + 1; size = 32; }
        e[0] = 0; e[1] = (unsigned char)v;                             /* right (weak), left (strong) */
        if (light == 1) { *flags |= 0x02; e[2] = rgb[0]; e[3] = rgb[1]; e[4] = rgb[2]; }
    }
    if (d->bt) { uint32_t c = bt_crc(0xa2, w, size - 4); w[size - 4] = (unsigned char)c; w[size - 3] = (unsigned char)(c >> 8); w[size - 2] = (unsigned char)(c >> 16); w[size - 1] = (unsigned char)(c >> 24); }
    DWORD len = d->out_len > size ? d->out_len : size;   /* Windows wants the longest output report's length */
    ResetEvent(d->wov.hEvent);
    if (WriteFile(d->h, w, len, NULL, &d->wov) || GetLastError() == ERROR_IO_PENDING) { d->writing = 1; return 1; }
    static int warned; if (!warned++) printf("pad: %s: output report failed (error %lu)\n", sony_name(d), GetLastError());
    return -1;
}

/* ---- XInput -------------------------------------------------------------------------------------------------------- */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XGamepad;       /* XINPUT_GAMEPAD */
typedef struct { DWORD packet; XGamepad pad; } XState;                              /* XINPUT_STATE */
typedef struct { WORD left, right; } XVibration;                                    /* XINPUT_VIBRATION: low / high frequency motor */
typedef DWORD (WINAPI *XGetState)(DWORD, XState *);
typedef DWORD (WINAPI *XSetState)(DWORD, XVibration *);
static struct {
    int tried; XGetState get; XSetState set;
    int on[4], slot[4]; double next_try[4]; int sent[4]; PadState st[4];
} g_xi;
static void xi_load(void)
{
    g_xi.tried = 1;
    static const char *dll[3] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !g_xi.get; i++) {
        HMODULE m = LoadLibraryA(dll[i]); if (!m) continue;
        g_xi.get = (XGetState)(void (*)(void))GetProcAddress(m, (LPCSTR)100);   /* XInputGetStateEx: with the Guide button (not in 9_1_0) */
        if (!g_xi.get) g_xi.get = (XGetState)(void (*)(void))GetProcAddress(m, "XInputGetState");
        g_xi.set = (XSetState)(void (*)(void))GetProcAddress(m, "XInputSetState");
        if (!g_xi.get || !g_xi.set) { g_xi.get = NULL; g_xi.set = NULL; FreeLibrary(m); }
    }
}
static void xi_off(int i)                        /* gone, or not read: motors off, the slot free */
{
    if (g_xi.on[i] && g_xi.sent[i] > 0) { XVibration z = { 0, 0 }; g_xi.set((DWORD)i, &z); }
    g_xi.on[i] = 0; slot_free(g_xi.slot[i]); g_xi.slot[i] = -1; memset(&g_xi.st[i], 0, sizeof g_xi.st[i]);
}
static float xaxis(SHORT v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }
static void xi_poll(int i, double now, int force)   /* force: look at an empty slot now */
{
    if (!g_xi.on[i] && !force && now < g_xi.next_try[i]) return;
    XState x; memset(&x, 0, sizeof x);
    if (g_xi.get((DWORD)i, &x) != ERROR_SUCCESS) {
        if (g_xi.on[i]) printf("pad: Xbox controller %d disconnected\n", i + 1);
        xi_off(i); g_xi.next_try[i] = now + 2.0; return;
    }
    if (!g_xi.on[i]) { g_xi.on[i] = 1; g_xi.sent[i] = -1; printf("pad: Xbox controller %d connected (XInput)\n", i + 1); }
    static const struct { WORD bit; int b; } map[15] = { {0x0001,PAD_UP}, {0x0002,PAD_DOWN}, {0x0004,PAD_LEFT}, {0x0008,PAD_RIGHT}, {0x0010,PAD_START},
        {0x0020,PAD_BACK}, {0x0040,PAD_LS}, {0x0080,PAD_RS}, {0x0100,PAD_LB}, {0x0200,PAD_RB}, {0x0400,PAD_GUIDE},
        {0x1000,PAD_A}, {0x2000,PAD_B}, {0x4000,PAD_X}, {0x8000,PAD_Y} };
    PadState *st = &g_xi.st[i]; uint32_t m = 0;
    for (int k = 0; k < 15; k++) if (x.pad.buttons & map[k].bit) m |= 1u << map[k].b;
    st->lt = x.pad.lt / 255.0f; st->rt = x.pad.rt / 255.0f;
    if (st->lt >= 0.25f) m |= 1u << PAD_LT;
    if (st->rt >= 0.25f) m |= 1u << PAD_RT;
    st->lx = xaxis(x.pad.lx); st->ly = -xaxis(x.pad.ly); st->rx = xaxis(x.pad.rx); st->ry = -xaxis(x.pad.ry);   /* XInput: y up = + */
    st->kind = PADK_XBOX;
    st->buttons = m;
}
static void xi_rumble(int i, int v)
{
    if (!g_xi.on[i] || v == g_xi.sent[i]) return;
    XVibration vib = { (WORD)(v * 257), 0 };     /* the big (low-frequency) motor */
    if (g_xi.set((DWORD)i, &vib) == ERROR_SUCCESS) g_xi.sent[i] = v;
}

/* ---- all pads ------------------------------------------------------------------------------------------------------- */
static unsigned g_dev_seen; static double g_scan_at[2] = { 0, -1 }; static int g_inited;
void pad_close(void)
{
    for (int i = 0; i < 4; i++) if (g_xi.set && g_xi.on[i] && g_xi.sent[i] > 0) { XVibration z = { 0, 0 }; g_xi.set((DWORD)i, &z); g_xi.sent[i] = 0; }
    for (int i = 0; i < 4; i++) {
        Sony *d = &g_sony[i]; if (!d->h) continue;
        if (d->full && (d->sent > 0 || (d->led && d->kind == PADK_DS5))) {
            double end = pad_clock() + 0.2; int r;
            while ((r = sony_write(d, 0, d->led && d->kind == PADK_DS5 ? 2 : 0)) == 0 && pad_clock() < end) Sleep(1);
            if (r > 0) WaitForSingleObject(d->wov.hEvent, 200);
        }
        sony_free(d);
    }
}
unsigned pad_poll(PadState st[PAD_SLOTS], int focused, unsigned devchanges)
{
    double now = pad_clock();
    if (!g_inited) {
        g_inited = 1; g_dev_seen = devchanges; atexit(pad_close);
        for (int i = 0; i < 4; i++) g_xi.slot[i] = -1;
    }
    if (devchanges != g_dev_seen) { g_dev_seen = devchanges; g_scan_at[0] = now + 0.5; g_scan_at[1] = now + 2.0; }   /* the device may need a moment: look twice */
    int scan = 0;
    for (int k = 0; k < 2; k++) if (g_scan_at[k] >= 0 && now >= g_scan_at[k]) { g_scan_at[k] = -1; sony_scan(); scan = 1; }
    memset(st, 0, sizeof *st * PAD_SLOTS);
    unsigned on = 0; int sony = 0;
    for (int i = 0; i < 4; i++) { sony_pump(&g_sony[i]); sony += g_sony[i].h != NULL; }
    if (!g_xi.tried) xi_load();
    int xinput = !sony || penv("RAMPAGE_PADXINPUT");
    for (int i = 0; g_xi.get && !xinput && i < 4; i++) {   /* a Sony pad's XInput twin, likely: not a player of its own */
        if (g_xi.on[i]) xi_off(i);
        g_xi.next_try[i] = 0;
    }
    for (int i = 0; i < 4; i++) {
        Sony *d = &g_sony[i]; if (!d->h) continue;
        if (d->slot < 0 && (d->slot = slot_take()) >= 0) { d->led = 0; printf("pad: %s is player %d's\n", sony_name(d), d->slot + 1); }
        if (d->slot < 0) continue;
        on |= 1u << d->slot;
        if (focused) st[d->slot] = d->st;
        st[d->slot].kind = d->kind;
        if (!d->full) continue;
        /* the light: right away over USB; over Bluetooth only once the pad's own connect animation is over (SDL waits
         * for its sensor clock to pass 10200000), or the pad overrides it */
        int light = !d->led && (d->kind == PADK_DS4 || !d->bt || d->stamp >= 10200000u) && !penv("RAMPAGE_PADNOLIGHT") ? 1 : 0;
        int lvl = focused ? rum_level(d->slot, now) : 0;
        if (lvl != d->sent || light) { if (sony_write(d, lvl, light) > 0) { d->sent = lvl; if (light) d->led = 1; } }
    }
    for (int i = 0; g_xi.get && xinput && i < 4; i++) {
        xi_poll(i, now, scan); if (!g_xi.on[i]) continue;
        if (g_xi.slot[i] < 0 && (g_xi.slot[i] = slot_take()) >= 0) printf("pad: Xbox controller %d is player %d's\n", i + 1, g_xi.slot[i] + 1);
        if (g_xi.slot[i] < 0) continue;
        on |= 1u << g_xi.slot[i];
        if (focused) st[g_xi.slot[i]] = g_xi.st[i];
        st[g_xi.slot[i]].kind = PADK_XBOX;
        xi_rumble(i, focused ? rum_level(g_xi.slot[i], now) : 0);
    }
    return on;
}
