// The Created Superstar saves at start (caw_check.h), for problem reports ("my CAW won't load").
//
// A .cas is 1,347,532 bytes: four attire slots of 149,528 bytes at 746,340 (each: the 256 x 512 list picture,
// DXT5, at +0), the record at 0x1483C4 (+0x20 its SaveData slot, big-endian), and a checksum stored three
// times (big-endian at +12, +0x148FC8, +0x148DBC). SaveData.dat holds the same value in the slot's record
// (0x13FB8 + slot * 0x6BC, +0x65C) and its own checksum at +0x18 (the byte sum of 0x1C..0x812DC). An edit
// that doesn't keep these in step (the Mod Maker's CAW pictures page redoes them) loses the CAW. The value
// itself isn't checked here: the port's logo cache keeps part of a CAW outside the file.
#include "caw_check.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace fs = std::filesystem;

namespace svr2011 {
namespace {

constexpr size_t kCasSize = 1347532, kSlotAt = 746340, kSlotSize = 149528, kPicture = 131072, kRecordAt = 0x1483C4;
constexpr size_t kSum1 = 12, kSum2 = 0x148FC8, kSum3 = 0x148DBC;
constexpr size_t kRecords = 0x13FB8, kRecord = 0x6BC, kRecordSum = 0x65C, kSaveSum = 0x18, kSaveFrom = 0x1C, kSaveTo = 0x812DC;

uint32_t Be32(const std::vector<uint8_t>& d, size_t at) {
  return at + 4 <= d.size() ? uint32_t(d[at]) << 24 | uint32_t(d[at + 1]) << 16 | uint32_t(d[at + 2]) << 8 | d[at + 3] : 0;
}

bool Read(const fs::path& p, std::vector<uint8_t>& out) {
  FILE* f = nullptr;
#ifdef _WIN32
  _wfopen_s(&f, p.c_str(), L"rb");
#else
  f = std::fopen(p.c_str(), "rb");
#endif
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? size_t(n) : 0);
  const bool ok = out.empty() || std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

}  // namespace

void CheckCawSaves(const fs::path& saves) {
  std::error_code ec;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(saves, ec)) {
    std::string n = e.path().filename().string();
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (e.is_regular_file(ec) && n.size() > 19 && n.compare(n.size() - 19, 19, "createsuperstar.cas") == 0)
      files.push_back(e.path());
  }
  if (files.empty()) return;
  std::sort(files.begin(), files.end());
  std::vector<uint8_t> sd;
  const bool have_sd = Read(saves / "SaveData.dat", sd) && sd.size() > kSaveTo;
  if (have_sd) {
    uint64_t sum = 0;
    for (size_t i = kSaveFrom; i < kSaveTo; ++i) sum += sd[i];
    if (uint32_t(sum) != Be32(sd, kSaveSum))
      REXLOG_WARN("caw saves: SaveData.dat's own checksum doesn't match its data - the game may call the save damaged");
  }
  int bad = 0;
  for (const auto& f : files) {
    std::vector<uint8_t> d;
    const std::string name = f.filename().string();
    if (!Read(f, d) || d.size() != kCasSize) {
      ++bad;
      REXLOG_WARN("caw saves: {} is {} bytes, not {} - the game can't load it", name, d.size(), kCasSize);
      continue;
    }
    const uint32_t slot = uint32_t(d[kRecordAt + 0x20]) << 8 | d[kRecordAt + 0x21];
    std::string pictures;
    for (int a = 0; a < 4; ++a) {
      const uint8_t* s = &d[kSlotAt + size_t(a) * kSlotSize];
      bool any = false;
      for (size_t i = 0; i < kPicture && !any; i += 97) any = s[i] != 0;
      if (any) pictures += (pictures.empty() ? "" : "+") + std::to_string(a + 1);
    }
    const uint32_t s1 = Be32(d, kSum1), s2 = Be32(d, kSum2), s3 = Be32(d, kSum3);
    const bool copies = s1 == s2 && s1 == s3;
    const size_t rec = kRecords + size_t(slot) * kRecord + kRecordSum;
    const bool in_sd = have_sd && slot < 50 && rec + 4 <= sd.size();
    const uint32_t sdv = in_sd ? Be32(sd, rec) : 0;
    const bool agree = copies && (!in_sd || sdv == s1);
    if (!agree) ++bad;
    if (agree)
      REXLOG_INFO("caw saves: {} - slot {}, attires with a picture {}, checksums agree{}", name, slot,
                  pictures.empty() ? "none" : pictures, in_sd ? " (with SaveData.dat)" : "");
    else
      REXLOG_WARN("caw saves: {} - slot {}, attires with a picture {}: its checksums DON'T agree (.cas {:08X} / {:08X} / "
                  "{:08X}, SaveData.dat {}) - the game will call it damaged or leave it out",
                  name, slot, pictures.empty() ? "none" : pictures, s1, s2, s3, in_sd ? fmt::format("{:08X}", sdv) : "-");
  }
  REXLOG_INFO("caw saves: {} Created Superstar save(s), {} with a problem", files.size(), bad);
}

}  // namespace svr2011
