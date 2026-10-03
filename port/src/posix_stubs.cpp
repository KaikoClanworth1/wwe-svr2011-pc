// WWE SmackDown vs. Raw 2011 - what the Windows-only parts of the port
// provide, for other platforms (Android, Linux) until they have their own:
// PC music for entrances (music.cpp, XAudio2 + Media Foundation). (Crash
// reports: crash_report_posix.cpp.)
#include "music.h"

#include <rex/logging.h>

namespace svr2011 {

#if !defined(__ANDROID__)  // (Android: music.cpp)
void InstallUserMusic(const std::filesystem::path&) {
  REXLOG_INFO("user music: not available on this platform yet");
}
std::filesystem::path UserMusicFolder() { return {}; }
std::filesystem::path UserMusicSong(const std::string&) { return {}; }
#endif

}  // namespace svr2011
