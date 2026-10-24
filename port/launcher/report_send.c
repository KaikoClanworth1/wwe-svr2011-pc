/* WWE SmackDown vs. Raw 2011 - "Report a problem" sent to the online server (report_send.h). */

#include "report_send.h"

#include <winhttp.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ── a growing buffer ─────────────────────────────────────────────────────── */

typedef struct {
    char *p;
    size_t n, cap;
    int bad;
} Buf;

static void buf_add(Buf *b, const void *data, size_t n)
{
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        char *m;
        while (cap < b->n + n + 1) cap *= 2;
        if (!(m = (char *)realloc(b->p, cap))) { b->bad = 1; return; }
        b->p = m, b->cap = cap;
    }
    memcpy(b->p + b->n, data, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

/* ── report.json ──────────────────────────────────────────────────────────── */

static void json_str(Buf *b, const char *key, const char *v, int first)
{
    const unsigned char *p = (const unsigned char *)(v ? v : "");
    char esc[8];
    if (!first) buf_str(b, ",");
    buf_str(b, "\"");
    buf_str(b, key);
    buf_str(b, "\":\"");
    for (; *p; p++) {
        if (*p == '"' || *p == '\\') { esc[0] = '\\', esc[1] = (char)*p; buf_add(b, esc, 2); }
        else if (*p == '\n') buf_str(b, "\\n");
        else if (*p == '\r') buf_str(b, "\\r");
        else if (*p == '\t') buf_str(b, "\\t");
        else if (*p < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", *p); buf_str(b, esc); }
        else buf_add(b, p, 1);
    }
    buf_str(b, "\"");
}

char *report_json(const char *app, const char *version, const char *problem, const char *happens, const char *what,
                  const char *before, const char *contact, const char *signature)
{
    Buf b = { 0 };
    char os[64];
    OSVERSIONINFOEXW vi;
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOEXW *);
    RtlGetVersionFn get = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    ZeroMemory(&vi, sizeof vi);
    vi.dwOSVersionInfoSize = sizeof vi;
    if (get && get(&vi) == 0)
        snprintf(os, sizeof os, "%lu.%lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
    else
        strcpy(os, "?");
    buf_str(&b, "{\"format\":1,");
    json_str(&b, "app", app, 1);
    json_str(&b, "version", version, 0);
    json_str(&b, "platform", "windows", 0);
    json_str(&b, "os", os, 0);
    json_str(&b, "problem", problem, 0);
    json_str(&b, "happens", happens, 0);
    json_str(&b, "what", what, 0);
    json_str(&b, "before", before, 0);
    json_str(&b, "contact", contact, 0);
    json_str(&b, "signature", signature, 0);
    buf_str(&b, "}");
    if (b.bad) { free(b.p); return NULL; }
    return b.p;
}

/* ── the signature hint ───────────────────────────────────────────────────── */

static uint8_t *read_all(const WCHAR *path, size_t *n, size_t max)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    uint8_t *d = NULL;
    DWORD got = 0;
    *n = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart <= (LONGLONG)max && (d = (uint8_t *)malloc((size_t)sz.QuadPart + 1)) &&
        ReadFile(h, d, (DWORD)sz.QuadPart, &got, NULL) && got == (DWORD)sz.QuadPart) {
        d[got] = 0;
        *n = got;
    } else {
        free(d);
        d = NULL;
    }
    CloseHandle(h);
    return d;
}

/* The newest file in dir matching pattern (its full path in out); 0 if none. */
static int newest(const WCHAR *dir, const WCHAR *pattern, WCHAR *out, int n)
{
    WCHAR glob[MAX_PATH];
    WIN32_FIND_DATAW fd;
    FILETIME best = { 0 };
    HANDLE f;
    int found = 0;
    swprintf_s(glob, MAX_PATH, L"%s\\%s", dir, pattern);
    if ((f = FindFirstFileW(glob, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!found || CompareFileTime(&fd.ftLastWriteTime, &best) > 0) {
            best = fd.ftLastWriteTime;
            swprintf_s(out, n, L"%s\\%s", dir, fd.cFileName);
            found = 1;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return found;
}

static void copy_line(const char *from, const char *end, char *out, int n)
{
    int k = 0;
    while (from < end && (*from == ' ' || *from == '\t')) from++;
    while (from < end && *from != '\r' && *from != '\n' && k + 1 < n) out[k++] = *from++;
    out[k] = 0;
}

void report_signature(const WCHAR *game_dir, char *out, int n)
{
    WCHAR dir[MAX_PATH], file[MAX_PATH];
    uint8_t *d;
    size_t len;
    out[0] = 0;
    swprintf_s(dir, MAX_PATH, L"%s\\crashes", game_dir);
    if (newest(dir, L"crash_*.txt", file, MAX_PATH) && (d = read_all(file, &len, 1u << 20)) != NULL) {
        copy_line((const char *)d, (const char *)d + len, out, n);  /* ("SvR 2011 crash: <reason> (thread ...)") */
        free(d);
        if (out[0]) return;
    }
    swprintf_s(dir, MAX_PATH, L"%s\\logs", game_dir);
    if (newest(dir, L"svr2011_*.log", file, MAX_PATH) && (d = read_all(file, &len, 64u << 20)) != NULL) {
        const char *p = (const char *)d, *end = p + len, *hit = NULL;
        for (; p < end; p++)  /* (the last "[error]" line) */
            if (*p == '[' && end - p > 7 && !memcmp(p, "[error]", 7)) hit = p;
        if (hit) {
            while (hit < end && *hit != ']') hit++;      /* (after the level: "[core] [tNNN] text") */
            copy_line(hit + 1, end, out, n);
        }
        free(d);
    }
}

/* ── pictures ─────────────────────────────────────────────────────────────── */

static const char *image_type(const uint8_t *d, size_t n)
{
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return "image/png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "image/jpeg";
    if (n >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4)) return "image/webp";
    return NULL;
}

int report_image_ok(const WCHAR *path, WCHAR *why, int why_n)
{
    size_t n;
    uint8_t *d = read_all(path, &n, 5u << 20);
    const WCHAR *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    if (!d) {
        swprintf_s(why, why_n, L"%s can't be added: it can't be read, or it's over 5 MB.", name);
        return 0;
    }
    if (!image_type(d, n)) {
        free(d);
        swprintf_s(why, why_n, L"%s isn't a PNG, JPEG or WebP picture.", name);
        return 0;
    }
    free(d);
    return 1;
}

/* ── sending ──────────────────────────────────────────────────────────────── */

void report_send_summary(const ReportUpload *u, WCHAR *out, int n)
{
    swprintf_s(out, n, L"Sends: your answers%s%s, %d picture%s - no password, token or internet addresses.",
               u->zip ? L", the report zip (newest logs, crash reports, settings" : L"",
               u->zip ? L")" : L"", u->n_images, u->n_images == 1 ? L"" : L"s");
}

static void part_head(Buf *b, const char *bound, const char *name, const char *file, const char *ctype)
{
    char h[400];
    snprintf(h, sizeof h, "--%s\r\nContent-Disposition: form-data; name=\"%s\"%s%s%s\r\nContent-Type: %s\r\n\r\n",
             bound, name, file ? "; filename=\"" : "", file ? file : "", file ? "\"" : "", ctype);
    buf_str(b, h);
}

/* A JSON answer's string or number value for "key" (a flat object: no nesting needed here). */
static int json_get(const char *j, const char *key, char *out, int n)
{
    char pat[64];
    const char *p;
    int k = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    if (!j || !(p = strstr(j, pat))) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p == '"') {
        for (p++; *p && *p != '"' && k + 1 < n; p++) {
            if (*p == '\\' && p[1]) p++;
            out[k++] = *p;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ' ' && k + 1 < n) out[k++] = *p++;
    }
    out[k] = 0;
    return 1;
}

int report_send(const WCHAR *server, const ReportUpload *u, ReportResult *r)
{
    static const char bound[] = "----svr2011report5d2e81";
    Buf body = { 0 };
    WCHAR url[1000], host[256], upath[900], headers[200];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, sz = sizeof status, got = 0;
    char *answer = NULL, value[300];
    size_t total = 0, cap = 0;
    int i, ok = 0;
    ZeroMemory(r, sizeof *r);
    if (!server || !server[0] || !u->json) {
        wcscpy_s(r->why, 300, L"No server is set (online_server).");
        return 0;
    }
    /* the body: report, zip, pictures */
    part_head(&body, bound, "report", NULL, "application/json; charset=utf-8");
    buf_str(&body, u->json);
    buf_str(&body, "\r\n");
    if (u->zip) {
        size_t n;
        uint8_t *d = read_all(u->zip, &n, 16u << 20);
        if (!d) {
            wcscpy_s(r->why, 300, L"The report zip can't be read, or it's over 16 MB.");
            free(body.p);
            return 0;
        }
        part_head(&body, bound, "zip", "report.zip", "application/zip");
        buf_add(&body, d, n);
        buf_str(&body, "\r\n");
        free(d);
    }
    for (i = 0; i < u->n_images && i < REPORT_MAX_IMAGES; i++) {
        size_t n;
        uint8_t *d = u->images[i] ? read_all(u->images[i], &n, 5u << 20) : NULL;
        const char *type = d ? image_type(d, n) : NULL;
        char name[16], file[24];
        if (!type) {
            free(d);
            report_image_ok(u->images[i] ? u->images[i] : L"?", r->why, 300);
            free(body.p);
            return 0;
        }
        snprintf(name, sizeof name, "image%d", i);
        snprintf(file, sizeof file, "image%d.%s", i, type + 6);
        part_head(&body, bound, name, file, type);
        buf_add(&body, d, n);
        buf_str(&body, "\r\n");
        free(d);
    }
    buf_str(&body, "--");
    buf_str(&body, bound);
    buf_str(&body, "--\r\n");
    if (body.bad) {
        wcscpy_s(r->why, 300, L"Not enough memory to send the report.");
        free(body.p);
        return 0;
    }
    /* the request */
    swprintf_s(url, 1000, L"%s%s", wcsstr(server, L"://") ? L"" : L"http://", server);  /* (a custom server: host:port) */
    while (wcslen(url) && url[wcslen(url) - 1] == L'/') url[wcslen(url) - 1] = 0;
    wcscat_s(url, 1000, L"/api/reports");
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = upath; uc.dwUrlPathLength = 900;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        swprintf_s(r->why, 300, L"The server address isn't a web address: %s", server);
        free(body.p);
        return 0;
    }
    ses = WinHttpOpen(L"SvR2011 Report", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, 10000, 15000, 120000, 60000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
    }
    if (con)
        req = WinHttpOpenRequest(con, L"POST", upath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    swprintf_s(headers, 200, L"Content-Type: multipart/form-data; boundary=%S\r\n", bound);
    if (req && WinHttpSendRequest(req, headers, (DWORD)-1L, body.p, (DWORD)body.n, (DWORD)body.n, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &sz, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            char *m;
            if (total + 4097 > cap) {
                cap = cap ? cap * 2 : 8192;
                if (cap > (1u << 20) || !(m = (char *)realloc(answer, cap))) break;
                answer = m;
            }
            if (!WinHttpReadData(req, answer + total, 4096, &got) || !got) break;
            total += got;
        }
        if (answer) answer[total] = 0;
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(body.p);
    r->status = (int)status;
    if (status == 200 && json_get(answer, "id", r->id, sizeof r->id) && r->id[0]) {
        if (json_get(answer, "seen_before", value, sizeof value)) r->seen_before = atoi(value);
        ok = 1;
    } else if (!status) {
        wcscpy_s(r->why, 300, L"The server can't be reached (offline, or it's down).");
    } else if (json_get(answer, "error", value, sizeof value) && value[0]) {
        WCHAR w[260];
        MultiByteToWideChar(CP_UTF8, 0, value, -1, w, 260);
        swprintf_s(r->why, 300, L"The server refused it (%lu): %s", status, w);
    } else {
        swprintf_s(r->why, 300, L"The server refused it (HTTP %lu).", status);
    }
    free(answer);
    return ok;
}
