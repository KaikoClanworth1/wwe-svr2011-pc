// WWE SmackDown vs. Raw 2011 - Road to WrestleMania's Streak story played as a superstar mod (rtwm_streak.cpp).
#pragma once

#include <cstdint>

namespace svr2011 {

// The main menu's last choice was Road to WrestleMania (menu_hooks.cpp).
bool InRoadToWrestleMania();
// A list block was built (sort_filters.cpp): `on` - of Create-a-Superstar slots (the Streak's), its first CAW what the
// story gets for a mod; off - a roster list (picks there aren't the Streak's).
void RtwmCasList(uint32_t first_caw, bool on);
// A match's people are built (test_match.cpp's hook): in Road to WrestleMania, the CAW the mod stands in for becomes
// the mod.
void RtwmStreakPeople(uint8_t* base, uint32_t match);

}  // namespace svr2011
