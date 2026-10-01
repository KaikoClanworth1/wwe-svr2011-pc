/* WWE SmackDown vs. Raw 2011 - the Android launcher's native helpers
 * (libsvrtools.so, tools/build_apk.py): the PC launcher's Bink movie reader
 * (bink_decode.c) and writer (bink_encode.c) for the Movies tab
 * (MovieTools.java). Handles are pointers in Java longs. */
#include <jni.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "bink_decode.h"
#include "bink_encode.h"

static wchar_t g_err[512];

static void to_wide(JNIEnv *env, jstring s, wchar_t *out, size_t n)
{
    const char *u = (*env)->GetStringUTFChars(env, s, NULL);
    size_t k = mbstowcs(out, u, n - 1);
    out[k == (size_t)-1 ? 0 : k] = 0;
    (*env)->ReleaseStringUTFChars(env, s, u);
}

JNIEXPORT jstring JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_lastError(JNIEnv *env, jclass c)
{
    char u[1024];
    size_t k = wcstombs(u, g_err, sizeof u - 1);
    u[k == (size_t)-1 ? 0 : k] = 0;
    (void)c;
    return (*env)->NewStringUTF(env, u);
}

/* ── reading (bink_decode.c) ── */

JNIEXPORT jlong JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_binkOpen(JNIEnv *env, jclass c, jstring path)
{
    wchar_t p[4096];
    (void)c;
    to_wide(env, path, p, sizeof p / sizeof *p);
    g_err[0] = 0;
    return (jlong)(intptr_t)bink_open(p, g_err, sizeof g_err / sizeof *g_err);
}

#define R(h) ((BinkReader *)(intptr_t)(h))

JNIEXPORT jint JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_binkWidth(JNIEnv *e, jclass c, jlong h) { (void)e; (void)c; return bink_width(R(h)); }
JNIEXPORT jint JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_binkHeight(JNIEnv *e, jclass c, jlong h) { (void)e; (void)c; return bink_height(R(h)); }
JNIEXPORT jint JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_binkFrames(JNIEnv *e, jclass c, jlong h) { (void)e; (void)c; return bink_frames(R(h)); }
JNIEXPORT jlong JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_binkFrameTime(JNIEnv *e, jclass c, jlong h) { (void)e; (void)c; return (jlong)bink_frame_time(R(h)); }
JNIEXPORT jboolean JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_binkNext(JNIEnv *e, jclass c, jlong h) { (void)e; (void)c; return bink_next(R(h)) ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT void JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_binkRewind(JNIEnv *e, jclass c, jlong h) { (void)e; (void)c; bink_rewind(R(h)); }
JNIEXPORT jint JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_binkPosition(JNIEnv *e, jclass c, jlong h) { (void)e; (void)c; return bink_position(R(h)); }
JNIEXPORT void JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_binkClose(JNIEnv *e, jclass c, jlong h) { (void)e; (void)c; if (h) bink_close(R(h)); }

/* The decoded frame as BGRA into `out` (width x height x 4 bytes). */
JNIEXPORT void JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_binkBgra(JNIEnv *env, jclass c, jlong h, jbyteArray out)
{
    jbyte *b = (*env)->GetByteArrayElements(env, out, NULL);
    (void)c;
    bink_bgra(R(h), (uint8_t *)b);
    (*env)->ReleaseByteArrayElements(env, out, b, 0);
}

/* ── writing (bink_encode.c) ── */

typedef struct { FILE *f; BinkWriter *w; } Writer;

JNIEXPORT jlong JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_writerOpen(JNIEnv *env, jclass c, jstring path, jint frames)
{
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    Writer *w = (Writer *)calloc(1, sizeof *w);
    (void)c;
    if (w) w->f = fopen(p, "wb");
    (*env)->ReleaseStringUTFChars(env, path, p);
    if (!w || !w->f || !(w->w = bink_writer_open(w->f, frames))) {
        if (w && w->f) fclose(w->f);
        free(w);
        return 0;
    }
    return (jlong)(intptr_t)w;
}

/* 1 written, 0 out of memory, -1 write failed (disk full). */
JNIEXPORT jint JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_writerFrame(JNIEnv *env, jclass c, jlong h, jbyteArray bgra)
{
    Writer *w = (Writer *)(intptr_t)h;
    jbyte *b = (*env)->GetByteArrayElements(env, bgra, NULL);
    int r = bink_writer_frame(w->w, (const uint8_t *)b);
    (void)c;
    (*env)->ReleaseByteArrayElements(env, bgra, b, JNI_ABORT);
    return r;
}

JNIEXPORT jboolean JNICALL Java_io_github_kaikoclanworth1_svr2011_MovieTools_writerClose(JNIEnv *env, jclass c, jlong h, jboolean keep)
{
    Writer *w = (Writer *)(intptr_t)h;
    int ok = 1;
    (void)env; (void)c;
    if (keep) bink_writer_close(w->w);
    else bink_writer_free(w->w);
    if (fclose(w->f)) ok = 0;
    free(w);
    return ok ? JNI_TRUE : JNI_FALSE;
}
