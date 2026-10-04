// Move packs (arenas branch): moves the game doesn't have (ported from
// SvR 2010) added from mods, without touching the game's files.
// docs/MOVE_PACKS.md.
#pragma once

#include <string>

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace svr2011 {

// At start-up, before the game mounts its pacs: merges every enabled move
// pack (Mods/Superstars/<mod>/moves/pack.txt, Mods/Moves/<pack>/pack.txt)
// into copies of m.pac, misc.pac and mpsp.pac in <game>/Mods/PacOverlay
// (rebuilt only when a pack or a game file changed), with a pac list naming
// them there.
void InstallMovePacks(rex::filesystem::VirtualFileSystem* fs);

// The folder (under the game folder) the game reads its pac list from, or ""
// for the game's own. With an overlay the pacs are mounted from their own
// tables (the pre-built directory plist360.arc only knows the originals).
const std::string& PacListFolder();

}  // namespace svr2011
