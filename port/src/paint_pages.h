// Paint Tool pages: 10 pages of 20 logos instead of the game's 20.
//
// The game only ever sees its normal 20-slot Paint Tool file; the port decides
// which page of 20 that is. Page 1 is the game's own Saves\00PaintTool.pt
// (untouched, so old saves, the launcher and online keep working); pages 2-10
// live in Saves\.paint\pNN_sMM.bin, one file per used slot (the raw 0x604CC-
// byte slot). On pages 2-10 the Paint Tool storage jobs (open / read / write /
// close) are done here instead of on the file, and reported to the game as
// done. LB / RB switch pages in the Paint Tool grid and the Created Superstar
// logo picker; Community Creations lists all 200 slots. Created Superstars
// keep their own copy of each logo, so logos on any page stay on them.
// Plan and findings: docs/PAINT_TOOL_PAGES_PLAN.md.
#pragma once

#include <cstdint>
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

// Community Creations: called when a downloaded Paint Tool logo is about to be
// written into its slot (sub_82518268, the only hook on it), after the chosen
// page was loaded into the screen's file buffer. `screen` is the slot list
// (vtable 0x82027C20): +268 the downloaded 0x604CC-byte slot, +272 the file
// buffer (holding `page`), +0x8344 the slot number 0-199 - slot % 20 on
// `page` (0-based; 0 = 00PaintTool.pt, else Saves\.paint\pNN_sMM.bin).
// Returning false skips the game's write - the callback then has to leave
// the screen in a state the game can go on from. Set from the game's thread before Community Creations runs (online.h).
using PaintDownloadCheck = bool (*)(uint8_t* base, uint32_t screen, int page);
void SetPaintDownloadCheck(PaintDownloadCheck check);

}  // namespace svr2011
