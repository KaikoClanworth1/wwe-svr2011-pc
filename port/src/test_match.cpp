// WWE SmackDown vs. Raw 2011 - test aid: a match with the wrestlers and arena
// a test asks for, whatever was picked on the select screen.
//
//   SVR2011_TEST_MATCH="people=JOHN CENA,RANDY ORTON arena=11"
//
// people: roster names (as the log's "match:" lines print them) or ids,
// comma-separated, in person order; arena: the arena id (the "match:" line's
// "arena N"). The rule stays SVR2011_TEST_RULE's (match_types.cpp). With
// "route match" (script_input.h) a test goes from the start to that match.
//
// As the match's people are built from the picks (sub_828BC5E8(match) ->
// sub_828BBEF0: 2116-byte slots at match+432, +8 superstar id * 100 + attire,
// +54 the id, +5 the team, +4 the kind, -8 the controller - see
// match_types.cpp), the slots get the asked-for superstars; slots beyond the
// picks become CPU wrestlers of their own team. The arena: the match
// screen's choice (below).

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "script_input.h"

namespace {

constexpr uint32_t kSlots = 432, kSlotSize = 2116, kMaxPeople = 6;
constexpr uint32_t kLive = 0x82E3DE00, kArena = 64;
constexpr uint32_t kIdToIndex = 0x82DB3610, kRecords = 0x82E407C0, kRecordSize = 260, kOwnId = 32, kName = 34;
constexpr uint32_t kNoPick = 51200;

uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void Wr16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

struct TestMatch {
  bool set = false;
  std::vector<std::string> people;  // names or ids
  int arena = -1;
};

const TestMatch& Config() {
  static const TestMatch config = [] {
    TestMatch m;
    const char* v = std::getenv("SVR2011_TEST_MATCH");
    if (!v || !*v) return m;
    m.set = true;
    // "key=value" pairs; a value runs to the next " key=" (names have spaces)
    std::string s = v;
    size_t at = 0;
    while (at < s.size()) {
      const size_t eq = s.find('=', at);
      if (eq == std::string::npos) break;
      std::string key = s.substr(at, eq - at);
      while (!key.empty() && key.front() == ' ') key.erase(0, 1);
      size_t end = s.size();
      for (size_t next = s.find('=', eq + 1); next != std::string::npos; next = s.find('=', next + 1)) {
        const size_t space = s.rfind(' ', next);
        if (space != std::string::npos && space > eq) {
          end = space;
          break;
        }
      }
      const std::string value = s.substr(eq + 1, end - eq - 1);
      if (key == "people") {
        std::stringstream list(value);
        std::string name;
        while (std::getline(list, name, ',') && m.people.size() < kMaxPeople) {
          while (!name.empty() && name.front() == ' ') name.erase(0, 1);
          while (!name.empty() && name.back() == ' ') name.pop_back();
          if (!name.empty()) m.people.push_back(name);
        }
      } else if (key == "arena") {
        m.arena = std::atoi(value.c_str());
      } else {
        REXLOG_WARN("test match: unknown setting '{}'", key);
      }
      at = end;
    }
    return m;
  }();
  return config;
}

const uint8_t* Record(const uint8_t* base, uint32_t id) {
  if (id >= 1000) return nullptr;
  const uint32_t index = Rd16(base + kIdToIndex + id * 2);
  if (index >= 1000) return nullptr;
  const uint8_t* rec = base + kRecords + index * kRecordSize;
  return Rd16(rec + kOwnId) == id ? rec : nullptr;
}

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// A name or id -> the superstar id (0: none).
uint32_t FindSuperstar(const uint8_t* base, const std::string& want) {
  if (!want.empty() && std::isdigit(static_cast<unsigned char>(want[0]))) {
    const uint32_t id = uint32_t(std::atoi(want.c_str()));
    return Record(base, id) ? id : 0;
  }
  const std::string w = Upper(want);
  for (uint32_t id = 1; id < 1000; ++id)
    if (const uint8_t* rec = Record(base, id); rec && Upper(reinterpret_cast<const char*>(rec + kName)) == w) return id;
  return 0;
}

// Not the title screen's demo match: only a match chosen in the menus (the
// last entry chosen is not the title screen's, group 0).
bool Applies() {
  if (!Config().set) return false;
  uint32_t group = 0, row = 0;
  return svr2011::ScriptMenuSelects(&group, &row) > 0 && group != 0;
}

}  // namespace

// The match's arena: the match screen's choice (its object, *0x82EDEB48:
// +244, read by sub_8247B6C8(screen); sub_82773F00 copies it to the live
// match record's +64 as the match loads). It is set as soon as the screen
// exists - as if picked in SELECT ARENA - so everything the game works out
// from it (a cage match's cage, ...) fits. Done from the live record's
// getter (sub_825740C8), which runs all the time. (Putting it straight into
// the live record left parts of the load with the arena they had read
// first: no cage, and a freeze on a phone.)
namespace {
constexpr uint32_t kMatchScreen = 0x82EDEB48, kScreenArena = 244;

void SetScreenArena(uint8_t* base, uint32_t screen) {
  const int arena = Config().arena;
  if (arena < 0 || !screen || !Applies() || Rd32(base + screen + kScreenArena) == uint32_t(arena)) return;
  static uint32_t logged = 0;
  if (logged++ < 2)
    REXLOG_INFO("test match: arena {} (the match screen had {})", arena, Rd32(base + screen + kScreenArena));
  Wr32(base + screen + kScreenArena, uint32_t(arena));
}
}  // namespace

REX_EXTERN(__imp__sub_825740C8);
REX_HOOK_RAW(sub_825740C8) {
  __imp__sub_825740C8(ctx, base);
  if (Config().arena >= 0) {
    const uint32_t r3 = ctx.r3.u32;
    SetScreenArena(base, Rd32(base + kMatchScreen));
    ctx.r3.u64 = r3;
  }
}

REX_EXTERN(__imp__sub_8247B6C8);
REX_HOOK_RAW(sub_8247B6C8) {
  SetScreenArena(base, ctx.r3.u32);
  __imp__sub_8247B6C8(ctx, base);
}

REX_EXTERN(__imp__sub_828BC5E8);
REX_HOOK_RAW(sub_828BC5E8) {
  const TestMatch& m = Config();
  if (Applies() && ctx.r3.u32) {
    const uint32_t match = ctx.r3.u32;
    std::string done;
    for (uint32_t i = 0; i < m.people.size(); ++i) {
      const uint32_t id = FindSuperstar(base, m.people[i]);
      if (!id) {
        REXLOG_WARN("test match: no superstar '{}'", m.people[i]);
        continue;
      }
      uint8_t* slot = base + match + kSlots + i * kSlotSize;
      const bool picked = Rd32(slot + 8) != kNoPick;
      Wr32(slot + 8, id * 100 + 2);  // (attire: the first, as the select screen gives)
      Wr16(slot + 54, id);
      if (!picked) {
        slot[5] = uint8_t(i);  // (a team of its own)
        slot[4] = 0;           // (kind: a wrestler)
        slot[-8] = 1;          // (controller: the CPU)
      }
      done += (done.empty() ? "" : ", ") + std::string(reinterpret_cast<const char*>(Record(base, id) + kName));
    }
    REXLOG_INFO("test match: people {}; arena {}", done.empty() ? "as picked" : done,
                Rd32(base + kLive + kArena));
  }
  __imp__sub_828BC5E8(ctx, base);
}
