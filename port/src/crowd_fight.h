// Crowd fighting (SvR 2008's): in an Extreme Rules match on an arena ported from SvR 2008, the fight can go
// through the open back barricade into the crowd area and up its steps. See crowd_fight.cpp.
#pragma once

#include <cstdint>

namespace svr2011 {

// This match plays with the crowd area open (decided when its stage loads).
bool CrowdMatch();
// The match about to load opens the crowd area (its rule Extreme Rules, its arena a crowd one - CrowdCollisionId's
// rule), read from the live settings: usable before the stage loads (the weapons are made first).
bool CrowdAreaFor(uint8_t* base);

// The stage loader's collision look-up (sub_826A0D98 called from sub_82187258): the id to ask for -
// SvR 2008's crowd collision (999) in place of the match's (996) in a crowd match.
uint32_t CrowdCollisionId(uint8_t* base, uint32_t lr, uint32_t id);

}  // namespace svr2011
