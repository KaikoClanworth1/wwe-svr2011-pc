// Blender -> arena: reads an FBX (ufbx) made from an export (arena.fbx) and
// applies it to the original arena.
//  - Objects with svr_id (or a name ending __<hex id>) replace that model's
//    geometry. Each FBX material slot named <object>_m<i> maps back to mesh
//    i (keeping its shader and params); other slots become new meshes made
//    from a template mesh with the slot's diffuse texture.
//  - Objects without an id are new: their meshes are added to a host model
//    the game always draws (the arena floor, see ImportOptions).
//  - Textures: PNGs next to the FBX (textures/<name>.png) replace arena
//    textures of the same name when they differ; new names are added to the
//    arena's texture bundle (DXT1, or DXT5 with alpha).
//  - Skinned models (ropes, apron, turnbuckles) keep their weights; only
//    their textures and vertex positions with the same vertex count change.
#pragma once

#include <string>
#include <vector>

#include "arena.h"

namespace svrfmt {

struct ImportOptions {
  std::string textures_dir;    // default: <fbx dir>/textures
  std::string host_model;      // model new objects join (default: biggest "free" floor model)
  bool hide_missing = false;   // models missing from the FBX: hidden (else kept)
};

struct ImportReport {
  int models_changed = 0, models_hidden = 0, objects_added = 0, meshes_added = 0;
  int textures_replaced = 0, textures_added = 0;
  std::vector<std::string> warnings, errors;
};

bool ImportFbx(Arena& arena, const std::string& fbx_path, const ImportOptions& opt, ImportReport& rep);

}  // namespace svrfmt
