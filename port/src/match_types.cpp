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
#include "mystery_opponent.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <rex/filesystem.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "arena_mods.h"
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
// of it with this label, description and rule (its match type mod: `mod`).
struct Row {
  uint32_t anchor, group;
  uint32_t label, description, rule;
  const char* mod;
};

// PLAY -> ... (groups: 06 ONE ON ONE, 07 TWO ON TWO, 08 TRIPLE THREAT,
// 09 FATAL-4-WAY, 0A 6-MAN, 0C ROYAL RUMBLE). All the game's own rules and
// names: Falls Count Anywhere (the labels of the rows cut from the menu),
// 15- and 25-man Royal Rumble, Lumberjack.
const Row kRows[] = {
    {0xA053, 0x06, 0xA054, kNoText, 0x2B, "falls_count_anywhere"},  // ONE ON ONE: after FIRST BLOOD
    {0xA061, 0x07, 0xA062, kNoText, 0x2E, "falls_count_anywhere"},  // TWO ON TWO: after MIXED TAG
    {0xA06A, 0x08, 0xA06B, kNoText, 0x2F, "falls_count_anywhere"},  // TRIPLE THREAT: after NORMAL
    {0xA072, 0x09, 0xA073, kNoText, 0x30, "falls_count_anywhere"},  // FATAL-4-WAY: after NORMAL
    // FATAL-4-WAY: CHAMPIONSHIP SCRAMBLE (the game's own rule 0x26 and label
    // A038, cut from PLAY - its row there only opens submenus): 5 people, 2
    // start and one more comes in each minute; a fall makes an interim
    // champion and the match goes on; at the bell (5:00) the last one wins.
    {0xA072, 0x09, 0xA038, kNoText, 0x26, "championship_scramble"},
    {0xA08C, 0x0C, 0x5313, kNoText, 0x15, "royal_rumble_15_25"},  // ROYAL RUMBLE: 15-MAN after 10-MAN
    {0xA08D, 0x0C, 0x5314, kNoText, 0x17, "royal_rumble_15_25"},  // 25-MAN after 20-MAN
    {0xA07F, 0x0A, 0x0052, kNoText, 0x55, "lumberjack"},          // 6-MAN: LUMBERJACK after ARMAGEDDON
    // BACKSTAGE (1 on 1, 2 on 2): FREE-ROAMING BACKSTAGE after PARKING LOT -
    // the whole backstage (rules without a named area, see below).
    {0xA0DD, 0x12, 0x9C90, kNoText, 0x19, "free_roaming_backstage"},
    {0xA0DD, 0x14, 0x9C90, kNoText, 0x1A, "free_roaming_backstage"},
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

// Backstage mods with their own row (arena_mods.h BackstageRows: e.g. the SvR
// 2008 parking lot): in BACKSTAGE (1 on 1, group 0x12), after the row of
// their room's rule (0x1B + room), a copy labelled with the mod's row= text
// (string ids kOwnRowLabel + i, see MatchTypeString). The row's place in its
// group is remembered: picking it plays that room's rule with the mod's bg78.
constexpr uint32_t kBackstage1v1Group = 0x12, kBackstage1v1FirstRule = 0x1B;
constexpr uint32_t kOwnRowLabel = 0x0FA0B000;
int g_own_row_index[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
int g_pending_own_row = -1;
std::vector<uint32_t> g_own_row_text;  // guest strings

// WEAPONS EVERYWHERE: after each EXTREME RULES row (rules 0x4D-0x50, no
// disqualification; the Extreme Rules weapons are loaded), a copy labelled
// kWeaponsLabel. Its match is that Extreme Rules match with weapons already
// lying in and around the ring at the bell (see "Weapons everywhere" below).
constexpr uint32_t kWeaponsLabel = 0x0FA0B100, kWeaponsText = 0x0FA0B101;  // (label, description)
constexpr uint32_t kExtremeFirst = 0x4D, kExtremeLast = 0x50;
constexpr uint32_t kFullGroup = 0x06;  // ONE ON ONE: 14 rows, the most a list shows
struct WeaponsRow {
  uint32_t group;
  uint32_t index;  // its place in the group
};
std::vector<WeaponsRow> g_weapons_rows;
bool g_pending_weapons = false;
bool g_weapons = false;  // the match set up is a WEAPONS EVERYWHERE one
uint32_t g_weapons_text[2] = {};

// SLOBBER KNOCKER: after HANDICAP -> GAUNTLET (group 0x0B, rule 0x52), a
// copy: player 1 against an endless line of opponents, one at a time, until
// pinned (see "Slobber Knocker" below).
constexpr uint32_t kSlobberLabel = 0x0FA0B102, kSlobberText = 0x0FA0B103;
constexpr uint32_t kGauntletRow = 0xA081, kHandicapGroup = 0x0B, kGauntlet = 0x52;
constexpr int kSlobberPeople = 5;  // player 1, the opponent in the match, 3 waiting
int g_slobber_row = -1;  // its place in the group
bool g_pending_slobber = false;
bool g_slobber = false;  // the match set up is a Slobber Knocker
uint32_t g_slobber_text[2] = {};

// ELIMINATION: after NORMAL in TRIPLE THREAT (rule 0x0D) and FATAL-4-WAY
// (0x0E), a copy whose match has the ELIMINATION rule on (as MATCH CREATOR ->
// RULES -> ELIMINATION: live option +37 = 1): a pin or give up eliminates
// that wrestler - he leaves - and the match goes on until one is left. The
// engine does it (a triple threat: one eliminated, he walked out, the match
// went on, the last fall decided it, highlights as usual). The game itself
// offers ELIMINATION only for these rules' normal, falls count anywhere and
// extreme rules versions (MRPD), not with a cage, cell, ladder, table, TLC or
// backstage - left so.
constexpr uint32_t kElimLabel = 0x0FA0B104, kElimText = 0x0FA0B105;
// THREE STAGES OF HELL: in ONE ON ONE -> EXTREME RULES, after WEAPONS
// EVERYWHERE (three_stages.cpp).
constexpr uint32_t kStagesLabel = 0x0FA0B106, kStagesText = 0x0FA0B107;
WeaponsRow g_stages_row = {~0u, ~0u};
bool g_pending_stages = false;
// MYSTERY OPPONENT: ONE ON ONE -> NORMAL MATCH becomes a submenu of NORMAL
// MATCH and MYSTERY OPPONENT (mystery_opponent.cpp: a normal one on one).
WeaponsRow g_mystery_row = {~0u, ~0u};
bool g_pending_mystery = false;
uint32_t g_stages_text[2] = {};
constexpr uint32_t kTripleThreat = 0x0D, kFatal4Way = 0x0E;
std::vector<WeaponsRow> g_elim_rows;  // (group, place in the group)
bool g_pending_elimination = false;
bool g_elimination = false;  // the match set up is an ELIMINATION one
uint32_t g_elim_text[2] = {};

// Developer aid: SVR2011_TEST_RULE=<hex id> plays that rule record wherever
// the game would play ONE ON ONE -> NORMAL (id 0), to try a rule in game.
int g_test_rule = -1;
// Test aid: SVR2011_TEST_MODE=<mode> - ONE ON ONE NORMAL (route match) plays
// a mode only a runtime row gives: three_stages, elimination_tt,
// elimination_f4w, weapons, slobber, lumberjack (as if its row was picked).
enum class TestMode { kNone, kThreeStages, kElimTT, kElimF4W, kWeapons, kSlobber, kLumberjack };
TestMode g_test_mode = TestMode::kNone;

// Node ids (+0x1C): the game finds a node by binary search over the table
// (sub_82BA9B48) - B goes back through the row's parent (sub_8243FC18) - so
// they must not go down along the table. A submenu row added in the middle
// of a group takes the node id of the record before it (or of the one it
// replaces): the two then share it, which B doesn't mind (it wants the
// parent's group, the same for both); its own rows, at the table's end, get
// new ids above all others. (A new top id in the middle left B doing nothing
// in the submenu after backing out of character select.)
// The menu table with the rows added (empty if it isn't the main menus).
std::vector<uint8_t> WithRows(const uint8_t* table, uint32_t size) {
  if (size < kFirst || Rd32(table) != 1) return {};
  g_weapons_rows.clear();
  g_elim_rows.clear();
  const uint32_t total = Rd32(table + 4), shown = Rd32(table + 8);
  if (total == 0 || total > 0x1000 || kFirst + total * kRec > size) return {};
  // Test aid: SVR2011_TEST_MENU_DUMP=<hex group> - that group's records and
  // any record of rule 0x26 (CHAMPIONSHIP SCRAMBLE), logged as read.
  if (const char* v = std::getenv("SVR2011_TEST_MENU_DUMP")) {
    const uint32_t want = uint32_t(std::strtoul(v, nullptr, 16));
    for (uint32_t i = 0; i < total; ++i) {
      const uint8_t* rec = table + kFirst + i * kRec;
      if (Rd32(rec + 0x18) == want || (Rd32(rec + 0x5C) == 0x26 && Rd32(rec + 0x54) == kScreenMatch))
        REXLOG_INFO("menu dump: #{} label {:X} text {:X} group {:X} node {:X} parent {:X} +3C {:X} +40 {:X} screen {:X} rule {:X} (shown {})",
                    i, Rd32(rec), Rd32(rec + 4), Rd32(rec + 0x18), Rd32(rec + 0x1C), Rd32(rec + 0x38), Rd32(rec + 0x3C),
                    Rd32(rec + 0x40), Rd32(rec + 0x54), Rd32(rec + 0x5C), shown);
    }
  }
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
  // (match type mods switched off: their rows aren't added)
  const bool elimination = svr2011::MatchTypeOn("elimination"), slobber = svr2011::MatchTypeOn("slobber_knocker");
  const bool mystery = svr2011::MatchTypeOn("mystery_opponent"), weapons = svr2011::MatchTypeOn("weapons_everywhere");
  const bool stages = svr2011::MatchTypeOn("three_stages_of_hell");
  const bool more_backstage = svr2011::MatchTypeOn("backstage_more_people");
  const bool free_roam_on = svr2011::MatchTypeOn("free_roaming_backstage");
  for (uint32_t i = 0; i < total; ++i) {
    const uint8_t* rec = table + kFirst + i * kRec;
    out.insert(out.end(), rec, rec + kRec);
    if (Rd32(rec + 0x54) != kScreenMatch) continue;
    for (const Row& row : kRows) {
      if (Rd32(rec) != row.anchor || Rd32(rec + 0x18) != row.group || !svr2011::MatchTypeOn(row.mod)) continue;
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
    if (const uint32_t rule = Rd32(rec + 0x5C);
        elimination && (rule == kTripleThreat || rule == kFatal4Way) && Rd32(rec + 0x54) == kScreenMatch) {
      const uint32_t in_group = Rd32(rec + 0x18);
      std::vector<uint8_t> copy(rec, rec + kRec);
      Wr32(copy.data() + 0x00, kElimLabel);
      Wr32(copy.data() + 0x04, kElimText);
      Wr32(copy.data() + 0x40, 0);
      uint8_t* prev = out.data() + out.size() - kRec;  // (the group's last row stays last)
      const uint32_t flags = Rd32(prev + 0x3C);
      Wr32(prev + 0x3C, flags & ~2u);
      Wr32(copy.data() + 0x3C, (Rd32(copy.data() + 0x3C) & ~2u) | (flags & 2u));
      uint32_t index = 0;
      for (size_t at = kFirst; at < out.size(); at += kRec)
        if (Rd32(out.data() + at + 0x18) == in_group) ++index;
      g_elim_rows.push_back({in_group, index});
      out.insert(out.end(), copy.begin(), copy.end());
      ++added;
      REXLOG_INFO("match types: ELIMINATION (rule {:02X}) at group {:02X} row {}", rule, in_group, index);
    }
    if (slobber && Rd32(rec) == kGauntletRow && Rd32(rec + 0x18) == kHandicapGroup) {
      std::vector<uint8_t> copy(rec, rec + kRec);
      Wr32(copy.data() + 0x00, kSlobberLabel);
      Wr32(copy.data() + 0x04, kSlobberText);
      Wr32(copy.data() + 0x40, 0);
      uint8_t* prev = out.data() + out.size() - kRec;  // (the group's last row stays last)
      const uint32_t flags = Rd32(prev + 0x3C);
      Wr32(prev + 0x3C, flags & ~2u);
      Wr32(copy.data() + 0x3C, (Rd32(copy.data() + 0x3C) & ~2u) | (flags & 2u));
      int index = 0;
      for (size_t at = kFirst; at < out.size(); at += kRec)
        if (Rd32(out.data() + at + 0x18) == kHandicapGroup) ++index;
      g_slobber_row = index;
      out.insert(out.end(), copy.begin(), copy.end());
      ++added;
      REXLOG_INFO("match types: SLOBBER KNOCKER at group {:02X} row {}", kHandicapGroup, index);
    }
    if (mystery && Rd32(rec + 0x5C) == 0x00 && Rd32(rec + 0x18) == kFullGroup && submenu) {
      // NORMAL MATCH becomes a submenu (its own node) of NORMAL MATCH and
      // MYSTERY OPPONENT (a new group), as EXTREME RULES below.
      out.resize(out.size() - kRec);
      std::vector<uint8_t> row(submenu, submenu + kRec);
      Wr32(row.data() + 0x00, Rd32(rec + 0x00));
      Wr32(row.data() + 0x04, Rd32(rec + 0x04));
      Wr32(row.data() + 0x18, kFullGroup);
      Wr32(row.data() + 0x1C, Rd32(rec + 0x1C));
      Wr32(row.data() + 0x38, Rd32(rec + 0x38));
      Wr32(row.data() + 0x3C, (Rd32(row.data() + 0x3C) & ~2u) | (Rd32(rec + 0x3C) & 2u));
      Wr32(row.data() + 0x40, Rd32(rec + 0x40));
      out.insert(out.end(), row.begin(), row.end());
      for (uint32_t k = 0; k < 2; ++k) {  // (NORMAL MATCH, MYSTERY OPPONENT)
        std::vector<uint8_t> leaf(rec, rec + kRec);
        Wr32(leaf.data() + 0x18, group);
        Wr32(leaf.data() + 0x1C, node + k);
        Wr32(leaf.data() + 0x38, Rd32(rec + 0x1C));
        Wr32(leaf.data() + 0x14, Rd32(rec + 0x14) + 0x100);
        Wr32(leaf.data() + 0x40, 0);
        Wr32(leaf.data() + 0x3C, k == 1 ? Rd32(rec + 0x3C) | 2u : Rd32(rec + 0x3C) & ~2u);
        if (k == 1) {
          Wr32(leaf.data() + 0x00, svr2011::kMysteryLabel);
          Wr32(leaf.data() + 0x04, svr2011::kMysteryText);
        }
        tail.insert(tail.end(), leaf.begin(), leaf.end());
        ++added;
      }
      g_mystery_row = {group, 1};
      REXLOG_INFO("match types: NORMAL MATCH submenu with MYSTERY OPPONENT, group {:02X}", group);
      node += 2;
      ++group;
    }
    if (const uint32_t rule = Rd32(rec + 0x5C); (weapons || stages) && rule >= kExtremeFirst &&
                                                rule <= kExtremeLast && Rd32(rec + 0x18) == kFullGroup && submenu) {
      // A full list (14 rows): EXTREME RULES becomes a submenu (a new node in
      // its place) of EXTREME RULES and WEAPONS EVERYWHERE (a new group).
      out.resize(out.size() - kRec);
      std::vector<uint8_t> row(submenu, submenu + kRec);
      Wr32(row.data() + 0x00, Rd32(rec + 0x00));
      Wr32(row.data() + 0x04, Rd32(rec + 0x04));  // (description)
      Wr32(row.data() + 0x18, kFullGroup);
      Wr32(row.data() + 0x1C, Rd32(rec + 0x1C));  // (its node: EXTREME RULES' own - node ids, above)
      Wr32(row.data() + 0x38, Rd32(rec + 0x38));
      Wr32(row.data() + 0x3C, (Rd32(row.data() + 0x3C) & ~2u) | (Rd32(rec + 0x3C) & 2u));
      Wr32(row.data() + 0x40, Rd32(rec + 0x40));
      out.insert(out.end(), row.begin(), row.end());
      // (EXTREME RULES, then WEAPONS EVERYWHERE and THREE STAGES OF HELL - those on)
      std::vector<uint32_t> kinds = {0};
      if (weapons) kinds.push_back(1);
      if (stages) kinds.push_back(2);
      for (uint32_t k = 0; k < kinds.size(); ++k) {
        std::vector<uint8_t> leaf(rec, rec + kRec);
        Wr32(leaf.data() + 0x18, group);
        Wr32(leaf.data() + 0x1C, node + k);
        Wr32(leaf.data() + 0x38, Rd32(rec + 0x1C));
        Wr32(leaf.data() + 0x14, Rd32(rec + 0x14) + 0x100);  // (depth +0x16: one down)
        Wr32(leaf.data() + 0x40, 0);
        Wr32(leaf.data() + 0x3C, k + 1 == kinds.size() ? Rd32(rec + 0x3C) | 2u : Rd32(rec + 0x3C) & ~2u);
        if (kinds[k] == 1) {
          Wr32(leaf.data() + 0x00, kWeaponsLabel);
          Wr32(leaf.data() + 0x04, kWeaponsText);
          g_weapons_rows.push_back({group, k});
        } else if (kinds[k] == 2) {
          Wr32(leaf.data() + 0x00, kStagesLabel);
          Wr32(leaf.data() + 0x04, kStagesText);
          Wr32(leaf.data() + 0x5C, 0x00);  // (a normal one on one; three_stages.cpp changes its rules)
          g_stages_row = {group, k};
        }
        tail.insert(tail.end(), leaf.begin(), leaf.end());
        ++added;
      }
      REXLOG_INFO("match types: EXTREME RULES (rule {:02X}) submenu with WEAPONS EVERYWHERE, group {:02X}", rule, group);
      node += uint32_t(kinds.size());
      ++group;
    } else if (const uint32_t rule = Rd32(rec + 0x5C); weapons && rule >= kExtremeFirst && rule <= kExtremeLast) {
      const uint32_t in_group = Rd32(rec + 0x18);
      std::vector<uint8_t> copy(rec, rec + kRec);
      Wr32(copy.data() + 0x00, kWeaponsLabel);
      Wr32(copy.data() + 0x04, kWeaponsText);
      Wr32(copy.data() + 0x40, 0);
      uint8_t* prev = out.data() + out.size() - kRec;  // (the group's last row stays last)
      const uint32_t flags = Rd32(prev + 0x3C);
      Wr32(prev + 0x3C, flags & ~2u);
      Wr32(copy.data() + 0x3C, (Rd32(copy.data() + 0x3C) & ~2u) | (flags & 2u));
      uint32_t index = 0;
      for (size_t at = kFirst; at < out.size(); at += kRec)
        if (Rd32(out.data() + at + 0x18) == in_group) ++index;
      g_weapons_rows.push_back({in_group, index});
      out.insert(out.end(), copy.begin(), copy.end());
      ++added;
      REXLOG_INFO("match types: WEAPONS EVERYWHERE (rule {:02X}) at group {:02X} row {}", rule, in_group, index);
    }
    if (Rd32(rec + 0x18) == kBackstage1v1Group) {
      // (after the room's row and the rows added after it)
      const auto& own = svr2011::BackstageRows();
      for (size_t k = 0; k < own.size() && k < 8; ++k) {
        if (Rd32(rec + 0x5C) != kBackstage1v1FirstRule + uint32_t(own[k].area)) continue;
        std::vector<uint8_t> copy(rec, rec + kRec);
        Wr32(copy.data() + 0x00, kOwnRowLabel + uint32_t(k));
        Wr32(copy.data() + 0x64, kNoText);
        Wr32(copy.data() + 0x40, 0);
        // (the group's last row stays last)
        uint8_t* prev = out.data() + out.size() - kRec;
        const uint32_t flags = Rd32(prev + 0x3C);
        Wr32(prev + 0x3C, flags & ~2u);
        Wr32(copy.data() + 0x3C, (Rd32(copy.data() + 0x3C) & ~2u) | (flags & 2u));
        uint32_t index = 0;  // the row's place in its group
        for (size_t at = kFirst; at < out.size(); at += kRec)
          if (Rd32(out.data() + at + 0x18) == kBackstage1v1Group) ++index;
        g_own_row_index[k] = int(index);
        out.insert(out.end(), copy.begin(), copy.end());
        ++added;
        REXLOG_INFO("match types: backstage row '{}' at BACKSTAGE row {}", own[k].label, index);
      }
    }
    for (Backstage& b : g_backstage) {
      if (!more_backstage || !submenu || areas.size() != 7) break;
      if (Rd32(rec) != b.anchor || Rd32(rec + 0x18) != b.group) continue;
      // The submenu row: in this group, a new node.
      std::vector<uint8_t> row(submenu, submenu + kRec);
      Wr32(row.data() + 0x18, b.group);
      Wr32(row.data() + 0x1C, Rd32(rec + 0x1C));  // (its node: the anchor's - node ids, above)
      Wr32(row.data() + 0x38, Rd32(rec + 0x38));
      Wr32(row.data() + 0x3C, Rd32(row.data() + 0x3C) & ~2u);
      Wr32(row.data() + 0x40, 0);
      out.insert(out.end(), row.begin(), row.end());
      ++added;
      // Its areas: a new group under that node.
      b.menu_group = group;
      const uint32_t count = uint32_t(areas.size()) + (free_roam_on ? 1 : 0);
      for (uint32_t k = 0; k < count; ++k) {
        // The 7 areas, then FREE-ROAMING BACKSTAGE (the group's last).
        const bool free_roam = k == areas.size();
        const bool last = k + 1 == count;
        std::vector<uint8_t> leaf(areas[free_roam ? k - 1 : k], areas[free_roam ? k - 1 : k] + kRec);
        Wr32(leaf.data() + 0x18, group);
        Wr32(leaf.data() + 0x1C, node + k);
        Wr32(leaf.data() + 0x38, Rd32(rec + 0x1C));
        Wr32(leaf.data() + 0x40, 0);
        const uint32_t flags = Rd32(leaf.data() + 0x3C);
        Wr32(leaf.data() + 0x3C, last ? flags | 2u : flags & ~2u);
        if (free_roam) {
          Wr32(leaf.data() + 0x00, kFreeRoamLabel);
          Wr32(leaf.data() + 0x5C, kWholeBackstage2);
        }
        tail.insert(tail.end(), leaf.begin(), leaf.end());
        ++added;
      }
      node += count;
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
// +44 (match family) and +52, +64 (its first rule flag); +34 (the people
// count) and +35 (free for all) of the 64-byte option record. (Only +35 was
// copied before: a 6-man backstage match had 4 people - the areas' 2 on 2
// count.)
constexpr uint32_t kRules = 0x82EDE954;  // -> 119 x 100 bytes, then 119 x 64
constexpr uint32_t kRuleSize = 100, kRule2 = 11900, kRule2Size = 64;
constexpr uint32_t kRule2People = 34, kRule2FreeForAll = 35;
struct Span {
  uint32_t at, size;
};
constexpr Span kShape[] = {{0, 28}, {44, 4}, {52, 4}, {64, 1}};
// Lumberjack (0x55, story mode only): 2 wrestlers picked (the 1 on 1 select
// screen of rule 00: +24 team layout, +36, +44, +52, +64) and 4 lumberjacks
// - people 2-5, team 3, as manager slots (kind 2: the select screen leaves
// them out) filled with random superstars as the match loads (below).
constexpr uint32_t kLumberjack = 0x55, kLumberjackLike = 0x00, kSixMan = 0x0A;
// At most 4: the people builder (sub_828BBEF0) takes exactly 6 people from
// the match's slots (the referee is person 6, the commentators 7-8) - 6-8
// lumberjacks would need it and every per-person table behind it redone.
constexpr uint32_t kMaxLumberjacks = 4;
// How many lumberjacks (test aid: SVR2011_TEST_LJ_COUNT=<n>, 1-4).
uint32_t LumberjackCount() {
  static const uint32_t n = [] {
    const char* v = std::getenv("SVR2011_TEST_LJ_COUNT");
    return v ? std::clamp<uint32_t>(uint32_t(std::atoi(v)), 1, kMaxLumberjacks) : 4u;
  }();
  return n;
}
constexpr Span kSelect[] = {{24, 4}, {36, 4}, {44, 4}, {52, 4}, {64, 1}};

// The match row picked last: the rule record to reshape like which, and how
// (pending_like ~0: none).
uint32_t g_pending_rule_lo = 0, g_pending_rule_hi = 0, g_pending_like = ~0u;
bool g_pending_select_only = false;
bool g_lumberjacks_chosen = false;  // (people 2-5 of this Lumberjack match filled)
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
    // teams 0, 1, 2, and 3 lumberjacks): people 2-5 team 3, manager slots.
    if (rule == kLumberjack) {
      for (int i = 2; i < 6; ++i) rec[2 + i * 3 + 1] = 3, rec[2 + i * 3 + 2] = 2;
    }
    // Slobber Knocker: the 2 waiting opponents (people 2-3, kind 3) as manager
    // slots while the select screen runs (it would have them picked); kind 3
    // again as the people are built (sub_828BBEF0 below).
    if (rule == kGauntlet && g_slobber) {
      // (and a third: person 4 - the gauntlet has 4 people, the 5-person one
      // 0x58 shows 5 work)
      const uint8_t* five = base + rules + 0x58 * kRuleSize;  // (the 5-person gauntlet's count)
      rec[0] = five[0], rec[1] = five[1];
      rec2[34] = base[rules + kRule2 + 0x58 * kRule2Size + 34];
      rec[2 + 4 * 3] = 4, rec[2 + 4 * 3 + 1] = 1;
      for (int i = 2; i < kSlobberPeople; ++i) rec[2 + i * 3 + 2] = 2;
    }
  } else {
    for (const Span& span : kShape) std::memcpy(rec + span.at, from + span.at, span.size);
    for (uint32_t at : {kRule2People, kRule2FreeForAll}) rec2[at] = base[rules + kRule2 + like * kRule2Size + at];
  }
  REXLOG_INFO("match types: rule {:02X} set up like rule {:02X}{}", rule, like, select_only ? " (select screen)" : "");
}

}  // namespace

namespace svr2011 {

bool MatchTypeOn(const char* id) {
  static const std::map<std::string, bool> on = [] {
    std::map<std::string, bool> m;
    const std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "Mods" / "MatchTypes";
    std::error_code ec;
    for (const char* k : {"falls_count_anywhere", "championship_scramble", "royal_rumble_15_25", "lumberjack",
                          "free_roaming_backstage", "backstage_more_people", "weapons_everywhere", "slobber_knocker",
                          "three_stages_of_hell", "elimination", "mystery_opponent"}) {
      m[k] = !std::filesystem::exists(dir / k / "disabled", ec);
      if (!m[k]) REXLOG_INFO("match types: {} off (mod switched off)", k);
    }
    return m;
  }();
  const auto it = on.find(id);
  return it == on.end() || it->second;
}

void InstallMatchTypes(rex::memory::Memory* memory) {
  g_memory = memory;
  if (const char* v = std::getenv("SVR2011_TEST_MODE"); v && *v) {
    const std::string m = v;
    struct Mode {
      const char* name;
      TestMode mode;
      int rule;
    };
    constexpr Mode kModes[] = {{"three_stages", TestMode::kThreeStages, 0x00},
                               {"elimination_tt", TestMode::kElimTT, int(kTripleThreat)},
                               {"elimination_f4w", TestMode::kElimF4W, int(kFatal4Way)},
                               {"weapons", TestMode::kWeapons, int(kExtremeFirst)},
                               {"slobber", TestMode::kSlobber, int(kGauntlet)},
                               {"lumberjack", TestMode::kLumberjack, int(kLumberjack)}};
    for (const Mode& k : kModes)
      if (m == k.name) {
        g_test_mode = k.mode;
        if (k.rule) g_test_rule = k.rule;
        REXLOG_INFO("match types: ONE ON ONE NORMAL plays {} (SVR2011_TEST_MODE, rule {:02X})", k.name, k.rule);
      }
    if (g_test_mode == TestMode::kNone) REXLOG_WARN("match types: unknown SVR2011_TEST_MODE '{}'", m);
  }
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
  const uint32_t group = ctx.r4.u32, ctx_row = ctx.r5.u32 & 0xFFFF;
  __imp__sub_8243FCC8(ctx, base);
  const int32_t result = ctx.r3.s32;
  if (result < 2000 || result >= 2119) return;
  g_pending_like = ~0u;
  g_pending_own_row = -1;
  g_pending_slobber = group == kHandicapGroup && g_slobber_row >= 0 && ctx_row == uint32_t(g_slobber_row);
  if (g_pending_slobber) {  // (the 1 on 1 select screen: player 1 and the first opponent)
    g_pending_rule_lo = g_pending_rule_hi = kGauntlet;
    g_pending_like = kLumberjackLike;
    g_pending_select_only = true;
  }
  g_pending_stages = group == g_stages_row.group && ctx_row == g_stages_row.index;
  g_pending_mystery = group == g_mystery_row.group && ctx_row == g_mystery_row.index;
  g_pending_elimination = false;
  for (const WeaponsRow& w : g_elim_rows)
    if (group == w.group && ctx_row == w.index) g_pending_elimination = true;
  g_pending_weapons = false;
  for (const WeaponsRow& w : g_weapons_rows)
    if (group == w.group && ctx_row == w.index) g_pending_weapons = true;
  for (int k = 0; k < 8; ++k)
    if (group == kBackstage1v1Group && g_own_row_index[k] >= 0 && ctx_row == uint32_t(g_own_row_index[k]))
      g_pending_own_row = k;
  if (std::getenv("SVR2011_TEST_ROW_LOG"))
    REXLOG_INFO("match types: row picked: group {:02X} row {} -> rule {:02X} (own row {})", group, ctx_row,
                result - 2000, g_pending_own_row);
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
  // (test aid SVR2011_TEST_MODE: ONE ON ONE NORMAL as the mode's row)
  if (g_test_mode != TestMode::kNone && result == 2000) {
    switch (g_test_mode) {
      case TestMode::kThreeStages: g_pending_stages = true; break;
      case TestMode::kElimTT:
      case TestMode::kElimF4W: g_pending_elimination = true; break;
      case TestMode::kWeapons: g_pending_weapons = true; break;
      case TestMode::kSlobber:
        g_pending_slobber = true;
        g_pending_rule_lo = g_pending_rule_hi = kGauntlet;
        g_pending_like = kLumberjackLike;
        g_pending_select_only = true;
        break;
      case TestMode::kLumberjack:
        g_pending_rule_lo = g_pending_rule_hi = kLumberjack;
        g_pending_like = kLumberjackLike;
        g_pending_select_only = true;
        break;
      default: break;
    }
  }
}

// A match is set up: sub_827374A0(match, rule, ...).
REX_EXTERN(__imp__sub_827374A0);
REX_HOOK_RAW(sub_827374A0) {
  const uint32_t rule = ctx.r4.u32;
  // Test aid: SVR2011_TEST_SETUP_TRACE=1 - each match set-up with its caller
  // and the guest call stack (for REMATCH and the roaming FCA's reload).
  if (std::getenv("SVR2011_TEST_SETUP_TRACE")) {
    std::string chain;
    uint32_t sp = ctx.r1.u32;
    for (int k = 0; k < 12 && sp; ++k) {
      const uint32_t next = Rd32(base + sp);
      if (next <= sp || next - sp > 0x10000 || next < 0x70000000u || next >= 0x80000000u) break;  // (guest stacks)
      chain += fmt::format(" {:08X}", Rd32(base + next - 8));
      sp = next;
    }
    REXLOG_INFO("match types: set-up of rule {:02X} (match {:08X}) from {:08X};{}", rule, ctx.r3.u32, uint32_t(ctx.lr), chain);
  }
  svr2011::SetMatchLoading(true);  // (arena_mods: a custom arena's loading pictures)
  RestoreRule(base);
  g_lumberjacks_chosen = false;
  g_free_roam = (rule == kWholeBackstage || rule == kWholeBackstage2) && !Rd32(base + kStoryContext);
  // A backstage mod's own row: its bg78 for this match (any other: the usual)
  {
    const auto& own = svr2011::BackstageRows();
    const int k = g_pending_own_row;
    const bool use = k >= 0 && size_t(k) < own.size() && !Rd32(base + kStoryContext) &&
                     rule == kBackstage1v1FirstRule + uint32_t(own[size_t(k)].area);
    if (use) REXLOG_INFO("match types: backstage area '{}'", own[size_t(k)].label);
    svr2011::UseBackstageRow(use ? k : -1);
    g_pending_own_row = -1;
  }
  g_slobber = g_pending_slobber && rule == kGauntlet && !Rd32(base + kStoryContext);
  g_pending_slobber = false;
  if (g_slobber) {
    REXLOG_INFO("match types: SLOBBER KNOCKER");
    svr2011::SlobberKnockerStart();
  }
  svr2011::ThreeStagesSetup(g_pending_stages && rule == 0x00 && !Rd32(base + kStoryContext));
  g_pending_stages = false;
  svr2011::MysteryOpponentSetup(base, g_pending_mystery && rule == 0x00 && !Rd32(base + kStoryContext));
  g_pending_mystery = false;
  g_elimination = g_pending_elimination && (rule == kTripleThreat || rule == kFatal4Way) && !Rd32(base + kStoryContext);
  g_pending_elimination = false;
  if (g_elimination) REXLOG_INFO("match types: ELIMINATION (rule {:02X})", rule);
  g_weapons = g_pending_weapons && rule >= kExtremeFirst && rule <= kExtremeLast && !Rd32(base + kStoryContext);
  g_pending_weapons = false;
  if (g_weapons) REXLOG_INFO("match types: WEAPONS EVERYWHERE (rule {:02X})", rule);
  const uint32_t played = !g_free_roam ? rule : rule == kWholeBackstage ? 0x1Bu : 0x70u;
  if (g_free_roam) REXLOG_INFO("match types: free-roaming backstage, played as rule {:02X}", played);
  if (g_pending_like != ~0u && ((rule >= g_pending_rule_lo && rule <= g_pending_rule_hi) ||
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
  for (uint32_t i = 0; i < 2 + kMaxLumberjacks; ++i) {
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
  if (base[0x82E3DE00] == kLumberjack && ch && Rd32(base + ch + 1156) >= 2) ctx.r3.u64 = 0;
}

REX_EXTERN(__imp__sub_82225D68);
REX_HOOK_RAW(sub_82225D68) {
  MarkLumberjacks(ctx, base);
  __imp__sub_82225D68(ctx, base);
}

// The lumberjacks: as the match's people are built (sub_828BBEF0(match):
// its picks, 2116-byte slots at match+432 - +8 superstar id * 100 + attire
// (51200: none), +54 the id, +5 the team, +4 the kind, -8 the controller
// (1: CPU) - become people), slots 2-5 of a Lumberjack match (manager slots:
// the select screen leaves them empty) get 4 random superstars - selectable,
// not DLC, not the two picked, of the picked wrestlers' gender (a diva match
// gets divas) - as CPU managers of team 3 (ringside).
namespace {

constexpr uint32_t kSlots = 432, kSlotSize = 2116;
uint32_t g_lumberjack_ids[kMaxLumberjacks] = {};

uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
void Wr16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }

// The lumberjacks' slot kind: 2, managers - they stay at ringside (as
// wrestlers, kind 0, they get in and brawl). (test aid:
// SVR2011_TEST_LJ_KIND=<n>)
uint8_t LumberjackKind() {
  static const int kind = [] { const char* v = std::getenv("SVR2011_TEST_LJ_KIND"); return v ? std::atoi(v) : 2; }();
  return uint8_t(kind);
}

// (each time the people are built - the choice is kept unless it clashes with
// a pick)
void FillRandomSlots(uint8_t* base, uint32_t match, uint32_t count, uint8_t team, uint8_t kind, const char* what);

void FillLumberjackSlots(uint8_t* base, uint32_t match) {
  FillRandomSlots(base, match, LumberjackCount(), 3, LumberjackKind(), "lumberjacks");
}

// People 2.. of the match's slots: `count` random superstars (not the picks,
// their gender), CPU, on `team` with slot kind `kind`.
void FillRandomSlots(uint8_t* base, uint32_t match, uint32_t count, uint8_t team, uint8_t kind, const char* what) {
  constexpr uint32_t kIdToIndex = 0x82DB3610, kRecords = 0x82E407C0, kRecordSize = 260;
  constexpr uint32_t kOwnId = 32, kName = 34, kGender = 208, kSelectable = 221, kSamePerson = 228, kDlc = 257;
  uint8_t* slot[2 + kMaxLumberjacks];
  for (uint32_t i = 0; i < 2 + count; ++i) slot[i] = base + match + kSlots + i * kSlotSize;
  const uint32_t picked[2] = {Rd32(slot[0] + 8) / 100, Rd32(slot[1] + 8) / 100};  // (+8: what the people are built from)
  auto record = [&](uint32_t id) -> const uint8_t* {
    if (id >= 1000) return nullptr;
    const uint32_t index = Rd16(base + kIdToIndex + id * 2);
    if (index >= 1000) return nullptr;
    const uint8_t* rec = base + kRecords + index * kRecordSize;
    return Rd16(rec + kOwnId) == id ? rec : nullptr;
  };
  const uint8_t* first = record(picked[0]);
  const uint32_t gender = first ? first[kGender] : 0;
  // (one of the picks, under another id: the same name or the same person, +228)
  auto is_picked = [&](uint32_t id) {
    const uint8_t* rec = record(id);
    if (!rec) return false;
    for (uint32_t p : picked) {
      const uint8_t* pr = record(p);
      if (id == p || (pr && (std::strncmp(reinterpret_cast<const char*>(rec + kName), reinterpret_cast<const char*>(pr + kName), 32) == 0 ||
                             (Rd16(rec + kSamePerson) && Rd16(rec + kSamePerson) == Rd16(pr + kSamePerson)) ||
                             Rd16(rec + kSamePerson) == p || Rd16(pr + kSamePerson) == id)))
        return true;
    }
    return false;
  };
  bool keep = g_lumberjacks_chosen;
  for (uint32_t j = 0; j < count; ++j) {
    const uint32_t id = g_lumberjack_ids[j];
    const uint8_t* rec = record(id);
    if (!rec || is_picked(id) || rec[kGender] != gender) keep = false;
  }
  std::string names;
  if (!keep) {
    std::vector<uint32_t> pool;
    for (uint32_t id = 1; id < 1000; ++id) {
      const uint8_t* rec = record(id);
      if (rec && rec[kSelectable] == 1 && !rec[kDlc] && rec[kGender] == gender && !is_picked(id))
        pool.push_back(id);
    }
    if (pool.size() < count) {
      REXLOG_WARN("match types: {} - only {} superstars to choose from", what, pool.size());
      return;
    }
    static std::mt19937 rng{std::random_device{}()};
    std::shuffle(pool.begin(), pool.end(), rng);
    uint32_t chosen = 0;
    std::vector<std::string> seen;  // (distinct names: one superstar under two ids once)
    for (uint32_t id : pool) {
      if (chosen == count) break;
      const std::string name(reinterpret_cast<const char*>(record(id) + kName), strnlen(reinterpret_cast<const char*>(record(id) + kName), 32));
      if (std::find(seen.begin(), seen.end(), name) != seen.end()) continue;
      seen.push_back(name);
      g_lumberjack_ids[chosen++] = id;
      names += fmt::format("{}{}", names.empty() ? "" : ", ", name);
    }
    if (chosen < count) return;
    g_lumberjacks_chosen = true;
  }
  for (uint32_t i = 2; i < 2 + count; ++i) {
    const uint32_t id = g_lumberjack_ids[i - 2], now = Rd32(slot[i] + 8);
    if (now != 51200 && now != id * 100 + 2) continue;  // (not an empty slot: someone's pick)
    Wr32(slot[i] + 8, id * 100 + 2);  // (attire: the first, as the select screen gives)
    Wr16(slot[i] + 54, id);
    slot[i][5] = team;
    slot[i][4] = kind;
    slot[i][-8] = 1;     // (controller: the CPU)
  }
  if (!names.empty()) REXLOG_INFO("match types: {}: {}", what, names);
}

}  // namespace

REX_EXTERN(__imp__sub_828BBEF0);
REX_HOOK_RAW(sub_828BBEF0) {
  if (std::getenv("SVR2011_TEST_PEOPLE")) {  // (test aid: the match's slots 0-5, 64 bytes each)
    for (uint32_t i = 0; i < 6; ++i) {
      const uint32_t slot = ctx.r3.u32 + 432 + i * 2116;
      std::string hex;
      for (uint32_t at = 0; at < 64; ++at) hex += fmt::format("{}{:02X}", at % 16 ? "" : " ", base[slot - 8 + at]);
      REXLOG_INFO("[svr2011] people: match {:08X} slot {} (from -8):{}", ctx.r3.u32, i, hex);
    }
  }
  if (base[0x82E3DE00] == kLumberjack && !Rd32(base + kStoryContext) && ctx.r3.u32) {
    FillLumberjackSlots(base, ctx.r3.u32);
  }
  if (g_slobber && base[0x82E3DE00] == kGauntlet && ctx.r3.u32) {  // (the 2 waiting opponents)
    if (const uint32_t rules = Rd32(base + kRules))
      for (int i = 2; i < kSlobberPeople; ++i) base[rules + kGauntlet * kRuleSize + 2 + i * 3 + 2] = 3;
    FillRandomSlots(base, ctx.r3.u32, kSlobberPeople - 2, 1, 3, "slobber knocker opponents");
    for (uint32_t i = kSlobberPeople; i < 6; ++i) Wr32(base + ctx.r3.u32 + kSlots + i * kSlotSize + 8, 51200);
  }
  svr2011::MysteryOpponentFill(base, ctx.r3.u32);  // (mystery_opponent.h)
  __imp__sub_828BBEF0(ctx, base);
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
// x 3.2 (x 2.4 was "really zoomed in"); a backstage mod's area: at most its
// camera= distance.
REX_EXTERN(__imp__sub_822F26B8);
REX_HOOK_RAW(sub_822F26B8) {
  const uint32_t camera = ctx.r3.u32;
  __imp__sub_822F26B8(ctx, base);
  // A backstage mod's own area may cap it (manifest camera=): a small area
  // ringed by objects (the 2008 parking lot's cars) - farther out the
  // camera would stand among them.
  if (svr2011::g_active_row >= 0 && size_t(svr2011::g_active_row) < svr2011::BackstageRows().size() &&
      !Rd32(base + 0x82E3C1F4)) {
    const float cap = svr2011::BackstageRows()[size_t(svr2011::g_active_row)].camera;
    if (cap > 0 && RdF(base + camera + 932) > cap) WrF(base + camera + 932, cap);
  }
  if (!WholeBackstage(base)) return;
  constexpr float kCloser = 3.2f / 3.7f;
  WrF(base + camera + 932, RdF(base + camera + 932) * kCloser);
}

// A backstage mod's own area with camera_height=: the match camera's eye is
// at least that high. The backstage camera's frame update (sub_822F2A38, the
// camera in r3) puts the eye at +304..+312 (the framed centre + its rotation
// x the distance; for rule 1B always on the +z side, looking toward -z), the
// look direction at +288..+296, and builds the view from them
// (sub_82227150(0x82E3CC00, cam+304, cam+288, ...)); the target (the framed
// centre) is on its stack at +128/+132/+136. There the eye is raised (-Y is
// up) and the direction re-aimed at the target: in the 2008 parking lot the
// camera, a radius x 3.7 out on the +z side, otherwise stood behind the
// vehicles on that side. (Clamping the eye inside the area, or putting it on
// the area's centre side, gave extreme close-ups.)
uint32_t g_camera_frame = 0;
REX_EXTERN(__imp__sub_822F2A38);
REX_HOOK_RAW(sub_822F2A38) {
  g_camera_frame = ctx.r3.u32;
  __imp__sub_822F2A38(ctx, base);
  g_camera_frame = 0;
}
REX_EXTERN(__imp__sub_82227150);
REX_HOOK_RAW(sub_82227150) {
  const uint32_t cam = g_camera_frame;
  if (cam && ctx.r3.u32 == 0x82E3CC00 && ctx.r4.u32 == cam + 304 && ctx.r5.u32 == cam + 288 &&
      svr2011::g_active_row >= 0 && size_t(svr2011::g_active_row) < svr2011::BackstageRows().size() &&
      !Rd32(base + 0x82E3C1F4)) {
    const auto& row = svr2011::BackstageRows()[size_t(svr2011::g_active_row)];
    const float height = row.camera_height;
    float ey = RdF(base + cam + 308);
    if (height > 0 && ey > -height) {
      const uint32_t sp = ctx.r1.u32;  // (still sub_822F2A38's frame)
      const float tx = RdF(base + sp + 128), ty = RdF(base + sp + 132), tz = RdF(base + sp + 136);
      const float ex = RdF(base + cam + 304), ez = RdF(base + cam + 312);
      ey = -height;
      WrF(base + cam + 308, ey);
      const float dx = tx - ex, dy = ty - ey, dz = tz - ez;
      const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (len > 1e-3f) {
        WrF(base + cam + 288, dx / len);
        WrF(base + cam + 292, dy / len);
        WrF(base + cam + 296, dz / len);
      }
    }
  }
  __imp__sub_82227150(ctx, base);
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
  if (box && std::getenv("SVR2011_TEST_BOX_LOG"))  // (test aid: the area's own box)
    REXLOG_INFO("match types: area box centre {:.0f} {:.0f} {:.0f}, half {:.0f} {:.0f} / {:.0f} {:.0f}",
                RdF(base + box + 32), RdF(base + box + 36), RdF(base + box + 40), RdF(base + box + 64),
                RdF(base + box + 68), RdF(base + box + 72), RdF(base + box + 76));
  // A backstage mod's own area with its own box (manifest box=): centred on
  // it, the area's half sizes inside, a little more outside.
  if (box && svr2011::g_active_row >= 0 && size_t(svr2011::g_active_row) < svr2011::BackstageRows().size() &&
      !Rd32(base + 0x82E3C1F4)) {
    const auto& row = svr2011::BackstageRows()[size_t(svr2011::g_active_row)];
    if (row.has_box) {
      WrF(base + box + 32, row.box[0]);
      WrF(base + box + 40, row.box[1]);
      const float half[4] = {row.box[2], row.box[3], row.box[2] + 5.0f, row.box[3] + 5.0f};
      for (uint32_t i = 0; i < 4; ++i) {
        WrF(base + box + 64 + i * 4, half[i]);
        WrF(base + 0x82D9E9D4 + i * 4, half[i]);
      }
      REXLOG_INFO("match types: '{}' fight box centre {:.0f} {:.0f}, half {:.0f} {:.0f}", row.label, row.box[0],
                  row.box[1], row.box[2], row.box[3]);
      return;
    }
  }
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
// The match in the log (once per placement - the match's start): rule, arena,
// each person: name (from the person's roster record), person number, team.
void LogMatch(PPCContext& ctx, uint8_t* base, uint32_t rule) {
  constexpr uint32_t kLive = 0x82E3DE00, kChars = 0x82E3CC50, kRoster = 0x82EDEA88;
  const auto saved = ctx;
  std::string people;
  for (uint32_t i = 0; i < 10; ++i) {
    const uint32_t ch = Rd32(base + kChars + i * 4);
    if (!ch) continue;
    const uint32_t id = Rd32(base + ch + 1156);
    std::string name;
    if (const uint32_t roster = Rd32(base + kRoster)) {
      ctx.r3.u64 = roster;
      ctx.r4.u64 = id;
      sub_828B5A18(ctx, base);
      if (const uint32_t rec = ctx.r3.u32) {  // (the record's name: its text at +0x22; +0x20 is a number)
        if (std::getenv("SVR2011_TEST_ROSTER_DUMP")) {  // (test aid: the record, 260 bytes)
          std::string hex;
          for (uint32_t at = 0; at < 260; ++at) hex += fmt::format("{}{:02X}", at % 16 ? "" : fmt::format(" +{:03X}:", at), base[rec + at]);
          REXLOG_INFO("[svr2011] match: test - roster record {} at {:08X}:{}", id, rec, hex);
        }
        for (uint32_t at = 0x22; at < 0x22 + 64 && base[rec + at] >= 0x20 && base[rec + at] < 0x7F; ++at)
          name += char(base[rec + at]);
      }
    }
    ctx.r3.u64 = id;
    sub_8257C190(ctx, base);
    sub_82573EC8(ctx, base);
    const uint32_t info = ctx.r3.u32;
    people += fmt::format("{}{} (person {}, team {})", people.empty() ? "" : ", ", name.empty() ? "?" : name, id,
                          info ? base[info + 13] : 255);
  }
  ctx = saved;
  std::string line = fmt::format("rule {:02X}, arena {} - {}", rule, Rd32(base + kLive + 64), people);
  static std::string last;  // (placed three times as a match loads: logged once)
  if (line == last) return;
  last = line;
  REXLOG_INFO("[svr2011] match: {}", line);
}

REX_EXTERN(__imp__sub_822AD738);
REX_HOOK_RAW(sub_822AD738) {
  const uint32_t placer = ctx.r3.u32;
  svr2011::SetMatchLoading(false);  // (its loading screen is up)
  LogMatch(ctx, base, ctx.r4.u32);
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

// Test aid: SVR2011_TEST_PEOPLE=1 - logs sub_828B5E28 (a person record
// built: people, index, ...) with its caller, for the Lumberjack picks work.
REX_EXTERN(__imp__sub_828B5E28);
REX_HOOK_RAW(sub_828B5E28) {
  static const bool on = std::getenv("SVR2011_TEST_PEOPLE") != nullptr;
  if (on)
    REXLOG_INFO("[svr2011] people: sub_828B5E28(r3 {:08X} r4 {} r5 {:08X} r6 {:08X}) from {:08X}", ctx.r3.u32, ctx.r4.u32,
                ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr));
  __imp__sub_828B5E28(ctx, base);
}

// The per-fighter controls (sub_82216C58(control), once a frame each: +32
// the fighter, +36 its input object), remembered per fighter for the
// lumberjacks' controller.
namespace {
struct Control {
  uint32_t fighter = 0, input = 0;
};
Control g_controls[8];
// Attacks the controller asks for: a lumberjack fighter and his wrestler
// (started from the control hook, which has a context).
struct Attack {
  uint32_t fighter = 0, wrestler = 0, at_frame = 0;
};
Attack g_attacks[6];
// Their attacks: paired grapples (sub_82191AA8(fighter, opponent, motion)
// puts both into the same motion, so only paired ones work - a strike, e.g.
// 3128, would make the wrestler throw it too). Motion ids, not misc.pac move
// ids (those froze both). Seen in CPU fights: 120 a front grapple, 8030 and
// 10901 throws that leave him down (6801 -> 50, 449). 1000 is only lying down.
constexpr uint32_t kAttackMotions[] = {120, 8030, 10901};
}  // namespace

REX_EXTERN(__imp__sub_82216C58);
REX_HOOK_RAW(sub_82216C58) {
  const uint32_t control = ctx.r3.u32, fighter = Rd32(base + control + 32), input = Rd32(base + control + 36);
  for (Control& c : g_controls)
    if (c.fighter == fighter || !c.fighter) {
      c.fighter = fighter, c.input = input;
      break;
    }
  __imp__sub_82216C58(ctx, base);
  // Test aid: SVR2011_TEST_MOTION_LOG=1 - each fighter's motion (+212) when
  // it changes, with the person and the match frame.
  static const bool motion_log = std::getenv("SVR2011_TEST_MOTION_LOG") != nullptr;
  if (motion_log && fighter) {
    static uint32_t last_motion[16] = {};
    const uint32_t person = std::min<uint32_t>(Rd32(base + fighter + 1156), 15), m = Rd32(base + fighter + 212);
    if (m != last_motion[person]) {
      last_motion[person] = m;
      REXLOG_INFO("motion: person {} -> {} (frame {}, state {})", person, m, Rd32(base + 0x82E3CD0C), base[fighter + 446]);
    }
  }
  for (Attack& a : g_attacks)
    if (a.fighter == fighter && a.wrestler) {
      // (test aid: SVR2011_TEST_LJ_MOTION=<id>[,<id>...] - each attack the next)
      static const std::vector<uint32_t> motions = [] {
        std::vector<uint32_t> m;
        if (const char* v = std::getenv("SVR2011_TEST_LJ_MOTION"))
          for (const char* p = v; *p;) {
            char* end = nullptr;
            const unsigned long id = std::strtoul(p, &end, 10);
            if (end == p) break;
            m.push_back(uint32_t(id));
            for (p = end; *p == ',' || *p == ' ';) ++p;
          }
        if (m.empty()) m.assign(std::begin(kAttackMotions), std::end(kAttackMotions));
        return m;
      }();
      static size_t next = 0;
      const uint32_t motion = motions[next++ % motions.size()];
      const auto saved = ctx;
      const uint32_t before = Rd32(base + fighter + 212), wbefore = Rd32(base + a.wrestler + 212);
      ctx.r3.u64 = fighter;
      ctx.r4.u64 = a.wrestler;
      ctx.r5.u64 = motion;
      sub_82191AA8(ctx, base);
      REXLOG_INFO("match types: lumberjack attack - motion {} (lumberjack {} -> {}, wrestler {} -> {})", motion, before,
                  Rd32(base + fighter + 212), wbefore, Rd32(base + a.wrestler + 212));
      ctx = saved;
      a.wrestler = 0;
    }
  // Test aid: SVR2011_TEST_FORCE_MOVE=<attacker person>,<target person>,<every s>,<motion>[,<motion>...]
  // - in a match, every <s> seconds the attacker starts the next motion (a
  // paired move, sub_82191AA8) on the target, wherever they are: for seeing
  // ported moves without waiting for the AI to pick them.
  static const std::vector<uint32_t> force = [] {
    std::vector<uint32_t> v;
    if (const char* e = std::getenv("SVR2011_TEST_FORCE_MOVE"))
      for (const char* p = e; *p;) {
        char* end = nullptr;
        const unsigned long n = std::strtoul(p, &end, 10);
        if (end == p) break;
        v.push_back(uint32_t(n));
        for (p = end; *p == ',' || *p == ' ';) ++p;
      }
    return v.size() >= 4 ? v : std::vector<uint32_t>{};
  }();
  if (!force.empty() && force[0] < 6 && force[1] < 6) {
    constexpr uint32_t kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C;
    const uint32_t att = Rd32(base + kChars + force[0] * 4), tgt = Rd32(base + kChars + force[1] * 4);
    const uint32_t frames = Rd32(base + kMatchFrames), every = std::max<uint32_t>(force[2], 1) * 60;
    static uint32_t last = 0, next = 0;
    if (att == fighter && tgt && frames > 300 && frames >= last + every) {
      last = frames;
      const uint32_t motion = force[3 + next++ % (force.size() - 3)];
      const auto saved = ctx;
      ctx.r3.u64 = att;
      ctx.r4.u64 = tgt;
      ctx.r5.u64 = motion;
      sub_82191AA8(ctx, base);
      REXLOG_INFO("match types: test - person {} starts motion {} on person {} ({} -> state {})", force[0], motion,
                  force[1], frames, Rd32(base + att + 212));
      ctx = saved;
    }
  }
}

// -- The lumberjacks' controller ---------------------------------------------
//
// The lumberjacks are managers (kind 2): they keep to their posts on the
// floor and their AI never fights (neither their role +2624 nor the AI's mark
// +168 changes that during a match). So the port makes them act: each world
// update, a wrestler on the floor (y about 0, outside the ring's +-30 - in
// the ring y is -12) for half a second gets the nearest lumberjacks within
// reach (two at most) - each grapples him once (kAttackMotions in turn,
// started from the fighter control hook above) - and back in the ring he is
// left alone until his next trip out. Their hits would disqualify them (lost
// 7, the match over): LumberjackBeforeJudge takes that back.
namespace {

constexpr float kRingHalf = 30.0f, kReach = 20.0f;

struct Pos {
  float x, y, z;
};
Pos PosOf(const uint8_t* base, uint32_t ch) { return {RdF(base + ch + 288), RdF(base + ch + 292), RdF(base + ch + 296)}; }
bool InRing(const Pos& p) { return p.y < -6.0f && std::fabs(p.x) < kRingHalf && std::fabs(p.z) < kRingHalf; }
bool OnFloor(const Pos& p) { return p.y > -6.0f && (std::fabs(p.x) > kRingHalf || std::fabs(p.z) > kRingHalf); }

void LumberjackController(uint8_t* base) {
  constexpr uint32_t kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C;
  static uint32_t last_frames = 0;
  static int floor_time[6] = {};        // (updates a wrestler has been on the floor (+) / in the ring (-))
  static bool attacked[6][6] = {};      // (lumberjack i has had wrestler w this trip out)
  static uint32_t busy_until[6] = {};   // (a lumberjack's last grab: no other for 3 s)
  const uint32_t frames = Rd32(base + kMatchFrames);
  const bool running = frames != last_frames && frames > 60;
  if (running && last_frames <= 60) {  // (once a match: its live rules - byte 5 the count out, 0x80 none)
    std::string live;
    for (uint32_t i = 0; i < 16; ++i) live += fmt::format(" {:02X}", base[0x82E3DE00 + i]);
    REXLOG_INFO("match types: Lumberjack live rules{}", live);
  }
  last_frames = frames;
  if (!running) {
    for (int& f : floor_time) f = 0;
    for (auto& row : attacked) for (bool& a : row) a = false;
    for (uint32_t& b : busy_until) b = 0;
    for (Attack& a : g_attacks) a = Attack{};
    return;
  }
  uint32_t ch[6];
  Pos pos[6];
  for (uint32_t i = 0; i < 6; ++i) {
    ch[i] = Rd32(base + kChars + i * 4);
    if (ch[i]) pos[i] = PosOf(base, ch[i]);
  }
  // The lumberjacks: people 2-5 (+1156), whatever their character slot.
  auto is_lumberjack = [&](uint32_t i) { return ch[i] && Rd32(base + ch[i] + 1156) >= 2; };
  // Test aid: SVR2011_TEST_LJ_FORCE=<s> - every <s> seconds the next
  // lumberjack attacks person 0 wherever they are (for trying attack moves).
  static const int force = [] { const char* v = std::getenv("SVR2011_TEST_LJ_FORCE"); return v ? std::atoi(v) : 0; }();
  if (force > 0 && frames % uint32_t(force * 60) == 0 && ch[0]) {
    static uint32_t who = 2;
    for (uint32_t k = 0; k < 6; ++k, who = 2 + (who - 1) % 4)
      if (is_lumberjack(who)) {
        g_attacks[who] = Attack{ch[who], ch[0], frames};
        REXLOG_INFO("match types: test - lumberjack {} attacks person 0", who);
        who = 2 + (who - 1) % 4;
        break;
      }
  }
  for (uint32_t w = 0; w < 6; ++w) {
    if (!ch[w] || is_lumberjack(w)) continue;
    if (OnFloor(pos[w])) floor_time[w] = std::max(floor_time[w], 0) + 1;
    else if (InRing(pos[w])) floor_time[w] = std::min(floor_time[w], 0) - 1;
    if (floor_time[w] <= -30)  // (back in the ring: a new trip out may be punished again)
      for (uint32_t i = 0; i < 6; ++i) attacked[i][w] = false;
    if (floor_time[w] < 30) continue;
    int going = 0;
    for (uint32_t i = 0; i < 6; ++i) going += attacked[i][w];
    while (going < 2) {  // (the nearest lumberjacks within reach who haven't had him yet)
      int best = -1;
      float best_d = kReach;
      for (uint32_t i = 0; i < 6; ++i)
        if (is_lumberjack(i) && !attacked[i][w] && frames >= busy_until[i] && !InRing(pos[i])) {
          const float d = std::hypot(pos[i].x - pos[w].x, pos[i].z - pos[w].z);
          if (d < best_d) best = int(i), best_d = d;
        }
      if (best < 0) break;
      attacked[best][w] = true;
      busy_until[best] = frames + 180;
      g_attacks[best] = Attack{ch[best], ch[w], frames};
      REXLOG_INFO("match types: lumberjack {} goes for person {}", Rd32(base + ch[best] + 1156), Rd32(base + ch[w] + 1156));
      ++going;
    }
  }
}

}  // namespace

namespace svr2011 {

bool SlobberKnockerMatch() { return g_slobber; }

// A lumberjack (a manager, team 3) who hits a wrestler gets the lost byte
// (+447) 7 - disqualified for interference - and the judge then ends the
// match ("WINS BY WAY OF DQ"). Their attacks are the point here: taken back.
void LumberjackBeforeJudge(uint8_t* base) {
  if (base[0x82E3DE00] != kLumberjack || Rd32(base + kStoryContext) || !MatchTypeOn("lumberjack")) return;
  constexpr uint32_t kChars = 0x82E3CC50;
  for (uint32_t i = 0; i < 6; ++i) {
    const uint32_t c = Rd32(base + kChars + i * 4);
    if (!c || Rd32(base + c + 1156) < 2 || !base[c + 447]) continue;
    static int count = 0;
    if (++count <= 20)
      REXLOG_INFO("match types: lumberjack {} disqualified (lost {}) - taken back", Rd32(base + c + 1156), base[c + 447]);
    base[c + 447] = 0;
    base[c + 448] = 0;
  }
}

void MatchTypesUpdate(PPCContext& ctx, uint8_t* base) {
  constexpr uint32_t kChars = 0x82E3CC50;
  // Research aid: SVR2011_TEST_ITEM_DUMP=<file> - every 30 updates of a match,
  // the object table (0x82DE0220, 72 slots: people and weapons) appended:
  // u32 match frames, then per slot u32 object and its first 0xC00 bytes.
  if (static const char* dump = std::getenv("SVR2011_TEST_ITEM_DUMP"); dump && Rd32(base + 0x82E3CD0C) > 0) {
    static uint32_t n = 0;
    if (++n % 30 == 0)
      if (FILE* f = std::fopen(dump, "ab")) {
        const uint32_t frames = Rd32(base + 0x82E3CD0C);
        std::fwrite(&frames, 4, 1, f);
        for (uint32_t i = 0; i < 72; ++i) {
          const uint32_t o = Rd32(base + 0x82DE0220 + i * 4);
          std::fwrite(&o, 4, 1, f);
          if (o) std::fwrite(base + o, 1, 0xC00, f);
        }
        std::fclose(f);
      }
  }
  // Test aid: SVR2011_TEST_ITEM_LOG=1 - each weapon picked up (its holder,
  // +44, from none to a person): its slot, model, where it lay, and whether it
  // came from under the ring (hidden at 0,-12,0 until then).
  if (static const bool item_log = std::getenv("SVR2011_TEST_ITEM_LOG") != nullptr; item_log) {
    // (kind per slot: 0 unknown, 1 lying at the bell (placed), 2 hidden (under the ring), 3 out from under it)
    static uint32_t holder[72], kind[72], last_frames = 0;
    static float last_pos[72][3];
    const uint32_t frames = Rd32(base + 0x82E3CD0C);
    if (frames < last_frames)
      for (uint32_t i = 0; i < 72; ++i) holder[i] = 0xFF, kind[i] = 0;
    last_frames = frames;
    for (uint32_t i = 0; i < 72; ++i) {
      const uint32_t o = Rd32(base + 0x82DE0220 + i * 4);
      if (!o) continue;
      const uint32_t h = Rd32(base + o + 44) & 0xFF;
      const uint32_t model = Rd32(base + o + 80);
      if (model >= 1000) continue;  // (not a weapon)
      const float x = RdF(base + o + 288), y = RdF(base + o + 292), z = RdF(base + o + 296);
      const bool hidden_now = x == 0.f && z == 0.f && y < -11.f;
      if (kind[i] == 0 && frames >= 120 && frames < 600 && h == 0xFF) kind[i] = hidden_now ? 2 : 1;
      if (kind[i] == 2 && !hidden_now && last_pos[i][1] < -11.f && last_pos[i][0] == 0.f) {
        std::string who;  // (out from under the ring: what the people are doing)
        for (uint32_t c = 0; c < 6; ++c)
          if (const uint32_t p = Rd32(base + kChars + c * 4))
            who += fmt::format(" [{}: motion {} at ({:.0f},{:.0f})]", Rd32(base + p + 1156), Rd32(base + p + 212),
                               RdF(base + p + 288), RdF(base + p + 296));
        REXLOG_INFO("item: slot {} (model {}) out from under the ring at ({:.0f},{:.0f},{:.0f}), frame {};{}", i, model,
                    x, y, z, frames, who);
      }
      if (h != 0xFF && holder[i] == 0xFF && kind[i] != 0) {
        static const char* kKinds[] = {"?", "placed", "from under the ring", "again"};
        REXLOG_INFO("item: person {} picks up slot {} (model {}) {} at ({:.0f},{:.0f},{:.0f}), frame {}", h, i, model,
                    kKinds[kind[i]], last_pos[i][0], last_pos[i][1], last_pos[i][2], frames);
        if (kind[i] == 2) kind[i] = 3;
      }
      holder[i] = h;
      if (h == 0xFF)
        for (int k = 0; k < 3; ++k) last_pos[i][k] = RdF(base + o + 288 + k * 4);
    }
  }
  if (g_slobber && base[0x82E3DE00] == kGauntlet) SlobberKnockerUpdate(ctx, base);
  if (ThreeStagesMatch()) ThreeStagesUpdate(base);
  if (base[0x82E3DE00] != kLumberjack || Rd32(base + kStoryContext) || !MatchTypeOn("lumberjack")) return;
  LumberjackController(base);
  static const bool probe = std::getenv("SVR2011_TEST_PEOPLE") != nullptr;
  static uint32_t frames = 0;
  if (probe && ++frames % 60 == 0) {
    std::string all;
    for (uint32_t i = 0; i < 6; ++i)
      if (const uint32_t c = Rd32(base + kChars + i * 4)) {
        all += fmt::format(" {}:{:08X} ({:.0f},{:.0f},{:.0f}) area {} role {} +2348 {} +464 {} +476 {:08X}", i, c,
                           RdF(base + c + 288), RdF(base + c + 292), RdF(base + c + 296), base[c + 444],
                           Rd32(base + c + 2624), Rd32(base + c + 2348), base[c + 464], Rd32(base + c + 476));
      }
    REXLOG_INFO("[svr2011] people: roles{}", all);
    if (const uint32_t c2 = Rd32(base + kChars + 8)) {  // (character 2's position object +304 and body +112)
      std::string f;
      const uint32_t po = Rd32(base + c2 + 304), body = Rd32(base + c2 + 112);
      for (uint32_t at = 0; po && at < 96; at += 4) f += fmt::format(" {:.1f}", RdF(base + po + at));
      f += " | body";
      for (uint32_t at = 32; body && at < 96; at += 4) f += fmt::format(" {:.1f}", RdF(base + body + at));
      REXLOG_INFO("[svr2011] people: 2 at ({:.1f},{:.1f},{:.1f}) pos object {:08X}:{}", RdF(base + c2 + 288),
                  RdF(base + c2 + 292), RdF(base + c2 + 296), po, f);
    }
    const uint32_t c0 = Rd32(base + kChars);
    for (const Control& c : g_controls)
      if (c.fighter == c0 && c.input) {
        std::string hex;
        for (uint32_t at = 0; at < 128; ++at) hex += fmt::format("{}{:02X}", at % 16 ? "" : " ", base[c.input + at]);
        REXLOG_INFO("[svr2011] people: input of 0 at {:08X}:{}", c.input, hex);
      }
  }
}

}  // namespace svr2011

// -- Weapons everywhere ------------------------------------------------------
//
// The weapons lying around at the bell come from a table (bgEtc.pac
// STG/WPON 45010, kept at *(0x82E3BE98) once loaded): per rule id a list of
// 28-byte records (f32 position x/y/z, f32 rotation x/y/z in degrees, s16
// motion, u8 place: 0 ring / 1 floor, u8 model), plus a shared ringside set
// (announce tables, steps). sub_8227B938 makes the weapons as the match loads
// and sub_8227D130 puts them in place at the start. Extreme Rules (0x4D-0x50)
// has no records but loads the Extreme Rules weapons' models (live option
// +56: table 5, ladder 11, trash can 63, guitar 89, crutch 60, ... and the
// chair 4 of every match). For a WEAPONS EVERYWHERE match its list is ours
// while those two run; any other match gets the game's back.
namespace {

constexpr uint32_t kWeaponTable = 0x82E3BE98;
struct Placed {
  uint8_t model, place;
  float x, y, z, yaw;
};
// In the ring y = -12, on the floor 0 (a table stands at y -6.7 by the
// apron); the ring is about +-30, the announce tables at z -76.
constexpr Placed kPlaced[] = {
    {4, 0, 10.f, -12.f, 12.f, 30.f},      // chairs in the ring
    {4, 0, -12.f, -12.f, -8.f, 200.f},
    {5, 1, 0.f, -6.7f, 54.f, 0.f},        // tables by the apron
    {5, 1, -52.f, 0.f, 0.f, 90.f},
    {11, 1, 52.f, 0.f, 0.f, 90.f},        // a ladder
    {63, 1, 40.f, 0.f, 55.f, 0.f},        // trash can
    {89, 1, -40.f, 0.f, 55.f, 30.f},      // guitar
    {4, 1, 45.f, 0.f, -45.f, 60.f},       // a chair on the floor
};
uint32_t g_weapon_records = 0;  // guest: our records
struct Swapped {
  uint32_t entry = 0, count = 0, records = 0;  // the game's, while ours are in
} g_swapped;

// The rule's {count, records} in the table (0 if none).
uint32_t WeaponEntry(uint8_t* base, uint32_t rule) {
  const uint32_t table = Rd32(base + kWeaponTable);
  if (!table) return 0;
  const int16_t n = int16_t(uint16_t(base[table + 4] << 8 | base[table + 5]));
  const uint32_t heads = Rd32(base + table + 8), entries = Rd32(base + table + 12);
  for (int i = 0; i < n && heads && entries; ++i) {
    const uint32_t h = heads + uint32_t(i) * 8;
    if (uint32_t(base[h] << 8 | base[h + 1]) == rule) return entries + uint32_t(i) * 8;
  }
  return 0;
}

void RestoreWeapons(uint8_t* base) {
  if (!g_swapped.entry) return;
  Wr32(base + g_swapped.entry, g_swapped.count);
  Wr32(base + g_swapped.entry + 4, g_swapped.records);
  g_swapped = {};
}

// Our list in (a WEAPONS EVERYWHERE match), or the game's back.
void ApplyWeapons(uint8_t* base) {
  RestoreWeapons(base);
  const uint32_t rule = base[0x82E3DE00];
  if (!g_weapons || rule < kExtremeFirst || rule > kExtremeLast || !g_memory) return;
  const uint32_t entry = WeaponEntry(base, rule);
  if (!entry) {
    REXLOG_WARN("match types: no weapon list for rule {:02X}", rule);
    return;
  }
  constexpr uint32_t n = uint32_t(sizeof(kPlaced) / sizeof(kPlaced[0]));
  if (!g_weapon_records) {
    g_weapon_records = g_memory->SystemHeapAlloc(n * 28);
    if (!g_weapon_records) return;
    for (uint32_t i = 0; i < n; ++i) {
      uint8_t* r = base + g_weapon_records + i * 28;
      const Placed& w = kPlaced[i];
      WrF(r, w.x), WrF(r + 4, w.y), WrF(r + 8, w.z);
      WrF(r + 12, 0.f), WrF(r + 16, w.yaw), WrF(r + 20, 0.f);
      r[24] = uint8_t(15010 >> 8), r[25] = uint8_t(15010 & 0xFF);  // (motion: lying)
      r[26] = w.place;
      r[27] = w.model;
    }
  }
  g_swapped = {entry, Rd32(base + entry), Rd32(base + entry + 4)};
  Wr32(base + entry, n);
  Wr32(base + entry + 4, g_weapon_records);
}

}  // namespace

// The match's weapons are made (as it loads): sub_8227B938.
REX_EXTERN(__imp__sub_8227B938);
REX_HOOK_RAW(sub_8227B938) {
  ApplyWeapons(base);
  if (g_swapped.entry) REXLOG_INFO("match types: WEAPONS EVERYWHERE - the weapons are made");
  __imp__sub_8227B938(ctx, base);
  RestoreWeapons(base);
}

// ... and put in place at the start: sub_8227D130(weapons).
REX_EXTERN(__imp__sub_8227D130);
REX_HOOK_RAW(sub_8227D130) {
  ApplyWeapons(base);
  __imp__sub_8227D130(ctx, base);
  RestoreWeapons(base);
}

// The CPU and the weapons lying about. Its behaviour script asks conditions
// by id (sub_828578E8(this, id), *this the AI: +0 its person, +400 its
// position): 157 "a usable weapon lies somewhere" (sub_8280DFB0 any) leads to
// its "go get a weapon" actions (300 any, 301-304 / 329 a kind: sub_82837C30
// finds the nearest), 158 "one can come from under the ring" (the stock list
// at *0x82E3C06C not empty, fewer than 4 out) to the search under it (actions
// 140 / 141, sub_82828490: to the apron, motions 15480 / 15481) - and it
// mostly searched, some of it without asking 158: in WEAPONS EVERYWHERE the
// CPU walked past the placed weapons to the apron. Now, while a usable weapon
// lies within kNearWeapon, 158 is false and an action 140 / 141 or 260 (the
// search itself, sub_82828968) being made (the action factories
// sub_82812838(ai, id), sub_828136D0) is a 300 instead: the CPU goes for that
// weapon; under the ring only when nothing usable is near.
// (test aid: SVR2011_TEST_WE_AI_OFF=1 - as the game)
namespace {
constexpr float kNearWeapon = 90.f;  // (the ring is about +-30, the placed ones at most ~75 from its middle)

// The nearest usable weapon lying within kNearWeapon of the AI (its slot), or -1.
int32_t NearWeapon(PPCContext& ctx, uint8_t* base, uint32_t ai) {
  static const bool ai_off = std::getenv("SVR2011_TEST_WE_AI_OFF") != nullptr;
  if (!g_weapons || ai_off || !ai || base[0x82E3DE00] < kExtremeFirst || base[0x82E3DE00] > kExtremeLast) return -1;
  const uint32_t person = Rd32(base + ai);
  if (!person) return -1;
  PPCContext c = ctx;
  c.r3.u64 = ai, c.r4.u64 = uint64_t(-1), c.r5.u64 = uint64_t(-1);
  sub_8280DFB0(c, base);
  const int32_t slot = int32_t(c.r3.u32);
  const uint32_t item = slot >= 0 && slot < 72 ? Rd32(base + 0x82DE0220 + uint32_t(slot) * 4) : 0;
  if (!item) return -1;
  const float dx = RdF(base + item + 288) - RdF(base + ai + 400), dz = RdF(base + item + 296) - RdF(base + ai + 408);
  const float d = std::sqrt(dx * dx + dz * dz);
  if (d >= kNearWeapon) return -1;
  static const bool item_log = std::getenv("SVR2011_TEST_ITEM_LOG") != nullptr;
  static uint32_t last_logged = 0;
  const uint32_t frames = Rd32(base + 0x82E3CD0C);
  if (item_log && frames - last_logged > 60) {
    last_logged = frames;
    REXLOG_INFO("item: person {} - not under the ring: slot {} (model {}) lies {:.0f} away, frame {}",
                Rd32(base + person + 1156), slot, Rd32(base + item + 80), d, frames);
  }
  return slot;
}
}  // namespace

REX_EXTERN(__imp__sub_828578E8);
REX_HOOK_RAW(sub_828578E8) {
  if (ctx.r4.u32 == 158 && ctx.r3.u32 && NearWeapon(ctx, base, Rd32(base + ctx.r3.u32)) >= 0) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_828578E8(ctx, base);
}

REX_EXTERN(__imp__sub_82812838);
REX_HOOK_RAW(sub_82812838) {
  const uint32_t id = ctx.r4.u32;
  if ((id == 140 || id == 141 || id == 260) && NearWeapon(ctx, base, ctx.r3.u32) >= 0) ctx.r4.u64 = 300;
  __imp__sub_82812838(ctx, base);
}

// ... and its sibling for actions 146-346: 260, the search itself (sub_82828968).
REX_EXTERN(__imp__sub_828136D0);
REX_HOOK_RAW(sub_828136D0) {
  if (ctx.r4.u32 == 260 && NearWeapon(ctx, base, ctx.r3.u32) >= 0) {
    ctx.r4.u64 = 300;
    sub_82812838(ctx, base);  // (300: the first factory's)
    return;
  }
  __imp__sub_828136D0(ctx, base);
}

// Research aid (with SVR2011_TEST_ITEM_LOG): the CPU's "go get a weapon"
// action starts (sub_82837C30(action): +84 the kind wanted - 301 one in a
// hand, 302 a ladder, 303 a table, 304 a chair, 329 model 1, else any; +52
// the person; +80 the weapon found, -1 none).
REX_EXTERN(__imp__sub_82837C30);
REX_HOOK_RAW(sub_82837C30) {
  const uint32_t action = ctx.r3.u32;
  __imp__sub_82837C30(ctx, base);
  static const bool item_log = std::getenv("SVR2011_TEST_ITEM_LOG") != nullptr;
  if (item_log) {
    const uint32_t ai = Rd32(base + action + 52), person = ai ? Rd32(base + ai) : 0;  // (the AI: +0 its person)
    REXLOG_INFO("item: person {} goes for a weapon - kind {}, found slot {}, frame {} (area {})",
                person ? Rd32(base + person + 1156) : 99, Rd32(base + action + 84), int32_t(Rd32(base + action + 80)),
                Rd32(base + 0x82E3CD0C), ai ? Rd32(base + ai + 208) : 99);
  }
}

namespace svr2011 {

// Menu text of the rows added here (menu_hooks.cpp's string lookup), else 0.
uint32_t MatchTypeString(uint32_t id) {
  if (const uint32_t t = MysteryOpponentString(id, g_memory)) return t;
  if ((id == kStagesLabel || id == kStagesText) && g_memory) {
    static const char* const kText[] = {
        "THREE STAGES OF HELL",
        "The first to win two falls: a normal fall, then Falls Count Anywhere, then Last Man Standing - "
        "the damage carries over."};
    const uint32_t k = id - kStagesLabel;
    if (!g_stages_text[k]) {
      const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
      g_stages_text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_stages_text[k]), kText[k], n);
    }
    return g_stages_text[k];
  }
  if ((id == kElimLabel || id == kElimText) && g_memory) {
    static const char* const kText[] = {
        "ELIMINATION",
        "A pin or a give up eliminates that Superstar, and the match goes on until only one is left."};
    const uint32_t k = id - kElimLabel;
    if (!g_elim_text[k]) {
      const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
      g_elim_text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_elim_text[k]), kText[k], n);
    }
    return g_elim_text[k];
  }
  if ((id == kSlobberLabel || id == kSlobberText) && g_memory) {
    static const char* const kText[] = {
        "SLOBBER KNOCKER",
        "One on one against an endless line of opponents. Beat one and the next comes in - how many can "
        "you beat before you're pinned?"};
    const uint32_t k = id - kSlobberLabel;
    if (!g_slobber_text[k]) {
      const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
      g_slobber_text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_slobber_text[k]), kText[k], n);
    }
    return g_slobber_text[k];
  }
  if ((id == kWeaponsLabel || id == kWeaponsText) && g_memory) {
    static const char* const kText[] = {
        "WEAPONS EVERYWHERE",
        "Extreme Rules with chairs, tables, a ladder and more already waiting in and around the ring at "
        "the bell."};
    const uint32_t k = id - kWeaponsLabel;
    if (!g_weapons_text[k]) {
      const uint32_t n = uint32_t(std::strlen(kText[k]) + 1);
      g_weapons_text[k] = g_memory->SystemHeapAlloc(n);
      std::memcpy(g_memory->TranslateVirtual<char*>(g_weapons_text[k]), kText[k], n);
    }
    return g_weapons_text[k];
  }
  if (id < kOwnRowLabel || id >= kOwnRowLabel + 8 || !g_memory) return 0;
  const auto& own = BackstageRows();
  const size_t k = id - kOwnRowLabel;
  if (k >= own.size()) return 0;
  if (g_own_row_text.size() < own.size()) g_own_row_text.resize(own.size(), 0);
  if (!g_own_row_text[k]) {
    std::string t = own[k].label;
    for (char& c : t) c = char(std::toupper(static_cast<unsigned char>(c)));
    const uint32_t at = g_memory->SystemHeapAlloc(uint32_t(t.size() + 1));
    std::memcpy(g_memory->TranslateVirtual<char*>(at), t.c_str(), t.size() + 1);
    g_own_row_text[k] = at;
  }
  return g_own_row_text[k];
}

}  // namespace svr2011

// -- Fewer locked rules ------------------------------------------------------
//
// Each rule's 64-byte option record (*(0x82EDE954) + 11900 + id * 64, copied
// from misc.pac by sub_828B5078) has a byte per option; bit 7 locks it: the
// rules screen greys the row (sub_82470BC0) and the match uses the record's
// value, not the player's (sub_828C48E8). Unlocked here, for the matches
// they can't break: K.O. (+2), ROPE BREAK (+3) and GIVE UP (+7) - extra ways
// to win or a rope break, which every one on one / tag / multi-person match
// has - and DQ (+4) where it is locked on (tag, handicap: No DQ can be
// chosen). Left locked: Royal Rumbles (eliminations over the top only),
// Elimination Chamber, backstage and story rules, Lumberjack (its rules are
// the match), count-outs in matches of more than 2 (the count logic assumes
// two), over the top rope (needs the battle royal rule swap), entrances.
namespace {

bool KeepLocked(uint32_t rule) {
  return (rule >= 0x14 && rule <= 0x18) || rule == 0x56 ||  // Royal Rumble
         rule == 0x27 || rule == 0x28 ||                    // Elimination Chamber
         (rule >= 0x19 && rule <= 0x21) || (rule >= 0x61 && rule <= 0x76) ||  // backstage, in-ring brawls
         rule == 0x54 || rule == 0x55 || rule == 0x57 || rule == 0x59 || rule == 0x5A || rule == 0x5C ||
         rule == 0x5D;  // story rules, Lumberjack
}

}  // namespace

REX_EXTERN(__imp__sub_828B5078);
REX_HOOK_RAW(sub_828B5078) {
  const uint32_t rules = ctx.r3.u32;
  __imp__sub_828B5078(ctx, base);
  constexpr uint32_t kCount = 119, kKo = 2, kRopeBreak = 3, kDq = 4, kGiveUp = 7;
  int unlocked = 0;
  for (uint32_t rule = 0; rule < kCount; ++rule) {
    if (KeepLocked(rule)) continue;
    uint8_t* opt = base + rules + kRule2 + rule * kRule2Size;
    for (uint32_t at : {kKo, kRopeBreak, kGiveUp})
      if (opt[at] & 0x80) opt[at] &= 0x7F, ++unlocked;
    if (opt[kDq] == 0x80) opt[kDq] = 0x00, ++unlocked;  // (locked on: No DQ can be chosen)
  }
  REXLOG_INFO("match types: {} locked match rules unlocked", unlocked);
}

// -- MATCH CREATOR: everything allowed in every match ------------------------
//
// The MATCH CREATOR's 3 pages (ENVIRONMENT, WIN CONDITION, RULES) grey rows
// from misc.pac /MRME/MRPD, loaded by sub_82490558(loader): 10 tables at
// loader +48, +52, +64, +68, +72, +76, +80, +84, +88, +92 (chunks 1-10).
// - Per match family (sub_82490918(rule): 68 families, table 0x8201E4F8 -
//   12 bytes each: a u32 rule list, its count) and row: the defaults (1, 3,
//   7) and what is allowed (2 ENVIRONMENT, 8 rows: ring STANDARD / CAGE /
//   HELL IN A CELL / CHAMBER / INFERNO, entrance, replay, momentum; 4 WIN
//   CONDITION, 11 rows: pin and give up, 2 out of 3, ironman, over the top,
//   K.O., last man standing, finisher, first blood, flaming table, climb out,
//   escape; 8 RULES, 6 rows: DQ, rope break, ring out, elimination, falls
//   count anywhere, time limit): 0x09 free, 0x00 greyed, 0x02-0x04 fixed.
// - Combinations: 5 (ring structure x win rows), 6 (a win row's value x the
//   others: 0x0A itself), 9 and 10 (the same for the rules).
// Every greyed (0x00) cell is allowed here - fixed ones stay - but for the
// families kept as they are (KeepFamily: Royal Rumble, Elimination Chamber,
// backstage, in-ring brawls). A row freed here was an option locked off in
// the rule's 64-byte record (bit 7, value 0): those are opened while a
// match's live rules are built (sub_828C48E8 below), so the choice counts.
namespace {

bool KeepFamily(uint32_t f) {
  return f == 12 || f == 26 || f == 55 || f == 60 || f == 48 || f == 49 || f == 61 || (f >= 62 && f <= 67) ||
         f == 1;  // (1: INFERNO 1 on 1 - with LAST MAN STANDING it crashed)
}

// What stays greyed (the sweep, tools/mc_sweep.ps1 - docs/MATCH_TYPES_RESEARCH.md):
// - ladder / TLC families in a cage, cell or inferno ring (the CPU stood idle),
//   and their win conditions as they are (fatal-4-way ladder with LAST MAN
//   STANDING hung in the characters' job);
bool LadderFamily(uint32_t f) { return f == 4 || f == 5 || f == 18 || f == 19 || f == 30 || f == 31 || f == 39 || f == 40 || f == 50; }
// - tag and team families with OVER THE TOP ROPE (crashed at the start);
bool TeamFamily(uint32_t f) { return (f >= 14 && f <= 27) || (f >= 45 && f <= 47) || f == 52; }
// - the INFERNO ring but where it already was and in 1 on 1 / triple threat /
//   fatal-4-way normal (elsewhere the CPU stood idle or, in a tag match,
//   crashed); the CHAMBER ring where it already was (not tested).
bool KeepEnvironment(uint32_t f, uint32_t c) {
  if (c == 3) return true;
  if (c == 4) return !(f == 0 || f == 28 || f == 36);
  return (c == 1 || c == 2) && LadderFamily(f);
}

// A rule's MATCH CREATOR family (sub_82490918; -1: none).
int FamilyOf(const uint8_t* base, uint32_t rule) {
  constexpr uint32_t kFamilyTable = 0x8201E4F8;
  for (uint32_t f = 0; f < 68; ++f) {
    const uint32_t list = Rd32(base + kFamilyTable + f * 12), n = Rd32(base + kFamilyTable + f * 12 + 4);
    for (uint32_t k = 0; list && k < n && k < 16; ++k)
      if (Rd32(base + list + k * 4) == rule) return int(f);
  }
  return -1;
}

// The option record bytes the MATCH CREATOR's rows set (sub_828C48E8, from
// the saved record: +0 ring, +4 pin, +5/+6 falls / ironman, +7 over the top,
// +8 K.O., +9 last man standing, +10 finisher, +11 first blood, +12 flaming
// table, +13 climb out, +14 escape, +15 ring out, +16 rope break, +17 DQ /
// count out, +18 elimination, +19 falls count anywhere, +20 time limit).
constexpr uint8_t kMcBytes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 20, 21, 22, 23, 25, 26,
                                27, 28, 29, 32, 37, 38, 39, 40, 41, 44, 46, 59, 63};

}  // namespace

REX_EXTERN(__imp__sub_82490558);
REX_HOOK_RAW(sub_82490558) {
  const uint32_t loader = ctx.r3.u32;
  __imp__sub_82490558(ctx, base);
  constexpr uint32_t kFamilies = 68;
  constexpr uint8_t kFree = 0x09;
  // (test aid: SVR2011_TEST_MC_OLD=1 - the tables as the game has them)
  static const bool old = std::getenv("SVR2011_TEST_MC_OLD") != nullptr;
  if (old) return;
  int n = 0;
  // (keep(row, column): a cell left as it is)
  auto free_cells = [&](uint32_t table, uint32_t width, uint32_t rows, bool families, auto keep) {
    if (!table) return;
    for (uint32_t row = 0; row < rows; ++row) {
      if (families && KeepFamily(row)) continue;
      uint8_t* cells = base + table + row * width;
      for (uint32_t c = 0; c < width; ++c)
        if (cells[c] == 0 && !keep(row, c)) cells[c] = kFree, ++n;
    }
  };
  auto none = [](uint32_t, uint32_t) { return false; };
  free_cells(Rd32(base + loader + 52), 8, kFamilies, true, KeepEnvironment);  // ENVIRONMENT
  // WIN CONDITION: FLAMING TABLE (8) stays a table match's own; OVER THE TOP
  // (3) not in team matches.
  free_cells(Rd32(base + loader + 68), 11, kFamilies, true,
             [](uint32_t f, uint32_t c) { return c == 8 || (c == 3 && TeamFamily(f)) || LadderFamily(f); });
  free_cells(Rd32(base + loader + 84), 6, kFamilies, true, none);  // RULES
  // Ring x win: the ring decides FLAMING TABLE, CLIMB OUT and ESCAPE (8-10;
  // the last two a cage's own) and OVER THE TOP (3: not out of a cage, cell
  // or chamber) - left as they are.
  free_cells(Rd32(base + loader + 72), 11, 5, false,
             [](uint32_t ring, uint32_t c) { return c >= 8 || (c == 3 && ring >= 1 && ring <= 3); });
  free_cells(Rd32(base + loader + 76), 11, 24, false, none);  // win x win
  free_cells(Rd32(base + loader + 88), 6, 29, false, none);   // ... x rules
  free_cells(Rd32(base + loader + 92), 6, 19, false, none);   // rules x rules
  static bool logged = false;
  if (!logged) {
    REXLOG_INFO("match types: MATCH CREATOR - {} greyed cells allowed (everything in every match)", n);
    logged = true;
  }
}

// The live rules of a match are built: sub_828C48E8(rule, live, saved) -
// from the rule's option record and the player's MATCH CREATOR record for
// it (saved: 28 bytes, +18 ELIMINATION). For an ELIMINATION row the saved
// record has ELIMINATION on while the live rules are built - the game then
// sets up its elimination as MATCH CREATOR's own does (only the live +37 byte
// was not enough: the first fall ended the match) - and is put back after,
// so the player's MATCH CREATOR settings stay as they were.
REX_EXTERN(__imp__sub_828C48E8);
REX_HOOK_RAW(sub_828C48E8) {
  constexpr uint32_t kSavedElimination = 18;
  const uint32_t rule = ctx.r3.u32, live = ctx.r4.u32, saved = ctx.r5.u32;
  const bool elim = g_elimination && (rule == kTripleThreat || rule == kFatal4Way) && saved;
  const uint8_t was = elim ? base[saved + kSavedElimination] : 0;
  if (elim) base[saved + kSavedElimination] = 1;
  // Test aid: SVR2011_TEST_MC="<saved byte>=<value>,..." - the MATCH CREATOR
  // record of every match set so (e.g. "9=1" last man standing, "0=1" a cage).
  static const std::vector<std::pair<uint32_t, uint8_t>> test_mc = [] {
    std::vector<std::pair<uint32_t, uint8_t>> v;
    if (const char* e = std::getenv("SVR2011_TEST_MC"))
      for (const char* p = e; *p;) {
        char* end = nullptr;
        const unsigned long k = std::strtoul(p, &end, 10);
        if (end == p || *end != '=') break;
        const unsigned long val = std::strtoul(end + 1, &end, 10);
        v.push_back({uint32_t(k), uint8_t(val)});
        for (p = end; *p == ',' || *p == ' ';) ++p;
      }
    return v;
  }();
  uint8_t mc_was[28] = {};
  if (saved && !test_mc.empty()) {
    std::memcpy(mc_was, base + saved, 28);
    std::string set;
    for (const auto& [k, val] : test_mc)
      if (k < 28) base[saved + k] = val, set += fmt::format(" +{}={}", k, val);
    REXLOG_INFO("match types: test - MATCH CREATOR record for rule {:02X}:{}", rule, set);
  }
  // MATCH CREATOR, everything allowed: the options this rule has locked off
  // (bit 7, value 0) are open while its live rules are built.
  static const bool old = std::getenv("SVR2011_TEST_MC_OLD") != nullptr;
  const int family = old || Rd32(base + kStoryContext) ? -1 : FamilyOf(base, rule);
  uint8_t* opt = family >= 0 && !KeepFamily(uint32_t(family)) && Rd32(base + kRules)
                     ? base + Rd32(base + kRules) + kRule2 + rule * kRule2Size
                     : nullptr;
  uint8_t opened[sizeof(kMcBytes)] = {};
  if (opt)
    for (size_t k = 0; k < sizeof(kMcBytes); ++k)
      if (opt[kMcBytes[k]] == 0x80) opt[kMcBytes[k]] = 0x00, opened[k] = 1;
  __imp__sub_828C48E8(ctx, base);
  if (opt)
    for (size_t k = 0; k < sizeof(kMcBytes); ++k)
      if (opened[k]) opt[kMcBytes[k]] = 0x80;
  if (saved && !test_mc.empty()) std::memcpy(base + saved, mc_was, 28);
  if (elim) base[saved + kSavedElimination] = was;
  svr2011::MysteryOpponentLive(base, live);  // (entrances on: mystery_opponent.h)
}
