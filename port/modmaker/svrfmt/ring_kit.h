// The Ring Kit: rope looks, hidden ropes, turnbuckles and corner pads.
//
// The game draws the ropes from one model per side, three times (heights
// from code: 3.4 above the mat, then every 4.2 units). An arena that holds
// the entries 900-911 instead gets one model per rope (id 900 + side +
// 4 * rope, rope 0 = bottom), a mode the game has but no shipped arena uses.
// The kit writes those twelve models, so each rope can have its own colour
// or texture, or be left out. Its gameplay side (rope heights, rope moves on
// missing ropes) is read by the port from the mod's manifest (ring.* keys).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "arena.h"

namespace svrfmt {

struct RopeLook {
  bool visible = true;
  uint32_t tint = 0xFFFFFF;  // RGB multiplied into the rope's material colours
  std::string texture;       // optional: picture file (PNG/JPG/...) for this rope
};

struct RingSpec {
  RopeLook ropes[3];        // 0 = bottom, 2 = top
  bool turnbuckles = true;  // the three turnbuckles on each post
  bool pads = true;         // the corner pads (ar_cover)
  // Gameplay (port side, written to the manifest): rope height and spacing
  // in game units (1 = 10 cm). 0 = the game's (3.4, 4.2).
  float rope_base = 0.0f, rope_gap = 0.0f;

  bool Default() const;
  int VisibleRopes() const;
  // manifest lines: "ring.ropes=1 1 1", "ring.tints=ffffff ffffff ffffff", ...
  std::string ManifestLines() const;
  // reads the keys back (unknown keys are ignored)
  void FromManifest(const std::string& text);
};

struct RingReport {
  int models_added = 0, models_changed = 0, textures_added = 0;
  std::vector<std::string> warnings;
};

// Applies the looks to the arena's models (adds 900-911 when needed).
bool ApplyRing(Arena& arena, const RingSpec& spec, RingReport& rep);

}  // namespace svrfmt
