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

// ---- EPAC / EPK8
// EPAC: 12-byte entries {name 4, sector, size / 0x100}, a group's count word
// = entries x 3. EPK8 (characters, entrances): 16-byte entries {name 8,
// sector, size / 0x100}, the count word (u16) = entries x 4, then a u16 group
// number and a u32 kept as is. Both lay entries on 0x800 sectors from 0x4000.
struct EpacEntry {
  std::string name;  // 4 chars (EPK8: 8)
  Bytes data;
};
struct EpacGroup {
  std::string type;  // 4 chars
  std::vector<EpacEntry> entries;
  uint16_t number = 0;  // EPK8: the u16 after the count
  uint32_t extra = 0;   // EPK8: the u32 after that
};
struct Epac {
  Bytes header;   // bytes 0..0x800
  std::vector<EpacGroup> groups;
  Bytes trailer;  // packer footer ("EOP5/plugin version ..."), kept on repack
  bool epk8 = false;
  int32_t toc_extra = 0;  // EPK8: header +4 minus the table's size (kept, so a repack is byte-identical)
};
bool EpacRead(const Bytes& in, Epac& out);  // EPAC or EPK8
Bytes EpacWrite(const Epac& e);

}  // namespace svrfmt
