// Superstar Threads for superstar mods.
//
// Threads lists the superstars of edit\edit.pac EDIT/SSTL: one PACH child, a u16 count, then 484-byte
// records {u32 id (little-endian), 48 u32 part-name string ids (4 attires x 12, -1 = none), per part an
// area / view / kind byte, default RGB}. Part p of an attire paints channel (p-1)%4 of that attire's
// Mask1/2/3 texture in the model. A superstar's three slots are the last 324 bytes of his 1056-byte
// profile (+0x2DC, +0x348, +0x3B4: 108 bytes each, +0 = -1 when empty), saved with it in SaveData.dat.
//
// threads.pac in Mods\SuperstarOverlay holds the game's SSTL plus a record for each listed mod as
// EDIT/SSTM, and the game's lookup of /EDIT/SSTL is pointed at it. A mod gets
// - the base's record under its own id when its model is its base's (the attire-1 models, byte for byte:
//   an attire swap, a renamed copy);
// - else one from its threads.txt (tools/threads_parts.py names its masks' channels with stock part
//   names; the bytes come from the stock parts of the same name);
// - else none: not listed.
//
// A mod's slots are kept by its folder (Saves\.threads\<folder>.bin), not by the id it holds this run: put
// into its profile as it is set up (never the base's slots), written when the game saves.
#include "superstar_threads.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/ppc.h>

#include "../modmaker/svrfmt/pac.h"

#include "generated/default/svr2011_init.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kIdToIndex = 0x82DB3610, kProfiles = 0x82E7C920, kProfileSize = 1056;
constexpr uint32_t kSlotsAt = 0x2DC, kSlotSize = 108, kSlots = 3, kSlotsSize = kSlotSize * kSlots;
// a record: u32 id, 48 part names (4 attires x 12; part p of an attire paints channel (p-1)%4 of its
// Mask1/2/3), then per part its area (camera), view and kind bytes, then its default RGB
constexpr size_t kRecord = 484, kParts = 48, kAreaAt = 196, kViewAt = 244, kKindAt = 292, kColourAt = 340;

uint32_t g_alias_slash = 0, g_alias_bare = 0;  // guest "/EDIT/SSTM", "EDIT/SSTM"
fs::path g_dir;                                // Saves\.threads
std::mutex g_mutex;
std::vector<svr2011::ThreadsMod> g_mods;              // the listed mods
std::map<std::string, std::vector<uint8_t>> g_saved;  // folder -> its 324 bytes as last saved / loaded

std::vector<uint8_t> EmptySlots() {
  std::vector<uint8_t> s(kSlotsSize, 0);
  for (uint32_t k = 0; k < kSlots; ++k) std::memset(s.data() + k * kSlotSize, 0xFF, 4);
  return s;
}
bool IsEmpty(const uint8_t* s) {
  for (uint32_t k = 0; k < kSlots; ++k)
    if (std::memcmp(s + k * kSlotSize, "\xFF\xFF\xFF\xFF", 4)) return false;
  return true;
}

// A pac's attire-1 models: the EMD entries named "<6-digit id>0<kind>", by kind.
std::map<char, svrfmt::Bytes> Attire1Models(const fs::path& pac) {
  std::map<char, svrfmt::Bytes> out;
  svrfmt::Bytes raw;
  svrfmt::Epac e;
  if (!svrfmt::ReadFile(pac.string(), raw) || !svrfmt::EpacRead(raw, e)) return out;
  for (const auto& g : e.groups) {
    if (g.type.rfind("EMD", 0)) continue;
    for (const auto& x : g.entries)
      if (x.name.size() == 8 && x.name[6] == '0') out[x.name[7]] = x.data;
  }
  return out;
}

// A mod's threads.txt (tools/threads_parts.py) -> its record (id left 0): lines "<attire>.<part>=<name id>,<rrggbb>"
// with a part name stock records use (its bytes from them). False with why: no file, or nothing usable in it.
bool PartsRecord(const fs::path& file, const std::map<uint32_t, uint32_t>& codes, svrfmt::Bytes& rec, std::string& why) {
  FILE* f = std::fopen(file.string().c_str(), "rb");
  if (!f) {
    why = "its model isn't its base's and it has no threads.txt (Mod Maker: make Threads parts)";
    return false;
  }
  rec.assign(kRecord, 0);
  std::memset(rec.data() + 4, 0xFF, kParts * 4);
  std::memset(rec.data() + kColourAt, 0xFF, kParts * 3);
  int parts = 0, bad = 0;
  char line[256];
  while (std::fgets(line, sizeof line, f)) {
    if (char* c = std::strchr(line, '#')) *c = 0;
    unsigned attire = 0, part = 0, name = 0, rgb = 0;
    char end = 0;
    if (std::sscanf(line, " format = %u", &name) == 1) {
      if (name != 1) bad = 1000;  // (a newer layout: don't guess)
      continue;
    }
    const int got = std::sscanf(line, " %u.%u = %u , %6x %c", &attire, &part, &name, &rgb, &end);
    if (got <= 0) continue;
    const auto code = codes.find(name);
    if (got != 4 || attire < 1 || attire > 4 || part < 1 || part > 12 || code == codes.end()) {
      ++bad;
      continue;
    }
    const size_t k = (attire - 1) * 12 + (part - 1);
    for (int i = 0; i < 4; ++i) rec[4 + k * 4 + i] = uint8_t(name >> (8 * i));
    rec[kAreaAt + k] = uint8_t(code->second >> 16), rec[kViewAt + k] = uint8_t(code->second >> 8);
    rec[kKindAt + k] = uint8_t(code->second);
    rec[kColourAt + k * 3] = uint8_t(rgb >> 16), rec[kColourAt + k * 3 + 1] = uint8_t(rgb >> 8);
    rec[kColourAt + k * 3 + 2] = uint8_t(rgb);
    ++parts;
  }
  std::fclose(f);
  if (bad >= 1000) {
    why = "its threads.txt is a newer format than this game reads";
    return false;
  }
  if (!parts) {
    why = bad ? "its threads.txt has no usable line (" + std::to_string(bad) + " bad)" : "its threads.txt lists no part";
    return false;
  }
  if (bad) REXLOG_WARN("[svr2011] superstar threads: {}: {} line(s) skipped (not attire.part=name id,rrggbb with a game part name)",
                       file.string(), bad);
  return true;
}

std::string Dec(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof b, "%u", v);
  return b;
}

}  // namespace

namespace svr2011 {

void SetThreadsSaves(const fs::path& saves) { g_dir = saves / ".threads"; }

bool BuildThreadsOverlay(rex::memory::Memory* memory, const fs::path& game, const fs::path& overlay,
                         const std::vector<ThreadsMod>& mods) {
  std::error_code ec;
  const fs::path out = overlay / "threads.pac";
  fs::remove(out, ec);
  svrfmt::Bytes raw;
  svrfmt::Epac edit;
  if (mods.empty() || !svrfmt::ReadFile((game / "pac" / "edit" / "edit.pac").string(), raw) ||
      !svrfmt::EpacRead(raw, edit))
    return false;
  const svrfmt::EpacEntry* sstl = nullptr;
  svrfmt::EpacGroup group;
  for (const auto& g : edit.groups)
    if (g.type == "EDIT") {
      group = g;
      for (const auto& x : g.entries)
        if (x.name == "SSTL") sstl = &x;
    }
  std::vector<svrfmt::PachEntry> kids;
  if (!sstl || !svrfmt::PachRead(svrfmt::Unpack(sstl->data), kids) || kids.empty()) {
    REXLOG_WARN("[svr2011] superstar threads: edit.pac has no EDIT/SSTL as expected - mods not listed");
    return false;
  }
  svrfmt::Bytes list = svrfmt::Unpack(kids[0].data);
  const uint32_t count = list.size() >= 2 ? uint32_t(list[0] | list[1] << 8) : 0;
  if (!count || list.size() != 2 + count * kRecord) {
    REXLOG_WARN("[svr2011] superstar threads: SSTL is {} bytes for {} records - mods not listed", list.size(), count);
    return false;
  }
  std::map<uint32_t, size_t> at;  // id -> record offset
  std::map<uint32_t, std::map<uint32_t, int>> uses;  // part name -> {its area/view/kind bytes -> how many parts}
  for (uint32_t i = 0; i < count; ++i) {
    const size_t o = 2 + i * kRecord;
    at[uint32_t(list[o] | list[o + 1] << 8 | list[o + 2] << 16 | uint32_t(list[o + 3]) << 24)] = o;
    for (uint32_t k = 0; k < kParts; ++k) {
      const size_t n = o + 4 + k * 4;
      const uint32_t name = uint32_t(list[n] | list[n + 1] << 8 | list[n + 2] << 16 | uint32_t(list[n + 3]) << 24);
      if (name != 0xFFFFFFFFu)
        ++uses[name][uint32_t(list[o + kAreaAt + k]) << 16 | list[o + kViewAt + k] << 8 | list[o + kKindAt + k]];
    }
  }
  std::map<uint32_t, uint32_t> codes;  // part name -> the bytes stock parts of that name use most
  for (const auto& [name, m] : uses)
    codes[name] = std::max_element(m.begin(), m.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
  std::vector<ThreadsMod> listed;
  std::string names;
  for (const auto& m : mods) {
    if (at.count(m.slot)) continue;
    const auto b = at.find(m.base);  // (end: the base isn't in Threads - the mod can still bring threads.txt)
    char nm[16];
    std::snprintf(nm, sizeof nm, "ch%03u.pac", m.base);
    const auto base_models = Attire1Models(game / "pac" / "ch" / nm);
    const auto own = Attire1Models(m.ch_pac);
    svrfmt::Bytes rec;
    std::string why;
    if (b != at.end() && !base_models.empty() && own == base_models) {
      rec.assign(list.begin() + long(b->second), list.begin() + long(b->second + kRecord));
    } else if (!PartsRecord(m.ch_pac.parent_path() / "threads.txt", codes, rec, why)) {
      REXLOG_INFO("[svr2011] superstar threads: {} not listed - {}", m.folder, why);
      continue;
    }
    for (int k = 0; k < 4; ++k) rec[k] = uint8_t(m.slot >> (8 * k));
    list.insert(list.end(), rec.begin(), rec.end());
    listed.push_back(m);
    names += (names.empty() ? "" : ", ") + m.folder + " (id " + Dec(m.slot) + ")";
  }
  if (listed.empty()) return false;
  const uint32_t n = count + uint32_t(listed.size());
  list[0] = uint8_t(n), list[1] = uint8_t(n >> 8);
  kids[0].data = list;
  svrfmt::EpacEntry entry;
  entry.name = "SSTM";
  entry.data = svrfmt::PachWrite(kids);
  group.entries = {entry};
  edit.groups = {group};
  if (!svrfmt::WriteFile(out.string(), svrfmt::EpacWrite(edit))) {
    REXLOG_WARN("[svr2011] superstar threads: cannot write {}", out.string());
    return false;
  }
  uint8_t* mem = memory->TranslateVirtual<uint8_t*>(0);
  g_alias_slash = memory->SystemHeapAlloc(16);
  g_alias_bare = memory->SystemHeapAlloc(16);
  std::memcpy(mem + g_alias_slash, "/EDIT/SSTM", 11);
  std::memcpy(mem + g_alias_bare, "EDIT/SSTM", 10);
  {
    std::lock_guard lock(g_mutex);
    g_mods = listed;
  }
  REXLOG_INFO("[svr2011] superstar threads: {} mod(s) listed: {}", listed.size(), names);
  return true;
}

uint32_t ThreadsAlias(const char* name) {
  if (!g_alias_slash || !name) return 0;
  if (!std::strcmp(name, "/EDIT/SSTL")) return g_alias_slash;
  if (!std::strcmp(name, "EDIT/SSTL")) return g_alias_bare;
  return 0;
}

void ThreadsSlots(uint8_t* profile, const std::string& folder) {
  if (!profile || g_dir.empty()) return;
  std::lock_guard lock(g_mutex);
  auto it = g_saved.find(folder);
  if (it == g_saved.end()) {
    std::vector<uint8_t> s;
    if (FILE* f = std::fopen((g_dir / (folder + ".bin")).string().c_str(), "rb")) {
      s.resize(kSlotsSize);
      if (std::fread(s.data(), 1, kSlotsSize, f) != kSlotsSize) s.clear();
      std::fclose(f);
    }
    it = g_saved.emplace(folder, s.empty() ? EmptySlots() : s).first;
  }
  std::memcpy(profile + kSlotsAt, it->second.data(), kSlotsSize);
}

}  // namespace svr2011

// The save written (sub_827235D8 builds it from the records and profiles): each listed mod's slots, as its
// profile holds them now, into Saves\.threads\<folder>.bin (removed when all three are empty).
REX_EXTERN(__imp__sub_827235D8);
REX_HOOK_RAW(sub_827235D8) {
  {
    std::lock_guard lock(g_mutex);
    std::error_code ec;
    for (const auto& m : g_mods) {
      const uint32_t si = uint32_t(base[kIdToIndex + m.slot * 2] << 8 | base[kIdToIndex + m.slot * 2 + 1]);
      if (si >= 512) continue;
      const uint8_t* s = base + kProfiles + si * kProfileSize + kSlotsAt;
      std::vector<uint8_t> now(s, s + kSlotsSize);
      auto& was = g_saved[m.folder];
      if (was == now) continue;
      const fs::path f = g_dir / (m.folder + ".bin");
      if (IsEmpty(s)) {
        fs::remove(f, ec);
      } else {
        fs::create_directories(g_dir, ec);
        const fs::path part = fs::path(f).concat(".part");
        if (FILE* o = std::fopen(part.string().c_str(), "wb")) {
          const bool ok = std::fwrite(now.data(), 1, now.size(), o) == now.size();
          std::fclose(o);
          if (ok) fs::rename(part, f, ec);
        }
      }
      was = now;
      REXLOG_INFO("[svr2011] superstar threads: {} - slots saved ({})", m.folder, IsEmpty(s) ? "all empty" : "edited");
    }
  }
  __imp__sub_827235D8(ctx, base);
}
