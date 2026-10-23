// VS screen styles: older SmackDown vs. Raw games' VS screens (SvR 2008 / 2009 / 2010), made from the player's
// own disc by tools/vs_port.py into <game>/Mods/VsStyles/<style>/ (manifest.txt type=VsStyle, vs.pac). See
// vs_styles.cpp and docs/VS_STYLES.md.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// Before the superstar mods mount their overlay pacs: the installed styles merged into one (vsstyles.pac).
void InstallVsStyles(rex::memory::Memory* memory);

// The installed styles: {folder name (the vs_style setting's value), name to show}.
const std::vector<std::pair<std::string, std::string>>& VsStyles();

// The virtual file the game asked for, if the chosen style has it (a VS theme's /MENU/M<nn>L, I or F): the
// guest address of the style's name for it (superstar_mods.cpp's resolver hook) - else 0.
uint32_t VsStyleAlias(const char* name);

}  // namespace svr2011
