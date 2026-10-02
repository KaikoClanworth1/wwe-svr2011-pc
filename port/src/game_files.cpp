// The PC port's changes to the installed disc files, made at startup (once;
// a patched file is left alone), so an install from the launcher needs no
// extra tools. The same changes as tools/patch_strings.py and
// tools/patch_menu.py (keep them in sync):
//   pac/string.pac     console wording -> PC wording (dlc.cpp's table)
//   pac/menu/menu.pac  GRAPHICS in MY WWE -> OPTIONS, ACHIEVEMENTS in MY WWE,
//                      EXIT in the main menu (the port supplies their labels,
//                      menu_hooks.cpp)
#include "game_files.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <utility>
#include <vector>

#include <rex/logging.h>

#include "dlc.h"

namespace {

// menu.pac: MFLO/0000 is a table of 0x74-byte records (big-endian):
// +0x00 label string id, +0x04 description id, +0x18 group, +0x1C node id
// (ascending: the game finds a node by binary search), +0x3C flags (2 = last
// of its group), +0x40 "NEW" badge / skip bit (+0x41 bit 0).
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

// The record with this label in this group (0: none).
size_t Find(const std::vector<uint8_t>& d, uint32_t label, uint32_t group) {
  const uint32_t total = Be32(d, kMflo + 4);
  for (uint32_t i = 0; i < total; ++i) {
    const size_t o = kFirst + i * kRec;
    if (Be32(d, o) == label && Be32(d, o + 0x18) == group) return o;
  }
  return 0;
}

constexpr uint32_t kNoText = 0x05F5E0FF;

// A new last entry of `group` after `after_label`: a copy of `template_label`
// (0: of that entry) with the label and description ids, and the node id of
// the entry before it (keeps the ids in order).
bool AddEntry(std::vector<uint8_t>& d, uint32_t after_label, uint32_t group, uint32_t new_label,
              uint32_t template_label = 0, uint32_t description = kNoText) {
  const uint32_t total = Be32(d, kMflo + 4);
  const size_t anchor = Find(d, after_label, group);
  if (!anchor || !(Be32(d, anchor + 0x3C) & 2)) return false;
  const size_t source = template_label ? Find(d, template_label, group) : anchor;
  if (!source) return false;
  const size_t end = kFirst + total * kRec;
  if (end + kRec > kMflo + kMfloSlot) return false;
  uint8_t rec[kRec];
  std::memcpy(rec, &d[source], kRec);
  SetBe32(rec + 0x00, new_label);
  SetBe32(rec + 0x04, description);
  SetBe32(rec + 0x1C, Be32(d, anchor + 0x1C));
  SetBe32(rec + 0x3C, Be32(d, source + 0x3C) | 2);  // the group's last
  SetBe32(rec + 0x40, 0);                           // no "NEW" badge
  SetBe32(&d[anchor + 0x3C], Be32(d, anchor + 0x3C) & ~2u);
  const size_t at = anchor + kRec;
  std::memmove(&d[at + kRec], &d[at], end - at);
  std::memcpy(&d[at], rec, kRec);
  SetBe32(&d[kMflo + 4], total + 1);
  SetBe32(&d[kMflo + 8], Be32(d, kMflo + 8) + 1);
  return true;
}

// ACHIEVEMENTS (v1.0-v1.0.3) kept TEAM MANAGEMENT's node id (0x1A) after
// OPTIONS (0x1C): out of order, the game's binary search couldn't find
// OPTIONS, so B did nothing in MY WWE -> OPTIONS (no parent). It takes
// OPTIONS' id. True when changed.
bool FixNodeOrder(std::vector<uint8_t>& d) {
  const size_t options = Find(d, 0xA030, 0x05), achievements = Find(d, 0xAFC2, 0x05);
  if (!options || !achievements || Be32(d, achievements + 0x1C) == Be32(d, options + 0x1C)) return false;
  SetBe32(&d[achievements + 0x1C], Be32(d, options + 0x1C));
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
  // The table's header (records, shown records): the disc's, after the first
  // patch (GRAPHICS, EXIT: v0.1-v0.3) and after this one (+ ACHIEVEMENTS).
  auto state = [&] { return std::pair(Be32(d, kMflo + 4), Be32(d, kMflo + 8)); };
  using State = std::pair<uint32_t, uint32_t>;
  const bool patched = state() == State(0xE5, 0xE0) && Find(d, 0xAFC4, 0x11);
  if (patched && !FixNodeOrder(d)) return;  // patched already
  // Room for the records: hidden ones (never shown) go, the main menu's second
  // ONLINE and a NEW SUPERSTAR copy; the table can't move (tools/patch_menu.py).
  if (!patched && state() == State(0xE4, 0xDC)) {
    if (!DropSkipped(d, 0xA02E, 0x01) || !AddEntry(d, 0xA050, 0x11, 0xAFC0) ||  // GRAPHICS after CHEAT CODES
        !AddEntry(d, 0xA04B, 0x01, 0xAFC1)) {                                     // EXIT after SHOP
      REXLOG_WARN("{}: unexpected menu table; not patched", file.string());
      return;
    }
  }
  if (!patched && state() == State(0xE5, 0xDE)) {
    if (!Find(d, 0xAFC0, 0x11) || !DropSkipped(d, 0xA47E, 0x0E) ||
        !AddEntry(d, 0xA030, 0x05, 0xAFC2, 0xA0BD, 0xAFC3)) {  // ACHIEVEMENTS after OPTIONS
      REXLOG_WARN("{}: unexpected menu table; not patched", file.string());
      return;
    }
  }
  // (v1.0.4: + LANGUAGE)
  if (!patched && (state() != State(0xE5, 0xDF) || !Find(d, 0xAFC2, 0x05) || !DropSkipped(d, 0xA47F, 0x0E) ||
                   !AddEntry(d, 0xAFC0, 0x11, 0xAFC4))) {  // LANGUAGE after GRAPHICS
    REXLOG_WARN("{}: unexpected menu table; not patched", file.string());
    return;
  }
  FixNodeOrder(d);  // (v1.0.4)
  const auto tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
    if (!out) return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, file, ec);
  if (!ec) REXLOG_INFO("{}: {}", file.string(), patched ? "MY WWE -> OPTIONS back fixed (node order)"
                                                         : "added GRAPHICS, LANGUAGE, ACHIEVEMENTS and EXIT");
}

}  // namespace

namespace svr2011 {

void PatchGameFiles(const std::filesystem::path& game_dir) {
  PatchOnlineStrings(game_dir / "pac" / "string.pac");
  PatchMenu(game_dir / "pac" / "menu" / "menu.pac");
}

}  // namespace svr2011
