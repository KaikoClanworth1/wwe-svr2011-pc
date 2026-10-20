// WWE SmackDown vs. Raw 2011 - the performance overlay (and the FPS counter it grew from).
//
// In a corner of the window (perf_overlay_corner), at a level (perf_overlay; launcher: Settings; in game: MY WWE ->
// OPTIONS -> GRAPHICS, PERFORMANCE; F2 cycles it): off, the frame rate (FPS, frame time, 1% low - frame_stats.h), or
// full - with the game's own GPU memory (its textures and render targets; native_renderer.h LastFrameCosts). The
// system's numbers (RAM, CPU, the driver's VRAM budget) are in the perf: log lines only (perf_stats.h).

#pragma once

#include <string>

#include <rex/ui/imgui_dialog.h>

namespace svr2011 {

class FpsOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override;
};

// Creates the counter (shown if show_fps is set) and registers the F2 bind.
void InstallFpsOverlay(rex::ui::ImGuiDrawer* drawer);

// Shows or hides the counter (any thread).
void SetFpsCounterVisible(bool visible);
bool FpsCounterVisible();
// The overlay's level: 0 off, 1 the frame rate, 2 everything; its corner: 0 top left, 1 top right, 2 bottom left,
// 3 bottom right (any thread).
void SetPerfOverlayLevel(int level);
int PerfOverlayLevel();
void SetPerfOverlayCorner(int corner);
int PerfOverlayCorner();

// A short notice at the bottom centre for `seconds` (any thread), e.g. a save
// the port had to cancel.
void ShowNotice(const std::string& text, float seconds);

}  // namespace svr2011
