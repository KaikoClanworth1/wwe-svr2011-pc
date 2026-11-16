// CREATE A MOVE-SET: the move packs' own moves as a "MOD:" group, and a moveset naming a move whose pack
// is gone (cam_mod_moves.cpp, docs/CAM_MOD_MOVES.md).
#pragma once

namespace rex::memory {
class Memory;
}

#include <cstdint>

namespace svr2011 {
void InstallCamModMoves(rex::memory::Memory* memory);
// Whether the match's motion banks (*0x82E3CC40 / *0x82E3C108) have motion `id` at any x / y; no bank lists (a menu):
// true. The missing-motion guard (match_types.cpp) and the moves' fallback ask.
bool MotionInMatchBanks(uint8_t* base, uint16_t id);
}
