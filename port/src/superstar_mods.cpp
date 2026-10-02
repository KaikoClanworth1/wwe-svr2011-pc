// Superstar mods (arenas branch): new playable characters from
// <game>/Mods/Superstars/<id>/ (manifest.txt: name=, short=, base=<character
// id>; ch.pac: the character's model pac; a "disabled" file turns one off).
//
// The game's character database (docs/SUPERSTAR_MODS.md, from
// re_roster): ids 50..69 are the 20 DLC slots; real DLC uses 51..58 and
// 59..69 have records, table slots and save room but nobody in them. A
// superstar mod takes one of those ids:
// - files: the model pac's EMD entries are named "%06d%02d" (id, attire*10 +
//   kind); a copy with the slot's id in the names, and a pack of select
//   screen renders (SSFA/SSFB/SSFC "%04d" = attire*1000 + id) copied from
//   the base character's in DLC_HD.pac, both in Mods/SuperstarOverlay and
//   mounted with the game's own pac mount (sub_826A1780(path, 0)) once its
//   file system is up (end of sub_825953B0 / sub_82595428);
// - records: the base character's 260-byte roster record (0x82E407C0 +
//   index * 260, index from the u16 table at 0x82DB3610) and 1056-byte
//   profile (0x82E7C920 + index * 1056) copied to the slot, with the mod's
//   names, selectable (+221) and DLC (+257) set and its own id as "same
//   person" (+228); again after the loaders (sub_82B89E50, sub_82594C78), a
//   save load (sub_8257A218) and before every roster list is built
//   (sub_82736C68), since saves carry the records;
// - owned: DLC-flagged ids must be owned (sub_828C1288 / sub_82589198):
//   mods are.
// The select screen shows them in the EXTRA list (managers.cpp, the M tile).
#include "superstar_mods.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/pac.h"
#include "../modmaker/svrfmt/texture.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kFirstSlot = 59, kLastSlot = 69;
constexpr uint32_t kIdToIndex = 0x82DB3610;  // u16 per id
constexpr uint32_t kRecords = 0x82E407C0, kRecordSize = 260;
constexpr uint32_t kProfiles = 0x82E7C920, kProfileSize = 1056;
constexpr uint32_t kFullName = 34, kSecondName = 102, kShortName = 170, kNameLen = 32;
constexpr uint32_t kSelectable = 221, kDlc = 257, kSamePerson = 228;

struct Mod {
  std::string folder, name, short_name;
  uint32_t base = 0, slot = 0;
  std::string ch_guest, ssf_guest;  // the overlay files, as the game opens them
};
std::vector<Mod> g_mods;
rex::memory::Memory* g_memory = nullptr;
fs::path g_game;
bool g_mounted = false;

uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// "000106" -> "000059" in an EPK8's EMD entry names (same length: the
// directory is patched in place).
bool RenameModel(svrfmt::Bytes& d, uint32_t from, uint32_t to) {
  if (d.size() < 0x4000 || std::memcmp(d.data(), "EPK8", 4)) return false;
  char a[8], b[8];
  std::snprintf(a, sizeof a, "%06u", from);
  std::snprintf(b, sizeof b, "%06u", to);
  int n = 0;
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!std::memcmp(&d[p], "\0\0\0\0", 4)) break;
    const uint32_t cnt = uint32_t(d[p + 4]) | uint32_t(d[p + 5]) << 8;
    p += 12;
    for (uint32_t k = 0; k < cnt && p + 16 <= 0x4000; ++k, p += 16)
      if (!std::memcmp(&d[p], a, 6)) std::memcpy(&d[p], b, 6), ++n;
  }
  return n > 0;
}

// The base's select screen renders, renamed to the slot: an EPAC with
// SSFA / SSFB / SSFC.
bool BuildRenders(const fs::path& out, uint32_t base, uint32_t slot) {
  svrfmt::Bytes d;
  svrfmt::Epac src;
  if (!svrfmt::ReadFile((g_game / "pac" / "DLC_HD.pac").string(), d) || !svrfmt::EpacRead(d, src)) return false;
  svrfmt::Epac e;
  e.header = src.header;
  e.trailer = src.trailer;
  int n = 0;
  for (const auto& g : src.groups) {
    if (g.type != "SSFA" && g.type != "SSFB" && g.type != "SSFC") continue;
    svrfmt::EpacGroup og;
    og.type = g.type;
    for (const auto& en : g.entries) {
      const int key = std::atoi(en.name.c_str());
      if (key % 1000 != int(base)) continue;
      char nm[8];
      std::snprintf(nm, sizeof nm, "%04d", (key / 1000) * 1000 + int(slot));
      og.entries.push_back({nm, en.data});
      ++n;
    }
    if (!og.entries.empty()) e.groups.push_back(std::move(og));
  }
  return n > 0 && svrfmt::WriteFile(out.string(), svrfmt::EpacWrite(e));
}

void LoadMods() {
  std::error_code ec;
  const fs::path dir = g_game / "Mods" / "Superstars";
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory() && fs::exists(e.path() / "ch.pac", ec) && !fs::exists(e.path() / "disabled", ec))
      folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  const fs::path overlay = g_game / "Mods" / "SuperstarOverlay";
  fs::create_directories(overlay, ec);
  uint32_t slot = kFirstSlot;
  for (const auto& f : folders) {
    if (slot > kLastSlot) {
      REXLOG_WARN("[svr2011] superstar mods: only {} fit (ids {}-{}), {} left out", kLastSlot - kFirstSlot + 1,
                  kFirstSlot, kLastSlot, f.filename().string());
      continue;
    }
    Mod m;
    m.folder = f.filename().string();
    m.name = m.short_name = m.folder;
    if (FILE* t = std::fopen((f / "manifest.txt").string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("name=", 0) == 0) m.name = l.substr(5);
        if (l.rfind("short=", 0) == 0) m.short_name = l.substr(6);
        if (l.rfind("base=", 0) == 0) m.base = uint32_t(std::atoi(l.c_str() + 5));
      }
      std::fclose(t);
    }
    if (m.base < 100 || m.base > 321) {
      REXLOG_WARN("[svr2011] superstar mods: {} has no base= character (100-321)", m.folder);
      continue;
    }
    m.slot = slot;
    svrfmt::Bytes ch;
    char nm[32];
    std::snprintf(nm, sizeof nm, "ch%03u.pac", m.slot);
    if (!svrfmt::ReadFile((f / "ch.pac").string(), ch) || !RenameModel(ch, m.base, m.slot) ||
        !svrfmt::WriteFile((overlay / nm).string(), ch)) {
      REXLOG_WARN("[svr2011] superstar mods: {}: ch.pac is not character {}'s model pac", m.folder, m.base);
      continue;
    }
    m.ch_guest = std::string("GAME:\\Mods\\SuperstarOverlay\\") + nm;
    std::snprintf(nm, sizeof nm, "ssf%03u.pac", m.slot);
    if (BuildRenders(overlay / nm, m.base, m.slot)) m.ssf_guest = std::string("GAME:\\Mods\\SuperstarOverlay\\") + nm;
    REXLOG_INFO("[svr2011] superstar mods: {} as id {} (from {})", m.name, m.slot, m.base);
    g_mods.push_back(std::move(m));
    ++slot;
  }
}

const Mod* ModOf(uint32_t id) {
  for (const auto& m : g_mods)
    if (m.slot == id) return &m;
  return nullptr;
}

// sub_826A1780(path, 0) for each overlay file; the path in a frame below the caller's.
void Mount(PPCContext& ctx, uint8_t* base) {
  if (g_mounted || g_mods.empty()) return;
  g_mounted = true;
  const auto saved = ctx;
  const uint32_t str = saved.r1.u32 - 0x300;
  for (const auto& m : g_mods)
    for (const std::string* p : {&m.ch_guest, &m.ssf_guest}) {
      if (p->empty()) continue;
      std::memcpy(base + str, p->c_str(), p->size() + 1);
      ctx = saved;
      ctx.r1.u64 = saved.r1.u32 - 0x400;
      ctx.r3.u64 = str;
      ctx.r4.u64 = 0;
      sub_826A1780(ctx, base);
      REXLOG_INFO("[svr2011] superstar mods: mounted {} ({})", *p, ctx.r3.u32);
    }
  ctx = saved;
}

void PutName(uint8_t* rec, uint32_t off, const std::string& s) {
  std::memset(rec + off, 0, kNameLen);
  std::memcpy(rec + off, s.data(), std::min<size_t>(s.size(), kNameLen - 1));
}

// The base's record and profile into each mod's slot.
void ApplyRecords(uint8_t* base) {
  for (const auto& m : g_mods) {
    const uint32_t bi = Rd16(base + kIdToIndex + m.base * 2), si = Rd16(base + kIdToIndex + m.slot * 2);
    if (bi == 0xFFFF || si == 0xFFFF || bi == si) continue;
    uint8_t* br = base + kRecords + bi * kRecordSize;
    uint8_t* sr = base + kRecords + si * kRecordSize;
    if (!br[kFullName]) continue;  // (not loaded yet)
    // the profile (entrance, moves, ...): CHAR/PRO and saves load after the
    // records, so it is checked on its own
    uint8_t* bp = base + kProfiles + bi * kProfileSize;
    uint8_t* sp = base + kProfiles + si * kProfileSize;
    if (std::memcmp(sp, bp, kProfileSize)) {
      std::memcpy(sp, bp, kProfileSize);
      REXLOG_INFO("[svr2011] superstar mods: profile {} -> id {}", m.base, m.slot);
    }
    if (sr[kSelectable] && !std::strncmp(reinterpret_cast<char*>(sr + kFullName), m.name.c_str(), kNameLen - 1) &&
        sr[kDlc])
      continue;  // (already)
    std::memcpy(sr, br, kRecordSize);
    PutName(sr, kFullName, m.name);
    PutName(sr, kSecondName, m.name);
    PutName(sr, kShortName, m.short_name);
    sr[kSelectable] = 1;
    sr[kDlc] = 1;
    sr[kSamePerson] = uint8_t(m.slot >> 8), sr[kSamePerson + 1] = uint8_t(m.slot);
    REXLOG_INFO("[svr2011] superstar mods: record {} -> id {} ({})", m.base, m.slot, m.name);
  }
}

// ---- The "MODDED" badge
// The select screen's panel shows a DLC-flagged character with the
// "DOWNLOADABLE CONTENT" badge (layout node cursor+312, sub_82466830), the
// texture DLCtex01 (menuHD.pac MENx/CHSI, 7001; 256 x 64 DXT5, one per
// language). Mods are DLC-flagged, so the same badge shows; while a panel
// hovers a mod, that texture's pixels (in guest memory, 16-bit words swapped)
// say "MODDED" (tools/make_modded_badge.py) and go back for real DLC. Both
// panels share the texture: a mod wins.
const uint8_t kModdedRgba[] = {
#include "modded_badge.inc"
};
constexpr uint32_t kBadgeW = 256, kBadgeH = 64;
constexpr uint32_t kBadgeCaller = 0x82466E40;  // sub_82466830: sub_828B6CD0(id) at the badge

struct Badge {
  svrfmt::Bytes data;  // swapped as in memory
  size_t sig = 0;      // a distinctive 64 bytes in it
};
std::vector<Badge> g_badges;  // [0]: MODDED, then the originals
std::mutex g_badge_mutex;
std::vector<std::pair<uint32_t, bool>> g_panel_mod;  // select screen cursor -> hovering a mod
int64_t g_badge_seen = 0;                            // last panel update (a select screen is up)
bool g_badge_thread = false;

svrfmt::Bytes Swap16(const uint8_t* p, size_t n) {
  svrfmt::Bytes o(p, p + n);
  for (size_t i = 0; i + 1 < n; i += 2) std::swap(o[i], o[i + 1]);
  return o;
}

size_t Signature(const svrfmt::Bytes& d) {
  for (size_t at = 0; at + 64 <= d.size(); at += 64) {
    int distinct = 0;
    for (int i = 0; i < 4; ++i) {
      bool seen = false;
      for (int j = 0; j < i; ++j) seen |= !std::memcmp(&d[at + i * 16], &d[at + j * 16], 16);
      distinct += !seen;
    }
    if (distinct >= 4) return at;
  }
  return 0;
}

// The originals from menuHD.pac (MENU, MENF, MENG, MENI, MENE: CHSI) and MODDED.
void LoadBadges() {
  FILE* f = std::fopen((g_game / "pac" / "menu" / "menuHD.pac").string().c_str(), "rb");
  if (!f) return;
  svrfmt::Bytes toc(0x4000);
  if (std::fread(toc.data(), 1, toc.size(), f) != toc.size() || std::memcmp(toc.data(), "EPAC", 4)) {
    std::fclose(f);
    return;
  }
  auto le32 = [&](size_t p) { return uint32_t(toc[p]) | toc[p + 1] << 8 | toc[p + 2] << 16 | uint32_t(toc[p + 3]) << 24; };
  std::vector<svrfmt::Bytes> found;
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!le32(p)) break;
    const uint32_t cnt = le32(p + 4) / 3;
    p += 12;
    for (uint32_t i = 0; i < cnt && p + 12 <= 0x4000; ++i, p += 12) {
      if (std::memcmp(&toc[p], "CHSI", 4)) continue;
      svrfmt::Bytes e(size_t(le32(p + 8)) * 0x100);
      std::fseek(f, long(0x4000 + size_t(le32(p + 4)) * 0x800), SEEK_SET);
      if (std::fread(e.data(), 1, e.size(), f) != e.size()) continue;
      std::vector<svrfmt::PachEntry> pe;
      if (!svrfmt::PachRead(e, pe)) continue;
      for (const auto& x : pe) {
        std::vector<svrfmt::BundleTexture> texs;
        if (!svrfmt::BundleRead(svrfmt::Unpack(x.data), texs)) continue;
        for (const auto& t : texs)
          if (t.name == "DLCtex01" && t.data.size() == 128 + kBadgeW * kBadgeH) found.push_back(t.data);
      }
    }
  }
  std::fclose(f);
  if (found.empty()) return;
  svrfmt::Image img;
  img.w = kBadgeW, img.h = kBadgeH;
  img.rgba.assign(kModdedRgba, kModdedRgba + sizeof kModdedRgba);
  const svrfmt::Bytes dds = svrfmt::DdsEncode(img, svrfmt::DxtFormat::kDxt5, false);
  g_badges.push_back({Swap16(dds.data() + 128, kBadgeW * kBadgeH), 0});
  for (const auto& o : found) {
    Badge b{Swap16(o.data() + 128, kBadgeW * kBadgeH), 0};
    b.sig = Signature(b.data);
    g_badges.push_back(std::move(b));
  }
  REXLOG_INFO("[svr2011] superstar mods: badge ({} languages)", found.size());
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool Committed(uint32_t at) {
  auto* heap = g_memory->LookupHeap(at);
  rex::memory::HeapAllocationInfo info{};
  return heap && heap->QueryRegionInfo(at & ~0xFFFu, &info) && info.region_size &&
         (info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead);
}

// Which badge the texture at guest holds (-1: none of them).
int BadgeAt(uint8_t* b, uint32_t guest) {
  if (!guest || !Committed(guest) || !Committed(guest + kBadgeW * kBadgeH - 1)) return -1;
  for (size_t i = 0; i < g_badges.size(); ++i)
    if (!std::memcmp(b + guest, g_badges[i].data.data(), g_badges[i].data.size())) return int(i);
  return -1;
}

void BadgeLoop() {
  uint32_t guest = 0;
  int original = 1;  // the language's original there
  int64_t last_scan = 0;
  int scans = 0;  // this visit of the select screen
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool want = false, active;
    {
      std::lock_guard lock(g_badge_mutex);
      active = NowMs() - g_badge_seen < 5000;
      if (!active) g_panel_mod.clear(), scans = 0;
      for (const auto& p : g_panel_mod) want |= p.second;
    }
    uint8_t* b = g_memory->virtual_membase();
    int now = BadgeAt(b, guest);
    if (now < 0) guest = 0;
    if (!guest) {
      if (!active || scans >= 10 || NowMs() - last_scan < 1000) continue;
      last_scan = NowMs();
      ++scans;
      // (the menu textures: physical memory, seen at 0xE0000000+ in the
      // VS screen search; then the other views)
      for (uint64_t lo : {0xE0000000ull, 0xA0000000ull}) {
        const uint64_t hi = lo == 0xE0000000ull ? 0xFFFF0000ull : 0xE0000000ull;
        for (uint64_t at = lo; at < hi && !guest;) {
          auto* heap = g_memory->LookupHeap(uint32_t(at));
          rex::memory::HeapAllocationInfo info{};
          if (!heap || !heap->QueryRegionInfo(uint32_t(at) & ~0xFFFu, &info) || !info.region_size) {
            at = (at & ~0xFFFull) + 0x10000;
            continue;
          }
          const uint64_t end = std::min<uint64_t>(uint64_t(info.base_address) + info.region_size, hi);
          if ((info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead))
            for (uint64_t a = at; a + 64 <= end && !guest; a += 16)
              for (size_t i = 0; i < g_badges.size(); ++i) {
                const auto& g = g_badges[i];
                if (a < lo + g.sig || std::memcmp(b + a, g.data.data() + g.sig, 64)) continue;
                if (BadgeAt(b, uint32_t(a - g.sig)) == int(i)) {
                  guest = uint32_t(a - g.sig);
                  break;
                }
              }
          at = std::max<uint64_t>(end, at + 0x1000);
        }
        if (guest) break;
      }
      if (!guest) continue;
      now = BadgeAt(b, guest);
      REXLOG_INFO("[svr2011] superstar mods: badge texture at {:08X}", guest);
    }
    if (now > 0) original = now;
    const int target = want ? 0 : original;
    if (now != target) std::memcpy(b + guest, g_badges[target].data.data(), g_badges[target].data.size());
  }
}

}  // namespace

// The select screen's panel: "is it DLC" for the badge (r26: the id; r31:
// the cursor).
REX_EXTERN(__imp__sub_828B6CD0);
REX_HOOK_RAW(sub_828B6CD0) {
  if (uint32_t(ctx.lr) == kBadgeCaller && !g_badges.empty()) {
    const uint32_t cursor = ctx.r31.u32;
    const bool mod = ModOf(ctx.r3.u32) != nullptr;
    std::lock_guard lock(g_badge_mutex);
    g_badge_seen = NowMs();
    bool found = false;
    for (auto& p : g_panel_mod)
      if (p.first == cursor) p.second = mod, found = true;
    if (!found) g_panel_mod.push_back({cursor, mod});
    if (!g_badge_thread) {
      g_badge_thread = true;
      std::thread(BadgeLoop).detach();
    }
  }
  __imp__sub_828B6CD0(ctx, base);
}

// The DLC tile's list filter (sub_8244A128(screen, id): DLC-flagged,
// selectable, owned): mods are in the EXTRA list instead.
REX_EXTERN(__imp__sub_8244A128);
REX_HOOK_RAW(sub_8244A128) {
  if (ModOf(ctx.r4.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8244A128(ctx, base);
}

// Test aid: SVR2011_TEST_VFS_LOG=1 logs each virtual file lookup that finds
// nothing (sub_826B87B0(vfs, "/TYPE/NAME", out)), once per name.
REX_EXTERN(__imp__sub_826B87B0);
REX_HOOK_RAW(sub_826B87B0) {
  static const bool log = [] {
    const char* e = std::getenv("SVR2011_TEST_VFS_LOG");
    return e && *e == '1';
  }();
  if (!log) {
    __imp__sub_826B87B0(ctx, base);
    return;
  }
  const std::string name(reinterpret_cast<const char*>(base + ctx.r4.u32), 0,
                         strnlen(reinterpret_cast<const char*>(base + ctx.r4.u32), 64));
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_826B87B0(ctx, base);
  if (ctx.r3.u32) return;
  static std::mutex m;
  static std::vector<std::string> seen;
  std::lock_guard lock(m);
  if (std::find(seen.begin(), seen.end(), name) != seen.end()) return;
  seen.push_back(name);
  REXLOG_INFO("[svr2011] vfs miss: {} (from {:08X})", name, caller);
}

// The file system's pacs registered (from the ARC / from each pac's header).
REX_EXTERN(__imp__sub_825953B0);
REX_HOOK_RAW(sub_825953B0) {
  __imp__sub_825953B0(ctx, base);
  const uint64_t r3 = ctx.r3.u64;
  Mount(ctx, base);
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_82595428);
REX_HOOK_RAW(sub_82595428) {
  __imp__sub_82595428(ctx, base);
  const uint64_t r3 = ctx.r3.u64;
  Mount(ctx, base);
  ctx.r3.u64 = r3;
}

// The roster records loaded (CHAR/DAT), the profiles (CHAR/PRO), a save loaded.
REX_EXTERN(__imp__sub_82B89E50);
REX_HOOK_RAW(sub_82B89E50) {
  __imp__sub_82B89E50(ctx, base);
  const auto r3 = ctx.r3.u64;
  ApplyRecords(base);
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_82594C78);
REX_HOOK_RAW(sub_82594C78) {
  __imp__sub_82594C78(ctx, base);
  const auto r3 = ctx.r3.u64;
  ApplyRecords(base);
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_8257A218);
REX_HOOK_RAW(sub_8257A218) {
  __imp__sub_8257A218(ctx, base);
  const auto r3 = ctx.r3.u64;
  ApplyRecords(base);
  ctx.r3.u64 = r3;
}
// A roster list is about to be built.
REX_EXTERN(__imp__sub_82736C68);
REX_HOOK_RAW(sub_82736C68) {
  ApplyRecords(base);
  __imp__sub_82736C68(ctx, base);
}

// Owned (DLC-flagged ids must be): mods are.
REX_EXTERN(__imp__sub_828C1288);
REX_HOOK_RAW(sub_828C1288) {
  if (ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_828C1288(ctx, base);
}
REX_EXTERN(__imp__sub_82589198);
REX_HOOK_RAW(sub_82589198) {
  if (ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82589198(ctx, base);
}

namespace svr2011 {

void InstallSuperstarMods(rex::memory::Memory* memory) {
  g_memory = memory;
  g_game = rex::filesystem::GetExecutableFolder();
  LoadMods();
  if (!g_mods.empty()) LoadBadges();
  REXLOG_INFO("[svr2011] superstar mods: {}", g_mods.size());
}

std::vector<uint32_t> SuperstarModIds() {
  std::vector<uint32_t> ids;
  for (const auto& m : g_mods) ids.push_back(m.slot);
  return ids;
}

bool IsSuperstarMod(uint32_t id) { return ModOf(id) != nullptr; }

}  // namespace svr2011
