/* WWE SmackDown vs. Raw 2011 PC launcher - drawing helpers (see ui_theme.h).
 * GDI+'s flat API, declared here (its headers are C++ only). */
#include "ui_theme.h"

#pragma comment(lib, "gdiplus.lib")

typedef int GpStatus;
typedef struct { UINT32 version; void *debug_callback; BOOL no_background_thread, no_codecs; } GpStartupInput;
typedef void GpGraphics, GpPath, GpBrush, GpPen, GpImage;
typedef struct { INT X, Y; } GpPointI;

GpStatus WINAPI GdiplusStartup(ULONG_PTR *token, const GpStartupInput *input, void *output);
GpStatus WINAPI GdipCreateFromHDC(HDC dc, GpGraphics **g);
GpStatus WINAPI GdipDeleteGraphics(GpGraphics *g);
GpStatus WINAPI GdipSetSmoothingMode(GpGraphics *g, int mode);
GpStatus WINAPI GdipSetPixelOffsetMode(GpGraphics *g, int mode);
GpStatus WINAPI GdipCreatePath(int fill_mode, GpPath **path);
GpStatus WINAPI GdipDeletePath(GpPath *path);
GpStatus WINAPI GdipAddPathArc(GpPath *path, float x, float y, float w, float h, float start, float sweep);
GpStatus WINAPI GdipClosePathFigure(GpPath *path);
GpStatus WINAPI GdipCreateSolidFill(UINT32 argb, GpBrush **brush);
GpStatus WINAPI GdipDeleteBrush(GpBrush *brush);
GpStatus WINAPI GdipFillPath(GpGraphics *g, GpBrush *brush, GpPath *path);
GpStatus WINAPI GdipCreatePen1(UINT32 argb, float width, int unit, GpPen **pen);
GpStatus WINAPI GdipDeletePen(GpPen *pen);
GpStatus WINAPI GdipDrawPath(GpGraphics *g, GpPen *pen, GpPath *path);
GpStatus WINAPI GdipLoadImageFromFile(const WCHAR *file, GpImage **image);
GpStatus WINAPI GdipDisposeImage(GpImage *image);
GpStatus WINAPI GdipGetImageWidth(GpImage *image, UINT *w);
GpStatus WINAPI GdipGetImageHeight(GpImage *image, UINT *h);
GpStatus WINAPI GdipSetInterpolationMode(GpGraphics *g, int mode);
GpStatus WINAPI GdipSetClipPath(GpGraphics *g, GpPath *path, int combine);
GpStatus WINAPI GdipSetClipRectI(GpGraphics *g, INT x, INT y, INT w, INT h, int combine);
GpStatus WINAPI GdipDrawImageRectRectI(GpGraphics *g, GpImage *image, INT dx, INT dy, INT dw, INT dh, INT sx, INT sy,
                                       INT sw, INT sh, int unit, const void *attributes, void *callback, void *data);
GpStatus WINAPI GdipCreateLineBrushI(const GpPointI *p1, const GpPointI *p2, UINT32 c1, UINT32 c2, int wrap,
                                     GpBrush **brush);
GpStatus WINAPI GdipFillRectangleI(GpGraphics *g, GpBrush *brush, INT x, INT y, INT w, INT h);

static int s_gdiplus;

void theme_init(void)
{
    static ULONG_PTR token;
    GpStartupInput in = { 1, NULL, FALSE, FALSE };
    if (!s_gdiplus) s_gdiplus = GdiplusStartup(&token, &in, NULL) == 0 ? 1 : -1;
}

static UINT32 argb(COLORREF c) { return 0xFF000000u | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c); }

static GpPath *rounded(float x, float y, float w, float h, float r)
{
    GpPath *p = NULL;
    const float d = r * 2;
    if (GdipCreatePath(0, &p)) return NULL;
    if (d <= 0) {
        GdipAddPathArc(p, x, y, 0.01f, 0.01f, 180, 90);
    } else {
        GdipAddPathArc(p, x, y, d, d, 180, 90);
        GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
        GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
        GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    }
    GdipClosePathFigure(p);
    return p;
}

void theme_round_rect(HDC dc, const RECT *r, int radius, COLORREF fill, COLORREF border, float border_w)
{
    GpGraphics *g = NULL;
    GpPath *p;
    theme_init();
    if (s_gdiplus < 0 || GdipCreateFromHDC(dc, &g)) {  /* (GDI: no anti-aliasing) */
        HBRUSH b = fill != THEME_NONE ? CreateSolidBrush(fill) : (HBRUSH)GetStockObject(NULL_BRUSH);
        HPEN pen = border != THEME_NONE ? CreatePen(PS_SOLID, (int)border_w, border) : (HPEN)GetStockObject(NULL_PEN);
        HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
        RoundRect(dc, r->left, r->top, r->right, r->bottom, radius * 2, radius * 2);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        if (fill != THEME_NONE) DeleteObject(b);
        if (border != THEME_NONE) DeleteObject(pen);
        return;
    }
    GdipSetSmoothingMode(g, 4 /* anti-alias */);
    GdipSetPixelOffsetMode(g, 4 /* half */);
    if (fill != THEME_NONE) {
        GpBrush *b = NULL;
        p = rounded((float)r->left, (float)r->top, (float)(r->right - r->left), (float)(r->bottom - r->top),
                    (float)radius);
        if (p && !GdipCreateSolidFill(argb(fill), &b)) {
            GdipFillPath(g, b, p);
            GdipDeleteBrush(b);
        }
        if (p) GdipDeletePath(p);
    }
    if (border != THEME_NONE && border_w > 0) {
        GpPen *pen = NULL;
        const float h = border_w / 2;
        p = rounded(r->left + h, r->top + h, r->right - r->left - border_w, r->bottom - r->top - border_w,
                    radius - h > 0 ? radius - h : 0);
        if (p && !GdipCreatePen1(argb(border), border_w, 2 /* pixels */, &pen)) {
            GdipDrawPath(g, pen, p);
            GdipDeletePen(pen);
        }
        if (p) GdipDeletePath(p);
    }
    GdipDeleteGraphics(g);
}

/* The hero picture (theme_hero): loaded once per path. */
static GpImage *s_hero;
static WCHAR s_hero_path[MAX_PATH];
static UINT s_hero_w, s_hero_h;

int theme_hero(HDC dc, const RECT *r, int radius, const WCHAR *image, COLORREF card, int scroll)
{
    GpGraphics *g = NULL;
    GpPath *p;
    int moving = 0;
    theme_init();
    if (s_gdiplus < 0 || !image || !image[0])
        return 0;
    if (wcscmp(image, s_hero_path)) {
        if (s_hero) GdipDisposeImage(s_hero);
        s_hero = NULL;
        s_hero_w = s_hero_h = 0;
        wcsncpy_s(s_hero_path, MAX_PATH, image, _TRUNCATE);
        if (!GdipLoadImageFromFile(image, &s_hero)) {
            GdipGetImageWidth(s_hero, &s_hero_w);
            GdipGetImageHeight(s_hero, &s_hero_h);
        }
    }
    if (!s_hero || !s_hero_w || !s_hero_h || GdipCreateFromHDC(dc, &g))
        return 0;
    GdipSetSmoothingMode(g, 4);
    GdipSetInterpolationMode(g, 7 /* high quality bicubic */);
    p = rounded((float)r->left, (float)r->top, (float)(r->right - r->left), (float)(r->bottom - r->top), (float)radius);
    if (p) GdipSetClipPath(g, p, 0);
    {
        /* The right part of the card: the picture at the card's height. A picture more than twice that
           area's width scrolls (scroll = pixels moved so far; it wraps around). */
        const int h = r->bottom - r->top, area_l = r->left + (r->right - r->left) * 3 / 5, area_w = r->right - area_l;
        int w = (int)((double)s_hero_w * h / s_hero_h), x;
        GdipSetClipRectI(g, area_l, r->top, area_w, h, 1 /* intersect */);
        if (w < area_w) {   /* (narrow: at the area's width, its middle band) */
            const int sh = (int)((double)s_hero_w * h / area_w);
            GdipDrawImageRectRectI(g, s_hero, area_l, r->top, area_w, h, 0, ((int)s_hero_h - sh) / 2, (int)s_hero_w, sh,
                                   2, NULL, NULL, NULL);
        } else if (w > area_w * 2) {
            moving = 1;
            x = area_l - (scroll % w);
            GdipDrawImageRectRectI(g, s_hero, x, r->top, w, h, 0, 0, (int)s_hero_w, (int)s_hero_h, 2, NULL, NULL, NULL);
            if (x + w < r->right)
                GdipDrawImageRectRectI(g, s_hero, x + w, r->top, w, h, 0, 0, (int)s_hero_w, (int)s_hero_h, 2, NULL, NULL,
                                       NULL);
        } else {            /* (its right end against the card's) */
            GdipDrawImageRectRectI(g, s_hero, r->right - w, r->top, w, h, 0, 0, (int)s_hero_w, (int)s_hero_h, 2, NULL,
                                   NULL, NULL);
        }
        /* a little darker (text sits on its left part), faded into the card from the left */
        {
            GpBrush *b = NULL;
            const GpPointI a = { area_l - 1, 0 }, z = { area_l + area_w * 3 / 5, 0 };
            const UINT32 c = argb(card);
            if (!GdipCreateSolidFill(0x48000000u, &b)) {
                GdipFillRectangleI(g, b, area_l, r->top, area_w, h);
                GdipDeleteBrush(b);
            }
            b = NULL;
            if (!GdipCreateLineBrushI(&a, &z, c, c & 0x00FFFFFFu, 0, &b)) {
                GdipFillRectangleI(g, b, area_l, r->top, area_w * 3 / 5 + 1, h);
                GdipDeleteBrush(b);
            }
        }
    }
    if (p) GdipDeletePath(p);
    GdipDeleteGraphics(g);
    return moving ? 1 : 0;
}
