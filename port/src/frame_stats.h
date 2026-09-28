// WWE SmackDown vs. Raw 2011 - guest frame timing.
//
// Every frame the game presents goes through one function (its D3D Present,
// the only caller of VdSwap), which is hooked to time frames. The result
// feeds the F3 overlay's "Guest FPS" line and, every few seconds, the log
// ("fps: ..."), which the automated tests read.

#pragma once

#include <rex/ui/overlay/debug_overlay.h>

namespace svr2011 {

rex::ui::FrameStats GetFrameStats();

// Over the last second of presented frames.
struct FrameTiming {
  double fps = 0;
  double frame_ms = 0;
  double low_1pct_fps = 0;
};
FrameTiming GetFrameTiming();

}  // namespace svr2011
