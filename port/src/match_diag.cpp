// Problem-report diagnostics for a frozen match (match_diag.h). The move getters (sub_8218E600 / 580: every
// attack and AI choice asks them) note each fighter's last answer; the characters are the match's list at
// 0x82E3CC50 (ten pointers; +1156 the superstar id). Reads guest memory only - safe from the watchdog
// thread - and logs each case once a match.
#include "match_diag.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <mutex>

#include <rex/logging.h>
#include <rex/ppc.h>

#include "move_packs.h"
#include "superstar_mods.h"

namespace svr2011 {
namespace {

constexpr uint32_t kChars = 0x82E3CC50, kMatchFrames = 0x82E3CD0C;
constexpr uint32_t kQuietFrames = 30 * 60;  // (the match clock: 60 a second)

using Clock = std::chrono::steady_clock;

// Each fighter's last answer: a small table written lock-free (the getters run many times a frame).
struct Last {
  std::atomic<uint32_t> fighter{0}, slot{0}, move{0};
  std::atomic<int64_t> at{0};
};
constexpr int kFighters = 16;
std::array<Last, kFighters> g_last;
std::atomic<bool> g_moved{false};              // an answer since the tick looked

std::mutex g_mutex;                            // (the rest: the match line's names, the tick's state)
std::array<std::string, 10> g_names;           // character index -> name (the match line's)
std::array<uint32_t, 10> g_ids{};               // character index -> superstar id (0xFFFF: unknown)
uint32_t g_clock_at_move = 0;                  // the match clock at the last answer the tick saw
uint32_t g_told_quiet = 0;                     // (the match clock's value when told: once a match)
bool g_was_in_match = false;

uint32_t Rd32(const uint8_t* base, uint32_t a) {
  return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
}

}  // namespace

void NoteMove(uint32_t fighter, uint32_t slot, uint32_t move) {
  if (!fighter) return;
  Last* l = nullptr;
  for (auto& e : g_last) {
    uint32_t f = e.fighter.load(std::memory_order_relaxed);
    if (f == fighter) { l = &e; break; }
    if (!f && e.fighter.compare_exchange_strong(f, fighter)) { l = &e; break; }
  }
  if (!l) return;  // (more than 16 fighters: not noted)
  l->slot.store(slot, std::memory_order_relaxed), l->move.store(move & 0x7FFF, std::memory_order_relaxed);
  l->at.store(Clock::now().time_since_epoch().count(), std::memory_order_relaxed);
  g_moved.store(true, std::memory_order_relaxed);
}

void ClearMatchPeople() {
  std::lock_guard lock(g_mutex);
  g_names = {};
  g_ids.fill(0xFFFF);
}

void NoteMatchPerson(uint32_t index, uint32_t superstar, const std::string& name) {
  if (index >= 10) return;
  std::lock_guard lock(g_mutex);
  g_names[index] = superstar < 0xFFFF ? name + " (id " + std::to_string(superstar) + ")" : name;
  g_ids[index] = superstar;
}

void LogMatchState(uint8_t* base, const char* why) {
  if (!base) return;
  std::lock_guard lock(g_mutex);
  const auto now = Clock::now();
  REXLOG_WARN("[svr2011] match state ({}): match clock {} frames", why, Rd32(base, kMatchFrames));
  for (uint32_t i = 0; i < 10; ++i) {
    const uint32_t ch = Rd32(base, kChars + i * 4);
    if (ch < 0x40000000u || (ch & 3)) continue;  // (a guest heap pointer: the characters live in the high heaps)
    const uint32_t person = Rd32(base, ch + 1156), id = g_ids[i];
    if (g_names[i].empty()) continue;  // (not one of the match line's people: a leftover)
    const std::string mod = id < 512 ? SuperstarModFolder(id) : std::string();
    std::string move = "no move asked yet";
    for (const auto& e : g_last) {
      if (e.fighter.load() != ch) continue;
      const uint16_t m = uint16_t(e.move.load());
      move = fmt::format("last move {} (slot {:03X}, {}, {} s ago)", m, e.slot.load() * 2,
                         PackMove(m)            ? "a move pack's"
                         : MoveMissing(m)       ? "MISSING - its pack is gone"
                         : MoveUsable(m) == 0   ? "the game's, no move record"
                                                : "the game's",
                         std::chrono::duration_cast<std::chrono::seconds>(now - Clock::time_point(Clock::duration(e.at.load()))).count());
      break;
    }
    REXLOG_WARN("[svr2011] match state:   person {} {} - {}; {}", person, g_names[i], mod.empty() ? "not a mod" : "mod " + mod, move);
  }
}

uint32_t MatchClock(uint8_t* base) { return base ? Rd32(base, kMatchFrames) : 0; }

std::vector<MatchPerson> MatchPeople(uint8_t* base) {
  std::vector<MatchPerson> out;
  if (!base) return out;
  std::lock_guard lock(g_mutex);
  const auto now = Clock::now();
  for (uint32_t i = 0; i < 10; ++i) {
    const uint32_t ch = Rd32(base, kChars + i * 4);
    if (ch < 0x40000000u || (ch & 3) || g_names[i].empty()) continue;  // (as LogMatchState)
    MatchPerson p;
    p.index = i, p.person = Rd32(base, ch + 1156), p.superstar = g_ids[i], p.name = g_names[i];
    if (p.superstar < 512) p.mod = SuperstarModFolder(p.superstar);
    for (const auto& e : g_last) {
      if (e.fighter.load() != ch) continue;
      p.last_move = e.move.load() & 0xFFFF;
      p.move_age_s = std::chrono::duration<double>(now - Clock::time_point(Clock::duration(e.at.load()))).count();
      break;
    }
    // (the character's position: +304 points at x, y, z - match_types.cpp's lumberjack framing)
    auto f = [&](uint32_t a) { const uint32_t v = Rd32(base, a); float r; std::memcpy(&r, &v, 4); return r; };
    if (const uint32_t pos = Rd32(base, ch + 304); pos >= 0x40000000u && !(pos & 3))
      p.has_position = true, p.x = f(pos), p.y = f(pos + 4), p.z = f(pos + 8);
    // (the current motion and the root - what hctp_room's PlaceFighter writes, +288, besides the body at +304)
    p.motion = Rd32(base, ch + 212);
    p.root_x = f(ch + 288), p.root_y = f(ch + 292), p.root_z = f(ch + 296);
    out.push_back(std::move(p));
  }
  return out;
}

void MatchDiagTick(uint8_t* base, bool in_match) {
  if (!base) return;
  const uint32_t clock = Rd32(base, kMatchFrames);
  bool tell = false;
  {
    std::lock_guard lock(g_mutex);
    if (!in_match) {  // (a match ended: the next starts clean - its people are noted as it loads, before it runs)
      if (g_was_in_match) {
        for (auto& e : g_last) e.fighter = 0;
        g_names = {}, g_ids.fill(0xFFFF), g_told_quiet = 0, g_clock_at_move = 0;
      }
      g_was_in_match = false;
      return;
    }
    if (!g_was_in_match) {  // (a match starts: what menus asked before isn't the match's)
      for (auto& e : g_last) e.fighter = 0;
      g_clock_at_move = 0;
    }
    g_was_in_match = true;
    if (g_moved.exchange(false) || !g_clock_at_move) g_clock_at_move = clock ? clock : 1;
    // (the match clock ran 30 s on since the last answer: nobody attacks or chooses - "both stopped")
    if (clock > g_clock_at_move + kQuietFrames && g_told_quiet == 0 && g_last[0].fighter.load()) tell = true, g_told_quiet = clock;
  }
  if (tell) LogMatchState(base, "the match clock ran 30 s with no wrestler asking for a move");
  // Test aid: SVR2011_TEST_MATCH_STATE=<s> - the state once, <s> s into the match clock.
  static const uint32_t test_at = [] { const char* v = std::getenv("SVR2011_TEST_MATCH_STATE"); return v ? uint32_t(std::atoi(v)) * 60 : 0u; }();
  static bool test_told = false;
  if (test_at && clock >= test_at && !test_told) test_told = true, LogMatchState(base, "test");
}

}  // namespace svr2011
