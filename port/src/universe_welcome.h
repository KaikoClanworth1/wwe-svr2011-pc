// The WWE Universe welcome screen kept as seen once accepted (universe_welcome.cpp): its save flag goes into
// SaveData.dat at once, not only with the game's next save.
#pragma once

#include <cstdint>
#include <filesystem>

namespace svr2011 {

void InstallUniverseWelcome(const std::filesystem::path& saves, const std::filesystem::path& user_data);
// Once a frame (CheckModProfiles).
void UniverseWelcomeTick(uint8_t* base);

}  // namespace svr2011
