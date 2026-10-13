// Move packs (arenas branch): moves the game doesn't have (ported from
// SvR 2010) added from mods, without touching the game's files.
// docs/MOVE_PACKS.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

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

// Can a moveset name this move: 1 if the game's move records (the last
// MOVS/WAZE loaded, the packs' moves in) have it with category bits, 0 if not
// (no record, or one 2011 kept for a cut move: no motion either), -1 before
// the records are loaded.
int MoveUsable(uint16_t id);

// The entrances the move packs add (pacentry EVP? <n>, n < 10000), for CREATE AN ENTRANCE's MODS
// rows (cae_mod_motions.cpp): label = the superstar mod's name, else the script's own name and the
// game it was converted from; parts: bit k set when CAE part 10000 + 1000k + n is in the pack too.
struct ModEntrance {
  int number = 0;
  std::string label;
  uint8_t parts = 0;
};
std::vector<ModEntrance> ModEntrances();

// Is there an entrance script `n` (an entrance or a CAE part) - the game's own (pac\evt) or a pack's?
bool EntranceScriptExists(int n);

}  // namespace svr2011
