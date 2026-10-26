/* apkbundle.c - makes the one-file Android build for your own use (make_android_bundle.bat): the app's APK + the game data
 * of YOUR disc that the port reads, as the assets assets/SYSTEM.CNF, assets/SLES_538.00 (the ELF SYSTEM.CNF boots) and
 * assets/RAMPAGE.VOL (1.34 GB). After WoodyRE's tools/native/apkbundle.c, which adds a whole CD image.
 * Usage: apkbundle <in.apk> <data> <out.apk> <key file> <certificate name> [wii]
 *   data: your disc image (.iso), or a RAMPAGE.VOL with SLES_538.00 (or RAMPAGE.ELF) and SYSTEM.CNF beside it, or "auto":
 *   the data the viewer finds by itself (src/iso.c data_find)
 *   wii: your extracted Wii disc (the folder with DATA, its DATA/files, or the folder with Game in it), or "auto":
 *   RAMPAGE_WII_DATA, else rampage.cfg's wii_data= (the current folder's), else a wii folder here; none without it.
 *   The files of its Game, Shell and Audio (docs/WII_PLAN.md) go in as assets/wii/Game/..., stored and aligned as the
 *   rest, which the app reads in place (src/vol.c vol_add_wii, src/main.c); about 320 MB more
 * Every entry of the APK stays byte for byte at its offset (so the 16 KB alignment of the native libraries holds); the
 * game's files are added behind them stored, not compressed, and 4096-byte aligned (zipalign's 0xd935 extra field), so the
 * app reads them in place in the installed APK: src/iso.c takes such an APK as a disc image (zip_find) and data_find
 * looks at the app's own APK (android_apk), so the app neither asks for the disc nor copies anything. Only the files the
 * port reads go in, not the whole disc image (up to 4.7 GB): the APK stays below 2 GB, inside every 32-bit ZIP field
 * (no ZIP64, which older Android versions cannot read) and clear of signed 32-bit offsets.
 * The APK's signature no longer fits, so the result is signed again with APK Signature Scheme v2 (Android 7+, the app's
 * minSdk 24; v3 adds only key rotation, which a key of your own does not need) under a key of your own: an RSA key made
 * with Windows' CNG at the first run and kept in <key file>, so a later bundle installs as an update of this one. Only
 * Windows' own libraries are used. The result holds the game's data: it is for your own devices only, never share it.
 * Build (build.bat apkbundle): zig cc -std=c99 -O2 -DRAMPAGE_TOOL -Isrc -o apkbundle.exe tools/native/apkbundle.c
 * src/iso.c -lbcrypt -lshell32 */
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plat.h"
#include "iso.h"

#define MAX_APK 0x7fffffffull                       /* the result stays below 2 GB */

static const wchar_t *g_part;                       /* the half-written output, removed when something fails */
static void die(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    fputs("apkbundle: ", stderr);
    vfprintf(stderr, fmt, a);
    fputc('\n', stderr);
    va_end(a);
    if (g_part)
        _wremove(g_part);
    exit(1);
}

typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void put(Buf *b, const void *d, size_t n)
{
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->p = realloc(b->p, b->cap);
        if (!b->p)
            die("out of memory");
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
static void put8(Buf *b, unsigned v) { uint8_t c = (uint8_t)v; put(b, &c, 1); }
static void put16(Buf *b, unsigned v) { put8(b, v); put8(b, v >> 8); }
static void put32(Buf *b, uint32_t v) { put16(b, v & 0xffff); put16(b, v >> 16); }
static void put64(Buf *b, uint64_t v) { put32(b, (uint32_t)v); put32(b, (uint32_t)(v >> 32)); }
static void put_lp(Buf *b, const Buf *in) { put32(b, (uint32_t)in->n); put(b, in->p, in->n); }   /* length-prefixed (v2) */
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static uint8_t *read_all(const wchar_t *path, size_t *n)
{
    FILE *f = _wfopen(path, L"rb");
    if (!f)
        return NULL;
    _fseeki64(f, 0, SEEK_END);
    long long sz = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    uint8_t *b = sz > 0 && sz < (1ll << 31) ? malloc((size_t)sz) : NULL;
    if (b && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        free(b);
        b = NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

/* ---- SHA-256 and RSA through Windows' CNG ---- */
static BCRYPT_ALG_HANDLE g_sha, g_rsa;
static void sha256(const void *a, size_t an, const void *b, size_t bn, uint8_t out[32])
{
    BCRYPT_HASH_HANDLE h;
    if (BCryptCreateHash(g_sha, &h, NULL, 0, NULL, 0, 0) || BCryptHashData(h, (PUCHAR)a, (ULONG)an, 0) ||
        (bn && BCryptHashData(h, (PUCHAR)b, (ULONG)bn, 0)) || BCryptFinishHash(h, out, 32, 0))
        die("SHA-256 failed");
    BCryptDestroyHash(h);
}
static Buf rsa_sign(BCRYPT_KEY_HANDLE k, const void *d, size_t n)   /* RSASSA-PKCS1-v1_5 with SHA-256 */
{
    uint8_t hash[32];
    sha256(d, n, NULL, 0, hash);
    BCRYPT_PKCS1_PADDING_INFO pad = { BCRYPT_SHA256_ALGORITHM };
    ULONG len = 0;
    Buf s = { 0 };
    if (BCryptSignHash(k, &pad, hash, 32, NULL, 0, &len, BCRYPT_PAD_PKCS1))
        die("RSA signing failed");
    s.p = malloc(len);
    s.cap = len;
    if (!s.p || BCryptSignHash(k, &pad, hash, 32, s.p, len, &len, BCRYPT_PAD_PKCS1))
        die("RSA signing failed");
    s.n = len;
    return s;
}

/* ---- DER for the self-signed X.509 certificate (the APK signature carries one; Android only compares its key) ---- */
static void der_len(Buf *b, size_t n)
{
    if (n < 0x80)
        put8(b, (unsigned)n);
    else if (n < 0x100) {
        put8(b, 0x81);
        put8(b, (unsigned)n);
    } else if (n < 0x10000) {
        put8(b, 0x82);
        put8(b, (unsigned)(n >> 8));
        put8(b, (unsigned)n);
    } else {
        put8(b, 0x83);
        put8(b, (unsigned)(n >> 16));
        put8(b, (unsigned)(n >> 8));
        put8(b, (unsigned)n);
    }
}
static void der(Buf *b, int tag, const void *d, size_t n) { put8(b, tag); der_len(b, n); put(b, d, n); }
static void wrap(Buf *b, int tag) { Buf h = { 0 }; der(&h, tag, b->p, b->n); free(b->p); *b = h; }
static void der_uint(Buf *b, const uint8_t *v, size_t n)      /* a big-endian unsigned number */
{
    while (n > 1 && !*v) {
        v++;
        n--;
    }
    Buf t = { 0 };
    if (*v & 0x80)
        put8(&t, 0);
    put(&t, v, n);
    der(b, 0x02, t.p, t.n);
    free(t.p);
}
static const uint8_t k_sha256rsa[] = { 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x0b, 0x05, 0x00 };
static const uint8_t k_rsa[] = { 0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00 };

static Buf spki(const uint8_t *blob)                        /* SubjectPublicKeyInfo of a BCRYPT_RSAFULLPRIVATE_BLOB */
{
    const BCRYPT_RSAKEY_BLOB *h = (const BCRYPT_RSAKEY_BLOB *)blob;
    const uint8_t *e = blob + sizeof *h;
    Buf k = { 0 };
    der_uint(&k, e + h->cbPublicExp, h->cbModulus);
    der_uint(&k, e, h->cbPublicExp);
    wrap(&k, 0x30);
    Buf bits = { 0 };
    put8(&bits, 0);
    put(&bits, k.p, k.n);
    wrap(&bits, 0x03);
    free(k.p);
    Buf s = { 0 };
    put(&s, k_rsa, sizeof k_rsa);
    put(&s, bits.p, bits.n);
    wrap(&s, 0x30);
    free(bits.p);
    return s;
}
static Buf make_cert(BCRYPT_KEY_HANDLE key, const uint8_t *blob, const char *cn)
{
    Buf name = { 0 };
    put(&name, "\x06\x03\x55\x04\x03", 5);                     /* CN=<cn> */
    der(&name, 0x0c, cn, strlen(cn));
    wrap(&name, 0x30);
    wrap(&name, 0x31);
    wrap(&name, 0x30);
    time_t now = time(NULL);
    struct tm t = *gmtime(&now);
    char from[16], to[20];
    strftime(from, sizeof from, "%y%m%d%H%M%SZ", &t);
    t.tm_year += 50;
    strftime(to, sizeof to, "%Y%m%d%H%M%SZ", &t);
    Buf valid = { 0 };
    der(&valid, 0x17, from, strlen(from));
    der(&valid, 0x18, to, strlen(to));
    wrap(&valid, 0x30);
    uint8_t serial[8];
    BCryptGenRandom(NULL, serial, sizeof serial, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    serial[0] = (serial[0] & 0x7f) | 0x01;
    Buf pub = spki(blob), tbs = { 0 };
    put(&tbs, "\xa0\x03\x02\x01\x02", 5);                       /* version 3 */
    der_uint(&tbs, serial, sizeof serial);
    put(&tbs, k_sha256rsa, sizeof k_sha256rsa);
    put(&tbs, name.p, name.n);
    put(&tbs, valid.p, valid.n);
    put(&tbs, name.p, name.n);
    put(&tbs, pub.p, pub.n);
    wrap(&tbs, 0x30);
    Buf sig = rsa_sign(key, tbs.p, tbs.n), bits = { 0 };
    put8(&bits, 0);
    put(&bits, sig.p, sig.n);
    wrap(&bits, 0x03);
    Buf c = { 0 };
    put(&c, tbs.p, tbs.n);
    put(&c, k_sha256rsa, sizeof k_sha256rsa);
    put(&c, bits.p, bits.n);
    wrap(&c, 0x30);
    free(name.p); free(valid.p); free(pub.p); free(tbs.p); free(sig.p); free(bits.p);
    return c;
}

/* the key file: "APKBKEY1", u32 length + BCRYPT_RSAFULLPRIVATE_BLOB, u32 length + certificate (DER); WoodyRE's format */
static BCRYPT_KEY_HANDLE load_key(const wchar_t *path, const char *cn, Buf *blob, Buf *cert)
{
    BCRYPT_KEY_HANDLE k = NULL;
    size_t n;
    uint8_t *f = read_all(path, &n);
    if (f) {
        uint32_t bl = n >= 12 ? rd32(f + 8) : 0, cl = n >= 16ull + bl ? rd32(f + 12 + bl) : 0;
        if (n < 16 || memcmp(f, "APKBKEY1", 8) || 16ull + bl + cl != n)
            die("%ls is not a key file of this tool", path);
        blob->n = 0;
        put(blob, f + 12, bl);
        cert->n = 0;
        put(cert, f + 16 + bl, cl);
        free(f);
        if (BCryptImportKeyPair(g_rsa, NULL, BCRYPT_RSAFULLPRIVATE_BLOB, &k, blob->p, (ULONG)blob->n, 0))
            die("cannot read the key in %ls", path);
        return k;
    }
    printf("Making your signing key (once): %ls\n", path);
    ULONG len = 0;
    if (BCryptGenerateKeyPair(g_rsa, &k, 3072, 0) || BCryptFinalizeKeyPair(k, 0) ||
        BCryptExportKey(k, NULL, BCRYPT_RSAFULLPRIVATE_BLOB, NULL, 0, &len, 0))
        die("cannot make an RSA key");
    blob->p = malloc(len);
    blob->cap = len;
    if (!blob->p || BCryptExportKey(k, NULL, BCRYPT_RSAFULLPRIVATE_BLOB, blob->p, len, &len, 0))
        die("cannot make an RSA key");
    blob->n = len;
    *cert = make_cert(k, blob->p, cn);
    wchar_t dir[MAX_PATH];                                     /* its folder, one level is enough */
    wcsncpy(dir, path, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    wchar_t *s = wcsrchr(dir, L'\\'), *s2 = wcsrchr(dir, L'/');
    if (s2 > s)
        s = s2;
    if (s) {
        *s = 0;
        CreateDirectoryW(dir, NULL);
    }
    Buf o = { 0 };
    put(&o, "APKBKEY1", 8);
    put32(&o, (uint32_t)blob->n);
    put(&o, blob->p, blob->n);
    put32(&o, (uint32_t)cert->n);
    put(&o, cert->p, cert->n);
    FILE *w = _wfopen(path, L"wb");
    if (!w || fwrite(o.p, 1, o.n, w) != o.n || fclose(w))
        die("cannot write the key file %ls", path);
    free(o.p);
    return k;
}

/* ---- ZIP ---- */
static uint32_t g_crc[256];
static uint32_t crc32_add(uint32_t c, const uint8_t *p, size_t n)
{
    if (!g_crc[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++)
                v = v & 1 ? 0xedb88320u ^ v >> 1 : v >> 1;
            g_crc[i] = v;
        }
    c = ~c;
    while (n--)
        c = g_crc[(c ^ *p++) & 0xff] ^ c >> 8;
    return ~c;
}
static int v1_signature_file(const char *n, size_t l)       /* META-INF/MANIFEST.MF, *.SF, *.RSA, *.DSA, *.EC: dropped */
{
    if (l < 9 || memcmp(n, "META-INF/", 9) || memchr(n + 9, '/', l - 9))
        return 0;
    static const char *ext[] = { ".SF", ".RSA", ".DSA", ".EC" };
    if (l == 20 && !memcmp(n, "META-INF/MANIFEST.MF", 20))
        return 1;
    for (int i = 0; i < 4; i++) {
        size_t e = strlen(ext[i]);
        if (l > e && !_strnicmp(n + l - e, ext[i], e))
            return 1;
    }
    return 0;
}

/* ---- APK Signature Scheme v2: SHA-256 over 1 MB chunks of the entries, the central directory and the end record ---- */
#define CHUNK (1u << 20)
static void chunk_digests(Buf *out, const uint8_t *d, size_t n)
{
    static uint8_t t[5 + CHUNK];
    for (size_t o = 0; o < n; o += CHUNK) {
        uint32_t len = (uint32_t)(n - o < CHUNK ? n - o : CHUNK);
        uint8_t h[32];
        t[0] = 0xa5;
        wr32(t + 1, len);
        memcpy(t + 5, d + o, len);
        sha256(t, 5 + len, NULL, 0, h);
        put(out, h, 32);
    }
}

/* the data path as the ANSI one src/iso.c's fopen takes (else its 8.3 name) */
static uint8_t g_buf[4 << 20];

/* one entry behind the others: a local header with an alignment extra field (zipalign's 0xd935), so its data starts on
 * 4096, then src's isz bytes, stored; its record into dir. what: the source, for the messages */
static void add_stored(FILE *o, Buf *dir, const char *asset, FILE *src, uint32_t isz, const wchar_t *partname, const char *what)
{
    size_t al = strlen(asset), lh = (size_t)_ftelli64(o), xl = 6;
    while ((lh + 30 + al + xl) % 4096)
        xl++;
    Buf h = { 0 };
    put32(&h, 0x04034b50); put16(&h, 10); put16(&h, 0); put16(&h, 0); put16(&h, 0); put16(&h, 0x21);   /* stored, 1980-01-01 */
    put32(&h, 0); put32(&h, isz); put32(&h, isz); put16(&h, (unsigned)al); put16(&h, (unsigned)xl);
    put(&h, asset, al); put16(&h, 0xd935); put16(&h, (unsigned)(xl - 4)); put16(&h, 4096);
    while (h.n < 30 + al + xl)
        put8(&h, 0);
    if (fwrite(h.p, 1, h.n, o) != h.n)
        die("cannot write %ls (disk full?)", partname);
    free(h.p);
    uint32_t crc = 0;
    for (uint32_t left = isz; left; ) {
        size_t k = left < sizeof g_buf ? left : sizeof g_buf;
        if (fread(g_buf, 1, k, src) != k)
            die("cannot read all of %s", what);
        crc = crc32_add(crc, g_buf, k);
        if (fwrite(g_buf, 1, k, o) != k)
            die("cannot write %ls (disk full?)", partname);
        left -= (uint32_t)k;
    }
    uint8_t c4[4];
    wr32(c4, crc);
    if (_fseeki64(o, (long long)lh + 14, SEEK_SET) != 0 || fwrite(c4, 1, 4, o) != 4 || _fseeki64(o, 0, SEEK_END) != 0)
        die("cannot write %ls", partname);
    put32(dir, 0x02014b50); put16(dir, 10); put16(dir, 10); put16(dir, 0); put16(dir, 0); put16(dir, 0); put16(dir, 0x21);
    put32(dir, crc); put32(dir, isz); put32(dir, isz); put16(dir, (unsigned)al); put16(dir, 0); put16(dir, 0);
    put16(dir, 0); put16(dir, 0); put32(dir, 0); put32(dir, (uint32_t)lh); put(dir, asset, al);
}

#include "wiifiles.h"

static void ansi(const wchar_t *w, char *out, size_t cap)
{
    BOOL bad = FALSE;
    int utf8 = GetACP() == CP_UTF8;
    if (WideCharToMultiByte(CP_ACP, utf8 ? 0 : WC_NO_BEST_FIT_CHARS, w, -1, out, (int)cap, NULL, utf8 ? NULL : &bad) && !bad)
        return;
    wchar_t s[MAX_PATH * 2];
    DWORD k = GetShortPathNameW(w, s, MAX_PATH * 2);
    bad = FALSE;
    if (!k || k >= MAX_PATH * 2 ||
        !WideCharToMultiByte(CP_ACP, utf8 ? 0 : WC_NO_BEST_FIT_CHARS, s, -1, out, (int)cap, NULL, utf8 ? NULL : &bad) || bad)
        die("cannot open %ls: rename it or its folder (letters this system's code page lacks)", w);
}

int main(void)
{
    int argc;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || (argc != 6 && argc != 7)) {
        fprintf(stderr, "usage: apkbundle <in.apk> <disc image | RAMPAGE.VOL | auto> <out.apk> <key file> <certificate name> "
                "[extracted Wii disc | auto]\n");
        return 2;
    }
    char cn[200], data[1024];
    snprintf(cn, sizeof cn, "%ls", argv[5]);
    if (!wcscmp(argv[2], L"auto")) {
        if (!data_find(data, sizeof data))
            die("found no game data in %s.\nPass your disc image or RAMPAGE.VOL: drag it onto make_android_bundle.bat.",
                data_where());
    } else
        ansi(argv[2], data, sizeof data);
    DataPart part[3];
    if (!data_parts(data, part))
        die("%s holds no RAMPAGE.VOL: not the game's disc image, nor its RAMPAGE.VOL", data);
    printf("Game data: %s\n", data);
    if (!part[1].size)
        printf("(no %s there: the effects' words will be missing)\n", part[1].name);
    if (argc == 7)                                             /* the Wii's (docs/WII_PLAN.md) */
        wii_take(argv[6]);
    if (BCryptOpenAlgorithmProvider(&g_sha, BCRYPT_SHA256_ALGORITHM, NULL, 0) ||
        BCryptOpenAlgorithmProvider(&g_rsa, BCRYPT_RSA_ALGORITHM, NULL, 0))
        die("Windows' crypto (bcrypt) is not available");
    Buf blob = { 0 }, cert = { 0 };
    BCRYPT_KEY_HANDLE key = load_key(argv[4], cn, &blob, &cert);

    /* the APK: its entries end where the old APK Signing Block (or else the central directory) starts */
    size_t an;
    uint8_t *apk = read_all(argv[1], &an);
    if (!apk)
        die("cannot read %ls", argv[1]);
    size_t eo = an >= 22 ? an - 22 : 0;
    while (eo > 0 && !(rd32(apk + eo) == 0x06054b50 && eo + 22 + rd16(apk + eo + 20) == an))
        eo--;
    if (an < 22 || rd32(apk + eo) != 0x06054b50)
        die("%ls is not an APK (no ZIP directory)", argv[1]);
    size_t cd = rd32(apk + eo + 16), cds = rd32(apk + eo + 12);
    if (cd + cds != eo)
        die("%ls: unexpected ZIP layout", argv[1]);
    size_t end = cd;
    if (cd >= 32 && !memcmp(apk + cd - 16, "APK Sig Block 42", 16)) {
        uint64_t bs = rd64(apk + cd - 24);
        if (bs > cd - 8 || rd64(apk + cd - 8 - bs) != bs)
            die("%ls: damaged APK Signing Block", argv[1]);
        end = cd - 8 - (size_t)bs;
    }
    Buf dir = { 0 };
    unsigned count = 0;                                        /* the central directory without the old v1 signature */
    for (size_t o = cd; o < eo; ) {
        if (o + 46 > eo || rd32(apk + o) != 0x02014b50)
            die("%ls: damaged ZIP directory", argv[1]);
        size_t nl = rd16(apk + o + 28), len = 46 + nl + rd16(apk + o + 30) + rd16(apk + o + 32);
        const char *nm = (const char *)apk + o + 46;
        for (int i = 0; i < 3; i++) {
            char a[80];
            int al = snprintf(a, sizeof a, "assets/%s", part[i].name);
            if (((size_t)al == nl && !_strnicmp(nm, a, nl)) || (nl > 11 && !_strnicmp(nm, "assets/wii/", 11)))
                die("%ls already holds the game's data: start from the APK of a release (or of your own build)", argv[1]);
        }
        if (!v1_signature_file(nm, nl)) {
            put(&dir, apk + o, len);
            count++;
        }
        o += len;
    }
    uint64_t total = end;
    for (int i = 0; i < 3; i++)
        total += part[i].size + 30 + 80 + 4096;
    for (unsigned i = 0; i < g_nwii; i++)
        total += g_wii[i].size + 30 + 128 + 4096 + 46 + 128;
    if (total + dir.n + (1u << 16) > MAX_APK)
        die("the bundle would be 2 GB or more");

    wchar_t partname[MAX_PATH + 8];
    _snwprintf(partname, MAX_PATH + 8, L"%ls.part", argv[3]);
    partname[MAX_PATH + 7] = 0;
    FILE *o = _wfopen(partname, L"w+b");
    if (!o)
        die("cannot write %ls", partname);
    g_part = partname;
    if (fwrite(apk, 1, end, o) != end)
        die("cannot write %ls", partname);
    free(apk);

    /* the new entries, RAMPAGE.VOL last of the disc's, then the Wii's */
    for (int i = 0; i < 3; i++) {
        if (!part[i].size)
            continue;
        char asset[80];
        snprintf(asset, sizeof asset, "assets/%s", part[i].name);
        printf("Adding %s (%u MB) ...\n", asset, part[i].size >> 20);
        FILE *src = fopen(part[i].file, "rb");
        if (!src || _fseeki64(src, part[i].at, SEEK_SET) != 0)
            die("cannot read %s", part[i].file);
        add_stored(o, &dir, asset, src, part[i].size, partname, part[i].file);
        fclose(src);
        count++;
    }
    if (g_nwii)
        printf("Adding the Wii's %u files ...\n", g_nwii);
    for (unsigned i = 0; i < g_nwii; i++) {
        char what[MAX_PATH * 2], asset[128];
        snprintf(what, sizeof what, "%ls", g_wii[i].path);
        snprintf(asset, sizeof asset, "assets/%s", g_wii[i].rel);
        FILE *src = _wfopen(g_wii[i].path, L"rb");
        if (!src)
            die("cannot read %s", what);
        add_stored(o, &dir, asset, src, g_wii[i].size, partname, what);
        fclose(src);
        count++;
    }
    size_t e = (size_t)_ftelli64(o);
    if (count > 0xffff || e > MAX_APK)
        die("too many entries, or too large");
    Buf eocd = { 0 };                                          /* the directory's offset is the signing block's, for the digest */
    put32(&eocd, 0x06054b50); put16(&eocd, 0); put16(&eocd, 0); put16(&eocd, count); put16(&eocd, count);
    put32(&eocd, (uint32_t)dir.n); put32(&eocd, (uint32_t)e); put16(&eocd, 0);

    /* the v2 digest of [0, e) (read back), the directory and the end record */
    printf("Signing ...\n");
    Buf digs = { 0 };
    uint32_t chunks = 0;
    _fseeki64(o, 0, SEEK_SET);
    for (size_t at = 0; at < e; ) {
        size_t n = e - at < CHUNK ? e - at : CHUNK;
        if (fread(g_buf, 1, n, o) != n)
            die("cannot read %ls back", partname);
        chunk_digests(&digs, g_buf, n);
        at += n;
        chunks++;
    }
    size_t before = digs.n;
    chunk_digests(&digs, dir.p, dir.n);
    chunk_digests(&digs, eocd.p, eocd.n);
    chunks += (uint32_t)((digs.n - before) / 32);
    uint8_t pre[5] = { 0x5a }, top[32];
    wr32(pre + 1, chunks);
    sha256(pre, 5, digs.p, digs.n, top);

    Buf dg = { 0 }, dseq = { 0 }, cseq = { 0 }, sd = { 0 };
    put32(&dg, 0x0103); put32(&dg, 32); put(&dg, top, 32); put_lp(&dseq, &dg);   /* RSASSA-PKCS1-v1_5, SHA-256 chunks */
    put_lp(&cseq, &cert);
    put_lp(&sd, &dseq); put_lp(&sd, &cseq); put32(&sd, 0);                       /* no additional attributes */
    Buf sig = rsa_sign(key, sd.p, sd.n), s1 = { 0 }, sseq = { 0 }, pub = spki(blob.p), signer = { 0 }, signers = { 0 }, value = { 0 };
    put32(&s1, 0x0103); put_lp(&s1, &sig); put_lp(&sseq, &s1);
    put_lp(&signer, &sd); put_lp(&signer, &sseq); put_lp(&signer, &pub);
    put_lp(&signers, &signer); put_lp(&value, &signers);
    Buf blk = { 0 };
    uint64_t bsz = 8 + 4 + value.n + 8 + 16;
    put64(&blk, bsz); put64(&blk, 4 + value.n); put32(&blk, 0x7109871a); put(&blk, value.p, value.n); put64(&blk, bsz);
    put(&blk, "APK Sig Block 42", 16);
    wr32(eocd.p + 16, (uint32_t)(e + blk.n));
    _fseeki64(o, (long long)e, SEEK_SET);
    if (fwrite(blk.p, 1, blk.n, o) != blk.n || fwrite(dir.p, 1, dir.n, o) != dir.n || fwrite(eocd.p, 1, eocd.n, o) != eocd.n ||
        fclose(o))
        die("cannot write %ls (disk full?)", partname);
    if (!MoveFileExW(partname, argv[3], MOVEFILE_REPLACE_EXISTING))
        die("cannot write %ls (is it open somewhere?)", argv[3]);
    g_part = NULL;
    printf("Done: %ls, %.0f MB\n", argv[3], (double)(e + blk.n + dir.n + eocd.n) / (1 << 20));
    return 0;
}
