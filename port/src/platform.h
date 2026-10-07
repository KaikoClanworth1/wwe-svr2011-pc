// WWE SmackDown vs. Raw 2011 - what the game runs on: Windows, or Wine/Proton
// on Linux (the Steam Deck). Logged at startup so reports say where they came
// from.

#pragma once

#include <cstdint>
#include <string>

namespace svr2011 {

// Wine or Proton (ntdll exports wine_get_version).
bool IsWine();

// A Steam Deck (Steam sets SteamDeck=1 for what it starts there).
bool IsSteamDeck();

// "Windows 10.0.26200", "Wine 9.0 on Linux 6.5.0 (Steam Deck)", ...
std::string PlatformDescription();
// Windows' build number (19041 = 10 version 2004); 0 off Windows and under Wine.
uint32_t WindowsBuild();

}  // namespace svr2011
