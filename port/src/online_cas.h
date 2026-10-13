// WWE SmackDown vs. Raw 2011 - Created Superstars with Paint Tool logos in
// online matches, peer to peer (no server).
//
// The game lets a Created Superstar into an online match only when the
// Paint Tool data of its attire has been "uploaded": entering ONLINE it asks
// to upload each attire still without, as a CASData record (Sake) whose file
// is the attire's data (187504 bytes: a 0x22C-byte header, then the start of
// the .cas - its logos). It marks the attire done in the Superstar's record
// (SaveData.dat, record + 0x69C + attire) with the record's id (+ 0x6A4 + 4 *
// attire). In a match the other games fetch that record and its file to draw
// the logos.
//
// Here the relay (online_net.h) sends those uploads to the server as before
// (so Community Creations works as it did) and keeps a copy in
// Saves\.online (<file id>.cas, r<record id>.txt); when the server doesn't
// take them, they're kept only here, under an id of this player's own
// (0x40000000 and up), and the game stamps the Superstar with that. A game
// asking for such a record or file - or for one the server can't give - gets
// it from its owner over P2P (p2p.h; kept in Saves\.online\cache), with the
// extra High Resolution logos it uses (caw_logos.h, Saves\.logos).
// (The game signs in to GameSpy before it fetches them: with the server
// down it leaves the match.)
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace svr2011 {

void InstallOnlineCas(const std::filesystem::path& saves);

// Other players' Created Superstar pictures in the online lobby
// (online_cpu.cpp): the 256 x 512 render of attire `attire` of `owner`'s
// Created Superstar `id`, from its owner over P2P (once per version: kept in
// Saves\.online\pictures by its hash). nullptr while it's being fetched -
// the first ask starts it - or when nobody had it (asked again after 15 s).
const std::string* OnlinePicture(uint64_t owner, uint32_t id, uint32_t attire);
// A Created Superstar of this game whose .cas a picture object can read (it
// then shows the render given instead: online_cpu.cpp); -1 none.
int OnlineLocalCaw();

}  // namespace svr2011
