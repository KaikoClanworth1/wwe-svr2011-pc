// The objects (meshes) of an arena model: list them, add one from an OBJ or FBX file, remove one, export one
// as OBJ + MTL + PNG textures. The 3D Arena Editor's Objects panel and svrmod's objects / obj-add / obj-remove /
// obj-export use it.
//
// What the game needs of an object (a JBOY mesh), as svrfmt/jboy.h writes it:
//  - geometry: 28-byte vertices (position, normal, ARGB colour), UVs, one weight block (palette slot 0, weight
//    1 - static), triangle strips (u16 indices: at most 65,535 vertices a mesh, so a big file is split by material);
//  - a material: a shader name and its params (texDiffuse = a slot in the model's texture list). New meshes copy
//    the host model's first plain (static) mesh, so they are lit and drawn like the model they join;
//  - bounds: the mesh's sphere, its node's sphere (the game culls a model whose node sphere is out of view) and the
//    arena's visibility box for the model (entry 0xC364) - all grown after an edit (FitNodeSpheres, GrowVisibility);
//  - textures in the arena's texture set (DXT1 / DXT5, power-of-two sizes) within the file budget (FitFile);
//  - room: the arena's vertex / index totals (GeometryOf: past the stock maxima needs 2.1.6, kMaxArenaVertsNew the
//    most) and its mesh count: stock arenas have 2-446 meshes (bg01 441, bg02 446); HCTP backstage rooms crashed the
//    stage allocator at 625 (sub_826E73F8; 5 rooms of ~400 were fine) - MeshCount warns above kMeshWarn (more than
//    any stock arena) and strongly from kMeshDanger.
// Removing empties the mesh in place (one vertex, no triangles - as MakeEmpty does): nothing that counts meshes by
// index changes, and the game still finds every mesh it expects.
#pragma once

#include <string>
#include <vector>

#include "arena.h"

namespace svrfmt {

struct ObjectInfo {
  int mesh = 0;
  std::string texture;  // the diffuse texture's name ("" if none)
  size_t verts = 0, triangles = 0;
  bool skinned = false;  // rigged (ropes, apron, crowd): not removable / exportable as a prop
  bool empty = false;    // no triangles (removed, or a placeholder)
};
std::vector<ObjectInfo> ObjectsOf(const Model& m);

// Arena-wide mesh count (non-empty meshes of every top-level model) and the level a warning starts at.
size_t MeshCount(const Arena& a);
constexpr size_t kMeshWarn = 450, kMeshDanger = 600;

struct AddOptions {
  float pos[3] = {0, 0, 0};  // where the file's origin goes, in game units (10 cm; y down: -y is up)
  float scale = 1.0f;        // on top of the units
  float yaw_degrees = 0.0f;  // a turn about the vertical axis
  float obj_unit_meters = 1.0f;  // an OBJ's unit (OBJ files don't say): 1 = metres, 0.01 = centimetres; FBX says
};
struct AddReport {
  int meshes_added = 0, textures_added = 0;
  int first_mesh = -1;
  std::vector<std::string> warnings, errors;
};
// Adds the file's geometry (one mesh per material) to model `model`. Its textures (map_Kd / the FBX diffuse
// texture, a PNG / JPG / TGA / BMP / DDS beside it) join the arena's texture set.
bool AddObjectFile(Arena& a, int model, const std::string& path, const AddOptions& opt, AddReport& rep);

// Empties mesh `mesh` of model `model` (-1: every mesh of it). false (and *error) for a rigged mesh.
bool RemoveObject(Arena& a, int model, int mesh, std::string* error = nullptr);

// out_dir/<name>.obj + <name>.mtl + textures as PNG: meshes `meshes` of model `model` (empty: all of it). Metres,
// Y up, counter-clockwise front faces - what other 3D programs expect, and what AddObjectFile reads back.
bool ExportObjectObj(const Arena& a, int model, const std::vector<int>& meshes, const std::string& out_dir,
                     std::string* obj_path = nullptr, std::string* error = nullptr, const std::string& name = "");

// After edits: every changed model's node spheres and visibility boxes grown. -> the boxes grown.
int RefreshBounds(Arena& a);

// A model by PACH id (hex or decimal) or name -> index, -1 if none.
int FindModel(const Arena& a, const std::string& key);

}  // namespace svrfmt
