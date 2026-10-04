// WWE SmackDown vs. Raw 2011 - the online LEADERBOARDS (Xbox LIVE stats),
// kept by the Community Creations server (port/server/leaderboards.py).
//
// The game's stats calls:
//   - XSessionWriteStats (XGI 0xB0025, after a ranked match and on entering
//     ONLINE): the player's own rows go to <online_server>/api/stats/write -
//     the server applies the game's own column rules (its XLAST: Sum / Last,
//     the rating orders the view, weekly views reset);
//   - XUserReadStats (XGI 0xB0021): given players' rows (/api/stats/read);
//   - the stats enumerators (by rank: sub_82904C20, around a player:
//     sub_82904C80 - XamUserCreateStatsEnumerator) and XEnumerate on them
//     (sub_829048B0): a page of a view (/api/stats/page). Their handles are
//     the port's own (0xFEED00xx); other enumerators go on to the SDK.
#pragma once

#include <cstdint>
#include <optional>

namespace rex::memory {
class Memory;
}
namespace rex::system {
class KernelState;
}

namespace svr2011 {

void InstallLeaderboards(rex::memory::Memory* memory, rex::system::KernelState* kernel);

// The stats XGI messages (p2p.cpp's XGI hook passes them on): the result, or
// nullopt for the SDK.
std::optional<uint32_t> LeaderboardsXgi(uint32_t message, uint32_t buffer, uint32_t length);

}  // namespace svr2011
