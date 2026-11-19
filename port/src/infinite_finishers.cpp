// WWE SmackDown vs. Raw 2011 - INFINITE FINISHERS (suggestion 9beda8, as in WWE 2K14): a match option where a
// finisher doesn't use the momentum meter up - with the meter full, every wrestler (player or CPU) can keep doing
// finishers. OFF by default (OPTIONS -> CPU tab, the PC launcher's Settings, Android's settings), and always off in
// online and LAN matches (both sides would have to agree; the lockstep needs the same rules on both).
//
// The meter: the character's stats object (character +2572) - floats at +144 / +148 (sub_821F4D90 and
// sub_821F4FA0 add the momentum gain, sub_821F1C50 sets +148; cpu_difficulty.cpp hooks sub_821F4D90 for the CPU
// tab's momentum level).
#include "infinite_finishers.h"

#include <rex/cvar.h>

#include "p2p.h"

REXCVAR_DEFINE_BOOL(infinite_finishers, false, "Gameplay",
                    "INFINITE FINISHERS: a finisher doesn't use the momentum meter up (offline matches only)");

namespace svr2011 {

bool InfiniteFinishersOn() {
  if (!REXCVAR_GET(infinite_finishers)) return false;
  return !P2POnlineMode() && !P2PMatchLobby().in;
}

}  // namespace svr2011
