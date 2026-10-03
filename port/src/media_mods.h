// Media mods (arenas branch): packs that replace the game's superstars'
// entrance videos, entrance themes and renders, arena screen pictures, and
// sounds / menu music. See media_mods.cpp.
#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// Before the Custom Movies folder is mounted (its device lists the files
// once): the packs' entrance videos are copied into it.
void CopyMediaMovies(const std::filesystem::path& movies);

// After the user music / movies and the arena mods, before the superstar
// mods (it writes its overlay pac for them to mount).
void InstallMediaMods(rex::memory::Memory* memory);

// The virtual file the game asked for, if a pack replaces it: the guest
// address of the replacement's name (superstar_mods.cpp's resolver hook).
uint32_t MediaAlias(const char* name);

// An audio event the game posts (name, game object): true if a pack replaces
// it (played on a host voice; *playing_id what to return) - else the game's.
// Stop / pause events update the host voices and return false.
bool MediaEvent(uint8_t* base, const char* name, uint32_t object, uint32_t* playing_id);

}  // namespace svr2011
