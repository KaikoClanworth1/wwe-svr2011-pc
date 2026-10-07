// WWE SmackDown vs. Raw 2011 - MY WWE -> OPTIONS -> BACKGROUNDS: which of
// the main menu's 12 background pictures it cycles through (a menu entry the
// port adds: tools/patch_menu.py, game_files.cpp, menu_hooks.cpp).
//
// The main menu shows one picture (menuHD.pac MENU/EB00-EB11) and moves on to
// the next each time the player leaves it (docs/MENU_BACKGROUNDS.md). The page
// turns pictures on and off (cvar menu_backgrounds_off); the cycle then skips
// the ones that are off. With every picture off all of them play (never a
// blank menu). Controller (D-pad / stick: choose, A: on / off, Y: all on /
// all off, B: back), keyboard (arrows, Enter / Space, Esc) or touch (tap a
// row). While it is open the game sees an idle controller.

#pragma once

#include <cstdint>

struct ImFont;

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class ImGuiDrawer;
}
namespace rex::input {
class InputSystem;
}

namespace svr2011 {

void InstallBackgroundsPage(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input);
void SetBackgroundsPageFonts(ImFont* menu, ImFont* title);
void OpenBackgroundsPage();
bool BackgroundsPageHoldsInput();

}  // namespace svr2011
