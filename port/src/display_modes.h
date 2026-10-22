// WWE SmackDown vs. Raw 2011 - the window sizes the screen offers (display_modes.cpp).
#pragma once

#include <vector>

namespace svr2011 {

struct DisplaySize {
  int w = 0, h = 0;
};

// The sizes the display `hwnd` is on reports to Windows (EnumDisplaySettings:
// distinct, at least 854 x 480 and at most the desktop's), with the desktop's
// own size; smallest first. Empty: use a fixed list (not Windows, Proton,
// fewer than two sizes). Test aid: SVR2011_TEST_DISPLAY_MODES=7680x4320,
// 5120x2880,... - that list (the largest is the desktop's).
std::vector<DisplaySize> DisplaySizes(void* hwnd);

}  // namespace svr2011
