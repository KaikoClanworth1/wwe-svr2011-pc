// More exhibition match types: rows added to the PLAY menus for rule
// records the game has but doesn't offer (and new ones). Research and plan:
// docs/MATCH_TYPES_RESEARCH.md.
#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallMatchTypes(rex::memory::Memory* memory);

// Each world update (frame_rate.cpp): the Lumberjack match's lumberjacks
// (match_types.cpp: their controller).
void MatchTypesUpdate(uint8_t* base);

}  // namespace svr2011
