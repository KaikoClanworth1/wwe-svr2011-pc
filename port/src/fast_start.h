// WWE SmackDown vs. Raw 2011 - a faster start (fast_start.cpp): cvars
// skip_intros (no logos / intro movie at launch: straight to the Start
// Screen) and skip_training (no practice ring after it: straight to the main
// menu), both off by default; the PC launcher's Play tab, the Android app's
// settings and GRAPHICS -> DISPLAY set them.

#pragma once

#include <cstdint>

namespace svr2011 {

// A press-START screen's frame / its START taken (pad_types.cpp's
// sub_8258B5F8 hook).
void FastStartScreenFrame(uint8_t* base, uint32_t screen);
void FastStartPressStart(uint8_t* base, uint32_t screen, uint32_t pad);

// Player 1's pad was just read (frame_rate.cpp's XamInputGetState hook: an
// X_INPUT_STATE, +0 packet number, +4 buttons): the skips press START.
void FastStartInput(uint8_t* base, uint32_t state);

}  // namespace svr2011
