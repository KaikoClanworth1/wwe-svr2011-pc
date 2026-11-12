// An arena's visibility tree (entry 0xC364): models it leaves out given nodes. Game code too (src/arena_mods.cpp):
// only pac + jboy here, no FBX / PNG.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "jboy.h"
#include "pac.h"

namespace svrfmt {

struct VisModel {
  std::string name;
  float lo[3], hi[3];  // model units
};
// m's vertex bounds -> false when it has no name or vertices.
bool VisBounds(const Model& m, VisModel& out);

// The game draws only the arena models its visibility tree names: a model the tree leaves out is culled from the
// match's view (camera cuts showed it for a moment). Ports from other games (WWE 2K17 bg32: the tree named 32 of its
// 150 models - no mats, barricades or stands; report 8e4b10, 12 Nov) and 2007-2009 arenas had such trees. Every model
// it doesn't name gets a node under VT_ROOT boxed by its vertices (as arena_port.py vis_tree: 3ds Max axes in metres,
// (x, z, -y) / 10), VT_ROOT's child count and box grown; `vis` (the 0xC364 entry as stored) rewritten, BPE.
// -> the nodes added (0: none, or no VT_ROOT).
int AddVisNodes(PachEntry& vis, const std::vector<VisModel>& models);

// The models whose flags (0xC351) carry "x": WWE '12 - 2K17 ports have it on every model, no stock SvR2011 arena on
// any (all 27 bgNN.pac). Only those are added - a stock arena leaves stage, glow and beam models out of its tree and
// draws them as they are, so its tree stays as it is (a test without the x flags drew this arena's barricades from
// one side only: the tree is what counts, 12 Nov).
std::set<uint32_t> XFlagged(const std::vector<PachEntry>& entries);

// The same on an arena pac's stage entries: the x-flagged models (ids below 0x190, a JBOY or a PACH whose part 0 is
// one) read from them. -> the nodes added (0: none / no tree).
int ListAllInVisibility(std::vector<PachEntry>& entries);

}  // namespace svrfmt
