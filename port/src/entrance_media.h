// WWE SmackDown vs. Raw 2011 - Community Creations: a Created Superstar's
// entrance song (USER PLAYLIST, Music folder) and movie (USER MOVIES, Custom
// Movies folder) go with it.
//
// The game uploads a Superstar's entrance with it (the .cas carries the
// profile record, the entrance's song by name) but resets a highlight-reel
// movie - which is what a user movie is to the game - to none. The port sends
// the song file and the movie (shrunk to an MP4, movie_transcode.h) to the
// server beside the Superstar; a player who downloads it gets them in Music
// and Custom Movies (under other names if theirs are taken by different
// files) and the entrance pointing at them.
#pragma once

#include <cstdint>
#include <filesystem>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// (after InstallUserMusic and InstallUserMovies: their folders)
void InstallEntranceMedia(rex::memory::Memory* memory);

// A downloaded Superstar (its .cas at guest address `cas`) is about to be
// saved: its entrance gets this PC's song and movie.
void PrepareDownloadedEntrance(uint8_t* base, uint32_t cas);

}  // namespace svr2011
