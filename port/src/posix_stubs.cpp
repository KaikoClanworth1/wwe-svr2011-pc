// WWE SmackDown vs. Raw 2011 - what the Windows-only parts of the port
// provide, for other platforms (Android, Linux) until they have their own:
// crash reports (crash_report.cpp) and PC music for entrances (music.cpp,
// XAudio2 + Media Foundation).
#include "crash_report.h"
#include "music.h"

#include <rex/logging.h>

namespace svr2011 {

void InstallCrashReporter(const std::filesystem::path&) {}
void SetCrashHook(void (*)()) {}

#if !defined(__ANDROID__)  // (Android: music.cpp)
void InstallUserMusic(const std::filesystem::path&) {
  REXLOG_INFO("user music: not available on this platform yet");
}
std::filesystem::path UserMusicFolder() { return {}; }
std::filesystem::path UserMusicSong(const std::string&) { return {}; }
#endif

}  // namespace svr2011
