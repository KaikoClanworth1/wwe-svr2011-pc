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

// Backstage mods with an own menu row (manifest row=<label>): their bg78 plays
// only in matches from that row. area: the room (0 parking lot ... 6 catering).
struct BackstageRow {
  std::string label, file;
  std::string gimmick;  // its gm.pac group (GM..) or entry played for the room's GMGB package ("" = the room's)
  int area = -1;
};
const std::vector<BackstageRow>& BackstageRows();
// The bg78 of row i for the next match (-1: the usual one).
void UseBackstageRow(int i);
extern int g_active_row;  // (the row in use, -1 none)

}  // namespace svr2011
