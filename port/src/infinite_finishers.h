#pragma once

#include <cstdint>

namespace svr2011 {

// INFINITE FINISHERS (infinite_finishers.cpp): the setting is on and the match is offline.
bool InfiniteFinishersOn();
// The meter's writers (cpu_difficulty.cpp's hooks): a match's start set it / something wrote it (kept full once it was).
void InfiniteFinishersMeterSet(uint32_t stats);
void InfiniteFinishersMeterWritten(uint8_t* base, uint32_t stats);

}  // namespace svr2011
