/* Bink 1 ("BIKi") encoder for USER MOVIES (bink_encode.c): 320 x 320,
 * 30 fps, no audio, laid out like the game's titantron movies. Plain C. */
#pragma once
#include <stdint.h>
#include <stdio.h>

#define BINK_ENC_W 320
#define BINK_ENC_H 320
#define BINK_ENC_FPS 30

typedef struct BinkWriter BinkWriter;

/* Starts a movie of `frames` frames in `f` (opened "wb"; the header and the
 * frame index are written by bink_writer_close). NULL: out of memory. */
BinkWriter *bink_writer_open(FILE *f, int frames);
/* Encodes the next frame (320 x 320 BGRA): 1, 0 out of memory, -1 write failed. */
int bink_writer_frame(BinkWriter *w, const uint8_t *bgra);
/* Writes the header and the frame index, and frees the writer. */
int bink_writer_close(BinkWriter *w);
/* Frees a writer that won't be finished. */
void bink_writer_free(BinkWriter *w);
