// WWE SmackDown vs. Raw 2011 - online services (Community Creations).
//
// With online_enabled (SDK, xam_net.cpp) the profile is signed in to LIVE,
// has every privilege, and the game's GameSpy web services - Sake storage
// (Community Creations), AuthService, CompetitionService - go to
// online_server (port/server/gamespy_server.py) instead of the dead
// *.gamespy.com. InstallOnline adapts the game for that server:
//   - AuthService over plain http (the game's only https URL);
//   - the key that signs login certificates: the server's own.
#pragma once

#include <filesystem>
#include <string>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// Before the settings load: the online keys a phone's package brought
// (<exe>/online.toml, from the PC's launcher) go into the settings file, and
// the profile gets its own random online id (online_xuid) once.
void PrepareOnlineSettings(const std::filesystem::path& config, const std::filesystem::path& exe_dir);

// saves: the saves folder (the Superstars' extra logos in Saves\.logos go
// to and come from the server).
void InstallOnline(rex::memory::Memory* memory, const std::filesystem::path& saves);

// The game's text for string id (menu_hooks.cpp's lookup hook): the guest
// address of the port's replacement, or 0 for the game's own. Removes what
// the PC doesn't have (gamer cards, Xbox LIVE Party).
uint32_t OnlineString(uint32_t id);
// The ONLINE screen's NEWS panel: the server's news (the presence poll's
// config "news", UTF-8, already in the game font's characters) for the next
// visit. No server answer: the game keeps what it fetched.
void SetOnlineNews(const std::string& text);

// Guest scratch memory for online_cpu.cpp's calls into the game (4 KB, the
// game's thread only); 0 before InstallOnline.
uint32_t OnlineScratch();
// A picture holder's object failed to load and the holder falls back to its
// "no picture" (sub_8273BB60 from 0x8273C080: online_cpu.cpp's Created
// Superstar pictures shown with a stand-in - that one isn't usable).
void OnlineCawPictureFailed(uint8_t* base, uint32_t holder);

}  // namespace svr2011
