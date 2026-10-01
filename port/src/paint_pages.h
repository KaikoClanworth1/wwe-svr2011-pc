// Paint Tool pages: 10 pages of 20 logos instead of the game's 20.
//
// The game only ever sees its normal 20-slot Paint Tool file; the port decides
// which page of 20 that is. Page 1 is the game's own Saves\00PaintTool.pt
// (untouched, so old saves, the launcher and online keep working); pages 2-10
// live in Saves\.paint\pNN_sMM.bin, one file per used slot (the raw 0x604CC-
// byte slot). On pages 2-10 the Paint Tool storage jobs (open / read / write /
// close) are done here instead of on the file, and reported to the game as
// done. LB / RB in the Paint Tool grid switch pages. Created Superstars keep
// their own copy of each logo, so logos on any page stay on them.
// Plan and findings: docs/PAINT_TOOL_PAGES_PLAN.md.
#pragma once

#include <filesystem>

namespace rex::input {
class InputSystem;
}
namespace rex::memory {
class Memory;
}
namespace rex::ui {
class ImGuiDrawer;
}

namespace svr2011 {

constexpr int kPaintPages = 10;

void InstallPaintPages(rex::memory::Memory* memory, rex::input::InputSystem* input,
                       const std::filesystem::path& saves);

// The page label over the Paint Tool grid ("LB PAGE 3 / 10 RB").
void InstallPaintPagesOverlay(rex::ui::ImGuiDrawer* drawer);

// The page the game sees, 0-based.
int PaintPage();

}  // namespace svr2011
