// Game archives: EPAC (outer, little-endian), PACH (index), BPE (Yuke's
// byte-pair compression). Mirrors port/tools/svrfmt.py (tested on all arena,
// menu and crowd archives: repacks are byte-identical).
#pragma once

#include <string>
#include <vector>

#include "bytes.h"

namespace svrfmt {

// ---- BPE
bool BpeDecode(const Bytes& in, Bytes& out);
// "Stored" blocks (identity pair table + raw data): instant, any decoder reads it.
Bytes BpeEncodeStored(const Bytes& raw);
// Gage byte-pair compression in 4000-byte blocks (the game's block buffer).
Bytes BpeEncode(const Bytes& raw);
inline bool IsBpe(const Bytes& b) { return b.size() >= 16 && !std::memcmp(b.data(), "BPE ", 4); }
// An entry as stored in a PACH, unpacked if it is BPE.
Bytes Unpack(const Bytes& entry);

// ---- PACH
struct PachEntry {
  uint32_t id = 0;
  Bytes data;  // as stored (usually BPE)
};
inline bool IsPach(const Bytes& b) { return b.size() >= 8 && !std::memcmp(b.data(), "PACH", 4); }
bool PachRead(const Bytes& in, std::vector<PachEntry>& out);
Bytes PachWrite(const std::vector<PachEntry>& entries);

// ---- EPAC
struct EpacEntry {
  std::string name;  // 4 chars
  Bytes data;
};
struct EpacGroup {
  std::string type;  // 4 chars
  std::vector<EpacEntry> entries;
};
struct Epac {
  Bytes header;   // bytes 0..0x800
  std::vector<EpacGroup> groups;
  Bytes trailer;  // packer footer ("EOP5/plugin version ..."), kept on repack
};
bool EpacRead(const Bytes& in, Epac& out);
Bytes EpacWrite(const Epac& e);

}  // namespace svrfmt
