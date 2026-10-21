// WWE SmackDown vs. Raw 2011 - weapon mods (see weapon_mods.h).
//
// bgEtc.pac's STG/WPON (and STG3/WPON, the backstage areas') is a PACH: weapon packs by id (each a PACH: the
// model JBOY at 0x0, its texture set at 0xA, ...) and the weapon settings - the text 0x9C40 (the developers',
// not complete) and the table the game uses, 0x9C41: "=A2\1", u32 version 100, u32 size, u32 records at 0x40,
// u32 record size 0x68, ..., u32 count at +0x1C; records (little-endian) {u16 model = pack id x 10, u16 type
// (the grip / moves family), u16 hits to break (0 never), u16 weight (0 light .. 3 can't lift), f32 damage,
// 5 x {f32 x, y, z, radius} hit spheres, sounds ...}. A mod's pack goes in by its id, its record is a copy of
// the like= weapon's with the model number its own.

#include "weapon_mods.h"

#include "crowd_fight.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"

#include "no_mods.h"
#include "../modmaker/svrfmt/jboy.h"
#include "../modmaker/svrfmt/pac.h"

namespace {

namespace fs = std::filesystem;

constexpr int kFirstId = 160, kLastId = 255;  // (one byte: the placed lists keep a weapon as u8)
constexpr uint32_t kSettingsTable = 0x9C41;
constexpr size_t kRecordAt = 0x40, kRecord = 0x68;

std::vector<svr2011::WeaponMod> g_mods;

fs::path Game() { return rex::filesystem::GetExecutableFolder(); }

std::vector<std::string> Lines(const fs::path& file) {
  std::vector<std::string> out;
  std::ifstream in(file);
  for (std::string l; std::getline(in, l);) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    out.push_back(l);
  }
  return out;
}

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void PutLe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 16), p[3] = uint8_t(v >> 24); }

// Why a weapon pack's model (PACH entry 0) can't be used, or null.
const char* BadModel(const svrfmt::Bytes& pack) {
  std::vector<svrfmt::PachEntry> kids;
  if (!svrfmt::PachRead(pack, kids)) return "can't be read";
  for (const auto& k : kids) {
    if (k.id != 0) continue;
    const svrfmt::Bytes d = svrfmt::Unpack(k.data);
    if (!svrfmt::IsJboy(d)) return "is missing (no JBOY at entry 0)";
    auto Be32 = [](const uint8_t* p) {
      return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    // (WWE '13's layout: the first mesh, at 8 + the mesh table's offset, names its material at +0x7C)
    const uint32_t meshes = Be32(&d[0x18]);
    const size_t o = 8 + size_t(Be32(&d[0x1C]));
    if (meshes && o + 0xB8 <= d.size() && Be32(&d[o + 0x78]) < 0x10000 && d[o + 0x7C] == 'y')
      return "is in WWE '13's / 2K14's format (not a SvR2011 model)";
    svrfmt::Model model;
    if (!svrfmt::JboyRead(d, model)) return "can't be read";
    return nullptr;
  }
  return "is missing (no entry 0)";
}

// Stable ids: Mods/Weapons/ids.txt ("<folder>\t<id>" a line); new folders get the first free id.
std::map<std::string, int> ReadIds(const fs::path& file) {
  std::map<std::string, int> ids;
  for (const auto& l : Lines(file)) {
    const size_t tab = l.find('\t');
    if (tab != std::string::npos) ids[l.substr(0, tab)] = std::atoi(l.c_str() + tab + 1);
  }
  return ids;
}

// The settings table with the mods' records added (empty: not the expected table).
svrfmt::Bytes AddRecords(const svrfmt::Bytes& table, std::vector<svr2011::WeaponMod>& mods) {
  if (table.size() < kRecordAt || std::memcmp(table.data(), "=A2", 3) || Le32(&table[16]) != kRecord) return {};
  uint32_t count = Le32(&table[0x1C]);
  if (kRecordAt + size_t(count) * kRecord > table.size()) return {};
  // (the table ends in empty records: the used ones are those with a model number)
  std::vector<svrfmt::Bytes> records;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* r = &table[kRecordAt + i * kRecord];
    if (r[0] | r[1]) records.emplace_back(r, r + kRecord);
  }
  for (auto& m : mods) {
    if (!m.id) continue;  // (left out)
    const svrfmt::Bytes* like = nullptr;
    for (const auto& r : records)
      if (int(r[0] | r[1] << 8) == m.like * 10) like = &r;
    if (!like) {
      REXLOG_WARN("[svr2011] weapon mods: {} - like={} is no weapon of the game: left out", m.folder, m.like);
      m.id = 0;
      continue;
    }
    svrfmt::Bytes r = *like;
    const int model = m.id * 10;
    r[0] = uint8_t(model), r[1] = uint8_t(model >> 8);
    records.push_back(r);
  }
  std::sort(records.begin(), records.end(), [](const svrfmt::Bytes& a, const svrfmt::Bytes& b) {
    return (a[0] | a[1] << 8) < (b[0] | b[1] << 8);
  });
  records.emplace_back(kRecord, 0), records.emplace_back(kRecord, 0);  // (the two empty ones the game's has)
  svrfmt::Bytes out(table.begin(), table.begin() + kRecordAt);
  for (const auto& r : records) out.insert(out.end(), r.begin(), r.end());
  PutLe32(&out[8], uint32_t(out.size()));
  PutLe32(&out[0x1C], uint32_t(records.size()));
  return out;
}

}  // namespace

// The weapons in a match come from STG/WPON as the game read it at start (a bgEtc.pac copy served in its place
// was never what the loader looked in, 21 Oct). Every reader looks a child up through a handle - {u32 the PACH in memory, u32 where its children's data starts
// (from the PACH: 8 + count * 12)} - with sub_826A0D98(handle, id), a binary search by id: the weapon loader
// (sub_8227DD80, through sub_821528D0) and the settings table's reader (sub_8227C660, once). At a search in the
// game's WPON / WPBS (STG, or STG3 the backstage areas') the handle is pointed at ours (LoadWeaponMods; put in guest
// memory at the first search); the game's own pointer is put back before the handle frees what it holds (sub_826A0D78), so the game
// frees its own buffer.
rex::memory::Memory* g_memory = nullptr;

struct Swapped {
  uint32_t handle, original, start;  // the game's PACH and data start the handle had
};
struct WponCopy {
  std::string group;  // "STG/WPON", "STG/WPBS", "STG3/WPON"
  svrfmt::Bytes data;  // the copy's table, unpacked
  uint32_t guest = 0;  // ... in guest memory
  std::vector<Swapped> handles;  // pointed at the copy
};
std::vector<WponCopy> g_wpons;
std::mutex g_wpons_mutex;  // (look-ups come from the loader's thread and others)

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void PutBe32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }

// Whether the PACH in memory is the game's own of this copy: the copy's ids without the mods' (160-255).
bool IsOriginalOf(const uint8_t* pach, const WponCopy& c) {
  const uint32_t n = Le32(pach + 4), mine = Le32(c.data.data() + 4);
  if (n == 0 || n >= mine) return false;
  uint32_t j = 0;
  for (uint32_t i = 0; i < mine; ++i) {
    const uint32_t id = Le32(c.data.data() + 8 + i * 12);
    if (j < n && Le32(pach + 8 + j * 12) == id) ++j;
    else if (id < uint32_t(kFirstId) || id > uint32_t(kLastId)) return false;
  }
  return j == n;
}

void PointAtCopy(uint8_t* base, uint32_t handle) {
  if (g_wpons.empty() || !g_memory || !handle) return;
  const uint32_t now = Be32(base + handle);
  if (!now || std::memcmp(base + now, "PACH", 4)) return;
  std::lock_guard lock(g_wpons_mutex);
  for (auto& c : g_wpons) {
    if (c.data.empty() || now == c.guest || !IsOriginalOf(base + now, c)) continue;
    const uint32_t n = Le32(base + now + 4), mine = Le32(c.data.data() + 4);
    const uint32_t start = Be32(base + handle + 4);
    if (start != 8 + n * 12) {
      REXLOG_WARN("[svr2011] weapon mods: the {} weapons' handle isn't as known (data at {:X}, {} weapons) - the game's only",
                  c.group, start, n);
      c.data.clear();
      return;
    }
    if (!c.guest) {
      c.guest = g_memory->SystemHeapAlloc(uint32_t(c.data.size()));
      if (!c.guest) {
        REXLOG_WARN("[svr2011] weapon mods: no guest memory for the {} weapons ({} KB) - the game's only", c.group,
                    c.data.size() / 1024);
        c.data.clear();
        return;
      }
      std::memcpy(base + c.guest, c.data.data(), c.data.size());
      REXLOG_INFO("[svr2011] weapon mods: the {} weapons looked up in ours ({} weapons, the game's {})", c.group, mine, n);
    }
    c.handles.push_back({handle, now, start});
    PutBe32(base + handle, c.guest);
    PutBe32(base + handle + 4, 8 + mine * 12);
    return;
  }
}

REX_EXTERN(__imp__sub_826A0D78);
REX_HOOK_RAW(sub_826A0D78) {
  if (!g_wpons.empty()) {
    std::lock_guard lock(g_wpons_mutex);
    for (auto& c : g_wpons)
      for (auto it = c.handles.begin(); it != c.handles.end(); ++it)
        if (it->handle == ctx.r3.u32) {
          if (Be32(base + it->handle) == c.guest) {
            PutBe32(base + it->handle, it->original);
            PutBe32(base + it->handle + 4, it->start);
          }
          c.handles.erase(it);
          break;
        }
  }
  __imp__sub_826A0D78(ctx, base);
}

REX_EXTERN(__imp__sub_826A0D98);
REX_HOOK_RAW(sub_826A0D98) {
  static const bool log = std::getenv("SVR2011_TEST_WEAPON_LOOKUP") != nullptr;  // (test aid: the settings' look-ups)
  const uint32_t handle = ctx.r3.u32, id = ctx.r4.u32, lr = uint32_t(ctx.lr);
  PointAtCopy(base, handle);
  if (const uint32_t crowd = svr2011::CrowdCollisionId(base, lr, id); crowd != id) {  // (crowd_fight.h)
    const auto saved = ctx;
    ctx.r4.u64 = crowd;
    __imp__sub_826A0D98(ctx, base);
    if (ctx.r3.s32 >= 0) return;
    ctx = saved;  // (no crowd collision in this arena: its own)
  }
  __imp__sub_826A0D98(ctx, base);
  if (log && ctx.r3.s32 < 0 && lr >= 0x8227DD80 && lr < 0x8227ED88)  // (the loader: a search that found nothing)
    REXLOG_INFO("[svr2011] weapon lookup: id {} not in PACH {:08X} ({} children, first id {}) (lr {:08X})", id,
                Be32(base + handle), Le32(base + Be32(base + handle) + 4), Le32(base + Be32(base + handle) + 8), lr);
  if (log && (id & 0xFFFF) == kSettingsTable)
    REXLOG_INFO("[svr2011] weapon lookup: settings in PACH {:08X} ({} children) -> {} (lr {:08X})", Be32(base + handle),
                Le32(base + Be32(base + handle) + 4), ctx.r3.s32, lr);
}

REX_EXTERN(__imp__sub_821528D0);
REX_HOOK_RAW(sub_821528D0) {
  static const bool log = std::getenv("SVR2011_TEST_WEAPON_LOOKUP") != nullptr;  // (test aid: the loader's look-ups)
  const uint32_t pach = ctx.r3.u32, id = ctx.r4.u32, lr = uint32_t(ctx.lr);
  __imp__sub_821528D0(ctx, base);
  if (log && lr >= 0x8227DD80 && lr < 0x8227ED88) {
    const uint8_t* h = base + pach;
    const uint32_t data = uint32_t(h[0]) << 24 | uint32_t(h[1]) << 16 | uint32_t(h[2]) << 8 | h[3];  // (*handle: the PACH)
    const uint8_t* p = base + data;
    const uint32_t n = Le32(p + 4);
    const uint32_t last = n && n < 4096 ? Le32(p + 8 + (n - 1) * 12) : 0;
    REXLOG_INFO("[svr2011] weapon lookup: PACH {:08X} ({:c}{:c}{:c}{:c}, {} children, last id {}) id {} -> {:08X} (lr {:08X})",
                data, char(p[0]), char(p[1]), char(p[2]), char(p[3]), n, last, id, ctx.r3.u32, lr);
    if (!ctx.r3.u32 && n && n < 4096) {
      std::string ids;
      for (uint32_t i = n > 12 ? n - 12 : 0; i < n; ++i)
        ids += fmt::format(" {}@{:X}", Le32(p + 8 + i * 12), Le32(p + 8 + i * 12 + 4));
      REXLOG_INFO("[svr2011] weapon lookup: its last ids (id@offset):{}", ids);
    }
  }
}

namespace svr2011 {

void SetWeaponModsMemory(rex::memory::Memory* memory) { g_memory = memory; }

void LoadWeaponMods() {
  g_mods.clear();
  {
    std::lock_guard lock(g_wpons_mutex);
    g_wpons.clear();
  }
  if (NoMods()) return;  // (Start Without Mods: no_mods.h)
  const fs::path dir = Game() / "Mods" / "Weapons";
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory() && fs::exists(e.path() / "weapon.pac", ec) && !fs::exists(e.path() / "disabled", ec))
      folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  auto ids = ReadIds(dir / "ids.txt");
  bool ids_changed = false;
  for (const auto& f : folders) {
    WeaponMod m;
    m.folder = f.filename().string();
    m.name = m.folder;
    for (const auto& l : Lines(f / "manifest.txt")) {
      if (l.rfind("name=", 0) == 0) m.name = l.substr(5);
      if (l.rfind("like=", 0) == 0) m.like = std::atoi(l.c_str() + 5);
      if (l.rfind("pool=", 0) == 0) {
        m.extreme = l.find("extreme") != std::string::npos;
        m.backstage = l.find("backstage") != std::string::npos;
      }
    }
    if (m.like <= 0) {
      REXLOG_WARN("[svr2011] weapon mods: {} has no like=<a stock weapon> in its manifest: left out", m.folder);
      continue;
    }
    auto it = ids.find(m.folder);
    if (it == ids.end() || it->second < kFirstId || it->second > kLastId) {
      int id = kFirstId;
      auto taken = [&](int v) {
        for (const auto& [k, x] : ids)
          if (x == v && k != m.folder) return true;
        return false;
      };
      while (id <= kLastId && taken(id)) ++id;
      if (id > kLastId) {
        REXLOG_WARN("[svr2011] weapon mods: no weapon id left for {} (ids {}-{})", m.folder, kFirstId, kLastId);
        continue;
      }
      ids[m.folder] = id, ids_changed = true;
    }
    m.id = ids[m.folder];
    g_mods.push_back(m);
  }
  if (ids_changed) {
    std::ofstream out(dir / "ids.txt", std::ios::trunc);
    for (const auto& [k, v] : ids) out << k << '\t' << v << '\n';
  }
  if (g_mods.empty()) return;
  // The tables with the mods: built from the game's bgEtc.pac at each start (in memory only - its file is never
  // written, nor a copy of it).
  const fs::path src = Game() / "pac" / "bg" / "bgEtc.pac";
  svrfmt::Bytes data;
  svrfmt::Epac e;
  if (!svrfmt::ReadFile(src.string(), data) || !svrfmt::EpacRead(data, e)) {
    REXLOG_WARN("[svr2011] weapon mods: pac/bg/bgEtc.pac can't be read - no weapon mods");
    g_mods.clear();
    return;
  }
  std::map<int, svrfmt::Bytes> packs;
  for (auto& m : g_mods) {
    svrfmt::Bytes p;
    if (!svrfmt::ReadFile((dir / m.folder / "weapon.pac").string(), p) || !svrfmt::IsPach(svrfmt::Unpack(p))) {
      REXLOG_WARN("[svr2011] weapon mods: {} - weapon.pac is no weapon pack (a PACH): left out", m.folder);
      m.id = 0;
      continue;
    }
    // Its model (entry 0) must be a SvR2011 JBOY: WWE '13 / 2K14 ones (0xB8-byte meshes, a material name at +0x7C)
    // crash the game as it makes the weapon.
    if (const char* why = BadModel(svrfmt::Unpack(p))) {
      REXLOG_WARN("[svr2011] weapon mods: {} - weapon.pac's model {}: left out", m.folder, why);
      m.id = 0;
      continue;
    }
    m.kb = int((svrfmt::Unpack(p).size() + 1023) / 1024);
    uint64_t h = 0xCBF29CE484222325ull;  // (FNV-1a of the pack as stored)
    for (uint8_t b : p) h = (h ^ b) * 0x100000001B3ull;
    m.key = fmt::format("{}\t{:016X}\t{}", m.folder, h, m.like);
    packs[m.id] = std::move(p);
  }
  std::vector<WponCopy> tables;
  bool table_done = false;
  for (const auto& g : e.groups) {
    if (g.type != "STG " && g.type != "STG3") continue;  // (matches and backstage areas)
    for (const auto& en : g.entries) {
      // (WPBS: STG's backstage weapons - the loader's second pass in a backstage match looks there)
      if (en.name != "WPON" && en.name != "WPBS") continue;
      std::vector<svrfmt::PachEntry> kids;
      if (!svrfmt::PachRead(svrfmt::Unpack(en.data), kids)) continue;
      for (auto& k : kids)
        if (k.id == kSettingsTable) {
          const svrfmt::Bytes t = AddRecords(svrfmt::Unpack(k.data), g_mods);
          if (t.empty()) {
            REXLOG_WARN("[svr2011] weapon mods: {} {}'s settings table not as expected", g.type, en.name);
            continue;
          }
          k.data = svrfmt::IsBpe(k.data) ? svrfmt::BpeEncode(t) : t;
          table_done = true;
        }
      for (const auto& m : g_mods) {
        if (!m.id || !packs.count(m.id)) continue;
        kids.erase(std::remove_if(kids.begin(), kids.end(), [&](const svrfmt::PachEntry& k) { return k.id == uint32_t(m.id); }),
                   kids.end());
        kids.push_back({uint32_t(m.id), packs[m.id]});
      }
      std::sort(kids.begin(), kids.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
      WponCopy c;
      c.group = (g.type == "STG " ? std::string("STG") : g.type) + "/" + en.name;
      c.data = svrfmt::PachWrite(kids);
      tables.push_back(std::move(c));
    }
  }
  g_mods.erase(std::remove_if(g_mods.begin(), g_mods.end(), [](const WeaponMod& m) { return !m.id; }), g_mods.end());
  if (tables.empty() || !table_done || g_mods.empty()) {
    REXLOG_WARN("[svr2011] weapon mods: bgEtc.pac not as expected ({} weapon tables) - no weapon mods", tables.size());
    g_mods.clear();
    return;
  }
  {
    std::lock_guard lock(g_wpons_mutex);
    g_wpons = std::move(tables);
  }
  std::string list;
  for (const auto& m : g_mods) list += (list.empty() ? "" : ", ") + m.name + " (" + std::to_string(m.id) + ", like " + std::to_string(m.like) + ")";
  REXLOG_INFO("[svr2011] weapon mods: {} ({} weapon tables): {}", g_mods.size(), g_wpons.size(), list);
}

const std::vector<WeaponMod>& WeaponMods() { return g_mods; }

int WeaponModId(const std::string& folder) {
  for (const auto& m : g_mods)
    if (m.folder == folder) return m.id;
  return 0;
}

}  // namespace svr2011
