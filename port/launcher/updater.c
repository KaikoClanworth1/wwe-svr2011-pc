/* WWE SmackDown vs. Raw 2011 - PC port launcher: updates.
 *
 * The newest release comes from the GitHub API
 * (api.github.com/repos/<UPDATE_REPO>/releases, newest first, pre-releases
 * included). Its SvR2011-PC-v*.zip is downloaded only from this repository's
 * own release downloads; the launcher unpacks it and copies the program files
 * over the install (svr2011_launcher.c, update_apply). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "updater.h"

#pragma comment(lib, "winhttp.lib")

#define AGENT L"SvR2011-PC-Launcher"

typedef struct { HINTERNET session, connect, request; } Http;

static void http_close(Http *h)
{
    if (h->request) WinHttpCloseHandle(h->request);
    if (h->connect) WinHttpCloseHandle(h->connect);
    if (h->session) WinHttpCloseHandle(h->session);
    memset(h, 0, sizeof *h);
}

/* GET https://<host><path>; follows redirects. 1 when the answer is 200. */
static int http_get(Http *h, const WCHAR *url, const WCHAR *accept, WCHAR *err, size_t errn)
{
    URL_COMPONENTS uc;
    WCHAR host[256], path[2048];
    DWORD status = 0, len = sizeof status;
    memset(h, 0, sizeof *h);
    memset(&uc, 0, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        swprintf_s(err, errn, L"Bad address: %s", url);
        return 0;
    }
    h->session = WinHttpOpen(AGENT, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                             WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session)
        h->session = WinHttpOpen(AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
    if (!h->session
            || !(h->connect = WinHttpConnect(h->session, host, uc.nPort, 0))
            || !(h->request = WinHttpOpenRequest(h->connect, L"GET", path, NULL, WINHTTP_NO_REFERER,
                                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE))) {
        swprintf_s(err, errn, L"Could not reach %s (error %lu).", host, GetLastError());
        http_close(h);
        return 0;
    }
    WinHttpSetTimeouts(h->request, 10000, 10000, 15000, 30000);
    if (accept)
        WinHttpAddRequestHeaders(h->request, accept, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    if (!WinHttpSendRequest(h->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(h->request, NULL)
            || !WinHttpQueryHeaders(h->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX)) {
        swprintf_s(err, errn, L"Could not reach GitHub (error %lu). Check your internet connection.", GetLastError());
        http_close(h);
        return 0;
    }
    if (status != 200) {
        swprintf_s(err, errn, status == 403 ? L"GitHub is limiting requests right now (HTTP 403); try again later."
                                            : L"GitHub answered HTTP %lu.", status);
        http_close(h);
        return 0;
    }
    return 1;
}

/* The whole answer, NUL-terminated (up to `max` bytes). */
static char *http_read_all(Http *h, size_t max, size_t *out_n)
{
    size_t n = 0, cap = 65536;
    char *buf = (char *)malloc(cap + 1);
    DWORD got;
    if (!buf) return NULL;
    for (;;) {
        if (n == cap) {
            char *p;
            if (cap >= max) break;
            cap *= 2;
            p = (char *)realloc(buf, cap + 1);
            if (!p) { free(buf); return NULL; }
            buf = p;
        }
        got = 0;
        if (!WinHttpReadData(h->request, buf + n, (DWORD)(cap - n), &got) || !got) break;
        n += got;
    }
    buf[n] = 0;
    if (out_n) *out_n = n;
    return buf;
}

/* The string value of "key" at or after p, before `end`; copied to out. */
static const char *json_string(const char *p, const char *end, const char *key, char *out, size_t outn)
{
    char pat[64];
    const char *k, *s;
    size_t n = 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    k = strstr(p, pat);
    if (!k || k >= end) return NULL;
    s = k + strlen(pat);
    while (s < end && (*s == ' ' || *s == ':')) s++;
    if (s >= end || *s != '"') return NULL;
    s++;
    while (s < end && *s != '"' && n + 1 < outn) {
        if (*s == '\\' && s + 1 < end) s++;
        out[n++] = *s++;
    }
    out[n] = 0;
    return s;
}

static int json_bool(const char *p, const char *end, const char *key)
{
    char pat[64];
    const char *k;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    k = strstr(p, pat);
    if (!k || k >= end) return 0;
    k += strlen(pat);
    while (k < end && (*k == ' ' || *k == ':')) k++;
    return !strncmp(k, "true", 4);
}

int version_newer(const WCHAR *a, const WCHAR *b)
{
    while (*a || *b) {
        long x = wcstol(a, (WCHAR **)&a, 10), y = wcstol(b, (WCHAR **)&b, 10);
        if (x != y) return x > y;
        if (*a == L'.') a++;
        if (*b == L'.') b++;
        if (*a && !iswdigit(*a)) break;
        if (*b && !iswdigit(*b)) break;
    }
    return 0;
}

int update_check(UpdateInfo *out, WCHAR *err, size_t errn)
{
    Http h;
    char *json, *rel, *end, tag[64], url[1024], page[512], name[256];
    const char *a;
    size_t n = 0;
    int ok = 0;
    char prefix[256];
    memset(out, 0, sizeof *out);
    if (!http_get(&h, L"https://api.github.com/repos/" UPDATE_REPO L"/releases?per_page=10",
                  L"Accept: application/vnd.github+json", err, errn))
        return 0;
    json = http_read_all(&h, 4u << 20, &n);
    http_close(&h);
    if (!json) { swprintf_s(err, errn, L"Out of memory."); return 0; }
    /* Releases come newest first; drafts are not listed. Each release object
       starts at its "url" of .../releases/<id> - split on "tag_name". */
    rel = strstr(json, "\"tag_name\"");
    if (!rel) { swprintf_s(err, errn, L"No releases found."); goto done; }
    end = strstr(rel + 10, "\"tag_name\"");
    if (!end) end = json + n;
    if (!json_string(rel, end, "tag_name", tag, sizeof tag)) goto bad;
    json_string(json, end, "html_url", page, sizeof page);
    out->prerelease = json_bool(json, end, "prerelease");
    /* its zip: the first asset named SvR2011-PC-*.zip */
    snprintf(prefix, sizeof prefix, "https://github.com/%ls/releases/download/", UPDATE_REPO);
    for (a = rel; (a = json_string(a, end, "name", name, sizeof name)) != NULL;) {
        if (!strncmp(name, "SvR2011-PC-", 11) && strlen(name) > 4 && !_stricmp(name + strlen(name) - 4, ".zip")) {
            const char *u = json_string(a, end, "browser_download_url", url, sizeof url);
            const char *sz = strstr(a, "\"size\"");
            if (!u || strncmp(url, prefix, strlen(prefix))) break;   /* only this repository's downloads */
            if (sz && sz < end) out->size = _strtoui64(sz + 7, NULL, 10);
            MultiByteToWideChar(CP_UTF8, 0, url, -1, out->url, 1024);
            break;
        }
    }
    if (!out->url[0]) { swprintf_s(err, errn, L"The newest release (%hs) has no download for Windows.", tag); goto done; }
    MultiByteToWideChar(CP_UTF8, 0, tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag, -1, out->version, 32);
    MultiByteToWideChar(CP_UTF8, 0, page, -1, out->page, 512);
    ok = 1;
    goto done;
bad:
    swprintf_s(err, errn, L"Could not read GitHub's answer.");
done:
    free(json);
    return ok;
}

int update_download(const WCHAR *url, const WCHAR *path, HWND notify, UINT msg, volatile LONG *cancel,
                    WCHAR *err, size_t errn)
{
    Http h;
    HANDLE f;
    WCHAR len_text[32];
    DWORD len_size = sizeof len_text, got, wrote;
    uint64_t total = 0, done = 0;
    char *buf;
    int ok = 0, last = -1;
    if (!http_get(&h, url, L"Accept: application/octet-stream", err, errn))
        return 0;
    if (WinHttpQueryHeaders(h.request, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, len_text,
                            &len_size, WINHTTP_NO_HEADER_INDEX))
        total = _wcstoui64(len_text, NULL, 10);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    buf = (char *)malloc(1 << 20);
    if (f == INVALID_HANDLE_VALUE || !buf) {
        swprintf_s(err, errn, L"Could not create %s.", path);
        goto out;
    }
    for (;;) {
        if (cancel && *cancel) { swprintf_s(err, errn, L"Stopped."); goto out; }
        got = 0;
        if (!WinHttpReadData(h.request, buf, 1 << 20, &got)) {
            swprintf_s(err, errn, L"The download was interrupted (error %lu).", GetLastError());
            goto out;
        }
        if (!got) break;
        if (!WriteFile(f, buf, got, &wrote, NULL) || wrote != got) {
            swprintf_s(err, errn, L"Could not write the download (disk full?).");
            goto out;
        }
        done += got;
        if (notify && total) {
            int pct = (int)(done * 100 / total);
            if (pct != last) { last = pct; PostMessageW(notify, msg, (WPARAM)pct, 0); }
        }
    }
    if (total && done != total) { swprintf_s(err, errn, L"The download was incomplete."); goto out; }
    ok = 1;
out:
    free(buf);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) DeleteFileW(path);
    http_close(&h);
    return ok;
}
