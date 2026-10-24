/* USER MOVIES maker (movie_maker.c): entrance movies for the game's Custom
 * Movies folder, laid out like the game's titantron movies: an .mp4 (the game
 * plays it as an HD movie) or a Bink file. */
#pragma once
#include <windows.h>
#include <stdint.h>

#define MOVIE_W 320
#define MOVIE_H 320

enum { MOVIE_FIT = 0,      /* whole picture, black bars */
       MOVIE_FILL = 1,     /* fills the screen, edges cut off */
       MOVIE_STRETCH = 2 };

typedef struct MovieJob {
    WCHAR video[MAX_PATH];     /* the big-screen video (or a picture) */
    WCHAR bottom[MAX_PATH];    /* the strip below it: picture or video; "" = black */
    int fit;                   /* MOVIE_FIT / MOVIE_FILL / MOVIE_STRETCH */
    int max_seconds;           /* 0 = the whole video */
    int size;                  /* the movie's size: 0 / 320 (the game's own), 640 or 1024 (HD: the same
                                  layout, the big screen the top size*220/320 rows) */
    WCHAR out[MAX_PATH];       /* the .bik to write, or an .mp4 (H.264; where Windows can't encode it,
                                  the .bik of that name instead: out is changed and wrote_bik set) */
    int wrote_bik;             /* out: an .mp4 was asked for, the .bik made */
    volatile LONG cancel;
    HWND notify;               /* gets `msg` with the percentage done */
    UINT msg;
    int frames;                /* out: frames written */
    WCHAR err[512];            /* out: why it failed */
} MovieJob;

/* Writes job->out; 1 on success. Call from a worker thread (COM initialised). */
int movie_make(MovieJob *job);

/* The movie's frame at `seconds` as 320 x 320 BGRA (for a preview; any size
 * job looks the same, sharper). */
int movie_preview(const WCHAR *video, const WCHAR *bottom, int fit, double seconds, uint8_t *bgra,
                  WCHAR *err, size_t errn);
