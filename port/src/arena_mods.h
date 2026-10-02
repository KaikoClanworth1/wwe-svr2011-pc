// Arena mods (arenas branch): custom arenas served in place of a host arena.
// Plan and findings: docs/ARENA_MOD_MAKER_PLAN.md.
#pragma once

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallArenaMods(rex::memory::Memory* memory);

}  // namespace svr2011
