/* WWE SmackDown vs. Raw 2011 - where the main menu puts its 1024 x 512
 * background picture, and a picture fitted to it (shared by the launcher's
 * backgrounds.c and the Mod Maker's Backgrounds page; the Android app's
 * Backgrounds.java has the same numbers).
 *
 * Measured with a grid picture (the layout MENU/EXBL draws it in parts): in
 * the game's 1280 x 720 frame a texel (tx, ty) lands at
 *   x = -59.75 + 1.8511 tx            (tx < 320: the left part, stretched)
 *   x = 532.6 + 1.1471 (tx - 320)     (the rest, squeezed)
 *   y = -42.84 + 1.3819 ty
 * so the screen shows tx 32-972, ty 31-512 (down to y 665; the menu's footer
 * is below). The game's own pictures are drawn pre-distorted for it. A
 * player's picture is scaled to cover that visible area (1280 x 665),
 * centred - no squashing; what falls outside is cut - and every texel takes
 * the picture's pixel at the place it lands, so it looks right on screen.
 * Texels off screen repeat the picture's edge.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

enum { BG_W = 1024, BG_H = 512 };

#define BG_X0 (-59.75)
#define BG_XS1 1.8511
#define BG_SEAM 320
#define BG_XS2 1.1471
#define BG_Y0 (-42.84)
#define BG_YS 1.3819
#define BG_VIEW_W 1280.0
#define BG_VIEW_H 664.7 /* (BG_Y0 + 512 BG_YS: the picture's bottom on screen) */

static inline double bg_screen_x(int tx)
{
    return tx < BG_SEAM ? BG_X0 + BG_XS1 * (tx + 0.5) : BG_X0 + BG_XS1 * BG_SEAM + BG_XS2 * (tx + 0.5 - BG_SEAM);
}
static inline double bg_screen_y(int ty) { return BG_Y0 + BG_YS * (ty + 0.5); }

/* The size a w x h picture is scaled to so it covers the visible area. */
static inline void bg_cover_size(unsigned w, unsigned h, unsigned *sw, unsigned *sh)
{
    const double scale = BG_VIEW_W / w > BG_VIEW_H / h ? BG_VIEW_W / w : BG_VIEW_H / h;
    *sw = (unsigned)(w * scale + 0.5);
    *sh = (unsigned)(h * scale + 0.5);
    if (*sw < (unsigned)BG_VIEW_W) *sw = (unsigned)BG_VIEW_W;
    if (*sh < (unsigned)(BG_VIEW_H + 0.5)) *sh = (unsigned)(BG_VIEW_H + 0.5);
}

/* img: the picture already scaled to bg_cover_size (sw x sh, 4 bytes a pixel,
 * any channel order) -> out: the 1024 x 512 texture (the same order), each
 * texel the picture where it lands (bilinear). */
static inline void bg_map(const uint8_t *img, unsigned sw, unsigned sh, uint8_t *out)
{
    const double ox = BG_VIEW_W / 2 - sw / 2.0, oy = BG_VIEW_H / 2 - sh / 2.0;
    int x, y, c;
    for (y = 0; y < BG_H; y++) {
        double fy = bg_screen_y(y) - oy - 0.5, ay;
        int y0, y1;
        if (fy < 0) fy = 0;
        if (fy > sh - 1) fy = sh - 1;
        y0 = (int)fy;
        y1 = y0 + 1 < (int)sh ? y0 + 1 : y0;
        ay = fy - y0;
        for (x = 0; x < BG_W; x++) {
            double fx = bg_screen_x(x) - ox - 0.5, ax;
            int x0, x1;
            const uint8_t *p00, *p01, *p10, *p11;
            if (fx < 0) fx = 0;
            if (fx > sw - 1) fx = sw - 1;
            x0 = (int)fx;
            x1 = x0 + 1 < (int)sw ? x0 + 1 : x0;
            ax = fx - x0;
            p00 = img + ((size_t)y0 * sw + x0) * 4; p01 = img + ((size_t)y0 * sw + x1) * 4;
            p10 = img + ((size_t)y1 * sw + x0) * 4; p11 = img + ((size_t)y1 * sw + x1) * 4;
            for (c = 0; c < 4; c++) {
                const double v = (p00[c] * (1 - ax) + p01[c] * ax) * (1 - ay) + (p10[c] * (1 - ax) + p11[c] * ax) * ay;
                out[((size_t)y * BG_W + x) * 4 + c] = (uint8_t)(v + 0.5);
            }
        }
    }
}
