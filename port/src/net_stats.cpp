// WWE SmackDown vs. Raw 2011 - online match network statistics (net_stats.h).
//
// Online matches run the game's own lockstep (Yuke's "gss" library): every
// frame each human's game sends its packed pad (message type 16, 4 bytes) and
// the world steps only once every human's input for that frame has arrived.
// The match net object (*0x82EDE914, sub_828928C8) keeps a "waiting" flag at
// +2264: while it is set the game's task groups don't run, so the world stands
// still while rendering goes on at 60 fps - online "lag" is these waits.
//
// The object: 6 entries of 72 bytes at +0 (+0 the net player number, +4 the
// person / lobby slot, +44 its input for this frame arrived, +48 own input,
// +52 the received input), +2232 this game's entry, +2236 the entry count.
// The send (sub_82893BD0, once a frame, r3 = the object) is where this samples.
//
// Every 5 s of a match the log gets a line: frames, frames waited, the waits'
// count and longest, and which entries the waits were for. The ping indicator
// (fps_overlay.cpp) reads NetStatsNow().
#include "net_stats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"
#include "net_sim.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kNetObject = 0x82EDE914;
constexpr uint32_t kEntrySize = 72, kMaxEntries = 6;
constexpr uint32_t kGotInput = 44, kLocal = 2232, kCount = 2236, kWaiting = 2264;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

struct Window {
  uint64_t frames = 0, waited = 0, waits = 0;
  double longest_ms = 0, waited_ms = 0;
  std::array<uint64_t, kMaxEntries> blamed{};  // frames waited for each entry's input
};

std::mutex g_mutex;
Window g_window;                 // the current 5 s
Clock::time_point g_window_start{};
bool g_was_waiting = false;
Clock::time_point g_wait_start{}, g_last_frame{};
uint32_t g_entries = 0, g_local = 0;
svr2011::NetStats g_now{};       // the last finished window (the indicator)
std::atomic<int64_t> g_last_sample_ms{0};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

void Report(Clock::time_point now) {
  const double secs = std::chrono::duration<double>(now - g_window_start).count();
  std::string blame;
  for (uint32_t k = 0; k < g_entries && k < kMaxEntries; ++k) {
    if (k == g_local) continue;
    blame += fmt::format("{}{}:{}", blame.empty() ? "" : " ", k, g_window.blamed[k]);
  }
  std::string rtts;
  for (const auto& l : svr2011::NetLinks()) {
    rtts += fmt::format("{}{}.{}: {:.0f} (worst {:.0f}){}", rtts.empty() ? "" : ", ", (l.peer >> 8) & 255,
                        l.peer & 255, l.rtt_ms, l.worst_ms, l.relayed ? " relay" : "");
  }
  REXLOG_INFO("net: {:.1f} s, {} frames ({:.1f}/s), waited {} frames ({:.0f} ms) in {} waits, longest {:.0f} ms; "
              "waiting for entries [{}] (this game {} of {}); round trips ms [{}]",
              secs, g_window.frames, g_window.frames / std::max(secs, 0.001), g_window.waited, g_window.waited_ms,
              g_window.waits, g_window.longest_ms, blame, g_local, g_entries, rtts);
  g_now.active = true;
  g_now.waited_pct = g_window.frames ? float(100.0 * g_window.waited / g_window.frames) : 0.0f;
  g_now.longest_ms = float(g_window.longest_ms);
  g_now.waits = uint32_t(g_window.waits);
  g_window = {};
  g_window_start = now;
}

}  // namespace

// The lockstep send, once a frame (r3 = the match net object).
REX_EXTERN(__imp__sub_82893BD0);
REX_HOOK_RAW(sub_82893BD0) {
  static const bool off = std::getenv("SVR2011_NET_STATS") && std::getenv("SVR2011_NET_STATS")[0] == '0';
  const uint32_t obj = ctx.r3.u32;
  if (!off && obj) {
    const uint8_t* o = base + obj;
    const auto now = Clock::now();
    std::lock_guard lock(g_mutex);
    if (g_window_start == Clock::time_point{}) g_window_start = now;
    g_entries = std::min(Rd32(o + kCount), kMaxEntries);
    g_local = Rd32(o + kLocal);  // (this game's lobby slot: its entry is the one with that slot at +4 - they differ with CPU slots)
    for (uint32_t k = 0; k < g_entries; ++k)
      if (Rd32(o + k * kEntrySize + 4) == Rd32(o + kLocal)) g_local = k;
    const bool waiting = Rd32(o + kWaiting) != 0;
    ++g_window.frames;
    if (waiting) {
      ++g_window.waited;
      if (!g_was_waiting) g_wait_start = now, ++g_window.waits;
      if (g_last_frame != Clock::time_point{})
        g_window.waited_ms += std::chrono::duration<double, std::milli>(now - g_last_frame).count();
      for (uint32_t k = 0; k < g_entries; ++k) {
        if (k != g_local && Rd32(o + k * kEntrySize + kGotInput) == 0) ++g_window.blamed[k];
      }
    } else if (g_was_waiting) {
      g_window.longest_ms =
          std::max(g_window.longest_ms, std::chrono::duration<double, std::milli>(now - g_wait_start).count());
    }
    g_was_waiting = waiting;
    g_last_frame = now;
    g_last_sample_ms = NowMs();
    if (now - g_window_start >= std::chrono::seconds(5)) Report(now);
  }
  __imp__sub_82893BD0(ctx, base);
}

namespace svr2011 {

NetStats NetStatsNow() {
  std::lock_guard lock(g_mutex);
  NetStats s = g_now;
  // (no lockstep frames for 2 s: not in an online match)
  s.active = s.active && NowMs() - g_last_sample_ms.load() < 2000;
  return s;
}

}  // namespace svr2011
