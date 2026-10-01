// More exhibition match types: rows added to the PLAY menus for rule
// records the game has but doesn't offer (and new ones). Research and plan:
// docs/MATCH_TYPES_RESEARCH.md.
#pragma once

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallMatchTypes(rex::memory::Memory* memory);

}  // namespace svr2011
