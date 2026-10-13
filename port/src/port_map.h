// WWE SmackDown vs. Raw 2011 - a port mapping on the player's router for the
// P2P socket (UPnP IGD or NAT-PMP), so peers far away can reach this game
// directly instead of through the relay (port_map.cpp).
#pragma once

#include <cstdint>
#include <functional>

namespace svr2011 {

// Maps UDP `port` (outside = inside) to `lan_ip` (host order) in the
// background; `mapped(external_ip, port)` is called once it holds (the
// router's own outside address; 0 if it didn't say). Renewed while the game
// runs, removed at exit; nothing when the router refuses or has neither
// protocol (cvar p2p_port_mapping off: never tried).
void StartPortMapping(uint16_t port, uint32_t lan_ip, std::function<void(uint32_t, uint16_t)> mapped);

// The mapping removed now (the game is closing: its exit skips atexit).
void StopPortMapping();

}  // namespace svr2011
