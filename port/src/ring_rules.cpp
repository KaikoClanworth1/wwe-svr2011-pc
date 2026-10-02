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

void LogFighter(uint8_t* base, uint32_t f) {
  constexpr uint32_t kChars = 0x82E3CC50;
  int i = 0;
  while (i < 6 && Rd32(base + kChars + i * 4) != f) ++i;
  if (i == 6) return;
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

// A fighter's update, once a frame each (test aid above).
REX_EXTERN(__imp__sub_822ED230);
REX_HOOK_RAW(sub_822ED230) {
  if (!g_log_fields.empty()) LogFighter(base, ctx.r4.u32);
  __imp__sub_822ED230(ctx, base);
}

namespace svr2011 {

void InstallRingRules(rex::memory::Memory* memory) {
  g_memory = memory;
  if (const char* v = std::getenv("SVR2011_TEST_STATE_LOG"); v && *v) {
    for (const char* p = v; *p;) {
      char* end = nullptr;
      const unsigned long off = std::strtoul(p, &end, 0);
      if (end == p) { ++p; continue; }
      g_log_fields.push_back(uint32_t(off));
      p = end;
    }
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
  Apply(g_rules);
  REXLOG_INFO("[svr2011] ring: ropes {}{}{}, bottom rope {:.1f} above the mat, gap {:.1f}", int(g_rules.ropes[0]),
              int(g_rules.ropes[1]), int(g_rules.ropes[2]), g_rules.rope_base, g_rules.rope_gap);
}

}  // namespace svr2011
