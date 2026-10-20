/* WWE SmackDown vs. Raw 2011 PC launcher - Mods -> Discovery (see discover_tab.h).
 *
 * Three views on one page: the grid (search by words, type and order; 24 a page, each with its first picture),
 * a mod's page (its four pictures, what it is, Download and install; Delete for its uploader) and the upload
 * form (name, version, types, tags, description, the .svrmod, four pictures - made 1024 x 768 JPEG here:
 * the server has no image library and only checks them). Every request runs on a thread of its own and
 * answers with WM_APP_DISCOVER; a page's late answers (an older search) are dropped by their number. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include "discover_tab.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <winhttp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "windowscodecs.lib")

enum {
    D_SEARCH, D_TYPE, D_SORT, D_GO, D_GRID, D_PREV, D_PAGE, D_NEXT, D_UPLOAD, D_STATUS,
    D_PIC, D_PICPREV, D_PICLBL, D_PICNEXT, D_INFO, D_DESC, D_BACK, D_DOWNLOAD, D_DELETE,
    U_TITLE, U_NAME_L, U_NAME, U_VER_L, U_VER, U_TYPES_L, U_T0, U_T1, U_T2, U_T3, U_TAGS_L, U_TAGS, U_DESC_L, U_DESC,
    U_FILE_L, U_FILE, U_FILE_PICK, U_PICS_L, U_PICS, U_PICS_PICK, U_AUTHOR, U_SEND, U_CANCEL, U_PROGRESS, D_COUNT
};
enum { V_GRID, V_MOD, V_UPLOAD };
enum { R_LIST, R_THUMB, R_DETAIL, R_PIC, R_DOWNLOAD, R_UPLOAD, R_DELETE, R_PROGRESS };
#define PER_PAGE 24
#define THUMB_W 160
#define THUMB_H 120
#define PIC_W 400
#define PIC_H 300

typedef struct {
    int id, downloads;
    long long size;
    WCHAR name[64], author[48], types[80], tags[220], version[24];
} Mod;

static HWND s_wnd, s_c[D_COUNT];
static int s_base, s_view = V_GRID, s_page, s_total, s_n, s_gen, s_shown, s_busy, s_pic, s_cur = -1;
static Mod s_mods[PER_PAGE];
static HIMAGELIST s_thumbs;
static HBITMAP s_pics[4];
static WCHAR s_game[MAX_PATH], s_file_name[MAX_PATH], s_up_file[MAX_PATH], s_up_pics[4][MAX_PATH];
static discover_server_fn s_server;
static discover_account_fn s_account;

static const WCHAR *const k_types[4] = { L"entrance", L"superstar", L"arena", L"moveset" };
static const WCHAR *const k_type_names[4] = { L"Entrance", L"Superstar", L"Arena", L"Move-set" };

/* ── small helpers ─────────────────────────────────────────────────────── */

static void status(const WCHAR *t) { SetWindowTextW(s_c[D_STATUS], t); }

static void fmt_bytes(WCHAR *out, size_t n, long long b)
{
    if (b >= (1ll << 30)) swprintf_s(out, n, L"%.2f GB", b / 1073741824.0);
    else if (b >= (1 << 20)) swprintf_s(out, n, L"%.1f MB", b / 1048576.0);
    else swprintf_s(out, n, L"%lld KB", (b + 1023) / 1024);
}

/* A JSON value's start for "key" inside [p, end), or NULL. */
static const char *jkey(const char *p, const char *end, const char *key)
{
    char k[48];
    size_t kl;
    sprintf_s(k, sizeof k, "\"%s\"", key);
    kl = strlen(k);
    for (; p && p + kl < end; p++)
        if (!memcmp(p, k, kl)) {
            p += kl;
            while (p < end && (*p == ' ' || *p == ':')) p++;
            return p;
        }
    return NULL;
}

/* A JSON string at p (its quote) as UTF-16 (\" \\ \n \uXXXX); the char after it. */
static const char *jstr(const char *p, const char *end, WCHAR *out, size_t n)
{
    char u[4096];
    size_t k = 0;
    out[0] = 0;
    if (!p || p >= end || *p != '"') return p;
    for (p++; p < end && *p != '"' && k + 4 < sizeof u; p++) {
        if (*p == '\\' && p + 1 < end) {
            p++;
            if (*p == 'n') u[k++] = '\n';
            else if (*p == 'u' && p + 4 < end) {
                unsigned c = 0;
                sscanf_s(p + 1, "%4x", &c);
                p += 4;
                if (c < 0x80) u[k++] = (char)c;
                else if (c < 0x800) { u[k++] = (char)(0xC0 | c >> 6); u[k++] = (char)(0x80 | (c & 63)); }
                else { u[k++] = (char)(0xE0 | c >> 12); u[k++] = (char)(0x80 | (c >> 6 & 63)); u[k++] = (char)(0x80 | (c & 63)); }
            } else u[k++] = *p;
        } else u[k++] = *p;
    }
    u[k] = 0;
    MultiByteToWideChar(CP_UTF8, 0, u, -1, out, (int)n);
    return p < end ? p + 1 : p;
}

static long long jnum(const char *p) { return p ? _atoi64(p) : 0; }

/* A JSON array of strings at p, joined with ", ". */
static void jlist(const char *p, const char *end, const WCHAR *const *names, WCHAR *out, size_t n)
{
    WCHAR one[64];
    out[0] = 0;
    if (!p || *p != '[') return;
    for (p++; p < end && *p != ']';) {
        if (*p == '"') {
            int i;
            p = jstr(p, end, one, 64);
            if (names)
                for (i = 0; i < 4; i++)
                    if (!wcscmp(one, k_types[i])) wcscpy_s(one, 64, names[i]);
            if (out[0]) wcscat_s(out, n, L", ");
            wcscat_s(out, n, one);
        } else p++;
    }
}

/* The end of the {...} starting at p (strings skipped). */
static const char *jobject_end(const char *p, const char *end)
{
    int depth = 0, in_str = 0;
    for (; p < end; p++) {
        if (in_str) { if (*p == '\\') p++; else if (*p == '"') in_str = 0; continue; }
        if (*p == '"') in_str = 1;
        else if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p + 1;
    }
    return end;
}

static void parse_mod(const char *p, const char *end, Mod *m)
{
    ZeroMemory(m, sizeof *m);
    m->id = (int)jnum(jkey(p, end, "id"));
    jstr(jkey(p, end, "name"), end, m->name, 64);
    jstr(jkey(p, end, "author"), end, m->author, 48);
    jstr(jkey(p, end, "version"), end, m->version, 24);
    jlist(jkey(p, end, "types"), end, k_type_names, m->types, 80);
    jlist(jkey(p, end, "tags"), end, NULL, m->tags, 220);
    m->size = jnum(jkey(p, end, "size"));
    m->downloads = (int)jnum(jkey(p, end, "downloads"));
}

/* ── pictures (WIC) ────────────────────────────────────────────────────── */

static IWICImagingFactory *wic(void)
{
    IWICImagingFactory *f = NULL;
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    return f;
}

/* A frame cut to 4:3 (its middle) and scaled to w x h, 32-bit BGRA. */
static IWICBitmapSource *fit_43(IWICImagingFactory *f, IWICBitmapFrameDecode *frame, int w, int h)
{
    UINT sw = 0, sh = 0;
    IWICBitmapClipper *clip = NULL;
    IWICBitmapScaler *scale = NULL;
    IWICFormatConverter *conv = NULL;
    IWICBitmapSource *src = (IWICBitmapSource *)frame;
    WICRect r;
    IWICBitmapFrameDecode_GetSize(frame, &sw, &sh);
    if (!sw || !sh) return NULL;
    r.X = 0; r.Y = 0; r.Width = (INT)sw; r.Height = (INT)sh;
    if ((double)sw / sh > 4.0 / 3.0) { r.Width = (INT)(sh * 4 / 3); r.X = ((INT)sw - r.Width) / 2; }
    else { r.Height = (INT)(sw * 3 / 4); r.Y = ((INT)sh - r.Height) / 2; }
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapClipper(f, &clip)) &&
        SUCCEEDED(IWICBitmapClipper_Initialize(clip, src, &r)))
        src = (IWICBitmapSource *)clip;
    if (FAILED(IWICImagingFactory_CreateBitmapScaler(f, &scale)) ||
        FAILED(IWICBitmapScaler_Initialize(scale, src, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)) ||
        FAILED(IWICImagingFactory_CreateFormatConverter(f, &conv)) ||
        FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)scale, &GUID_WICPixelFormat32bppBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) {
        if (conv) IWICFormatConverter_Release(conv);
        conv = NULL;
    }
    if (scale) IWICBitmapScaler_Release(scale);
    if (clip) IWICBitmapClipper_Release(clip);
    return (IWICBitmapSource *)conv;
}

/* JPEG / PNG bytes as a w x h bitmap (cut to 4:3), or NULL. */
static HBITMAP picture_bitmap(const uint8_t *data, size_t n, int w, int h)
{
    IWICImagingFactory *f = wic();
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *src = NULL;
    HBITMAP bmp = NULL;
    if (!f) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(f, &stream)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, (DWORD)n)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand,
                                                              &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) && (src = fit_43(f, frame, w, h))) {
        BITMAPINFO bi;
        void *bits = NULL;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp && FAILED(IWICBitmapSource_CopyPixels(src, NULL, (UINT)w * 4, (UINT)(w * h * 4), (BYTE *)bits))) {
            DeleteObject(bmp);
            bmp = NULL;
        }
        IWICBitmapSource_Release(src);
    }
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (stream) IWICStream_Release(stream);
    IWICImagingFactory_Release(f);
    return bmp;
}

/* A picture file as the 1024 x 768 JPEG the server takes (malloc'd), or NULL. */
static uint8_t *picture_jpeg(const WCHAR *path, size_t *n)
{
    IWICImagingFactory *f = wic();
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *src = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *out = NULL;
    IPropertyBag2 *props = NULL;
    IStream *mem = NULL;
    uint8_t *bytes = NULL;
    *n = 0;
    if (!f) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ,
                                                                WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) && (src = fit_43(f, frame, 1024, 768)) &&
        SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &mem)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatJpeg, NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, mem, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &out, &props))) {
        PROPBAG2 opt;
        VARIANT v;
        WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
        IWICFormatConverter *rgb = NULL;
        ZeroMemory(&opt, sizeof opt);
        opt.pstrName = L"ImageQuality";
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = 0.85f;
        IPropertyBag2_Write(props, 1, &opt, &v);
        if (SUCCEEDED(IWICBitmapFrameEncode_Initialize(out, props)) &&
            SUCCEEDED(IWICBitmapFrameEncode_SetSize(out, 1024, 768)) &&
            SUCCEEDED(IWICBitmapFrameEncode_SetPixelFormat(out, &fmt)) &&
            SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &rgb)) &&
            SUCCEEDED(IWICFormatConverter_Initialize(rgb, src, &fmt, WICBitmapDitherTypeNone, NULL, 0,
                                                     WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(IWICBitmapFrameEncode_WriteSource(out, (IWICBitmapSource *)rgb, NULL)) &&
            SUCCEEDED(IWICBitmapFrameEncode_Commit(out)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc))) {
            HGLOBAL g = NULL;
            if (SUCCEEDED(GetHGlobalFromStream(mem, &g))) {
                STATSTG st;
                if (SUCCEEDED(IStream_Stat(mem, &st, STATFLAG_NONAME)) && (bytes = (uint8_t *)malloc((size_t)st.cbSize.QuadPart))) {
                    memcpy(bytes, GlobalLock(g), (size_t)st.cbSize.QuadPart);
                    GlobalUnlock(g);
                    *n = (size_t)st.cbSize.QuadPart;
                }
            }
        }
        if (rgb) IWICFormatConverter_Release(rgb);
    }
    if (props) IPropertyBag2_Release(props);
    if (out) IWICBitmapFrameEncode_Release(out);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (mem) IStream_Release(mem);
    if (src) IWICBitmapSource_Release(src);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    IWICImagingFactory_Release(f);
    return bytes;
}

/* ── requests (threads) ────────────────────────────────────────────────── */

typedef struct {
    int kind, gen, index, n_ids, ids[PER_PAGE];
    WCHAR path[700], file[MAX_PATH], pics[4][MAX_PATH];
    char token[160], *fields;    /* (an upload: its text fields, "name\0value\0..." ) */
    size_t fields_n;
} Job;

typedef struct {
    int kind, gen, index, status;
    uint8_t *data;
    size_t n;
    WCHAR text[300];
} Result;

static void post(Job *j, int status, uint8_t *data, size_t n, int index, const WCHAR *text)
{
    Result *r = (Result *)calloc(1, sizeof *r);
    if (!r) { free(data); return; }
    r->kind = j->kind; r->gen = j->gen; r->index = index; r->status = status; r->data = data; r->n = n;
    if (text) wcscpy_s(r->text, 300, text);
    if (!PostMessageW(s_wnd, WM_APP_DISCOVER, 0, (LPARAM)r)) { free(data); free(r); }
}

/* One request to <server><path>: the body (sent in pieces: progress for uploads); the answer (up to `max`). */
static int http(const Job *j, const WCHAR *method, const WCHAR *path, const char *ctype, const uint8_t *body,
                size_t body_n, uint8_t **out, size_t *out_n, size_t max)
{
    WCHAR base[260], url[1000], host[256], upath[900], headers[400];
    URL_COMPONENTS uc;
    HINTERNET ses = NULL, con = NULL, req = NULL;
    DWORD status = 0, sz = sizeof status, got = 0;
    size_t total = 0, cap = 0;
    *out = NULL;
    *out_n = 0;
    s_server(base, 260);
    while (wcslen(base) && base[wcslen(base) - 1] == L'/') base[wcslen(base) - 1] = 0;
    swprintf_s(url, 1000, L"%s%s", base, path);
    ZeroMemory(&uc, sizeof uc);
    uc.dwStructSize = sizeof uc;
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = upath; uc.dwUrlPathLength = 900;
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) return 0;
    ses = WinHttpOpen(L"SvR2011 Launcher", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (ses) {
        WinHttpSetTimeouts(ses, 10000, 15000, 600000, 600000);
        con = WinHttpConnect(ses, host, uc.nPort, 0);
    }
    if (con)
        req = WinHttpOpenRequest(con, method, upath, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if (req) {
        headers[0] = 0;
        if (j->token[0]) swprintf_s(headers, 400, L"Authorization: Bearer %S\r\n", j->token);
        if (ctype) { wcscat_s(headers, 400, L"Content-Type: "); { WCHAR c[160]; swprintf_s(c, 160, L"%S\r\n", ctype); wcscat_s(headers, 400, c); } }
        if (WinHttpSendRequest(req, headers[0] ? headers : WINHTTP_NO_ADDITIONAL_HEADERS, (DWORD)-1L,
                               WINHTTP_NO_REQUEST_DATA, 0, (DWORD)body_n, 0)) {
            size_t sent = 0;
            int ok = 1, last = -1;
            while (ok && sent < body_n) {
                DWORD put = 0, piece = (DWORD)(body_n - sent > (256u << 10) ? (256u << 10) : body_n - sent);
                ok = WinHttpWriteData(req, body + sent, piece, &put) && put;
                sent += put;
                if (body_n > (1u << 20) && (int)(sent * 1000 / body_n) / 10 != last) {
                    last = (int)(sent * 1000 / body_n) / 10;
                    PostMessageW(s_wnd, WM_APP_DISCOVER, R_PROGRESS, (LPARAM)(sent * 1000 / body_n));
                }
            }
            if (ok && WinHttpReceiveResponse(req, NULL)) {
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
                for (;;) {
                    if (total + 65536 > cap) {
                        uint8_t *m;
                        cap = cap ? cap * 2 : 262144;
                        if (cap > max + 65536) cap = max + 65536;
                        if (total + 65536 > cap || !(m = (uint8_t *)realloc(*out, cap + 1))) break;
                        *out = m;
                    }
                    if (!WinHttpReadData(req, *out + total, 65536, &got) || !got) break;
                    total += got;
                }
                if (*out) (*out)[total] = 0;
                *out_n = total;
            }
        }
    }
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    return (int)status;
}

static DWORD WINAPI job_thread(LPVOID p)
{
    Job *j = (Job *)p;
    uint8_t *data = NULL;
    size_t n = 0;
    int st, i;
    WCHAR path[64];
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    switch (j->kind) {
    case R_LIST:
        st = http(j, L"GET", j->path, NULL, NULL, 0, &data, &n, 1 << 20);
        if (st == 200 && data) {   /* (the page's ids, for its pictures) */
            const char *q = jkey((const char *)data, (const char *)data + n, "mods"), *end = (const char *)data + n;
            while (q && q < end && j->n_ids < PER_PAGE && (q = strchr(q, '{')) && q < end) {
                const char *e = jobject_end(q, end);
                j->ids[j->n_ids++] = (int)jnum(jkey(q, e, "id"));
                q = e;
            }
        }
        post(j, st, data, n, 0, NULL);
        /* then the pictures of the page, one after the other */
        for (i = 0; i < j->n_ids && st == 200; i++) {
            if (j->gen != s_gen) break;
            swprintf_s(path, 64, L"/api/mods/%d/img/0", j->ids[i]);
            data = NULL;
            if (http(j, L"GET", path, NULL, NULL, 0, &data, &n, 2 << 20) == 200) {
                j->kind = R_THUMB;
                post(j, 200, data, n, i, NULL);
                j->kind = R_LIST;
            } else free(data);
        }
        break;
    case R_DETAIL:
        st = http(j, L"GET", j->path, NULL, NULL, 0, &data, &n, 1 << 20);
        post(j, st, data, n, 0, NULL);
        j->kind = R_PIC;
        for (i = 0; i < 4 && st == 200 && j->gen == s_gen; i++) {
            swprintf_s(path, 64, L"%s/img/%d", j->path, i);
            data = NULL;
            if (http(j, L"GET", path, NULL, NULL, 0, &data, &n, 2 << 20) == 200) post(j, 200, data, n, i, NULL);
            else free(data);
        }
        break;
    case R_DOWNLOAD: {
        WCHAR dir[MAX_PATH];
        st = http(j, L"GET", j->path, NULL, NULL, 0, &data, &n, 300u << 20);
        if (st == 200 && n) {
            HANDLE f;
            DWORD put = 0;
            swprintf_s(dir, MAX_PATH, L"%s\\Tools", s_game);
            CreateDirectoryW(dir, NULL);
            wcscat_s(dir, MAX_PATH, L"\\.downloads");
            CreateDirectoryW(dir, NULL);
            swprintf_s(j->file, MAX_PATH, L"%s\\discovery.svrmod", dir);
            f = CreateFileW(j->file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f == INVALID_HANDLE_VALUE || !WriteFile(f, data, (DWORD)n, &put, NULL) || put != n) st = -1;
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        }
        free(data);
        post(j, st, NULL, 0, 0, j->file);
        break;
    }
    case R_DELETE:
        st = http(j, L"DELETE", j->path, NULL, NULL, 0, &data, &n, 1 << 16);
        post(j, st, data, n, 0, NULL);
        break;
    case R_UPLOAD: {
        /* multipart: the fields, the four pictures (made here), the file */
        static const char bound[] = "----svr2011discovery7f3a9c";
        uint8_t *pics[4] = { 0 }, *file = NULL, *body = NULL;
        size_t pic_n[4] = { 0 }, file_n = 0, at = 0, cap;
        WCHAR why[200] = L"";
        const char *f;
        HANDLE h;
        for (i = 0; i < 4 && !why[0]; i++)
            if (!(pics[i] = picture_jpeg(j->pics[i], &pic_n[i])) || pic_n[i] > (1u << 20))
                swprintf_s(why, 200, L"Picture %d could not be made into a 1024 x 768 JPEG.", i + 1);
        h = why[0] ? INVALID_HANDLE_VALUE
                   : CreateFileW(j->file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (!why[0]) {
            LARGE_INTEGER sz;
            DWORD got = 0;
            if (h == INVALID_HANDLE_VALUE || !GetFileSizeEx(h, &sz) || sz.QuadPart > (250ll << 20) ||
                !(file = (uint8_t *)malloc((size_t)sz.QuadPart + 1)) ||
                !ReadFile(h, file, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)
                wcscpy_s(why, 200, L"The .svrmod could not be read (250 MB at most).");
            else file_n = (size_t)sz.QuadPart;
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        cap = file_n + j->fields_n + 4 * (1u << 20) + 8192;
        if (!why[0] && (body = (uint8_t *)malloc(cap))) {
            char head[512], fname[MAX_PATH * 3];
            const WCHAR *slash = wcsrchr(j->file, L'\\');
#define PUT(src, len) (memcpy(body + at, (src), (len)), at += (len))
            for (f = j->fields; f < j->fields + j->fields_n; f += strlen(f) + 1) {
                const char *v = f + strlen(f) + 1;
                int k = sprintf_s(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"%s\"\r\n\r\n", bound, f);
                PUT(head, (size_t)k);
                PUT(v, strlen(v));
                PUT("\r\n", 2);
                f = v;
            }
            for (i = 0; i < 4; i++) {
                int k = sprintf_s(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"img%d\"; "
                                  "filename=\"%d.jpg\"\r\nContent-Type: image/jpeg\r\n\r\n", bound, i, i);
                PUT(head, (size_t)k);
                PUT(pics[i], pic_n[i]);
                PUT("\r\n", 2);
            }
            WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : j->file, -1, fname, sizeof fname, NULL, NULL);
            {
                char *q;
                for (q = fname; *q; q++) if (*q == '"') *q = '_';
            }
            {
                int k = sprintf_s(head, sizeof head, "--%s\r\nContent-Disposition: form-data; name=\"svrmod\"; "
                                  "filename=\"%.300s\"\r\nContent-Type: application/octet-stream\r\n\r\n", bound, fname);
                PUT(head, (size_t)k);
            }
            PUT(file, file_n);
            {
                int k = sprintf_s(head, sizeof head, "\r\n--%s--\r\n", bound);
                PUT(head, (size_t)k);
            }
#undef PUT
            free(file);
            file = NULL;
            {
                char ctype[96];
                sprintf_s(ctype, sizeof ctype, "multipart/form-data; boundary=%s", bound);
                st = http(j, L"POST", L"/api/mods", ctype, body, at, &data, &n, 1 << 16);
            }
            post(j, st, data, n, 0, NULL);
        } else {
            post(j, -1, NULL, 0, 0, why[0] ? why : L"Not enough memory for the upload.");
        }
        free(body);
        free(file);
        for (i = 0; i < 4; i++) free(pics[i]);
        break;
    }
    }
    free(j->fields);
    free(j);
    CoUninitialize();
    return 0;
}

static void start(Job *j)
{
    HANDLE t;
    WCHAR name[8];
    j->gen = s_gen;
    if (!j->token[0]) s_account(j->token, sizeof j->token, name, 1);
    t = CreateThread(NULL, 0, job_thread, j, 0, NULL);
    if (t) CloseHandle(t);
    else { free(j->fields); free(j); }
}

static Job *job(int kind)
{
    Job *j = (Job *)calloc(1, sizeof *j);
    if (j) j->kind = kind;
    return j;
}

/* ── the views ─────────────────────────────────────────────────────────── */

static const int k_grid_ids[] = { D_SEARCH, D_TYPE, D_SORT, D_GO, D_GRID, D_PREV, D_PAGE, D_NEXT, D_UPLOAD };
static const int k_mod_ids[] = { D_PIC, D_PICPREV, D_PICLBL, D_PICNEXT, D_INFO, D_DESC, D_BACK, D_DOWNLOAD, D_DELETE };

static void set_view(int v)
{
    int i;
    s_view = v;
    for (i = 0; i < D_COUNT; i++) {
        int show = i == D_STATUS;
        size_t k;
        for (k = 0; k < sizeof k_grid_ids / sizeof k_grid_ids[0]; k++) if (k_grid_ids[k] == i) show = v == V_GRID;
        for (k = 0; k < sizeof k_mod_ids / sizeof k_mod_ids[0]; k++) if (k_mod_ids[k] == i) show = v == V_MOD;
        if (i >= U_TITLE && i <= U_PROGRESS) show = v == V_UPLOAD;
        if (s_c[i]) ShowWindow(s_c[i], show ? SW_SHOW : SW_HIDE);
    }
}

static void search(void)
{
    static const char *const types[] = { "", "entrance", "superstar", "arena", "moveset" };
    static const char *const sorts[] = { "new", "popular", "name" };
    WCHAR q[120], qe[400];
    char u[400];
    const char *s;
    size_t k = 0;
    int ti = (int)SendMessageW(s_c[D_TYPE], CB_GETCURSEL, 0, 0), si = (int)SendMessageW(s_c[D_SORT], CB_GETCURSEL, 0, 0);
    Job *j = job(R_LIST);
    if (!j) return;
    GetWindowTextW(s_c[D_SEARCH], q, 120);
    WideCharToMultiByte(CP_UTF8, 0, q, -1, u, sizeof u, NULL, NULL);
    for (s = u; *s && k + 4 < 400; s++) {   /* (percent-encoded) */
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
            qe[k++] = c;
        else { swprintf_s(qe + k, 400 - k, L"%%%02X", c); k += 3; }
    }
    qe[k] = 0;
    s_gen++;
    swprintf_s(j->path, 700, L"/api/mods?q=%s&type=%S&sort=%S&page=%d", qe, types[ti > 0 && ti < 5 ? ti : 0],
               sorts[si > 0 && si < 3 ? si : 0], s_page);
    status(L"Looking\x2026");
    start(j);
}

static void open_mod(int i)
{
    Job *j;
    int k;
    if (i < 0 || i >= s_n) return;
    s_cur = i;
    s_pic = 0;
    for (k = 0; k < 4; k++) if (s_pics[k]) { DeleteObject(s_pics[k]); s_pics[k] = NULL; }
    SetWindowTextW(s_c[D_INFO], L"");
    SetWindowTextW(s_c[D_DESC], L"");
    SetWindowTextW(s_c[D_PICLBL], L"1 / 4");
    {
        char token[160];
        WCHAR me[64];
        s_account(token, sizeof token, me, 64);
        ShowWindow(s_c[D_DELETE], SW_HIDE);
        set_view(V_MOD);
        ShowWindow(s_c[D_DELETE], me[0] && !_wcsicmp(me, s_mods[i].author) ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(s_c[D_PIC], NULL, TRUE);
    if (!(j = job(R_DETAIL))) return;
    s_gen++;
    swprintf_s(j->path, 700, L"/api/mods/%d", s_mods[i].id);
    start(j);
}

static void upload_view(void)
{
    char token[160];
    WCHAR me[64], t[160];
    s_account(token, sizeof token, me, 64);
    set_view(V_UPLOAD);
    if (token[0] && me[0]) swprintf_s(t, 160, L"Author: %s (your online name)", me);
    else wcscpy_s(t, 160, L"Sign in online first (the Online page): your online name is the author.");
    SetWindowTextW(s_c[U_AUTHOR], t);
    EnableWindow(s_c[U_SEND], token[0] && me[0] && !s_busy);
}

static int pick_files(const WCHAR *title, int pictures, WCHAR out[][MAX_PATH], int max)
{
    IFileOpenDialog *dlg = NULL;
    IShellItemArray *items = NULL;
    DWORD n = 0, i;
    int got = 0;
    COMDLG_FILTERSPEC mod[] = { { L"SvR2011 mods (*.svrmod)", L"*.svrmod" } };
    COMDLG_FILTERSPEC pic[] = { { L"Pictures (*.png, *.jpg, *.jpeg, *.bmp)", L"*.png;*.jpg;*.jpeg;*.bmp" } };
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void **)&dlg)))
        return 0;
    IFileOpenDialog_SetFileTypes(dlg, 1, pictures ? pic : mod);
    IFileOpenDialog_SetOptions(dlg, FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (pictures ? FOS_ALLOWMULTISELECT : 0));
    IFileOpenDialog_SetTitle(dlg, title);
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, s_wnd)) && SUCCEEDED(IFileOpenDialog_GetResults(dlg, &items))) {
        IShellItemArray_GetCount(items, &n);
        for (i = 0; i < n && got < max; i++) {
            IShellItem *it = NULL;
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItemArray_GetItemAt(items, i, &it)) &&
                SUCCEEDED(IShellItem_GetDisplayName(it, SIGDN_FILESYSPATH, &p))) {
                wcsncpy_s(out[got++], MAX_PATH, p, _TRUNCATE);
                CoTaskMemFree(p);
            }
            if (it) IShellItem_Release(it);
        }
        IShellItemArray_Release(items);
    }
    IFileOpenDialog_Release(dlg);
    return got;
}

static void send_upload(void)
{
    static const char *const keys[] = { "name", "version", "tags", "description" };
    static const int ids[] = { U_NAME, U_VER, U_TAGS, U_DESC };
    Job *j;
    char types[64] = "", *fields;
    size_t at = 0;
    int i, k;
    WCHAR w[2001];
    for (i = 0; i < 4; i++)
        if (SendMessageW(s_c[U_T0 + i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            if (types[0]) strcat_s(types, sizeof types, ",");
            WideCharToMultiByte(CP_UTF8, 0, k_types[i], -1, types + strlen(types), (int)(sizeof types - strlen(types)), NULL, NULL);
        }
    GetWindowTextW(s_c[U_NAME], w, 61);
    if (!w[0] || !GetWindowTextLengthW(s_c[U_VER]) || !types[0]) { status(L"A name, a version and the mod's type are needed."); return; }
    if (!s_up_file[0]) { status(L"Choose the .svrmod file."); return; }
    for (i = 0; i < 4; i++) if (!s_up_pics[i][0]) { status(L"Choose four pictures of the mod."); return; }
    if (!(j = job(R_UPLOAD)) || !(fields = (char *)malloc(16384))) { free(j); return; }
    for (k = 0; k < 4; k++) {
        GetWindowTextW(s_c[ids[k]], w, 2001);
        at += (size_t)sprintf_s(fields + at, 16384 - at, "%s", keys[k]) + 1;
        at += (size_t)WideCharToMultiByte(CP_UTF8, 0, w, -1, fields + at, (int)(16384 - at - 100), NULL, NULL);
    }
    at += (size_t)sprintf_s(fields + at, 16384 - at, "types") + 1;
    at += (size_t)sprintf_s(fields + at, 16384 - at, "%s", types) + 1;
    j->fields = fields;
    j->fields_n = at;
    wcscpy_s(j->file, MAX_PATH, s_up_file);
    for (i = 0; i < 4; i++) wcscpy_s(j->pics[i], MAX_PATH, s_up_pics[i]);
    s_busy = 1;
    EnableWindow(s_c[U_SEND], FALSE);
    SendMessageW(s_c[U_PROGRESS], PBM_SETPOS, 0, 0);
    ShowWindow(s_c[U_PROGRESS], SW_SHOW);
    status(L"Making the pictures and sending the mod\x2026");
    start(j);
}

/* ── the launcher's side ───────────────────────────────────────────────── */

void discover_build(HWND wnd, mods_add_fn add, int tab, int id_base, discover_server_fn server,
                    discover_account_fn account)
{
    static const WCHAR *const types[] = { L"All types", L"Entrance", L"Superstar", L"Arena", L"Move-set" };
    static const WCHAR *const sorts[] = { L"Newest", L"Most downloaded", L"By name" };
    int i, y;
#define C(i, cls, text, style, x, y, w, h) (s_c[i] = add(tab, cls, text, style, x, y, w, h, id_base + (i)))
    s_wnd = wnd;
    s_base = id_base;
    s_server = server;
    s_account = account;
    /* the grid */
    C(D_SEARCH, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 28, 54, 214, 26);
    SendMessageW(s_c[D_SEARCH], EM_SETCUEBANNER, TRUE, (LPARAM)L"Search by name, author or tag");
    C(D_TYPE, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 250, 54, 120, 200);
    for (i = 0; i < 5; i++) SendMessageW(s_c[D_TYPE], CB_ADDSTRING, 0, (LPARAM)types[i]);
    SendMessageW(s_c[D_TYPE], CB_SETCURSEL, 0, 0);
    C(D_SORT, L"ComboBox", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 378, 54, 120, 200);
    for (i = 0; i < 3; i++) SendMessageW(s_c[D_SORT], CB_ADDSTRING, 0, (LPARAM)sorts[i]);
    SendMessageW(s_c[D_SORT], CB_SETCURSEL, 0, 0);
    C(D_GO, L"Button", L"Search", BS_DEFPUSHBUTTON | WS_TABSTOP, 506, 53, 82, 28);
    C(D_GRID, WC_LISTVIEWW, L"", LVS_ICON | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP, 28, 88, 560, 392);
    ListView_SetExtendedListViewStyle(s_c[D_GRID], LVS_EX_DOUBLEBUFFER);
    s_thumbs = ImageList_Create(THUMB_W, THUMB_H, ILC_COLOR32, PER_PAGE, 0);
    ListView_SetImageList(s_c[D_GRID], s_thumbs, LVSIL_NORMAL);
    ListView_SetIconSpacing(s_c[D_GRID], THUMB_W + 22, THUMB_H + 46);
    C(D_PREV, L"Button", L"\x2190 Previous", BS_PUSHBUTTON | WS_TABSTOP, 28, 488, 100, 28);
    C(D_PAGE, L"Static", L"", SS_CENTER | SS_NOPREFIX, 132, 494, 170, 20);
    C(D_NEXT, L"Button", L"Next \x2192", BS_PUSHBUTTON | WS_TABSTOP, 306, 488, 100, 28);
    C(D_UPLOAD, L"Button", L"Upload a mod\x2026", BS_PUSHBUTTON | WS_TABSTOP, 444, 486, 144, 32);
    C(D_STATUS, L"Static", L"", SS_LEFT | SS_NOPREFIX, 28, 528, 560, 36);
    /* a mod's page */
    C(D_PIC, L"Static", L"", SS_OWNERDRAW, 28, 54, PIC_W, PIC_H);
    C(D_PICPREV, L"Button", L"\x2190", BS_PUSHBUTTON | WS_TABSTOP, 28, 360, 44, 28);
    C(D_PICLBL, L"Static", L"1 / 4", SS_CENTER, 80, 366, 268, 20);
    C(D_PICNEXT, L"Button", L"\x2192", BS_PUSHBUTTON | WS_TABSTOP, 384, 360, 44, 28);
    C(D_INFO, L"Static", L"", SS_LEFT | SS_NOPREFIX, 442, 54, 146, 300);
    C(D_DESC, L"Edit", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER, 28, 396, 560, 84);
    C(D_BACK, L"Button", L"\x2190 Back", BS_PUSHBUTTON | WS_TABSTOP, 28, 488, 100, 30);
    C(D_DOWNLOAD, L"Button", L"Download and install", BS_DEFPUSHBUTTON | WS_TABSTOP, 136, 486, 200, 32);
    C(D_DELETE, L"Button", L"Delete my mod", BS_PUSHBUTTON | WS_TABSTOP, 458, 488, 130, 30);
    /* the upload form */
    C(U_TITLE, L"Static", L"Upload a mod: every field but tags and the description is needed. Only .svrmod files.",
      SS_LEFT | SS_NOPREFIX, 28, 54, 560, 20);
    y = 82;
    C(U_NAME_L, L"Static", L"Name", SS_LEFT, 28, y + 4, 90, 20);
    C(U_NAME, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 120, y, 280, 26);
    SendMessageW(s_c[U_NAME], EM_LIMITTEXT, 60, 0);
    C(U_VER_L, L"Static", L"Version", SS_LEFT, 412, y + 4, 60, 20);
    C(U_VER, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 474, y, 114, 26);
    SendMessageW(s_c[U_VER], EM_LIMITTEXT, 20, 0);
    y += 36;
    C(U_TYPES_L, L"Static", L"Type", SS_LEFT, 28, y + 2, 90, 20);
    for (i = 0; i < 4; i++)
        C(U_T0 + i, L"Button", k_type_names[i], BS_AUTOCHECKBOX | WS_TABSTOP, 120 + i * 116, y, 112, 22);
    y += 32;
    C(U_TAGS_L, L"Static", L"Tags", SS_LEFT, 28, y + 4, 90, 20);
    C(U_TAGS, L"Edit", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 120, y, 468, 26);
    SendMessageW(s_c[U_TAGS], EM_SETCUEBANNER, TRUE, (LPARAM)L"e.g. attitude era, cage, classic (commas between)");
    y += 36;
    C(U_DESC_L, L"Static", L"Description", SS_LEFT, 28, y + 4, 90, 20);
    C(U_DESC, L"Edit", L"", ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 120, y, 468, 76);
    SendMessageW(s_c[U_DESC], EM_LIMITTEXT, 2000, 0);
    y += 86;
    C(U_FILE_L, L"Static", L".svrmod", SS_LEFT, 28, y + 6, 90, 20);
    C(U_FILE, L"Static", L"(none chosen)", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, 120, y + 6, 330, 20);
    C(U_FILE_PICK, L"Button", L"Choose\x2026", BS_PUSHBUTTON | WS_TABSTOP, 468, y, 120, 30);
    y += 38;
    C(U_PICS_L, L"Static", L"Pictures", SS_LEFT, 28, y + 6, 90, 20);
    C(U_PICS, L"Static", L"(four, any size: made 1024 x 768)", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 120, y + 6, 330, 20);
    C(U_PICS_PICK, L"Button", L"Choose 4\x2026", BS_PUSHBUTTON | WS_TABSTOP, 468, y, 120, 30);
    y += 40;
    C(U_AUTHOR, L"Static", L"", SS_LEFT | SS_NOPREFIX, 28, y, 560, 20);
    y += 28;
    C(U_CANCEL, L"Button", L"\x2190 Back", BS_PUSHBUTTON | WS_TABSTOP, 28, y, 100, 30);
    C(U_SEND, L"Button", L"Upload", BS_DEFPUSHBUTTON | WS_TABSTOP, 136, y - 1, 140, 32);
    C(U_PROGRESS, PROGRESS_CLASSW, L"", PBS_SMOOTH, 288, y + 8, 300, 14);
    SendMessageW(s_c[U_PROGRESS], PBM_SETRANGE32, 0, 1000);
#undef C
    set_view(V_GRID);
}

void discover_show(const WCHAR *game_dir)
{
    wcscpy_s(s_game, MAX_PATH, game_dir ? game_dir : L"");
    set_view(s_view);
    if (s_view == V_UPLOAD) upload_view();
    if (s_view == V_UPLOAD && !s_busy) ShowWindow(s_c[U_PROGRESS], SW_HIDE);
    if (!s_shown) {
        s_shown = 1;
        search();
    }
}

int discover_command(int id, int code)
{
    const int i = id - s_base;
    if (i < 0 || i >= D_COUNT) return 0;
    if (code != BN_CLICKED && i != D_SEARCH && i != D_TYPE && i != D_SORT) return 0;
    switch (i) {
    case D_TYPE: case D_SORT:
        if (code == CBN_SELCHANGE) { s_page = 0; search(); }
        return 1;
    case D_GO: s_page = 0; search(); return 1;
    case D_PREV: if (s_page > 0) { s_page--; search(); } return 1;
    case D_NEXT: if ((s_page + 1) * PER_PAGE < s_total) { s_page++; search(); } return 1;
    case D_UPLOAD: upload_view(); return 1;
    case D_BACK: case U_CANCEL: s_gen++; set_view(V_GRID); return 1;
    case D_PICPREV: case D_PICNEXT: {
        WCHAR t[16];
        s_pic = (s_pic + (i == D_PICNEXT ? 1 : 3)) % 4;
        swprintf_s(t, 16, L"%d / 4", s_pic + 1);
        SetWindowTextW(s_c[D_PICLBL], t);
        InvalidateRect(s_c[D_PIC], NULL, TRUE);
        return 1;
    }
    case D_DOWNLOAD: {
        Job *j;
        if (s_cur < 0 || s_busy) return 1;
        if (!s_game[0]) { status(L"Install the game first (Setup)."); return 1; }
        if (!(j = job(R_DOWNLOAD))) return 1;
        s_busy = 1;
        EnableWindow(s_c[D_DOWNLOAD], FALSE);
        swprintf_s(j->path, 700, L"/api/mods/%d/file", s_mods[s_cur].id);
        status(L"Downloading\x2026");
        start(j);
        return 1;
    }
    case D_DELETE: {
        Job *j;
        WCHAR t[200];
        if (s_cur < 0) return 1;
        swprintf_s(t, 200, L"Delete \"%s\" from Discovery for everyone?", s_mods[s_cur].name);
        if (MessageBoxW(s_wnd, t, L"Delete my mod", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return 1;
        if (!(j = job(R_DELETE))) return 1;
        swprintf_s(j->path, 700, L"/api/mods/%d", s_mods[s_cur].id);
        start(j);
        return 1;
    }
    case U_FILE_PICK: {
        WCHAR f[1][MAX_PATH];
        if (pick_files(L"Choose the .svrmod to upload", 0, f, 1)) {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            WCHAR t[MAX_PATH + 40], sz[32] = L"";
            wcscpy_s(s_up_file, MAX_PATH, f[0]);
            if (GetFileAttributesExW(f[0], GetFileExInfoStandard, &fa))
                fmt_bytes(sz, 32, ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
            swprintf_s(t, MAX_PATH + 40, L"%s  (%s)", wcsrchr(f[0], L'\\') ? wcsrchr(f[0], L'\\') + 1 : f[0], sz);
            SetWindowTextW(s_c[U_FILE], t);
        }
        return 1;
    }
    case U_PICS_PICK: {
        WCHAR p[4][MAX_PATH], t[600] = L"";
        int n = pick_files(L"Choose four pictures of the mod (Ctrl+click)", 1, p, 4), k;
        if (!n) return 1;
        if (n < 4) { status(L"Choose four pictures at once (Ctrl+click them)."); return 1; }
        for (k = 0; k < 4; k++) {
            wcscpy_s(s_up_pics[k], MAX_PATH, p[k]);
            if (t[0]) wcscat_s(t, 600, L", ");
            wcscat_s(t, 600, wcsrchr(p[k], L'\\') ? wcsrchr(p[k], L'\\') + 1 : p[k]);
        }
        SetWindowTextW(s_c[U_PICS], t);
        return 1;
    }
    case U_SEND: send_upload(); return 1;
    }
    return 0;
}

int discover_notify(const NMHDR *nm)
{
    if (!s_c[D_GRID] || nm->hwndFrom != s_c[D_GRID]) return 0;
    if (nm->code == NM_CLICK || nm->code == NM_RETURN) {
        const int i = nm->code == NM_CLICK ? ((const NMITEMACTIVATE *)nm)->iItem
                                           : ListView_GetNextItem(s_c[D_GRID], -1, LVNI_SELECTED);
        if (i >= 0) {
            LVITEMW it;
            ZeroMemory(&it, sizeof it);
            it.mask = LVIF_PARAM;
            it.iItem = i;
            if (ListView_GetItem(s_c[D_GRID], &it)) open_mod((int)it.lParam);
        }
        return 1;
    }
    return 0;
}

int discover_draw(const DRAWITEMSTRUCT *d)
{
    RECT r = d->rcItem;
    HDC mdc;
    if (!s_c[D_PIC] || d->hwndItem != s_c[D_PIC]) return 0;
    FillRect(d->hDC, &r, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (s_pics[s_pic] && (mdc = CreateCompatibleDC(d->hDC))) {
        HGDIOBJ old = SelectObject(mdc, s_pics[s_pic]);
        SetStretchBltMode(d->hDC, HALFTONE);
        StretchBlt(d->hDC, r.left, r.top, r.right - r.left, r.bottom - r.top, mdc, 0, 0, PIC_W, PIC_H, SRCCOPY);
        SelectObject(mdc, old);
        DeleteDC(mdc);
    } else {
        SetBkMode(d->hDC, TRANSPARENT);
        SetTextColor(d->hDC, RGB(150, 153, 163));
        DrawTextW(d->hDC, L"Loading the pictures\x2026", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    return 1;
}

/* An error answer's text ({"error": "..."}), or a general one. */
static void answer_error(const Result *r, const WCHAR *what, WCHAR *out, size_t n)
{
    WCHAR e[200] = L"";
    if (r->data) jstr(jkey((const char *)r->data, (const char *)r->data + r->n, "error"), (const char *)r->data + r->n, e, 200);
    if (r->text[0]) swprintf_s(out, n, L"%s: %s", what, r->text);
    else if (e[0]) swprintf_s(out, n, L"%s: %s", what, e);
    else if (r->status <= 0) swprintf_s(out, n, L"%s: the server didn't answer (online server, internet?).", what);
    else swprintf_s(out, n, L"%s: the server said %d.", what, r->status);
}

int discover_message(UINT m, WPARAM wp, LPARAM lp)
{
    Result *r;
    WCHAR t[400];
    if (m != WM_APP_DISCOVER) return 0;
    if (wp == R_PROGRESS) {
        SendMessageW(s_c[U_PROGRESS], PBM_SETPOS, (WPARAM)lp, 0);
        return 1;
    }
    r = (Result *)lp;
    if (!r) return 1;
    if ((r->kind == R_LIST || r->kind == R_THUMB || r->kind == R_DETAIL || r->kind == R_PIC) && r->gen != s_gen) {
        free(r->data);
        free(r);
        return 1;
    }
    switch (r->kind) {
    case R_LIST: {
        const char *p, *end = (const char *)r->data + r->n;
        ListView_DeleteAllItems(s_c[D_GRID]);
        ImageList_RemoveAll(s_thumbs);
        s_n = 0;
        if (r->status != 200 || !r->data) { answer_error(r, L"Discovery", t, 400); status(t); break; }
        s_total = (int)jnum(jkey((const char *)r->data, end, "total"));
        p = jkey((const char *)r->data, end, "mods");
        while (p && p < end && s_n < PER_PAGE && (p = strchr(p, '{')) && p < end) {
            const char *e = jobject_end(p, end);
            LVITEMW it;
            WCHAR label[140];
            parse_mod(p, e, &s_mods[s_n]);
            swprintf_s(label, 140, L"%s\n%s", s_mods[s_n].name, s_mods[s_n].author);
            ZeroMemory(&it, sizeof it);
            it.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
            it.iItem = s_n;
            it.iImage = -1;
            it.lParam = s_n;
            it.pszText = s_mods[s_n].name;
            ListView_InsertItem(s_c[D_GRID], &it);
            s_n++;
            p = e;
        }
        swprintf_s(t, 400, s_total ? L"Page %d of %d" : L"", s_page + 1, (s_total + PER_PAGE - 1) / PER_PAGE);
        SetWindowTextW(s_c[D_PAGE], t);
        EnableWindow(s_c[D_PREV], s_page > 0);
        EnableWindow(s_c[D_NEXT], (s_page + 1) * PER_PAGE < s_total);
        swprintf_s(t, 400, s_total ? L"%d mod%s found. Click one for its page." : L"No mods found yet - be the first: Upload a mod.",
                   s_total, s_total == 1 ? L"" : L"s");
        status(t);
        static int tested;
        if (!tested++) {   /* test aids (--capture discover), after the first list only: SVR2011_DISCOVER_OPEN=<n> opens
                              the n-th mod's page, SVR2011_DISCOVER_UPLOAD=1 the upload form */
            const char *v = getenv("SVR2011_DISCOVER_OPEN");
            if (v && *v && atoi(v) < s_n) open_mod(atoi(v));
            else if ((v = getenv("SVR2011_DISCOVER_UPLOAD")) && *v == '1') upload_view();
            else if ((v = getenv("SVR2011_DISCOVER_SEND")) && *v && !s_busy) {
                /* SVR2011_DISCOVER_SEND=<file.svrmod>|<picture>: the form filled (the picture four times), sent */
                WCHAR w[2 * MAX_PATH], *bar;
                int k;
                MultiByteToWideChar(CP_UTF8, 0, v, -1, w, 2 * MAX_PATH);
                if ((bar = wcschr(w, L'|')) != NULL) {
                    *bar = 0;
                    upload_view();
                    SetWindowTextW(s_c[U_NAME], L"Upload test");
                    SetWindowTextW(s_c[U_VER], L"0.1");
                    SetWindowTextW(s_c[U_TAGS], L"Test, Launcher");
                    SendMessageW(s_c[U_T2], BM_SETCHECK, BST_CHECKED, 0);
                    wcscpy_s(s_up_file, MAX_PATH, w);
                    for (k = 0; k < 4; k++) wcscpy_s(s_up_pics[k], MAX_PATH, bar + 1);
                    send_upload();
                }
            }
        }
        break;
    }
    case R_THUMB: {
        HBITMAP b = picture_bitmap(r->data, r->n, THUMB_W, THUMB_H);
        if (b && r->index < s_n) {
            LVITEMW it;
            ZeroMemory(&it, sizeof it);
            it.mask = LVIF_IMAGE;
            it.iItem = r->index;
            it.iImage = ImageList_Add(s_thumbs, b, NULL);
            ListView_SetItem(s_c[D_GRID], &it);
        }
        if (b) DeleteObject(b);
        break;
    }
    case R_DETAIL: {
        const char *end = (const char *)r->data + r->n;
        WCHAR desc[2001], sz[32];
        Mod *md = &s_mods[s_cur >= 0 ? s_cur : 0];
        if (r->status != 200 || !r->data) { answer_error(r, L"The mod's page", t, 400); status(t); break; }
        parse_mod((const char *)r->data, end, md);
        jstr(jkey((const char *)r->data, end, "description"), end, desc, 2001);
        jstr(jkey((const char *)r->data, end, "file_name"), end, s_file_name, MAX_PATH);
        fmt_bytes(sz, 32, md->size);
        swprintf_s(t, 400, L"%s\nby %s\n\n%s\nVersion %s\n%s\n%d download%s%s%s", md->name, md->author, md->types,
                   md->version, sz, md->downloads, md->downloads == 1 ? L"" : L"s", md->tags[0] ? L"\n\nTags: " : L"",
                   md->tags);
        SetWindowTextW(s_c[D_INFO], t);
        {   /* (the description's line ends as the edit control wants them) */
            WCHAR crlf[4100];
            size_t a = 0, b2 = 0;
            for (; desc[a] && b2 + 3 < 4100; a++) {
                if (desc[a] == L'\n') crlf[b2++] = L'\r';
                crlf[b2++] = desc[a];
            }
            crlf[b2] = 0;
            SetWindowTextW(s_c[D_DESC], crlf[0] ? crlf : L"(no description)");
        }
        status(L"");
        break;
    }
    case R_PIC:
        if (r->index >= 0 && r->index < 4) {
            if (s_pics[r->index]) DeleteObject(s_pics[r->index]);
            s_pics[r->index] = picture_bitmap(r->data, r->n, PIC_W, PIC_H);
            if (r->index == s_pic) InvalidateRect(s_c[D_PIC], NULL, TRUE);
        }
        break;
    case R_DOWNLOAD:
        s_busy = 0;
        EnableWindow(s_c[D_DOWNLOAD], TRUE);
        if (r->status != 200) { answer_error(r, L"Download", t, 400); status(t); break; }
        if (mods_install(s_game, r->text)) {
            swprintf_s(t, 400, L"Installed \"%s\" - it's on the Installed Mods tab and loads the next time the game starts.",
                       s_cur >= 0 ? s_mods[s_cur].name : L"");
            status(t);
        } else {
            status(L"Downloaded, but it could not be installed - the Installed Mods tab says why.");
        }
        DeleteFileW(r->text);
        break;
    case R_DELETE:
        if (r->status != 200) { answer_error(r, L"Delete", t, 400); status(t); break; }
        status(L"Deleted from Discovery.");
        set_view(V_GRID);
        search();
        break;
    case R_UPLOAD:
        s_busy = 0;
        ShowWindow(s_c[U_PROGRESS], SW_HIDE);
        upload_view();
        if (r->status != 200) { answer_error(r, L"Upload", t, 400); status(t); break; }
        {
            int k;
            static const int clear[] = { U_NAME, U_VER, U_TAGS, U_DESC };
            for (k = 0; k < 4; k++) SetWindowTextW(s_c[clear[k]], L"");
            for (k = 0; k < 4; k++) { SendMessageW(s_c[U_T0 + k], BM_SETCHECK, BST_UNCHECKED, 0); s_up_pics[k][0] = 0; }
            s_up_file[0] = 0;
            SetWindowTextW(s_c[U_FILE], L"(none chosen)");
            SetWindowTextW(s_c[U_PICS], L"(four, any size: made 1024 x 768)");
        }
        status(L"Uploaded - it's in Discovery for everyone now.");
        set_view(V_GRID);
        s_page = 0;
        search();
        break;
    }
    free(r->data);
    free(r);
    return 1;
}
