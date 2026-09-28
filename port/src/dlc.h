// WWE SmackDown vs. Raw 2011 - DLC installation.
//
// Downloadable content packages (STFS, as downloaded on the Xbox 360) placed
// in "Game Files\DLC" - the launcher's "Install DLC" copies them there - are
// unpacked at startup into UserData (0000000000000000\<title>\00000002\), where
// the game finds them like content on the console's hard drive. Packages
// already unpacked are skipped.

#pragma once

#include <filesystem>

namespace rex::system {
class KernelState;
}

namespace svr2011 {

// Rewrites a string.pac's console wording for the PC (in place, once).
void PatchOnlineStrings(const std::filesystem::path& file);

void InstallDlc(rex::system::KernelState* kernel_state, const std::filesystem::path& dlc_dir,
                const std::filesystem::path& user_data_root);

}  // namespace svr2011
