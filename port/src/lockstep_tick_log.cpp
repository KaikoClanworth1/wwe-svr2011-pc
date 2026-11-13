// WWE SmackDown vs. Raw 2011 - online lockstep tick log (a test aid, SVR2011_TEST_TICK_LOG=1).
//
// Which path of the lockstep tick (sub_82895078, once a frame from the match update) holds the world: the
// tick ends with the object's +2264 ("waiting": the task groups don't run while it is set). Its paths in the
// match (state +2252 == 6), as the 13 Nov round showed: the normal frame - sub_8288D3B8() == 0, the pump
// sub_82893080, the check sub_82893268 (lr 0x82895144) == 0, the advance sub_82892C50, the tail
// (sub_828937E0 ...), +2264 ends 0; the HOLD - sub_8288D3B8() != 0 (an input is missing: the session's queue,
// before any pump), the check without a pump (lr 0x8289511C) == 0, +2264 = 1, no advance, no tail. Every
// 5 s this logs how many ticks took which path with which check results and end flag, and the first ticks
// that end waiting in detail (the entries' +44 flags, which stay 1 through a hold). The re-poll of a late input
// belongs on sub_8288D3B8 from the tick (lockstep_lag.cpp hooked the check at 0x82895144, which a hold never
// reaches).
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

#include "generated/default/svr2011_init.h"

namespace {

using Clock = std::chrono::steady_clock;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

bool On() {
  static const bool on = [] { const char* v = std::getenv("SVR2011_TEST_TICK_LOG"); return v && *v && *v != '0'; }();
  return on;
}

struct Tick {
  bool in = false;
  uint32_t obj = 0;
  bool pumped = false, advanced = false, tail = false;  // (the check sub_82893268 is lockstep_lag.cpp's hook: not here)
};
thread_local Tick t_tick;

std::map<std::string, uint32_t> g_hist;  // (signature -> ticks, the match thread only)
uint32_t g_detail = 0;
Clock::time_point g_report{};

}  // namespace

REX_EXTERN(__imp__sub_82893080);
REX_HOOK_RAW(sub_82893080) {  // (the pump)
  if (t_tick.in) t_tick.pumped = true;
  __imp__sub_82893080(ctx, base);
}

REX_EXTERN(__imp__sub_82892C50);
REX_HOOK_RAW(sub_82892C50) {  // (the advance: +2276 the frame)
  if (t_tick.in) t_tick.advanced = true;
  __imp__sub_82892C50(ctx, base);
}

REX_EXTERN(__imp__sub_828937E0);
REX_HOOK_RAW(sub_828937E0) {  // (the tail's first call)
  if (t_tick.in) t_tick.tail = true;
  __imp__sub_828937E0(ctx, base);
}

REX_EXTERN(__imp__sub_82895078);
REX_HOOK_RAW(sub_82895078) {  // (the tick)
  if (!On() || !ctx.r3.u32) {
    __imp__sub_82895078(ctx, base);
    return;
  }
  const uint32_t obj = ctx.r3.u32;
  const uint8_t* o = base + obj;
  const uint32_t state = Rd32(o + 2252), f2248 = Rd32(o + 2248), count = Rd32(o + 2236);
  const uint32_t w_before = Rd32(o + 2264), f2268_before = Rd32(o + 2268), f2272_before = Rd32(o + 2272),
                 frame_before = Rd32(o + 2276);
  t_tick = Tick{};
  t_tick.in = true;
  t_tick.obj = obj;
  const auto t0 = Clock::now();
  __imp__sub_82895078(ctx, base);
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
  t_tick.in = false;
  const uint32_t w_after = Rd32(o + 2264), f2268 = Rd32(o + 2268), f2272 = Rd32(o + 2272), frame = Rd32(o + 2276);

  std::string sig = "state " + std::to_string(state) + (f2248 + 1 == count ? " 2248+1==count" : "") +
                    (t_tick.pumped ? " pump" : "");
  sig += t_tick.advanced ? " adv" : "";
  sig += t_tick.tail ? " tail" : "";
  sig += " w" + std::to_string(w_before) + ">" + std::to_string(w_after);
  sig += " f2268=" + std::to_string(f2268) + (f2272 != f2272_before ? " f2272+" : " f2272=");
  sig += frame != frame_before ? " frame+" : " frame=";
  ++g_hist[sig];

  if (state == 6 && w_after != 0 && g_detail < 40) {
    ++g_detail;
    std::string entries;
    for (uint32_t k = 0; k < std::min<uint32_t>(count, 6); ++k) {
      const uint8_t* e = o + k * 72;
      entries += " [" + std::to_string(k) + ": got " + std::to_string(Rd32(e + 44)) + " q32 " + std::to_string(Rd32(e + 32)) + "]";
    }
    REXLOG_INFO("tick: waiting after {} ({} us): frame {} -> {}, +2268 {} -> {}, local entry {}, entries{}", sig, us,
                frame_before, frame, f2268_before, f2268, Rd32(o + 2232), entries);
  }

  if (g_report == Clock::time_point{}) g_report = t0;
  if (t0 - g_report >= std::chrono::seconds(5)) {
    std::string lines;
    for (const auto& [k, n] : g_hist) lines += "\n    " + std::to_string(n) + " x " + k;
    REXLOG_INFO("tick: 5 s of ticks by path (state, pump, adv, tail, +2264 before>after, +2268, +2272 moved, "
                "frame +2276 moved):{}",
                lines);
    g_hist.clear();
    g_report = t0;
  }
}
