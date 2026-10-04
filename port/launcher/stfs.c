/* WWE SmackDown vs. Raw 2011 - Xbox 360 save import (see stfs.h).
 *
 * An STFS package: a header (magic at 0, header size at 0x340, content type
 * at 0x344 - 1 = saved game, title id at 0x360, the volume descriptor at
 * 0x379, the display name, UTF-16BE, at 0x411), then 4 KB blocks with hash
 * tables between them: one per 0xAA data blocks (level 0), one per 0x70E4
 * (level 1). A "male" package (the volume descriptor's block separation
 * bit 0 clear) keeps two copies of each table; which one is current comes
 * from the level above (status bit 0x40) or, at the top, the separation's
 * bit 1. The file table: 0x40-byte entries - name (length in the low 6 bits
 * of +0x28; 0x40 = blocks in a row, 0x80 = folder), block count at +0x29 and
 * first block at +0x2F (24-bit little-endian), size at +0x34; other blocks
 * follow their hash entry's next-block field.
 *
 * The game's 360 saves each hold one file, SaveData.Dat - the same bytes as
 * the port's save file of that name (SaveData.dat, NNCreateSuperStar.cas,
 * ...). The port keeps a 328-byte header beside each (the game finds Created
 * Superstars etc. by display name): src/saves.cpp RestoreHeaders. */
#include "stfs.h"

#include <shlobj.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define TITLE_ID 0x5451085Du

typedef struct {
    const uint8_t *d;
    size_t size;
    uint32_t sex, step0, step1, first, top_level, sep;
} Pkg;

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le24(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }

static uint32_t data_block(const Pkg *k, uint32_t b)
{
    uint32_t r = (((b + 0xAA) / 0xAA) << k->sex) + b;
    if (b < 0xAA)
        return r;
    if (b < 0x70E4)
        return r + (((b + 0x70E4) / 0x70E4) << k->sex);
    return (1u << k->sex) + r + (((b + 0x70E4) / 0x70E4) << k->sex);
}

static const uint8_t *block(const Pkg *k, uint32_t b)
{
    size_t at = ((size_t)data_block(k, b) << 12) + k->first;
    return at + 0x1000 <= k->size ? k->d + at : NULL;
}

/* The next block after `b` (its level-0 hash entry); ~0 if out of range. */
static uint32_t next_block(const Pkg *k, uint32_t b)
{
    uint32_t l0 = 0;
    size_t at;
    if (b >= 0xAA) {
        l0 = (b / 0xAA) * k->step0 + (((b / 0x70E4) + 1) << k->sex);
        if (b / 0x70E4)
            l0 += 1u << k->sex;
    }
    at = ((size_t)l0 << 12) + k->first + (b % 0xAA) * 0x18;
    if (k->top_level == 0) {
        at += (size_t)(k->sep & 2) << 0xB;
    } else {
        uint32_t l1 = b < 0x70E4 ? k->step0 : (1u << k->sex) + (b / 0x70E4) * k->step1;
        size_t top = ((size_t)l1 << 12) + k->first + ((size_t)(k->sep & 2) << 0xB) + (b / 0xAA) * 0x18 + 0x14;
        if (top >= k->size)
            return ~0u;
        at += (size_t)(k->d[top] & 0x40) << 6;
    }
    if (at + 0x18 > k->size)
        return ~0u;
    return (uint32_t)k->d[at + 0x15] << 16 | (uint32_t)k->d[at + 0x16] << 8 | k->d[at + 0x17];
}

/* Copies `count` blocks from `start` (`out` has count * 4 KB). */
static int read_blocks(const Pkg *k, uint32_t start, uint32_t count, int in_a_row, uint8_t *out)
{
    uint32_t b = start, i;
    for (i = 0; i < count; i++) {
        const uint8_t *p = block(k, b);
        if (!p)
            return 0;
        memcpy(out + (size_t)i * 0x1000, p, 0x1000);
        b = in_a_row ? b + 1 : next_block(k, b);
        if (b == ~0u && i + 1 < count)
            return 0;
    }
    return 1;
}

static uint8_t *load(const WCHAR *path, size_t *size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER n;
    uint8_t *d = NULL;
    DWORD got;
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    if (GetFileSizeEx(h, &n) && n.QuadPart >= 0x1000 && n.QuadPart < 512 * 1024 * 1024
            && (d = (uint8_t *)malloc((size_t)n.QuadPart)) != NULL) {
        if (!ReadFile(h, d, (DWORD)n.QuadPart, &got, NULL) || got != (DWORD)n.QuadPart) {
            free(d);
            d = NULL;
        }
        *size = (size_t)n.QuadPart;
    }
    CloseHandle(h);
    return d;
}

static int write_all(const WCHAR *path, const void *data, size_t n)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD put = 0;
    int ok;
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    ok = WriteFile(h, data, (DWORD)n, &put, NULL) && put == n;
    CloseHandle(h);
    if (!ok)
        DeleteFileW(path);
    return ok;
}

/* One package -> dest\name (+ .info\name.header); with no `dest`, its save
 * file into *mem (malloc'd, *mem_size bytes) instead. 1: done, 0: not one of
 * the game's saves, -1: unreadable. */
static int import_one(const WCHAR *path, const WCHAR *name, const WCHAR *dest, uint8_t **mem, size_t *mem_size)
{
    size_t size = 0, i;
    uint8_t *d = load(path, &size), *table = NULL, *data = NULL;
    Pkg k;
    uint32_t header_size, ft_count, ft_block, allocated, file_size = 0, file_start = 0, file_blocks = 0;
    int in_a_row = 0, found = 0, ret = -1;
    WCHAR out[MAX_PATH], info[MAX_PATH];
    uint8_t h[328];
    if (!d)
        return 0;
    if (memcmp(d, "CON ", 4) && memcmp(d, "LIVE", 4) && memcmp(d, "PIRS", 4)) {
        free(d);
        return 0;
    }
    if (size < 0x1000 || be32(d + 0x360) != TITLE_ID || be32(d + 0x344) != 1) {
        free(d);
        return 0;
    }
    header_size = be32(d + 0x340);
    k.d = d;
    k.size = size;
    k.sep = d[0x379 + 2];
    ft_count = d[0x379 + 3] | (uint32_t)d[0x379 + 4] << 8;
    ft_block = le24(d + 0x379 + 5);
    allocated = be32(d + 0x379 + 0x1C);
    k.sex = (~k.sep) & 1;
    k.step0 = k.sex ? 0xAC : 0xAB;
    k.step1 = k.sex ? 0x723A : 0x718F;
    k.first = (header_size + 0xFFF) & 0xFFFFF000u;
    k.top_level = allocated <= 0xAA ? 0 : allocated <= 0x70E4 ? 1 : 2;
    if (k.top_level == 2 || !ft_count || ft_count > 64)  /* (saves are far smaller) */
        goto done;
    table = (uint8_t *)malloc((size_t)ft_count * 0x1000);
    if (!table || !read_blocks(&k, ft_block, ft_count, 0, table))
        goto done;
    for (i = 0; i + 0x40 <= (size_t)ft_count * 0x1000; i += 0x40) {
        const uint8_t *e = table + i;
        if (!(e[0x28] & 0x3F) || (e[0x28] & 0x80))  /* (empty / a folder) */
            continue;
        file_blocks = le24(e + 0x29);
        file_start = le24(e + 0x2F);
        file_size = be32(e + 0x34);
        in_a_row = (e[0x28] & 0x40) != 0;
        found = 1;
        break;
    }
    if (!found || file_size > (size_t)file_blocks * 0x1000 || !file_blocks)
        goto done;
    data = (uint8_t *)malloc((size_t)file_blocks * 0x1000);
    if (!data || !read_blocks(&k, file_start, file_blocks, in_a_row, data))
        goto done;
    if (!dest) {
        *mem = data;
        *mem_size = file_size;
        data = NULL;
        ret = 1;
        goto done;
    }
    swprintf_s(out, MAX_PATH, L"%s\\%s", dest, name);
    swprintf_s(info, MAX_PATH, L"%s\\.info", dest);
    CreateDirectoryW(info, NULL);
    if (!write_all(out, data, file_size))
        goto done;
    memset(h, 0, sizeof h);
    h[3] = 1;  /* device */
    h[7] = 1;  /* saved game */
    memcpy(h + 8, d + 0x411, 0x80);  /* the display name (UTF-16BE, as the package keeps it) */
    for (i = 0; name[i] && i < 41; i++)
        h[264 + i] = (uint8_t)name[i];
    h[320] = 0x54, h[321] = 0x51, h[322] = 0x08, h[323] = 0x5D;
    swprintf_s(info, MAX_PATH, L"%s\\.info\\%s.header", dest, name);
    ret = write_all(info, h, sizeof h) ? 1 : -1;
done:
    free(data);
    free(table);
    free(d);
    return ret;
}

static void scan(const WCHAR *dir, const WCHAR *dest, int depth, int *done, int *skipped)
{
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE f;
    if (swprintf_s(pat, MAX_PATH, L"%s\\*", dir) < 0 || (f = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")
                || swprintf_s(p, MAX_PATH, L"%s\\%s", dir, fd.cFileName) < 0 || !_wcsicmp(p, dest))
            continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 5)
                scan(p, dest, depth + 1, done, skipped);
        } else if (fd.nFileSizeHigh == 0 && fd.nFileSizeLow >= 0x1000) {
            int r = import_one(p, fd.cFileName, dest, NULL, NULL);
            if (r > 0)
                ++*done;
            else if (r < 0)
                ++*skipped;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

int stfs_import_saves(const WCHAR *src, const WCHAR *dest, int *skipped)
{
    int done = 0, skip = 0;
    SHCreateDirectoryExW(NULL, dest, NULL);
    scan(src, dest, 0, &done, &skip);
    if (skipped)
        *skipped = skip;
    return done;
}

int stfs_read_save(const WCHAR *path, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    return import_one(path, NULL, NULL, data, size) == 1;
}
