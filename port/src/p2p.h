// WWE SmackDown vs. Raw 2011 - peer-to-peer online matches (Player Match /
// Ranked Match lobbies and play) with no server: games find each other on
// the network themselves.
//
// The game uses Xbox LIVE's session API (XSessionCreate / Search / Join ...,
// app 0xFB) and XNet (its own address, QoS probes, peers' addresses) over
// plain UDP sockets. Here (through the SDK's online_hooks.h):
//   - each game has an address of its own: its LAN IP, its public IP when
//     known, its port base (p2p_port) and a hardware ID from the player's
//     online id;
//   - each peer gets a private address 10.64.x.y the game uses in its socket
//     calls, mapped to the peer's real IP and ports (its own port base: two
//     games on one PC are two peers);
//   - a host advertises its sessions on its port base (UDP); a search asks
//     the LAN (broadcast) and the addresses in p2p_peers, a QoS probe asks the
//     host directly. A joining game says hello first, so the host knows it.
// The online server (online_server) is never needed to play: it only keeps
// the leaderboards.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallP2P(rex::memory::Memory* memory);

// Files games fetch from each other (online_cas.cpp: the Created Superstars'
// Paint Tool data and extra logos): a file is a kind and a key. The source
// gives this game's copy (nullopt: it hasn't); P2PFetch asks the peers (the
// games this one knows) and returns the first that has it, whole.
using P2PFileSource = std::optional<std::string> (*)(uint8_t kind, const std::string& key);
void SetP2PFileSource(P2PFileSource source);
std::optional<std::string> P2PFetch(uint8_t kind, const std::string& key, std::chrono::milliseconds timeout);

// Invites (online_overlay.h): the match session this game is in (hosts, or
// else joined) as an XSESSION_INFO (60 bytes: id, host XNADDR, key) and its
// slots; false when there's none.
bool P2PSessionInfo(uint8_t* out, uint32_t* slots = nullptr);
// An invited game, before the game joins: reaches the session's host (directly
// or through the relay) so its packets get through; true if it still has it.
bool P2PReachHost(const uint8_t* info);
// An invite accepted: for 15 minutes the game's session searches list that
// session first, with its private slots open to this game.
void P2PExpectInvite(const uint8_t* info);

}  // namespace svr2011
