// More High Resolution (256x256) Paint Tool logos on a Created Superstar.
//
// The game keeps a CAW's logos in a fixed cache inside its data: 2 slots for
// 256x256 logos or 10 for 128x128 ones (the same memory, hence no mixing).
// Slots 2-9 for 256x256 logos are added here: their pixels live in a store
// beside the saves (Saves\.logos\<hash>.bin, one file per distinct logo) and
// the CAW data only records which logo each slot holds, in space the 256x256
// layout leaves unused (so it travels with every copy and save of the CAW).
#pragma once

#include <filesystem>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallCawLogos(rex::memory::Memory* memory, const std::filesystem::path& saves);

}  // namespace svr2011
