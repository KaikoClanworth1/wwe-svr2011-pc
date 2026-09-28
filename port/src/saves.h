// WWE SmackDown vs. Raw 2011 - saves as plain files in the game folder's
// "Saves" folder (one file per save: SaveData.dat, 00CreateSuperStar.cas,
// 00PaintTool.pt, ...), so the player and the launcher can manage them.
// The runtime mounts them for the game (ContentManager::SetFlatSavesRoot).

#pragma once

#include <filesystem>

namespace rex::system {
class KernelState;
}

namespace svr2011 {

// Moves saves from the runtime's per-profile folders
// (<user data>\<profile>\5451085D\00000001\<save>\SaveData.Dat) into
// `saves`, then has the runtime keep them there. Before the game starts.
void UseFlatSaves(rex::system::KernelState* kernel_state, const std::filesystem::path& user_data,
                  const std::filesystem::path& saves);

}  // namespace svr2011
