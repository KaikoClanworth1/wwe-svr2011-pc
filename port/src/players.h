// More local players: pads 5-8 (src/players.cpp).
#pragma once

#include <cstdint>

namespace svr2011 {

// While the game polls pads 5-8 (its pad reader run again over them): the
// XamInputGetState user index to add (frame_rate.cpp), else 0.
uint32_t PadUserOffset();

}  // namespace svr2011
