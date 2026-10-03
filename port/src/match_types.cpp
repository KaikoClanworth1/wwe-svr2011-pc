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
#include <cmath>
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
    // BACKSTAGE (1 on 1, 2 on 2): FREE-ROAMING BACKSTAGE after PARKING LOT -
    // the whole backstage (rules without a named area, see below).
    {0xA0DD, 0x12, 0x9C90, kNoText, 0x19},
    {0xA0DD, 0x14, 0x9C90, kNoText, 0x1A},
};
constexpr uint32_t kFreeRoamLabel = 0x9C90;
constexpr uint32_t kWholeBackstage = 0x19, kWholeBackstage2 = 0x1A;  // 1 on 1, 2 on 2
// The match being played is a free-roaming backstage one (set at match set-up).
bool g_free_roam = false;

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
      for (uint32_t k = 0; k <= areas.size(); ++k) {
        // The 7 areas, then FREE-ROAMING BACKSTAGE (the group's last).
        const bool free_roam = k == areas.size();
        std::vector<uint8_t> leaf(areas[free_roam ? k - 1 : k], areas[free_roam ? k - 1 : k] + kRec);
        Wr32(leaf.data() + 0x18, group);
        Wr32(leaf.data() + 0x1C, node + 1 + k);
        Wr32(leaf.data() + 0x38, node);
        Wr32(leaf.data() + 0x40, 0);
        const uint32_t flags = Rd32(leaf.data() + 0x3C);
        Wr32(leaf.data() + 0x3C, free_roam ? flags | 2u : flags & ~2u);
        if (free_roam) {
          Wr32(leaf.data() + 0x00, kFreeRoamLabel);
          Wr32(leaf.data() + 0x5C, kWholeBackstage2);
        }
        tail.insert(tail.end(), leaf.begin(), leaf.end());
        ++added;
      }
      node += 9;
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
  uint8_t rec2[kRule2Size];
};
Saved g_saved;  // the record reshaped now (rule 0: none)

void RestoreRule(uint8_t* base) {
  if (!g_saved.rule) return;
  if (const uint32_t rules = Rd32(base + kRules)) {
    std::memcpy(base + rules + g_saved.rule * kRuleSize, g_saved.rec, kRuleSize);
    std::memcpy(base + rules + kRule2 + g_saved.rule * kRule2Size, g_saved.rec2, kRule2Size);
  }
  g_saved.rule = 0;
}

// The whole backstage (rules 0x19 1 on 1, 0x1A 2 on 2: no named area) is a
// story mode rule: in an exhibition its CPU stands still, whatever its
// records (its area, from the rule id, has none for the AI). So it is played
// as the parking lot (0x1B 1 on 1, 0x70 2 on 2: the CPU fights), starting
// there, with g_free_roam: every room shown, a fight box around the whole
// backstage, the backstage camera.
constexpr uint32_t kStoryContext = 0x82E3C1F4;

void ShapeRule(uint8_t* base, uint32_t rule, uint32_t like, bool select_only) {
  const uint32_t rules = Rd32(base + kRules);
  if (!rules) return;
  uint8_t* rec = base + rules + rule * kRuleSize;
  const uint8_t* from = base + rules + like * kRuleSize;
  uint8_t* rec2 = base + rules + kRule2 + rule * kRule2Size;
  if (g_saved.rule != rule) {  // (the whole backstage's are already saved)
    g_saved.rule = rule;
    std::memcpy(g_saved.rec, rec, kRuleSize);
    std::memcpy(g_saved.rec2, rec2, kRule2Size);
  }
  if (select_only) {
    for (const Span& span : kSelect) std::memcpy(rec + span.at, from + span.at, span.size);
    // Lumberjack: 2 wrestlers and 4 lumberjacks (the record has 3 wrestlers,
    // teams 0, 1, 2): the third person joins the lumberjacks (team 3).
    if (rule == kLumberjack) rec[2 + 2 * 3 + 1] = 3;
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
  g_free_roam = (rule == kWholeBackstage || rule == kWholeBackstage2) && !Rd32(base + kStoryContext);
  const uint32_t played = !g_free_roam ? rule : rule == kWholeBackstage ? 0x1Bu : 0x70u;
  if (g_free_roam) REXLOG_INFO("match types: free-roaming backstage, played as rule {:02X}", played);
  if (g_pending_like && ((rule >= g_pending_rule_lo && rule <= g_pending_rule_hi) ||
                         (!g_pending_select_only && rule == kWholeBackstage2)))
    ShapeRule(base, played, g_pending_like, g_pending_select_only);
  ctx.r4.u64 = played;
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
      REXLOG_INFO("match types: lumberjack {} stays ringside (character {:08X})", i, ch);
    }
  }
  ctx = saved;
}

}  // namespace

// A per-character check (sub_825EEA80(?, character)) that, in a Lumberjack
// match, says no for the lumberjacks - people 3 and up in the story match;
// here from person 2 (the third person is a lumberjack too).
REX_EXTERN(__imp__sub_825EEA80);
REX_HOOK_RAW(sub_825EEA80) {
  const uint32_t ch = ctx.r4.u32;
  __imp__sub_825EEA80(ctx, base);
  if (base[0x82E3DE00] == kLumberjack && ch && Rd32(base + ch + 1156) == 2) ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_82225D68);
REX_HOOK_RAW(sub_82225D68) {
  MarkLumberjacks(ctx, base);
  __imp__sub_82225D68(ctx, base);
}

// -- The whole backstage ---------------------------------------------------
//
// The free-roam backstage stage (arena 78) has every room loaded; a task
// ('CFRS', sub_825310B8 every frame) hides them all but the corridor, the
// interview set and the one room at a position - the walking player's in
// Road to WrestleMania, the match camera's otherwise - so a fight elsewhere
// looks into black rooms. For a match in no named area (rule 0x19 / 0x1A:
// the whole backstage), every room is shown after the update:
// sub_8252FFF0(task, room 0-9, show), less the objects the game always hides.
REX_EXTERN(__imp__sub_825310B8);
REX_HOOK_RAW(sub_825310B8) {
  const uint32_t task = ctx.r3.u32 - 32;
  __imp__sub_825310B8(ctx, base);
  constexpr uint32_t kLive = 0x82E3DE00, kStory = 0x82E3C1F4;
  if (Rd32(base + kLive + 64) != 78 || !g_free_roam || Rd32(base + kStory)) return;
  const auto saved = ctx;
  for (uint32_t room : {0u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u}) {
    ctx.r3.u64 = task;
    ctx.r4.u64 = room;
    ctx.r5.u64 = 1;
    sub_8252FFF0(ctx, base);
  }
  // The objects the update always hides stay hidden: sub_8252FF40(lo, hi, 0).
  for (uint32_t id : {6u, 25u, 43u, 72u, 85u, 172u, 184u, 197u, 198u}) {
    ctx.r3.u64 = id;
    ctx.r4.u64 = id;
    ctx.r5.u64 = 0;
    sub_8252FF40(ctx, base);
  }
  ctx = saved;
}

// -- The backstage camera --------------------------------------------------
//
// In-match cameras are made by sub_82313D98: the backstage camera (follows
// the fighters, turns with them) when sub_821852E0() says the match is
// backstage, the ring camera otherwise. sub_821852E0 leaves out the whole
// backstage (rule 0x19), which then gets the ring camera: far, high and
// fixed - it looks down onto the room boxes. While the cameras are made, the
// whole backstage counts as backstage.
namespace {

constexpr uint32_t kLiveSettings = 0x82E3DE00;
bool g_making_cameras = false;

bool WholeBackstage(uint8_t* base) { return Rd32(base + kLiveSettings + 64) == 78 && g_free_roam; }

float RdF(const uint8_t* p) {
  const uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WrF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Wr32(p, v);
}

}  // namespace

REX_EXTERN(__imp__sub_82313D98);
REX_HOOK_RAW(sub_82313D98) {
  g_making_cameras = true;
  __imp__sub_82313D98(ctx, base);
  g_making_cameras = false;
}

REX_EXTERN(__imp__sub_821852E0);
REX_HOOK_RAW(sub_821852E0) {
  if (WholeBackstage(base) && !Rd32(base + 0x82E3C1F4)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_821852E0(ctx, base);
}

// The backstage camera's distance: sub_822F26B8(camera) sets camera+932 =
// the framed radius x 3.7. A little closer in the free-roaming backstage:
// x 3.2 (x 2.4 was "really zoomed in").
REX_EXTERN(__imp__sub_822F26B8);
REX_HOOK_RAW(sub_822F26B8) {
  const uint32_t camera = ctx.r3.u32;
  __imp__sub_822F26B8(ctx, base);
  if (!WholeBackstage(base)) return;
  constexpr float kCloser = 3.2f / 3.7f;
  WrF(base + camera + 932, RdF(base + camera + 932) * kCloser);
}

// What the match camera frames: sub_82227690(camera) -> a sphere (centre
// +16/+20/+24, radius +32) around every competitor. Backstage with more than
// two people, it is centred on their average position instead (radius: the
// farthest of them from it).
REX_EXTERN(__imp__sub_82227690);
REX_HOOK_RAW(sub_82227690) {
  __imp__sub_82227690(ctx, base);
  const uint32_t sphere = ctx.r3.u32;
  if (!sphere || Rd32(base + kLiveSettings + 64) != 78) return;
  constexpr uint32_t kChars = 0x82E3CC50;
  float xs[9], zs[9];
  int n = 0;
  const auto saved = ctx;
  for (uint32_t i = 0; i < 9; ++i) {
    const uint32_t ch = Rd32(base + kChars + i * 4);
    if (!ch) continue;
    ctx.r3.u64 = ch;
    sub_82225D68(ctx, base);
    const bool competitor = ctx.r3.u32 != 0;
    const uint32_t pos = Rd32(base + ch + 304);
    if (!competitor || !pos) continue;
    xs[n] = RdF(base + pos);
    zs[n] = RdF(base + pos + 8);
    ++n;
  }
  ctx = saved;
  ctx.r3.u64 = sphere;
  if (n <= 2) return;
  // Around the fighters near the player (1P: the first competitor): their
  // average; anyone farther away (another fight elsewhere) is left out, so
  // the camera stays close to the player's fight.
  const float px = xs[0], pz = zs[0];
  constexpr float kNear = 220.0f, kMaxRadius = 90.0f;
  float mx = 0, mz = 0;
  int kept = 0;
  for (int i = 0; i < n; ++i) {
    if (std::hypot(xs[i] - px, zs[i] - pz) > kNear) continue;
    mx += xs[i];
    mz += zs[i];
    ++kept;
  }
  mx /= float(kept);
  mz /= float(kept);
  float r = 0;
  for (int i = 0; i < n; ++i)
    if (std::hypot(xs[i] - px, zs[i] - pz) <= kNear) r = std::max(r, std::hypot(xs[i] - mx, zs[i] - mz));
  r = std::min(r + 12.0f, kMaxRadius);
  WrF(base + sphere + 16, mx);
  WrF(base + sphere + 24, mz);
  WrF(base + sphere + 32, r);
}

// -- The whole backstage's fight box --------------------------------------
//
// A backstage match (sub_821852E0() true) keeps its fighters in a box set up
// by sub_8224EF28(box) for its area: centre box+32/+36/+40, half sizes +64
// inner x, +68 inner z, +72 outer x, +76 outer z (also copied to
// 0x82D9E9D4..E0, which the per-body clamp sub_821E5408 and the AI read).
// The whole backstage has no area there - the box would stay a point - so it
// gets a box around all of it: the walls of the whole backstage (bg78 wall
// mesh 0, x -530..510, z -550..470) keep the fighters in, as in story mode.
REX_EXTERN(__imp__sub_8224EF28);
REX_HOOK_RAW(sub_8224EF28) {
  const uint32_t box = ctx.r3.u32;
  __imp__sub_8224EF28(ctx, base);
  if (!box || !WholeBackstage(base) || Rd32(base + 0x82E3C1F4)) return;
  WrF(base + box + 32, -10.0f);
  WrF(base + box + 36, 0.0f);
  WrF(base + box + 40, -40.0f);
  WrF(base + box + 44, 1.0f);
  float kHalf[4] = {520.0f, 510.0f, 525.0f, 515.0f};
  if (const char* v = std::getenv("SVR2011_TEST_BOX")) {  // (test aid: half sizes)
    const float h = float(std::atof(v));
    for (float& f : kHalf) f = h;
  }
  constexpr uint32_t kGlobals = 0x82D9E9D4;
  for (uint32_t i = 0; i < 4; ++i) {
    WrF(base + box + 64 + i * 4, kHalf[i]);
    WrF(base + kGlobals + i * 4, kHalf[i]);
  }
  REXLOG_INFO("match types: the whole backstage's fight box covers all of it");
}

// -- Lumberjack start places ----------------------------------------------
//
// People start where sub_822AD738(placer, rule, ?) puts them: the placer's
// table (+4) has an 88-byte entry per rule id - +0 the id, then per person
// 12 bytes (+0 kind, 1 wrestler / 2 manager; int16 x, y, z, facing at +4..;
// y -120 in the ring, 0 on the floor; tenths). The story Lumberjack's entry
// (0x55) has 3 wrestlers in the ring and its lumberjacks on the floor around
// it: (50,0,500), (-500,0,0), (0,0,-500). Here person 2 is a lumberjack too:
// on the fourth side, (500, 0, 0), facing the ring.
REX_EXTERN(__imp__sub_822AD738);
REX_HOOK_RAW(sub_822AD738) {
  const uint32_t placer = ctx.r3.u32;
  if (ctx.r4.u32 != kLumberjack || !placer || Rd32(base + kStoryContext)) {
    __imp__sub_822AD738(ctx, base);
    return;
  }
  uint8_t* person2 = base + Rd32(base + placer + 4) + kLumberjack * 88 + 4 + 2 * 12;
  uint8_t saved[12];
  std::memcpy(saved, person2, 12);
  const uint8_t ringside[12] = {0x01, 0, 0, 0, 0x01, 0xF4, 0x00, 0x00, 0x00, 0x00, 0x03, 0x84};  // 500, 0, 0, 900
  std::memcpy(person2, ringside, 12);
  __imp__sub_822AD738(ctx, base);
  std::memcpy(person2, saved, 12);
}
