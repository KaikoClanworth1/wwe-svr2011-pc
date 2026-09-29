/* Bink 1 video reader (bink_decode.c): the game's titantron movies and the
 * ones movie_maker.c writes. LGPL 2.1+ (ported from FFmpeg). */
#pragma once
#include <windows.h>
#include <stdint.h>

typedef struct BinkReader BinkReader;

/* Opens a .bik; NULL if it isn't a Bink 1 video (err says why). */
BinkReader *bink_open(const WCHAR *path, WCHAR *err, size_t errn);
void bink_close(BinkReader *r);

int bink_width(const BinkReader *r);
int bink_height(const BinkReader *r);
int bink_frames(const BinkReader *r);
/* Frame duration in 100 ns units. */
LONGLONG bink_frame_time(const BinkReader *r);

/* Decodes the next frame (after the last one: the first again). 0 on a
 * damaged frame. */
int bink_next(BinkReader *r);
/* The decoded frame as BGRA (width x height x 4). */
void bink_bgra(const BinkReader *r, uint8_t *bgra);
/* Back to before the first frame. */
void bink_rewind(BinkReader *r);
/* Frames decoded since opening (or since it started over). */
int bink_position(const BinkReader *r);
