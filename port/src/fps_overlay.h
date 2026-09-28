// WWE SmackDown vs. Raw 2011 - on-screen FPS counter.
//
// A small readout at the top of the window of the game's own frame rate
// (frame_stats.h): FPS, frame time and the 1% low over the last second. Shown
// when the show_fps setting is on (launcher: Settings; in game: MY WWE ->
// OPTIONS -> GRAPHICS), toggled with F2.

#pragma once

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

}  // namespace svr2011
