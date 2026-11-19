// WWE SmackDown vs. Raw 2011 - INFINITE FINISHERS (suggestion 9beda8, as in WWE 2K14): a match option where a
// finisher doesn't use the momentum meter up - with the meter full, every wrestler (player or CPU) can keep doing
// finishers. OFF by default (OPTIONS -> CPU tab, the PC launcher's Settings, Android's settings), and always off in
// online and LAN matches (both sides would have to agree; the lockstep needs the same rules on both).
//
// The meter: the character's stats object (character +2572) +144, a float from -1 to 1 (full: 1). +148 is the
// gain waiting to be added (sub_821F4D90 / sub_821F4FA0 add to it). Its writers: sub_821F5370 (adds a delta and
// clamps - every update, from sub_821F6158), sub_821F3010 (puts it back to a constant), sub_821F1CC0 (sets it - at
// a match's start, from sub_825B3470). Their hooks are in cpu_difficulty.cpp. As in WWE 2K14: once a wrestler's
// meter has been full, it stays full for the rest of the match - whatever spends or drains it is put back.
#include "infinite_finishers.h"

#include <mutex>
#include <set>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "p2p.h"
#include "../modmaker/svrfmt/bytes.h"

REXCVAR_DEFINE_BOOL(infinite_finishers, false, "Gameplay",
                    "INFINITE FINISHERS: a finisher doesn't use the momentum meter up (offline matches only)");

namespace svr2011 {

bool InfiniteFinishersOn() {
  if (!REXCVAR_GET(infinite_finishers)) return false;
  return !P2POnlineMode() && !P2PMatchLobby().in;
}

namespace {
std::mutex g_mutex;
std::set<uint32_t> g_full;  // stats objects whose meter has been full this match
}  // namespace

void InfiniteFinishersMeterSet(uint32_t stats) {
  std::lock_guard lock(g_mutex);
  g_full.erase(stats);  // (a match's start sets the meter: a new match)
}

void InfiniteFinishersMeterWritten(uint8_t* base, uint32_t stats) {
  if (!stats || !InfiniteFinishersOn()) return;
  const float m = svrfmt::RdF(base + stats + 144);
  std::lock_guard lock(g_mutex);
  if (m >= 1.0f) {
    if (g_full.insert(stats).second) REXLOG_INFO("[svr2011] infinite finishers: stats {:08X} full - stays full this match", stats);
    return;
  }
  if (!g_full.count(stats)) return;
  svrfmt::WrF(base + stats + 144, 1.0f);
  static uint32_t said = 0;
  if (said++ < 20) REXLOG_INFO("[svr2011] infinite finishers: stats {:08X} meter {:.3f} put back to full", stats, m);
}

}  // namespace svr2011
