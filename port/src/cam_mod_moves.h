// CREATE A MOVE-SET: the move packs' own moves as a "MOD:" group, and a moveset naming a move whose pack
// is gone (cam_mod_moves.cpp, docs/CAM_MOD_MOVES.md).
#pragma once

namespace rex::memory {
class Memory;
}

namespace svr2011 {
void InstallCamModMoves(rex::memory::Memory* memory);
}
