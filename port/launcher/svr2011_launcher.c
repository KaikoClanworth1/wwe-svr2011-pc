/**
 * WWE SmackDown vs. Raw 2011 -- PC port launcher.
 *
 * Six tabs:
 *
 *   Play      the game folder, Play (runs svr2011.exe there), close-on-start.
 *   Settings  display mode, resolution, vsync, controller API, mute --
 *             written to svr2011.toml in the game folder, the runtime's
 *             CVar file (unknown lines in it are kept).
 *   Install   the game from the Xbox 360 disc image (ISO or XISO): its
 *             XDVDFS file system is read directly and every file copied
 *             out, then the port's program files are copied beside them.
 *
 *   DLC       copies DLC packages into the game folder (unpacked at startup).
 *   Saves     the save files (<game>\Saves): back up, restore, export, import, delete.
 *   Paint Tool the Paint Tool's logos: see them, export as PNG, import images.
 *
 * Command line (tests):  --capture <play|settings|install|dlc|saves|paint> <file.bmp>
 *                        --paint-export <file.pt> <slot> <out.png>  /  --paint-import <file.pt> <slot> <image>
 *                        --install <image> <folder>   (no window; exit code)
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <tlhelp32.h>
#include <windowsx.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <wchar.h>

#include "movie_maker.h"
#include "updater.h"

#ifndef PORT_VERSION
#define PORT_VERSION L"0.0.0"   /* set from port/VERSION by CMake */
#endif

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#define LAUNCHER_VERSION  L"1.0"
#define GAME_TITLE        L"WWE SmackDown vs. Raw 2011"
#define WINDOW_TITLE      GAME_TITLE L" \x2014 PC Port Launcher"
#define GAME_EXE          L"svr2011.exe"
#define GAME_TOML         L"svr2011.toml"
#define TITLE_ID          0x5451085Du     /* from the disc's default.xex execution info */
#define GAME_BYTES_EST    5690000000ull   /* about 5.3 GB of disc files */
#define COPY_CHUNK        (8u << 20)

/* ── state ─────────────────────────────────────────────────────────────── */

enum { TAB_PLAY, TAB_SETTINGS, TAB_INSTALL, TAB_DLC, TAB_SAVES, TAB_PAINT, TAB_MOVIES, TAB_COUNT };

enum {
    ID_TAB = 100,
    /* play */
    ID_GAMEDIR, ID_GAMEDIR_CHANGE, ID_PLAY, ID_CLOSE_ON_PLAY, ID_PLAY_STATUS, ID_VERSION,
    /* settings */
    ID_WINDOWED, ID_FULLSCREEN, ID_RESOLUTION, ID_VSYNC, ID_INPUT, ID_AUDIO, ID_MUTE, ID_SHOWFPS, ID_MSAA, ID_RENDERER, ID_MUSIC_OPEN, ID_DEFAULTS, ID_SAVE,
    ID_SETTINGS_STATUS,
    /* install */
    ID_IMAGE, ID_IMAGE_BROWSE, ID_TARGET, ID_TARGET_BROWSE, ID_FREE, ID_INSTALL, ID_CANCEL,
    ID_PROGRESS, ID_INSTALL_STATUS,
    /* dlc */
    ID_DLC_DIR, ID_DLC_BROWSE, ID_DLC_INSTALL, ID_DLC_STATUS, ID_DLC_LIST,
    /* saves */
    ID_SV_LIST, ID_SV_BACKUP, ID_SV_RESTORE, ID_SV_BACKUPS, ID_SV_EXPORT, ID_SV_IMPORT, ID_SV_DELETE, ID_SV_OPEN,
    ID_SV_STATUS,
    /* paint tool */
    ID_PT_GRID, ID_PT_EXPORT, ID_PT_IMPORT, ID_PT_DELETE, ID_PT_EXPORTALL, ID_PT_REFRESH, ID_PT_STATUS,
    /* movies */
    ID_MV_VIDEO, ID_MV_VIDEO_BROWSE, ID_MV_BOTTOM, ID_MV_BOTTOM_BROWSE, ID_MV_BOTTOM_NONE, ID_MV_BOTTOM_STAR, ID_MV_FIT, ID_MV_FILL,
    ID_MV_STRETCH, ID_MV_LENGTH, ID_MV_NAME, ID_MV_VIEW, ID_MV_LIST, ID_MV_DELETE, ID_MV_OPEN, ID_MV_PREVIEW,
    ID_MV_CREATE, ID_MV_STOP, ID_MV_PROGRESS, ID_MV_STATUS,
    /* updates (Play tab) */
    ID_UP_STATUS, ID_UP_BUTTON, ID_UP_AUTO, ID_UP_PROGRESS,
};

#define WM_APP_PROGRESS (WM_APP + 1)     /* wParam permille, lParam heap WCHAR* or 0 */
#define WM_APP_DONE     (WM_APP + 2)     /* wParam 1 ok / 0 failed, lParam heap WCHAR* */
#define WM_APP_GAMEEND  (WM_APP + 3)     /* wParam exit code */
#define WM_APP_DLC      (WM_APP + 4)     /* wParam 1 finished / 0 progress, lParam heap WCHAR* */
#define WM_APP_MOVIE    (WM_APP + 5)     /* wParam percent */
#define WM_APP_MOVIE_DONE (WM_APP + 6)   /* wParam 1 ok / 0 failed */
#define WM_APP_UPD_CHECKED (WM_APP + 7)  /* wParam 1 ok, lParam heap error text */
#define WM_APP_UPD_PROGRESS (WM_APP + 8) /* wParam percent, lParam 1 = unpacking */
#define WM_APP_UPD_DONE (WM_APP + 9)     /* wParam 1 ok, lParam heap error text */

static HINSTANCE s_inst;
static HWND      s_wnd, s_tab;
static HFONT     s_font, s_big, s_title;
static int       s_dpi = 96;
static HICON     s_icon;
static HWND      s_ctl[TAB_COUNT][32];
static int       s_nctl[TAB_COUNT];
static WCHAR     s_launcher_dir[MAX_PATH], s_launcher_exe[MAX_PATH], s_launcher_ini[MAX_PATH];
static WCHAR     s_game_dir[MAX_PATH];
static volatile LONG s_busy, s_cancel;
static HANDLE    s_game_proc, s_worker;
static int       s_settings_dirty;
static int       s_console;               /* --install: print instead of posting */
static HANDLE    s_out;
static uint64_t  s_image_bytes;           /* game bytes on the chosen image, 0 = unknown */

/* Every control, with its layout in 96-DPI units, so a DPI change can re-lay it. */
typedef struct { HWND h; int x, y, w, hh, big; } Placed;
static Placed s_placed[128];
static int    s_nplaced;

static const struct { int w, h, scale; const WCHAR *label; } k_res[] = {
    { 1280,  720, 1, L"1280 \x00D7 720  (the console's)" },
    { 1600,  900, 2, L"1600 \x00D7 900" },
    { 1920, 1080, 2, L"1920 \x00D7 1080" },
    { 2560, 1440, 2, L"2560 \x00D7 1440" },
    { 3840, 2160, 3, L"3840 \x00D7 2160  (4K)" },
};
#define N_RES ((int)(sizeof k_res / sizeof k_res[0]))

static int S(int v) { return MulDiv(v, s_dpi, 96); }

/* ── small helpers ─────────────────────────────────────────────────────── */

static int file_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int dir_exists(const WCHAR *p)
{
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* a\b into out (MAX_PATH); 0 when it would not fit. */
static int join(WCHAR *out, const WCHAR *a, const WCHAR *b)
{
    size_t la = wcslen(a);
    if (la + 1 + wcslen(b) >= MAX_PATH) {
        out[0] = 0;
        return 0;
    }
    swprintf_s(out, MAX_PATH, (la && a[la - 1] == L'\\') ? L"%s%s" : L"%s\\%s", a, b);
    return 1;
}

static int mkdirs(const WCHAR *path)
{
    WCHAR tmp[MAX_PATH];
    WCHAR *p;
    wcscpy_s(tmp, MAX_PATH, path);
    for (p = tmp + (tmp[0] && tmp[1] == L':' ? 3 : 2); *p; p++) {
        if (*p == L'\\' || *p == L'/') {
            WCHAR c = *p;
            *p = 0;
            CreateDirectoryW(tmp, NULL);
            *p = c;
        }
    }
    return CreateDirectoryW(tmp, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static WCHAR *wdup(const WCHAR *s)
{
    size_t n = wcslen(s) + 1;
    WCHAR *d = (WCHAR *)malloc(n * sizeof(WCHAR));
    if (d)
        wcscpy_s(d, n, s);
    return d;
}

static int64_t file_size(const WCHAR *p)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return -1;
    return (int64_t)(((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow);
}

static int same_dir(const WCHAR *a, const WCHAR *b)
{
    WCHAR fa[MAX_PATH], fb[MAX_PATH];
    size_t n;
    if (!GetFullPathNameW(a, MAX_PATH, fa, NULL) || !GetFullPathNameW(b, MAX_PATH, fb, NULL))
        return 0;
    n = wcslen(fa);
    if (n > 3 && fa[n - 1] == L'\\') fa[n - 1] = 0;
    n = wcslen(fb);
    if (n > 3 && fb[n - 1] == L'\\') fb[n - 1] = 0;
    return !_wcsicmp(fa, fb);
}

/* "1.2 GB" style. */
static void fmt_size(WCHAR *out, size_t n, uint64_t bytes)
{
    double g = bytes / 1073741824.0;
    if (g >= 1024.0)
        swprintf_s(out, n, L"%.2f TB", g / 1024.0);
    else if (g >= 1.0)
        swprintf_s(out, n, L"%.1f GB", g);
    else
        swprintf_s(out, n, L"%.0f MB", bytes / 1048576.0);
}

/* Whole megabytes with thousands separators: "2,210". */
static void fmt_mb(WCHAR *out, size_t n, uint64_t bytes)
{
    WCHAR raw[32];
    unsigned long long mb = (unsigned long long)(bytes / 1048576u);
    size_t len, i, o = 0;
    swprintf_s(raw, 32, L"%llu", mb);
    len = wcslen(raw);
    for (i = 0; i < len && o + 2 < n; i++) {
        out[o++] = raw[i];
        if ((len - i - 1) % 3 == 0 && i + 1 < len)
            out[o++] = L',';
    }
    out[o] = 0;
}

static void con_print(const WCHAR *s)
{
    DWORD n, mode;
    if (!s_out || s_out == INVALID_HANDLE_VALUE)
        return;
    if (GetConsoleMode(s_out, &mode)) {
        WriteConsoleW(s_out, s, (DWORD)wcslen(s), &n, NULL);
    } else {
        char u[2048];
        int k = WideCharToMultiByte(CP_UTF8, 0, s, -1, u, (int)sizeof u, NULL, NULL);
        if (k > 1)
            WriteFile(s_out, u, (DWORD)(k - 1), &n, NULL);
    }
}

static void post_progress(int permille, const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    va_list ap;
    if (fmt) {
        va_start(ap, fmt);
        vswprintf_s(buf, 1024, fmt, ap);
        va_end(ap);
    }
    if (s_console) {
        if (fmt) {
            con_print(buf);
            con_print(L"\n");
        }
        return;
    }
    PostMessageW(s_wnd, WM_APP_PROGRESS, (WPARAM)permille, fmt ? (LPARAM)wdup(buf) : 0);
}

static HWND ctl(int id)
{
    return GetDlgItem(s_wnd, id);
}

static void set_text(int id, const WCHAR *t)
{
    SetWindowTextW(ctl(id), t);
}

static int is_game_folder(const WCHAR *dir)
{
    WCHAR a[MAX_PATH];
    return dir[0] && join(a, dir, L"default.xex") && file_exists(a);
}

static int has_program(const WCHAR *dir)
{
    WCHAR a[MAX_PATH];
    return dir[0] && join(a, dir, GAME_EXE) && file_exists(a);
}

static int game_running(void)
{
    return s_game_proc && WaitForSingleObject(s_game_proc, 0) == WAIT_TIMEOUT;
}

/* ── launcher.ini ──────────────────────────────────────────────────────── */

static void load_launcher_ini(void)
{
    GetPrivateProfileStringW(L"Launcher", L"GameFolder", L"", s_game_dir, MAX_PATH, s_launcher_ini);
    if (!s_game_dir[0] || !dir_exists(s_game_dir))
        wcscpy_s(s_game_dir, MAX_PATH, s_launcher_dir);
}

static void save_launcher_ini(void)
{
    WritePrivateProfileStringW(L"Launcher", L"GameFolder", s_game_dir, s_launcher_ini);
    WritePrivateProfileStringW(L"Launcher", L"CloseOnPlay",
        IsDlgButtonChecked(s_wnd, ID_CLOSE_ON_PLAY) == BST_CHECKED ? L"1" : L"0", s_launcher_ini);
}

/* ── the disc image: XDVDFS ────────────────────────────────────────────── */

typedef struct {
    WCHAR    path[MAX_PATH];     /* relative, backslashes */
    uint32_t sector, size;
    int      is_dir;
} DiscFile;

typedef struct {
    HANDLE    f;
    uint64_t  base;              /* partition offset in the image */
    DiscFile *files;             /* directories and files, in table order */
    int       count, cap, nfiles;
    uint64_t  total;             /* bytes in files */
} Disc;

static int disc_read(Disc *d, uint64_t off, void *buf, DWORD n)
{
    LARGE_INTEGER li;
    DWORD got = 0;
    li.QuadPart = (LONGLONG)(d->base + off);
    return SetFilePointerEx(d->f, li, NULL, FILE_BEGIN) && ReadFile(d->f, buf, n, &got, NULL) && got == n;
}

static int disc_add(Disc *d, const WCHAR *path, uint32_t sector, uint32_t size, int is_dir)
{
    if (d->count == d->cap) {
        int cap = d->cap ? d->cap * 2 : 256;
        DiscFile *n = (DiscFile *)realloc(d->files, sizeof(DiscFile) * (size_t)cap);
        if (!n)
            return 0;
        d->files = n;
        d->cap = cap;
    }
    wcscpy_s(d->files[d->count].path, MAX_PATH, path);
    d->files[d->count].sector = sector;
    d->files[d->count].size = size;
    d->files[d->count].is_dir = is_dir;
    d->count++;
    if (!is_dir) {
        d->nfiles++;
        d->total += size;
    }
    return 1;
}

/* One directory table, entry by entry in the order they are stored:
 *   u16 left, u16 right, u32 sector, u32 size, u8 attributes, u8 name length, name
 * 4-byte aligned; 0xFFFF where an entry would start pads to the next sector. */
static int disc_dir(Disc *d, uint32_t sector, uint32_t size, const WCHAR *prefix, int depth)
{
    uint8_t *buf;
    uint32_t off = 0;
    int ok = 1;
    if (depth > 32 || size > (16u << 20) || d->count > 1000000)
        return 0;
    buf = (uint8_t *)malloc(size);
    if (!buf)
        return 0;
    if (!disc_read(d, (uint64_t)sector * 2048, buf, size)) {
        free(buf);
        return 0;
    }
    while (ok && off + 14 <= size) {
        const uint8_t *e = buf + off;
        uint16_t left = (uint16_t)(e[0] | e[1] << 8);
        uint32_t sec, sz;
        uint8_t attr = e[12], nlen = e[13];
        WCHAR name[256], path[MAX_PATH];
        int i;
        if (left == 0xFFFF || nlen == 0) {          /* padding to the next sector */
            off = (off / 2048 + 1) * 2048;
            continue;
        }
        if (off + 14 + nlen > size) {
            ok = 0;
            break;
        }
        memcpy(&sec, e + 4, 4);
        memcpy(&sz, e + 8, 4);
        for (i = 0; i < nlen; i++)
            name[i] = (WCHAR)e[14 + i];
        name[nlen] = 0;
        if (wcschr(name, L'\\') || wcschr(name, L'/') || wcschr(name, L':') || !wcscmp(name, L"..")
                || !wcscmp(name, L".")) {
            ok = 0;                                 /* unsafe or broken */
            break;
        }
        if (wcslen(prefix) + 1 + nlen >= MAX_PATH) {
            ok = 0;
            break;
        }
        swprintf_s(path, MAX_PATH, prefix[0] ? L"%s\\%s" : L"%s%s", prefix, name);
        if (attr & 0x10) {
            ok = disc_add(d, path, sec, sz, 1) && (!sz || disc_dir(d, sec, sz, path, depth + 1));
        } else {
            ok = disc_add(d, path, sec, sz, 0);
        }
        off = (off + 14 + nlen + 3) & ~3u;
    }
    free(buf);
    return ok;
}

static void disc_close(Disc *d)
{
    if (d->f && d->f != INVALID_HANDLE_VALUE)
        CloseHandle(d->f);
    free(d->files);
    memset(d, 0, sizeof *d);
}

/* Open an image and list its files; on failure err explains why. */
static int disc_open(Disc *d, const WCHAR *image, WCHAR *err, size_t errn)
{
    static const uint64_t bases[] = { 0, 0x18300000ull, 0xFD90000ull, 0x2080000ull };
    uint8_t vd[2048];
    int i;
    memset(d, 0, sizeof *d);
    d->f = CreateFileW(image, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (d->f == INVALID_HANDLE_VALUE) {
        swprintf_s(err, errn, L"The disc image could not be opened (error %lu).", GetLastError());
        return 0;
    }
    for (i = 0; i < 4; i++) {
        d->base = bases[i];
        if (disc_read(d, 32 * 2048, vd, sizeof vd) && !memcmp(vd, "MICROSOFT*XBOX*MEDIA", 20)) {
            uint32_t root, rsize;
            memcpy(&root, vd + 20, 4);
            memcpy(&rsize, vd + 24, 4);
            if (!disc_dir(d, root, rsize, L"", 0)) {
                swprintf_s(err, errn, L"The disc image is damaged: its file list could not be read.");
                return 0;
            }
            return 1;
        }
    }
    swprintf_s(err, errn, L"This is not an Xbox 360 disc image (no XDVDFS file system was found in it).");
    return 0;
}

static DiscFile *disc_find(Disc *d, const WCHAR *path)
{
    int i;
    for (i = 0; i < d->count; i++)
        if (!d->files[i].is_dir && !_wcsicmp(d->files[i].path, path))
            return &d->files[i];
    return NULL;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* The title ID in default.xex's execution info (optional header 0x00040006). */
static int disc_title_id(Disc *d, DiscFile *x, uint32_t *id)
{
    uint8_t hdr[24], opt[64 * 8], exe[16];
    uint64_t base = (uint64_t)x->sector * 2048;
    uint32_t n, i;
    if (x->size < 24 || !disc_read(d, base, hdr, 24) || memcmp(hdr, "XEX2", 4))
        return 0;
    n = be32(hdr + 20);
    if (n == 0 || n > 64 || 24 + n * 8 > x->size || !disc_read(d, base + 24, opt, n * 8))
        return 0;
    for (i = 0; i < n; i++) {
        if (be32(opt + i * 8) == 0x00040006u) {
            uint32_t at = be32(opt + i * 8 + 4);
            if (at + 16 > x->size || !disc_read(d, base + at, exe, 16))
                return 0;
            *id = be32(exe + 12);
            return 1;
        }
    }
    return 0;
}

/* Check an image is this game; on failure err says why. Leaves d open on success. */
static int disc_check(Disc *d, const WCHAR *image, WCHAR *err, size_t errn)
{
    DiscFile *x;
    uint32_t tid = 0;
    if (!disc_open(d, image, err, errn))
        return 0;
    x = disc_find(d, L"default.xex");
    if (!x) {
        swprintf_s(err, errn, L"The disc image has no default.xex at its root. It is not an Xbox 360 game disc, "
                              L"or it is the wrong disc.");
        return 0;
    }
    if (!disc_title_id(d, x, &tid)) {
        swprintf_s(err, errn, L"The disc image's default.xex could not be read; the image may be incomplete.");
        return 0;
    }
    if (tid != TITLE_ID) {
        swprintf_s(err, errn, L"This disc is not " GAME_TITLE L" (its title ID is %08X; the game is %08X).",
                   tid, TITLE_ID);
        return 0;
    }
    return 1;
}

/* ── install ───────────────────────────────────────────────────────────── */

typedef struct {
    WCHAR image[MAX_PATH], target[MAX_PATH];
} InstallJob;

typedef struct {
    uint64_t  done, total, copied;
    ULONGLONG t0, last;
    const WCHAR *cur;
} Progress;

static InstallJob s_job;
static WCHAR s_install_msg[1024];

static void progress_tick(Progress *pr, int force)
{
    ULONGLONG now = GetTickCount64();
    double secs, speed;
    int pct;
    WCHAR a[32], b[32], eta[64];
    if (!force && now - pr->last < (s_console ? 2000u : 150u))
        return;
    pr->last = now;
    secs = (now - pr->t0) / 1000.0;
    speed = secs > 0.01 ? pr->copied / secs : 0.0;
    pct = (int)(pr->total ? pr->done * 100 / pr->total : 100);
    fmt_mb(a, 32, pr->done);
    fmt_mb(b, 32, pr->total);
    eta[0] = 0;
    if (speed > 1048576.0 && pr->total > pr->done) {
        double left = (pr->total - pr->done) / speed;
        if (left >= 90)
            swprintf_s(eta, 64, L"  \x2014  about %d min left", (int)(left / 60 + 0.5));
        else
            swprintf_s(eta, 64, L"  \x2014  %d s left", (int)left + 1);
    }
    if (s_console)
        post_progress(0, L"%3d%%  %s / %s MB  %6.1f MB/s  %s", pct, a, b, speed / 1048576.0, pr->cur ? pr->cur : L"");
    else
        post_progress((int)(pr->total ? pr->done * 990 / pr->total : 990),
                      L"%d%%  \x2014  %s of %s MB  \x2014  %.1f MB/s%s\nCopying %s", pct, a, b, speed / 1048576.0, eta,
                      pr->cur ? pr->cur : L"");
}

static int copy_out(Disc *d, DiscFile *df, const WCHAR *target, uint8_t *buf, Progress *pr)
{
    WCHAR dst[MAX_PATH], part[MAX_PATH], dir[MAX_PATH], *slash;
    HANDLE out;
    uint32_t left = df->size;
    uint64_t off = (uint64_t)df->sector * 2048;
    LARGE_INTEGER li;

    if (!join(dst, target, df->path) || wcslen(dst) + 5 >= MAX_PATH) {
        swprintf_s(s_install_msg, 1024, L"The install folder's path is too long for %s.", df->path);
        return 0;
    }
    if (file_size(dst) == (int64_t)df->size) {       /* already there: resume */
        pr->done += df->size;
        return 1;
    }
    swprintf_s(part, MAX_PATH, L"%s.part", dst);
    wcscpy_s(dir, MAX_PATH, dst);
    slash = wcsrchr(dir, L'\\');
    if (slash) {
        *slash = 0;
        mkdirs(dir);
    }
    pr->cur = df->path;
    out = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (out == INVALID_HANDLE_VALUE) {
        swprintf_s(s_install_msg, 1024, L"Could not create %s (error %lu).", part, GetLastError());
        return 0;
    }
    if (df->size) {                                  /* reserve the space up front */
        li.QuadPart = df->size;
        if (SetFilePointerEx(out, li, NULL, FILE_BEGIN))
            SetEndOfFile(out);
        li.QuadPart = 0;
        SetFilePointerEx(out, li, NULL, FILE_BEGIN);
    }
    while (left) {
        DWORD n = left > COPY_CHUNK ? COPY_CHUNK : left, wr = 0;
        if (s_cancel) {
            CloseHandle(out);
            DeleteFileW(part);
            wcscpy_s(s_install_msg, 1024, L"Installation cancelled. Files already copied are kept; "
                                          L"install again to continue where it stopped.");
            return 0;
        }
        if (!disc_read(d, off, buf, n)) {
            CloseHandle(out);
            DeleteFileW(part);
            swprintf_s(s_install_msg, 1024, L"The disc image could not be read at %s. It may be incomplete.", df->path);
            return 0;
        }
        if (!WriteFile(out, buf, n, &wr, NULL) || wr != n) {
            DWORD e = GetLastError();
            CloseHandle(out);
            DeleteFileW(part);
            swprintf_s(s_install_msg, 1024, L"Could not write %s (error %lu). Is the disk full?", dst, e);
            return 0;
        }
        off += n;
        left -= n;
        pr->done += n;
        pr->copied += n;
        progress_tick(pr, 0);
    }
    CloseHandle(out);
    if (!MoveFileExW(part, dst, MOVEFILE_REPLACE_EXISTING)) {
        swprintf_s(s_install_msg, 1024, L"Could not rename %s (error %lu).", part, GetLastError());
        DeleteFileW(part);
        return 0;
    }
    return 1;
}

/* The port's own files, from the launcher's folder to the install. */
/* Copies the files of <launcher folder>\<name> into <target>\<name>. */
static void copy_folder_files(const WCHAR *name, const WCHAR *target)
{
    WCHAR sdir[MAX_PATH], ddir[MAX_PATH], pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!join(sdir, s_launcher_dir, name) || !dir_exists(sdir) || !join(ddir, target, name) || !join(pat, sdir, L"*"))
        return;
    CreateDirectoryW(ddir, NULL);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (join(src, sdir, fd.cFileName) && join(dst, ddir, fd.cFileName))
            CopyFileW(src, dst, FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void copy_program(const WCHAR *target)
{
    WCHAR src[MAX_PATH], dst[MAX_PATH], pat[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    const WCHAR *exe_name;
    if (same_dir(target, s_launcher_dir))
        return;
    if (join(src, s_launcher_dir, GAME_EXE) && file_exists(src) && join(dst, target, GAME_EXE))
        CopyFileW(src, dst, FALSE);
    if (join(src, s_launcher_dir, GAME_TOML) && file_exists(src) && join(dst, target, GAME_TOML))
        CopyFileW(src, dst, TRUE);                   /* keep settings already in the target */
    if (join(src, s_launcher_dir, L"Read Me.txt") && file_exists(src) && join(dst, target, L"Read Me.txt"))
        CopyFileW(src, dst, FALSE);
    if (join(pat, s_launcher_dir, L"*.dll")) {
        h = FindFirstFileW(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    continue;
                if (join(src, s_launcher_dir, fd.cFileName) && join(dst, target, fd.cFileName))
                    CopyFileW(src, dst, FALSE);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    exe_name = wcsrchr(s_launcher_exe, L'\\');
    exe_name = exe_name ? exe_name + 1 : s_launcher_exe;
    if (join(dst, target, exe_name))
        CopyFileW(s_launcher_exe, dst, FALSE);
    /* The native renderer's shaders: without them it draws a black screen. */
    copy_folder_files(L"native_shaders", target);
}

static int cmp_sector(const void *a, const void *b)
{
    const DiscFile *x = *(const DiscFile *const *)a, *y = *(const DiscFile *const *)b;
    return x->sector < y->sector ? -1 : x->sector > y->sector;
}

/* Free bytes on the drive a (maybe not yet existing) folder would be on. */
static int free_space(const WCHAR *path, uint64_t *out)
{
    WCHAR p[MAX_PATH], *s;
    ULARGE_INTEGER fb;
    if (!path[0] || !GetFullPathNameW(path, MAX_PATH, p, NULL))
        return 0;
    while (!dir_exists(p)) {
        s = wcsrchr(p, L'\\');
        if (!s || s - p < 2)
            break;
        *s = 0;
        if (wcslen(p) == 2 && p[1] == L':')
            wcscat_s(p, MAX_PATH, L"\\");
    }
    if (!GetDiskFreeSpaceExW(p, &fb, NULL, NULL))
        return 0;
    *out = fb.QuadPart;
    return 1;
}

static int install_run(InstallJob *job)
{
    Disc d;
    Progress pr;
    DiscFile **order;
    uint64_t need = 0, freeb = 0;
    uint8_t *buf;
    int i, n, ok = 1, present = 0;
    WCHAR err[512], a[32], b[32];

    s_install_msg[0] = 0;
    post_progress(0, L"Reading the disc image...");
    if (!disc_check(&d, job->image, err, 512)) {
        wcscpy_s(s_install_msg, 1024, err);
        disc_close(&d);
        return 0;
    }
    if (!mkdirs(job->target)) {
        swprintf_s(s_install_msg, 1024, L"Could not create %s.", job->target);
        disc_close(&d);
        return 0;
    }
    for (i = 0; i < d.count; i++) {
        WCHAR p[MAX_PATH];
        if (d.files[i].is_dir)
            continue;
        if (join(p, job->target, d.files[i].path) && file_size(p) == (int64_t)d.files[i].size)
            present++;
        else
            need += d.files[i].size;
    }
    if (free_space(job->target, &freeb) && freeb < need + (64ull << 20)) {
        fmt_size(a, 32, need + (64ull << 20));
        fmt_size(b, 32, freeb);
        swprintf_s(s_install_msg, 1024, L"Not enough free space: the game needs about %s more, and the drive has %s.", a, b);
        disc_close(&d);
        return 0;
    }
    /* Folders first (some may be empty), then files in disc order. */
    for (i = 0; i < d.count; i++) {
        WCHAR p[MAX_PATH];
        if (d.files[i].is_dir && join(p, job->target, d.files[i].path))
            mkdirs(p);
    }
    order = (DiscFile **)malloc(sizeof(DiscFile *) * (size_t)(d.nfiles ? d.nfiles : 1));
    buf = (uint8_t *)malloc(COPY_CHUNK);
    if (!order || !buf) {
        free(order);
        free(buf);
        disc_close(&d);
        wcscpy_s(s_install_msg, 1024, L"Out of memory.");
        return 0;
    }
    for (i = 0, n = 0; i < d.count; i++)
        if (!d.files[i].is_dir)
            order[n++] = &d.files[i];
    qsort(order, (size_t)n, sizeof *order, cmp_sector);

    memset(&pr, 0, sizeof pr);
    pr.total = d.total;
    pr.t0 = GetTickCount64();
    fmt_size(a, 32, d.total);
    post_progress(0, L"Copying %d files (%s) from the disc image...", n, a);
    for (i = 0; i < n && ok; i++)
        ok = copy_out(&d, order[i], job->target, buf, &pr);
    if (ok)
        progress_tick(&pr, 1);
    free(buf);
    free(order);
    disc_close(&d);
    if (!ok)
        return 0;
    post_progress(995, L"Copying the PC port's program files...");
    copy_program(job->target);
    if (s_cancel) {
        wcscpy_s(s_install_msg, 1024, L"Installation cancelled.");
        return 0;
    }
    {
        double secs = (GetTickCount64() - pr.t0) / 1000.0;
        fmt_size(a, 32, pr.total);
        swprintf_s(s_install_msg, 1024, L"%d files (%s) installed in %.0f s%s%s", n, a, secs,
                   present ? L"; files already there were kept." : L".",
                   has_program(job->target) ? L"" : L" Note: " GAME_EXE L" was not found beside the launcher, so "
                                                    L"copy the port's program files into the folder before playing.");
    }
    return 1;
}

static DWORD WINAPI install_thread(LPVOID unused)
{
    int ok;
    (void)unused;
    ok = install_run(&s_job);
    PostMessageW(s_wnd, WM_APP_DONE, ok ? 1 : 0, (LPARAM)wdup(s_install_msg));
    return 0;
}

/* ── settings: svr2011.toml ────────────────────────────────────────────── */

typedef struct { char **v; int n, cap; } Lines;

static void lines_free(Lines *l)
{
    int i;
    for (i = 0; i < l->n; i++)
        free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static int lines_insert(Lines *l, int at, const char *s)
{
    size_t len = strlen(s) + 1;
    char *c = (char *)malloc(len);
    if (!c)
        return 0;
    memcpy(c, s, len);
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 32;
        char **nv = (char **)realloc(l->v, sizeof(char *) * (size_t)cap);
        if (!nv) {
            free(c);
            return 0;
        }
        l->v = nv;
        l->cap = cap;
    }
    memmove(l->v + at + 1, l->v + at, sizeof(char *) * (size_t)(l->n - at));
    l->v[at] = c;
    l->n++;
    return 1;
}

static int toml_read(const WCHAR *path, Lines *l)
{
    FILE *f;
    char line[1024];
    memset(l, 0, sizeof *l);
    if (_wfopen_s(&f, path, L"rb") || !f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        size_t k = strlen(line);
        char *s = line;
        while (k && (line[k - 1] == '\n' || line[k - 1] == '\r'))
            line[--k] = 0;
        if (l->n == 0 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s += 3;
        lines_insert(l, l->n, s);
    }
    fclose(f);
    return 1;
}

/* "key = value" at the top level: the key (lowercase-sensitive, as TOML) and value text. */
static int toml_kv(const char *line, char *key, size_t kn, char *val, size_t vn)
{
    const char *p = line, *eq, *e;
    size_t k;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!*p || *p == '#' || *p == '[')
        return 0;
    eq = strchr(p, '=');
    if (!eq)
        return 0;
    e = eq;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    k = (size_t)(e - p);
    if (!k || k >= kn)
        return 0;
    memcpy(key, p, k);
    key[k] = 0;
    p = eq + 1;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '"') {
        const char *q = strchr(p + 1, '"');
        k = q ? (size_t)(q - p - 1) : strlen(p + 1);
        p++;
    } else {
        const char *h = strchr(p, '#');
        k = h ? (size_t)(h - p) : strlen(p);
        while (k && (p[k - 1] == ' ' || p[k - 1] == '\t'))
            k--;
    }
    if (k >= vn)
        k = vn - 1;
    memcpy(val, p, k);
    val[k] = 0;
    return 1;
}

static void settings_path(WCHAR *out)
{
    join(out, s_game_dir, GAME_TOML);
}

static void settings_show(int fullscreen, int res, int vsync, int sdl, int sdl_audio, int mute, int fps, int msaa,
                          int emulated)
{
    CheckDlgButton(s_wnd, ID_MSAA, msaa ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(ctl(ID_RENDERER), CB_SETCURSEL, (WPARAM)(emulated ? 1 : 0), 0);
    CheckRadioButton(s_wnd, ID_WINDOWED, ID_FULLSCREEN, fullscreen ? ID_FULLSCREEN : ID_WINDOWED);
    SendMessageW(ctl(ID_RESOLUTION), CB_SETCURSEL, (WPARAM)res, 0);
    CheckDlgButton(s_wnd, ID_VSYNC, vsync ? BST_CHECKED : BST_UNCHECKED);
    SendMessageW(ctl(ID_INPUT), CB_SETCURSEL, (WPARAM)(sdl ? 1 : 0), 0);
    SendMessageW(ctl(ID_AUDIO), CB_SETCURSEL, (WPARAM)(sdl_audio ? 1 : 0), 0);
    CheckDlgButton(s_wnd, ID_MUTE, mute ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(s_wnd, ID_SHOWFPS, fps ? BST_CHECKED : BST_UNCHECKED);
}

/* A Steam Deck (Steam sets SteamDeck=1 for what it starts there). */
static int on_steam_deck(void)
{
    WCHAR v[8];
    return GetEnvironmentVariableW(L"SteamDeck", v, 8) == 1 && v[0] == L'1';
}

static void settings_load(void)
{
    WCHAR p[MAX_PATH];
    Lines l;
    int i, fullscreen = 0, vsync = 1, sdl = 0, sdl_audio = 0, mute = 0, fps = 1, w = 1280, h = 720, res = 0, in_section = 0;
    int msaa = 0, emulated = 0;
    settings_path(p);
    if (toml_read(p, &l)) {
        for (i = 0; i < l.n; i++) {
            char key[64], val[256];
            const char *s = l.v[i];
            while (*s == ' ' || *s == '\t')
                s++;
            if (*s == '[')
                in_section = 1;
            if (in_section || !toml_kv(l.v[i], key, sizeof key, val, sizeof val))
                continue;
            if (!strcmp(key, "fullscreen")) fullscreen = !strcmp(val, "true");
            else if (!strcmp(key, "vsync")) vsync = !strcmp(val, "true");
            else if (!strcmp(key, "audio_mute")) mute = !strcmp(val, "true");
            else if (!strcmp(key, "input_backend")) sdl = !_stricmp(val, "sdl");
            else if (!strcmp(key, "audio_backend")) sdl_audio = !_stricmp(val, "sdl");
            else if (!strcmp(key, "show_fps")) fps = !strcmp(val, "true");
            else if (!strcmp(key, "native_2x_msaa")) msaa = !strcmp(val, "true");
            else if (!strcmp(key, "native_renderer")) emulated = !_stricmp(val, "off");
            else if (!strcmp(key, "window_width")) w = atoi(val);
            else if (!strcmp(key, "window_height")) h = atoi(val);
        }
        lines_free(&l);
    } else if (on_steam_deck()) {
        fullscreen = 1;  /* no settings yet: the Deck's screen is the window */
    }
    for (i = 0; i < N_RES; i++)
        if (k_res[i].w == w && k_res[i].h == h)
            res = i;
    settings_show(fullscreen, res, vsync, sdl, sdl_audio, mute, fps, msaa, emulated);
    s_settings_dirty = 0;
    set_text(ID_SETTINGS_STATUS, L"");
}

static int settings_save(void)
{
    enum { NK = 13 };
    static const char *keys[NK] = { "gpu_plugin", "input_backend", "resolution", "resolution_scale", "window_width",
                                    "window_height", "fullscreen", "vsync", "audio_mute", "audio_backend", "show_fps",
                                    "native_2x_msaa", "native_renderer" };
    char vals[NK][64];
    int done[NK] = { 0 };
    WCHAR p[MAX_PATH], tmp[MAX_PATH];
    Lines l;
    FILE *f;
    int i, k, sel = (int)SendMessageW(ctl(ID_RESOLUTION), CB_GETCURSEL, 0, 0), insert_at, in_section = 0;
    int sdl = (int)SendMessageW(ctl(ID_INPUT), CB_GETCURSEL, 0, 0) == 1;

    if (!s_game_dir[0] || !dir_exists(s_game_dir)) {
        set_text(ID_SETTINGS_STATUS, L"The game folder does not exist; choose it on the Play tab first.");
        return 0;
    }
    if (sel < 0 || sel >= N_RES)
        sel = 0;
    strcpy_s(vals[0], 64, "\"xenos\"");
    strcpy_s(vals[1], 64, sdl ? "\"sdl\"" : "\"xinput\"");
    strcpy_s(vals[2], 64, "\"720p\"");
    sprintf_s(vals[3], 64, "%d", k_res[sel].scale);
    sprintf_s(vals[4], 64, "%d", k_res[sel].w);
    sprintf_s(vals[5], 64, "%d", k_res[sel].h);
    strcpy_s(vals[6], 64, IsDlgButtonChecked(s_wnd, ID_FULLSCREEN) == BST_CHECKED ? "true" : "false");
    strcpy_s(vals[7], 64, IsDlgButtonChecked(s_wnd, ID_VSYNC) == BST_CHECKED ? "true" : "false");
    strcpy_s(vals[8], 64, IsDlgButtonChecked(s_wnd, ID_MUTE) == BST_CHECKED ? "true" : "false");
    strcpy_s(vals[10], 64, IsDlgButtonChecked(s_wnd, ID_SHOWFPS) == BST_CHECKED ? "true" : "false");
    strcpy_s(vals[9], 64, SendMessageW(ctl(ID_AUDIO), CB_GETCURSEL, 0, 0) == 1 ? "\"sdl\"" : "\"xaudio2\"");
    strcpy_s(vals[11], 64, IsDlgButtonChecked(s_wnd, ID_MSAA) == BST_CHECKED ? "true" : "false");
    strcpy_s(vals[12], 64, SendMessageW(ctl(ID_RENDERER), CB_GETCURSEL, 0, 0) == 1 ? "\"off\"" : "\"main\"");

    settings_path(p);
    if (!toml_read(p, &l))
        lines_insert(&l, 0, "# WWE SmackDown vs. Raw 2011 - settings (the launcher rewrites this file)");
    /* Known top-level keys are rewritten where they are (a repeat is dropped);
     * the rest is kept. Missing keys go before the first [section]. */
    insert_at = -1;
    for (i = 0; i < l.n; i++) {
        char key[64], val[256], line[128];
        const char *s = l.v[i];
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s == '[' && !in_section) {
            in_section = 1;
            insert_at = i;
        }
        if (in_section || !toml_kv(l.v[i], key, sizeof key, val, sizeof val))
            continue;
        for (k = 0; k < NK; k++)
            if (!strcmp(key, keys[k]))
                break;
        if (k == NK)
            continue;
        free(l.v[i]);
        l.v[i] = NULL;
        if (!done[k]) {
            sprintf_s(line, 128, "%s = %s", keys[k], vals[k]);
            l.v[i] = _strdup(line);
            done[k] = 1;
        }
    }
    if (insert_at < 0)
        insert_at = l.n;
    while (insert_at > 0 && (!l.v[insert_at - 1] || !l.v[insert_at - 1][0]))
        insert_at--;                                 /* before the blank lines */
    for (k = 0; k < NK; k++) {
        char line[128];
        if (done[k])
            continue;
        sprintf_s(line, 128, "%s = %s", keys[k], vals[k]);
        lines_insert(&l, insert_at++, line);
    }
    if (in_section && insert_at < l.n && l.v[insert_at] && l.v[insert_at][0])
        lines_insert(&l, insert_at, "");

    swprintf_s(tmp, MAX_PATH, L"%s.tmp", p);
    if (_wfopen_s(&f, tmp, L"wb") || !f) {
        lines_free(&l);
        set_text(ID_SETTINGS_STATUS, L"The settings could not be saved (is the game folder read-only?).");
        return 0;
    }
    for (i = 0; i < l.n; i++)
        if (l.v[i])
            fprintf(f, "%s\n", l.v[i]);
    fclose(f);
    lines_free(&l);
    if (!MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp);
        set_text(ID_SETTINGS_STATUS, L"The settings could not be saved.");
        return 0;
    }
    s_settings_dirty = 0;
    set_text(ID_SETTINGS_STATUS, game_running() ? L"Settings saved. They apply the next time the game starts."
                                                : L"Settings saved to " GAME_TOML L".");
    return 1;
}

static void settings_defaults(void)
{
    settings_show(0, 0, 1, 0, 0, 0, 1, 0, 0);
    s_settings_dirty = 1;
    set_text(ID_SETTINGS_STATUS, L"Defaults restored. Press Save or Play to keep them.");
}

/* ── play tab ──────────────────────────────────────────────────────────── */

static void refresh_play(void)
{
    const WCHAR *t;
    int ready = 0;
    set_text(ID_GAMEDIR, s_game_dir);
    if (!dir_exists(s_game_dir))
        t = L"The game folder does not exist. Choose another one, or install the game on the Install tab.";
    else if (!is_game_folder(s_game_dir))
        t = L"Game files not installed \x2014 use the Install tab.";
    else if (!has_program(s_game_dir))
        t = GAME_EXE L" is missing from the game folder \x2014 copy the port's program files into it, "
            L"or install again from the Install tab.";
    else if (game_running())
        t = L"Game running\x2026";
    else {
        t = L"Ready.";
        ready = 1;
    }
    set_text(ID_PLAY_STATUS, t);
    EnableWindow(ctl(ID_PLAY), ready && !s_busy);
}

static DWORD WINAPI game_watch(LPVOID p)
{
    HANDLE h = (HANDLE)p;
    DWORD code = 0;
    WaitForSingleObject(h, INFINITE);
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    PostMessageW(s_wnd, WM_APP_GAMEEND, code, 0);
    return 0;
}

static void play(void)
{
    WCHAR exe[MAX_PATH], cmd[MAX_PATH + 4];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (game_running()) {
        set_text(ID_PLAY_STATUS, L"The game is already running.");
        return;
    }
    if (!is_game_folder(s_game_dir) || !has_program(s_game_dir)) {
        refresh_play();
        MessageBoxW(s_wnd, is_game_folder(s_game_dir)
                               ? GAME_EXE L" is missing from the game folder."
                               : L"The game folder has no default.xex. Install the game from your disc image on the "
                                 L"Install tab first.",
                    WINDOW_TITLE, MB_OK | MB_ICONWARNING);
        return;
    }
    if (s_settings_dirty)
        settings_save();
    join(exe, s_game_dir, GAME_EXE);
    swprintf_s(cmd, MAX_PATH + 4, L"\"%s\"", exe);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, s_game_dir, &si, &pi)) {
        WCHAR m[MAX_PATH + 64];
        swprintf_s(m, MAX_PATH + 64, L"Windows could not start the game (error %lu).", GetLastError());
        set_text(ID_PLAY_STATUS, m);
        return;
    }
    CloseHandle(pi.hThread);
    if (s_game_proc)
        CloseHandle(s_game_proc);
    s_game_proc = pi.hProcess;
    save_launcher_ini();
    if (IsDlgButtonChecked(s_wnd, ID_CLOSE_ON_PLAY) == BST_CHECKED) {
        DestroyWindow(s_wnd);
        return;
    }
    {
        HANDLE dup;
        if (DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &dup,
                            SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0))
            CloseHandle(CreateThread(NULL, 0, game_watch, dup, 0, NULL));
    }
    set_text(ID_PLAY_STATUS, L"Game running\x2026");
    EnableWindow(ctl(ID_PLAY), FALSE);
}

/* ── folder / file pickers ─────────────────────────────────────────────── */

static int pick_folder(const WCHAR *title, WCHAR *out)
{
    IFileOpenDialog *d;
    IShellItem *it;
    PWSTR p = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    d->lpVtbl->SetOptions(d, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->lpVtbl->SetTitle(d, title);
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            wcscpy_s(out, MAX_PATH, p);
            CoTaskMemFree(p);
            ok = 1;
        }
        it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
    return ok;
}

static int pick_image(WCHAR *out)
{
    OPENFILENAMEW of;
    WCHAR buf[MAX_PATH] = L"";
    memset(&of, 0, sizeof of);
    of.lStructSize = sizeof of;
    of.hwndOwner = s_wnd;
    of.lpstrFilter = L"Xbox 360 disc images (*.iso, *.xiso)\0*.iso;*.xiso\0All files\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = MAX_PATH;
    of.lpstrTitle = L"Choose your " GAME_TITLE L" disc image";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&of))
        return 0;
    wcscpy_s(out, MAX_PATH, buf);
    return 1;
}

/* ── window ────────────────────────────────────────────────────────────── */

#define HEADER_H 64      /* dark title band */
#define DY       64      /* page content sits this much lower than laid out */
#define CLIENT_W 620
#define CLIENT_H 600     /* below the band */

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm;
    if (s_font) DeleteObject(s_font);
    if (s_big) DeleteObject(s_big);
    if (s_title) DeleteObject(s_title);
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, (UINT)s_dpi);
    s_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    ncm.lfMessageFont.lfHeight = -MulDiv(15, s_dpi, 72);
    s_big = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    ncm.lfMessageFont.lfHeight = -MulDiv(17, s_dpi, 72);
    s_title = CreateFontIndirectW(&ncm.lfMessageFont);
}

static HWND add(int tab, const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | style, S(x), S(y + DY), S(w), S(h), s_wnd,
                             (HMENU)(INT_PTR)id, s_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)s_font, TRUE);
    if (tab >= 0 && s_nctl[tab] < 32)
        s_ctl[tab][s_nctl[tab]++] = c;
    if (s_nplaced < 128) {
        Placed *pl = &s_placed[s_nplaced++];
        pl->h = c; pl->x = x; pl->y = y; pl->w = w; pl->hh = h; pl->big = 0;
    }
    return c;
}

static void set_big(HWND c)
{
    int i;
    SendMessageW(c, WM_SETFONT, (WPARAM)s_big, TRUE);
    for (i = 0; i < s_nplaced; i++)
        if (s_placed[i].h == c)
            s_placed[i].big = 1;
}

/* After a DPI change: new fonts, every control moved and resized. */
static void relayout(void)
{
    int i;
    make_fonts();
    for (i = 0; i < s_nplaced; i++) {
        Placed *pl = &s_placed[i];
        SetWindowPos(pl->h, NULL, S(pl->x), S(pl->y + DY), S(pl->w), S(pl->hh), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(pl->h, WM_SETFONT, (WPARAM)(pl->big ? s_big : s_font), TRUE);
    }
    InvalidateRect(s_wnd, NULL, TRUE);
}

static void update_free_space(void)
{
    WCHAR target[MAX_PATH], t[256], a[32], b[32];
    uint64_t freeb = 0, need = s_image_bytes ? s_image_bytes : GAME_BYTES_EST;
    GetWindowTextW(ctl(ID_TARGET), target, MAX_PATH);
    fmt_size(a, 32, need);
    if (!free_space(target, &freeb)) {
        swprintf_s(t, 256, L"The game needs about %s of free space.", a);
    } else {
        fmt_size(b, 32, freeb);
        swprintf_s(t, 256, freeb < need ? L"The game needs about %s; that drive has only %s free. (Files already "
                                          L"installed there are not copied again.)"
                                        : L"The game needs about %s of free space; that drive has %s free.", a, b);
    }
    set_text(ID_FREE, t);
}

static void saves_refresh(void);
static void pt_refresh(void);
static void mv_refresh(void);
static volatile LONG s_up_busy;

static void show_tab(int t)
{
    int i, k;
    for (k = 0; k < TAB_COUNT; k++)
        for (i = 0; i < s_nctl[k]; i++)
            ShowWindow(s_ctl[k][i], k == t ? SW_SHOW : SW_HIDE);
    if (t == TAB_PLAY)
        refresh_play();
    if (t == TAB_INSTALL)
        update_free_space();
    if (t == TAB_SAVES)
        saves_refresh();
    if (t == TAB_PAINT)
        pt_refresh();
    if (t == TAB_MOVIES)
        mv_refresh();
    if (t == TAB_PLAY && !s_up_busy)
        ShowWindow(ctl(ID_UP_PROGRESS), SW_HIDE);
    TabCtrl_SetCurSel(s_tab, t);
    /* The page area in full: hidden controls (group boxes, the save list,
       the logo grid) otherwise leave their pixels behind. */
    RedrawWindow(s_wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

static void saves_setup(HWND list);
static void saves_refresh(void);
static HWND pt_setup_grid(void);
static void mv_setup(void);
static void up_setup(void);

#define X0 28
static void build_ui(void)
{
    TCITEMW ti;
    HWND h;
    WCHAR v[64];
    int i;
    static const WCHAR *names[TAB_COUNT] = { L"Play", L"Settings", L"Install", L"DLC", L"Saves", L"Paint Tool", L"Movies" };

    /* Just the strip of tabs; the pages below are plain window. */
    s_tab = add(-1, WC_TABCONTROLW, L"", WS_VISIBLE | WS_CLIPSIBLINGS | TCS_FOCUSNEVER, 12, 10, 596, 28, ID_TAB);
    for (i = 0; i < TAB_COUNT; i++) {
        ti.mask = TCIF_TEXT;
        ti.pszText = (WCHAR *)names[i];
        TabCtrl_InsertItem(s_tab, i, &ti);
    }

    /* Play */
    add(TAB_PLAY, L"Static", L"The PC port of the 2010 Xbox 360 game, statically recompiled from the original game code.",
        SS_LEFT, X0, 60, 560, 24, 0);
    add(TAB_PLAY, L"Button", L"Game folder", BS_GROUPBOX, X0, 104, 560, 70, 0);
    add(TAB_PLAY, L"Edit", L"", ES_READONLY | ES_AUTOHSCROLL | WS_BORDER, X0 + 14, 130, 430, 24, ID_GAMEDIR);
    add(TAB_PLAY, L"Button", L"Change\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 129, 92, 26, ID_GAMEDIR_CHANGE);
    add(TAB_PLAY, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 190, 560, 40, ID_PLAY_STATUS);
    h = add(TAB_PLAY, L"Button", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, X0, 246, 200, 52, ID_PLAY);
    set_big(h);
    add(TAB_PLAY, L"Button", L"Close the launcher when the game starts", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0, 312, 400, 24, ID_CLOSE_ON_PLAY);
    add(TAB_PLAY, L"Static", L"Display, resolution, controller and audio options are on the Settings tab; they "
                             L"apply the next time the game starts.",
        SS_LEFT, X0, 356, 560, 40, 0);
    up_setup();

    /* Settings */
    add(TAB_SETTINGS, L"Button", L"Display", BS_GROUPBOX, X0, 50, 560, 188, 0);
    add(TAB_SETTINGS, L"Static", L"Mode", SS_LEFT, X0 + 16, 76, 120, 20, 0);
    add(TAB_SETTINGS, L"Button", L"Windowed", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, X0 + 150, 72, 110, 24, ID_WINDOWED);
    add(TAB_SETTINGS, L"Button", L"Fullscreen", BS_AUTORADIOBUTTON, X0 + 270, 72, 110, 24, ID_FULLSCREEN);
    add(TAB_SETTINGS, L"Static", L"Resolution", SS_LEFT, X0 + 16, 108, 120, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
            X0 + 150, 104, 300, 200, ID_RESOLUTION);
    for (i = 0; i < N_RES; i++)
        SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)k_res[i].label);
    add(TAB_SETTINGS, L"Static", L"The window's size; the picture is rendered to fit it. In game, MY WWE \x2192 "
                                 L"OPTIONS \x2192 GRAPHICS changes these at once.", SS_LEFT, X0 + 150, 134, 396, 32, 0);
    add(TAB_SETTINGS, L"Button", L"VSync (no tearing; waits for the monitor's refresh)", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0 + 16, 172, 360, 24, ID_VSYNC);
    add(TAB_SETTINGS, L"Button", L"Show FPS (F2)", BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 400, 172, 150, 24, ID_SHOWFPS);
    add(TAB_SETTINGS, L"Button", L"Anti-aliasing (smoother edges)", BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 16, 206, 220, 24,
        ID_MSAA);
    add(TAB_SETTINGS, L"Static", L"Renderer", SS_LEFT, X0 + 270, 210, 80, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 350, 206, 196, 200,
            ID_RENDERER);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Native (recommended)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Emulated");
    add(TAB_SETTINGS, L"Button", L"Input and audio", BS_GROUPBOX, X0, 248, 560, 90, 0);
    add(TAB_SETTINGS, L"Static", L"Controller API", SS_LEFT, X0 + 16, 276, 130, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 150, 272, 300, 200, ID_INPUT);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"XInput (Xbox controllers)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"SDL (other gamepads)");
    add(TAB_SETTINGS, L"Static", L"Audio output", SS_LEFT, X0 + 16, 308, 130, 20, 0);
    h = add(TAB_SETTINGS, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, X0 + 150, 304, 300, 200, ID_AUDIO);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"XAudio2 (recommended)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"SDL");
    add(TAB_SETTINGS, L"Button", L"Mute", BS_AUTOCHECKBOX | WS_TABSTOP, X0 + 464, 306, 84, 24, ID_MUTE);
    add(TAB_SETTINGS, L"Button", L"Entrance music", BS_GROUPBOX, X0, 348, 560, 80, 0);
    add(TAB_SETTINGS, L"Static", L"Create An Entrance \x2192 Music \x2192 USER PLAYLIST plays your own songs "
                                 L"(.mp3): give each song its own folder in the game's Music folder.",
        SS_LEFT, X0 + 16, 368, 370, 52, 0);
    add(TAB_SETTINGS, L"Button", L"Open Music folder", BS_PUSHBUTTON | WS_TABSTOP, X0 + 400, 374, 146, 28,
        ID_MUSIC_OPEN);
    add(TAB_SETTINGS, L"Button", L"Controls", BS_GROUPBOX, X0, 436, 560, 72, 0);
    add(TAB_SETTINGS, L"Static", L"An Xbox 360 controller is supported through XInput and works as on the console: "
                                 L"Start = Start, Back = Back, A/B/X/Y, bumpers, triggers and sticks unchanged. "
                                 L"Connect it before starting the game.", SS_LEFT, X0 + 16, 458, 530, 44, 0);
    add(TAB_SETTINGS, L"Button", L"Restore defaults", BS_PUSHBUTTON | WS_TABSTOP, X0, 518, 140, 30, ID_DEFAULTS);
    add(TAB_SETTINGS, L"Button", L"Save", BS_PUSHBUTTON | WS_TABSTOP, X0 + 452, 518, 108, 30, ID_SAVE);
    add(TAB_SETTINGS, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 556, 560, 36, ID_SETTINGS_STATUS);

    /* Install */
    add(TAB_INSTALL, L"Static", L"1.  Your " GAME_TITLE L" disc image (Xbox 360 ISO or XISO)",
        SS_LEFT, X0, 54, 560, 20, 0);
    add(TAB_INSTALL, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 20, 78, 424, 24, ID_IMAGE);
    add(TAB_INSTALL, L"Button", L"Browse\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 77, 92, 26, ID_IMAGE_BROWSE);
    add(TAB_INSTALL, L"Static", L"The disc image is only read, never changed.", SS_LEFT, X0 + 20, 106, 520, 20, 0);
    add(TAB_INSTALL, L"Static", L"2.  Install to", SS_LEFT, X0, 138, 560, 20, 0);
    add(TAB_INSTALL, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 20, 162, 424, 24, ID_TARGET);
    add(TAB_INSTALL, L"Button", L"Browse\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 161, 92, 26, ID_TARGET_BROWSE);
    add(TAB_INSTALL, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0 + 20, 190, 526, 36, ID_FREE);
    add(TAB_INSTALL, L"Button", L"Install", BS_DEFPUSHBUTTON | WS_TABSTOP, X0, 236, 140, 36, ID_INSTALL);
    add(TAB_INSTALL, L"Button", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, X0 + 150, 236, 100, 36, ID_CANCEL);
    add(TAB_INSTALL, PROGRESS_CLASSW, L"", PBS_SMOOTH, X0, 288, 560, 20, ID_PROGRESS);
    SendMessageW(ctl(ID_PROGRESS), PBM_SETRANGE32, 0, 1000);
    add(TAB_INSTALL, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 318, 560, 76, ID_INSTALL_STATUS);
    add(TAB_INSTALL, L"Static", L"Files already installed with the right size are skipped, so a stopped "
                                L"installation continues where it left off.", SS_LEFT, X0, 410, 560, 36, 0);

    /* DLC */
    add(TAB_DLC, L"Static", L"Downloadable content (DLC) and title updates as downloaded on the Xbox 360: "
                            L"the packages themselves, or .zip / .rar / .7z archives of them.",
        SS_LEFT, X0, 50, 560, 36, 0);
    add(TAB_DLC, L"Static", L"Folder with the DLC", SS_LEFT, X0, 96, 560, 20, 0);
    add(TAB_DLC, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 20, 120, 424, 24, ID_DLC_DIR);
    add(TAB_DLC, L"Button", L"Browse\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 119, 92, 26, ID_DLC_BROWSE);
    add(TAB_DLC, L"Button", L"Install DLC", BS_DEFPUSHBUTTON | WS_TABSTOP, X0, 160, 140, 36, ID_DLC_INSTALL);
    add(TAB_DLC, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 208, 560, 56, ID_DLC_STATUS);
    add(TAB_DLC, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 270, 560, 110, ID_DLC_LIST);
    add(TAB_DLC, L"Static", L"The packages are copied into the game folder's DLC folder and unpacked the next "
                            L"time the game starts. Title updates are not needed and are skipped.",
        SS_LEFT, X0, 396, 560, 40, 0);

    /* Saves */
    add(TAB_SAVES, L"Static", L"Your saves, one file each in the game folder's Saves folder. The main save keeps "
                              L"settings, unlocks and progress and ties the rest together.",
        SS_LEFT, X0, 50, 560, 36, 0);
    saves_setup(add(TAB_SAVES, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                    X0, 92, 560, 206, ID_SV_LIST));
    add(TAB_SAVES, L"Button", L"Back up all", BS_PUSHBUTTON | WS_TABSTOP, X0, 308, 130, 30, ID_SV_BACKUP);
    add(TAB_SAVES, L"Button", L"Restore backup\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 140, 308, 140, 30, ID_SV_RESTORE);
    add(TAB_SAVES, L"Button", L"Backups folder", BS_PUSHBUTTON | WS_TABSTOP, X0 + 290, 308, 130, 30, ID_SV_BACKUPS);
    add(TAB_SAVES, L"Button", L"Open save folder", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 308, 130, 30, ID_SV_OPEN);
    add(TAB_SAVES, L"Button", L"Export selected\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0, 346, 130, 30, ID_SV_EXPORT);
    add(TAB_SAVES, L"Button", L"Import\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 140, 346, 140, 30, ID_SV_IMPORT);
    add(TAB_SAVES, L"Button", L"Delete selected", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 346, 130, 30, ID_SV_DELETE);
    add(TAB_SAVES, L"Static", L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, X0, 388, 560, 40, ID_SV_STATUS);

    /* Paint Tool */
    add(TAB_PAINT, L"Static", L"The Paint Tool's 20 logos (CREATE A SUPERSTAR > PAINT TOOL in the game). Export one "
                              L"as a PNG, or import any image into a logo; it is fitted into 256 \x00D7 256.",
        SS_LEFT, X0, 50, 560, 32, 0);
    pt_setup_grid();
    add(TAB_PAINT, L"Button", L"Export PNG\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 86, 130, 30, ID_PT_EXPORT);
    add(TAB_PAINT, L"Button", L"Import image\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 124, 130, 30, ID_PT_IMPORT);
    add(TAB_PAINT, L"Button", L"Delete logo", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 162, 130, 30, ID_PT_DELETE);
    add(TAB_PAINT, L"Button", L"Export all\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 220, 130, 30, ID_PT_EXPORTALL);
    add(TAB_PAINT, L"Button", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, X0 + 430, 258, 130, 30, ID_PT_REFRESH);
    add(TAB_PAINT, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 424, 560, 48, ID_PT_STATUS);

    /* Movies */
    mv_setup();

    CheckDlgButton(s_wnd, ID_CLOSE_ON_PLAY,
                   GetPrivateProfileIntW(L"Launcher", L"CloseOnPlay", 0, s_launcher_ini) ? BST_CHECKED : BST_UNCHECKED);
    {
        WCHAR img[MAX_PATH];
        GetPrivateProfileStringW(L"Launcher", L"LastImage", L"", img, MAX_PATH, s_launcher_ini);
        set_text(ID_IMAGE, img);
    }
    set_text(ID_TARGET, s_launcher_dir);
    {
        /* DLC folder: the last one used, else "UpdatesAndDLC" beside the game folder. */
        WCHAR d[MAX_PATH], parent[MAX_PATH], *slash;
        GetPrivateProfileStringW(L"Launcher", L"LastDlcFolder", L"", d, MAX_PATH, s_launcher_ini);
        if (!d[0] && s_game_dir[0]) {
            wcscpy_s(parent, MAX_PATH, s_game_dir);
            slash = wcsrchr(parent, L'\\');
            if (slash && slash[1] == 0) {                 /* trailing backslash */
                *slash = 0;
                slash = wcsrchr(parent, L'\\');
            }
            if (slash) {
                *slash = 0;
                if (join(d, parent, L"UpdatesAndDLC") && !dir_exists(d))
                    d[0] = 0;
            }
        }
        set_text(ID_DLC_DIR, d);
    }
    settings_load();
}

/* ── DLC ──────────────────────────────────────────────────────────────── */

/* The DLC tab copies the game's Xbox 360 content packages (STFS: the files
 * as downloaded, or inside .zip / .rar / .7z archives, unpacked with Windows'
 * tar) into <game>\DLC; the game unpacks new ones there at startup (dlc.h). */

#define GAME_TITLE_ID 0x5451085Du

static struct {
    WCHAR src[MAX_PATH], game[MAX_PATH];
    int copied, present, updates, failed;
    WCHAR names[1200];
} s_dlc;
static HANDLE s_dlc_worker;

static void dlc_post(int done, const WCHAR *fmt, ...)
{
    WCHAR buf[1600];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 1600, fmt, ap);
    va_end(ap);
    if (s_console) {
        con_print(buf);
        if (done && s_dlc.names[0])
            con_print(s_dlc.names);
        con_print(L"\n");
        return;
    }
    PostMessageW(s_wnd, WM_APP_DLC, done, (LPARAM)wdup(buf));
}

/* 1: DLC for this game, 2: a title update for it, 0: anything else. */
static int stfs_kind(const WCHAR *path, WCHAR *name, size_t name_len)
{
    unsigned char h[0x491];
    DWORD got = 0;
    uint32_t type;
    size_t i;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return 0;
    ReadFile(f, h, sizeof h, &got, NULL);
    CloseHandle(f);
    if (got < sizeof h || (memcmp(h, "LIVE", 4) && memcmp(h, "PIRS", 4) && memcmp(h, "CON ", 4)))
        return 0;
    if (be32(h + 0x360) != GAME_TITLE_ID)
        return 0;
    for (i = 0; i + 1 < name_len && i < 0x40; i++) {      /* display name, UTF-16BE */
        name[i] = (WCHAR)(h[0x411 + 2 * i] << 8 | h[0x412 + 2 * i]);
        if (!name[i])
            break;
    }
    name[i] = 0;
    type = be32(h + 0x344);
    return type == 0x00000002 ? 1 : type == 0x000B0000 ? 2 : 0;
}

static int is_archive(const WCHAR *name)
{
    const WCHAR *ext = wcsrchr(name, L'.');
    return ext && (!_wcsicmp(ext, L".zip") || !_wcsicmp(ext, L".rar") || !_wcsicmp(ext, L".7z"));
}

/* Unpacks an archive with Windows' tar (bsdtar reads zip, rar and 7z). */
static int unpack(const WCHAR *archive, const WCHAR *to)
{
    WCHAR cmd[MAX_PATH * 3], sys[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf_s(cmd, MAX_PATH * 3, L"\"%s\\tar.exe\" -xf \"%s\" -C \"%s\"", sys, archive, to);
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

static void remove_tree(const WCHAR *dir)
{
    WCHAR from[MAX_PATH + 2];
    SHFILEOPSTRUCTW op;
    wcscpy_s(from, MAX_PATH, dir);
    from[wcslen(from) + 1] = 0;                        /* double-terminated */
    ZeroMemory(&op, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    SHFileOperationW(&op);
}

static void dlc_scan(const WCHAR *dir, int depth);

static void dlc_file(const WCHAR *path, const WCHAR *file_name)
{
    WCHAR name[80], dst[MAX_PATH], dlc_dir[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    int kind;
    if (is_archive(file_name)) {
        WCHAR base[MAX_PATH], tmp[MAX_PATH], sub[64];
        GetTempPathW(MAX_PATH, base);
        swprintf_s(sub, 64, L"svr2011_dlc_%lu", GetTickCount());
        if (!join(tmp, base, sub) || !mkdirs(tmp))
            return;
        dlc_post(0, L"Unpacking %s\x2026", file_name);
        if (unpack(path, tmp))
            dlc_scan(tmp, 0);
        else
            s_dlc.failed++;
        remove_tree(tmp);
        return;
    }
    kind = stfs_kind(path, name, 80);
    if (kind == 2) {
        s_dlc.updates++;
        return;
    }
    if (kind != 1)
        return;
    join(dlc_dir, s_dlc.game, L"DLC");
    mkdirs(dlc_dir);
    if (!join(dst, dlc_dir, file_name))
        return;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a) &&
        GetFileAttributesExW(dst, GetFileExInfoStandard, &b) &&
        a.nFileSizeLow == b.nFileSizeLow && a.nFileSizeHigh == b.nFileSizeHigh) {
        s_dlc.present++;
        return;
    }
    dlc_post(0, L"Copying %s\x2026", name[0] ? name : file_name);
    if (CopyFileW(path, dst, FALSE)) {
        s_dlc.copied++;
        if (wcslen(s_dlc.names) + wcslen(name) + 4 < 1200) {
            wcscat_s(s_dlc.names, 1200, L"\n  ");
            wcscat_s(s_dlc.names, 1200, name[0] ? name : file_name);
        }
    } else {
        s_dlc.failed++;
    }
}

static void dlc_scan(const WCHAR *dir, int depth)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (depth > 8 || !join(pattern, dir, L"*"))
        return;
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (!join(path, dir, fd.cFileName))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            dlc_scan(path, depth + 1);
        else
            dlc_file(path, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static DWORD WINAPI dlc_thread(LPVOID unused)
{
    (void)unused;
    dlc_scan(s_dlc.src, 0);
    if (!s_dlc.copied && !s_dlc.present) {
        dlc_post(1, L"No DLC for " GAME_TITLE L" was found in %s.%s", s_dlc.src,
                 s_dlc.failed ? L" (An archive could not be unpacked.)" : L"");
    } else {
        dlc_post(1, L"%d DLC package%s installed%s%s%s. %s%s", s_dlc.copied, s_dlc.copied == 1 ? L"" : L"s",
                 s_dlc.present ? L", " : L"",
                 s_dlc.present ? L"others already there" : L"",
                 s_dlc.updates ? L" (title update skipped: not needed)" : L"",
                 s_dlc.failed ? L"Some files could not be copied. " : L"",
                 s_dlc.copied ? L"They are set up the next time the game starts:" : L"");
    }
    return 0;
}

static void start_dlc(void)
{
    WCHAR t[MAX_PATH + 100];
    if (s_dlc_worker && WaitForSingleObject(s_dlc_worker, 0) == WAIT_TIMEOUT)
        return;
    GetWindowTextW(ctl(ID_DLC_DIR), s_dlc.src, MAX_PATH);
    if (!s_dlc.src[0] || !dir_exists(s_dlc.src)) {
        set_text(ID_DLC_STATUS, L"Choose the folder with the DLC first.");
        return;
    }
    if (!s_game_dir[0] || !has_program(s_game_dir)) {
        set_text(ID_DLC_STATUS, L"Install the game first (Install tab).");
        return;
    }
    wcscpy_s(s_dlc.game, MAX_PATH, s_game_dir);
    s_dlc.copied = s_dlc.present = s_dlc.updates = s_dlc.failed = 0;
    s_dlc.names[0] = 0;
    WritePrivateProfileStringW(L"Launcher", L"LastDlcFolder", s_dlc.src, s_launcher_ini);
    EnableWindow(ctl(ID_DLC_INSTALL), FALSE);
    EnableWindow(ctl(ID_DLC_BROWSE), FALSE);
    swprintf_s(t, MAX_PATH + 100, L"Looking for DLC in %s\x2026", s_dlc.src);
    set_text(ID_DLC_STATUS, t);
    set_text(ID_DLC_LIST, L"");
    if (s_dlc_worker)
        CloseHandle(s_dlc_worker);
    s_dlc_worker = CreateThread(NULL, 0, dlc_thread, NULL, 0, NULL);
    if (!s_dlc_worker) {
        EnableWindow(ctl(ID_DLC_INSTALL), TRUE);
        EnableWindow(ctl(ID_DLC_BROWSE), TRUE);
        set_text(ID_DLC_STATUS, L"The DLC installation could not be started.");
    }
}

/* ── Saves ────────────────────────────────────────────────────────────── */

/* The game keeps each save as one plain file in <game>\Saves: SaveData.dat
 * is the main save (settings, unlocks, progress) that ties the rest
 * together; created superstars (.cas), Paint Tool logos (.pt), replays (.rec),
 * highlight reels (.scn) and other created content are files of their own.
 * The hidden Saves\.info holds their Xbox 360 content headers (optional).
 * Earlier builds kept them in UserData\<profile>\5451085D\00000001\<save>\
 * SaveData.Dat: those are moved to Saves the first time the tab or the game
 * looks. Backups go to <game>\SaveBackups\<date time>. */

#define SAVES_DIR      L"Saves"

static int any_game_running(void)
{
    PROCESSENTRY32W pe;
    HANDLE snap;
    int found = 0;
    if (game_running())
        return 1;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return 0;
    pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe))
        do {
            if (!_wcsicmp(pe.szExeFile, GAME_EXE))
                found = 1;
        } while (!found && Process32NextW(snap, &pe));
    CloseHandle(snap);
    return found;
}

static int saves_dir(WCHAR *out)
{
    return s_game_dir[0] && join(out, s_game_dir, SAVES_DIR);
}

/* A shell copy / delete (whole folders), without dialogs. */
static int shell_op(UINT func, const WCHAR *from, const WCHAR *to, FILEOP_FLAGS extra)
{
    WCHAR f[MAX_PATH + 2], t[MAX_PATH + 2];
    SHFILEOPSTRUCTW op;
    wcscpy_s(f, MAX_PATH, from);
    f[wcslen(f) + 1] = 0;                             /* double-terminated */
    if (to) {
        wcscpy_s(t, MAX_PATH, to);
        t[wcslen(t) + 1] = 0;
    }
    ZeroMemory(&op, sizeof op);
    op.hwnd = s_wnd;
    op.wFunc = func;
    op.pFrom = f;
    op.pTo = to ? t : NULL;
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_SILENT | extra;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

static int is_save_entry(const WIN32_FIND_DATAW *fd)
{
    return fd->cFileName[0] != L'.' && _wcsicmp(fd->cFileName, L"desktop.ini") != 0
        && !(fd->dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
}

/* Moves one old-style package folder into Saves (as the game does). */
static void saves_migrate_package(const WCHAR *pkg, const WCHAR *name, const WCHAR *hdr_dir, const WCHAR *saves)
{
    WCHAR pat[MAX_PATH], file[MAX_PATH], only[MAX_PATH], dest[MAX_PATH], info[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int files = 0, dirs = 0;
    if (!join(dest, saves, name) || file_exists(dest) || dir_exists(dest) || !join(info, saves, L".info"))
        return;
    only[0] = 0;
    if (!join(pat, pkg, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            dirs++;
        else if (!_wcsicmp(fd.cFileName, L"__thumbnail.png")) {
            swprintf_s(b, MAX_PATH, L"%s\\%s.png", info, name);
            if (join(a, pkg, fd.cFileName))
                MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
        } else {
            files++;
            wcscpy_s(only, MAX_PATH, fd.cFileName);
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    if (files == 1 && !dirs) {
        if (!join(file, pkg, only) || !MoveFileExW(file, dest, 0))
            return;
        if (_wcsicmp(only, L"SaveData.Dat")) {       /* the file's own name, if not the usual */
            FILE *t;
            swprintf_s(a, MAX_PATH, L"%s\\%s.file", info, name);
            if (!_wfopen_s(&t, a, L"wb") && t) {
                char n8[MAX_PATH];
                WideCharToMultiByte(CP_UTF8, 0, only, -1, n8, sizeof n8, NULL, NULL);
                fwrite(n8, 1, strlen(n8), t);
                fclose(t);
            }
        }
        RemoveDirectoryW(pkg);
    } else if (!MoveFileExW(pkg, dest, 0))
        return;
    swprintf_s(a, MAX_PATH, L"%s\\%s.header", hdr_dir, name);
    swprintf_s(b, MAX_PATH, L"%s\\%s.header", info, name);
    MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
}

static void saves_migrate(void)
{
    WCHAR ud[MAX_PATH], pat[MAX_PATH], saves[MAX_PATH], info[MAX_PATH], prof[MAX_PATH], pkgs[MAX_PATH],
        hdrs[MAX_PATH], t[MAX_PATH], pkg[MAX_PATH], pat2[MAX_PATH];
    WIN32_FIND_DATAW fd, fd2;
    HANDLE f, f2;
    if (!saves_dir(saves) || !join(ud, s_game_dir, L"UserData") || !join(pat, ud, L"*") || !join(info, saves, L".info"))
        return;
    f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen(fd.cFileName) != 16
                || !wcscmp(fd.cFileName, L"0000000000000000"))
            continue;
        swprintf_s(t, MAX_PATH, L"%08X", TITLE_ID);
        if (!join(prof, ud, fd.cFileName) || !join(pkgs, prof, t) || !join(hdrs, pkgs, L"Headers")
                || !join(t, pkgs, L"00000001") || !join(pat2, t, L"*"))
            continue;
        wcscpy_s(pkgs, MAX_PATH, t);
        join(t, hdrs, L"00000001");
        wcscpy_s(hdrs, MAX_PATH, t);
        f2 = FindFirstFileW(pat2, &fd2);
        if (f2 == INVALID_HANDLE_VALUE)
            continue;
        mkdirs(info);
        do {
            if ((fd2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd2.cFileName[0] != L'.'
                    && join(pkg, pkgs, fd2.cFileName))
                saves_migrate_package(pkg, fd2.cFileName, hdrs, saves);
        } while (FindNextFileW(f2, &fd2));
        FindClose(f2);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static const WCHAR *save_kind(const WCHAR *name)
{
    const WCHAR *ext = wcsrchr(name, L'.');
    if (!_wcsicmp(name, L"SaveData.dat"))
        return L"Main save (ties the rest together)";
    if (ext && !_wcsicmp(ext, L".cas"))
        return L"Created Superstars";
    if (ext && !_wcsicmp(ext, L".pt"))
        return L"Paint Tool logos";
    if (ext && !_wcsicmp(ext, L".rec"))
        return L"Replays";
    if (ext && !_wcsicmp(ext, L".scn"))
        return L"Highlight reels";
    return L"Created content";
}

/* Total size and newest change of a save (a file, or a folder of files). */
static void entry_info(const WCHAR *path, const WIN32_FIND_DATAW *self, uint64_t *bytes, FILETIME *newest)
{
    WCHAR pat[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    if (!(self->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        *bytes += ((uint64_t)self->nFileSizeHigh << 32) | self->nFileSizeLow;
        if (CompareFileTime(&self->ftLastWriteTime, newest) > 0)
            *newest = self->ftLastWriteTime;
        return;
    }
    if (!join(pat, path, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (join(sub, path, fd.cFileName))
            entry_info(sub, &fd, bytes, newest);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static void saves_status(const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 1024, fmt, ap);
    va_end(ap);
    set_text(ID_SV_STATUS, buf);
}

static void list_columns(HWND list, const WCHAR *const *names, const int *widths, int n)
{
    LVCOLUMNW c;
    int i;
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < n; i++) {
        ZeroMemory(&c, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = i == 2 ? LVCFMT_RIGHT : LVCFMT_LEFT;
        c.cx = S(widths[i]);
        c.pszText = (WCHAR *)names[i];
        ListView_InsertColumn(list, i, &c);
    }
}

static void saves_setup(HWND list)
{
    static const WCHAR *const names[] = { L"Save file", L"Contents", L"Size", L"Changed" };
    static const int widths[] = { 190, 180, 66, 116 };
    list_columns(list, names, widths, 4);
}

static void fmt_when(WCHAR *out, size_t n, FILETIME ft)
{
    SYSTEMTIME st;
    FILETIME local;
    out[0] = 0;
    if ((ft.dwLowDateTime || ft.dwHighDateTime) && FileTimeToLocalFileTime(&ft, &local)
            && FileTimeToSystemTime(&local, &st))
        swprintf_s(out, n, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static void saves_refresh(void)
{
    HWND list = ctl(ID_SV_LIST);
    WCHAR saves[MAX_PATH], pat[MAX_PATH], path[MAX_PATH], size[32], when[32];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int n = 0;
    uint64_t total = 0;
    ListView_DeleteAllItems(list);
    if (!s_game_dir[0]) {
        saves_status(L"Install the game first (Install tab).");
        return;
    }
    if (!any_game_running())
        saves_migrate();
    if (!saves_dir(saves) || !join(pat, saves, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) {
        saves_status(L"No saves yet. The game makes them the first time it saves.");
        return;
    }
    do {
        LVITEMW it;
        uint64_t bytes = 0;
        FILETIME newest = { 0, 0 };
        if (!is_save_entry(&fd) || !join(path, saves, fd.cFileName))
            continue;
        entry_info(path, &fd, &bytes, &newest);
        total += bytes;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = !_wcsicmp(fd.cFileName, L"SaveData.dat") ? 0 : n;   /* the main save first */
        it.pszText = fd.cFileName;
        it.iItem = ListView_InsertItem(list, &it);
        ListView_SetItemText(list, it.iItem, 1, (WCHAR *)save_kind(fd.cFileName));
        fmt_size(size, 32, bytes);
        ListView_SetItemText(list, it.iItem, 2, size);
        fmt_when(when, 32, newest);
        ListView_SetItemText(list, it.iItem, 3, when);
        n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    fmt_size(size, 32, total);
    if (n)
        saves_status(L"%d save file%s, %s, in %s.", n, n == 1 ? L"" : L"s", size, saves);
    else
        saves_status(L"No saves yet. The game makes them the first time it saves.");
}

/* Refuses (with a message) while the game runs: it keeps its saves open. */
static int saves_locked(void)
{
    if (!any_game_running())
        return 0;
    MessageBoxW(s_wnd, L"Close the game first: it keeps its save files open while it runs.", WINDOW_TITLE,
                MB_OK | MB_ICONINFORMATION);
    return 1;
}

/* Copies the Saves folder to <game>\SaveBackups\<date time><suffix>. */
static int saves_backup_to(const WCHAR *suffix, WCHAR *out)
{
    WCHAR saves[MAX_PATH], base[MAX_PATH], name[96], mnt[MAX_PATH];
    SYSTEMTIME st;
    if (!saves_dir(saves) || !dir_exists(saves))
        return 0;
    GetLocalTime(&st);
    swprintf_s(name, 96, L"%04u-%02u-%02u %02u-%02u-%02u%s", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
               st.wSecond, suffix);
    if (!join(base, s_game_dir, L"SaveBackups") || !mkdirs(base) || !join(out, base, name) || dir_exists(out))
        return 0;
    if (!shell_op(FO_COPY, saves, out, 0))
        return 0;
    if (join(mnt, out, L".mount") && dir_exists(mnt))  /* the game's scratch, not saves */
        shell_op(FO_DELETE, mnt, NULL, 0);
    return 1;
}

static void saves_backup(void)
{
    WCHAR out[MAX_PATH];
    if (saves_locked())
        return;
    if (saves_backup_to(L"", out))
        saves_status(L"Backed up to %s.", out);
    else
        saves_status(L"Nothing was backed up (no saves yet, or the copy failed).");
}

/* A folder picker opening in `start`. */
static int pick_folder_in(const WCHAR *title, const WCHAR *start, WCHAR *out)
{
    IFileOpenDialog *d;
    IShellItem *it;
    PWSTR p = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    d->lpVtbl->SetOptions(d, FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->lpVtbl->SetTitle(d, title);
    if (start && dir_exists(start) && SUCCEEDED(SHCreateItemFromParsingName(start, NULL, &IID_IShellItem, (void **)&it))) {
        d->lpVtbl->SetFolder(d, it);
        it->lpVtbl->Release(it);
    }
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            wcscpy_s(out, MAX_PATH, p);
            CoTaskMemFree(p);
            ok = 1;
        }
        it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
    return ok;
}

/* Empties the Saves folder (its files, folders and .info; into the Recycle Bin). */
static void saves_clear(const WCHAR *saves)
{
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    if (!join(pat, saves, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !wcscmp(fd.cFileName, L".mount"))
            continue;
        if (join(p, saves, fd.cFileName))
            shell_op(FO_DELETE, p, NULL, FOF_ALLOWUNDO);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

static void saves_restore(void)
{
    WCHAR base[MAX_PATH], pick[MAX_PATH], saves[MAX_PATH], before[MAX_PATH], pat[MAX_PATH], from[MAX_PATH],
        to[MAX_PATH], t[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    int n = 0, had;
    if (saves_locked() || !join(base, s_game_dir, L"SaveBackups") || !saves_dir(saves))
        return;
    if (!pick_folder_in(L"Choose the backup to restore (a folder in SaveBackups)", base, pick))
        return;
    if (!join(pat, pick, L"*") || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do
        if (is_save_entry(&fd))
            n++;
    while (FindNextFileW(f, &fd));
    FindClose(f);
    if (!n) {
        saves_status(L"That folder has no saves in it.");
        return;
    }
    swprintf_s(t, MAX_PATH * 2, L"Replace your saves with the %d in\n%s?\n\nYour current saves are backed up first.",
               n, pick);
    if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    had = dir_exists(saves);
    if (had && !saves_backup_to(L" (before restore)", before)) {
        saves_status(L"Could not back up the current saves; nothing was restored.");
        return;
    }
    if (had)
        saves_clear(saves);
    mkdirs(saves);
    f = FindFirstFileW(pat, &fd);
    n = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !wcscmp(fd.cFileName, L".mount"))
            continue;
        if (join(from, pick, fd.cFileName) && join(to, saves, fd.cFileName) && shell_op(FO_COPY, from, to, 0)
                && fd.cFileName[0] != L'.')
            n++;
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    saves_refresh();
    if (had)
        saves_status(L"Restored %d save%s from %s. Your previous saves were backed up to %s.", n, n == 1 ? L"" : L"s",
                     pick, before);
    else
        saves_status(L"Restored %d save%s from %s.", n, n == 1 ? L"" : L"s", pick);
}

/* The selected rows' file names (col 0); returns the count. */
static int saves_selected(WCHAR names[][MAX_PATH], int max)
{
    HWND list = ctl(ID_SV_LIST);
    int i = -1, n = 0;
    while (n < max && (i = ListView_GetNextItem(list, i, LVNI_SELECTED)) >= 0) {
        ListView_GetItemText(list, i, 0, names[n], MAX_PATH);
        n++;
    }
    return n;
}

static void saves_export(void)
{
    static WCHAR names[64][MAX_PATH];
    WCHAR dest[MAX_PATH], saves[MAX_PATH], from[MAX_PATH], to[MAX_PATH];
    int n, i, done = 0, skipped = 0;
    if (saves_locked())
        return;
    n = saves_selected(names, 64);
    if (!n) {
        saves_status(L"Choose the saves to export in the list first (Ctrl+click for several).");
        return;
    }
    if (!saves_dir(saves) || !pick_folder(L"Choose where to export the saves to", dest))
        return;
    for (i = 0; i < n; i++) {
        if (!join(from, saves, names[i]) || !join(to, dest, names[i]))
            continue;
        if (file_exists(to) || dir_exists(to)) {
            skipped++;
            continue;
        }
        if (shell_op(FO_COPY, from, to, 0))
            done++;
    }
    saves_status(L"Exported %d save%s to %s.%s", done, done == 1 ? L"" : L"s", dest,
                 skipped ? L" Some were already there and were left as they were." : L"");
}

/* Picks save files to import (several at once). Returns the count; `buf`
 * gets them as full paths, one after the other (0-separated). */
static int pick_save_files(WCHAR *buf, size_t n)
{
    IFileOpenDialog *d;
    IShellItemArray *items;
    DWORD count = 0, i;
    size_t at = 0;
    int got = 0;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    d->lpVtbl->SetOptions(d, FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    d->lpVtbl->SetTitle(d, L"Choose the save files to import");
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResults(d, &items))) {
        items->lpVtbl->GetCount(items, &count);
        for (i = 0; i < count; i++) {
            IShellItem *it;
            PWSTR p = NULL;
            if (FAILED(items->lpVtbl->GetItemAt(items, i, &it)))
                continue;
            if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                size_t len = wcslen(p) + 1;
                if (at + len < n) {
                    wcscpy_s(buf + at, n - at, p);
                    at += len;
                    got++;
                }
                CoTaskMemFree(p);
            }
            it->lpVtbl->Release(it);
        }
        items->lpVtbl->Release(items);
    }
    d->lpVtbl->Release(d);
    return got;
}

static void saves_import(void)
{
    static WCHAR files[32768];
    WCHAR saves[MAX_PATH], to[MAX_PATH], before[MAX_PATH], t[512];
    const WCHAR *p, *name;
    int n, i, exists = 0, done = 0, backed = 0;
    if (saves_locked() || !saves_dir(saves))
        return;
    n = pick_save_files(files, 32768);
    if (!n)
        return;
    for (p = files, i = 0; i < n; i++, p += wcslen(p) + 1) {
        name = wcsrchr(p, L'\\');
        name = name ? name + 1 : p;
        if (join(to, saves, name) && (file_exists(to) || dir_exists(to)))
            exists++;
    }
    if (exists) {
        swprintf_s(t, 512, L"%d of the %d files replace saves you have. Replace them?\n\n"
                           L"Your current saves are backed up first.", exists, n);
        if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
        if (!saves_backup_to(L" (before import)", before)) {
            saves_status(L"Could not back up the current saves; nothing was imported.");
            return;
        }
        backed = 1;
    }
    mkdirs(saves);
    for (p = files, i = 0; i < n; i++, p += wcslen(p) + 1) {
        name = wcsrchr(p, L'\\');
        name = name ? name + 1 : p;
        if (join(to, saves, name) && CopyFileW(p, to, FALSE))
            done++;
    }
    saves_refresh();
    if (backed)
        saves_status(L"Imported %d save%s. Your previous saves were backed up to %s.", done, done == 1 ? L"" : L"s",
                     before);
    else
        saves_status(L"Imported %d save%s.", done, done == 1 ? L"" : L"s");
}

static void saves_delete(void)
{
    static WCHAR names[64][MAX_PATH];
    static const WCHAR *const extra[] = { L".header", L".file", L".png" };
    WCHAR saves[MAX_PATH], p[MAX_PATH], t[512];
    int n, i, j, main = 0, done = 0;
    if (saves_locked())
        return;
    n = saves_selected(names, 64);
    if (!n) {
        saves_status(L"Choose the saves to delete in the list first.");
        return;
    }
    for (i = 0; i < n; i++)
        if (!_wcsicmp(names[i], L"SaveData.dat"))
            main = 1;
    swprintf_s(t, 512, L"Move %d save%s to the Recycle Bin?%s", n, n == 1 ? L"" : L"s",
               main ? L"\n\nThis includes the main save: the game starts over (settings, unlocks and progress)."
                    : L"");
    if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | (main ? MB_ICONWARNING : MB_ICONQUESTION)) != IDYES)
        return;
    if (!saves_dir(saves))
        return;
    for (i = 0; i < n; i++) {
        if (join(p, saves, names[i]) && shell_op(FO_DELETE, p, NULL, FOF_ALLOWUNDO))
            done++;
        for (j = 0; j < 3; j++) {
            swprintf_s(p, MAX_PATH, L"%s\\.info\\%s%s", saves, names[i], extra[j]);
            if (file_exists(p))
                shell_op(FO_DELETE, p, NULL, FOF_ALLOWUNDO);
        }
    }
    saves_refresh();
    saves_status(L"Moved %d save%s to the Recycle Bin.", done, done == 1 ? L"" : L"s");
}

static void saves_open(void)
{
    WCHAR saves[MAX_PATH];
    if (saves_dir(saves) && mkdirs(saves))
        ShellExecuteW(s_wnd, L"open", saves, NULL, NULL, SW_SHOWNORMAL);
}

static void saves_open_backups(void)
{
    WCHAR b[MAX_PATH];
    if (join(b, s_game_dir, L"SaveBackups") && mkdirs(b))
        ShellExecuteW(s_wnd, L"open", b, NULL, NULL, SW_SHOWNORMAL);
}

/* ── Paint Tool ───────────────────────────────────────────────────────── */

/* Saves\00PaintTool.pt holds the Paint Tool's 20 logos (reverse-engineered
 * from the game: the loader sub_827B3630, the checksums sub_827B35C8 /
 * sub_827B3118). Big-endian:
 *   0        u32 magic 0x02A78E9A, u32 version 3
 *   8+k*S    slot k (S = 0x604CC); offsets below are from k*S:
 *     8..51      header (+36 used, +44 / +48 width / height = 256)
 *     52         the logo: 256 x 256 pixels, A R G B bytes (the editor's canvas)
 *     0x40034    the same as an 8-bit colour-mapped TGA (20-byte header,
 *                256 A R G B palette entries, 65536 indices, top-down)
 *     0x50448    the same as a DXT5 DDS (128-byte header, 64 KB), what the
 *                game shows
 *     0x604C8    saved at: u16 year, month, day, hour, minute, second, weekday+1 (UTC)
 *     0x604D0    u32 checksum: a sum of the slot's fields by type (pt_slot_sum)
 *   end-4    u32 magic + version + the 20 slot checksums
 * The game refuses the whole file when a checksum is off. */

#define PT_FILE    L"00PaintTool.pt"
#define PT_SLOTS   20
#define PT_SLOT    0x604CCu
#define PT_BYTES   (8u + PT_SLOTS * PT_SLOT + 4u)
#define PT_MAGIC   0x02A78E9Au
#define PT_W       256
#define PT_CANVAS  52u
#define PT_TGA     0x40034u
#define PT_DDS     0x50448u
#define PT_STAMP   0x604C8u
#define PT_SUM     0x604D0u

static const uint8_t k_pt_used_header[44] = {
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0E, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00 };
static const uint8_t k_pt_empty_header[44] = {
    0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00 };
static const uint8_t k_pt_tga_header[20] = {
    0x00, 0x01, 0x01, 0x00, 0x00, 0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00,
    0x08, 0x00, 0x04, 0x01 };
static const uint8_t k_pt_dds_header[128] = {
    0x44, 0x44, 0x53, 0x20, 0x7C, 0x00, 0x00, 0x00, 0x07, 0x10, 0x08, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, [76] = 0x20, [80] = 0x04, [84] = 0x44, 0x58, 0x54, 0x35,
    [108] = 0x02, 0x10 };

static uint8_t *s_pt;                     /* the loaded file, PT_BYTES */
static int      s_pt_sel = -1;
static HWND     s_pt_grid;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static uint8_t *pt_slot(uint8_t *f, int k) { return f + (size_t)k * PT_SLOT; }

/* The game's slot checksum (sub_827B3118): every field of the slot, added as
 * the type the game reads it as (signed bytes at +20..27). */
static uint32_t pt_slot_sum(const uint8_t *f, int k)
{
    const uint8_t *b = f + (size_t)k * PT_SLOT, *t = b + PT_TGA, *d = b + PT_DDS, *e = b + PT_STAMP;
    static const int words[] = { 8, 12, 16, 28, 32, 36, 40, 44, 48 };
    static const int tb[] = { 0, 1, 2, 7, 16, 17 }, th[] = { 3, 5, 8, 10, 12, 14, 18 };
    uint32_t s = 0;
    int i, j;
    for (i = 20; i < 28; i++)
        s += (uint32_t)(int32_t)(int8_t)b[i];
    for (i = 0; i < 9; i++)
        s += rd32(b + words[i]);
    for (i = 0; i < PT_W * PT_W; i++)
        s += rd32(b + PT_CANVAS + 4 * i);
    for (i = 0; i < 6; i++)
        s += t[tb[i]];
    for (i = 0; i < 7; i++)
        s += rd16(t + th[i]);
    for (i = 0; i < 256; i++)
        s += rd32(t + 20 + 4 * i);
    for (i = 0; i < PT_W * PT_W; i++)
        s += t[0x414 + i];
    for (i = 0; i < 32; i++)
        s += rd32(d + 4 * i);
    for (j = 0; j < 2048; j++) {
        const uint8_t *q = d + 128 + 32 * j;
        s += rd16(q) + rd16(q + 2) + rd32(q + 4) + rd16(q + 8) + rd16(q + 10) + rd32(q + 12)
           + rd16(q + 16) + rd16(q + 18) + rd32(q + 20) + rd16(q + 24) + rd16(q + 26) + rd32(q + 28);
    }
    s += rd16(e);
    for (i = 2; i < 8; i++)
        s += e[i];
    return s;
}

/* Recomputes every checksum (the game's sub_827B35C8). */
static void pt_fix_sums(uint8_t *f)
{
    uint32_t total = rd32(f) + rd32(f + 4);
    int k;
    for (k = 0; k < PT_SLOTS; k++) {
        uint32_t s = pt_slot_sum(f, k);
        wr32(pt_slot(f, k) + PT_SUM, s);
        total += s;
    }
    wr32(f + PT_BYTES - 4, total);
}

static int pt_valid(const uint8_t *f)
{
    uint32_t total;
    int k;
    if (rd32(f) != PT_MAGIC || rd32(f + 4) != 3)
        return 0;
    total = rd32(f) + rd32(f + 4);
    for (k = 0; k < PT_SLOTS; k++)
        total += rd32(f + (size_t)k * PT_SLOT + PT_SUM);
    return total == rd32(f + PT_BYTES - 4);
}

static int pt_used(const uint8_t *f, int k) { return rd32(f + (size_t)k * PT_SLOT + 36) != 0; }

static int pt_path(WCHAR *out)
{
    WCHAR saves[MAX_PATH];
    return saves_dir(saves) && join(out, saves, PT_FILE);
}

/* Reads a Paint Tool file (0 on failure, with a message in `err`). */
static uint8_t *pt_read(const WCHAR *path, WCHAR *err, size_t errn)
{
    uint8_t *buf;
    FILE *f;
    size_t got;
    if (_wfopen_s(&f, path, L"rb") || !f) {
        swprintf_s(err, errn, L"There is no Paint Tool save yet: open CREATE MODES > CREATE A SUPERSTAR > "
                              L"PAINT TOOL in the game once.");
        return NULL;
    }
    buf = (uint8_t *)malloc(PT_BYTES + 1);
    got = buf ? fread(buf, 1, PT_BYTES + 1, f) : 0;
    fclose(f);
    if (!buf || got != PT_BYTES || !pt_valid(buf)) {
        free(buf);
        swprintf_s(err, errn, L"%s is not a Paint Tool save the game accepts.", path);
        return NULL;
    }
    return buf;
}

/* Writes the file back (checksums fixed), through a temporary file. */
static int pt_write(const WCHAR *path, uint8_t *f)
{
    WCHAR tmp[MAX_PATH + 8];
    FILE *o;
    int ok;
    pt_fix_sums(f);
    swprintf_s(tmp, MAX_PATH + 8, L"%s.new", path);
    if (_wfopen_s(&o, tmp, L"wb") || !o)
        return 0;
    ok = fwrite(f, 1, PT_BYTES, o) == PT_BYTES;
    ok = fclose(o) == 0 && ok;
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

/* ── images (WIC) ── */

static IWICImagingFactory *wic(void)
{
    static IWICImagingFactory *factory;
    if (!factory)
        CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                         (void **)&factory);
    return factory;
}

/* Saves 256 x 256 BGRA pixels as a PNG. */
static int png_write(const WCHAR *path, const uint8_t *bgra)
{
    IWICImagingFactory *fac = wic();
    IWICStream *stream = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    int ok = 0;
    if (!fac || FAILED(fac->lpVtbl->CreateStream(fac, &stream))
            || FAILED(stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE))
            || FAILED(fac->lpVtbl->CreateEncoder(fac, &GUID_ContainerFormatPng, NULL, &enc))
            || FAILED(enc->lpVtbl->Initialize(enc, (IStream *)stream, WICBitmapEncoderNoCache))
            || FAILED(enc->lpVtbl->CreateNewFrame(enc, &frame, NULL))
            || FAILED(frame->lpVtbl->Initialize(frame, NULL))
            || FAILED(frame->lpVtbl->SetSize(frame, PT_W, PT_W))
            || FAILED(frame->lpVtbl->SetPixelFormat(frame, &fmt))
            || !IsEqualGUID(&fmt, &GUID_WICPixelFormat32bppBGRA)
            || FAILED(frame->lpVtbl->WritePixels(frame, PT_W, PT_W * 4, PT_W * PT_W * 4, (BYTE *)bgra))
            || FAILED(frame->lpVtbl->Commit(frame)) || FAILED(enc->lpVtbl->Commit(enc)))
        goto done;
    ok = 1;
done:
    if (frame) frame->lpVtbl->Release(frame);
    if (enc) enc->lpVtbl->Release(enc);
    if (stream) stream->lpVtbl->Release(stream);
    if (!ok)
        DeleteFileW(path);
    return ok;
}

/* Loads any image Windows can read, fitted into 256 x 256 (aspect kept,
 * centred, transparent around it), as BGRA. */
static int image_read(const WCHAR *path, uint8_t *bgra, WCHAR *err, size_t errn)
{
    IWICImagingFactory *fac = wic();
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICBitmapSource *src;
    UINT w = 0, h = 0, sw, sh, x0, y0, y;
    uint8_t *tmp = NULL;
    int ok = 0;
    if (!fac || FAILED(fac->lpVtbl->CreateDecoderFromFilename(fac, path, NULL, GENERIC_READ,
                                                               WICDecodeMetadataCacheOnDemand, &dec))
            || FAILED(dec->lpVtbl->GetFrame(dec, 0, &frame))
            || FAILED(fac->lpVtbl->CreateFormatConverter(fac, &conv))
            || FAILED(conv->lpVtbl->Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                               WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))
            || FAILED(conv->lpVtbl->GetSize(conv, &w, &h)) || !w || !h) {
        swprintf_s(err, errn, L"Could not read %s as an image.", path);
        goto done;
    }
    if (w >= h) { sw = PT_W; sh = (UINT)(((uint64_t)h * PT_W + w / 2) / w); }
    else        { sh = PT_W; sw = (UINT)(((uint64_t)w * PT_W + h / 2) / h); }
    if (!sw) sw = 1;
    if (!sh) sh = 1;
    src = (IWICBitmapSource *)conv;
    if (sw != w || sh != h) {
        if (FAILED(fac->lpVtbl->CreateBitmapScaler(fac, &scaler))
                || FAILED(scaler->lpVtbl->Initialize(scaler, src, sw, sh, WICBitmapInterpolationModeHighQualityCubic))) {
            swprintf_s(err, errn, L"Could not scale %s.", path);
            goto done;
        }
        src = (IWICBitmapSource *)scaler;
    }
    tmp = (uint8_t *)malloc((size_t)sw * sh * 4);
    if (!tmp || FAILED(src->lpVtbl->CopyPixels(src, NULL, sw * 4, sw * sh * 4, tmp))) {
        swprintf_s(err, errn, L"Could not read the pixels of %s.", path);
        goto done;
    }
    memset(bgra, 0, PT_W * PT_W * 4);
    x0 = (PT_W - sw) / 2;
    y0 = (PT_W - sh) / 2;
    for (y = 0; y < sh; y++)
        memcpy(bgra + ((size_t)(y0 + y) * PT_W + x0) * 4, tmp + (size_t)y * sw * 4, (size_t)sw * 4);
    ok = 1;
done:
    free(tmp);
    if (scaler) scaler->lpVtbl->Release(scaler);
    if (conv) conv->lpVtbl->Release(conv);
    if (frame) frame->lpVtbl->Release(frame);
    if (dec) dec->lpVtbl->Release(dec);
    return ok;
}

/* ── the slot's three copies of the logo ── */

/* 256-colour palette by median cut over the image's colours (A R G B). */
typedef struct { uint32_t argb, count; } PtColor;
typedef struct { int lo, hi; } PtBox;

static int pt_cmp_ch;
static int pt_cmp(const void *a, const void *b)
{
    uint32_t x = (((const PtColor *)a)->argb >> pt_cmp_ch) & 0xFF, y = (((const PtColor *)b)->argb >> pt_cmp_ch) & 0xFF;
    return (int)x - (int)y;
}
static int pt_cmp_argb(const void *a, const void *b)
{
    uint32_t x = ((const PtColor *)a)->argb, y = ((const PtColor *)b)->argb;
    return x < y ? -1 : x > y;
}

static void pt_make_tga(uint8_t *t, const uint32_t *argb)
{
    PtColor *c = (PtColor *)malloc(sizeof(PtColor) * PT_W * PT_W);
    PtBox box[256];
    uint32_t pal[256];
    int n = 0, nb = 1, i, j;
    if (!c)
        return;
    for (i = 0; i < PT_W * PT_W; i++) { c[i].argb = argb[i]; c[i].count = 1; }
    qsort(c, PT_W * PT_W, sizeof *c, pt_cmp_argb);
    for (i = 0; i < PT_W * PT_W; i++) {                 /* unique colours with counts */
        if (n && c[n - 1].argb == c[i].argb) c[n - 1].count++;
        else c[n++] = c[i];
    }
    box[0].lo = 0; box[0].hi = n;
    while (nb < 256) {                                   /* split the widest box */
        int best = -1, bestw = 0, bestch = 0;
        for (i = 0; i < nb; i++) {
            if (box[i].hi - box[i].lo < 2) continue;
            for (j = 0; j < 32; j += 8) {
                int lo = 255, hi = 0, k;
                for (k = box[i].lo; k < box[i].hi; k++) {
                    int v = (c[k].argb >> j) & 0xFF;
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
                if (hi - lo > bestw) { bestw = hi - lo; best = i; bestch = j; }
            }
        }
        if (best < 0) break;
        pt_cmp_ch = bestch;
        qsort(c + box[best].lo, box[best].hi - box[best].lo, sizeof *c, pt_cmp);
        {   /* at the median by pixel count */
            uint64_t total = 0, acc = 0;
            int k, mid = -1;
            for (k = box[best].lo; k < box[best].hi; k++) total += c[k].count;
            for (k = box[best].lo; k < box[best].hi - 1; k++) {
                acc += c[k].count;
                if (acc * 2 >= total) { mid = k + 1; break; }
            }
            if (mid < 0) mid = box[best].hi - 1;
            box[nb].lo = mid; box[nb].hi = box[best].hi; box[best].hi = mid; nb++;
        }
    }
    for (i = 0; i < 256; i++) pal[i] = 0;
    for (i = 0; i < nb; i++) {                           /* each box's average */
        uint64_t s[4] = { 0, 0, 0, 0 }, w = 0;
        int k;
        for (k = box[i].lo; k < box[i].hi; k++) {
            for (j = 0; j < 4; j++) s[j] += (uint64_t)((c[k].argb >> (8 * j)) & 0xFF) * c[k].count;
            w += c[k].count;
        }
        if (w) pal[i] = (uint32_t)((s[3] / w) << 24 | (s[2] / w) << 16 | (s[1] / w) << 8 | (s[0] / w));
    }
    free(c);
    memcpy(t, k_pt_tga_header, 20);
    for (i = 0; i < 256; i++) wr32(t + 20 + 4 * i, pal[i]);
    for (i = 0; i < PT_W * PT_W; i++) {                  /* nearest palette entry */
        uint32_t p = argb[i];
        int best = 0;
        uint32_t bd = 0xFFFFFFFF;
        for (j = 0; j < nb; j++) {
            int da = (int)(p >> 24) - (int)(pal[j] >> 24), dr = (int)((p >> 16) & 0xFF) - (int)((pal[j] >> 16) & 0xFF),
                dg = (int)((p >> 8) & 0xFF) - (int)((pal[j] >> 8) & 0xFF), db = (int)(p & 0xFF) - (int)(pal[j] & 0xFF);
            uint32_t dd = (uint32_t)(da * da + dr * dr + dg * dg + db * db);
            if (dd < bd) { bd = dd; best = j; if (!dd) break; }
        }
        t[0x414 + i] = (uint8_t)best;
    }
}

static uint16_t to565(int r, int g, int b)
{
    return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}
static void from565(uint16_t c, int *r, int *g, int *b)
{
    *r = ((c >> 11) & 31) * 255 / 31; *g = ((c >> 5) & 63) * 255 / 63; *b = (c & 31) * 255 / 31;
}

/* DXT5, block by block (little-endian, as a PC DDS). */
static void pt_make_dds(uint8_t *d, const uint32_t *argb)
{
    int bx, by, i;
    uint8_t *o = d + 128;
    memcpy(d, k_pt_dds_header, 128);
    for (by = 0; by < PT_W / 4; by++)
        for (bx = 0; bx < PT_W / 4; bx++, o += 16) {
            uint32_t px[16];
            int amin = 255, amax = 0, mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 };
            uint8_t apal[8];
            uint64_t abits = 0;
            uint32_t cbits = 0;
            uint16_t c0, c1;
            int cr[4], cg[4], cb[4];
            for (i = 0; i < 16; i++) {
                uint32_t p = argb[(by * 4 + i / 4) * PT_W + bx * 4 + i % 4];
                int a = p >> 24, ch[3] = { (int)(p >> 16) & 0xFF, (int)(p >> 8) & 0xFF, (int)p & 0xFF }, k;
                px[i] = p;
                if (a < amin) amin = a;
                if (a > amax) amax = a;
                for (k = 0; k < 3; k++) { if (ch[k] < mn[k]) mn[k] = ch[k]; if (ch[k] > mx[k]) mx[k] = ch[k]; }
            }
            /* alpha: 8 steps between max and min */
            apal[0] = (uint8_t)amax; apal[1] = (uint8_t)amin;
            for (i = 1; i < 7; i++) apal[i + 1] = (uint8_t)(((7 - i) * amax + i * amin) / 7);
            for (i = 0; i < 16; i++) {
                int a = px[i] >> 24, best = 0, bd = 1 << 30, k;
                for (k = 0; k < 8; k++) { int dd = abs(a - apal[k]); if (dd < bd) { bd = dd; best = k; } }
                abits |= (uint64_t)best << (3 * i);
            }
            o[0] = (uint8_t)amax; o[1] = (uint8_t)amin;
            for (i = 0; i < 6; i++) o[2 + i] = (uint8_t)(abits >> (8 * i));
            /* colour: the block's two most different pixels (the bounding
               box's corners can be colours the block doesn't have), four-colour mode */
            {
                int best = -1, pa = 0, pb = 0, a2, b2;
                for (a2 = 0; a2 < 16; a2++)
                    for (b2 = a2 + 1; b2 < 16; b2++) {
                        int dr = (int)((px[a2] >> 16) & 0xFF) - (int)((px[b2] >> 16) & 0xFF),
                            dg = (int)((px[a2] >> 8) & 0xFF) - (int)((px[b2] >> 8) & 0xFF),
                            db = (int)(px[a2] & 0xFF) - (int)(px[b2] & 0xFF), dd = dr * dr + dg * dg + db * db;
                        if (dd > best) { best = dd; pa = a2; pb = b2; }
                    }
                c0 = to565((px[pa] >> 16) & 0xFF, (px[pa] >> 8) & 0xFF, px[pa] & 0xFF);
                c1 = to565((px[pb] >> 16) & 0xFF, (px[pb] >> 8) & 0xFF, px[pb] & 0xFF);
                (void)mn; (void)mx;
            }
            if (c0 < c1) { uint16_t t = c0; c0 = c1; c1 = t; }
            if (c0 == c1) { cbits = 0; }
            else {
                from565(c0, &cr[0], &cg[0], &cb[0]);
                from565(c1, &cr[1], &cg[1], &cb[1]);
                cr[2] = (2 * cr[0] + cr[1]) / 3; cg[2] = (2 * cg[0] + cg[1]) / 3; cb[2] = (2 * cb[0] + cb[1]) / 3;
                cr[3] = (cr[0] + 2 * cr[1]) / 3; cg[3] = (cg[0] + 2 * cg[1]) / 3; cb[3] = (cb[0] + 2 * cb[1]) / 3;
                for (i = 0; i < 16; i++) {
                    int r = (px[i] >> 16) & 0xFF, g = (px[i] >> 8) & 0xFF, b = px[i] & 0xFF, best = 0, bd = 1 << 30, k;
                    for (k = 0; k < 4; k++) {
                        int dd = (r - cr[k]) * (r - cr[k]) + (g - cg[k]) * (g - cg[k]) + (b - cb[k]) * (b - cb[k]);
                        if (dd < bd) { bd = dd; best = k; }
                    }
                    cbits |= (uint32_t)best << (2 * i);
                }
            }
            o[8] = (uint8_t)c0; o[9] = (uint8_t)(c0 >> 8); o[10] = (uint8_t)c1; o[11] = (uint8_t)(c1 >> 8);
            for (i = 0; i < 4; i++) o[12 + i] = (uint8_t)(cbits >> (8 * i));
        }
}

/* Puts a logo (BGRA, 256 x 256) in slot k, as the game would have saved it. */
static void pt_put(uint8_t *f, int k, const uint8_t *bgra)
{
    uint8_t *b = pt_slot(f, k);
    uint32_t *argb = (uint32_t *)malloc(PT_W * PT_W * 4);
    SYSTEMTIME st;
    int i;
    if (!argb)
        return;
    memcpy(b + 8, k_pt_used_header, 44);
    for (i = 0; i < PT_W * PT_W; i++) {
        const uint8_t *p = bgra + 4 * i;
        argb[i] = (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0];
        wr32(b + PT_CANVAS + 4 * i, argb[i]);
    }
    pt_make_tga(b + PT_TGA, argb);
    pt_make_dds(b + PT_DDS, argb);
    GetSystemTime(&st);
    b[PT_STAMP] = (uint8_t)(st.wYear >> 8); b[PT_STAMP + 1] = (uint8_t)st.wYear;
    b[PT_STAMP + 2] = (uint8_t)st.wMonth; b[PT_STAMP + 3] = (uint8_t)st.wDay;
    b[PT_STAMP + 4] = (uint8_t)st.wHour; b[PT_STAMP + 5] = (uint8_t)st.wMinute;
    b[PT_STAMP + 6] = (uint8_t)st.wSecond; b[PT_STAMP + 7] = (uint8_t)(st.wDayOfWeek + 1);
    free(argb);
}

static void pt_clear(uint8_t *f, int k)
{
    uint8_t *b = pt_slot(f, k);
    memset(b + 8, 0, PT_STAMP - 8);
    memcpy(b + 8, k_pt_empty_header, 44);
}

/* Slot k's logo as BGRA. */
static void pt_get(const uint8_t *f, int k, uint8_t *bgra)
{
    const uint8_t *c = f + (size_t)k * PT_SLOT + PT_CANVAS;
    int i;
    for (i = 0; i < PT_W * PT_W; i++, c += 4) {
        bgra[4 * i] = c[3]; bgra[4 * i + 1] = c[2]; bgra[4 * i + 2] = c[1]; bgra[4 * i + 3] = c[0];
    }
}

/* ── the Paint Tool tab ── */

static void pt_status(const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 1024, fmt, ap);
    va_end(ap);
    set_text(ID_PT_STATUS, buf);
}

static void pt_refresh(void)
{
    WCHAR path[MAX_PATH], err[512];
    int k, n = 0;
    free(s_pt);
    s_pt = NULL;
    if (!s_game_dir[0]) {
        pt_status(L"Install the game first (Install tab).");
    } else {
        if (!any_game_running())
            saves_migrate();
        if (pt_path(path) && (s_pt = pt_read(path, err, 512)) != NULL) {
            for (k = 0; k < PT_SLOTS; k++)
                n += pt_used(s_pt, k);
            pt_status(L"%d of %d logos used. Choose a logo, then Export as PNG or Import an image into it.", n, PT_SLOTS);
        } else
            pt_status(L"%s", err);
    }
    if (s_pt_sel >= PT_SLOTS) s_pt_sel = -1;
    InvalidateRect(s_pt_grid, NULL, TRUE);
}

#define PT_CELL 72
#define PT_GAP  10

static LRESULT CALLBACK pt_grid_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), cx = x / S(PT_CELL + PT_GAP), cy = y / S(PT_CELL + PT_GAP);
        if (cx < 5 && cy < 4 && x % S(PT_CELL + PT_GAP) < S(PT_CELL) && y % S(PT_CELL + PT_GAP) < S(PT_CELL)) {
            s_pt_sel = cy * 5 + cx;
            InvalidateRect(w, NULL, FALSE);
        }
        SetFocus(w);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        SendMessageW(s_wnd, WM_COMMAND, ID_PT_EXPORT, 0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        BITMAPINFO bi;
        uint8_t *px = (uint8_t *)malloc(PT_W * PT_W * 4);
        HBRUSH bg = GetSysColorBrush(COLOR_WINDOW), cell = CreateSolidBrush(RGB(40, 40, 46)),
               sel = CreateSolidBrush(RGB(200, 16, 32));
        int k;
        GetClientRect(w, &rc);
        FillRect(dc, &rc, bg);
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = PT_W;
        bi.bmiHeader.biHeight = -PT_W;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, s_font);
        SetStretchBltMode(dc, HALFTONE);
        for (k = 0; k < PT_SLOTS; k++) {
            RECT r;
            WCHAR label[16];
            r.left = (k % 5) * S(PT_CELL + PT_GAP); r.top = (k / 5) * S(PT_CELL + PT_GAP);
            r.right = r.left + S(PT_CELL); r.bottom = r.top + S(PT_CELL);
            if (k == s_pt_sel) {
                RECT o = { r.left - S(4), r.top - S(4), r.right + S(4), r.bottom + S(4) };
                FillRect(dc, &o, sel);
            }
            FillRect(dc, &r, cell);
            if (s_pt && px && pt_used(s_pt, k)) {
                int i;
                pt_get(s_pt, k, px);
                for (i = 0; i < PT_W * PT_W; i++) {      /* over white, as the game shows it */
                    int a = px[4 * i + 3], j;
                    for (j = 0; j < 3; j++) px[4 * i + j] = (uint8_t)((px[4 * i + j] * a + 255 * (255 - a)) / 255);
                }
                StretchDIBits(dc, r.left, r.top, S(PT_CELL), S(PT_CELL), 0, 0, PT_W, PT_W, px, &bi, DIB_RGB_COLORS,
                              SRCCOPY);
                SetTextColor(dc, RGB(90, 90, 90));
            } else
                SetTextColor(dc, RGB(200, 200, 205));
            swprintf_s(label, 16, s_pt && pt_used(s_pt, k) ? L"%d" : L"%d  empty", k + 1);
            {
                RECT t = { r.left + S(4), r.top + S(2), r.right, r.top + S(20) };
                DrawTextW(dc, label, -1, &t, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        free(px);
        DeleteObject(cell);
        DeleteObject(sel);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(w, m, wp, lp);
}

static HWND pt_setup_grid(void)
{
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = pt_grid_proc;
    wc.hInstance = s_inst;
    wc.hCursor = LoadCursor(NULL, IDC_HAND);
    wc.lpszClassName = L"SvR2011PaintGrid";
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    return s_pt_grid = add(TAB_PAINT, L"SvR2011PaintGrid", L"", WS_TABSTOP, X0, 86,
                           5 * (PT_CELL + PT_GAP), 4 * (PT_CELL + PT_GAP), ID_PT_GRID);
}

static int pick_image_file(WCHAR *out, int save, const WCHAR *name)
{
    IFileDialog *d;
    IShellItem *it;
    PWSTR p = NULL;
    int ok = 0;
    static const COMDLG_FILTERSPEC png[] = { { L"PNG image", L"*.png" } };
    static const COMDLG_FILTERSPEC img[] = { { L"Images", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.tif;*.tiff" },
                                             { L"All files", L"*.*" } };
    if (FAILED(CoCreateInstance(save ? &CLSID_FileSaveDialog : &CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                                save ? &IID_IFileSaveDialog : &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    if (save) {
        d->lpVtbl->SetFileTypes(d, 1, png);
        d->lpVtbl->SetDefaultExtension(d, L"png");
        d->lpVtbl->SetFileName(d, name);
        d->lpVtbl->SetTitle(d, L"Export the logo as a PNG");
    } else {
        d->lpVtbl->SetFileTypes(d, 2, img);
        d->lpVtbl->SetTitle(d, L"Choose an image for the logo");
    }
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            wcscpy_s(out, MAX_PATH, p);
            CoTaskMemFree(p);
            ok = 1;
        }
        it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
    return ok;
}

static int pt_need_slot(int used)
{
    if (!s_pt) {
        pt_refresh();
        if (!s_pt)
            return 0;
    }
    if (s_pt_sel < 0) {
        pt_status(L"Choose a logo in the grid first.");
        return 0;
    }
    if (used && !pt_used(s_pt, s_pt_sel)) {
        pt_status(L"Logo %d is empty.", s_pt_sel + 1);
        return 0;
    }
    return 1;
}

static void pt_export(void)
{
    WCHAR out[MAX_PATH], name[64];
    uint8_t *px;
    if (!pt_need_slot(1))
        return;
    swprintf_s(name, 64, L"Logo %d.png", s_pt_sel + 1);
    if (!pick_image_file(out, 1, name))
        return;
    px = (uint8_t *)malloc(PT_W * PT_W * 4);
    if (!px)
        return;
    pt_get(s_pt, s_pt_sel, px);
    if (png_write(out, px))
        pt_status(L"Logo %d exported to %s.", s_pt_sel + 1, out);
    else
        pt_status(L"Could not write %s.", out);
    free(px);
}

static void pt_export_all(void)
{
    WCHAR dir[MAX_PATH], out[MAX_PATH], name[64];
    uint8_t *px;
    int k, n = 0;
    if (!s_pt) {
        pt_refresh();
        if (!s_pt)
            return;
    }
    if (!pick_folder(L"Choose where to export the logos to", dir))
        return;
    px = (uint8_t *)malloc(PT_W * PT_W * 4);
    if (!px)
        return;
    for (k = 0; k < PT_SLOTS; k++) {
        if (!pt_used(s_pt, k))
            continue;
        swprintf_s(name, 64, L"Logo %d.png", k + 1);
        pt_get(s_pt, k, px);
        if (join(out, dir, name) && png_write(out, px))
            n++;
    }
    free(px);
    pt_status(L"Exported %d logo%s to %s.", n, n == 1 ? L"" : L"s", dir);
}

/* Changes slot s_pt_sel (after a backup of the whole Saves folder). */
static void pt_change(int import)
{
    WCHAR img[MAX_PATH], path[MAX_PATH], before[MAX_PATH], err[512], t[512];
    uint8_t *px = NULL;
    if (saves_locked() || !pt_need_slot(!import) || !pt_path(path))
        return;
    if (import) {
        if (!pick_image_file(img, 0, NULL))
            return;
        px = (uint8_t *)malloc(PT_W * PT_W * 4);
        if (!px || !image_read(img, px, err, 512)) {
            pt_status(L"%s", px ? err : L"Out of memory.");
            free(px);
            return;
        }
        if (pt_used(s_pt, s_pt_sel)) {
            swprintf_s(t, 512, L"Replace logo %d with %s?", s_pt_sel + 1, img);
            if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES) {
                free(px);
                return;
            }
        }
    } else {
        swprintf_s(t, 512, L"Delete logo %d?", s_pt_sel + 1);
        if (MessageBoxW(s_wnd, t, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    if (!saves_backup_to(L" (before Paint Tool change)", before)) {
        pt_status(L"Could not back up the saves first; nothing was changed.");
        free(px);
        return;
    }
    if (import)
        pt_put(s_pt, s_pt_sel, px);
    else
        pt_clear(s_pt, s_pt_sel);
    free(px);
    if (!pt_write(path, s_pt)) {
        pt_status(L"Could not write %s; nothing was changed.", path);
        pt_refresh();
        return;
    }
    {
        int keep = s_pt_sel;
        pt_refresh();
        s_pt_sel = keep;
    }
    InvalidateRect(s_pt_grid, NULL, FALSE);
    if (import)
        pt_status(L"Logo %d is now %s. (Your saves were backed up to %s.)", s_pt_sel + 1, img, before);
    else
        pt_status(L"Logo %d deleted. (Your saves were backed up to %s.)", s_pt_sel + 1, before);
}

/* --paint-export <file.pt> <slot 1-20> <out.png> / --paint-import <file.pt> <slot> <image> (tests). */
static int pt_console(int import, const WCHAR *file, int slot, const WCHAR *img)
{
    WCHAR err[512];
    uint8_t *f = pt_read(file, err, 512), *px = (uint8_t *)malloc(PT_W * PT_W * 4);
    int ok = 0;
    if (!f || !px || slot < 1 || slot > PT_SLOTS) {
        con_print(f ? L"bad slot\n" : err);
        goto done;
    }
    if (import) {
        if (!image_read(img, px, err, 512)) { con_print(err); goto done; }
        pt_put(f, slot - 1, px);
        ok = pt_write(file, f);
    } else {
        pt_get(f, slot - 1, px);
        ok = pt_used(f, slot - 1) && png_write(img, px);
    }
    con_print(ok ? L"ok\n" : L"failed\n");
done:
    free(f);
    free(px);
    return ok ? 0 : 1;
}

static void set_busy(int busy)
{
    int ids[] = { ID_IMAGE, ID_IMAGE_BROWSE, ID_TARGET, ID_TARGET_BROWSE, ID_INSTALL, ID_GAMEDIR_CHANGE };
    int i;
    s_busy = busy;
    for (i = 0; i < (int)(sizeof ids / sizeof ids[0]); i++)
        EnableWindow(ctl(ids[i]), !busy);
    EnableWindow(ctl(ID_CANCEL), busy);
    if (busy)
        EnableWindow(ctl(ID_PLAY), FALSE);
    else
        refresh_play();
}

static void start_install(void)
{
    WCHAR t[MAX_PATH + 200];
    if (s_busy)
        return;
    GetWindowTextW(ctl(ID_IMAGE), s_job.image, MAX_PATH);
    GetWindowTextW(ctl(ID_TARGET), s_job.target, MAX_PATH);
    if (!s_job.image[0] || !file_exists(s_job.image)) {
        set_text(ID_INSTALL_STATUS, L"Choose your " GAME_TITLE L" disc image (ISO or XISO) first.");
        return;
    }
    if (!s_job.target[0]) {
        set_text(ID_INSTALL_STATUS, L"Choose the folder to install the game to.");
        return;
    }
    if (game_running()) {
        set_text(ID_INSTALL_STATUS, L"Close the game before installing it again.");
        return;
    }
    WritePrivateProfileStringW(L"Launcher", L"LastImage", s_job.image, s_launcher_ini);
    s_cancel = 0;
    set_busy(1);
    SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 0, 0);
    swprintf_s(t, MAX_PATH + 200, L"Installing to %s\x2026", s_job.target);
    set_text(ID_INSTALL_STATUS, t);
    if (s_worker)
        CloseHandle(s_worker);
    s_worker = CreateThread(NULL, 0, install_thread, NULL, 0, NULL);
    if (!s_worker) {
        set_busy(0);
        set_text(ID_INSTALL_STATUS, L"The installation could not be started.");
    }
}

static void check_image(const WCHAR *p)
{
    Disc d;
    WCHAR err[512];
    s_image_bytes = 0;
    if (!disc_check(&d, p, err, 512)) {
        set_text(ID_INSTALL_STATUS, err);
    } else {
        WCHAR t[160], a[32];
        s_image_bytes = d.total;
        fmt_size(a, 32, d.total);
        swprintf_s(t, 160, GAME_TITLE L" disc image found: %d files, %s.", d.nfiles, a);
        set_text(ID_INSTALL_STATUS, t);
    }
    disc_close(&d);
    update_free_space();
}

/* ── Movies tab: USER MOVIES (movie_maker.c) ── */

static MovieJob s_mv_job;
static volatile LONG s_mv_busy;
static uint8_t *s_mv_preview;      /* 320 x 320 BGRA, or NULL */
static HWND s_mv_view;
static const int s_mv_lengths[] = { 180, 30, 60, 90 };

static int mv_folder(WCHAR *out)
{
    if (!s_game_dir[0] || !dir_exists(s_game_dir) || !join(out, s_game_dir, L"Custom Movies"))
        return 0;
    CreateDirectoryW(out, NULL);
    return 1;
}

static void mv_status(const WCHAR *fmt, ...)
{
    WCHAR buf[600];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 600, fmt, ap);
    va_end(ap);
    set_text(ID_MV_STATUS, buf);
}

static void mv_refresh(void)
{
    WCHAR dir[MAX_PATH], pat[MAX_PATH], line[MAX_PATH + 32];
    HWND list = ctl(ID_MV_LIST);
    WIN32_FIND_DATAW fd;
    HANDLE h;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    if (!mv_folder(dir) || !join(pat, dir, L"*.bik"))
        return;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        WCHAR size[32], *dot;
        fmt_size(size, 32, ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        wcscpy_s(line, MAX_PATH + 32, fd.cFileName);
        dot = wcsrchr(line, L'.');
        if (dot) *dot = 0;
        wcscat_s(line, MAX_PATH + 32, L"   (");
        wcscat_s(line, MAX_PATH + 32, size);
        wcscat_s(line, MAX_PATH + 32, L")");
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)line);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int mv_fit(void)
{
    if (IsDlgButtonChecked(s_wnd, ID_MV_FILL) == BST_CHECKED) return MOVIE_FILL;
    if (IsDlgButtonChecked(s_wnd, ID_MV_STRETCH) == BST_CHECKED) return MOVIE_STRETCH;
    return MOVIE_FIT;
}

static int mv_pick(WCHAR *out, int bottom)
{
    IFileDialog *d;
    IShellItem *it;
    PWSTR p = NULL;
    int ok = 0;
    static const COMDLG_FILTERSPEC video[] = {
        { L"Videos and pictures", L"*.mp4;*.m4v;*.mov;*.wmv;*.avi;*.mkv;*.webm;*.mpg;*.mpeg;*.ts;*.bik;*.png;*.jpg;*.jpeg;*.bmp;*.gif" },
        { L"All files", L"*.*" } };
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&d)))
        return 0;
    d->lpVtbl->SetFileTypes(d, 2, video);
    d->lpVtbl->SetTitle(d, bottom ? L"Choose a picture or video for the bottom strip" : L"Choose the video");
    if (SUCCEEDED(d->lpVtbl->Show(d, s_wnd)) && SUCCEEDED(d->lpVtbl->GetResult(d, &it))) {
        if (SUCCEEDED(it->lpVtbl->GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
            wcscpy_s(out, MAX_PATH, p);
            CoTaskMemFree(p);
            ok = 1;
        }
        it->lpVtbl->Release(it);
    }
    d->lpVtbl->Release(d);
    return ok;
}

/* The game's titantron movies (movies\titantron\<id>.bik) and whose they
 * are, as CREATE AN ENTRANCE lists them; the DLC superstars' movies are in
 * the installed DLC. Their bottom strips can be reused (Superstar...). */
static const struct { int id; const WCHAR *name; } k_star_movies[] = {
    { 297, L"ALICIA FOX" },
    { 160, L"BATISTA" },
    { 224, L"BETH PHOENIX" },
    { 125, L"BIG SHOW" },
    { 294, L"BRIE BELLA" },
    { 137, L"CHAVO GUERRERO" },
    { 104, L"CHRIS JERICHO" },
    { 276, L"CHRISTIAN" },
    { 902, L"CHRISTIAN OLD" },
    { 131, L"CM PUNK" },
    { 205, L"CODY RHODES" },
    { 424, L"CRYME TYME" },
    { 281, L"DAVID HART SMITH" },
    { 407, L"D-GENERATION X" },
    { 275, L"DOLPH ZIGGLER" },
    { 278, L"DREW McINTYRE" },
    { 110, L"EDGE" },
    { 423, L"EDGE & CHRISTIAN" },
    { 901, L"EDGE OLD" },
    { 265, L"EVAN BOURNE" },
    { 292, L"EVE" },
    { 268, L"EZEKIEL JACKSON" },
    { 158, L"FINLAY" },
    { 293, L"GAIL KIM" },
    { 277, L"GOLDUST" },
    { 401, L"THE HART DYNASTY" },
    { 267, L"JACK SWAGGER" },
    { 132, L"JAKE ROBERTS" },
    { 122, L"JIMMY SNUKA" },
    { 139, L"JOHN CENA" },
    { 175, L"JOHN MORRISON" },
    { 165, L"JTG" },
    { 107, L"KANE" },
    { 903, L"MASKED KANE" },
    { 164, L"KELLY KELLY" },
    { 176, L"KOFI KINGSTON" },
    { 321, L"LUKE GALLOWS" },
    { 179, L"MARK HENRY" },
    { 291, L"MARYSE" },
    { 112, L"MATT HARDY" },
    { 174, L"MELINA" },
    { 182, L"MICHELLE McCOOL" },
    { 143, L"MICKIE JAMES" },
    { 263, L"MIKE KNOX" },
    { 218, L"THE MIZ" },
    { 115, L"MR. McMAHON" },
    { 135, L"MVP" },
    { 295, L"NIKKI BELLA" },
    { 271, L"PRIMO" },
    { 161, L"RANDY ORTON" },
    { 123, L"REY MYSTERIO" },
    { 305, L"RICKY STEAMBOAT" },
    { 306, L"ROB VAN DAM" },
    { 100, L"THE ROCK" },
    { 272, L"R-TRUTH" },
    { 201, L"SANTINO MARELLA" },
    { 178, L"SHAD GASPARD" },
    { 145, L"SHAWN MICHAELS" },
    { 279, L"SHEAMUS" },
    { 208, L"SHELTON BENJAMIN" },
    { 422, L"SHOWMIZ" },
    { 101, L"STONE COLD" },
    { 131, L"THE STRAIGHT EDGE SOCIETY" },
    { 261, L"TED DIBIASE" },
    { 169, L"TERRY FUNK" },
    { 186, L"THEODORE LONG" },
    { 102, L"TRIPLE H" },
    { 103, L"UNDERTAKER" },
    { 284, L"VANCE ARCHER" },
    { 262, L"VLADIMIR KOZLOV" },
    { 117, L"WILLIAM REGAL" },
    { 283, L"YOSHI TATSU" },
    { 219, L"ZACK RYDER" },
    { 50, L"CHRIS MASTERS" },
    { 51, L"LEX LUGER" },
    { 52, L"BRITISH BULLDOG" },
    { 53, L"JUSTIN GABRIEL" },
    { 54, L"DAVID OTUNGA" },
    { 55, L"WADE BARRETT" },
    { 56, L"LAYLA" },
    { 255, L"WWE LOGO" },
    { 256, L"LEGENDS LOGO" }
};

/* The movie of k_star_movies[i]: in the game folder or in the installed DLC. */
static int mv_star_path(int i, WCHAR *out)
{
    WCHAR file[32], p[MAX_PATH], pat[MAX_PATH], dlc[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int found = 0;
    swprintf_s(file, 32, L"%03d.bik", k_star_movies[i].id);
    swprintf_s(p, MAX_PATH, L"%s\\movies\\titantron\\%s", s_game_dir, file);
    if (file_exists(p)) { wcscpy_s(out, MAX_PATH, p); return 1; }
    swprintf_s(dlc, MAX_PATH, L"%s\\UserData\\0000000000000000\\5451085D\\00000002", s_game_dir);
    swprintf_s(pat, MAX_PATH, L"%s\\*", dlc);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        swprintf_s(p, MAX_PATH, L"%s\\%s\\movies\\titantron\\%s", dlc, fd.cFileName, file);
        if (file_exists(p)) { wcscpy_s(out, MAX_PATH, p); found = 1; }
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static void mv_preview(void);

/* A superstar's strip picked with Superstar...: the box shows its name. */
static WCHAR s_mv_star_label[96], s_mv_star_path[MAX_PATH];

/* The bottom strip's file: what the box says, or the superstar's movie. */
static void mv_bottom_path(WCHAR *out)
{
    GetWindowTextW(ctl(ID_MV_BOTTOM), out, MAX_PATH);
    if (s_mv_star_label[0] && !wcscmp(out, s_mv_star_label)) wcscpy_s(out, MAX_PATH, s_mv_star_path);
}

/* Uses k_star_movies[i]'s bottom strip. */
static void mv_star_pick(int i)
{
    WCHAR p[MAX_PATH];
    int found = 0, k;
    if (!mv_star_path(i, s_mv_star_path)) return;
    for (k = 0; k < (int)(sizeof k_star_movies / sizeof *k_star_movies); k++) found += mv_star_path(k, p);
    swprintf_s(s_mv_star_label, 96, L"%s (superstar's strip)", k_star_movies[i].name);
    set_text(ID_MV_BOTTOM, s_mv_star_label);
    mv_preview();
    if (s_mv_preview)   /* (else the preview's error stays) */
        mv_status(L"Bottom strip: %s's, from the game's movie (%d superstar movies found). It loops if your video "
                  L"is longer.", k_star_movies[i].name, found);
}

/* Superstar...: a menu of the superstars whose movies are installed; the
 * pick's bottom strip becomes this movie's strip. */
static void mv_star_menu(void)
{
    enum { FIRST = 1, PER_COLUMN = 24 };
    HMENU m = CreatePopupMenu();
    RECT rc;
    WCHAR p[MAX_PATH];
    int i, n = 0, cmd;
    if (!m) return;
    for (i = 0; i < (int)(sizeof k_star_movies / sizeof *k_star_movies); i++) {
        if (!mv_star_path(i, p)) continue;
        AppendMenuW(m, MF_STRING | (n && n % PER_COLUMN == 0 ? MF_MENUBARBREAK : 0), (UINT_PTR)(FIRST + i),
                    k_star_movies[i].name);
        n++;
    }
    if (!n) {
        DestroyMenu(m);
        mv_status(L"No superstar movies found - install the game on the Install tab first.");
        return;
    }
    GetWindowRect(ctl(ID_MV_BOTTOM_STAR), &rc);
    cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, rc.left, rc.bottom, 0,
                              s_wnd, NULL);
    DestroyMenu(m);
    if (cmd >= FIRST) mv_star_pick(cmd - FIRST);
}

/* The movie's name as the game will list it: letters, digits, spaces, - and _. */
static int mv_name(WCHAR *name, size_t n)
{
    WCHAR raw[64], *s;
    size_t k = 0;
    GetWindowTextW(ctl(ID_MV_NAME), raw, 64);
    for (s = raw; *s && k + 1 < n && k < 32; s++) {
        WCHAR c = *s;
        if ((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9') || c == L' ' || c == L'-' || c == L'_')
            name[k++] = c;
    }
    while (k && name[k - 1] == L' ') k--;
    name[k] = 0;
    s = name;
    while (*s == L' ') s++;
    if (s != name) memmove(name, s, (wcslen(s) + 1) * sizeof(WCHAR));
    return name[0] != 0;
}

static void mv_preview(void)
{
    WCHAR video[MAX_PATH], bottom[MAX_PATH], err[512];
    HCURSOR old;
    GetWindowTextW(ctl(ID_MV_VIDEO), video, MAX_PATH);
    mv_bottom_path(bottom);
    if (!video[0]) { mv_status(L"Choose a video first."); return; }
    if (!s_mv_preview) s_mv_preview = (uint8_t *)malloc(MOVIE_W * MOVIE_H * 4);
    if (!s_mv_preview) return;
    old = SetCursor(LoadCursor(NULL, IDC_WAIT));
    if (movie_preview(video, bottom, mv_fit(), 2.0, s_mv_preview, err, 512))
        mv_status(L"The movie as the game stores it: the big screen on top (the game shows it wider, at 16:9), "
                  L"the stage and ramp strip below.");
    else {
        free(s_mv_preview); s_mv_preview = NULL;
        mv_status(L"%s", err);
    }
    SetCursor(old);
    InvalidateRect(s_mv_view, NULL, FALSE);
}

static DWORD WINAPI mv_thread(LPVOID arg)
{
    int ok;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ok = movie_make(&s_mv_job);
    CoUninitialize();
    PostMessageW(s_wnd, WM_APP_MOVIE_DONE, (WPARAM)ok, 0);
    return 0;
}

static void mv_create(void)
{
    WCHAR dir[MAX_PATH], name[40], file[64], video[MAX_PATH];
    int len;
    if (s_mv_busy) return;
    GetWindowTextW(ctl(ID_MV_VIDEO), video, MAX_PATH);
    if (!video[0] || !file_exists(video)) { mv_status(L"Choose the video first."); return; }
    if (!mv_name(name, 40)) { mv_status(L"Give the movie a name (letters and digits); the game lists it by that name."); return; }
    if (!mv_folder(dir)) { mv_status(L"The game folder does not exist; choose it on the Play tab first."); return; }
    swprintf_s(file, 64, L"%s.bik", name);
    memset(&s_mv_job, 0, sizeof s_mv_job);
    if (!join(s_mv_job.out, dir, file)) return;
    if (file_exists(s_mv_job.out)) {
        WCHAR q[200];
        swprintf_s(q, 200, L"A movie named %s already exists. Replace it?", name);
        if (MessageBoxW(s_wnd, q, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
            return;
    }
    wcscpy_s(s_mv_job.video, MAX_PATH, video);
    mv_bottom_path(s_mv_job.bottom);
    s_mv_job.fit = mv_fit();
    len = (int)SendMessageW(ctl(ID_MV_LENGTH), CB_GETCURSEL, 0, 0);
    s_mv_job.max_seconds = len >= 0 && len < 4 ? s_mv_lengths[len] : 180;
    s_mv_job.notify = s_wnd;
    s_mv_job.msg = WM_APP_MOVIE;
    InterlockedExchange(&s_mv_busy, 1);
    EnableWindow(ctl(ID_MV_CREATE), FALSE);
    EnableWindow(ctl(ID_MV_STOP), TRUE);
    SendMessageW(ctl(ID_MV_PROGRESS), PBM_SETPOS, 0, 0);
    mv_status(L"Making %s\x2026", name);
    CloseHandle(CreateThread(NULL, 0, mv_thread, NULL, 0, NULL));
}

static void mv_done(int ok)
{
    InterlockedExchange(&s_mv_busy, 0);
    EnableWindow(ctl(ID_MV_CREATE), TRUE);
    EnableWindow(ctl(ID_MV_STOP), FALSE);
    if (ok) {
        const WCHAR *slash = wcsrchr(s_mv_job.out, L'\\');
        SendMessageW(ctl(ID_MV_PROGRESS), PBM_SETPOS, 100, 0);
        mv_status(L"Made %s (%d s). In the game: CREATE AN ENTRANCE \x2192 FINALIZE \x2192 MOVIE, after NONE.",
                  slash ? slash + 1 : s_mv_job.out, (s_mv_job.frames + 29) / 30);
    } else {
        SendMessageW(ctl(ID_MV_PROGRESS), PBM_SETPOS, 0, 0);
        mv_status(L"%s", s_mv_job.err[0] ? s_mv_job.err : L"The movie could not be made.");
    }
    mv_refresh();
}

static void mv_delete(void)
{
    WCHAR dir[MAX_PATH], line[MAX_PATH + 32], path[MAX_PATH], q[300], *cut;
    int sel = (int)SendMessageW(ctl(ID_MV_LIST), LB_GETCURSEL, 0, 0);
    if (sel < 0) { mv_status(L"Choose a movie in the list first."); return; }
    SendMessageW(ctl(ID_MV_LIST), LB_GETTEXT, (WPARAM)sel, (LPARAM)line);
    cut = wcsstr(line, L"   (");
    if (cut) *cut = 0;
    swprintf_s(q, 300, L"Delete the movie %s? Entrances that use it show no movie.", line);
    if (!mv_folder(dir) || MessageBoxW(s_wnd, q, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    wcscat_s(line, MAX_PATH + 32, L".bik");
    if (join(path, dir, line) && shell_op(FO_DELETE, path, NULL, FOF_ALLOWUNDO))
        mv_status(L"Deleted %s (it is in the Recycle Bin).", line);
    else
        mv_status(L"Could not delete %s (is the game using it?).", line);
    mv_refresh();
}

/* After choosing a video: its file name as the movie's name, if none yet. */
static void mv_name_from(const WCHAR *path)
{
    WCHAR name[40], n[40], *dot;
    const WCHAR *b = wcsrchr(path, L'\\');
    GetWindowTextW(ctl(ID_MV_NAME), name, 40);
    if (name[0]) return;
    wcsncpy_s(n, 40, b ? b + 1 : path, _TRUNCATE);
    dot = wcsrchr(n, L'.');
    if (dot) *dot = 0;
    set_text(ID_MV_NAME, n);
}

/* WM_COMMAND for the Movies tab; 1 if handled. */
static int mv_command(int id, int code)
{
    WCHAR f[MAX_PATH];
    switch (id) {
    case ID_MV_VIDEO_BROWSE:
        if (mv_pick(f, 0)) { set_text(ID_MV_VIDEO, f); mv_name_from(f); mv_preview(); }
        return 1;
    case ID_MV_BOTTOM_BROWSE:
        if (mv_pick(f, 1)) { set_text(ID_MV_BOTTOM, f); mv_preview(); }
        return 1;
    case ID_MV_BOTTOM_STAR:
        mv_star_menu();
        return 1;
    case ID_MV_BOTTOM_NONE:
        set_text(ID_MV_BOTTOM, L"");
        if (s_mv_preview) mv_preview();
        return 1;
    case ID_MV_FIT: case ID_MV_FILL: case ID_MV_STRETCH:
        if (code == BN_CLICKED && s_mv_preview) mv_preview();
        return 1;
    case ID_MV_PREVIEW: mv_preview(); return 1;
    case ID_MV_CREATE:  mv_create();  return 1;
    case ID_MV_STOP:
        if (s_mv_busy) { InterlockedExchange(&s_mv_job.cancel, 1); mv_status(L"Stopping\x2026"); }
        return 1;
    case ID_MV_DELETE:  mv_delete();  return 1;
    case ID_MV_OPEN:
        if (mv_folder(f)) ShellExecuteW(s_wnd, L"open", f, NULL, NULL, SW_SHOWNORMAL);
        else mv_status(L"The game folder does not exist; choose it on the Play tab first.");
        return 1;
    }
    return 0;
}

static LRESULT CALLBACK mv_view_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT rc;
        HBRUSH bg = CreateSolidBrush(RGB(24, 24, 28));
        GetClientRect(w, &rc);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        if (s_mv_preview) {
            BITMAPINFO bi;
            ZeroMemory(&bi, sizeof bi);
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = MOVIE_W;
            bi.bmiHeader.biHeight = -MOVIE_H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            SetStretchBltMode(dc, HALFTONE);
            StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, MOVIE_W, MOVIE_H, s_mv_preview, &bi, DIB_RGB_COLORS, SRCCOPY);
        } else {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(170, 170, 176));
            SelectObject(dc, s_font);
            DrawTextW(dc, L"Preview", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(w, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND)
        return 1;
    return DefWindowProcW(w, m, wp, lp);
}

static void mv_setup(void)
{
    WNDCLASSW wc;
    HWND h;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = mv_view_proc;
    wc.hInstance = s_inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"SvR2011MoviePreview";
    RegisterClassW(&wc);
    add(TAB_MOVIES, L"Static", L"Entrance movies of your own, in CREATE AN ENTRANCE \x2192 FINALIZE \x2192 MOVIE "
                               L"\x2192 USER MOVIES. The video plays on the big screen, the strip on the stage and ramp.",
        SS_LEFT, X0, 50, 560, 40, 0);
    add(TAB_MOVIES, L"Static", L"Video", SS_LEFT, X0, 100, 100, 20, 0);
    add(TAB_MOVIES, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 110, 96, 336, 24, ID_MV_VIDEO);
    add(TAB_MOVIES, L"Button", L"Browse\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 454, 95, 106, 26, ID_MV_VIDEO_BROWSE);
    add(TAB_MOVIES, L"Static", L"Bottom strip", SS_LEFT, X0, 134, 100, 20, 0);
    add(TAB_MOVIES, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 110, 130, 146, 24, ID_MV_BOTTOM);
    add(TAB_MOVIES, L"Button", L"Browse\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 262, 129, 92, 26, ID_MV_BOTTOM_BROWSE);
    add(TAB_MOVIES, L"Button", L"Superstar\x2026", BS_PUSHBUTTON | WS_TABSTOP, X0 + 360, 129, 100, 26, ID_MV_BOTTOM_STAR);
    add(TAB_MOVIES, L"Button", L"None (black)", BS_PUSHBUTTON | WS_TABSTOP, X0 + 466, 129, 94, 26, ID_MV_BOTTOM_NONE);
    add(TAB_MOVIES, L"Static", L"Big screen", SS_LEFT, X0, 168, 100, 20, 0);
    /* Fill by default: the video covers the whole big screen (black bars in
       the video itself are cut off first - movie_maker.c content_find). */
    h = add(TAB_MOVIES, L"Button", L"Fill the screen", BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, X0 + 110, 164, 140, 24, ID_MV_FILL);
    SendMessageW(h, BM_SETCHECK, BST_CHECKED, 0);
    add(TAB_MOVIES, L"Button", L"Whole picture (bars)", BS_AUTORADIOBUTTON, X0 + 256, 164, 160, 24, ID_MV_FIT);
    add(TAB_MOVIES, L"Button", L"Stretch", BS_AUTORADIOBUTTON, X0 + 422, 164, 100, 24, ID_MV_STRETCH);
    add(TAB_MOVIES, L"Static", L"Length", SS_LEFT, X0, 202, 100, 20, 0);
    h = add(TAB_MOVIES, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP, X0 + 110, 198, 190, 200, ID_MV_LENGTH);
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"Whole video (up to 3 min)");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"First 30 seconds");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"First 60 seconds");
    SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)L"First 90 seconds");
    SendMessageW(h, CB_SETCURSEL, 1, 0);
    add(TAB_MOVIES, L"Static", L"Name", SS_LEFT, X0 + 318, 202, 50, 20, 0);
    add(TAB_MOVIES, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, X0 + 370, 198, 190, 24, ID_MV_NAME);
    s_mv_view = add(TAB_MOVIES, L"SvR2011MoviePreview", L"", 0, X0, 238, 200, 200, ID_MV_VIEW);
    add(TAB_MOVIES, L"Static", L"Your movies", SS_LEFT, X0 + 220, 238, 200, 20, 0);
    add(TAB_MOVIES, L"ListBox", L"", LBS_NOTIFY | WS_VSCROLL | WS_BORDER | WS_TABSTOP | LBS_NOINTEGRALHEIGHT,
        X0 + 220, 260, 340, 138, ID_MV_LIST);
    add(TAB_MOVIES, L"Button", L"Delete", BS_PUSHBUTTON | WS_TABSTOP, X0 + 220, 406, 110, 30, ID_MV_DELETE);
    add(TAB_MOVIES, L"Button", L"Open folder", BS_PUSHBUTTON | WS_TABSTOP, X0 + 340, 406, 120, 30, ID_MV_OPEN);
    add(TAB_MOVIES, L"Button", L"Preview", BS_PUSHBUTTON | WS_TABSTOP, X0, 452, 110, 32, ID_MV_PREVIEW);
    add(TAB_MOVIES, L"Button", L"Make movie", BS_DEFPUSHBUTTON | WS_TABSTOP, X0 + 120, 452, 140, 32, ID_MV_CREATE);
    add(TAB_MOVIES, L"Button", L"Stop", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, X0 + 270, 452, 90, 32, ID_MV_STOP);
    add(TAB_MOVIES, PROGRESS_CLASSW, L"", PBS_SMOOTH, X0, 494, 560, 18, ID_MV_PROGRESS);
    add(TAB_MOVIES, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0, 520, 560, 44, ID_MV_STATUS);
    {   /* tests (with --capture movies): SVR2011_MOVIE_VIDEO / _BOTTOM fill the fields and show a preview */
        WCHAR v[MAX_PATH];
        if (GetEnvironmentVariableW(L"SVR2011_MOVIE_VIDEO", v, MAX_PATH)) {
            set_text(ID_MV_VIDEO, v);
            if (GetEnvironmentVariableW(L"SVR2011_MOVIE_BOTTOM", v, MAX_PATH)) set_text(ID_MV_BOTTOM, v);
            mv_preview();
            if (GetEnvironmentVariableW(L"SVR2011_MOVIE_STAR", v, MAX_PATH)) {   /* as picked with Superstar... */
                int i;
                for (i = 0; i < (int)(sizeof k_star_movies / sizeof *k_star_movies); i++)
                    if (!_wcsicmp(k_star_movies[i].name, v)) mv_star_pick(i);
            }
        }
    }
}

/* ── updates (updater.c): check GitHub, download, copy the new files in ── */

static UpdateInfo s_up;
static volatile LONG s_up_cancel;
static int s_up_available, s_up_auto_run;

static void up_status(const WCHAR *fmt, ...)
{
    WCHAR buf[600];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(buf, 600, fmt, ap);
    va_end(ap);
    set_text(ID_UP_STATUS, buf);
}

static DWORD WINAPI up_check_thread(LPVOID arg)
{
    WCHAR err[512] = L"";
    UpdateInfo info;
    int ok = update_check(&info, err, 512);
    (void)arg;
    if (ok) s_up = info;
    PostMessageW(s_wnd, WM_APP_UPD_CHECKED, (WPARAM)ok, ok ? 0 : (LPARAM)wdup(err));
    return 0;
}

static void up_check(int automatic)
{
    if (InterlockedCompareExchange(&s_up_busy, 1, 0))
        return;
    s_up_auto_run = automatic;
    up_status(L"Checking for updates\x2026");
    EnableWindow(ctl(ID_UP_BUTTON), FALSE);
    CloseHandle(CreateThread(NULL, 0, up_check_thread, NULL, 0, NULL));
}

/* Copies the top-level files of `from` (and its native_shaders folder) into
 * `to`, keeping the player's settings. The running launcher is renamed out of
 * the way first (Windows lets a running exe be renamed, not overwritten). */
static int up_copy_into(const WCHAR *from, const WCHAR *to, WCHAR *err, size_t errn)
{
    static const WCHAR *keep[] = { L"launcher.ini", GAME_TOML };
    WCHAR pat[MAX_PATH], src[MAX_PATH], dst[MAX_PATH], old[MAX_PATH + 8], sub[MAX_PATH], subdst[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    size_t i;
    int ok = 1;
    if (!join(pat, from, L"*") || (h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return 0;
    do {
        int skip = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        for (i = 0; i < sizeof keep / sizeof *keep; i++)
            if (!_wcsicmp(fd.cFileName, keep[i]))
                skip = 1;
        if (skip || !join(src, from, fd.cFileName) || !join(dst, to, fd.cFileName))
            continue;
        if (!_wcsicmp(dst, s_launcher_exe)) {
            swprintf_s(old, MAX_PATH + 8, L"%s.old", dst);
            DeleteFileW(old);
            MoveFileExW(dst, old, MOVEFILE_REPLACE_EXISTING);
        }
        if (!CopyFileW(src, dst, FALSE)) {
            swprintf_s(err, errn, L"Could not replace %s (error %lu). Is the game or another launcher running?",
                       dst, GetLastError());
            ok = 0;
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    if (ok && join(sub, from, L"native_shaders") && dir_exists(sub) && join(subdst, to, L"native_shaders")) {
        CreateDirectoryW(subdst, NULL);
        if (join(pat, sub, L"*") && (h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && join(src, sub, fd.cFileName)
                        && join(dst, subdst, fd.cFileName))
                    CopyFileW(src, dst, FALSE);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    return ok;
}

/* Downloads s_up's zip and copies its files over the install. */
static int up_install(WCHAR *err, size_t errn)
{
    WCHAR tmp[MAX_PATH], work[MAX_PATH], zip[MAX_PATH], files[MAX_PATH], probe[MAX_PATH];
    WCHAR name[80];
    int ok = 0;
    GetTempPathW(MAX_PATH, tmp);
    if (!join(work, tmp, L"svr2011-update"))
        goto done;
    remove_tree(work);
    CreateDirectoryW(work, NULL);
    swprintf_s(name, 80, L"SvR2011-PC-v%s.zip", s_up.version);
    if (!join(zip, work, name) || !join(files, work, L"files"))
        goto done;
    CreateDirectoryW(files, NULL);
    if (!update_download(s_up.url, zip, s_wnd, WM_APP_UPD_PROGRESS, &s_up_cancel, err, errn))
        goto done;
    PostMessageW(s_wnd, WM_APP_UPD_PROGRESS, 100, 1);
    if (!unpack(zip, files)) {
        swprintf_s(err, errn, L"Could not unpack the update.");
        goto done;
    }
    if (!join(probe, files, GAME_EXE) || !file_exists(probe)) {
        swprintf_s(err, errn, L"The update has no %s; nothing was changed.", GAME_EXE);
        goto done;
    }
    if (any_game_running()) {
        swprintf_s(err, errn, L"The game is running. Close it, then update again.");
        goto done;
    }
    /* The game folder, and the launcher's own folder if it is another one. */
    ok = 1;
    if (s_game_dir[0] && dir_exists(s_game_dir) && has_program(s_game_dir))
        ok = up_copy_into(files, s_game_dir, err, errn);
    if (ok && !same_dir(s_game_dir, s_launcher_dir))
        ok = up_copy_into(files, s_launcher_dir, err, errn);
done:
    remove_tree(work);
    return ok;
}

static DWORD WINAPI up_apply_thread(LPVOID arg)
{
    WCHAR err[600] = L"";
    int ok = up_install(err, 600);
    (void)arg;
    PostMessageW(s_wnd, WM_APP_UPD_DONE, (WPARAM)ok, ok ? 0 : (LPARAM)wdup(err[0] ? err : L"The update failed."));
    return 0;
}

static void up_apply(void)
{
    WCHAR q[400];
    if (!s_up_available || s_up_busy || s_busy)
        return;
    if (any_game_running()) {
        up_status(L"Close the game first, then update.");
        return;
    }
    swprintf_s(q, 400, L"Update the PC port to version %s?\n\nThe launcher downloads it (%.0f MB), replaces the "
                       L"program files and restarts. Your saves, settings, music and movies are kept.",
               s_up.version, s_up.size / 1048576.0);
    if (MessageBoxW(s_wnd, q, WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    InterlockedExchange(&s_up_busy, 1);
    InterlockedExchange(&s_up_cancel, 0);
    EnableWindow(ctl(ID_UP_BUTTON), FALSE);
    EnableWindow(ctl(ID_PLAY), FALSE);
    ShowWindow(ctl(ID_UP_PROGRESS), SW_SHOW);
    SendMessageW(ctl(ID_UP_PROGRESS), PBM_SETPOS, 0, 0);
    up_status(L"Downloading version %s\x2026", s_up.version);
    CloseHandle(CreateThread(NULL, 0, up_apply_thread, NULL, 0, NULL));
}

static void up_checked(int ok, WCHAR *err)
{
    InterlockedExchange(&s_up_busy, 0);
    EnableWindow(ctl(ID_UP_BUTTON), TRUE);
    s_up_available = ok && version_newer(s_up.version, PORT_VERSION);
    if (!ok) {
        up_status(L"%s", err ? err : L"Could not check for updates.");
    } else if (s_up_available) {
        WCHAR b[64];
        up_status(L"Version %s is available%s.", s_up.version, s_up.prerelease ? L" (preview)" : L"");
        swprintf_s(b, 64, L"Update to %s", s_up.version);
        set_text(ID_UP_BUTTON, b);
        if (s_up_auto_run)
            up_apply();
    } else {
        up_status(L"You have the newest version.");
    }
    free(err);
}

static void up_done(int ok, WCHAR *err)
{
    ShowWindow(ctl(ID_UP_PROGRESS), SW_HIDE);
    InterlockedExchange(&s_up_busy, 0);
    EnableWindow(ctl(ID_UP_BUTTON), TRUE);
    refresh_play();
    if (!ok) {
        up_status(L"%s", err ? err : L"The update failed.");
        free(err);
        return;
    }
    {
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        WCHAR cmd[MAX_PATH + 32];
        MessageBoxW(s_wnd, L"The PC port is updated. The launcher restarts now.", WINDOW_TITLE,
                    MB_OK | MB_ICONINFORMATION);
        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        swprintf_s(cmd, MAX_PATH + 32, L"\"%s\" --updated", s_launcher_exe);
        if (CreateProcessW(s_launcher_exe, cmd, NULL, NULL, FALSE, 0, NULL, s_launcher_dir, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        save_launcher_ini();
        DestroyWindow(s_wnd);
    }
}

/* WM_COMMAND for the update controls; 1 if handled. */
static int up_command(int id, int code)
{
    (void)code;
    if (id == ID_UP_BUTTON) {
        if (s_up_available) up_apply();
        else up_check(0);
        return 1;
    }
    if (id == ID_UP_AUTO) {
        WritePrivateProfileStringW(L"Launcher", L"CheckUpdates",
            IsDlgButtonChecked(s_wnd, ID_UP_AUTO) == BST_CHECKED ? L"1" : L"0", s_launcher_ini);
        return 1;
    }
    return 0;
}

static void up_setup(void)
{
    WCHAR v[64];
    swprintf_s(v, 64, L"Version %s", PORT_VERSION);
    add(TAB_PLAY, L"Button", L"Updates", BS_GROUPBOX, X0, 410, 560, 110, 0);
    add(TAB_PLAY, L"Static", v, SS_LEFT, X0 + 14, 434, 150, 20, ID_VERSION);
    add(TAB_PLAY, L"Static", L"", SS_LEFT | SS_NOPREFIX, X0 + 14, 456, 380, 36, ID_UP_STATUS);
    add(TAB_PLAY, L"Button", L"Check for updates", BS_PUSHBUTTON | WS_TABSTOP, X0 + 400, 430, 146, 28, ID_UP_BUTTON);
    add(TAB_PLAY, L"Button", L"Check when the launcher starts", BS_AUTOCHECKBOX | WS_TABSTOP,
        X0 + 14, 490, 300, 22, ID_UP_AUTO);
    add(TAB_PLAY, PROGRESS_CLASSW, L"", PBS_SMOOTH, X0 + 330, 494, 216, 14, ID_UP_PROGRESS);
    ShowWindow(ctl(ID_UP_PROGRESS), SW_HIDE);
    CheckDlgButton(s_wnd, ID_UP_AUTO,
                   GetPrivateProfileIntW(L"Launcher", L"CheckUpdates", 1, s_launcher_ini) ? BST_CHECKED : BST_UNCHECKED);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_NOTIFY: {
        NMHDR *n = (NMHDR *)lp;
        if (n->idFrom == ID_TAB && n->code == TCN_SELCHANGE)
            show_tab(TabCtrl_GetCurSel(s_tab));
        break;
    }
    case WM_COMMAND:
        if (mv_command(LOWORD(wp), HIWORD(wp)) || up_command(LOWORD(wp), HIWORD(wp)))
            return 0;
        switch (LOWORD(wp)) {
        case ID_PLAY:
            play();
            break;
        case ID_GAMEDIR_CHANGE: {
            WCHAR d[MAX_PATH];
            if (pick_folder(L"Choose the folder that holds the installed game", d)) {
                if (!is_game_folder(d)
                        && MessageBoxW(w, L"That folder does not have the game's files in it (default.xex). "
                                          L"To set the game up from your disc image, use the Install tab.\n\n"
                                          L"Use this folder anyway?",
                                       WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
                    break;
                wcscpy_s(s_game_dir, MAX_PATH, d);
                save_launcher_ini();
                settings_load();
                refresh_play();
            }
            break;
        }
        case ID_MUSIC_OPEN: {
            WCHAR m[MAX_PATH];
            if (!s_game_dir[0] || !dir_exists(s_game_dir)) {
                set_text(ID_SETTINGS_STATUS, L"The game folder does not exist; choose it on the Play tab first.");
                break;
            }
            join(m, s_game_dir, L"Music");
            CreateDirectoryW(m, NULL);
            ShellExecuteW(s_wnd, L"open", m, NULL, NULL, SW_SHOWNORMAL);
            break;
        }
        case ID_RESOLUTION: case ID_INPUT: case ID_AUDIO: case ID_RENDERER:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                s_settings_dirty = 1;
                set_text(ID_SETTINGS_STATUS, L"");
            }
            break;
        case ID_WINDOWED: case ID_FULLSCREEN: case ID_VSYNC: case ID_MUTE: case ID_SHOWFPS: case ID_MSAA:
            if (HIWORD(wp) == BN_CLICKED) {
                s_settings_dirty = 1;
                set_text(ID_SETTINGS_STATUS, L"");
            }
            break;
        case ID_DEFAULTS:
            settings_defaults();
            break;
        case ID_SAVE:
            settings_save();
            break;
        case ID_IMAGE_BROWSE: {
            WCHAR p[MAX_PATH];
            if (pick_image(p)) {
                set_text(ID_IMAGE, p);
                check_image(p);
            }
            break;
        }
        case ID_IMAGE:
            if (HIWORD(wp) == EN_CHANGE)
                s_image_bytes = 0;
            break;
        case ID_TARGET:
            if (HIWORD(wp) == EN_CHANGE)
                update_free_space();
            break;
        case ID_TARGET_BROWSE: {
            WCHAR p[MAX_PATH];
            if (pick_folder(L"Choose where to install the game", p))
                set_text(ID_TARGET, p);
            break;
        }
        case ID_INSTALL:
            start_install();
            break;
        case ID_DLC_BROWSE: {
            WCHAR p[MAX_PATH];
            if (pick_folder(L"Choose the folder with the DLC", p))
                set_text(ID_DLC_DIR, p);
            break;
        }
        case ID_DLC_INSTALL:
            start_dlc();
            break;
        case ID_SV_BACKUP:   saves_backup();       break;
        case ID_SV_RESTORE:  saves_restore();      break;
        case ID_SV_BACKUPS:  saves_open_backups(); break;
        case ID_SV_EXPORT:   saves_export();       break;
        case ID_SV_IMPORT:   saves_import();       break;
        case ID_SV_DELETE:   saves_delete();       break;
        case ID_SV_OPEN:     saves_open();         break;
        case ID_PT_EXPORT:    pt_export();         break;
        case ID_PT_IMPORT:    pt_change(1);        break;
        case ID_PT_DELETE:    pt_change(0);        break;
        case ID_PT_EXPORTALL: pt_export_all();     break;
        case ID_PT_REFRESH:   pt_refresh();        break;
        case ID_CANCEL:
            if (s_busy) {
                InterlockedExchange(&s_cancel, 1);
                set_text(ID_INSTALL_STATUS, L"Cancelling\x2026");
            }
            break;
        }
        break;
    case WM_APP_PROGRESS:
        SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, wp, 0);
        if (lp) {
            if (!s_cancel)
                set_text(ID_INSTALL_STATUS, (WCHAR *)lp);
            free((void *)lp);
        }
        break;
    case WM_APP_UPD_CHECKED:
        up_checked((int)wp, (WCHAR *)lp);
        return 0;
    case WM_APP_UPD_PROGRESS:
        SendMessageW(ctl(ID_UP_PROGRESS), PBM_SETPOS, wp, 0);
        if (lp) up_status(L"Installing version %s\x2026", s_up.version);
        return 0;
    case WM_APP_UPD_DONE:
        up_done((int)wp, (WCHAR *)lp);
        return 0;
    case WM_APP_MOVIE:
        SendMessageW(ctl(ID_MV_PROGRESS), PBM_SETPOS, wp, 0);
        return 0;
    case WM_APP_MOVIE_DONE:
        mv_done((int)wp);
        return 0;
    case WM_APP_DLC: {
        WCHAR *msg = (WCHAR *)lp;
        if (msg)
            set_text(ID_DLC_STATUS, msg);
        if (wp) {
            WCHAR list[1300];
            swprintf_s(list, 1300, L"%s", s_dlc.names[0] ? s_dlc.names + 1 : L"");
            set_text(ID_DLC_LIST, list);
            EnableWindow(ctl(ID_DLC_INSTALL), TRUE);
            EnableWindow(ctl(ID_DLC_BROWSE), TRUE);
        }
        free(msg);
        break;
    }
    case WM_APP_DONE: {
        WCHAR *msg = (WCHAR *)lp, t[1400];
        set_busy(0);
        if (wp) {
            SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 1000, 0);
            wcscpy_s(s_game_dir, MAX_PATH, s_job.target);
            save_launcher_ini();
            settings_load();
            swprintf_s(t, 1400, L"Installed in %s. %s", s_game_dir, msg ? msg : L"");
            set_text(ID_INSTALL_STATUS, t);
            show_tab(TAB_PLAY);
            if (has_program(s_game_dir))
                set_text(ID_PLAY_STATUS, L"Installed. Ready.");
        } else {
            swprintf_s(t, 1400, L"The game was not installed. %s", msg ? msg : L"");
            set_text(ID_INSTALL_STATUS, t);
            SendMessageW(ctl(ID_PROGRESS), PBM_SETPOS, 0, 0);
        }
        free(msg);
        break;
    }
    case WM_APP_GAMEEND: {
        WCHAR t[200];
        refresh_play();
        if (wp == 0)
            swprintf_s(t, 200, L"Ready. The game exited normally (exit code 0).");
        else
            swprintf_s(t, 200, L"The game exited with code %lu (0x%08lX).", (unsigned long)wp, (unsigned long)wp);
        set_text(ID_PLAY_STATUS, t);
        break;
    }
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        s_dpi = HIWORD(wp);
        SetWindowPos(w, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        relayout();
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_PAINT: {
        /* The title band: near-black with a red rule under it. */
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        RECT r, tr, rule;
        HBRUSH band = CreateSolidBrush(RGB(18, 18, 22)), red = CreateSolidBrush(RGB(200, 16, 32));
        GetClientRect(w, &r);
        r.bottom = S(HEADER_H);
        FillRect(dc, &r, band);
        rule = r;
        rule.top = r.bottom - S(4);
        FillRect(dc, &rule, red);
        rule = r;
        rule.left = S(16);
        rule.right = S(22);
        rule.top = S(12);
        rule.bottom = S(50);
        FillRect(dc, &rule, red);
        DeleteObject(band);
        DeleteObject(red);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(250, 250, 250));
        SelectObject(dc, s_title);
        tr = r;
        tr.left = S(32);
        tr.top = S(6);
        tr.bottom = S(36);
        DrawTextW(dc, L"WWE SmackDown vs. Raw 2011", -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SetTextColor(dc, RGB(170, 170, 178));
        SelectObject(dc, s_font);
        tr.top = S(34);
        tr.bottom = S(56);
        DrawTextW(dc, L"PC Port Launcher", -1, &tr, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (s_busy) {
            if (MessageBoxW(w, L"Stop the installation and close?", WINDOW_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            InterlockedExchange(&s_cancel, 1);
            if (s_worker)
                WaitForSingleObject(s_worker, 10000);
        }
        if (s_settings_dirty && dir_exists(s_game_dir)) {
            int r = MessageBoxW(w, L"Save the changed settings?", WINDOW_TITLE, MB_YESNOCANCEL | MB_ICONQUESTION);
            if (r == IDCANCEL)
                return 0;
            if (r == IDYES)
                settings_save();
        }
        save_launcher_ini();
        DestroyWindow(w);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

/* --capture: the window as a 24-bit BMP (for checking the layout). */
static void capture(const WCHAR *file)
{
    RECT r;
    HDC wdc, mdc;
    HBITMAP bmp;
    BITMAPINFOHEADER bi;
    BITMAPFILEHEADER fh;
    uint8_t *bits;
    int w, h, stride;
    FILE *f;
    GetWindowRect(s_wnd, &r);
    w = r.right - r.left;
    h = r.bottom - r.top;
    wdc = GetDC(s_wnd);
    mdc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, w, h);
    SelectObject(mdc, bmp);
    PrintWindow(s_wnd, mdc, 2 /* PW_RENDERFULLCONTENT */);
    stride = (w * 3 + 3) & ~3;
    bits = (uint8_t *)malloc((size_t)stride * (size_t)h);
    if (bits) {
        memset(&bi, 0, sizeof bi);
        bi.biSize = sizeof bi; bi.biWidth = w; bi.biHeight = h; bi.biPlanes = 1; bi.biBitCount = 24;
        GetDIBits(mdc, bmp, 0, (UINT)h, bits, (BITMAPINFO *)&bi, DIB_RGB_COLORS);
        memset(&fh, 0, sizeof fh);
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof fh + sizeof bi;
        fh.bfSize = fh.bfOffBits + (DWORD)(stride * h);
        if (!_wfopen_s(&f, file, L"wb") && f) {
            fwrite(&fh, sizeof fh, 1, f);
            fwrite(&bi, sizeof bi, 1, f);
            fwrite(bits, (size_t)stride * (size_t)h, 1, f);
            fclose(f);
        }
        free(bits);
    }
    DeleteObject(bmp);
    DeleteDC(mdc);
    ReleaseDC(s_wnd, wdc);
}

/* --install: print to the console the launcher was started from. */
static void console_setup(void)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    int inherited = h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    AttachConsole(ATTACH_PARENT_PROCESS);
    if (inherited)
        s_out = h;                                   /* a pipe or file from the parent */
    else
        s_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    MSG msg;
    RECT r;
    int argc = 0, capture_tab = -1, capture_seq[8], capture_seq_n = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc), *slash, *capture_file = NULL;
    (void)prev; (void)cmd;

    s_inst = inst;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    GetModuleFileNameW(NULL, s_launcher_exe, MAX_PATH);
    wcscpy_s(s_launcher_dir, MAX_PATH, s_launcher_exe);
    slash = wcsrchr(s_launcher_dir, L'\\');
    if (slash)
        *slash = 0;
    join(s_launcher_ini, s_launcher_dir, L"launcher.ini");
    load_launcher_ini();
    {   /* the launcher an update replaced */
        WCHAR old[MAX_PATH + 8];
        int k;
        swprintf_s(old, MAX_PATH + 8, L"%s.old", s_launcher_exe);
        for (k = 0; k < 20 && file_exists(old) && !DeleteFileW(old); k++)
            Sleep(250);
    }

    /* --install <image> <folder>: install without a window (tests). */
    if (argv && argc >= 5 && (!wcscmp(argv[1], L"--paint-export") || !wcscmp(argv[1], L"--paint-import"))) {
        console_setup();
        return pt_console(argv[1][8] == L'i', argv[2], _wtoi(argv[3]), argv[4]);
    }
    /* --update [<game folder>] [--from <version>]: check GitHub and install the
       newest release without a window (--from pretends to be that version). */
    if (argv && argc >= 2 && !wcscmp(argv[1], L"--update")) {
        WCHAR err[600] = L"";
        const WCHAR *from = PORT_VERSION;
        int i;
        console_setup();
        for (i = 2; i < argc; i++) {
            if (!wcscmp(argv[i], L"--from") && i + 1 < argc) from = argv[++i];
            else wcscpy_s(s_game_dir, MAX_PATH, argv[i]);
        }
        if (!update_check(&s_up, err, 600)) { wprintf(L"check failed: %s\n", err); return 1; }
        wprintf(L"newest release %s (%s), this is %s\n", s_up.version, s_up.url, from);
        if (!version_newer(s_up.version, from)) { wprintf(L"up to date\n"); return 0; }
        if (!up_install(err, 600)) { wprintf(L"update failed: %s\n", err); return 1; }
        wprintf(L"updated %s to %s\n", s_game_dir, s_up.version);
        return 0;
    }
    /* --movie-make <video> <bottom or -> <fit 0-2> <seconds> <out.bik> (tests) */
    if (argv && argc >= 7 && !wcscmp(argv[1], L"--movie-make")) {
        static MovieJob job;
        console_setup();
        wcscpy_s(job.video, MAX_PATH, argv[2]);
        if (wcscmp(argv[3], L"-")) wcscpy_s(job.bottom, MAX_PATH, argv[3]);
        job.fit = _wtoi(argv[4]);
        job.max_seconds = _wtoi(argv[5]);
        wcscpy_s(job.out, MAX_PATH, argv[6]);
        if (!movie_make(&job)) { wprintf(L"failed: %s\n", job.err); return 1; }
        wprintf(L"ok: %d frames\n", job.frames);
        return 0;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--install-dlc")) {   /* <folder> <game folder> */
        s_console = 1;
        console_setup();
        wcscpy_s(s_dlc.src, MAX_PATH, argv[2]);
        wcscpy_s(s_dlc.game, MAX_PATH, argv[3]);
        dlc_thread(NULL);
        return s_dlc.failed ? 1 : 0;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--install")) {
        int ok;
        s_console = 1;
        console_setup();
        wcscpy_s(s_job.image, MAX_PATH, argv[2]);
        if (!GetFullPathNameW(argv[3], MAX_PATH, s_job.target, NULL))
            wcscpy_s(s_job.target, MAX_PATH, argv[3]);
        post_progress(0, L"Installing %s", s_job.image);
        post_progress(0, L"        to %s", s_job.target);
        ok = install_run(&s_job);
        post_progress(0, L"%s: %s", ok ? L"installed" : L"failed", s_install_msg);
        return ok ? 0 : 1;
    }
    if (argv && argc >= 4 && !wcscmp(argv[1], L"--capture")) {
        static const WCHAR *names[TAB_COUNT] = { L"play", L"settings", L"install", L"dlc", L"saves", L"paint", L"movies" };
        int i;
        const WCHAR *p = argv[2];
        /* A comma list: each tab is shown in turn (after 300 ms), the last captured. */
        capture_seq_n = 0;
        while (*p && capture_seq_n < 8) {
            const WCHAR *c = wcschr(p, L',');
            size_t len = c ? (size_t)(c - p) : wcslen(p);
            for (i = 0; i < TAB_COUNT; i++)
                if (wcslen(names[i]) == len && !_wcsnicmp(p, names[i], len))
                    capture_seq[capture_seq_n++] = i;
            p += len + (c ? 1 : 0);
        }
        if (capture_seq_n)
            capture_tab = capture_seq[0];
        capture_file = argv[3];
    }

    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);
    s_dpi = (int)GetDpiForSystem();
    make_fonts();
    /* The game's own icon (launcher.rc, from its Xbox 360 title image). */
    s_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_SHARED | LR_DEFAULTSIZE);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"SvR2011Launcher";
    wc.hIcon = s_icon;
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    RegisterClassExW(&wc);
    r.left = 0; r.top = 0; r.right = S(CLIENT_W); r.bottom = S(CLIENT_H + DY);
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0, (UINT)s_dpi);
    s_wnd = CreateWindowExW(0, wc.lpszClassName, WINDOW_TITLE,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (!s_wnd)
        return 1;
    build_ui();
    refresh_play();
    show_tab(capture_tab >= 0 ? capture_tab : (is_game_folder(s_game_dir) ? TAB_PLAY : TAB_INSTALL));
    if (capture_file) {
        /* Drawn off-screen, never activated (tests run beside the user's work). */
        SetWindowPos(s_wnd, NULL, -8000, -8000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        show = SW_SHOWNOACTIVATE;
    }
    ShowWindow(s_wnd, show);
    UpdateWindow(s_wnd);
    if (!capture_file && GetPrivateProfileIntW(L"Launcher", L"CheckUpdates", 1, s_launcher_ini))
        up_check(1);
    if (capture_file) {
        MSG pm;
        int k;
        for (k = 0; k < (capture_seq_n ? capture_seq_n : 1); k++) {
            ULONGLONG t0 = GetTickCount64();
            if (k) {
                /* as a click on the tab does */
                NMHDR n = { s_tab, ID_TAB, TCN_SELCHANGE };
                TabCtrl_SetCurSel(s_tab, capture_seq[k]);
                SendMessageW(s_wnd, WM_NOTIFY, ID_TAB, (LPARAM)&n);
            }
            while (GetTickCount64() - t0 < 400)
                while (PeekMessageW(&pm, NULL, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&pm);
                    DispatchMessageW(&pm);
                }
        }
        capture(capture_file);
        return 0;
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(s_wnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
