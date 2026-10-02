// An arena file (pac/bg/bgNN.pac) as the Mod Maker sees it: its models,
// textures and the entries it does not edit (kept byte for byte), plus the
// Blender round trip (export: arena.fbx + textures/*.png + arena.json).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "jboy.h"
#include "pac.h"
#include "texture.h"

namespace svrfmt {

enum class Zone { kRing, kRingside, kEntrance, kFree };
const char* ZoneName(Zone z);
Zone ZoneOf(const std::string& model_name);

struct ArenaModel {
  uint32_t id = 0;  // PACH id
  Model model;
  Zone zone = Zone::kFree;
  bool changed = false;
};

struct ArenaTextureSet {
  uint32_t id = 0;  // PACH id of the bundle
  std::vector<BundleTexture> textures;
  bool changed = false;
  size_t original_raw = 0;  // unpacked size as shipped (the game's budget)
};

struct Arena {
  Epac epac;
  size_t group = 0, entry = 0;          // the STG entry holding the PACH
  std::vector<PachEntry> entries;       // all entries, as stored
  std::vector<ArenaModel> models;       // top-level JBOY entries
  std::vector<ArenaTextureSet> bundles; // top-level texture bundles

  size_t original_file = 0;  // .pac size as shipped

  bool Load(const std::string& path, std::string* error = nullptr);
  // The game keeps each arena in fixed memory: an arena that outgrows its
  // shipped size fails to load (crash or endless NOW LOADING). Halves the
  // largest textures not in `keep` until every texture set is back within
  // its shipped size. Returns the textures that were reduced.
  std::vector<std::string> FitBudget(const std::vector<std::string>& keep);
  // Rebuild the .pac: changed models/bundles re-compressed, the rest as read.
  // *error is set if a changed entry would not load (packed >= unpacked).
  Bytes Save(std::string* error = nullptr) const;
  // Texture by name across bundles (nullptr if none).
  BundleTexture* FindTexture(const std::string& name);
};

struct ExportReport {
  int models = 0, meshes = 0, triangles = 0, textures = 0, textures_failed = 0;
  std::vector<std::string> warnings;
};
// out_dir/arena.fbx, out_dir/textures/<name>.png, out_dir/arena.json
bool ExportArena(const Arena& a, const std::string& out_dir, const std::string& title, ExportReport& rep);

}  // namespace svrfmt
