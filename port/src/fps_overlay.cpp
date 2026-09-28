// WWE SmackDown vs. Raw 2011 - on-screen FPS counter (see fps_overlay.h).

#include "fps_overlay.h"

#include <atomic>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>

#include "frame_stats.h"
#include "native/native_renderer.h"

REXCVAR_DEFINE_BOOL(show_fps, true, "UI", "Show the frame rate counter (toggle in game: F2)");

namespace svr2011 {

namespace {
FpsOverlay* g_overlay = nullptr;     // lives for the whole run
std::atomic<bool> g_visible{false};
}

void FpsOverlay::OnDraw(ImGuiIO& io) {
  if (!g_visible.load(std::memory_order_relaxed)) return;
  const FrameTiming t = GetFrameTiming();
  // Top centre of the window.
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 6.0f), ImGuiCond_Always,
                          ImVec2(0.5f, 0.0f));
  ImGui::SetNextWindowBgAlpha(0.45f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("##svr2011_fps", nullptr, flags)) {
    ImGui::SetWindowFontScale(1.5f);
    const ImVec4 colour = t.fps >= 59.0   ? ImVec4(0.45f, 1.0f, 0.45f, 1.0f)
                          : t.fps >= 45.0 ? ImVec4(1.0f, 0.85f, 0.3f, 1.0f)
                                          : ImVec4(1.0f, 0.4f, 0.4f, 1.0f);
    ImGui::TextColored(colour, "%.0f FPS", t.fps);
    ImGui::SameLine();
    ImGui::TextDisabled("%.1f ms  1%% low %.0f", t.frame_ms, t.low_1pct_fps);
#ifndef SVR2011_D3D_TRACE  // the census build has no native renderer
    ImGui::SameLine();
    ImGui::TextDisabled("|  %s", native::RendererLabel().c_str());
#endif
  }
  ImGui::End();
}

void InstallFpsOverlay(rex::ui::ImGuiDrawer* drawer) {
  g_visible = REXCVAR_GET(show_fps);
  g_overlay = new FpsOverlay(drawer);
  rex::ui::RegisterBind("bind_fps_counter", "F2", "Toggle frame rate counter",
                        [] { SetFpsCounterVisible(!FpsCounterVisible()); });
}

void SetFpsCounterVisible(bool visible) { g_visible = visible; }

bool FpsCounterVisible() { return g_visible; }

}  // namespace svr2011
