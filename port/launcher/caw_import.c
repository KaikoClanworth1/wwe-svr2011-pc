/* WWE SmackDown vs. Raw 2011 - Created Superstar import (see caw_import.h).
 *
 * A Created Superstar is two things:
 *  - NNCreateSuperStar.cas (1347532 bytes): header 0x4B, 0x18, 7, checksum
 *    (at +12, again at +0x148FC8 and +0x148DBC; sub_828CBE28: sums of typed
 *    fields - the record copy at +0x1483C4 is summed as plain bytes, so a
 *    change there moves the checksum by the change of its byte sum). Four
 *    attires' logo caches at 20 + i * 186580 (src/caw_logos.cpp).
 *  - its record in SaveData.dat: 0x6BC bytes at 0x13FB8 + slot * 0x6BC (50
 *    slots; all zero = free) - the .cas's copy (+0x1483C4) up to +0x548, then
 *    the save's own part: +0x54A/+0x54C set, +0x65C the .cas's checksum (the game
 *    checks it: "damaged or missing" otherwise), +0x69C.. online upload stamps.
 *    +0x20 (u16) is the slot, in both.
 *  SaveData.dat's checksum (+0x18, sub_827235D8) is the byte sum of 0x1C..0x812DC.
 * The display name the game finds the file by is "NN.CREATED SUPERSTAR"
 * (slot + 1) in the port's header (.info). */
#include "caw_import.h"

#include "stfs.h"

#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CAS_SIZE     1347532u
#define REC_IN_CAS   0x1483C4u
#define REC_BASE     0x13FB8u
#define REC_SIZE     0x6BCu
#define MAX_CAW      50
#define SD_SUM_FROM  0x1Cu
#define SD_SUM_TO    0x812DCu
#define CACHE0       20u
#define CACHE_SIZE   186580u
#define ATTIRES      4

/* the logo cache (src/caw_logos.cpp) */
#define LOW_USED     12168u
#define HIGH_USED    12178u
#define PALETTES     12180u
#define HIGH_PIXELS  14228u
#define LOW_PIXELS   22420u
#define HIGH_IDS     145300u
#define LOW_IDS      186260u
#define EXT_OFFSET   160000u
#define EXT_MAGIC    0x584C4731u  /* "XLG1", little-endian (the port writes it) */
#define MAX_HIGH     10

/* The save's own part of a record (+0x548..), for a .cas without its save. */
static const uint8_t k_tail[REC_SIZE - 0x548] = {
    [0x54A - 0x548] = 0x02, [0x54C - 0x548] = 0x01,
    [0x667 - 0x548] = 0x4D, [0x66C - 0x548] = 0x01, [0x66D - 0x548] = 0x03,
    [0x676 - 0x548] = 0xC7, [0x677 - 0x548] = 0x03, [0x678 - 0x548] = 0x58, [0x679 - 0x548] = 0x05,
    [0x683 - 0x548] = 0x6A, [0x689 - 0x548] = 0x0E, [0x68C - 0x548] = 0x01, [0x690 - 0x548] = 0x01,
};

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t bytes_sum(const uint8_t *p, size_t n)
{
    uint32_t s = 0;
    while (n--)
        s += *p++;
    return s;
}

/* A file, or a 360 package's save file, into memory. */
static uint8_t *read_any(const WCHAR *path, size_t *size)
{
    uint8_t *d = NULL;
    FILE *f;
    long n;
    if (stfs_read_save(path, &d, size))
        return d;
    if (_wfopen_s(&f, path, L"rb") || !f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < 64 * 1024 * 1024 && (d = (uint8_t *)malloc((size_t)n)) != NULL) {
        if (fread(d, 1, (size_t)n, f) != (size_t)n) {
            free(d);
            d = NULL;
        } else {
            *size = (size_t)n;
        }
    }
    fclose(f);
    return d;
}

static int write_file(const WCHAR *path, const void *data, size_t n)
{
    WCHAR tmp[MAX_PATH + 8];
    FILE *f;
    int ok;
    swprintf_s(tmp, MAX_PATH + 8, L"%s.new", path);
    if (_wfopen_s(&f, tmp, L"wb") || !f)
        return 0;
    ok = fwrite(data, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp);
        return 0;
    }
    return 1;
}

static uint64_t fnv64(const uint8_t *p, size_t n)
{
    uint64_t h = 14695981039346656037ull;
    while (n--)
        h = (h ^ *p++) * 1099511628211ull;
    return h;
}

/* Adds a logo unless one with the same pixels and palette is there already. */
static void add_logo(CawLogo **list, int *n, int *cap, const uint8_t *palette, const uint8_t *pixels, int size,
                     uint32_t id)
{
    CawLogo *l;
    int i, x, y;
    if (*n == *cap) {
        CawLogo *more = (CawLogo *)realloc(*list, sizeof(CawLogo) * (size_t)(*cap ? *cap * 2 : 8));
        if (!more)
            return;
        *list = more;
        *cap = *cap ? *cap * 2 : 8;
    }
    l = &(*list)[*n];
    memcpy(l->palette, palette, 1024);
    if (size == 256) {
        memcpy(l->pixels, pixels, 65536);
    } else {
        for (y = 0; y < 256; y++)
            for (x = 0; x < 256; x++)
                l->pixels[y * 256 + x] = pixels[(y / 2) * 128 + x / 2];
    }
    l->id = id;
    for (i = 0; i < *n; i++)
        if (!memcmp((*list)[i].palette, l->palette, 1024) && !memcmp((*list)[i].pixels, l->pixels, 65536))
            return;
    ++*n;
}

/* The logos of every attire; the port's extra 256 x 256 ones come from the
 * source's .logos (copied to the destination's, which the game needs). */
static void collect_logos(const uint8_t *cas, const WCHAR *src_dir, const WCHAR *saves, CawLogo **list, int *n)
{
    int cap = 0, a, i;
    WCHAR from[MAX_PATH], to[MAX_PATH], dir[MAX_PATH];
    for (a = 0; a < ATTIRES; a++) {
        const uint8_t *c = cas + CACHE0 + (size_t)a * CACHE_SIZE, *e = c + EXT_OFFSET;
        const int high = c[HIGH_USED] || c[HIGH_USED + 1];
        if (high) {
            for (i = 0; i < 2; i++)
                if (c[HIGH_USED + i])
                    add_logo(list, n, &cap, c + PALETTES + 1024 * i, c + HIGH_PIXELS + 65536 * i, 256,
                             be32(c + HIGH_IDS + 4 * i));
        } else {
            for (i = 0; i < 10; i++)
                if (c[LOW_USED + i])
                    add_logo(list, n, &cap, c + PALETTES + 1024 * i, c + LOW_PIXELS + 16384 * i, 128,
                             be32(c + LOW_IDS + 4 * i));
        }
        if (!high || le32(e) != EXT_MAGIC)
            continue;
        /* the Ext record: magic, check, used[10], pad[6], hash u64[10], id u32[10] (host order) */
        for (i = 2; i < MAX_HIGH; i++) {
            uint64_t hash;
            uint8_t *bin;
            size_t size = 0;
            if (!e[8 + i])
                continue;
            memcpy(&hash, e + 24 + 8 * i, 8);
            swprintf_s(from, MAX_PATH, L"%s\\.logos\\%016llX.bin", src_dir, (unsigned long long)hash);
            swprintf_s(dir, MAX_PATH, L"%s\\.logos", saves);
            swprintf_s(to, MAX_PATH, L"%s\\%016llX.bin", dir, (unsigned long long)hash);
            if ((bin = read_any(from, &size)) != NULL && size == 1024 + 65536) {
                add_logo(list, n, &cap, bin, bin + 1024, 256, le32(e + 104 + 4 * i));
                if (GetFileAttributesW(to) == INVALID_FILE_ATTRIBUTES) {
                    CreateDirectoryW(dir, NULL);
                    CopyFileW(from, to, TRUE);
                }
            }
            free(bin);
        }
    }
}

int caw_import(const WCHAR *src, const WCHAR *saves, WCHAR *name, size_t name_n, CawLogo **logos, int *n_logos,
               WCHAR *err, size_t err_n)
{
    WCHAR sd_path[MAX_PATH], src_dir[MAX_PATH], path[MAX_PATH], *slash;
    uint8_t *cas = NULL, *sd = NULL, *src_sd = NULL, rec[REC_SIZE], h[328];
    size_t cas_size = 0, sd_size = 0, src_sd_size = 0, i;
    int slot = -1, k, ret = -1;
    uint32_t old, delta, src_slot;
    char disp[32], file[32];
    *logos = NULL;
    *n_logos = 0;
    name[0] = 0;
    cas = read_any(src, &cas_size);
    if (!cas || cas_size != CAS_SIZE || be32(cas) != 0x4B || be32(cas + 4) != 0x18 || be32(cas + 8) != 7) {
        swprintf_s(err, err_n, L"%s is not a Created Superstar save.", src);
        goto done;
    }
    swprintf_s(sd_path, MAX_PATH, L"%s\\SaveData.dat", saves);
    sd = read_any(sd_path, &sd_size);
    if (!sd || sd_size < SD_SUM_TO || be32(sd + 0x18) != bytes_sum(sd + SD_SUM_FROM, SD_SUM_TO - SD_SUM_FROM)) {
        swprintf_s(err, err_n, L"There is no main save (SaveData.dat) the game accepts in %s yet: start the game "
                               L"once first.", saves);
        goto done;
    }
    for (k = 0; k < MAX_CAW && slot < 0; k++) {
        const uint8_t *r = sd + REC_BASE + (size_t)k * REC_SIZE;
        int used = 0;
        for (i = 0; i < REC_SIZE && !used; i++)
            used = r[i] != 0;
        swprintf_s(path, MAX_PATH, L"%s\\%02dCreateSuperStar.cas", saves, k);
        if (!used && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
            slot = k;
    }
    if (slot < 0) {
        swprintf_s(err, err_n, L"All %d Created Superstar slots are in use: delete one in the game first.", MAX_CAW);
        goto done;
    }
    /* the source save's record, when it is beside the .cas and fits it */
    wcscpy_s(src_dir, MAX_PATH, src);
    slash = wcsrchr(src_dir, L'\\');
    if (slash)
        *slash = 0;
    src_slot = (uint32_t)cas[REC_IN_CAS + 0x20] << 8 | cas[REC_IN_CAS + 0x21];
    swprintf_s(path, MAX_PATH, L"%s\\SaveData.dat", src_dir);
    src_sd = read_any(path, &src_sd_size);
    if (src_sd && src_sd_size >= SD_SUM_TO && src_slot < MAX_CAW
            && be32(src_sd + REC_BASE + src_slot * REC_SIZE + 0x65C) == be32(cas + 12)) {
        memcpy(rec, src_sd + REC_BASE + src_slot * REC_SIZE, REC_SIZE);
    } else {
        memcpy(rec, cas + REC_IN_CAS, 0x548);
        memcpy(rec + 0x548, k_tail, sizeof k_tail);
    }
    /* the slot, in the .cas (its checksum moves by the byte sum's change) and the record */
    old = cas[REC_IN_CAS + 0x20] + (uint32_t)cas[REC_IN_CAS + 0x21];
    cas[REC_IN_CAS + 0x20] = (uint8_t)(slot >> 8);
    cas[REC_IN_CAS + 0x21] = (uint8_t)slot;
    delta = cas[REC_IN_CAS + 0x20] + (uint32_t)cas[REC_IN_CAS + 0x21] - old;
    wbe32(cas + 12, be32(cas + 12) + delta);
    wbe32(cas + 0x148FC8, be32(cas + 0x148FC8) + delta);
    wbe32(cas + 0x148DBC, be32(cas + 0x148DBC) + delta);
    rec[0x20] = (uint8_t)(slot >> 8);
    rec[0x21] = (uint8_t)slot;
    memcpy(rec + 0x65C, cas + 12, 4);
    memset(rec + 0x69C, 0, REC_SIZE - 0x69C);  /* (no online upload stamps) */
    for (i = 0; i < 31 && rec[0x22 + i] >= 0x20 && rec[0x22 + i] < 0x7F && i + 1 < name_n; i++)
        name[i] = rec[0x22 + i];
    name[i] = 0;
    /* the files: the .cas, its header, then SaveData.dat */
    sprintf_s(file, sizeof file, "%02dCreateSuperStar.cas", slot);
    swprintf_s(path, MAX_PATH, L"%s\\%02dCreateSuperStar.cas", saves, slot);
    if (!write_file(path, cas, CAS_SIZE)) {
        swprintf_s(err, err_n, L"Could not write %s.", path);
        goto done;
    }
    memset(h, 0, sizeof h);
    h[3] = 1;
    h[7] = 1;
    sprintf_s(disp, sizeof disp, "%02d.CREATED SUPERSTAR", slot + 1);
    for (i = 0; disp[i]; i++)
        h[8 + 2 * i + 1] = (uint8_t)disp[i];
    for (i = 0; file[i]; i++)
        h[264 + i] = (uint8_t)file[i];
    h[320] = 0x54, h[321] = 0x51, h[322] = 0x08, h[323] = 0x5D;
    swprintf_s(path, MAX_PATH, L"%s\\.info", saves);
    CreateDirectoryW(path, NULL);
    swprintf_s(path, MAX_PATH, L"%s\\.info\\%02dCreateSuperStar.cas.header", saves, slot);
    write_file(path, h, sizeof h);
    memcpy(sd + REC_BASE + (size_t)slot * REC_SIZE, rec, REC_SIZE);
    wbe32(sd + 0x18, bytes_sum(sd + SD_SUM_FROM, SD_SUM_TO - SD_SUM_FROM));
    if (!write_file(sd_path, sd, sd_size)) {
        swprintf_s(path, MAX_PATH, L"%s\\%02dCreateSuperStar.cas", saves, slot);
        DeleteFileW(path);
        swprintf_s(err, err_n, L"Could not write %s.", sd_path);
        goto done;
    }
    collect_logos(cas, src_dir, saves, logos, n_logos);
    ret = slot;
done:
    free(cas);
    free(sd);
    free(src_sd);
    return ret;
}
