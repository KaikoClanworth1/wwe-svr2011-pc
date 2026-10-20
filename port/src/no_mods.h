// Start Without Mods (the launcher's button: SVR2011_NO_MODS=1 in the game's environment): this run loads no
// mods at all and leaves everything of theirs as it is - Mods\ (slots.txt, limit.txt, left_out.txt, the overlays),
// Saves\.universe\mods.bin, the saves' mod superstar profiles. Each mod loader returns before it reads or writes
// anything; what keeps saves made with mods working stays on (the Universe side table with no mods: never
// written; the moves' match fallback from the stock tables; Create An Entrance's MISSING MOD rows).
// The restored match types (Mods\MatchTypes) are the game's own and stay as set.
#pragma once

#include <cstdlib>

namespace svr2011 {

inline bool NoMods() {
  static const bool on = [] {
    const char* v = std::getenv("SVR2011_NO_MODS");
    return v && v[0] == '1';
  }();
  return on;
}

}  // namespace svr2011
