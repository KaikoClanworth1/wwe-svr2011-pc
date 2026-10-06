// Arena mods (arenas branch): custom arenas served in place of a host arena.
// Plan and findings: docs/ARENA_MOD_MAKER_PLAN.md.
#pragma once

#include <string>
#include <vector>

namespace rex::memory {
class Memory;
}
namespace rex::filesystem {
class VirtualFileSystem;
}
namespace rex::ui {
class ImGuiDrawer;
}

namespace svr2011 {

void InstallArenaMods(rex::memory::Memory* memory, rex::filesystem::VirtualFileSystem* fs);

// The arena select page label (an ImGui overlay).
void InstallArenaModsOverlay(rex::ui::ImGuiDrawer* drawer);

// Serve <game folder>/<relative_file> whenever the game opens arena BGnn
// (empty: back to the original). Takes effect at the next open (arenas are
// opened when they load, not held open).
void RedirectArena(int arena, const std::string& relative_file);
// The file arena BGnn plays when no custom arena is on its tile (a media
// mod's arena with its own screen pictures); RedirectArena(arena, "") goes
// back to it.
void SetArenaDefault(int arena, const std::string& relative_file);
// A match is loading (set-up to its people placed): a custom arena's loading
// pictures are swapped only then.
void SetMatchLoading(bool loading);
// A player's pad buttons as the game read them (XInput bits): LB / RB turn
// the arena select page.
void ArenaSelectPad(uint32_t user, uint16_t buttons);

// Backstage mods with an own menu row (manifest row=<label>): their bg78 plays
// only in matches from that row. area: the room (0 parking lot ... 6 catering).
struct BackstageRow {
  std::string label, file;
  std::string gimmick;  // its GMGB entry (gimmick.pac) played for the room's package ("" = the room's)
  // box=<x>,<z>,<half x>,<half z>: the fight box (where the cameras look and
  // the fighters are kept), when the area isn't where the room's was
  bool has_box = false;
  float box[4] = {};
  float camera = 0;  // camera=<units>: the match camera's farthest distance (0 = the game's)
  float camera_height = 0;  // camera_height=<units>: the match camera at least that high (0 = the game's)
  int area = -1;
};
const std::vector<BackstageRow>& BackstageRows();
// The bg78 of row i for the next match (-1: the usual one).
void UseBackstageRow(int i);
extern int g_active_row;  // (the row in use, -1 none)

}  // namespace svr2011
