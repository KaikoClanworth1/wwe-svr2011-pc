// WWE SmackDown vs. Raw 2011 - the debug suite's direct boot (debug_suite.h: dbg_boot).
//
// dbg_boot=match: from launch straight into the match dbg_match asks for. The intros and the practice ring are
// skipped (fast_start.cpp's START pulses, on the Start Screen too), and as soon as the main menu is up the match
// set-up is filled and committed as the exhibition flow's sub_82736020 does after its select screens, and the
// next scene (1, the exhibition match) asked for - the game tears its menus down and loads the match itself
// (sub_82440328 -> the front end's fade -> menu+1864 -> sub_82452DE0 -> sub_82575250 -> sub_82197C40).
//
// The match set-up (*0x82EDEB48): sub_82594990 mode (0 exhibition), sub_828BD278 rule (all 30 slots cleared,
// the profile's match options), sub_828BD1F0 arena, per person sub_828BB290 (id, pad or -1 for a CPU, attire
// 0-3), sub_828B9D70 kind, sub_828B9D80 team, sub_828B9E30 / sub_828B9E40; sub_828BB418 the title on the line;
// game state *0x82E3DDFC +16 = 1; sub_828BC5E8 the commit to the live record; sub_828C3EA8; sub_82574050(live
// +288).
//
// Not done (the select screens' work): the port's match-type extras hung on sub_827374A0 (match_types.cpp:
// runtime rules, MYSTERY OPPONENT ...), CAW people's fighter entries, the exhibition flow object (after the match
// its menus may need it - a dbg run quits before).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "arena_mods.h"
#include "debug_suite/debug_suite.h"
#include "script_input.h"

REXCVAR_DEFINE_STRING(dbg_boot, "", "Debug", "SVRDBG: start straight in: match (dbg_match's)");
REXCVAR_DEFINE_STRING(dbg_match, "", "Debug",
                      "SVRDBG: the match for dbg_boot=match: people=EDGE,KANE attires=1,1 arena=1 rule=0 cpu=all");
REXCVAR_DEFINE_BOOL(dbg_entrances, false, "Debug", "SVRDBG: a booted match plays its entrances");
REXCVAR_DECLARE(bool, skip_intros);
REXCVAR_DECLARE(bool, skip_training);

namespace svr2011::dbg {
namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kSetup = 0x82EDEB48, kMenu = 0x82E3DB68, kGame = 0x82E3DDFC, kLive = 0x82E3DE00;
constexpr uint32_t kIdToIndex = 0x82DB3610, kRecords = 0x82E407C0, kRecordSize = 260, kOwnId = 32, kName = 34;
constexpr uint32_t kSelectable = 221;

uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

struct Match {
  std::vector<std::string> people;
  std::vector<int> attires;  // 1-4
  int arena = 1, rule = 0;
  bool all_cpu = false;
};

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// "people=EDGE,KANE attires=1,1 arena=1 rule=0 cpu=all" (a value runs to the next " key=": names have spaces;
// rule in hex as SVR2011_TEST_RULE)
Match Parse(const std::string& s) {
  Match m;
  size_t at = 0;
  while (at < s.size()) {
    const size_t eq = s.find('=', at);
    if (eq == std::string::npos) break;
    std::string key = s.substr(at, eq - at);
    while (!key.empty() && key.front() == ' ') key.erase(0, 1);
    size_t end = s.size();
    for (size_t next = s.find('=', eq + 1); next != std::string::npos; next = s.find('=', next + 1))
      if (const size_t space = s.rfind(' ', next); space != std::string::npos && space > eq) {
        end = space;
        break;
      }
    const std::string value = s.substr(eq + 1, end - eq - 1);
    std::stringstream list(value);
    std::string item;
    if (key == "people") {
      while (std::getline(list, item, ',') && m.people.size() < 6) {
        while (!item.empty() && item.front() == ' ') item.erase(0, 1);
        while (!item.empty() && item.back() == ' ') item.pop_back();
        if (!item.empty()) m.people.push_back(item);
      }
    } else if (key == "attires") {
      while (std::getline(list, item, ',')) m.attires.push_back(std::clamp(std::atoi(item.c_str()), 1, 4));
    } else if (key == "arena") {
      m.arena = std::atoi(value.c_str());
    } else if (key == "rule") {
      m.rule = int(std::strtol(value.c_str(), nullptr, 16));
    } else if (key == "cpu") {
      m.all_cpu = value == "all";
    } else {
      REXLOG_WARN("SVRDBG: boot: unknown dbg_match setting '{}'", key);
    }
    at = end;
  }
  return m;
}

const uint8_t* Record(const uint8_t* base, uint32_t id) {
  if (id >= 1000) return nullptr;
  const uint32_t index = Rd16(base + kIdToIndex + id * 2);
  if (index >= 1000) return nullptr;
  const uint8_t* rec = base + kRecords + index * kRecordSize;
  return Rd16(rec + kOwnId) == id ? rec : nullptr;
}

// A roster name (as the "match:" lines print it) or id -> the superstar id (0: none). As test_match.cpp.
uint32_t FindSuperstar(const uint8_t* base, const std::string& want) {
  if (!want.empty() && std::isdigit(static_cast<unsigned char>(want[0]))) {
    const uint32_t id = uint32_t(std::atoi(want.c_str()));
    return Record(base, id) ? id : 0;
  }
  const std::string w = Upper(want);
  uint32_t hidden = 0;
  for (uint32_t id = 1; id < 1000; ++id)
    if (const uint8_t* rec = Record(base, id); rec && Upper(reinterpret_cast<const char*>(rec + kName)) == w) {
      if (rec[kSelectable] == 1) return id;
      if (!hidden) hidden = id;
    }
  return hidden;
}

// A guest call: r3.. = args.
template <typename F>
void Call(PPCContext& ctx, uint8_t* base, F fn, std::initializer_list<int64_t> args) {
  PPCRegister* r[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  size_t i = 0;
  for (const int64_t a : args) r[i++]->s64 = a;
  fn(ctx, base);
}

bool g_done = false;
Clock::time_point g_menu_since{};
std::atomic<bool> g_booted{false}, g_entrances{false};
Clock::time_point g_entrances_since{};

void StartMatch(PPCContext& ctx, uint8_t* base) {
  const Match m = Parse(REXCVAR_GET(dbg_match));
  const uint32_t setup = Rd32(base + kSetup), menu = Rd32(base + kMenu);
  if (!setup || !menu) return;
  std::vector<uint32_t> ids;
  std::string line;
  for (const std::string& who : m.people) {
    const uint32_t id = FindSuperstar(base, who);
    if (!id) {
      REXLOG_ERROR("SVRDBG: boot: no superstar '{}' - not booted", who);
      g_done = true;
      return;
    }
    ids.push_back(id);
    line += fmt::format("{}{} ({})", line.empty() ? "" : ", ", who, id);
  }
  if (ids.size() < 2) {
    REXLOG_ERROR("SVRDBG: boot: dbg_match needs people=<two or more> - not booted");
    g_done = true;
    return;
  }
  const int arena = svr2011::ArenaNumberFor(m.arena);
  const PPCContext saved = ctx;
  Call(ctx, base, sub_82594990, {setup, 0});        // mode: exhibition
  Call(ctx, base, sub_828BD278, {setup, m.rule, 0});  // rule (slots cleared, the profile's options)
  Call(ctx, base, sub_828BD1F0, {setup, arena < 0 ? 300 : arena, 0});
  for (uint32_t i = 0; i < ids.size(); ++i) {
    const int attire = (i < m.attires.size() ? m.attires[i] : 1) - 1;
    const int pad = i == 0 && !m.all_cpu ? 0 : -1;
    Call(ctx, base, sub_828BB290, {setup, i, ids[i], pad, attire, 2, 1, 1});
    Call(ctx, base, sub_828B9D70, {setup, i, 0});  // a wrestler
    Call(ctx, base, sub_828B9D80, {setup, i, i});  // a team of its own
    Call(ctx, base, sub_828B9E30, {setup, i, 0});
    Call(ctx, base, sub_828B9E40, {setup, i, -1});
  }
  Call(ctx, base, sub_828BB418, {setup});
  if (const uint32_t game = Rd32(base + kGame)) Wr32(base + game + 16, 1);
  Call(ctx, base, sub_828BC5E8, {setup});  // the commit
  Call(ctx, base, sub_828C3EA8, {});
  Call(ctx, base, sub_82574050, {kLive + 288});
  Call(ctx, base, sub_82440328, {menu, 1});  // the next scene: the exhibition match
  ctx = saved;
  g_done = true;
  g_booted = true;
  REXLOG_INFO("SVRDBG: boot: match - {}; rule {:02X}, arena {}{}", line, m.rule, m.arena, m.all_cpu ? ", all CPU" : "");
}

}  // namespace

bool BootWanted() { return !g_done && REXCVAR_GET(dbg_boot) == "match"; }

void InstallBoot() {
  if (REXCVAR_GET(dbg_boot).empty()) return;
  if (REXCVAR_GET(dbg_boot) != "match") {
    REXLOG_ERROR("SVRDBG: boot: dbg_boot={} isn't known (match)", REXCVAR_GET(dbg_boot));
    return;
  }
  REXCVAR_SET(skip_intros, true);
  REXCVAR_SET(skip_training, true);
  REXLOG_INFO("SVRDBG: boot: match ({})", REXCVAR_GET(dbg_match));
}

void BootFrame(PPCContext& ctx, uint8_t* base) {
  if (!BootWanted()) return;
  uint32_t group = 0, row = 0;
  if (!ScriptMenuState(&group, &row) || group != 1) {
    g_menu_since = {};
    return;
  }
  // (the main menu up and settled a moment)
  if (g_menu_since == Clock::time_point{}) g_menu_since = Clock::now();
  if (Clock::now() - g_menu_since < std::chrono::milliseconds(300)) return;
  StartMatch(ctx, base);
}

void OnEntrances(bool on) {
  if (on && !g_entrances.load()) g_entrances_since = Clock::now();
  g_entrances = on;
}

void BootInput(uint8_t* base, uint32_t state) {
  if (!state || !g_booted.load() || !g_entrances.load() || REXCVAR_GET(dbg_entrances)) return;
  // A for 60 ms, then 60 ms without, the packet number moving on (as fast_start.cpp's START)
  constexpr uint16_t kA = 0x1000;
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - g_entrances_since).count();
  if ((ms / 60) % 2) return;
  uint8_t* buttons = base + state + 4;
  const uint16_t b = uint16_t((buttons[0] << 8 | buttons[1]) | kA);
  buttons[0] = uint8_t(b >> 8), buttons[1] = uint8_t(b);
  Wr32(base + state, Rd32(base + state) + 1);
}

}  // namespace svr2011::dbg
