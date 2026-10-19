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
/* The Play page's hero picture: `image` (any picture file; loaded once) on the right part of the card `r`,
   at its height, faded into `card` from the left and clipped to its rounded corners. A picture more than
   twice as wide as that part scrolls: drawn `scroll` pixels along (wrapping). 1 when it scrolls (keep
   calling with a growing `scroll`), 0 otherwise or without a picture. */
int theme_hero(HDC dc, const RECT *r, int radius, const WCHAR *image, COLORREF card, int scroll);
