// WWE SmackDown vs. Raw 2011 - MSVC CRT functions the port uses, for builds
// without them (Android, Linux). Force-included there (CMakeLists.txt).
#pragma once

#if !defined(_WIN32) && defined(__cplusplus)  // (C files: the launcher's Bink code has its own)

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>

inline int _dupenv_s(char** buffer, size_t* size, const char* name) {
  const char* v = std::getenv(name);
  *buffer = v ? strdup(v) : nullptr;
  if (size) *size = v ? std::strlen(v) + 1 : 0;
  return 0;
}

inline int fopen_s(FILE** file, const char* name, const char* mode) {
  *file = std::fopen(name, mode);
  return *file ? 0 : errno;
}

inline unsigned short _byteswap_ushort(unsigned short v) { return __builtin_bswap16(v); }
inline unsigned long _byteswap_ulong(unsigned long v) { return __builtin_bswap32(uint32_t(v)); }
inline unsigned long long _byteswap_uint64(unsigned long long v) { return __builtin_bswap64(v); }

inline int localtime_s(struct tm* out, const time_t* t) { return localtime_r(t, out) ? 0 : errno; }

#endif  // !_WIN32 && __cplusplus
