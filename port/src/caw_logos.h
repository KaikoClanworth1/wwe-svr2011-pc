// More High Resolution (256x256) Paint Tool logos on a Created Superstar.
//
// The game keeps a CAW's logos in a fixed cache inside its data: 2 slots for
// 256x256 logos or 10 for 128x128 ones (the same memory, hence no mixing).
// Slots 2-9 for 256x256 logos are added here: their pixels live in a store
// beside the saves (Saves\.logos\<hash>.bin, one file per distinct logo) and
// the CAW data only records which logo each slot holds, in space the 256x256
// layout leaves unused (so it travels with every copy and save of the CAW).
#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallCawLogos(rex::memory::Memory* memory, const std::filesystem::path& saves);

// Called (once per logo) when a CAW shows a logo whose file isn't in the
// store - e.g. a Superstar downloaded from Community Creations: online.cpp
// fetches it; the CAW shows it from its next load.
using MissingLogoHandler = void (*)(uint64_t hash, const std::filesystem::path& file);
void SetMissingLogoHandler(MissingLogoHandler handler);

}  // namespace svr2011
