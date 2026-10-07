/* Latin letters without their accents (á -> a, ñ -> n, ß -> s, Ł -> L), for
 * file names the game can use: USER MOVIES names must be plain ASCII (the
 * game's menus and paths are 8-bit). Shared by the launcher (movie names) and
 * the game (user_movies.cpp: older movies named with accents). The Android
 * launcher has the same table (MoviesPage.java). Plain C. */
#pragma once
#include <wchar.h>

/* The plain letter for c (U+00C0..U+017F), or 0: no plain form. */
static inline wchar_t ascii_fold_char(wchar_t c)
{
    static const char latin1[] =   /* U+00C0..U+00FF */
        "AAAAAAACEEEEIIIIDNOOOOO_OUUUUYTsaaaaaaaceeeeiiiidnooooo_ouuuuyty";
    static const char extended_a[] =   /* U+0100..U+017F */
        "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
    if (c < 0x80) return c;
    if (c >= 0xC0 && c <= 0xFF) return (wchar_t)latin1[c - 0xC0];
    if (c >= 0x100 && c <= 0x17F) return (wchar_t)extended_a[c - 0x100];
    return 0;
}
