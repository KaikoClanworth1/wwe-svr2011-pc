// Superstar Threads for superstar mods.
//
// Threads lists the superstars of edit\edit.pac EDIT/SSTL: one PACH child, a u16 count, then 484-byte
// records {u32 id (little-endian), 50 u32 part-name string ids (-1 padded), each part's material groups,
// default colours}. A superstar's three slots are the last 324 bytes of his 1056-byte profile (+0x2DC,
// +0x348, +0x3B4: 108 bytes each, +0 = -1 when empty), saved with the profile in SaveData.dat.
//
// A mod whose model is its base's (the base's attire-1 models, byte for byte: an attire swap, a renamed
// copy) gets the base's record under its own id: threads.pac in Mods\SuperstarOverlay holds the game's
// SSTL plus those records as EDIT/SSTM, and the game's lookup of /EDIT/SSTL is pointed at it. A mod with a
// model of its own isn't listed - the base's parts would colour the wrong materials.
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
constexpr size_t kRecord = 484;

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
  for (uint32_t i = 0; i < count; ++i) {
    const size_t o = 2 + i * kRecord;
    at[uint32_t(list[o] | list[o + 1] << 8 | list[o + 2] << 16 | uint32_t(list[o + 3]) << 24)] = o;
  }
  std::vector<ThreadsMod> listed;
  std::string names;
  for (const auto& m : mods) {
    const auto b = at.find(m.base);
    if (b == at.end() || at.count(m.slot)) continue;  // (the base isn't in Threads either)
    char nm[16];
    std::snprintf(nm, sizeof nm, "ch%03u.pac", m.base);
    const auto base_models = Attire1Models(game / "pac" / "ch" / nm);
    const auto own = Attire1Models(m.ch_pac);
    if (base_models.empty() || own != base_models) {
      REXLOG_INFO("[svr2011] superstar threads: {} not listed - its model isn't its base's (no parts to edit yet)",
                  m.folder);
      continue;
    }
    svrfmt::Bytes rec(list.begin() + long(b->second), list.begin() + long(b->second + kRecord));
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
