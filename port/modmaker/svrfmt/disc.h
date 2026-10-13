// Game files read straight from a disc image (checklist D1): an Xbox 360 /
// Xbox disc (XDVDFS, at one of 4 partition offsets) or a PS2 disc (ISO 9660).
// The directory tables are read once at Open; file data only when asked for.
// Same formats as port/tools/discfs.py (the converters' reader).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "bytes.h"

namespace svrfmt {

struct DiscEntry {
  std::string name;           // as on the disc (PS2: without ";1")
  uint64_t offset = 0;        // byte offset in the image
  uint64_t size = 0;
  bool dir = false;
  int parent = -1;
  std::vector<int> children;  // (directories) in table order
};

class Disc {
 public:
  // False with why (not an image, unreadable).
  bool Open(const std::wstring& image, std::string* error = nullptr);
  bool IsOpen() const { return !entries_.empty(); }
  const std::wstring& Image() const { return image_; }
  const char* Kind() const { return xbox_ ? "Xbox 360 (XDVDFS)" : "PS2 (ISO 9660)"; }
  bool Xbox() const { return xbox_; }
  // XEX title id ("5451080B") or the PS2 serial ("SLES_548.79"); "" if none.
  const std::string& Id() const { return id_; }
  int Root() const { return 0; }
  const DiscEntry& At(int i) const { return entries_[size_t(i)]; }
  int Count() const { return int(entries_.size()); }
  // An entry by path ('/' or '\', any case); -1 if absent.
  int Find(const std::string& path) const;
  std::string PathOf(int i) const;
  // Part of a file (clamped to its size); false on a read error.
  bool Read(int i, uint64_t offset, uint64_t length, Bytes& out) const;
  bool ReadAll(int i, Bytes& out) const { return Read(i, 0, At(i).size, out); }

 private:
  bool ReadAt(uint64_t offset, size_t length, Bytes& out) const;
  void XboxDir(uint64_t sector, uint32_t size, int parent, int depth);
  void IsoDir(uint32_t lba, uint32_t size, int parent, int depth);
  int Add(int parent, std::string name, uint64_t offset, uint64_t size, bool dir);

  std::wstring image_;
  bool xbox_ = false;
  uint64_t partition_ = 0;
  std::string id_;
  std::vector<DiscEntry> entries_;
};

}  // namespace svrfmt
