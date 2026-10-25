// WWE SmackDown vs. Raw 2011 - custom icons for the on-screen touch controller
// (touch_controls.h).
//
// A control can show a picture of the player's own instead of the drawn
// button: a PNG in <UserData>/touch_icons (transparent PNGs work best). The
// touch controller's editor picks the picture (ICON < name >); touch_layout.txt
// keeps the choice as an `icon <layout> <control id> <file name>` line, which
// older versions of the port simply skip.
//
// Pictures are loaded once, on the UI thread, when a control first draws
// them, and are kept for the rest of the run (so a changed file shows after a
// restart). A picture that is missing, too big or not a PNG leaves the control
// drawn as usual.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rex::ui {
class ImmediateDrawer;
class ImmediateTexture;
}  // namespace rex::ui

namespace svr2011 {

// Once: the drawer that makes the textures (null: no icons) and the user-data
// folder; <user_data>/touch_icons is created if it is not there.
void TouchIconsInit(rex::ui::ImmediateDrawer* drawer, const std::filesystem::path& user_data);

// The icons folder (empty before TouchIconsInit).
const std::filesystem::path& TouchIconsFolder();

// True for a name a layout file may hold: a plain file name ending in .png,
// with no folders, no "..", no drive and no control characters.
bool TouchIconNameValid(const std::string& name);

// The PNG files in the icons folder (valid names only, sorted, UTF-8).
std::vector<std::string> TouchIconList();

struct TouchIcon {
  rex::ui::ImmediateTexture* texture = nullptr;  // owned by the cache; never freed
  float width = 0, height = 0;                   // pixels
};

// UI thread. The loaded picture called `name`; false if there is none.
bool TouchIconGet(const std::string& name, TouchIcon* out);

// Forget which pictures failed to load, so the next draw tries them again
// (the editor calls it when it opens: a file may have been added).
void TouchIconsRetry();

}  // namespace svr2011
