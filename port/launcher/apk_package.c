/* WWE SmackDown vs. Raw 2011 launcher - "Create APK Package" (apk_package.h).

   The game zip is stored (not compressed: the disc files are compressed
   already) and always Zip64, since the game is over 4 GB. Each file is
   streamed once: its local header is written with placeholder CRC and sizes
   and patched after the data. The phone app (android/InstallActivity.java)
   extracts it into games/WWE SmackDown vs. Raw 2011. */

#include "apk_package.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define APK_NAME     L"SvR2011.apk"
#define ZIP_NAME     L"SvR2011-Game.zip"
#define CHUNK        (8u << 20)

typedef struct {
    WCHAR    name[MAX_PATH];     /* relative, '/' separated (UTF-16) */
    uint64_t size, offset;
    uint32_t crc;
    uint16_t time, date;
    int      dir;
} Entry;

typedef struct {
    const WCHAR    *root, *out_dir;
    Entry          *e;
    int             n, cap;
    uint64_t        total, done;
    volatile LONG  *cancel;
    apk_progress_fn progress;
    int             last;
} Pack;

static uint32_t s_crc_table[256];

static void crc_init(void)
{
    uint32_t i, k, c;
    for (i = 0; i < 256; i++) {
        for (c = i, k = 0; k < 8; k++)
            c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc_table[i] = c;
    }
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--)
        crc = s_crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* Left out of the package: the Windows programs and the PC's own state. */
static int skipped(const WCHAR *rel, int dir)
{
    static const WCHAR *dirs[] = { L"logs", L"SaveBackups", L"Android", L"$SystemUpdate",
                                   L"UserData/cache", L"UserData/crashes" };
    static const WCHAR *files[] = { L"launcher.ini", L"svr2011.toml" };
    const WCHAR *ext = wcsrchr(rel, L'.');
    int i;
    if (dir) {
        for (i = 0; i < (int)(sizeof dirs / sizeof dirs[0]); i++)
            if (!_wcsicmp(rel, dirs[i]))
                return 1;
        return 0;
    }
    for (i = 0; i < (int)(sizeof files / sizeof files[0]); i++)
        if (!_wcsicmp(rel, files[i]))
            return 1;
    return !wcschr(rel, L'/') && ext &&
           (!_wcsicmp(ext, L".exe") || !_wcsicmp(ext, L".dll") || !_wcsicmp(ext, L".pdb"));
}

static int add(Pack *p, const WCHAR *rel, uint64_t size, const FILETIME *ft, int dir)
{
    Entry *e;
    FILETIME local;
    WORD d = 0x21, t = 0;
    if (p->n == p->cap) {
        int cap = p->cap ? p->cap * 2 : 1024;
        Entry *ne = (Entry *)realloc(p->e, cap * sizeof(Entry));
        if (!ne)
            return 0;
        p->e = ne;
        p->cap = cap;
    }
    e = &p->e[p->n++];
    memset(e, 0, sizeof *e);
    swprintf_s(e->name, MAX_PATH, dir ? L"%s/" : L"%s", rel);
    e->size = size;
    e->dir = dir;
    if (FileTimeToLocalFileTime(ft, &local))
        FileTimeToDosDateTime(&local, &d, &t);
    e->date = d;
    e->time = t;
    p->total += size;
    return 1;
}

/* Lists root\rel into p (directories as entries too, so empty ones exist on
   the phone). */
static int walk(Pack *p, const WCHAR *rel)
{
    WCHAR pattern[MAX_PATH], child[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf_s(pattern, MAX_PATH, rel[0] ? L"%s\\%s\\*" : L"%s%s\\*", p->root, rel);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 1;
    do {
        int dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            continue;
        swprintf_s(child, MAX_PATH, rel[0] ? L"%s/%s" : L"%s%s", rel, fd.cFileName);
        if (skipped(child, dir))
            continue;
        if (dir) {
            swprintf_s(full, MAX_PATH, L"%s\\%s", p->root, child);
            if (!_wcsicmp(full, p->out_dir))  /* (packaging into the game folder) */
                continue;
            if (!add(p, child, 0, &fd.ftLastWriteTime, 1) || !walk(p, child)) {
                FindClose(h);
                return 0;
            }
        } else if (!add(p, child, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                        &fd.ftLastWriteTime, 0)) {
            FindClose(h);
            return 0;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

/* ── little-endian record writing ───────────────────────────────────────── */

typedef struct { uint8_t b[512]; int n; } Rec;
static void r16(Rec *r, uint32_t v) { r->b[r->n++] = (uint8_t)v; r->b[r->n++] = (uint8_t)(v >> 8); }
static void r32(Rec *r, uint32_t v) { r16(r, v & 0xFFFF); r16(r, v >> 16); }
static void r64(Rec *r, uint64_t v) { r32(r, (uint32_t)v); r32(r, (uint32_t)(v >> 32)); }
static void rbytes(Rec *r, const void *p, int n) { memcpy(r->b + r->n, p, n); r->n += n; }

static int write_all(HANDLE f, const void *p, DWORD n)
{
    DWORD w;
    return WriteFile(f, p, n, &w, NULL) && w == n;
}

static uint64_t tell(HANDLE f)
{
    LARGE_INTEGER z = { 0 }, pos;
    SetFilePointerEx(f, z, &pos, FILE_CURRENT);
    return (uint64_t)pos.QuadPart;
}

static void seek(HANDLE f, uint64_t at)
{
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)at;
    SetFilePointerEx(f, pos, NULL, FILE_BEGIN);
}

static int utf8(const WCHAR *s, char *out, int outn)
{
    int k = WideCharToMultiByte(CP_UTF8, 0, s, -1, out, outn, NULL, NULL);
    return k > 0 ? k - 1 : -1;
}

static void progress(Pack *p, const WCHAR *name)
{
    int pm = p->total ? (int)(p->done * 1000 / p->total) : 0;
    WCHAR msg[MAX_PATH + 64];
    if (pm == p->last && !name)
        return;
    p->last = pm;
    if (name)
        swprintf_s(msg, MAX_PATH + 64, L"Packing %s\x2026", name);
    p->progress(pm, name ? msg : NULL);
}

/* One entry: local header, data (CRC while copying), header patched. */
static int write_entry(Pack *p, HANDLE out, Entry *e, uint8_t *buf, WCHAR *err, int errn)
{
    char name[MAX_PATH * 3];
    int nlen = utf8(e->name, name, sizeof name);
    Rec r = { { 0 }, 0 };
    uint64_t header, got = 0;
    if (nlen < 0) {
        swprintf_s(err, errn, L"Bad file name: %s", e->name);
        return 0;
    }
    e->offset = header = tell(out);
    r32(&r, 0x04034b50); r16(&r, 45); r16(&r, 0x0800); r16(&r, 0);
    r16(&r, e->time); r16(&r, e->date);
    r32(&r, 0); r32(&r, 0xFFFFFFFF); r32(&r, 0xFFFFFFFF);
    r16(&r, nlen); r16(&r, 20);
    if (!write_all(out, r.b, r.n) || !write_all(out, name, nlen))
        goto write_failed;
    r.n = 0;
    r16(&r, 0x0001); r16(&r, 16); r64(&r, 0); r64(&r, 0);
    if (!write_all(out, r.b, r.n))
        goto write_failed;
    if (!e->dir) {
        WCHAR full[MAX_PATH], *c;
        HANDLE in;
        swprintf_s(full, MAX_PATH, L"%s\\%s", p->root, e->name);
        for (c = full; *c; c++)
            if (*c == L'/')
                *c = L'\\';
        in = CreateFileW(full, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        if (in == INVALID_HANDLE_VALUE) {
            swprintf_s(err, errn, L"Could not read %s", full);
            return 0;
        }
        for (;;) {
            DWORD n = 0;
            if (*p->cancel) {
                CloseHandle(in);
                swprintf_s(err, errn, L"Cancelled.");
                return 0;
            }
            if (!ReadFile(in, buf, CHUNK, &n, NULL)) {
                CloseHandle(in);
                swprintf_s(err, errn, L"Could not read %s", full);
                return 0;
            }
            if (!n)
                break;
            e->crc = crc_update(e->crc, buf, n);
            if (!write_all(out, buf, n)) {
                CloseHandle(in);
                goto write_failed;
            }
            got += n;
            p->done += n;
            progress(p, NULL);
        }
        CloseHandle(in);
        e->size = got;  /* (as read, should it differ from the listing) */
    }
    {
        uint64_t end = tell(out);
        Rec c = { { 0 }, 0 }, z = { { 0 }, 0 };
        r32(&c, e->crc);
        r64(&z, e->size); r64(&z, e->size);
        seek(out, header + 14);
        if (!write_all(out, c.b, c.n))
            goto write_failed;
        seek(out, header + 30 + nlen + 4);
        if (!write_all(out, z.b, z.n))
            goto write_failed;
        seek(out, end);
    }
    return 1;
write_failed:
    swprintf_s(err, errn, L"Could not write the package (is the drive full?).");
    return 0;
}

static int write_central(Pack *p, HANDLE out, WCHAR *err, int errn)
{
    uint64_t cd = tell(out), cd_size, eocd64;
    Rec r;
    int i;
    for (i = 0; i < p->n; i++) {
        Entry *e = &p->e[i];
        char name[MAX_PATH * 3];
        int nlen = utf8(e->name, name, sizeof name);
        r.n = 0;
        r32(&r, 0x02014b50); r16(&r, 45); r16(&r, 45); r16(&r, 0x0800); r16(&r, 0);
        r16(&r, e->time); r16(&r, e->date);
        r32(&r, e->crc); r32(&r, 0xFFFFFFFF); r32(&r, 0xFFFFFFFF);
        r16(&r, nlen); r16(&r, 28); r16(&r, 0); r16(&r, 0); r16(&r, 0);
        r32(&r, e->dir ? 0x10 : 0); r32(&r, 0xFFFFFFFF);
        rbytes(&r, name, nlen);
        r16(&r, 0x0001); r16(&r, 24); r64(&r, e->size); r64(&r, e->size); r64(&r, e->offset);
        if (!write_all(out, r.b, r.n))
            goto write_failed;
    }
    eocd64 = tell(out);
    cd_size = eocd64 - cd;
    r.n = 0;
    r32(&r, 0x06064b50); r64(&r, 44); r16(&r, 45); r16(&r, 45); r32(&r, 0); r32(&r, 0);
    r64(&r, (uint64_t)p->n); r64(&r, (uint64_t)p->n); r64(&r, cd_size); r64(&r, cd);
    r32(&r, 0x07064b50); r32(&r, 0); r64(&r, eocd64); r32(&r, 1);
    r32(&r, 0x06054b50); r16(&r, 0); r16(&r, 0); r16(&r, 0xFFFF); r16(&r, 0xFFFF);
    r32(&r, 0xFFFFFFFF); r32(&r, 0xFFFFFFFF); r16(&r, 0);
    if (!write_all(out, r.b, r.n))
        goto write_failed;
    return 1;
write_failed:
    swprintf_s(err, errn, L"Could not write the package (is the drive full?).");
    return 0;
}

int apk_find(const WCHAR *game_dir, const WCHAR *launcher_dir, WCHAR *out, int outn)
{
    const WCHAR *dirs[2] = { game_dir, launcher_dir };
    int i;
    for (i = 0; i < 2; i++) {
        if (!dirs[i] || !dirs[i][0])
            continue;
        swprintf_s(out, outn, L"%s\\Android\\" APK_NAME, dirs[i]);
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
            return 1;
    }
    out[0] = 0;
    return 0;
}

static const char kHowTo[] =
    "WWE SmackDown vs. Raw 2011 on Android\r\n"
    "=====================================\r\n"
    "\r\n"
    "1. Copy both files to your phone (USB cable: the phone's \"games\" or \"Download\" folder):\r\n"
    "     SvR2011.apk\r\n"
    "     SvR2011-Game.zip\r\n"
    "2. On the phone, open SvR2011.apk to install it (allow installing apps from your file manager).\r\n"
    "3. Start SvR 2011 and allow access to all files when asked: the game lives in\r\n"
    "     games/WWE SmackDown vs. Raw 2011\r\n"
    "   The first start installs SvR2011-Game.zip there; delete the zip afterwards to free the space.\r\n"
    "\r\n"
    "Needs a phone with Vulkan 1.1 and an arm64 processor (a recent Snapdragon is best).\r\n"
    "A controller is recommended.\r\n";

int apk_package(const WCHAR *game_dir, const WCHAR *apk, const WCHAR *out_dir,
                volatile LONG *cancel, apk_progress_fn progress_fn, WCHAR *err, int errn)
{
    Pack p;
    WCHAR zip[MAX_PATH], part[MAX_PATH], dst[MAX_PATH];
    HANDLE out;
    uint8_t *buf;
    int i, ok = 0;

    memset(&p, 0, sizeof p);
    p.root = game_dir;
    p.out_dir = out_dir;
    p.cancel = cancel;
    p.progress = progress_fn;
    p.last = -1;
    crc_init();

    CreateDirectoryW(out_dir, NULL);
    swprintf_s(dst, MAX_PATH, L"%s\\" APK_NAME, out_dir);
    if (!CopyFileW(apk, dst, FALSE)) {
        swprintf_s(err, errn, L"Could not copy the app to %s", dst);
        return 0;
    }
    {
        WCHAR txt[MAX_PATH];
        HANDLE f;
        swprintf_s(txt, MAX_PATH, L"%s\\Install on Android.txt", out_dir);
        f = CreateFileW(txt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            write_all(f, kHowTo, (DWORD)(sizeof kHowTo - 1));
            CloseHandle(f);
        }
    }

    progress_fn(0, L"Listing the game files\x2026");
    if (!walk(&p, L"")) {
        swprintf_s(err, errn, L"Out of memory listing the game files.");
        free(p.e);
        return 0;
    }
    {
        int has_xex = 0;
        for (i = 0; i < p.n; i++)
            if (!_wcsicmp(p.e[i].name, L"default.xex"))
                has_xex = 1;
        if (!has_xex) {
            swprintf_s(err, errn, L"%s has no default.xex: install the game first.", game_dir);
            free(p.e);
            return 0;
        }
    }

    swprintf_s(zip, MAX_PATH, L"%s\\" ZIP_NAME, out_dir);
    swprintf_s(part, MAX_PATH, L"%s.part", zip);
    buf = (uint8_t *)malloc(CHUNK);
    out = CreateFileW(part, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (!buf || out == INVALID_HANDLE_VALUE) {
        swprintf_s(err, errn, L"Could not create %s", part);
        goto done;
    }
    for (i = 0; i < p.n; i++) {
        if (!p.e[i].dir)
            progress(&p, p.e[i].name);
        if (!write_entry(&p, out, &p.e[i], buf, err, errn))
            goto done;
    }
    if (!write_central(&p, out, err, errn))
        goto done;
    ok = 1;
done:
    if (out != INVALID_HANDLE_VALUE)
        CloseHandle(out);
    free(buf);
    free(p.e);
    if (ok) {
        if (!MoveFileExW(part, zip, MOVEFILE_REPLACE_EXISTING)) {
            swprintf_s(err, errn, L"Could not rename %s", part);
            ok = 0;
        }
    }
    if (!ok)
        DeleteFileW(part);
    return ok;
}
