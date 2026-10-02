// Arena mods (arenas branch): custom arenas served in place of a host arena.
// Plan and findings: docs/ARENA_MOD_MAKER_PLAN.md.
#pragma once

#include <string>

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

}  // namespace svr2011
