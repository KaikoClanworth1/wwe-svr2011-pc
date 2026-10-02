// Ring Kit, game side (arenas branch): the ring settings a custom arena's
// manifest asks for (ring.* keys, written by the Mod Maker), applied to the
// game's ring constants while that arena is the one being played.
// Findings: docs/ARENA_MOD_MAKER_PLAN.md ("Ring code").
#pragma once

#include <string>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallRingRules(rex::memory::Memory* memory);

// The manifest text of the custom arena that will be played (empty: none,
// back to the game's ring).
void SetRingRules(const std::string& manifest);

}  // namespace svr2011
