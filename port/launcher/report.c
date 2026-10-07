/* WWE SmackDown vs. Raw 2011 launcher - "Report a problem" (report.h). */

#include "report.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ── a small zip writer (stored entries) ─────────────────────────────────── */

static uint32_t s_crc[256];

static void crc_init(void)
{
    uint32_t i, k, c;
    for (i = 0; i < 256; i++) {
        for (c = i, k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        s_crc[i] = c;
    }
}

static uint32_t crc32_of(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    while (n--)
        c = s_crc[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

typedef struct {
    char name[260];
    uint32_t crc, size, offset;
    uint16_t time, date;
} ZEntry;

typedef struct {
    FILE *f;
    ZEntry e[64];
    int n;
} Zip;

static void w16(FILE *f, uint32_t v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void w32(FILE *f, uint32_t v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }

static void dos_time(uint16_t *t, uint16_t *d)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    *t = (uint16_t)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
    *d = (uint16_t)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
}

static int zip_add(Zip *z, const char *name, const uint8_t *data, size_t size)
{
    ZEntry *e;
    size_t nl = strlen(name);
    if (z->n >= 64 || nl >= sizeof z->e[0].name || size > 0x7FFFFFFF)
        return 0;
    e = &z->e[z->n++];
    strcpy_s(e->name, sizeof e->name, name);
    e->crc = crc32_of(data, size);
    e->size = (uint32_t)size;
    e->offset = (uint32_t)ftell(z->f);
    dos_time(&e->time, &e->date);
    w32(z->f, 0x04034b50); w16(z->f, 10); w16(z->f, 0x0800); w16(z->f, 0);
    w16(z->f, e->time); w16(z->f, e->date); w32(z->f, e->crc); w32(z->f, e->size); w32(z->f, e->size);
    w16(z->f, (uint32_t)nl); w16(z->f, 0);
    fwrite(name, 1, nl, z->f);
    fwrite(data, 1, size, z->f);
    return 1;
}

static void zip_finish(Zip *z)
{
    uint32_t start = (uint32_t)ftell(z->f), i;
    for (i = 0; i < (uint32_t)z->n; i++) {
        ZEntry *e = &z->e[i];
        size_t nl = strlen(e->name);
        w32(z->f, 0x02014b50); w16(z->f, 20); w16(z->f, 10); w16(z->f, 0x0800); w16(z->f, 0);
        w16(z->f, e->time); w16(z->f, e->date); w32(z->f, e->crc); w32(z->f, e->size); w32(z->f, e->size);
        w16(z->f, (uint32_t)nl); w16(z->f, 0); w16(z->f, 0); w16(z->f, 0); w16(z->f, 0); w32(z->f, 0);
        w32(z->f, e->offset);
        fwrite(e->name, 1, nl, z->f);
    }
    {
        uint32_t end = (uint32_t)ftell(z->f);
        w32(z->f, 0x06054b50); w16(z->f, 0); w16(z->f, 0); w16(z->f, (uint32_t)z->n); w16(z->f, (uint32_t)z->n);
        w32(z->f, end - start); w32(z->f, start); w16(z->f, 0);
    }
}

/* ── the report ──────────────────────────────────────────────────────────── */

static uint8_t *read_file(const WCHAR *path, size_t *size, size_t max)
{
    FILE *f;
    uint8_t *buf;
    long n;
    if (_wfopen_s(&f, path, L"rb") || !f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    if ((size_t)n > max) {  /* (the end of a big log: what happened last) */
        fseek(f, n - (long)max, SEEK_SET);
        n = (long)max;
    }
    buf = (uint8_t *)malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    *size = fread(buf, 1, (size_t)n, f);
    fclose(f);
    return buf;
}

/* Every IPv4 address (the P2P lines log the player's public one) becomes
   x.x.x.x, in place - the text only gets shorter. Loopback stays, and so do
   lines with "ersion" in them (driver / API versions such as 1.3.1.1). */
static int is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

static void redact_ips(uint8_t *data, size_t *size)
{
    size_t i = 0, o = 0, n = *size, line = 0;
    int skip_line = -1;  /* (-1: not looked at yet) */
    while (i < n) {
        if (skip_line < 0) {
            size_t e = line;
            skip_line = 0;
            while (e < n && data[e] != '\n') {
                if (e + 5 < n && !memcmp(data + e, "ersion", 6)) skip_line = 1;
                e++;
            }
        }
        if (!skip_line && is_digit(data[i]) && (i == 0 || (!is_digit(data[i - 1]) && data[i - 1] != '.'))) {
            size_t p = i;
            int part, ok = 1;
            unsigned v[4];
            for (part = 0; part < 4 && ok; part++) {
                size_t s = p;
                v[part] = 0;
                while (p < n && is_digit(data[p]) && p - s < 3)
                    v[part] = v[part] * 10 + (data[p++] - '0');
                if (p == s || v[part] > 255) ok = 0;
                else if (part < 3) {
                    if (p < n && data[p] == '.') p++;
                    else ok = 0;
                }
            }
            if (ok && (p >= n || (!is_digit(data[p]) && !(data[p] == '.' && p + 1 < n && is_digit(data[p + 1]))))) {
                if (v[0] != 127) {
                    memcpy(data + o, "x.x.x.x", 7);
                    o += 7;
                } else {
                    memmove(data + o, data + i, p - i);
                    o += p - i;
                }
                i = p;
                continue;
            }
        }
        if (data[i] == '\n') {
            line = i + 1;
            skip_line = -1;
        }
        data[o++] = data[i++];
    }
    *size = o;
}

typedef struct {
    WCHAR name[MAX_PATH];
    FILETIME t;
} Found;

static int newer(const void *a, const void *b)
{
    return CompareFileTime(&((const Found *)b)->t, &((const Found *)a)->t);
}

/* The newest `want` files matching dir\pattern, added as folder/<name>. */
static int add_newest(Zip *z, const WCHAR *dir, const WCHAR *pattern, int want, const char *folder, size_t max)
{
    WCHAR pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    Found found[256];
    int n = 0, i, added = 0;
    swprintf_s(pat, MAX_PATH, L"%s\\%s", dir, pattern);
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < 256) {
            wcscpy_s(found[n].name, MAX_PATH, fd.cFileName);
            found[n].t = fd.ftLastWriteTime;
            n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(found, (size_t)n, sizeof found[0], newer);
    for (i = 0; i < n && added < want; i++) {
        size_t size = 0;
        uint8_t *data;
        char name[260], u[200];
        swprintf_s(path, MAX_PATH, L"%s\\%s", dir, found[i].name);
        data = read_file(path, &size, max);
        if (!data)
            continue;
        redact_ips(data, &size);
        WideCharToMultiByte(CP_UTF8, 0, found[i].name, -1, u, sizeof u, NULL, NULL);
        sprintf_s(name, sizeof name, "%s/%s", folder, u);
        added += zip_add(z, name, data, size);
        free(data);
    }
    return added;
}

/* svr2011.toml without the account's secrets. */
static void add_settings(Zip *z, const WCHAR *game_dir)
{
    WCHAR path[MAX_PATH];
    size_t size = 0, i = 0, o = 0;
    uint8_t *data, *out;
    swprintf_s(path, MAX_PATH, L"%s\\svr2011.toml", game_dir);
    data = read_file(path, &size, 1 << 20);
    if (!data)
        return;
    out = (uint8_t *)malloc(size + 64);
    if (!out) {
        free(data);
        return;
    }
    while (i < size) {
        size_t e = i;
        const char *line = (const char *)data + i;
        while (e < size && data[e] != '\n')
            e++;
        {
            size_t k = 0;
            while (line[k] == ' ' || line[k] == '\t')
                k++;
            if (!strncmp(line + k, "online_token", 12) || !strncmp(line + k, "online_password", 15)) {
                const char *red = !strncmp(line + k, "online_token", 12) ? "online_token = \"(removed)\"\n"
                                                                        : "online_password = \"(removed)\"\n";
                size_t rl = strlen(red);
                memcpy(out + o, red, rl);
                o += rl;
            } else {
                memcpy(out + o, data + i, e - i + (e < size ? 1 : 0));
                o += e - i + (e < size ? 1 : 0);
            }
        }
        i = e + 1;
    }
    zip_add(z, "svr2011.toml", out, o);
    free(out);
    free(data);
}

int report_make(const WCHAR *game_dir, const WCHAR *version, WCHAR *out, int outn, int *logs, int *crashes)
{
    return report_make_ex(game_dir, version, NULL, out, outn, logs, crashes);
}

int report_make_ex(const WCHAR *game_dir, const WCHAR *version, const char *description, WCHAR *out, int outn,
                   int *logs, int *crashes)
{
    WCHAR dir[MAX_PATH], sub[MAX_PATH];
    SYSTEMTIME st;
    Zip z;
    char info[512];
    OSVERSIONINFOW os = {sizeof os};
    crc_init();
    *logs = *crashes = 0;
    swprintf_s(dir, MAX_PATH, L"%s\\Reports", game_dir);
    CreateDirectoryW(dir, NULL);
    GetLocalTime(&st);
    swprintf_s(out, outn, L"%s\\SvR2011-report-%04d%02d%02d-%02d%02d%02d.zip", dir, st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    memset(&z, 0, sizeof z);
    if (_wfopen_s(&z.f, out, L"wb") || !z.f)
        return 0;
    {
        /* (GetVersionEx reports 6.2 without a manifest: RtlGetVersion tells the truth) */
        typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
        RtlGetVersionFn rtl = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
        if (rtl)
            rtl(&os);
        sprintf_s(info, sizeof info,
                  "SvR 2011 PC port problem report\r\nlauncher version: %ls\r\nWindows: %lu.%lu.%lu\r\n"
                  "made: %04d-%02d-%02d %02d:%02d:%02d\r\n"
                  "contents: the newest game logs (logs/), crash reports (crashes/) and svr2011.toml "
                  "(account token and password removed); IP addresses blanked (x.x.x.x)\r\n",
                  version, os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
        if (description && *description) {
            /* (the player's words first: what a reader looks for) */
            size_t dl = strlen(description), il = strlen(info);
            char *all = (char *)malloc(dl + il + 64);
            if (all) {
                sprintf_s(all, dl + il + 64, "%s\r\n\r\n%s", description, info);
                zip_add(&z, "report.txt", (const uint8_t *)all, strlen(all));
                free(all);
            } else {
                zip_add(&z, "report.txt", (const uint8_t *)info, il);
            }
        } else {
            zip_add(&z, "report.txt", (const uint8_t *)info, strlen(info));
        }
    }
    swprintf_s(sub, MAX_PATH, L"%s\\logs", game_dir);
    *logs = add_newest(&z, sub, L"svr2011_*.log", 3, "logs", 16u << 20);
    swprintf_s(sub, MAX_PATH, L"%s\\UserData\\crashes", game_dir);
    *crashes = add_newest(&z, sub, L"crash_*.txt", 3, "crashes", 1u << 20);
    add_settings(&z, game_dir);
    zip_finish(&z);
    fclose(z.f);
    return 1;
}
