/* WWE SmackDown vs. Raw 2011 PC launcher - zip extraction without tar.exe
 * (which Wine / Proton lacks). */

#pragma once

#include <stddef.h>
#include <wchar.h>

/* Extracts every file of the zip `zip` into the existing folder `to` (store
 * and deflate; no zip64 or encryption). 1 on success. */
int unzip_file(const wchar_t *zip, const wchar_t *to);

/* Raw DEFLATE (a zip entry's method 8 data) into out, exactly out_len bytes.
 * 1 on success. (The Mod Maker's zip reader uses it too.) */
int unzip_inflate(const unsigned char *in, size_t in_len, unsigned char *out, size_t out_len);
