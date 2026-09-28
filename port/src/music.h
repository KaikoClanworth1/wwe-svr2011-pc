// WWE SmackDown vs. Raw 2011 - user music for entrances.
//
// On the Xbox 360, Create An Entrance -> Music -> USER PLAYLIST let a player
// use a song from the console's music library as a Superstar's entrance
// music: the game lists the library's playlists through the Xbox music player
// (XMP) and has it play the chosen one during the entrance. Here the library
// is the "Music" folder beside the game: every subfolder is a playlist named
// after it (put one song in each, as the game asks), and a song placed
// directly in the folder is a playlist of its own, named after the file.
// .mp3, and anything else Media Foundation decodes (.wma, .m4a, .aac, .wav,
// .flac). The folder is read each time the game asks, so songs can be added
// while it runs.

#pragma once

#include <filesystem>

namespace svr2011 {

// Serves `folder` to the game's music player (xmp_app.h, HostMusic). Call once.
void InstallUserMusic(const std::filesystem::path& folder);

}  // namespace svr2011
