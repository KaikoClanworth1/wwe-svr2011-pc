// MODS rows in CREATE AN ENTRANCE - see cae_mod_motions.h and docs/CAE_MOD_MOTIONS.md.
//
// The game's lists are data (pac\DLC.pac ENEX/LSD: categories of {value, string id, unlock}),
// turned into menu rows by sub_8287DFD0(list, r4, table, category, base); a list is
// {count, capacity, items}, items 24 bytes {value, flags, unlock, text (a guest char*) at +12},
// sub_828829D8(list, value, text, 0, 0, unlock) appends one. The motion lists:
//   - the one-page editor's MOTION: category 2 (singles), 3 / 4 (tag / trio teams), and its
//     CHAMPION (16) / MULTI-TITLE (17) entrances: values = entrance numbers;
//   - the scene editor's MOTION for scenes 0-4 (segments 0/5/10/15/20): categories 15, 7, 8, 9, 10
//     (table 0x8206B708): values = CAE part numbers 10000 + 1000k + n.
// A saved entrance keeps only the numbers (S+16, or part +14 at S+24+34p), so a mod's rows need no
// ids of their own: the pac numbers themselves. The game finds a script by number in any pac
// (sub_82751C60), and skips a part whose script is missing (that scene doesn't play).
#include "cae_mod_motions.h"

#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "move_packs.h"

#include "generated/default/svr2011_init.h"

namespace {

constexpr uint32_t kItemSize = 24;
constexpr uint32_t kHeaderValue = 0xFFFFFFFE;  // (the disabled header row: a value no list has)
constexpr uint32_t kPartCategories[5] = {15, 7, 8, 9, 10};

rex::memory::Memory* g_memory = nullptr;
std::mutex g_mutex;
std::map<std::string, uint32_t> g_text;  // label -> guest string (lives for the whole run)
std::set<uint32_t> g_motion_lists;       // menu lists filled with a motion category

uint32_t Rd32(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
uint16_t Rd16(uint8_t* base, uint32_t a) { return uint16_t(base[a] << 8 | base[a + 1]); }
void Wr32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

uint32_t Text(const std::string& label) {  // (g_mutex held)
  if (auto it = g_text.find(label); it != g_text.end()) return it->second;
  const uint32_t s = g_memory->SystemHeapAlloc(uint32_t(label.size() + 1));
  if (!s) return 0;
  std::memcpy(g_memory->TranslateVirtual<char*>(s), label.c_str(), label.size() + 1);
  g_text[label] = s;
  return s;
}

int IndexOf(uint8_t* base, uint32_t list, uint32_t value) {
  const uint32_t n = Rd32(base, list), items = Rd32(base, list + 8);
  for (uint32_t i = 0; i < n && items; ++i)
    if (Rd32(base, items + i * kItemSize) == value) return int(i);
  return -1;
}

// sub_828829D8(list, value, text, 0, 0, unlock -1) -> the new row's index
int AddRow(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t value, uint32_t text, bool disabled) {
  const auto saved = ctx;
  ctx.r3.u64 = list;
  ctx.r4.u64 = value;
  ctx.r5.u64 = text;
  ctx.r6.u64 = 0;
  ctx.r7.u64 = 0;
  ctx.r8.u64 = uint64_t(int64_t(-1));
  sub_828829D8(ctx, base);
  ctx = saved;
  const uint32_t n = Rd32(base, list);
  if (!n || Rd32(base, Rd32(base, list + 8) + (n - 1) * kItemSize) != value) return -1;
  if (disabled) Wr32(base, Rd32(base, list + 8) + (n - 1) * kItemSize + 4, 1);  // (flags 1: greyed, not picked)
  return int(n - 1);
}

// The kind of motion list a category is: the CAE part k (0-4), or -2 singles, -3 teams; -1 none.
int ListKind(uint32_t category) {
  for (int k = 0; k < 5; ++k)
    if (category == kPartCategories[k]) return k;
  if (category == 2 || category == 16 || category == 17) return -2;
  if (category == 3 || category == 4) return -3;
  return -1;
}

bool TeamNumber(int n) { return n >= 700 && n < 800; }  // (2011's tag / trio entrances: 700-799)

}  // namespace

namespace svr2011 {

void InstallCaeModMotions(rex::memory::Memory* memory) { g_memory = memory; }

void CaeModRows(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t category) {
  const int kind = ListKind(category);
  if (!g_memory || !list) return;
  {
    std::lock_guard lock(g_mutex);  // (list objects are reused: a list filled with another category isn't one)
    if (kind == -1) g_motion_lists.erase(list);
    else g_motion_lists.insert(list);
  }
  if (kind == -1) return;
  std::vector<std::pair<uint32_t, std::string>> rows;
  for (const auto& e : ModEntrances()) {
    uint32_t value;
    if (kind >= 0) {
      if (!(e.parts >> kind & 1)) continue;
      value = uint32_t(10000 + 1000 * kind + e.number);
    } else {
      if ((kind == -3) != TeamNumber(e.number)) continue;
      value = uint32_t(e.number);
    }
    if (IndexOf(base, list, value) >= 0) continue;  // (a mod replacing a listed entrance: its own row)
    rows.emplace_back(value, "MOD: " + e.label);
  }
  if (rows.empty()) return;
  // (a group at the end of the list, each row "MOD: <label>": a disabled header row isn't drawn - flags 1
  // hides a row - so the prefix is the group's name)
  std::vector<std::pair<uint32_t, uint32_t>> texts;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [v, l] : rows) texts.emplace_back(v, Text(l));
  }
  for (const auto& [v, t] : texts)
    if (t) AddRow(ctx, base, list, v, t, false);
  static int logged = 0;
  if (logged++ < 24)
    REXLOG_INFO("[svr2011] cae mods: list category {} ({:08X}): {} mod rows, {} rows in all", category, list, texts.size(),
                Rd32(base, list));
}

int CaeMissingRow(PPCContext& ctx, uint8_t* base, uint32_t list, uint32_t value) {
  if (!g_memory || !list || value == 0 || value >= 0xFFFF || value == kHeaderValue) return -1;
  // Any stored number the list doesn't have would become row 0 (Alicia Fox) and be saved so: it keeps
  // a row of its own - a mod's (its label), a script the list leaves out ("ENTRANCE n"), or one whose
  // mod is gone ("MISSING MOD (n)": the match then falls back, EntranceFallback).
  std::string label;
  bool missing = false;
  {
    std::lock_guard lock(g_mutex);
    if (!g_motion_lists.count(list)) return -1;
  }
  const uint32_t own_n = value >= 10000 ? value % 1000 : value;
  for (const auto& e : ModEntrances())
    if (uint32_t(e.number) == own_n) label = "MOD: " + e.label;
  if (label.empty()) {
    missing = own_n >= 100 && !EntranceScriptExists(int(value));  // (below 100: DLC entrances, in DLC pacs)
    label = missing ? "MISSING MOD (" + std::to_string(value) + ")" : "ENTRANCE " + std::to_string(value);
  }
  uint32_t text = 0;
  {
    std::lock_guard lock(g_mutex);
    text = Text(label);
  }
  if (!text) return -1;
  const int row = AddRow(ctx, base, list, value, text, false);
  static int logged = 0;
  if (logged++ < 32)
    REXLOG_INFO("[svr2011] cae mods: stored entrance motion {} not in the list {:08X} ({} rows): row {} \"{}\"{}", value,
                list, Rd32(base, list), row, label, missing ? " (no script: mod removed?)" : "");
  return row;
}

uint32_t EntranceFallback(uint8_t* base, uint32_t mgr, uint32_t idx, int part, uint32_t n) {
  if (!n || n >= 0xFFFF || idx >= 16) return n;
  const uint32_t own_n = n >= 10000 ? n % 1000 : n;
  if (own_n < 100 || EntranceScriptExists(int(n))) return n;  // (DLC 50-57 live in DLC pacs)
  const uint32_t S = mgr + 36 + idx * 280;
  const uint32_t own = Rd16(base, S + 4);  // (the profile's own entrance: the default copy, E+0x08)
  std::vector<uint32_t> cands;
  if (n >= 10000) {
    const uint32_t k = (n - 10000) / 1000;
    cands = {10000 + 1000 * k + own, 10507 + 1000 * k, 10540 + 1000 * k};  // (own, then ENEX/BIN's defaults)
  } else {
    cands = {own, Rd16(base, S + 16)};
  }
  for (const uint32_t c : cands) {
    if (c && c != n && EntranceScriptExists(int(c))) {
      static int logged = 0;
      if (logged++ < 32)
        REXLOG_INFO("[svr2011] cae mods: entrance {} (slot {}, part {}) has no script (mod removed?): {} instead", n, idx,
                    part, c);
      return c;
    }
  }
  return n;
}

}  // namespace svr2011
