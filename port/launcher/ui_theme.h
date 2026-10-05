/* WWE SmackDown vs. Raw 2011 PC launcher - drawing helpers for its look
 * (ui_theme.c): anti-aliased rounded rectangles through GDI+. */
#pragma once

#include <windows.h>

#define THEME_NONE ((COLORREF)0xFFFFFFFF) /* (no border / no fill) */

/* GDI+ started (once; the GDI fallback draws square-ish corners without it). */
void theme_init(void);
/* A rounded rectangle: filled with `fill`, outlined with `border` (width in
   pixels), corner radius in pixels. */
void theme_round_rect(HDC dc, const RECT *r, int radius, COLORREF fill, COLORREF border, float border_w);
