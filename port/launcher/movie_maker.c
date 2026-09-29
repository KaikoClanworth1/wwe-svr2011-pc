/* WWE SmackDown vs. Raw 2011 - PC port launcher: USER MOVIES maker.
 *
 * Makes an entrance movie the game plays like its own titantron movies
 * (movies\titantron\<id>.bik): a Bink 1 ("BIKi") file, 320 x 320, 30 fps,
 * no audio. The frame is two pictures: the top 320 x 220 is the big screen
 * (shown at 16:9, so it is stored squeezed), the bottom 320 x 100 is the
 * strip for the stage and ramp screens (black if there is none).
 *
 * The Bink bitstream uses three of Bink's block types: raw 8 x 8 blocks,
 * fill blocks (one colour) and skip blocks (unchanged since the last frame),
 * with fixed 4-bit codes (tree 0) - see ffmpeg's libavcodec/bink.c for the
 * format. Pictures come from Media Foundation (videos) or WIC (images). */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <propvarutil.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "movie_maker.h"

#define FPS 30
#define TOP_H 220
#define BOT_H (MOVIE_H - TOP_H)

/* ── bits (least significant first, as Bink reads them) ── */

typedef struct { uint8_t *p; size_t n, cap; uint64_t acc; int bits; } Bits;

static int bits_put(Bits *b, uint32_t v, int n)
{
    b->acc |= (uint64_t)(v & ((1u << n) - 1)) << b->bits;
    b->bits += n;
    while (b->bits >= 8) {
        if (b->n == b->cap) {
            size_t cap = b->cap ? b->cap * 2 : 65536;
            uint8_t *p = (uint8_t *)realloc(b->p, cap);
            if (!p) return 0;
            b->p = p; b->cap = cap;
        }
        b->p[b->n++] = (uint8_t)b->acc;
        b->acc >>= 8;
        b->bits -= 8;
    }
    return 1;
}

static void bits_align32(Bits *b)
{
    int pad = (int)((32 - ((b->n * 8 + b->bits) & 31)) & 31);
    while (pad > 0) { int k = pad > 16 ? 16 : pad; bits_put(b, 0, k); pad -= k; }
}

static int ilog2(unsigned v) { int r = -1; while (v) { v >>= 1; r++; } return r; }

/* ── one plane of a Bink frame ── */

enum { BLK_SKIP = 0, BLK_FILL = 6, BLK_RAW = 9 };

/* Encodes plane `cur` (w x h, w and h multiples of 8) against `ref` (the
 * picture the decoder has; updated to what it will have after this frame). */
static int encode_plane(Bits *b, const uint8_t *cur, uint8_t *ref, int w, int h, int key, int skip_thr, int fill_thr)
{
    const int bw = w >> 3, bh = h >> 3;
    const int width = w < 8 ? 8 : w;
    const int len_types = ilog2((unsigned)(width >> 3) + 511) + 1;
    const int len_sub = ilog2((unsigned)(width >> 4) + 511) + 1;
    const int len_colors = ilog2((unsigned)bw * 64 + 511) + 1;
    const int len_pattern = ilog2(((unsigned)bw << 3) + 511) + 1;
    const int len_small = ilog2((unsigned)(width >> 3) + 511) + 1;   /* x, y, intra dc, inter dc */
    const int len_run = ilog2((unsigned)bw * 48 + 511) + 1;
    uint8_t *types = (uint8_t *)malloc((size_t)bw * bh);
    uint8_t *vals = (uint8_t *)malloc((size_t)w * h);           /* colour values, block order */
    size_t *row_start = (size_t *)malloc(sizeof(size_t) * ((size_t)bh + 1));
    size_t nv = 0, supplied = 0, consumed = 0;
    int by, bx, x, y, i, next_row = 0, colors_null = 0, first = 1;
    if (!types || !vals || !row_start) { free(types); free(vals); free(row_start); return 0; }

    for (by = 0; by < bh; by++) {
        row_start[by] = nv;
        for (bx = 0; bx < bw; bx++) {
            int mn = 255, mx = 0, diff = 0, sum = 0, t;
            for (y = 0; y < 8; y++) {
                const uint8_t *c = cur + (size_t)(by * 8 + y) * w + bx * 8;
                const uint8_t *r = ref + (size_t)(by * 8 + y) * w + bx * 8;
                for (x = 0; x < 8; x++) {
                    int d = c[x] - r[x];
                    if (d < 0) d = -d;
                    if (d > diff) diff = d;
                    if (c[x] < mn) mn = c[x];
                    if (c[x] > mx) mx = c[x];
                    sum += c[x];
                }
            }
            if (!key && diff <= skip_thr) {
                t = BLK_SKIP;
            } else if (mx - mn <= fill_thr) {
                uint8_t v = (uint8_t)((sum + 32) >> 6);
                t = BLK_FILL;
                vals[nv++] = v;
                for (y = 0; y < 8; y++) memset(ref + (size_t)(by * 8 + y) * w + bx * 8, v, 8);
            } else {
                t = BLK_RAW;
                for (y = 0; y < 8; y++) {
                    const uint8_t *c = cur + (size_t)(by * 8 + y) * w + bx * 8;
                    memcpy(vals + nv, c, 8);
                    memcpy(ref + (size_t)(by * 8 + y) * w + bx * 8, c, 8);
                    nv += 8;
                }
            }
            types[by * bw + bx] = (uint8_t)t;
        }
    }
    row_start[bh] = nv;

    /* bundle trees: all "tree 0" (4-bit codes); colours have 16 more */
    bits_put(b, 0, 4);                                  /* block types */
    bits_put(b, 0, 4);                                  /* sub-block types */
    for (i = 0; i < 16; i++) bits_put(b, 0, 4);         /* colour high nibbles */
    bits_put(b, 0, 4);                                  /* colours */
    bits_put(b, 0, 4);                                  /* pattern */
    bits_put(b, 0, 4);                                  /* x offsets */
    bits_put(b, 0, 4);                                  /* y offsets */
    bits_put(b, 0, 4);                                  /* runs (the dc bundles have none) */

    for (by = 0; by < bh; by++) {
        const uint8_t *t = types + by * bw;
        int same = 1;
        for (bx = 1; bx < bw; bx++) same &= t[bx] == t[0];
        bits_put(b, (uint32_t)bw, len_types);
        if (same) {
            bits_put(b, 1, 1);
            bits_put(b, t[0], 4);
        } else {
            bits_put(b, 0, 1);
            for (bx = 0; bx < bw; bx++) bits_put(b, t[bx], 4);
        }
        if (first) bits_put(b, 0, len_sub);
        /* colours: the decoder reads a batch only once it has used the last */
        if (!colors_null && supplied == consumed) {
            int r = next_row > by ? next_row : by;
            while (r < bh && row_start[r + 1] == row_start[r]) r++;
            if (r == bh) {
                bits_put(b, 0, len_colors);
                colors_null = 1;
            } else {
                size_t k, n = row_start[r + 1] - row_start[r];
                bits_put(b, (uint32_t)n, len_colors);
                bits_put(b, 0, 1);
                for (k = row_start[r]; k < row_start[r + 1]; k++) {
                    bits_put(b, vals[k] >> 4, 4);
                    bits_put(b, vals[k] & 15, 4);
                }
                supplied += n;
                next_row = r + 1;
            }
        }
        if (first) {
            bits_put(b, 0, len_pattern);
            bits_put(b, 0, len_small);   /* x */
            bits_put(b, 0, len_small);   /* y */
            bits_put(b, 0, len_small);   /* intra dc */
            bits_put(b, 0, len_small);   /* inter dc */
            bits_put(b, 0, len_run);
            first = 0;
        }
        consumed += row_start[by + 1] - row_start[by];
    }
    bits_align32(b);
    free(types); free(vals); free(row_start);
    return 1;
}

/* ── pictures ── */

typedef struct {
    int kind;                  /* 0 black, 1 image, 2 video */
    int w, h;                  /* picture size */
    double par;                /* pixel aspect ratio */
    uint8_t *bgra;             /* current picture */
    IMFSourceReader *rd;
    LONG stride;
    int fw, fh;                /* decoded frame layout */
    int ax, ay;                /* visible area offset */
    LONGLONG duration, base;   /* 100 ns; base = time offset of the current loop */
    int have_next, eof;
    LONGLONG next_ts;
    IMFSample *next;
    int cx, cy, cw, ch;        /* the picture without black borders (content_find) */
} Pic;

static int is_image(const WCHAR *path)
{
    static const WCHAR *ext[] = { L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff", L".webp", L".jxr" };
    const WCHAR *dot = wcsrchr(path, L'.');
    size_t i;
    if (!dot) return 0;
    for (i = 0; i < sizeof ext / sizeof *ext; i++)
        if (!_wcsicmp(dot, ext[i])) return 1;
    return 0;
}

static int image_open(Pic *p, const WCHAR *path, WCHAR *err, size_t errn)
{
    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0, h = 0;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac))
            || FAILED(IWICImagingFactory_CreateDecoderFromFilename(fac, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))
            || FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &frame))
            || FAILED(IWICImagingFactory_CreateFormatConverter(fac, &conv))
            || FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                                     WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))
            || FAILED(IWICFormatConverter_GetSize(conv, &w, &h)) || !w || !h || w > 16384 || h > 16384) {
        swprintf_s(err, errn, L"Could not read %s as an image.", path);
        goto done;
    }
    p->bgra = (uint8_t *)malloc((size_t)w * h * 4);
    if (!p->bgra || FAILED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, p->bgra))) {
        swprintf_s(err, errn, L"Could not read %s.", path);
        goto done;
    }
    p->kind = 1; p->w = (int)w; p->h = (int)h; p->par = 1.0;
    ok = 1;
done:
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (fac) IWICImagingFactory_Release(fac);
    return ok;
}

static int video_open(Pic *p, const WCHAR *path, WCHAR *err, size_t errn)
{
    IMFAttributes *attr = NULL;
    IMFMediaType *mt = NULL;
    PROPVARIANT pv;
    UINT32 w = 0, h = 0, pn = 1, pd = 1, stride = 0;
    UINT64 packed = 0;
    MFVideoArea area;
    int ok = 0;
    if (FAILED(MFCreateAttributes(&attr, 2))
            || FAILED(IMFAttributes_SetUINT32(attr, &MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE))
            || FAILED(MFCreateSourceReaderFromURL(path, attr, &p->rd))) {
        swprintf_s(err, errn, L"Could not open %s as a video (Windows can't play it).", path);
        goto done;
    }
    IMFSourceReader_SetStreamSelection(p->rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(p->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    if (FAILED(MFCreateMediaType(&mt))
            || FAILED(IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video))
            || FAILED(IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_RGB32))
            || FAILED(IMFSourceReader_SetCurrentMediaType(p->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, mt))) {
        swprintf_s(err, errn, L"%s has no video Windows can decode.", path);
        goto done;
    }
    IMFMediaType_Release(mt); mt = NULL;
    if (FAILED(IMFSourceReader_GetCurrentMediaType(p->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt))
            || FAILED(IMFMediaType_GetUINT64(mt, &MF_MT_FRAME_SIZE, &packed))
            || !(w = (UINT32)(packed >> 32)) || !(h = (UINT32)packed)) {
        swprintf_s(err, errn, L"%s: unknown picture size.", path);
        goto done;
    }
    p->fw = (int)w; p->fh = (int)h;
    p->w = (int)w; p->h = (int)h; p->ax = p->ay = 0;
    if (SUCCEEDED(IMFMediaType_GetBlob(mt, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof area, NULL))
            && area.Area.cx > 0 && area.Area.cy > 0 && area.Area.cx <= (LONG)w && area.Area.cy <= (LONG)h) {
        p->ax = area.OffsetX.value; p->ay = area.OffsetY.value;
        p->w = area.Area.cx; p->h = area.Area.cy;
    }
    p->stride = (LONG)w * 4;
    if (SUCCEEDED(IMFMediaType_GetUINT32(mt, &MF_MT_DEFAULT_STRIDE, &stride))) p->stride = (LONG)stride;
    p->par = 1.0;
    if (SUCCEEDED(IMFMediaType_GetUINT64(mt, &MF_MT_PIXEL_ASPECT_RATIO, &packed))) {
        pn = (UINT32)(packed >> 32); pd = (UINT32)packed;
        if (pn && pd) p->par = (double)pn / pd;
    }
    PropVariantInit(&pv);
    p->duration = 0;
    if (SUCCEEDED(IMFSourceReader_GetPresentationAttribute(p->rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &pv)))
        p->duration = (LONGLONG)pv.uhVal.QuadPart;
    PropVariantClear(&pv);
    p->bgra = (uint8_t *)calloc((size_t)p->w * p->h, 4);
    if (!p->bgra) goto done;
    p->kind = 2;
    ok = 1;
done:
    if (mt) IMFMediaType_Release(mt);
    if (attr) IMFAttributes_Release(attr);
    return ok;
}

static void pic_close(Pic *p)
{
    if (p->next) IMFSample_Release(p->next);
    if (p->rd) IMFSourceReader_Release(p->rd);
    free(p->bgra);
    memset(p, 0, sizeof *p);
}

static void content_find(Pic *p);

static int pic_open(Pic *p, const WCHAR *path, WCHAR *err, size_t errn)
{
    memset(p, 0, sizeof *p);
    if (!path || !path[0]) return 1;            /* black */
    if (!(is_image(path) ? image_open(p, path, err, errn) : video_open(p, path, err, errn))) return 0;
    content_find(p);
    return 1;
}

static void sample_copy(Pic *p, IMFSample *s)
{
    IMFMediaBuffer *buf = NULL;
    BYTE *data = NULL;
    DWORD len = 0;
    int y;
    if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &buf))) return;
    if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &data, NULL, &len))) {
        LONG stride = p->stride;
        const BYTE *row0 = stride < 0 ? data + (size_t)(-stride) * (p->fh - 1) : data;
        if ((size_t)(stride < 0 ? -stride : stride) * p->fh <= len) {
            for (y = 0; y < p->h; y++) {
                const BYTE *src = row0 + (LONG_PTR)stride * (y + p->ay) + (size_t)p->ax * 4;
                memcpy(p->bgra + (size_t)y * p->w * 4, src, (size_t)p->w * 4);
            }
        }
        IMFMediaBuffer_Unlock(buf);
    }
    IMFMediaBuffer_Release(buf);
}

/* Brings the picture to time t (100 ns). A video that has ended keeps its
 * last frame, or starts again if `loop`. */
static void pic_seek(Pic *p, LONGLONG t, int loop)
{
    if (p->kind != 2) return;
    for (;;) {
        if (!p->have_next && !p->eof) {
            DWORD idx, flags = 0;
            LONGLONG ts = 0;
            IMFSample *s = NULL;
            if (FAILED(IMFSourceReader_ReadSample(p->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s))
                    || (flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
                if (s) IMFSample_Release(s);
                if (loop && p->duration > 0 && p->base + p->duration <= t) {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    pv.vt = VT_I8;
                    pv.hVal.QuadPart = 0;
                    IMFSourceReader_SetCurrentPosition(p->rd, &GUID_NULL, &pv);
                    p->base += p->duration;
                    continue;
                }
                p->eof = 1;
                break;
            }
            if (!s) continue;                    /* a gap, a format change */
            p->next = s;
            p->next_ts = p->base + ts;
            p->have_next = 1;
        }
        if (!p->have_next || p->next_ts > t) break;
        sample_copy(p, p->next);
        IMFSample_Release(p->next);
        p->next = NULL;
        p->have_next = 0;
    }
}

/* Grows box (x0, y0, x1, y1) to the parts of the current picture that are
 * not black: rows and columns where enough pixels are brighter than dark
 * grey (so noise or a small logo in a black bar doesn't count). */
static void content_add(const Pic *p, int *x0, int *y0, int *x1, int *y1)
{
    int x, y, n;
    const int step = 2;
    for (y = 0; y < p->h; y += step) {
        for (x = 0, n = 0; x < p->w; x += step) {
            const uint8_t *s = p->bgra + ((size_t)y * p->w + x) * 4;
            if (s[0] + s[1] + s[2] > 3 * 32) n++;
        }
        if (n * step * 50 > p->w) { if (y < *y0) *y0 = y; if (y + step > *y1) *y1 = y + step; }
    }
    for (x = 0; x < p->w; x += step) {
        for (y = 0, n = 0; y < p->h; y += step) {
            const uint8_t *s = p->bgra + ((size_t)y * p->w + x) * 4;
            if (s[0] + s[1] + s[2] > 3 * 32) n++;
        }
        if (n * step * 50 > p->h) { if (x < *x0) *x0 = x; if (x + step > *x1) *x1 = x + step; }
    }
}

/* Black borders that are part of the picture (letterboxed or pillarboxed
 * videos): found on frames across the whole video, then cut off - so
 * "Fill" fills the screen with the picture, not with its black bars. */
static void content_find(Pic *p)
{
    int x0 = p->w, y0 = p->h, x1 = 0, y1 = 0, k;
    p->cx = 0; p->cy = 0; p->cw = p->w; p->ch = p->h;
    if (p->kind == 1) {
        content_add(p, &x0, &y0, &x1, &y1);
    } else if (p->kind == 2 && p->duration > 0) {
        for (k = 1; k <= 8; k++) {
            PROPVARIANT pv;
            DWORD idx, flags = 0;
            LONGLONG ts = 0;
            IMFSample *s = NULL;
            PropVariantInit(&pv);
            pv.vt = VT_I8;
            pv.hVal.QuadPart = p->duration * k / 10;
            if (FAILED(IMFSourceReader_SetCurrentPosition(p->rd, &GUID_NULL, &pv))) break;
            if (FAILED(IMFSourceReader_ReadSample(p->rd, (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &flags, &ts, &s))
                    || !s) { if (s) IMFSample_Release(s); continue; }
            sample_copy(p, s);
            IMFSample_Release(s);
            content_add(p, &x0, &y0, &x1, &y1);
        }
        {   /* back to the start */
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_I8;
            pv.hVal.QuadPart = 0;
            IMFSourceReader_SetCurrentPosition(p->rd, &GUID_NULL, &pv);
            memset(p->bgra, 0, (size_t)p->w * p->h * 4);
        }
    } else {
        return;
    }
    if (x1 > p->w) x1 = p->w;
    if (y1 > p->h) y1 = p->h;
    /* Only clear borders: 1.5% or more of a side, and a real picture left. */
    if (x1 - x0 < p->w / 4 || y1 - y0 < p->h / 4) return;
    if (x0 * 1000 < p->w * 15) x0 = 0;
    if ((p->w - x1) * 1000 < p->w * 15) x1 = p->w;
    if (y0 * 1000 < p->h * 15) y0 = 0;
    if ((p->h - y1) * 1000 < p->h * 15) y1 = p->h;
    p->cx = x0; p->cy = y0; p->cw = x1 - x0; p->ch = y1 - y0;
}

/* Box-filtered copy of source rectangle (sx, sy, sw, sh) of `p` into
 * destination rectangle (dx, dy, dw, dh) of a BGRA picture `dst` (dst_w wide). */
static void pic_draw(const Pic *p, double sx, double sy, double sw, double sh,
                     uint8_t *dst, int dst_w, int dx, int dy, int dw, int dh)
{
    int x, y;
    if (!p->bgra || dw <= 0 || dh <= 0) return;
    for (y = 0; y < dh; y++) {
        double y0 = sy + sh * y / dh, y1 = sy + sh * (y + 1) / dh;
        int iy0 = (int)y0, iy1 = (int)(y1 + 0.999);
        if (iy0 < 0) iy0 = 0;
        if (iy1 > p->h) iy1 = p->h;
        if (iy1 <= iy0) iy1 = iy0 + 1;
        for (x = 0; x < dw; x++) {
            double x0 = sx + sw * x / dw, x1 = sx + sw * (x + 1) / dw;
            int ix0 = (int)x0, ix1 = (int)(x1 + 0.999), i, j, n = 0;
            unsigned acc[3] = { 0, 0, 0 };
            uint8_t *o = dst + ((size_t)(dy + y) * dst_w + dx + x) * 4;
            if (ix0 < 0) ix0 = 0;
            if (ix1 > p->w) ix1 = p->w;
            if (ix1 <= ix0) ix1 = ix0 + 1;
            for (j = iy0; j < iy1 && j < p->h; j++)
                for (i = ix0; i < ix1 && i < p->w; i++) {
                    const uint8_t *s = p->bgra + ((size_t)j * p->w + i) * 4;
                    acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2];
                    n++;
                }
            if (n) { o[0] = (uint8_t)(acc[0] / n); o[1] = (uint8_t)(acc[1] / n); o[2] = (uint8_t)(acc[2] / n); }
            o[3] = 255;
        }
    }
}

/* The top picture: the big screen, shown at 16:9, stored as 320 x 220. */
static void draw_top(const Pic *p, int fit, uint8_t *frame)
{
    const double screen = 16.0 / 9.0;
    double aspect, sx = 0, sy = 0, sw, sh;
    int dx = 0, dy = 0, dw = MOVIE_W, dh = TOP_H;
    if (!p->bgra) return;
    sx = p->cx; sy = p->cy; sw = p->cw; sh = p->ch;
    aspect = sw * p->par / sh;
    if (fit == MOVIE_FILL) {
        if (aspect > screen) { sw = sh * screen / p->par; sx = p->cx + (p->cw - sw) / 2; }
        else                 { sh = sw * p->par / screen; sy = p->cy + (p->ch - sh) / 2; }
    } else if (fit == MOVIE_FIT) {
        if (aspect > screen) { dh = (int)(TOP_H * screen / aspect + 0.5); dy = (TOP_H - dh) / 2; }
        else                 { dw = (int)(MOVIE_W * aspect / screen + 0.5); dx = (MOVIE_W - dw) / 2; }
    }
    pic_draw(p, sx, sy, sw, sh, frame, MOVIE_W, dx, dy, dw, dh);
}

/* The bottom strip: 320 x 100, filled (cropped to its shape). */
static void draw_bottom(const Pic *p, uint8_t *frame)
{
    const double strip = (double)MOVIE_W / BOT_H;
    double aspect, sx = 0, sy = 0, sw, sh;
    if (!p->bgra) return;
    sx = p->cx; sy = p->cy; sw = p->cw; sh = p->ch;
    aspect = sw * p->par / sh;
    if (aspect > strip) { sw = sh * strip / p->par; sx = p->cx + (p->cw - sw) / 2; }
    else                { sh = sw * p->par / strip; sy = p->cy + (p->ch - sh) / 2; }
    pic_draw(p, sx, sy, sw, sh, frame, MOVIE_W, 0, TOP_H, MOVIE_W, BOT_H);
}

static void compose(Pic *top, Pic *bottom, int fit, LONGLONG t, uint8_t *frame)
{
    size_t i;
    for (i = 0; i < (size_t)MOVIE_W * MOVIE_H; i++) {
        frame[i * 4 + 0] = frame[i * 4 + 1] = frame[i * 4 + 2] = 0;
        frame[i * 4 + 3] = 255;
    }
    pic_seek(top, t, 0);
    pic_seek(bottom, t, 1);
    draw_top(top, fit, frame);
    draw_bottom(bottom, frame);
}

/* BGRA -> Y, U, V (4:2:0, BT.601 studio range as Bink's decoder expects). */
static void to_yuv(const uint8_t *bgra, uint8_t *Y, uint8_t *U, uint8_t *V)
{
    int x, y;
    for (y = 0; y < MOVIE_H; y++)
        for (x = 0; x < MOVIE_W; x++) {
            const uint8_t *s = bgra + ((size_t)y * MOVIE_W + x) * 4;
            Y[y * MOVIE_W + x] = (uint8_t)((66 * s[2] + 129 * s[1] + 25 * s[0] + 128) / 256 + 16);
        }
    for (y = 0; y < MOVIE_H / 2; y++)
        for (x = 0; x < MOVIE_W / 2; x++) {
            int r = 0, g = 0, b = 0, i, j;
            for (j = 0; j < 2; j++)
                for (i = 0; i < 2; i++) {
                    const uint8_t *s = bgra + ((size_t)(y * 2 + j) * MOVIE_W + x * 2 + i) * 4;
                    b += s[0]; g += s[1]; r += s[2];
                }
            r /= 4; g /= 4; b /= 4;
            U[y * (MOVIE_W / 2) + x] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
            V[y * (MOVIE_W / 2) + x] = (uint8_t)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
        }
}

static int mf_start(void)
{
    static LONG started;
    if (InterlockedCompareExchange(&started, 1, 0) == 0)
        return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    return 1;
}

int movie_preview(const WCHAR *video, const WCHAR *bottom, int fit, double seconds, uint8_t *bgra,
                  WCHAR *err, size_t errn)
{
    Pic top, bot;
    int ok;
    mf_start();
    if (!pic_open(&top, video, err, errn)) return 0;
    if (!pic_open(&bot, bottom, err, errn)) { pic_close(&top); return 0; }
    compose(&top, &bot, fit, (LONGLONG)(seconds * 1e7), bgra);
    ok = 1;
    pic_close(&top);
    pic_close(&bot);
    return ok;
}

int movie_make(MovieJob *job)
{
    Pic top, bot;
    FILE *f = NULL;
    uint8_t *frame = NULL, *Y = NULL, *U = NULL, *V = NULL, *rY = NULL, *rU = NULL, *rV = NULL;
    uint32_t *offs = NULL, largest = 0;
    LONGLONG length;
    int n, i, ok = 0;
    WCHAR tmp[MAX_PATH + 8];
    Bits b;
    memset(&b, 0, sizeof b);
    job->err[0] = 0;
    mf_start();
    if (!pic_open(&top, job->video, job->err, sizeof job->err / sizeof *job->err)) return 0;
    if (top.kind == 0) { wcscpy_s(job->err, sizeof job->err / sizeof *job->err, L"Choose the video."); return 0; }
    if (!pic_open(&bot, job->bottom, job->err, sizeof job->err / sizeof *job->err)) { pic_close(&top); return 0; }
    length = top.kind == 2 && top.duration > 0 ? top.duration : 10 * 10000000LL;
    if (job->max_seconds > 0 && length > (LONGLONG)job->max_seconds * 10000000LL)
        length = (LONGLONG)job->max_seconds * 10000000LL;
    n = (int)((length * FPS + 9999999) / 10000000);
    if (n < 1) n = 1;
    frame = (uint8_t *)malloc((size_t)MOVIE_W * MOVIE_H * 4);
    Y = (uint8_t *)malloc(MOVIE_W * MOVIE_H); rY = (uint8_t *)calloc(MOVIE_W * MOVIE_H, 1);
    U = (uint8_t *)malloc(MOVIE_W * MOVIE_H / 4); rU = (uint8_t *)calloc(MOVIE_W * MOVIE_H / 4, 1);
    V = (uint8_t *)malloc(MOVIE_W * MOVIE_H / 4); rV = (uint8_t *)calloc(MOVIE_W * MOVIE_H / 4, 1);
    offs = (uint32_t *)malloc(sizeof(uint32_t) * ((size_t)n + 1));
    swprintf_s(tmp, sizeof tmp / sizeof *tmp, L"%s.part", job->out);
    if (!frame || !Y || !U || !V || !rY || !rU || !rV || !offs || _wfopen_s(&f, tmp, L"wb") || !f) {
        wcscpy_s(job->err, sizeof job->err / sizeof *job->err, L"Could not create the movie file.");
        goto done;
    }
    {   /* header and frame index, filled in at the end */
        uint8_t zero[64] = { 0 };
        size_t k, head = 44 + 4 * ((size_t)n + 1);
        for (k = 0; k < head; k += sizeof zero) fwrite(zero, 1, head - k < sizeof zero ? head - k : sizeof zero, f);
    }
    for (i = 0; i < n; i++) {
        size_t pos, ysize;
        int key = i == 0;
        if (job->cancel) { wcscpy_s(job->err, sizeof job->err / sizeof *job->err, L"Stopped."); goto done; }
        compose(&top, &bot, job->fit, (LONGLONG)i * 10000000LL / FPS, frame);
        to_yuv(frame, Y, U, V);
        b.n = 0; b.bits = 0; b.acc = 0;
        bits_put(&b, 0, 32);                                          /* size of the Y plane, below */
        if (!encode_plane(&b, Y, rY, MOVIE_W, MOVIE_H, key, 2, 2)) goto nomem;
        ysize = b.n;
        if (!encode_plane(&b, V, rV, MOVIE_W / 2, MOVIE_H / 2, key, 2, 2)) goto nomem;   /* Bink 'i': V before U */
        if (!encode_plane(&b, U, rU, MOVIE_W / 2, MOVIE_H / 2, key, 2, 2)) goto nomem;
        b.p[0] = (uint8_t)ysize; b.p[1] = (uint8_t)(ysize >> 8); b.p[2] = (uint8_t)(ysize >> 16); b.p[3] = (uint8_t)(ysize >> 24);
        pos = (size_t)_ftelli64(f);
        offs[i] = (uint32_t)pos | (key ? 1u : 0u);
        if (fwrite(b.p, 1, b.n, f) != b.n) { wcscpy_s(job->err, sizeof job->err / sizeof *job->err, L"Could not write the movie (disk full?)."); goto done; }
        if (b.n > largest) largest = (uint32_t)b.n;
        if (job->notify) PostMessageW(job->notify, job->msg, (WPARAM)((i + 1) * 100 / n), 0);
    }
    offs[n] = (uint32_t)_ftelli64(f);
    {
        uint32_t hdr[11];
        memcpy(hdr, "BIKi", 4);
        hdr[1] = offs[n] - 8; hdr[2] = (uint32_t)n; hdr[3] = largest; hdr[4] = (uint32_t)n;
        hdr[5] = MOVIE_W; hdr[6] = MOVIE_H; hdr[7] = FPS; hdr[8] = 1; hdr[9] = 0; hdr[10] = 0;
        _fseeki64(f, 0, SEEK_SET);
        fwrite(hdr, 4, 11, f);
        fwrite(offs, 4, (size_t)n + 1, f);
    }
    fclose(f); f = NULL;
    if (!MoveFileExW(tmp, job->out, MOVEFILE_REPLACE_EXISTING)) {
        wcscpy_s(job->err, sizeof job->err / sizeof *job->err, L"Could not save the movie file (is the game using it?).");
        goto done;
    }
    job->frames = n;
    ok = 1;
    goto done;
nomem:
    wcscpy_s(job->err, sizeof job->err / sizeof *job->err, L"Out of memory.");
done:
    if (f) { fclose(f); DeleteFileW(tmp); }
    free(b.p); free(frame); free(Y); free(U); free(V); free(rY); free(rU); free(rV); free(offs);
    pic_close(&top);
    pic_close(&bot);
    return ok;
}
