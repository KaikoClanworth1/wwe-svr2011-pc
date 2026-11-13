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

// Test aid SVR2011_TEST_HEAP_LOG=<heap>: the heaps' delete (frame_rate.cpp sub_8269B2D0) was asked to free ptr - the
// watched heap's live blocks (allocated, not yet freed) are logged every 10 s by the caller that asked for them.
void HeapWatchFree(uint32_t ptr);
}  // namespace svr2011
