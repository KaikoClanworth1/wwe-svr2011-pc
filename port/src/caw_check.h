// Problem-report diagnostics: the Created Superstar saves at start, one line each (caw_check.cpp).
#pragma once

#include <filesystem>

namespace svr2011 {

// Logs each NNCreateSuperStar.cas in `saves`: size, save slot, the attires with a picture, and whether its
// checksums agree with each other and with SaveData.dat's record for its slot (a clear warning when not).
void CheckCawSaves(const std::filesystem::path& saves);

}  // namespace svr2011
