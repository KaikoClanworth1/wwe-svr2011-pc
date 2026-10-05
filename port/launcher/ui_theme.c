/* WWE SmackDown vs. Raw 2011 PC launcher - drawing helpers (see ui_theme.h).
 * GDI+'s flat API, declared here (its headers are C++ only). */
#include "ui_theme.h"

#pragma comment(lib, "gdiplus.lib")

typedef int GpStatus;
typedef struct { UINT32 version; void *debug_callback; BOOL no_background_thread, no_codecs; } GpStartupInput;
typedef void GpGraphics, GpPath, GpBrush, GpPen;

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
