// USER MOVIES: entrance movies of your own, from the "Custom Movies" folder
// beside the game (Bink files made by the launcher's Movies tab, 320x320
// like the game's titantron movies). They are listed in CREATE AN ENTRANCE >
// FINALIZE > MOVIE after NONE and play like the game's own movies.
#pragma once

#include <filesystem>
#include <string>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallUserMovies(rex::memory::Memory* memory, const std::filesystem::path& folder);

// For Community Creations (entrance_media.cpp):
// the Custom Movies folder;
std::filesystem::path UserMoviesFolder();
// the file user movie `id` (700-899) plays, empty if none;
std::filesystem::path UserMovieFile(int id);
// the id of `file` (a .bik in the folder, maybe still being written: its id
// is kept until FinishUserMovie), 0 if all are taken;
int ReserveUserMovie(const std::string& file);
void FinishUserMovie(const std::string& file);

}  // namespace svr2011
