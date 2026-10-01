// WWE SmackDown vs. Raw 2011 - user movies (Custom Movies\*.bik, made by the
// launcher's Movies tab) to and from H.264 MP4, for Community Creations: a
// Superstar's entrance movie goes to the server as a small MP4 and becomes a
// .bik again on the PC that downloads it. The launcher's Bink writer stores
// pictures nearly raw (hundreds of MB for an entrance); the MP4 is a few
// dozen. Windows (Media Foundation) only for now.
#pragma once

#include <cstdint>
#include <filesystem>

namespace svr2011 {

// Whether this platform can do the two below.
bool CanTranscodeMovies();

// A user movie as an MP4 of at most max_bytes (the bitrate is picked from its
// length). false if it can't (not a Bink movie, encoder failed, too big).
bool BikToMp4(const std::filesystem::path& bik, const std::filesystem::path& mp4, uint64_t max_bytes);

// An MP4 (from BikToMp4) as a user movie. Writes <bik>.part, then renames it.
bool Mp4ToBik(const std::filesystem::path& mp4, const std::filesystem::path& bik);

}  // namespace svr2011
