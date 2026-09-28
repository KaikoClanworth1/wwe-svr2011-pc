// The PC port's changes to the installed disc files, made at startup (once;
// a patched file is left alone), so an install from the launcher needs no
// extra tools. The same changes as tools/patch_strings.py and
// tools/patch_menu.py (keep them in sync):
//   pac/string.pac     console wording -> PC wording (dlc.cpp's table)
//   pac/menu/menu.pac  GRAPHICS in MY WWE -> OPTIONS, EXIT in the main menu
//                      (the port supplies their labels, menu_hooks.cpp)
#include "game_files.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

#include <rex/logging.h>

#include "dlc.h"

namespace {

// menu.pac: MFLO/0000 is a table of 0x74-byte records (big-endian):
// +0x00 label string id, +0x04 description id, +0x18 group, +0x3C flags
// (2 = last of its group), +0x40 "NEW" badge / skip bit (+0x41 bit 0).
constexpr size_t kMflo = 0x25F000, kMfloSlot = 0x6800, kRec = 0x74, kFirst = kMflo + 0x28;

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return uint32_t(d[o]) << 24 | uint32_t(d[o + 1]) << 16 | uint32_t(d[o + 2]) << 8 | d[o + 3];
}
void SetBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

bool DropSkipped(std::vector<uint8_t>& d, uint32_t label, uint32_t group) {
  const uint32_t total = Be32(d, kMflo + 4);
  for (uint32_t i = 0; i < total; ++i) {
    const size_t o = kFirst + i * kRec;
    if (Be32(d, o) != label || Be32(d, o + 0x18) != group) continue;
    if (!(d[o + 0x41] & 1) || (Be32(d, o + 0x3C) & 2)) continue;
    const size_t end = kFirst + total * kRec;
    std::memmove(&d[o], &d[o + kRec], end - o - kRec);
    std::memset(&d[end - kRec], 0, kRec);
    SetBe32(&d[kMflo + 4], total - 1);
    return true;
  }
  return false;
}

bool AddEntry(std::vector<uint8_t>& d, uint32_t after_label, uint32_t group, uint32_t new_label) {
  const uint32_t total = Be32(d, kMflo + 4);
  size_t anchor = 0;
  for (uint32_t i = 0; i < total; ++i) {
    const size_t o = kFirst + i * kRec;
    if (Be32(d, o) == after_label && Be32(d, o + 0x18) == group) {
      anchor = o;
      break;
    }
  }
  if (!anchor || !(Be32(d, anchor + 0x3C) & 2)) return false;
  const size_t end = kFirst + total * kRec;
  if (end + kRec > kMflo + kMfloSlot) return false;
  uint8_t rec[kRec];
  std::memcpy(rec, &d[anchor], kRec);
  SetBe32(rec + 0x00, new_label);
  SetBe32(rec + 0x04, 0x05F5E0FF);  // no description text
  SetBe32(rec + 0x40, 0);           // no "NEW" badge
  SetBe32(&d[anchor + 0x3C], Be32(d, anchor + 0x3C) & ~2u);
  const size_t at = anchor + kRec;
  std::memmove(&d[at + kRec], &d[at], end - at);
  std::memcpy(&d[at], rec, kRec);
  SetBe32(&d[kMflo + 4], total + 1);
  SetBe32(&d[kMflo + 8], Be32(d, kMflo + 8) + 1);
  return true;
}

void PatchMenu(const std::filesystem::path& file) {
  std::vector<uint8_t> d;
  {
    std::ifstream in(file, std::ios::binary);
    if (!in) return;
    d.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  if (d.size() < kMflo + kMfloSlot) return;
  if (Be32(d, kMflo + 4) != 0xE4 || Be32(d, kMflo + 8) != 0xDC) return;  // patched already (or not the disc's)
  // Room for the two records: the main menu's hidden second ONLINE record
  // (never shown) goes; the table can't move (see tools/patch_menu.py).
  if (!DropSkipped(d, 0xA02E, 0x01) || !AddEntry(d, 0xA050, 0x11, 0xAFC0) ||  // GRAPHICS after CHEAT CODES
      !AddEntry(d, 0xA04B, 0x01, 0xAFC1)) {                                     // EXIT after SHOP
    REXLOG_WARN("{}: unexpected menu table; not patched", file.string());
    return;
  }
  const auto tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, file, ec);
  if (!ec) REXLOG_INFO("{}: added GRAPHICS (MY WWE -> OPTIONS) and EXIT (main menu)", file.string());
}

}  // namespace

namespace svr2011 {

void PatchGameFiles(const std::filesystem::path& game_dir) {
  PatchOnlineStrings(game_dir / "pac" / "string.pac");
  PatchMenu(game_dir / "pac" / "menu" / "menu.pac");
}

}  // namespace svr2011
