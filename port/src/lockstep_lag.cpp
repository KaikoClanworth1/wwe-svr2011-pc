// WWE SmackDown vs. Raw 2011 - online lockstep: a late input waits its lateness, not a whole frame.
//
// The lockstep tick (sub_82895078, once a frame on the match thread) first asks the session whether this
// frame can run (sub_8288D3B8 at lr 0x8289510C: 0 = every player's input is in, from the session's receive
// side, which the network thread fills). On 0 it pumps the packets into the players' queues (sub_82893080),
// checks them (sub_82893268 at lr 0x82895144) and advances; on anything else it ends with the "waiting" flag
// (+2264) set and the world stands still for the whole frame, even for a packet that lands a millisecond later
// (the 13 Nov 2026 tick log, lockstep_tick_log.cpp: every hold took that path; the check after the pump never
// said no). Here, while the answer is "wait", the tick sleeps a millisecond, runs the tick's own pump and asks
// again, up to p2p_hold_poll_ms; a packet late by 3 ms would then cost 3 ms, not 17.
//
// Off by default (0): in the 13 Nov 2026 rounds on the private loopback (80 ms + 20 jitter + 2% loss, 150 ms +
// 40 + 5%, 10 ms of polling) every hold polled and none was rescued - 0 of 117 without the pump, 0 of 79 with
// it; hold counts and press-to-applied times were the same as without polling (80 ms link: 32 / 34 holds,
// 120 ms, as 31 / 32 and 120 ms). The inputs that hold a frame on such links are not a few milliseconds late:
// with inputs sent every frame, a lost packet's inputs come with the next frame's packet, 17 ms later at best,
// and a jitter spike runs for frames. The hook stays as the diagnostic (SVR2011 --p2p_hold_poll_ms=10 says how
// late the late inputs are) and for a link where the holds are near misses.
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

// Off by default (13 Nov 2026): on simulated 80 ms + 20 jitter and 150 ms + 40 jitter links the poll fired on
// every hold but no input came within the 10 ms (late packets there are late by more than a frame: jitter and a
// lost packet's repeat), so it bought nothing; the holds that remain are what the lead's margin is for.
REXCVAR_DEFINE_UINT32(p2p_hold_poll_ms, 0, "Online",
                      "Milliseconds an online match's tick keeps asking for a late input (every ms) before it holds "
                      "the frame (0: a late input holds a whole frame, as the game does; up to 15)");

namespace {

using Clock = std::chrono::steady_clock;

constexpr uint32_t kTickAsks = 0x8289510C;  // (the tick's "can this frame run" ask, before its pump)
constexpr uint32_t kNetObject = 0x82EDE914;  // (the match net object the tick runs on: net_stats.cpp)

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

uint64_t g_polled = 0, g_rescued = 0, g_waited_us = 0, g_gave_up_us = 0;  // (the match thread only)
uint32_t g_dry = 0, g_rest = 0;
Clock::time_point g_report{};

}  // namespace

REX_EXTERN(__imp__sub_8288D3B8);
REX_HOOK_RAW(sub_8288D3B8) {
  const uint32_t arg = ctx.r3.u32;
  const uint32_t from = uint32_t(ctx.lr);
  __imp__sub_8288D3B8(ctx, base);
  if (from != kTickAsks || ctx.r3.u32 == 0) return;
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
  const uint32_t net = Rd32(base + kNetObject);
  while (Clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (net) {
      ctx.r3.u64 = net;
      sub_82893080(ctx, base);  // (the pump: the socket's packets into the session; the tick's own, safe to repeat)
    }
    ctx.r3.u64 = arg;
    __imp__sub_8288D3B8(ctx, base);
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
