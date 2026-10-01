// More exhibition match types - see match_types.h.
//
// Game side (docs/MATCH_TYPES_RESEARCH.md): a match is a rule record (misc.pac
// RUL, 100 bytes, ids 0x00-0x76); a menu row (menu.pac MFLO/0000) names one by
// its id (+0x5C) and becomes group 2000 + id, which sub_824402A0(screen)
// turns back into the id when the match is set up.
//
// Menu rows are added as the game reads the menu table: sub_82BAA6E8(menus,
// table, size) copies each shown 0x74-byte record of MFLO/0000 into its own
// structures (sized by the table's counts, header +4 total and +8 shown) and
// the caller frees the table afterwards. The hook hands it a copy with the
// new rows inserted after their anchors (a group's rows are consecutive, in
// display order; flag 2 at +0x3C marks the group's last row; a submenu finds
// its rows by their parent node, +0x38). No file size limit applies.
#include "match_types.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"

namespace {

constexpr uint32_t kRec = 0x74, kFirst = 0x28;
constexpr uint32_t kScreenMatch = 0x7D0;
constexpr uint32_t kNoText = 0x05F5E0FF;

rex::memory::Memory* g_memory = nullptr;

uint32_t Rd32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
void Wr32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

// A row to add: after the row labelled `anchor` in menu group `group`, a copy
// of it with this label, description and rule.
struct Row {
  uint32_t anchor, group;
  uint32_t label, description, rule;
};

// PLAY -> ... (groups: 06 ONE ON ONE, 07 TWO ON TWO, 08 TRIPLE THREAT,
// 09 FATAL-4-WAY, 0A 6-MAN, 0C ROYAL RUMBLE). All the game's own rules and
// names: Falls Count Anywhere (the labels of the rows cut from the menu),
// 15- and 25-man Royal Rumble, Lumberjack.
const Row kRows[] = {
    {0xA053, 0x06, 0xA054, kNoText, 0x2B},  // ONE ON ONE: after FIRST BLOOD
    {0xA061, 0x07, 0xA062, kNoText, 0x2E},  // TWO ON TWO: after MIXED TAG
    {0xA06A, 0x08, 0xA06B, kNoText, 0x2F},  // TRIPLE THREAT: after NORMAL
    {0xA072, 0x09, 0xA073, kNoText, 0x30},  // FATAL-4-WAY: after NORMAL
    {0xA08C, 0x0C, 0x5313, kNoText, 0x15},  // ROYAL RUMBLE: 15-MAN after 10-MAN
    {0xA08D, 0x0C, 0x5314, kNoText, 0x17},  // 25-MAN after 20-MAN
    {0xA07F, 0x0A, 0x0052, kNoText, 0x55},  // 6-MAN: LUMBERJACK after ARMAGEDDON
};

// BACKSTAGE submenus (like ONE ON ONE's and TWO ON TWO's) for TRIPLE THREAT,
// FATAL-4-WAY and 6-MAN: the 7 areas of TWO ON TWO -> BACKSTAGE (menu group
// 0x14, rules 0x70-0x76: one per area), played with 3, 4 or 6 people (the
// rule record is reshaped for the match, see below).
struct Backstage {
  uint32_t anchor, group;   // after this row
  uint32_t people;
  uint32_t like;            // a rule with these people and teams
  uint32_t menu_group = 0;  // the areas' group, given when the rows are added
};
Backstage g_backstage[] = {
    {0xA070, 0x08, 3, 0x0D},  // TRIPLE THREAT: after TLC (like NORMAL)
    {0xA079, 0x09, 4, 0x0E},  // FATAL-4-WAY: after TLC (like NORMAL)
    {0xA09C, 0x0A, 6, 0x23},  // 6-MAN: after LADDER (like BATTLE ROYAL)
};
constexpr uint32_t kBackstageSubmenu = 0xA05D, kBackstageSubmenuGroup = 0x07;  // its row
constexpr uint32_t kBackstageAreasGroup = 0x14;                               // its areas
constexpr uint32_t kBackstageFirstRule = 0x70, kBackstageLastRule = 0x76;

// Developer aid: SVR2011_TEST_RULE=<hex id> plays that rule record wherever
// the game would play ONE ON ONE -> NORMAL (id 0), to try a rule in game.
int g_test_rule = -1;

// The menu table with the rows added (empty if it isn't the main menus).
std::vector<uint8_t> WithRows(const uint8_t* table, uint32_t size) {
  if (size < kFirst || Rd32(table) != 1) return {};
  const uint32_t total = Rd32(table + 4), shown = Rd32(table + 8);
  if (total == 0 || total > 0x1000 || kFirst + total * kRec > size) return {};
  // The rows the BACKSTAGE submenus copy, and free node / group ids.
  const uint8_t* submenu = nullptr;
  std::vector<const uint8_t*> areas;
  uint32_t node = 0, group = 0;
  for (uint32_t i = 0; i < total; ++i) {
    const uint8_t* rec = table + kFirst + i * kRec;
    node = std::max(node, Rd32(rec + 0x1C) + 1);
    group = std::max(group, Rd32(rec + 0x18) + 1);
    if (Rd32(rec) == kBackstageSubmenu && Rd32(rec + 0x18) == kBackstageSubmenuGroup) submenu = rec;
    if (Rd32(rec + 0x18) == kBackstageAreasGroup && Rd32(rec + 0x54) == kScreenMatch) areas.push_back(rec);
  }
  std::vector<uint8_t> out(table, table + kFirst);
  std::vector<uint8_t> tail;  // the submenus' areas: new groups, at the end
  uint32_t added = 0;
  for (uint32_t i = 0; i < total; ++i) {
    const uint8_t* rec = table + kFirst + i * kRec;
    out.insert(out.end(), rec, rec + kRec);
    if (Rd32(rec + 0x54) != kScreenMatch) continue;
    for (const Row& row : kRows) {
      if (Rd32(rec) != row.anchor || Rd32(rec + 0x18) != row.group) continue;
      uint8_t* prev = out.data() + out.size() - kRec;
      std::vector<uint8_t> copy(prev, prev + kRec);
      Wr32(copy.data() + 0x00, row.label);
      Wr32(copy.data() + 0x64, row.description);
      Wr32(copy.data() + 0x5C, row.rule);
      Wr32(copy.data() + 0x40, 0);  // no NEW badge, not hidden
      // The group's last row stays last.
      const uint32_t flags = Rd32(prev + 0x3C);
      Wr32(prev + 0x3C, flags & ~2u);
      Wr32(copy.data() + 0x3C, flags);
      out.insert(out.end(), copy.begin(), copy.end());
      ++added;
    }
    for (Backstage& b : g_backstage) {
      if (!submenu || areas.size() != 7) break;
      if (Rd32(rec) != b.anchor || Rd32(rec + 0x18) != b.group) continue;
      // The submenu row: in this group, a new node.
      std::vector<uint8_t> row(submenu, submenu + kRec);
      Wr32(row.data() + 0x18, b.group);
      Wr32(row.data() + 0x1C, node);
      Wr32(row.data() + 0x38, Rd32(rec + 0x38));
      Wr32(row.data() + 0x3C, Rd32(row.data() + 0x3C) & ~2u);
      Wr32(row.data() + 0x40, 0);
      out.insert(out.end(), row.begin(), row.end());
      ++added;
      // Its areas: a new group under that node.
      b.menu_group = group;
      for (uint32_t k = 0; k < areas.size(); ++k) {
        std::vector<uint8_t> leaf(areas[k], areas[k] + kRec);
        Wr32(leaf.data() + 0x18, group);
        Wr32(leaf.data() + 0x1C, node + 1 + k);
        Wr32(leaf.data() + 0x38, node);
        Wr32(leaf.data() + 0x40, 0);
        tail.insert(tail.end(), leaf.begin(), leaf.end());
        ++added;
      }
      node += 8;
      ++group;
    }
  }
  if (!added) return {};
  out.insert(out.end(), tail.begin(), tail.end());
  out.insert(out.end(), table + kFirst + total * kRec, table + size);
  Wr32(out.data() + 4, total + added);
  Wr32(out.data() + 8, shown + added);
  REXLOG_INFO("match types: {} rows added to the match menus", added);
  return out;
}

// -- Backstage with 3, 4 or 6 people --------------------------------------
//
// The area of a backstage match comes from its rule id (0x70-0x76: one per
// area, 2 on 2). For a row of the new submenus, that rule's record takes
// the shape of a match with that many people while the match is set up and
// played (setting up any other match puts it back): of the 100-byte record
// +0..+27 (people, their start places / teams, select screen, team layout),
// +44 (match family) and +52, +64 (its first rule flag); +35 of the 64-byte
// option record (the people count).
constexpr uint32_t kRules = 0x82EDE954;  // -> 119 x 100 bytes, then 119 x 64
constexpr uint32_t kRuleSize = 100, kRule2 = 11900, kRule2Size = 64;
constexpr uint32_t kRule2People = 35;
struct Span {
  uint32_t at, size;
};
constexpr Span kShape[] = {{0, 28}, {44, 4}, {52, 4}, {64, 1}};
// Lumberjack (0x55, story mode only) keeps its people and teams (3 fighters,
// 3 lumberjacks: team 3) but gets the 6-person select screen of 6-MAN BATTLE
// ROYAL: +24 (team layout), +36, +44, +52, +64.
constexpr uint32_t kLumberjack = 0x55, kLumberjackLike = 0x23, kSixMan = 0x0A;
constexpr Span kSelect[] = {{24, 4}, {36, 4}, {44, 4}, {52, 4}, {64, 1}};

// The match row picked last: the rule record to reshape like which, and how
// (pending_like 0: none).
uint32_t g_pending_rule_lo = 0, g_pending_rule_hi = 0, g_pending_like = 0;
bool g_pending_select_only = false;
struct Saved {
  uint32_t rule = 0;
  uint8_t rec[kRuleSize];
  uint8_t people;
};
Saved g_saved;  // the record reshaped now (rule 0: none)

void RestoreRule(uint8_t* base) {
  if (!g_saved.rule) return;
  if (const uint32_t rules = Rd32(base + kRules)) {
    std::memcpy(base + rules + g_saved.rule * kRuleSize, g_saved.rec, kRuleSize);
    base[rules + kRule2 + g_saved.rule * kRule2Size + kRule2People] = g_saved.people;
  }
  g_saved.rule = 0;
}

void ShapeRule(uint8_t* base, uint32_t rule, uint32_t like, bool select_only) {
  const uint32_t rules = Rd32(base + kRules);
  if (!rules) return;
  uint8_t* rec = base + rules + rule * kRuleSize;
  const uint8_t* from = base + rules + like * kRuleSize;
  uint8_t* rec2 = base + rules + kRule2 + rule * kRule2Size;
  g_saved.rule = rule;
  std::memcpy(g_saved.rec, rec, kRuleSize);
  g_saved.people = rec2[kRule2People];
  if (select_only) {
    for (const Span& span : kSelect) std::memcpy(rec + span.at, from + span.at, span.size);
  } else {
    for (const Span& span : kShape) std::memcpy(rec + span.at, from + span.at, span.size);
    rec2[kRule2People] = base[rules + kRule2 + like * kRule2Size + kRule2People];
  }
  REXLOG_INFO("match types: rule {:02X} set up like rule {:02X}{}", rule, like, select_only ? " (select screen)" : "");
}

}  // namespace

namespace svr2011 {

void InstallMatchTypes(rex::memory::Memory* memory) {
  g_memory = memory;
  if (const char* v = std::getenv("SVR2011_TEST_RULE"); v && *v) {
    g_test_rule = int(std::strtol(v, nullptr, 16));
    REXLOG_INFO("match types: ONE ON ONE NORMAL plays rule {:02X} (SVR2011_TEST_RULE)", g_test_rule);
  }
}

}  // namespace svr2011

// The menu table is read: sub_82BAA6E8(menus, table, size).
REX_EXTERN(__imp__sub_82BAA6E8);
REX_HOOK_RAW(sub_82BAA6E8) {
  const uint32_t table = ctx.r4.u32, size = ctx.r5.u32;
  std::vector<uint8_t> rows;
  if (g_memory && table) rows = WithRows(base + table, size);
  if (rows.empty()) {
    __imp__sub_82BAA6E8(ctx, base);
    return;
  }
  const uint32_t copy = g_memory->SystemHeapAlloc(uint32_t(rows.size()));
  if (!copy) {
    __imp__sub_82BAA6E8(ctx, base);
    return;
  }
  std::memcpy(base + copy, rows.data(), rows.size());
  ctx.r4.u64 = copy;
  ctx.r5.u64 = uint32_t(rows.size());
  __imp__sub_82BAA6E8(ctx, base);
  g_memory->SystemHeapFree(copy);
}

// The rule of the menu row picked: sub_824402A0(screen) -> id (119 = none).
REX_EXTERN(__imp__sub_824402A0);
REX_HOOK_RAW(sub_824402A0) {
  __imp__sub_824402A0(ctx, base);
  if (g_test_rule >= 0 && ctx.r3.u32 == 0) ctx.r3.u64 = uint32_t(g_test_rule);
}

// A menu row's match: sub_8243FCC8(?, group, row) -> 2000 + rule for a match
// row. Remembers whether it was a row of the backstage submenus.
REX_EXTERN(__imp__sub_8243FCC8);
REX_HOOK_RAW(sub_8243FCC8) {
  const uint32_t group = ctx.r4.u32;
  __imp__sub_8243FCC8(ctx, base);
  const int32_t result = ctx.r3.s32;
  if (result < 2000 || result >= 2119) return;
  g_pending_like = 0;
  for (const Backstage& b : g_backstage) {
    if (b.menu_group && group == b.menu_group) {
      g_pending_rule_lo = kBackstageFirstRule;
      g_pending_rule_hi = kBackstageLastRule;
      g_pending_like = b.like;
      g_pending_select_only = false;
    }
  }
  if (group == kSixMan && uint32_t(result - 2000) == kLumberjack) {
    g_pending_rule_lo = g_pending_rule_hi = kLumberjack;
    g_pending_like = kLumberjackLike;
    g_pending_select_only = true;
  }
}

// A match is set up: sub_827374A0(match, rule, ...).
REX_EXTERN(__imp__sub_827374A0);
REX_HOOK_RAW(sub_827374A0) {
  const uint32_t rule = ctx.r4.u32;
  RestoreRule(base);
  if (g_pending_like && rule >= g_pending_rule_lo && rule <= g_pending_rule_hi)
    ShapeRule(base, rule, g_pending_like, g_pending_select_only);
  __imp__sub_827374A0(ctx, base);
}

// -- Lumberjack outside Road to WrestleMania ------------------------------
//
// The story mode's match script (sub_822C3D08 -> sub_822BB158, story mode
// only) marks the lumberjacks (team 3) for the AI: character +2572 -> +168 =
// 1. Done here for every Lumberjack match, from the game's per-character
// "is a competitor" check (sub_82225D68(character)), which every match runs.
namespace {

void MarkLumberjacks(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kLive = 0x82E3DE00, kChars = 0x82E3CC50, kCharCount = 0x82E3CD0C;
  if (base[kLive] != kLumberjack || int32_t(Rd32(base + kCharCount)) <= 0) return;
  const auto saved = ctx;
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t ch = Rd32(base + kChars + i * 4);
    if (!ch) continue;
    const uint32_t ai = Rd32(base + ch + 2572);
    if (!ai || Rd32(base + ai + 168) == 1) continue;
    ctx.r3.u64 = Rd32(base + ch + 1156);
    sub_8257C190(ctx, base);
    sub_82573EC8(ctx, base);
    const uint32_t info = ctx.r3.u32;
    if (info && base[info + 13] == 3) {
      Wr32(base + ai + 168, 1);
      REXLOG_INFO("match types: lumberjack {} stays ringside", i);
    }
  }
  ctx = saved;
}

}  // namespace

REX_EXTERN(__imp__sub_82225D68);
REX_HOOK_RAW(sub_82225D68) {
  MarkLumberjacks(ctx, base);
  __imp__sub_82225D68(ctx, base);
}
