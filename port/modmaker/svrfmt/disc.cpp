// Disc images: XDVDFS (Xbox / Xbox 360) and ISO 9660 (PS2). See disc.h and
// port/tools/discfs.py (same layouts).
#include "disc.h"

#include <algorithm>
#include <cstdio>

namespace svrfmt {

namespace {

constexpr uint64_t kSector = 2048;
constexpr uint64_t kXboxOffsets[] = {0, 0x18300000, 0xFD90000, 0x2080000};  // XISO, XGD1, XGD2, XGD3
const char kXboxMagic[] = "MICROSOFT*XBOX*MEDIA";

char LowerC(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
bool SameName(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (LowerC(a[i]) != LowerC(b[i])) return false;
  return true;
}

}  // namespace

bool Disc::ReadAt(uint64_t offset, size_t length, Bytes& out) const {
  out.resize(length);
  if (!length) return true;
  FILE* f = _wfopen(image_.c_str(), L"rb");
  if (!f) return false;
  const bool ok = _fseeki64(f, int64_t(offset), SEEK_SET) == 0 && std::fread(out.data(), 1, length, f) == length;
  std::fclose(f);
  return ok;
}

int Disc::Add(int parent, std::string name, uint64_t offset, uint64_t size, bool dir) {
  for (int c : entries_[size_t(parent)].children)  // (a duplicate: keep the first)
    if (SameName(entries_[size_t(c)].name, name)) return -1;
  DiscEntry e;
  e.name = std::move(name);
  e.offset = offset;
  e.size = size;
  e.dir = dir;
  e.parent = parent;
  entries_.push_back(std::move(e));
  const int i = int(entries_.size() - 1);
  entries_[size_t(parent)].children.push_back(i);
  return i;
}

void Disc::XboxDir(uint64_t sector, uint32_t size, int parent, int depth) {
  if (!size || depth > 32 || size > (64u << 20)) return;
  Bytes t;
  if (!ReadAt(partition_ + sector * kSector, size, t)) return;
  for (size_t p = 0; p + 14 <= t.size();) {
    if (Le16(&t[p]) == 0xFFFF) {  // padding to the sector end
      p = (p / kSector + 1) * kSector;
      continue;
    }
    const uint32_t start = Le32(&t[p + 4]), length = Le32(&t[p + 8]);
    const uint8_t attr = t[p + 12], nlen = t[p + 13];
    if (p + 14 + nlen > t.size()) break;
    std::string name(reinterpret_cast<const char*>(&t[p + 14]), nlen);
    const bool dir = attr & 0x10;
    const int i = Add(parent, std::move(name), partition_ + uint64_t(start) * kSector, length, dir);
    if (dir && i >= 0) XboxDir(start, length, i, depth + 1);
    p = (p + 14 + nlen + 3) & ~size_t(3);
  }
}

void Disc::IsoDir(uint32_t lba, uint32_t size, int parent, int depth) {
  if (depth > 32 || size > (64u << 20)) return;
  Bytes d;
  if (!ReadAt(uint64_t(lba) * kSector, size, d)) return;
  for (size_t p = 0; p < d.size();) {
    const uint8_t n = d[p];
    if (!n) {  // records don't cross sectors
      p = (p / kSector + 1) * kSector;
      continue;
    }
    if (p + 33 > d.size()) break;
    const uint32_t ext = Le32(&d[p + 2]), len = Le32(&d[p + 10]);
    const uint8_t flags = d[p + 25], nl = d[p + 32];
    if (p + 33 + nl > d.size()) break;
    if (!(nl == 1 && (d[p + 33] == 0 || d[p + 33] == 1))) {
      std::string name(reinterpret_cast<const char*>(&d[p + 33]), nl);
      if (const size_t semi = name.find(';'); semi != std::string::npos) name.resize(semi);
      const bool dir = flags & 2;
      if (!dir && !name.empty() && name.back() == '.') name.pop_back();
      const int i = Add(parent, std::move(name), uint64_t(ext) * kSector, len, dir);
      if (dir && i >= 0) IsoDir(ext, len, i, depth + 1);
    }
    p += n;
  }
}

bool Disc::Open(const std::wstring& image, std::string* error) {
  auto fail = [&](const char* why) {
    if (error) *error = why;
    entries_.clear();
    return false;
  };
  image_ = image;
  entries_.clear();
  id_.clear();
  entries_.push_back(DiscEntry{"", 0, 0, true, -1, {}});
  Bytes b;
  xbox_ = false;
  for (uint64_t off : kXboxOffsets)
    if (ReadAt(off + 32 * kSector, 28, b) && !std::memcmp(b.data(), kXboxMagic, 20)) {
      xbox_ = true;
      partition_ = off;
      XboxDir(Le32(&b[20]), Le32(&b[24]), 0, 0);
      break;
    }
  if (!xbox_) {
    if (!ReadAt(16 * kSector, kSector, b)) return fail("the file could not be read");
    if (b[0] != 1 || std::memcmp(&b[1], "CD001", 5)) return fail("not an Xbox / Xbox 360 or PS2 disc image");
    IsoDir(Le32(&b[156 + 2]), Le32(&b[156 + 10]), 0, 0);
  }
  if (entries_.size() < 2) return fail("the disc's file table is empty");
  // the game's id: the XEX's execution info (key 0x40006) / SYSTEM.CNF BOOT2
  if (xbox_) {
    const int x = Find("default.xex");
    Bytes h;
    if (x >= 0 && Read(x, 0, 0x10000, h) && h.size() >= 0x18 && !std::memcmp(h.data(), "XEX2", 4)) {
      const uint32_t n = std::min<uint32_t>(Be32(&h[0x14]), 64);
      for (uint32_t k = 0; k < n && 0x18 + k * 8 + 8 <= h.size(); ++k)
        if (Be32(&h[0x18 + k * 8]) == 0x00040006) {
          const uint32_t v = Be32(&h[0x18 + k * 8 + 4]);
          if (v + 16 <= h.size()) {
            char t[16];
            std::snprintf(t, sizeof t, "%08X", Be32(&h[v + 12]));
            id_ = t;
          }
        }
    }
  } else {
    const int c = Find("SYSTEM.CNF");
    Bytes h;
    if (c >= 0 && ReadAll(c, h)) {
      const std::string s(h.begin(), h.end());
      if (size_t at = s.find("BOOT2"); at != std::string::npos) {  // BOOT2 = cdrom0:\SLES_548.79;1 (the serial)
        const size_t eq = s.find('=', at), eol = s.find_first_of("\r\n", at);
        if (eq != std::string::npos && eq < eol) {
          std::string v = s.substr(eq + 1, eol == std::string::npos ? std::string::npos : eol - eq - 1);
          if (const size_t semi = v.find(';'); semi != std::string::npos) v.resize(semi);
          if (const size_t sl = v.find_last_of("\\/:"); sl != std::string::npos) v = v.substr(sl + 1);
          while (!v.empty() && v.back() == ' ') v.pop_back();
          while (!v.empty() && v.front() == ' ') v.erase(0, 1);
          id_ = v;
        }
      }
    }
  }
  return true;
}

int Disc::Find(const std::string& path) const {
  if (entries_.empty()) return -1;
  int cur = 0;
  size_t at = 0;
  while (at <= path.size()) {
    size_t end = path.find_first_of("/\\", at);
    if (end == std::string::npos) end = path.size();
    const std::string part = path.substr(at, end - at);
    at = end + 1;
    if (part.empty() || part == ".") continue;
    int next = -1;
    for (int c : entries_[size_t(cur)].children)
      if (SameName(entries_[size_t(c)].name, part)) {
        next = c;
        break;
      }
    if (next < 0) return -1;
    cur = next;
  }
  return cur;
}

std::string Disc::PathOf(int i) const {
  std::string s;
  for (; i > 0; i = entries_[size_t(i)].parent) s = entries_[size_t(i)].name + (s.empty() ? "" : "/" + s);
  return s;
}

bool Disc::Read(int i, uint64_t offset, uint64_t length, Bytes& out) const {
  const DiscEntry& e = At(i);
  if (e.dir) return false;
  offset = std::min(offset, e.size);
  length = std::min(length, e.size - offset);
  return ReadAt(e.offset + offset, size_t(length), out);
}

}  // namespace svrfmt
