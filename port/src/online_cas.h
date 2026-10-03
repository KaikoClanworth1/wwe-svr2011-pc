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

#include <filesystem>

namespace svr2011 {

void InstallOnlineCas(const std::filesystem::path& saves);

}  // namespace svr2011
