/* WWE SmackDown vs. Raw 2011 PC launcher - the Supporters tab (supporters_tab.h).
 *
 * On top, the line "Supporting is optional and unlocks nothing - everything stays free. Thank you!" and the three
 * links (Patreon, Ko-fi, PayPal: each opens its page in the browser). Below, the names: GET <server>/api/supporters
 * on a thread when the tab opens (again after a minute, or on Refresh) - {"supporters":[{"name","platform","since"}]}
 * newest first, at most 500 (server/supporters.py: only names the supporters chose to show; never amounts). Offline,
 * or nobody yet: a friendly line instead. Read-only, no sign-in.
 *
 * (Android: not yet. The same endpoint would plug into the app's own page the way its My suggestions list reads
 * /api/reports/status - one GET, no token.) */
#include "supporters_tab.h"

#include <commctrl.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winhttp.h>

enum { S_THANKS, S_LINKS, S_PATREON, S_KOFI, S_PAYPAL, S_BOX, S_LIST, S_EMPTY, S_REFRESH, S_STATUS, S_COUNT };
enum { SUP_MAX = 500 };

#define SUP_THANKS L"Supporting is optional and unlocks nothing - everything stays free. Thank you!"
#define SUP_OFFLINE L"The list of supporters can't be loaded right now (offline, or the server is busy).\r\n" \
                    L"Thank you to everyone who supports the project!"
#define SUP_NOBODY L"No names to show yet. Supporters who choose to be listed show up here - thank you to everyone " \
                   L"who supports the project!"

typedef struct {
    WCHAR name[64], platform[16], since[16];
} Supporter;

static HWND s_wnd, s_c[S_COUNT];
static int s_base, s_busy, s_n, s_loaded;
static ULONGLONG s_asked_at;
static reports_server_fn s_server;
static int (*s_refuses)(const WCHAR *server, const char *what);
static WCHAR s_url[3][200];
static Supporter s_items[SUP_MAX];

/* The thread's answer (WM_APP_SUPPORTERS). */
typedef struct {
    int ok, n;
    DWORD status;
    WCHAR why[200];
    Supporter items[SUP_MAX];
} Job;

static void status(const WCHAR *t) { SetWindowTextW(s_c[S_STATUS], t); }

/* ── the request ─────────────────────────────────────────────────────────── */

/* GET <server><path>: 1 when an answer came (*status; *answer malloc'd, 0-ended). */
static int get(const WCHAR *server, const WCHAR *path, DWORD *status_out, char **answer)
{
    WCHAR url[1000], host[256], upath[900];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD sz = sizeof *status_out, got = 0;
    size_t total = 0, cap = 0;
    int ok = 0;
    *status_out = 0, *answer = NULL;
    swprintf_s(url, 1000, L"%s%s", wcsstr(server, L"://") ? L"" : L"http://", server);
    while (wcslen(url) && url[wcslen(url) - 1] == L'/') url[wcslen(url) - 1] = 0;
    wcscat_s(url, 1000, path);
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = upath; uc.dwUrlPathLength = 900;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    ses = WinHttpOpen(L"SvR2011 Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, 8000, 8000, 15000, 15000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
    }
    if (con)
        req = WinHttpOpenRequest(con, L"GET", upath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (req && WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            status_out, &sz, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            char *m;
            if (total + 4097 > cap) {
                cap = cap ? cap * 2 : 16384;
                if (cap > (2u << 20) || !(m = (char *)realloc(*answer, cap))) break;
                *answer = m;
            }
            if (!WinHttpReadData(req, *answer + total, 4096, &got) || !got) break;
            total += got;
        }
        if (*answer) (*answer)[total] = 0;
        ok = 1;
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return ok;
}

/* ── the answer: {"ok":true,"supporters":[{"name":"..","platform":"..","since":".."}, ...], ...} ── */

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }

static int hex4(const char *p, unsigned *v)
{
    int i;
    *v = 0;
    for (i = 0; i < 4; i++) {
        const char c = p[i];
        *v <<= 4;
        if (c >= '0' && c <= '9') *v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') *v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') *v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    return 1;
}

/* A JSON string at p (its opening quote) into out (UTF-16, cut to n); the text after it, or NULL. */
static const char *json_string(const char *p, WCHAR *out, size_t n)
{
    size_t k = 0;
    if (*p != '"') return NULL;
    for (p++; *p && *p != '"'; ) {
        unsigned cp;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case 'n': cp = '\n'; p++; break;
            case 't': cp = '\t'; p++; break;
            case 'r': cp = '\r'; p++; break;
            case 'b': cp = '\b'; p++; break;
            case 'f': cp = '\f'; p++; break;
            case 'u':
                if (!hex4(p + 1, &cp)) return NULL;
                p += 5;
                break;
            case 0: return NULL;
            default: cp = (unsigned char)*p++; break;
            }
            if (k + 1 < n) out[k++] = (WCHAR)cp;   /* (\u: a UTF-16 unit, surrogates as they come) */
            continue;
        }
        {   /* one UTF-8 sequence */
            const unsigned char c = (unsigned char)*p;
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0, i;
            cp = more == 3 ? c & 7 : more == 2 ? c & 15 : more == 1 ? c & 31 : c;
            p++;
            for (i = 0; i < more && (*p & 0xC0) == 0x80; i++) cp = (cp << 6) | (*p++ & 0x3F);
            if (cp >= 0x10000) {
                if (k + 2 < n) { out[k++] = (WCHAR)(0xD800 + ((cp - 0x10000) >> 10)); out[k++] = (WCHAR)(0xDC00 + (cp & 0x3FF)); }
            } else if (k + 1 < n) {
                out[k++] = (WCHAR)cp;
            }
        }
    }
    if (n) out[k < n ? k : n - 1] = 0;
    return *p == '"' ? p + 1 : NULL;
}

/* Any JSON value at p skipped (strings, numbers, true / false / null, nested objects and arrays). */
static const char *json_skip(const char *p)
{
    int depth = 0;
    for (;;) {
        p = skip_ws(p);
        if (!*p) return NULL;
        if (*p == '"') {   /* a string (its escapes skipped) */
            for (p++; *p && *p != '"'; p++)
                if (*p == '\\' && p[1]) p++;
            if (!*p) return NULL;
            p++;
        } else if (*p == '{' || *p == '[') {
            depth++, p++;
            continue;
        } else if (*p == '}' || *p == ']') {
            if (!depth) return p;   /* (the end of what holds it) */
            depth--, p++;
        } else if (*p == ',' || *p == ':') {
            if (!depth) return p;
            p++;
            continue;
        } else {   /* a number, true, false, null */
            while (*p && !strchr(",}] \t\r\n", *p)) p++;
        }
        if (!depth) return p;
    }
}

/* The "supporters" array's entries into items: how many (-1: not that answer). */
static int parse(const char *a, Supporter *items, int max)
{
    const char *p = strstr(a, "\"supporters\"");
    int n = 0;
    if (!p) return -1;
    p = skip_ws(p + 12);
    if (*p != ':') return -1;
    p = skip_ws(p + 1);
    if (*p != '[') return -1;
    p = skip_ws(p + 1);
    while (*p == '{') {
        Supporter s;
        ZeroMemory(&s, sizeof s);
        p = skip_ws(p + 1);
        while (*p == '"') {
            WCHAR key[32];
            if (!(p = json_string(p, key, 32))) return n;
            p = skip_ws(p);
            if (*p != ':') return n;
            p = skip_ws(p + 1);
            if (*p == '"') {
                WCHAR *to = !wcscmp(key, L"name") ? s.name : !wcscmp(key, L"platform") ? s.platform :
                            !wcscmp(key, L"since") ? s.since : NULL;
                WCHAR junk[8];
                p = to ? json_string(p, to, to == s.name ? 64 : 16) : json_string(p, junk, 8);
            } else {
                p = json_skip(p);
            }
            if (!p) return n;
            p = skip_ws(p);
            if (*p == ',') p = skip_ws(p + 1);
        }
        if (*p != '}') return n;
        p = skip_ws(p + 1);
        if (s.name[0] && n < max) items[n++] = s;
        if (*p == ',') p = skip_ws(p + 1);
    }
    return n;
}

static DWORD WINAPI ask_thread(void *arg)
{
    Job *j = (Job *)arg;
    WCHAR server[300];
    char *answer = NULL;
    s_server(server, 300);
    if (!server[0]) {
        wcscpy_s(j->why, 200, L"no online server is set");
    } else if (s_refuses && s_refuses(server, "/api/supporters")) {
        wcscpy_s(j->why, 200, L"test run: a server not on this PC was refused");
    } else if (!get(server, L"/api/supporters", &j->status, &answer) || !answer) {
        wcscpy_s(j->why, 200, L"no answer from the server");
    } else if (j->status != 200) {
        swprintf_s(j->why, 200, L"the server said HTTP %lu", j->status);
    } else if ((j->n = parse(answer, j->items, SUP_MAX)) < 0) {
        j->n = 0;
        wcscpy_s(j->why, 200, L"the server's answer wasn't the list");
    } else {
        j->ok = 1;
    }
    free(answer);
    PostMessageW(s_wnd, WM_APP_SUPPORTERS, 0, (LPARAM)j);
    return 0;
}

/* ── the page ────────────────────────────────────────────────────────────── */

static const WCHAR *platform_text(const WCHAR *p)
{
    if (!wcscmp(p, L"kofi")) return L"Ko-fi";
    if (!wcscmp(p, L"patreon")) return L"Patreon";
    if (!wcscmp(p, L"paypal")) return L"PayPal";
    return L"Other";
}

static void fill(int failed)
{
    int i;
    SendMessageW(s_c[S_LIST], WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(s_c[S_LIST]);
    for (i = 0; i < s_n; i++) {
        LVITEMW it;
        ZeroMemory(&it, sizeof it);
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = s_items[i].name;
        ListView_InsertItem(s_c[S_LIST], &it);
        ListView_SetItemText(s_c[S_LIST], i, 1, (WCHAR *)platform_text(s_items[i].platform));
        ListView_SetItemText(s_c[S_LIST], i, 2, s_items[i].since);
    }
    SendMessageW(s_c[S_LIST], WM_SETREDRAW, TRUE, 0);
    InvalidateRect(s_c[S_LIST], NULL, TRUE);
    ShowWindow(s_c[S_LIST], s_n ? SW_SHOW : SW_HIDE);
    SetWindowTextW(s_c[S_EMPTY], failed ? SUP_OFFLINE : s_loaded ? SUP_NOBODY : L"");
    ShowWindow(s_c[S_EMPTY], s_n ? SW_HIDE : SW_SHOW);
}

static void ask(void)
{
    Job *j;
    HANDLE h;
    if (s_busy) return;
    if (!(j = (Job *)calloc(1, sizeof *j))) return;
    s_busy = 1;
    s_asked_at = GetTickCount64();
    EnableWindow(s_c[S_REFRESH], FALSE);
    status(L"Loading the list\x2026");
    if ((h = CreateThread(NULL, 0, ask_thread, j, 0, NULL)) != NULL) {
        CloseHandle(h);
    } else {
        free(j);
        s_busy = 0;
        EnableWindow(s_c[S_REFRESH], TRUE);
        status(L"");
    }
}

void supporters_build(HWND wnd, mods_add_fn add, int tab, int id_base, reports_server_fn server,
                      int (*refuses)(const WCHAR *server, const char *what), const WCHAR *patreon,
                      const WCHAR *kofi, const WCHAR *paypal)
{
    static const WCHAR *const names[] = { L"Name", L"On", L"Since" };
    static const int widths[] = { 300, 120, 108 };   /* (the list is 532 wide) */
    HDC dc;
    int i, dpi;
#define C(i, cls, text, style, x, y, w, h) (s_c[i] = add(tab, cls, text, style, x, y, w, h, id_base + (i)))
    s_wnd = wnd;
    s_base = id_base;
    s_server = server;
    s_refuses = refuses;
    wcscpy_s(s_url[0], 200, patreon ? patreon : L"");
    wcscpy_s(s_url[1], 200, kofi ? kofi : L"");
    wcscpy_s(s_url[2], 200, paypal ? paypal : L"");
    dc = GetDC(wnd);
    dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(wnd, dc);
    C(S_THANKS, L"Static", SUP_THANKS, SS_LEFT | SS_NOPREFIX, 28, 10, 560, 36);
    C(S_LINKS, L"Button", L"Support the project (optional)", BS_GROUPBOX, 28, 50, 560, 74);
    C(S_PATREON, L"Button", L"Patreon", BS_PUSHBUTTON | WS_TABSTOP, 42, 78, 160, 32);
    C(S_KOFI, L"Button", L"Ko-fi", BS_PUSHBUTTON | WS_TABSTOP, 216, 78, 160, 32);
    C(S_PAYPAL, L"Button", L"PayPal", BS_PUSHBUTTON | WS_TABSTOP, 390, 78, 160, 32);
    C(S_BOX, L"Button", L"Thank you to", BS_GROUPBOX, 28, 132, 560, 392);
    C(S_LIST, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER | WS_BORDER | WS_TABSTOP, 42, 158,
      532, 352);
    ListView_SetExtendedListViewStyle(s_c[S_LIST], LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    for (i = 0; i < 3; i++) {
        LVCOLUMNW c;
        ZeroMemory(&c, sizeof c);
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        c.fmt = LVCFMT_LEFT;
        c.cx = MulDiv(widths[i], dpi, 96);
        c.pszText = (WCHAR *)names[i];
        ListView_InsertColumn(s_c[S_LIST], i, &c);
    }
    C(S_EMPTY, L"Static", L"", SS_CENTER | SS_NOPREFIX, 60, 300, 496, 60);
    C(S_REFRESH, L"Button", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, 28, 536, 90, 28);
    C(S_STATUS, L"Static", L"", SS_LEFT | SS_NOPREFIX, 128, 540, 460, 24);
#undef C
    fill(0);
}

void supporters_show(void)
{
    /* (once when it opens; again after a minute - the server caches it that long too) */
    if (!s_busy && (!s_loaded || GetTickCount64() - s_asked_at > 60000))
        ask();
    else
        fill(0);
}

int supporters_command(int id, int code)
{
    const int k = id - s_base;
    if (k < 0 || k >= S_COUNT) return 0;
    if (code != BN_CLICKED) return 1;
    if (k == S_REFRESH) ask();
    else if (k >= S_PATREON && k <= S_PAYPAL && s_url[k - S_PATREON][0])
        ShellExecuteW(s_wnd, L"open", s_url[k - S_PATREON], NULL, NULL, SW_SHOWNORMAL);
    return 1;
}

int supporters_message(UINT m, WPARAM wp, LPARAM lp)
{
    Job *j = (Job *)lp;
    (void)wp;
    if (m != WM_APP_SUPPORTERS) return 0;
    s_busy = 0;
    EnableWindow(s_c[S_REFRESH], TRUE);
    if (j->ok) {
        WCHAR t[120];
        memcpy(s_items, j->items, sizeof s_items);
        s_n = j->n;
        s_loaded = 1;
        if (s_n) swprintf_s(t, 120, L"%d supporter%s - thank you!", s_n, s_n == 1 ? L"" : L"s");
        else t[0] = 0;
        status(t);
        fill(0);
    } else {
        WCHAR t[260];
        swprintf_s(t, 260, L"Couldn't load the list: %s.", j->why);
        status(t);
        fill(!s_n);   /* (a list from before stays) */
    }
    free(j);
    return 1;
}

int supporters_test_state(int *n, WCHAR *line, size_t line_n)
{
    if (n) *n = s_n;
    if (line && line_n) GetWindowTextW(s_c[S_STATUS], line, (int)line_n);
    return s_busy;
}

void supporters_test_dump(FILE *f)
{
    int i;
    WCHAR empty[300];
    GetWindowTextW(s_c[S_EMPTY], empty, 300);
    if (!s_n) fwprintf(f, L"empty line: %s\n", empty);
    for (i = 0; i < s_n; i++)
        fwprintf(f, L"%s  [%s]  %s\n", s_items[i].name, platform_text(s_items[i].platform), s_items[i].since);
}
