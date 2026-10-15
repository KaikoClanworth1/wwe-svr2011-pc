// Superstar mods in WWE Universe (universe_mods.cpp): Universe keeps 150 superstars in the save; a switched-on mod
// gets an entry and its rivalries in a side table instead (guest memory, saved next to the save as
// Saves\.universe\mods.bin), so the save itself stays as older builds read it.
#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallUniverseMods(rex::memory::Memory* memory, const std::filesystem::path& saves);
// sub_825B7660 for a mod: a "virtual place" k whose entry math (P + 16 + 24k, 32-bit) lands on the mod's side entry;
// -1 when the mod has none (the side table full, or not set up).
int64_t UniverseModPlace(uint8_t* base, uint32_t id);
// sub_825AD850 with a mod in the pair (place_a / place_b: the other id's own place, -1 none): the offset from the
// rivalry table the caller reads (`table`) to the pair's side byte, -1 for none. One byte per pair, whichever order.
// Only Universe's own table (P + 3616) has the mods' bytes: a copy of it (sub_825C2BA0 works on one on the stack)
// answers -1, the "no pair" every caller tests.
int64_t UniverseModPair(uint8_t* base, uint32_t table, uint32_t a, int64_t place_a, uint32_t b, int64_t place_b);
// Whether a mod has a Universe entry.
bool UniverseModIn(uint32_t id);
// After Universe's reset (sub_825B6C60) and after a save is loaded (sub_8257A218): the side table cleared / read
// from its file (a save without one: cleared).
void UniverseModsReset();
void UniverseModsLoaded(uint8_t* base);
// Once a frame: the game wrote SaveData.dat -> the side table written beside it (tmp + rename).
void UniverseModsTick(uint8_t* base);

}  // namespace svr2011
