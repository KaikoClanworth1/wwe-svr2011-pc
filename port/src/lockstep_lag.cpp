// WWE SmackDown vs. Raw 2011 - online lockstep: a late input waits its lateness, not a whole frame.
//
// The lockstep tick (sub_82895078, once a frame on the match thread) pumps the session's packets into the
// players' queues (sub_82893080) and then asks whether every player's input for this frame is there
// (sub_82893268 at lr 0x82895144: 0 = yes). When one is missing the tick ends and the world stands still for
// the whole frame, even for a packet that lands a millisecond later: the socket is read only by that pump,
// once a frame (the lag budget of 11 Nov 2026: input packets are read 17 ms apart, and the lead's margin of
// 2-3 frames over the trip is what covers that). Here, while the answer is "missing", the tick sleeps a
// millisecond, pumps and asks again, up to p2p_hold_poll_ms; a packet late by 3 ms then costs 3 ms, and the
// input lead (net_stats.cpp PingLead) can follow the trip alone.
//
// Every 5 s of a match with polls a log line says how many ticks polled, how many got their input in time and
// how long they waited. When 40 polled ticks in a row got nothing (a player gone, a session not ready) the
// polling rests for 120 ticks.
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

// Off by default (13 Nov 2026): in three after-rounds (LAN, 80 ms, 150 ms simulated links) this hook never
// reported a poll while the 80 ms link held a frame 140 times, so the holds come through another path of the
// tick (its check at lr 0x8289511C, reached without a pump, or the +2268/+2272 count after sub_82892C50);
// the hook stays for that follow-up, --p2p_hold_poll_ms=10 turns it on.
REXCVAR_DEFINE_UINT32(p2p_hold_poll_ms, 0, "Online",
                      "Milliseconds an online match's tick keeps asking for a late input (every ms) before it holds "
                      "the frame (0: a late input holds a whole frame, as the game does; up to 15)");

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kCheckAfterPump = 0x82895144;  // (the tick's check right after its pump)

uint64_t g_polled = 0, g_rescued = 0, g_waited_us = 0, g_gave_up_us = 0;  // (the match thread only)
uint32_t g_dry = 0, g_rest = 0;
Clock::time_point g_report{};

}  // namespace

REX_EXTERN(__imp__sub_82893268);
REX_HOOK_RAW(sub_82893268) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t from = uint32_t(ctx.lr);
  __imp__sub_82893268(ctx, base);
  if (from != kCheckAfterPump || !obj || ctx.r3.u32 == 0) return;
  const uint32_t budget_ms = std::min<uint32_t>(REXCVAR_GET(p2p_hold_poll_ms), 15);
  if (!budget_ms) return;
  if (g_rest) {
    --g_rest;
    return;
  }
  const auto t0 = Clock::now();
  const auto deadline = t0 + std::chrono::milliseconds(budget_ms);
  ++g_polled;
  bool got = false;
  while (Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ctx.r3.u64 = obj;
    sub_82893080(ctx, base);  // (the pump: the session's packets into the queues; safe to repeat)
    ctx.r3.u64 = obj;
    __imp__sub_82893268(ctx, base);
    if (ctx.r3.u32 == 0) {
      got = true;
      break;
    }
  }
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
  if (got) {
    ++g_rescued;
    g_waited_us += us;
    g_dry = 0;
  } else {
    g_gave_up_us += us;
    if (++g_dry >= 40) {
      g_dry = 0;
      g_rest = 120;
    }
  }
  if (g_report == Clock::time_point{}) g_report = t0;
  if (t0 - g_report >= std::chrono::seconds(5)) {
    REXLOG_INFO("net: late inputs: {} ticks polled, {} got theirs after {:.1f} ms on average, {} held the frame "
                "after {} ms of polling",
                g_polled, g_rescued, g_rescued ? g_waited_us / 1000.0 / g_rescued : 0.0, g_polled - g_rescued,
                budget_ms);
    g_polled = g_rescued = g_waited_us = g_gave_up_us = 0;
    g_report = t0;
  }
}
