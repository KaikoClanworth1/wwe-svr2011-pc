/* WWE SmackDown vs. Raw 2011 PC launcher - music volume levelling (see music_level.h).
 *
 * The meter is the Mod Maker's (modmaker/loudness.cpp) in C: Media Foundation decodes the song to float, it is
 * K-weighted (a high shelf, then a high pass), cut in 400 ms blocks every 100 ms, gated at -70 LUFS and then 10 LU
 * under the mean of the blocks left. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS
#include "music_level.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct {
    double b0, b1, b2, a1, a2, z1, z2;
} Biquad;

static double run(Biquad *q, double x)
{
    const double y = q->b0 * x + q->z1;
    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

/* The K-weighting at this sample rate (the coefficients libebur128 derives). */
static void k_weighting(double fs, Biquad *shelf, Biquad *pass)
{
    const double pi = 3.14159265358979323846;
    double f0 = 1681.974450955533, g = 3.999843853973347, q = 0.7071752369554196;
    double k = tan(pi * f0 / fs), vh = pow(10.0, g / 20.0), vb = pow(vh, 0.4996667741545416), a0 = 1.0 + k / q + k * k;
    ZeroMemory(shelf, sizeof *shelf);
    ZeroMemory(pass, sizeof *pass);
    shelf->b0 = (vh + vb * k / q + k * k) / a0;
    shelf->b1 = 2.0 * (k * k - vh) / a0;
    shelf->b2 = (vh - vb * k / q + k * k) / a0;
    shelf->a1 = 2.0 * (k * k - 1.0) / a0;
    shelf->a2 = (1.0 - k / q + k * k) / a0;
    f0 = 38.13547087602444;
    q = 0.5003270373238773;
    k = tan(pi * f0 / fs);
    a0 = 1.0 + k / q + k * k;
    pass->b0 = 1.0;
    pass->b1 = -2.0;
    pass->b2 = 1.0;
    pass->a1 = 2.0 * (k * k - 1.0) / a0;
    pass->a2 = (1.0 - k / q + k * k) / a0;
}

static double loud(double z) { return -0.691 + 10.0 * log10(z > 0 ? z : 1e-20); }

int level_measure(const WCHAR *file, double *lufs, WCHAR *why, size_t why_n)
{
    static int started = -1;
    IMFSourceReader *rd = NULL;
    IMFMediaType *want = NULL, *got = NULL;
    UINT32 channels = 0, rate = 0, c;
    Biquad shelf[8], pass[8];
    double weight[8], acc = 0, sum, relative, *steps = NULL;
    size_t step, in_step = 0, n_steps = 0, cap = 0, i, n;
    HRESULT set;
#define FAIL(t) do { if (why) wcscpy_s(why, why_n, t); free(steps); if (rd) IMFSourceReader_Release(rd); return 0; } while (0)
    CoInitializeEx(NULL, COINIT_MULTITHREADED);   /* (a worker thread) */
    if (started < 0)   /* (mfplat / mfreadwrite are delay-loaded: Windows "N" without the Media Feature Pack has neither) */
        started = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) &&
                  LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) &&
                  SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    if (!started) FAIL(L"Media Foundation didn't start (Windows N: the Media Feature Pack)");
    if (FAILED(MFCreateSourceReaderFromURL(file, NULL, &rd))) FAIL(L"the song didn't open (format not supported?)");
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    IMFSourceReader_SetStreamSelection(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    MFCreateMediaType(&want);
    IMFMediaType_SetGUID(want, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
    IMFMediaType_SetGUID(want, &MF_MT_SUBTYPE, &MFAudioFormat_Float);
    set = IMFSourceReader_SetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want);
    IMFMediaType_Release(want);
    if (FAILED(set) || FAILED(IMFSourceReader_GetCurrentMediaType(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got)))
        FAIL(L"the song has no audio Media Foundation can decode");
    IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_NUM_CHANNELS, &channels);
    IMFMediaType_GetUINT32(got, &MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    IMFMediaType_Release(got);
    if (!channels || channels > 8 || !rate) FAIL(L"the song's audio format is unknown");
    for (c = 0; c < channels; ++c) {   /* L R C 1, LFE left out, surrounds 1.41 (5.1 order) */
        weight[c] = channels > 3 && c >= 3 ? (c == 3 ? 0.0 : 1.41) : 1.0;
        k_weighting((double)rate, &shelf[c], &pass[c]);
    }
    step = rate / 10;   /* (per 100 ms: the K-weighted energy, summed over the channels) */
    for (;;) {
        DWORD flags = 0;
        IMFSample *sample = NULL;
        IMFMediaBuffer *buf = NULL;
        if (FAILED(IMFSourceReader_ReadSample(rd, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, NULL, &flags, NULL, &sample)))
            break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (sample) IMFSample_Release(sample);
            break;
        }
        if (!sample) continue;
        if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(sample, &buf))) {
            BYTE *p = NULL;
            DWORD len = 0;
            if (SUCCEEDED(IMFMediaBuffer_Lock(buf, &p, NULL, &len))) {
                const float *f = (const float *)p;
                const size_t frames = len / sizeof(float) / channels;
                for (i = 0; i < frames; ++i) {
                    for (c = 0; c < channels; ++c) {
                        const double y = run(&pass[c], run(&shelf[c], (double)f[i * channels + c]));
                        acc += y * y * weight[c];
                    }
                    if (++in_step == step) {
                        if (n_steps == cap) {
                            double *m = (double *)realloc(steps, (cap = cap ? cap * 2 : 4096) * sizeof *steps);
                            if (!m) break;
                            steps = m;
                        }
                        steps[n_steps++] = acc;
                        acc = 0;
                        in_step = 0;
                    }
                }
                IMFMediaBuffer_Unlock(buf);
            }
            IMFMediaBuffer_Release(buf);
        }
        IMFSample_Release(sample);
    }
    IMFSourceReader_Release(rd);
    rd = NULL;
    if (n_steps < 4) FAIL(L"the song is too short to measure (under half a second)");
    /* 400 ms blocks every 100 ms (mean square over 4 steps, kept in steps[]), gated at -70, then 10 LU under */
    for (i = 0; i + 4 <= n_steps; ++i) steps[i] = (steps[i] + steps[i + 1] + steps[i + 2] + steps[i + 3]) / (double)(4 * step);
    n_steps -= 3;
    for (sum = 0, n = 0, i = 0; i < n_steps; ++i) if (loud(steps[i]) > -70.0) sum += steps[i], ++n;
    if (!n) FAIL(L"the song is silent");
    relative = loud(sum / (double)n) - 10.0;
    for (sum = 0, n = 0, i = 0; i < n_steps; ++i) if (loud(steps[i]) > -70.0 && loud(steps[i]) > relative) sum += steps[i], ++n;
    if (!n) FAIL(L"the song is silent");
    *lufs = loud(sum / (double)n);
    free(steps);
    return 1;
#undef FAIL
}

double level_gain_for(double lufs)
{
    double g = LEVEL_TARGET_LUFS - lufs;
    if (g < -20.0) g = -20.0;
    if (g > 6.0) g = 6.0;
    return floor(g * 10.0 + 0.5) / 10.0;
}

/* ── manifests ─────────────────────────────────────────────────────────── */

typedef struct {
    char **line;
    int n, cap;
    int crlf;
} Lines;

static void lines_free(Lines *l)
{
    int i;
    for (i = 0; i < l->n; i++) free(l->line[i]);
    free(l->line);
    ZeroMemory(l, sizeof *l);
}

static int lines_add(Lines *l, const char *s, size_t len)
{
    char *c;
    if (l->n == l->cap) {
        char **m = (char **)realloc(l->line, (size_t)(l->cap = l->cap ? l->cap * 2 : 64) * sizeof *m);
        if (!m) return 0;
        l->line = m;
    }
    if (!(c = (char *)malloc(len + 1))) return 0;
    memcpy(c, s, len);
    c[len] = 0;
    l->line[l->n++] = c;
    return 1;
}

static int lines_read(const WCHAR *path, Lines *l)
{
    FILE *f;
    char *buf;
    long size;
    size_t at = 0;
    ZeroMemory(l, sizeof *l);
    if (_wfopen_s(&f, path, L"rb") || !f) return 0;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0 || size > (1 << 20) || !(buf = (char *)malloc((size_t)size + 1))) { fclose(f); return 0; }
    size = (long)fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[size] = 0;
    l->crlf = strstr(buf, "\r\n") != NULL;
    while (at < (size_t)size) {
        size_t e = at;
        while (e < (size_t)size && buf[e] != '\n') e++;
        lines_add(l, buf + at, e > at && buf[e - 1] == '\r' ? e - at - 1 : e - at);
        at = e + 1;
    }
    free(buf);
    return 1;
}

/* Written whole into <path>.tmp, then put in its place. */
static int lines_write(const WCHAR *path, const Lines *l)
{
    WCHAR tmp[MAX_PATH + 8];
    FILE *f;
    int i, ok;
    swprintf_s(tmp, MAX_PATH + 8, L"%s.tmp", path);
    if (_wfopen_s(&f, tmp, L"wb") || !f) return 0;
    for (i = 0; i < l->n; i++) fprintf(f, "%s%s", l->line[i], l->crlf ? "\r\n" : "\n");
    ok = !ferror(f);
    ok &= fclose(f) == 0;
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp); return 0; }
    return 1;
}

/* The line "<key>=..." (from its first character, as the game matches keys), or -1. */
static int find_key(const Lines *l, const char *key)
{
    const size_t k = strlen(key);
    int i;
    for (i = 0; i < l->n; i++) if (!strncmp(l->line[i], key, k) && l->line[i][k] == '=') return i;
    return -1;
}

static void set_key(Lines *l, const char *key, const char *value)
{
    char t[600];
    const int i = find_key(l, key);
    const int n = sprintf_s(t, sizeof t, "%s=%s", key, value);
    if (n < 0) return;
    if (i >= 0) {
        char *c = _strdup(t);
        if (c) { free(l->line[i]); l->line[i] = c; }
    } else {
        lines_add(l, t, (size_t)n);
    }
}

static void report_add(WCHAR *report, size_t report_n, const WCHAR *what, const char *song_utf8, double gain, double lufs,
                       const WCHAR *why)
{
    WCHAR song[MAX_PATH], t[600];
    if (!report) return;
    MultiByteToWideChar(CP_UTF8, 0, song_utf8, -1, song, MAX_PATH);
    if (why) swprintf_s(t, 600, L"%s: %s - not measured: %s\r\n", what, song, why);
    else swprintf_s(t, 600, L"%s: %s %+.1f dB (was %.1f LUFS)\r\n", what, song, gain, lufs);
    wcsncat_s(report, report_n, t, _TRUNCATE);
}

/* One song key: its file measured and its gain written, unless the author gave one. 1 levelled, 0 left. */
static int level_one(const WCHAR *folder, Lines *l, const char *song_key, const char *gain_key, const char *auto_key,
                     const WCHAR *what, WCHAR *report, size_t report_n, int *changed)
{
    const int s = find_key(l, song_key), g = find_key(l, gain_key), a = find_key(l, auto_key);
    WCHAR rel[MAX_PATH], path[MAX_PATH], why[200], *c;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    double lufs = 0, gain;
    char v[32], stamp[48];
    if (s < 0) return 0;
    if (g >= 0 && a < 0) return 0;   /* (the author's own) */
    MultiByteToWideChar(CP_UTF8, 0, strchr(l->line[s], '=') + 1, -1, rel, MAX_PATH);
    if (!rel[0]) return 0;
    for (c = rel; *c; c++) if (*c == L'/') *c = L'\\';
    swprintf_s(path, MAX_PATH, L"%s\\%s", folder, rel);
    /* (ours: theme_gain_auto=<size>:<time> of the song measured - measured again only when it changed) */
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) ZeroMemory(&fa, sizeof fa);
    sprintf_s(stamp, sizeof stamp, "%llu:%llu", ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow,
              ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) | fa.ftLastWriteTime.dwLowDateTime);
    if (g >= 0 && a >= 0 && !strcmp(strchr(l->line[a], '=') + 1, stamp)) return 0;
    if (!level_measure(path, &lufs, why, 200)) {
        report_add(report, report_n, what, strchr(l->line[s], '=') + 1, 0, 0, why);
        return 0;
    }
    gain = level_gain_for(lufs);
    sprintf_s(v, sizeof v, "%.1f", gain);
    set_key(l, gain_key, v);
    set_key(l, auto_key, stamp);
    *changed = 1;
    report_add(report, report_n, what, strchr(l->line[s], '=') + 1, gain, lufs, NULL);
    return 1;
}

int level_mod(const WCHAR *folder, WCHAR *report, size_t report_n)
{
    WCHAR manifest[MAX_PATH];
    const WCHAR *what = wcsrchr(folder, L'\\') ? wcsrchr(folder, L'\\') + 1 : folder;
    Lines l;
    int done = 0, changed = 0, i, t;
    swprintf_s(manifest, MAX_PATH, L"%s\\manifest.txt", folder);
    if (!lines_read(manifest, &l)) return 0;
    t = find_key(&l, "type");
    if (t >= 0 && !_stricmp(l.line[t] + 5, "superstar")) {
        done += level_one(folder, &l, "song", "theme_gain", "theme_gain_auto", what, report, report_n, &changed);
    } else if (t >= 0 && !_stricmp(l.line[t] + 5, "media")) {
        const int n = l.n;   /* (theme.<id>= lines; the gains it adds come after them) */
        for (i = 0; i < n; i++) {
            char key[64], gk[80], ak[80];
            const char *eq = strchr(l.line[i], '=');
            if (strncmp(l.line[i], "theme.", 6) || !eq || eq - l.line[i] >= (int)sizeof key) continue;
            memcpy(key, l.line[i], (size_t)(eq - l.line[i]));
            key[eq - l.line[i]] = 0;
            sprintf_s(gk, sizeof gk, "theme_gain.%s", key + 6);
            sprintf_s(ak, sizeof ak, "theme_gain_auto.%s", key + 6);
            done += level_one(folder, &l, key, gk, ak, what, report, report_n, &changed);
        }
    }
    if (changed && !lines_write(manifest, &l)) done = -1;
    lines_free(&l);
    return done;
}

/* ── the Music folder ──────────────────────────────────────────────────── */

static const WCHAR *const k_song_ext[] = { L".mp3", L".wma", L".m4a", L".aac", L".wav", L".flac" };

static int is_song(const WCHAR *name)
{
    const WCHAR *dot = wcsrchr(name, L'.');
    int i;
    for (i = 0; dot && i < 6; i++) if (!_wcsicmp(dot, k_song_ext[i])) return 1;
    return 0;
}

/* A mod's theme copy (the game's .mod_themes list, or a media pack's "<pack> theme <id>"): its mod's gain, not ours. */
static int mod_copy(const WCHAR *music, const WCHAR *name)
{
    WCHAR path[MAX_PATH], stem[MAX_PATH], *dot;
    const WCHAR *t;
    Lines l;
    int i, found = 0;
    wcscpy_s(stem, MAX_PATH, name);
    if ((dot = wcsrchr(stem, L'.')) != NULL) *dot = 0;
    if ((t = wcsstr(stem, L" theme ")) != NULL && t[7] >= L'0' && t[7] <= L'9') return 1;
    swprintf_s(path, MAX_PATH, L"%s\\.mod_themes", music);
    if (!lines_read(path, &l)) return 0;
    for (i = 0; i < l.n && !found; i++) {
        WCHAR w[MAX_PATH], *tab;
        MultiByteToWideChar(CP_UTF8, 0, l.line[i], -1, w, MAX_PATH);
        if ((tab = wcschr(w, L'\t')) != NULL) *tab = 0;
        found = !_wcsicmp(w, stem);
    }
    lines_free(&l);
    return found;
}

static int level_dir(const WCHAR *music, const WCHAR *dir, int top, Lines *levels, WCHAR *report, size_t report_n,
                     int *changed)
{
    WCHAR pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int done = 0;
    swprintf_s(pat, MAX_PATH, L"%s\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        char name[MAX_PATH * 3], stamp[64], v[200];
        double lufs = 0, gain;
        WCHAR why[200];
        int i;
        if (fd.cFileName[0] == L'.') continue;
        swprintf_s(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {   /* (a playlist folder: one level down, as the game) */
            if (top) done += level_dir(music, path, 0, levels, report, report_n, changed);
            continue;
        }
        if (!is_song(fd.cFileName) || mod_copy(music, fd.cFileName)) continue;
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL);
        sprintf_s(stamp, sizeof stamp, "%llu\t%llu", ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow,
                  ((unsigned long long)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime);
        for (i = 0; i < levels->n; i++) {   /* ("<name>\t<dB>\t<size>\t<time>": measured already, unchanged) */
            const size_t k = strlen(name);
            const char *rest;
            if (strncmp(levels->line[i], name, k) || levels->line[i][k] != '\t') continue;
            rest = strchr(levels->line[i] + k + 1, '\t');
            if (rest && !strcmp(rest + 1, stamp)) break;
        }
        if (i < levels->n) continue;
        if (!level_measure(path, &lufs, why, 200)) {
            report_add(report, report_n, L"Music", name, 0, 0, why);
            continue;
        }
        gain = level_gain_for(lufs);
        for (i = 0; i < levels->n; i++) {   /* (its old line goes) */
            const size_t k = strlen(name);
            if (!strncmp(levels->line[i], name, k) && levels->line[i][k] == '\t') {
                free(levels->line[i]);
                memmove(&levels->line[i], &levels->line[i + 1], (size_t)(levels->n - i - 1) * sizeof *levels->line);
                levels->n--;
                i--;
            }
        }
        sprintf_s(v, sizeof v, "\t%.1f\t%s", gain, stamp);
        {
            char line[MAX_PATH * 3 + 200];
            const int n = sprintf_s(line, sizeof line, "%s%s", name, v);
            if (n > 0) lines_add(levels, line, (size_t)n);
        }
        *changed = 1;
        report_add(report, report_n, L"Music", name, gain, lufs, NULL);
        done++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return done;
}

int level_music_folder(const WCHAR *music, WCHAR *report, size_t report_n)
{
    WCHAR path[MAX_PATH];
    Lines levels;
    int changed = 0, done;
    swprintf_s(path, MAX_PATH, L"%s\\.levels.txt", music);
    if (!lines_read(path, &levels)) ZeroMemory(&levels, sizeof levels);
    done = level_dir(music, music, 1, &levels, report, report_n, &changed);
    if (changed && !lines_write(path, &levels)) done = -1;
    lines_free(&levels);
    return done;
}
