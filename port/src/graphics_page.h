// WWE SmackDown vs. Raw 2011 - the GRAPHICS page (MY WWE -> OPTIONS ->
// GRAPHICS, a menu entry the port adds: tools/patch_menu.py, menu_hooks.cpp).
//
// An in-game page drawn over the menu: resolution, anti-aliasing, FPS counter,
// VSync, fullscreen / windowed and the renderer. Driven by the controller
// (D-pad / left stick: choose and change, A: change, B: back) or the keyboard
// (arrows, Enter, Esc). While it is open the game sees an idle controller.
// Changes are saved to the settings file (svr2011.toml, shared with the
// launcher); the FPS counter, VSync and fullscreen apply at once, the rest the
// next time the game starts.

#pragma once

#include <filesystem>

struct ImFont;

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace rex::input {
class InputSystem;
}

namespace svr2011 {

// Once, when the dialogs are created. `input` may be null (no input hold).
void InstallGraphicsPage(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
                         rex::input::InputSystem* input,
                         const std::filesystem::path& config_path);

// Fonts for the page (from the app's font setup; null: ImGui's default).
void SetGraphicsPageFonts(ImFont* menu, ImFont* title);

// Opens the page (any thread; the guest's menu hook).
void OpenGraphicsPage();

// Closes the game as the window's close button does (any thread).
void RequestExit();

}  // namespace svr2011
