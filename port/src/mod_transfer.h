// WWE SmackDown vs. Raw 2011 - online: the host's gameplay mods to a joining player who lacks them, straight
// from the host's game (mod_transfer.cpp). P2P only over a direct link: no server copy and the relay never
// carries files (the user's rule) - a relayed peer is told why and gets nothing.
//
// The host serves the mods of its own gameplay set (online_files.h GameplayModSet) while it hosts a session:
// only files inside those mod folders, of the allowed kinds (ModFileAllowed). The joiner asks for the set,
// each mod's file list, then the files in ranges it requests itself (receiver-driven: the host never sends
// more than was asked, so a slow link backs off instead of flooding the lobby's traffic), checks every path
// again, writes into <user data>\OnlineMods\<kind>\<folder>@<crc>\ (.part files that resume), checks each
// file's CRC and then the whole mod's against the host's key. The folder is cleared when online ends and at
// the next start.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace svr2011 {

void InstallModTransfer();

// A path inside a mod folder ("moves/motions/x.ymk"; '/' between parts) that may travel: allowed kinds only,
// no "..", no absolute or drive paths, no ':' (streams), no device names (CON, NUL ...); .bin only under
// moves/, .toml / .ini only as the manifest. (why: the reason, for the log / the player.)
bool ModFileAllowed(const std::string& rel, std::string* why = nullptr);

// One mod of the host's set as the joiner sees it.
struct ModOffer {
  std::string kind, id, version, crc, name;  // (as the mod set: online_files.cpp)
  uint64_t bytes = 0;
  uint32_t files = 0;
  std::string unsendable;  // (the host's word: a file it can't send, and why - the mod can't come)
};

// A download from the host of the session this game joined.
struct ModDownloadState {
  enum Phase { kIdle, kAsking, kFetching, kDone, kFailed } phase = kIdle;
  uint64_t done = 0, total = 0;  // bytes
  double rate = 0;               // bytes / s, smoothed
  int mod = 0, mods = 0;         // the one being fetched (1-based), of
  std::string error;             // (kFailed: why, for the player)
};

// Start fetching these mods (from the host's set, by kind + id + crc) from the session's host; false: already
// fetching, or no host / no direct link (state().error says).
bool StartModDownload(const std::vector<ModOffer>& mods);
void CancelModDownload();
ModDownloadState ModDownload();
// Where a fetched mod is (empty until it is complete and checked).
std::filesystem::path FetchedModFolder(const ModOffer& mod);
// The host's set, asked for (empty: none, or no answer - why says which; "" when the host has none).
std::vector<ModOffer> AskHostMods(int timeout_ms = 3000, std::string* why = nullptr);
// Clears <user data>\OnlineMods (leaving online, the game's end, the next start).
void ClearFetchedMods();

// The lobby: once per session this game joined, the host's gameplay mods it lacks - the overlay asks the player
// (online_overlay.cpp: "THIS MATCH USES N MODS YOU DON'T HAVE (X MB) - DOWNLOAD THEM?", NO leaves the session).
// relayed: the link to the host goes through the relay, so nothing can come (mods only travel straight from
// the host); lacking then has what the lobby's mod tables say this game lacks (online_mods.h), for the reason.
struct ModLobbyOffer {
  uint64_t session = 0;
  std::vector<ModOffer> mods;
  uint64_t bytes = 0;
  bool relayed = false;
  std::vector<std::string> lacking;
  std::vector<std::string> unsendable;  // (mods this game lacks that the host can't send: "name: file (why)")
};
// The newest offer, once (false: none since the last call).
bool TakeModLobbyOffer(ModLobbyOffer* out);
// YES: fetch them (StartModDownload). (NO: the overlay leaves the session.)
void AnswerModLobbyOffer(const ModLobbyOffer& offer, bool download);

}  // namespace svr2011
