// Ring Kit, game side (arenas branch). See ring_rules.h.
//
// The ring is defined once, in a block of constants the ring code and the
// wrestlers' moves share (0x82D9D700): mat height -12 (+0x0C), edge 31.3
// (+0x08), size 64 (+0x04), rope spacing 4.2 (+0x1C), the four corner posts
// at (+-31.7, -12, +-31.7) (+0x80). Start-up code (0x82D35DD0-0x82D36100)
// derives globals from it; the bottom rope's height is one of them
// (0x82E354E4 = -12 - 3.4). Rope k (0 = bottom) is at that height minus
// k * 4.2. Units: 1 = 10 cm, y points down.
#include "ring_rules.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"

namespace {

rex::memory::Memory* g_memory = nullptr;
std::mutex g_mutex;

constexpr uint32_t kRingConstants = 0x82D9D700;
constexpr uint32_t kRopeGap = 0x82D9D71C;   // 4.2
constexpr uint32_t kRopeBase = 0x82E354E4;  // -15.4 (= -12 - 3.4)
constexpr float kGameGap = 4.2f, kGameBase = 3.4f, kMatY = -12.0f;

struct Rules {
  bool ropes[3] = {true, true, true};
  float rope_base = kGameBase, rope_gap = kGameGap;
};
Rules g_rules;
std::atomic<bool> g_no_ropes{false};  // every rope left out: rope gameplay off

bool NoRopes(const Rules& r) { return !r.ropes[0] && !r.ropes[1] && !r.ropes[2]; }

float RdF(const uint8_t* p) {
  const uint32_t v = uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void WrF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

Rules Parse(const std::string& text) {
  Rules r;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    std::istringstream vs(v);
    if (k == "ring.ropes") {
      for (bool& b : r.ropes) { int x = 1; vs >> x; b = x != 0; }
    } else if (k == "ring.rope_base") {
      r.rope_base = std::strtof(v.c_str(), nullptr);
    } else if (k == "ring.rope_gap") {
      r.rope_gap = std::strtof(v.c_str(), nullptr);
    }
  }
  // keep the top rope under the posts (25.1 above the floor, 13.1 above the mat)
  if (r.rope_base < 1.0f || r.rope_base > 8.0f) r.rope_base = kGameBase;
  if (r.rope_gap < 1.5f || r.rope_gap > 6.0f || r.rope_base + 2 * r.rope_gap > 12.5f) r.rope_gap = kGameGap;
  return r;
}

void Apply(const Rules& r) {
  if (!g_memory) return;
  uint8_t* base = g_memory->virtual_membase();
  WrF(base + kRopeGap, r.rope_gap);
  WrF(base + kRopeBase, kMatY - r.rope_base);
}

// Test aid (SVR2011_TEST_STATE_LOG=<offsets>, e.g. "212,216"): a fighter's
// words at those offsets and its position (+288), whenever one changes.
std::vector<uint32_t> g_log_fields;
bool g_log_scan = false;  // SVR2011_TEST_STATE_LOG=scan

void LogFighter(uint8_t* base, uint32_t f) {
  constexpr uint32_t kChars = 0x82E3CC50;
  int i = 0;
  while (i < 6 && Rd32(base + kChars + i * 4) != f) ++i;
  if (i == 6) return;
  if (g_log_scan) {
    // every small-int word of the first 4 KB (not counters) for fighter 0
    static uint32_t prev[1024];
    static bool primed = false;
    if (i != 0) return;
    std::string changed;
    for (uint32_t k = 0; k < 1024; ++k) {
      const uint32_t v = Rd32(base + f + 4 * k);
      const uint32_t o = prev[k];
      prev[k] = v;
      if (!primed || v == o || v > 4096 || o > 4096 || v == o + 1) continue;
      changed += fmt::format(" +{}: {}->{}", 4 * k, o, v);
    }
    primed = true;
    static int calls = 0;
    if (++calls % 600 == 1)
      REXLOG_INFO("[svr2011] ring scan: call {} fighter {:08X} +212 {} +0 {:08X}", calls, f, Rd32(base + f + 212),
                  Rd32(base + f));
    if (!changed.empty())
      REXLOG_INFO("[svr2011] ring scan: at ({:.1f} {:.1f} {:.1f}){}", RdF(base + f + 288), RdF(base + f + 292),
                  RdF(base + f + 296), changed);
    return;
  }
  static std::vector<uint32_t> last[6];
  auto& l = last[i];
  l.resize(g_log_fields.size());
  std::string changed;
  for (size_t k = 0; k < g_log_fields.size(); ++k) {
    const uint32_t v = Rd32(base + f + g_log_fields[k]);
    if (v == l[k]) continue;
    changed += fmt::format(" +{}: {} -> {}", g_log_fields[k], l[k], v);
    l[k] = v;
  }
  if (!changed.empty())
    REXLOG_INFO("[svr2011] ring state: {} at ({:.1f} {:.1f} {:.1f}){}", i, RdF(base + f + 288), RdF(base + f + 292),
                RdF(base + f + 296), changed);
}

}  // namespace

// The rope draw (once a frame in a match).
REX_EXTERN(__imp__sub_8219A508);
REX_HOOK_RAW(sub_8219A508) {
  Apply(g_rules);  // (start-up code sets the rope height after install)
  __imp__sub_8219A508(ctx, base);
}

// -- No ropes: rope gameplay off ---------------------------------------------
//
// (static analysis: docs/ARENA_MOD_MAKER_PLAN.md "Rope gameplay")
// A running fighter reaching the rope line: sub_823091A0(fighter, quarter)
// starts the rebound (motion 60); a whipped one: sub_82309398 (62, 64 ...);
// a second path checks sub_823054F0 first. With no ropes the runner brakes
// instead: the whip bytes cleared and the game's run brake (motion 53, as
// releasing run does). Rope breaks: every pin / submission / hold asks
// sub_821BD388(fighter) "touching the ropes"; with no ropes it answers no.
namespace {

void Brake(PPCContext& ctx, uint8_t* base, const char* why) {
  const uint32_t f = ctx.r3.u32;
  REXLOG_INFO("[svr2011] ring: no ropes, {} {:08X} brakes (motion {})", why, f, Rd32(base + f + 212));
  base[f + 1286] = 0;
  base[f + 1287] = 0;
  base[f + 1290] = 0;
  ctx.r3.u64 = f;
  ctx.r4.u64 = 53;
  sub_82397228(ctx, base);
  ctx.r3.u64 = 1;
}

}  // namespace

REX_EXTERN(__imp__sub_823091A0);
REX_HOOK_RAW(sub_823091A0) {
  if (g_no_ropes) {
    Brake(ctx, base, "runner");
    return;
  }
  __imp__sub_823091A0(ctx, base);
}

REX_EXTERN(__imp__sub_82309398);
REX_HOOK_RAW(sub_82309398) {
  if (g_no_ropes) {
    Brake(ctx, base, "whipped");
    return;
  }
  __imp__sub_82309398(ctx, base);
}

REX_EXTERN(__imp__sub_823054F0);
REX_HOOK_RAW(sub_823054F0) {
  if (g_no_ropes) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_823054F0(ctx, base);
}

REX_EXTERN(__imp__sub_821BD388);
REX_HOOK_RAW(sub_821BD388) {
  if (g_no_ropes) {
    static int n = 0;
    if (n++ % 600 == 0) REXLOG_INFO("[svr2011] ring: no ropes, no rope break ({} asks)", n);
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_821BD388(ctx, base);
}

namespace {

// Test aid (SVR2011_TEST_RUN=1): every 6 s, fighter 0 standing (motion 0 or
// 20) starts running the way it faces (sub_8220C808: motion 50), so a
// test sees rope rebounds (or brakes) without knowing the run button.
bool g_test_run = false;

void TestRun(PPCContext& ctx, uint8_t* base, uint32_t f) {
  if (Rd32(base + 0x82E3CC50) != f) return;
  static auto last = std::chrono::steady_clock::now();
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(6)) return;
  const uint32_t m = Rd32(base + f + 212);
  if (m != 0 && m != 20) return;
  last = now;
  const auto saved = ctx;
  ctx.r3.u64 = f;
  sub_8220C808(ctx, base);
  ctx = saved;
  REXLOG_INFO("[svr2011] ring: test run from ({:.1f} {:.1f})", RdF(base + f + 288), RdF(base + f + 296));
}

}  // namespace

// A fighter's control, once a frame each (test aids above).
// (sub_822EE2E0(control): fighter at +32; runs the 116 control handlers at
// 0x82008790 until one takes the frame - the run handler is entry 7)
REX_EXTERN(__imp__sub_822EE2E0);
REX_HOOK_RAW(sub_822EE2E0) {
  if (!g_log_fields.empty() || g_test_run) {
    const uint32_t f = Rd32(base + ctx.r3.u32 + 32);
    if (!g_log_fields.empty()) LogFighter(base, f);
    if (g_test_run) TestRun(ctx, base, f);
  }
  __imp__sub_822EE2E0(ctx, base);
}

namespace svr2011 {

void InstallRingRules(rex::memory::Memory* memory) {
  g_memory = memory;
  g_test_run = std::getenv("SVR2011_TEST_RUN") != nullptr;
  if (const char* v = std::getenv("SVR2011_TEST_STATE_LOG"); v && *v) {
    g_log_scan = std::string(v) == "scan";
    if (g_log_scan) g_log_fields.push_back(0);
    for (const char* p = v; *p;) {
      char* end = nullptr;
      const unsigned long off = std::strtoul(p, &end, 0);
      if (end == p) { ++p; continue; }
      g_log_fields.push_back(uint32_t(off));
      p = end;
    }
    REXLOG_INFO("[svr2011] ring: state log {} ({} fields)", g_log_scan ? "scan" : "fields", g_log_fields.size());
  }
  // the ring constants are in read-only data
  if (auto* heap = memory->LookupHeap(kRingConstants))
    heap->Protect(kRingConstants & ~0xFFFu, 0x1000,
                  rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  // Test aid: SVR2011_TEST_RING=<manifest lines, ';' for new lines>
  if (const char* v = std::getenv("SVR2011_TEST_RING"); v && *v) {
    std::string s = v;
    for (char& c : s)
      if (c == ';') c = '\n';
    SetRingRules(s);
  }
}

void SetRingRules(const std::string& manifest) {
  std::lock_guard lock(g_mutex);
  g_rules = Parse(manifest);
  g_no_ropes = NoRopes(g_rules);
  Apply(g_rules);
  REXLOG_INFO("[svr2011] ring: ropes {}{}{}, bottom rope {:.1f} above the mat, gap {:.1f}", int(g_rules.ropes[0]),
              int(g_rules.ropes[1]), int(g_rules.ropes[2]), g_rules.rope_base, g_rules.rope_gap);
}

}  // namespace svr2011
