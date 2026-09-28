// WWE SmackDown vs. Raw 2011 - ReXGlue recompiled PC port
//
// The program sits in the game folder beside the disc files (default.xex,
// pac\, sound\, movies\ ...), so that folder is the game root. Saves, the
// shader cache and logs go in UserData\ there too, which keeps an install
// self-contained. Settings come from svr2011.toml, written by the launcher.

#pragma once

#include <rex/rex_app.h>

class Svr2011App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Svr2011App>(new Svr2011App(ctx, "svr2011",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnPostLoadXexImage() override;
};
