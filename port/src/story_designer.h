// WWE SmackDown vs. Raw 2011 - Story Designer rules and keys (story_designer.cpp).
#pragma once

namespace rex::ui {
class ImGuiDrawer;
}

namespace svr2011 {

// A Story Designer list (moments, shows, moment groups) is on screen.
bool StoryListActive();
// Ctrl+C (false) / Ctrl+V (true) for that list: done on its next frame.
void StoryListKey(bool paste);
// The short notice those keys show.
void InstallStoryDesignerOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace svr2011
