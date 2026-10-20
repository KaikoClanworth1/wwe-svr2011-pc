// Arena lighting options: ARENA BRIGHTNESS (GRAPHICS, launcher) and an arena
// mod's own light keys, applied to the arena's light bank as the game unpacks
// it (entry 0x7530: the light sets - see arena_lights.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace svr2011 {

// The light bank the BPE decoder just unpacked, if `d` is one (arena_mods.cpp's
// hook of sub_826AEF10). `manifest`: the chosen arena mod's manifest text
// (empty: a game arena). Keys:
//   light_scale=<0.25-4>          this arena's lights x that (with ARENA BRIGHTNESS)
//   match_light=entrance          the match lit as the entrance (sets 10-14 take
//                                 set 200's light, its coloured key light left out)
//   match_light=entrance_spot     ... with the key light
void ArenaLightsDecoded(uint8_t* d, uint32_t size, const std::string& manifest);

// ARENA BRIGHTNESS, percent (50-200; 100 = the game's own): from the next arena load.
int ArenaBrightness();
void SetArenaBrightness(int percent);

}  // namespace svr2011
