// USER MOVIES: entrance movies of your own, from the "Custom Movies" folder
// beside the game (Bink files made by the launcher's Movies tab, 320x320
// like the game's titantron movies). They are listed in CREATE AN ENTRANCE >
// FINALIZE > MOVIE after NONE and play like the game's own movies.
#pragma once

#include <filesystem>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallUserMovies(rex::memory::Memory* memory, const std::filesystem::path& folder);

}  // namespace svr2011
