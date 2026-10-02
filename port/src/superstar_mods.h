// Superstar mods (arenas branch): new playable characters from
// <game>/Mods/Superstars/<id>/, in the free DLC slots 59-69. See
// superstar_mods.cpp.
#pragma once

#include <cstdint>
#include <vector>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallSuperstarMods(rex::memory::Memory* memory);
// The installed mods' character ids (the select screen's EXTRA list).
std::vector<uint32_t> SuperstarModIds();
bool IsSuperstarMod(uint32_t id);

}  // namespace svr2011
