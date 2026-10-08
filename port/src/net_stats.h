// WWE SmackDown vs. Raw 2011 - online match network statistics: how long the
// game's lockstep waited for other players' inputs (net_stats.cpp).
#pragma once

#include <cstdint>

namespace svr2011 {

struct NetStats {
  bool active = false;      // an online match is stepping (lockstep frames lately)
  float waited_pct = 0;     // frames spent waiting for inputs, last 5 s
  float longest_ms = 0;     // the longest wait, last 5 s
  uint32_t waits = 0;       // how many waits, last 5 s
};

NetStats NetStatsNow();

}  // namespace svr2011
