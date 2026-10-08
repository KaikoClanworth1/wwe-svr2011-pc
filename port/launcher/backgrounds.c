/* WWE SmackDown vs. Raw 2011 PC launcher - custom main menu backgrounds (see
 * backgrounds.h).
 *
 * The picture is fitted to where the main menu puts it: bg_map.h.
 */
#define COBJMACROS
#include "backgrounds.h"
#include "bg_map.h"

#include <objbase.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincodec.h>

static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* A DXT5 DDS header for 1024 x 512, one level. */
static void dds_header(uint8_t *d)
{
    memset(d, 0, 128);
    memcpy(d, "DDS ", 4);
    wr32(d + 4, 124);
    wr32(d + 8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000);  /* caps, height, width, pixel format, linear size */
    wr32(d + 12, BG_H);
    wr32(d + 16, BG_W);
    wr32(d + 20, BG_W * BG_H);                       /* (DXT5: a byte a pixel) */
    wr32(d + 76, 32);
    wr32(d + 80, 0x4);                               /* fourcc */
    memcpy(d + 84, "DXT5", 4);
    wr32(d + 108, 0x1000);                           /* texture */
}

static uint16_t to565(int r, int g, int b)
{
    return (uint16_t)(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

/* One opaque 4 x 4 block (px: BGRA) -> 16 bytes of DXT5: the colours' bounding
 * box, inset a little, its diagonal turned to follow the block's colours. */
static void dxt5_block(const uint8_t *px[16], uint8_t *o)
{
    int i, k, mn[3] = { 255, 255, 255 }, mx[3] = { 0, 0, 0 }, mean[3] = { 0, 0, 0 };
    int c0[3], c1[3], pal[4][3];
    uint16_t e0, e1;
    uint32_t bits = 0;
    long cov_rg = 0, cov_bg = 0;
    for (i = 0; i < 16; i++)
        for (k = 0; k < 3; k++) {
            const int v = px[i][2 - k];  /* r, g, b */
            mean[k] += v;
            if (v < mn[k]) mn[k] = v;
            if (v > mx[k]) mx[k] = v;
        }
    for (k = 0; k < 3; k++) mean[k] /= 16;
    for (i = 0; i < 16; i++) {
        const int r = px[i][2] - mean[0], g = px[i][1] - mean[1], b = px[i][0] - mean[2];
        cov_rg += r * g;
        cov_bg += b * g;
    }
    for (k = 0; k < 3; k++) {  /* inset by 1/16 of the range */
        const int inset = (mx[k] - mn[k]) / 16;
        c0[k] = mx[k] - inset;
        c1[k] = mn[k] + inset;
    }
    /* (red or blue going against green: swap that channel's ends) */
    if (cov_rg < 0) { const int t = c0[0]; c0[0] = c1[0]; c1[0] = t; }
    if (cov_bg < 0) { const int t = c0[2]; c0[2] = c1[2]; c1[2] = t; }
    e0 = to565(c0[0], c0[1], c0[2]);
    e1 = to565(c1[0], c1[1], c1[2]);
    if (e0 < e1) { const uint16_t t = e0; e0 = e1; e1 = t; }
    /* alpha: opaque */
    o[0] = 255; o[1] = 255;
    memset(o + 2, 0, 6);
    o[8] = (uint8_t)e0; o[9] = (uint8_t)(e0 >> 8); o[10] = (uint8_t)e1; o[11] = (uint8_t)(e1 >> 8);
    if (e0 == e1) {
        memset(o + 12, 0, 4);
        return;
    }
    pal[0][0] = ((e0 >> 11) & 31) * 255 / 31; pal[0][1] = ((e0 >> 5) & 63) * 255 / 63; pal[0][2] = (e0 & 31) * 255 / 31;
    pal[1][0] = ((e1 >> 11) & 31) * 255 / 31; pal[1][1] = ((e1 >> 5) & 63) * 255 / 63; pal[1][2] = (e1 & 31) * 255 / 31;
    for (k = 0; k < 3; k++) {
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
    for (i = 0; i < 16; i++) {
        const int r = px[i][2], g = px[i][1], b = px[i][0];
        int best = 0, bd = 1 << 30;
        for (k = 0; k < 4; k++) {
            const int dr = r - pal[k][0], dg = g - pal[k][1], db = b - pal[k][2];
            const int dd = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
            if (dd < bd) { bd = dd; best = k; }
        }
        bits |= (uint32_t)best << (2 * i);
    }
    wr32(o + 12, bits);
}

/* The picture (any format WIC reads) as 1024 x 512 BGRA, fitted as above. */
static int read_fitted(const WCHAR *path, uint8_t *out, WCHAR *err, size_t errn)
{
    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    IWICBitmapScaler *scaler = NULL;
    UINT w = 0, h = 0, sw, sh;
    uint8_t *img = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&fac))
            || FAILED(IWICImagingFactory_CreateDecoderFromFilename(fac, path, NULL, GENERIC_READ,
                                                                   WICDecodeMetadataCacheOnDemand, &dec))
            || FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame))
            || FAILED(IWICImagingFactory_CreateFormatConverter(fac, &conv))
            || FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                                     WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))
            || FAILED(IWICFormatConverter_GetSize(conv, &w, &h)) || w < 16 || h < 16) {
        swprintf_s(err, errn, L"Could not read %s as a picture (PNG, JPG, BMP or WebP).", path);
        goto done;
    }
    bg_cover_size(w, h, &sw, &sh);  /* (bg_map.h) */
    if (FAILED(IWICImagingFactory_CreateBitmapScaler(fac, &scaler))
            || FAILED(IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource *)conv, sw, sh,
                                                  WICBitmapInterpolationModeHighQualityCubic))) {
        swprintf_s(err, errn, L"Could not scale %s.", path);
        goto done;
    }
    img = (uint8_t *)malloc((size_t)sw * sh * 4);
    if (!img || FAILED(IWICBitmapScaler_CopyPixels(scaler, NULL, sw * 4, sw * sh * 4, img))) {
        swprintf_s(err, errn, L"Could not read the pixels of %s.", path);
        goto done;
    }
    bg_map(img, sw, sh, out);
    ok = 1;
done:
    free(img);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (fac) IWICImagingFactory_Release(fac);
    return ok;
}

/* A folder name from the picture's name: letters, digits, - and _. */
static void make_id(const WCHAR *name, WCHAR *id, size_t n)
{
    size_t k = 0;
    const WCHAR *p;
    for (p = name; *p && k + 1 < n && k < 40; p++) {
        const WCHAR c = *p;
        if ((c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'-' || c == L'_') id[k++] = c;
        else if (c >= L'A' && c <= L'Z') id[k++] = (WCHAR)(c - L'A' + L'a');
        else if (k && id[k - 1] != L'_') id[k++] = L'_';
    }
    while (k && id[k - 1] == L'_') k--;
    id[k] = 0;
    if (!k) wcscpy_s(id, n, L"background");
}

int bg_make(const WCHAR *game_dir, const WCHAR *image, const WCHAR *name, int logo, WCHAR *out_dir, WCHAR *err,
            size_t errn)
{
    WCHAR title[128], base_id[64], id[80], dir[MAX_PATH], path[MAX_PATH];
    uint8_t *px = NULL, *dds = NULL;
    const HRESULT co = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int ok = 0, n, bx, by;
    FILE *f = NULL;
    if (!game_dir || !game_dir[0]) {
        swprintf_s(err, errn, L"Install the game first (Install tab).");
        goto done;
    }
    /* the name: given, or the file's (without its folder and extension) */
    if (name && name[0]) {
        wcsncpy_s(title, 128, name, _TRUNCATE);
    } else {
        const WCHAR *s = wcsrchr(image, L'\\'), *s2 = wcsrchr(image, L'/');
        WCHAR *dot;
        if (s2 > s) s = s2;
        wcsncpy_s(title, 128, s ? s + 1 : image, _TRUNCATE);
        dot = wcsrchr(title, L'.');
        if (dot && dot != title) *dot = 0;
    }
    make_id(title, base_id, 64);
    px = (uint8_t *)malloc((size_t)BG_W * BG_H * 4);
    dds = (uint8_t *)malloc(128 + (size_t)BG_W * BG_H);
    if (!px || !dds) {
        swprintf_s(err, errn, L"Out of memory.");
        goto done;
    }
    if (!read_fitted(image, px, err, errn)) goto done;
    dds_header(dds);
    for (by = 0; by < BG_H / 4; by++)
        for (bx = 0; bx < BG_W / 4; bx++) {
            const uint8_t *b[16];
            int i;
            for (i = 0; i < 16; i++) b[i] = px + ((size_t)(by * 4 + i / 4) * BG_W + bx * 4 + i % 4) * 4;
            dxt5_block(b, dds + 128 + ((size_t)by * (BG_W / 4) + bx) * 16);
        }
    /* a new folder: <id>, <id>_2, ... */
    swprintf_s(path, MAX_PATH, L"%s\\Mods\\Backgrounds", game_dir);
    SHCreateDirectoryExW(NULL, path, NULL);
    for (n = 1; n < 1000; n++) {
        if (n == 1) wcscpy_s(id, 80, base_id);
        else swprintf_s(id, 80, L"%s_%d", base_id, n);
        swprintf_s(dir, MAX_PATH, L"%s\\Mods\\Backgrounds\\%s", game_dir, id);
        if (GetFileAttributesW(dir) == INVALID_FILE_ATTRIBUTES) break;
    }
    if (!CreateDirectoryW(dir, NULL)) {
        swprintf_s(err, errn, L"Could not make the folder %s.", dir);
        goto done;
    }
    swprintf_s(path, MAX_PATH, L"%s\\picture.dds", dir);
    if (_wfopen_s(&f, path, L"wb") || !f || fwrite(dds, 1, 128 + (size_t)BG_W * BG_H, f) != 128 + (size_t)BG_W * BG_H) {
        swprintf_s(err, errn, L"Could not write %s.", path);
        if (f) fclose(f);
        f = NULL;
        goto done;
    }
    fclose(f);
    f = NULL;
    swprintf_s(path, MAX_PATH, L"%s\\manifest.txt", dir);
    {
        char u_id[256], u_name[400];
        WideCharToMultiByte(CP_UTF8, 0, id, -1, u_id, sizeof u_id, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, title, -1, u_name, sizeof u_name, NULL, NULL);
        if (_wfopen_s(&f, path, L"wb") || !f) {
            swprintf_s(err, errn, L"Could not write %s.", path);
            goto done;
        }
        fprintf(f, "type=Background\nid=%s\nname=%s\nversion=1.0\nlogo=%d\n", u_id, u_name, logo ? 1 : 0);
        fclose(f);
        f = NULL;
    }
    if (out_dir) wcscpy_s(out_dir, MAX_PATH, dir);
    ok = 1;
done:
    free(px);
    free(dds);
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}
