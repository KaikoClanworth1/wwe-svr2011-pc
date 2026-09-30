// WWE SmackDown vs. Raw 2011 - the ACHIEVEMENTS page (MY WWE -> ACHIEVEMENTS,
// a menu entry the port adds: tools/patch_menu.py, menu_hooks.cpp).
//
// An in-game page drawn over the menu, in the style of the game's panels: the
// game's 39 achievements (from its own title data) with their icons, which are
// unlocked (and when), their gamerscore and how to earn each. The runtime
// records unlocks as the game reports them (UserData/achievements). Driven by
// the controller (D-pad / left stick: choose, LB / RB: page, Y: show all /
// unlocked / locked, B: back) or the keyboard (arrows, Page Up / Down, Tab,
// Esc). While it is open the game sees an idle controller.

#pragma once

struct ImFont;

namespace rex {
class Runtime;
}
namespace rex::ui {
class ImGuiDrawer;
class ImmediateDrawer;
}  // namespace rex::ui
namespace rex::input {
class InputSystem;
}

namespace svr2011 {

// Once, when the dialogs are created. `input` may be null.
void InstallAchievementsPage(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* immediate,
                             rex::Runtime* runtime, rex::input::InputSystem* input);

// Fonts for the page (null: ImGui's default).
void SetAchievementsPageFonts(ImFont* menu, ImFont* title);

// Opens the page (any thread; the guest's menu hook).
void OpenAchievementsPage();

// True while the page is open or the button that closed it is still held
// (the game then sees an idle controller).
bool AchievementsPageHoldsInput();

}  // namespace svr2011
