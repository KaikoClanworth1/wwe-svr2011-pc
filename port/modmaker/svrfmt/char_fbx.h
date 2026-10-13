// A superstar's model (ch.pac, EPK8) to FBX: the 2011 skeleton as named bones
// in their bind (rest) pose, every model of one attire (body, hair, lashes,
// attachments) as a skinned mesh with its weights, UVs and normals, and the
// textures as PNG files beside it. Blood overlays are left out.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "bytes.h"

namespace svrfmt {

struct CharExportReport {
  int bones = 0, models = 0, meshes = 0, vertices = 0, triangles = 0, textures = 0;
  std::vector<std::string> warnings;
};

// attire: 1-4 (the EMD entry ......<attire-1>2). Writes <out_dir>/<name>.fbx and <out_dir>/textures/*.png.
bool ExportCharacterFbx(const Bytes& pac, int attire, const std::string& out_dir, const std::string& name, CharExportReport& rep);

// An FBX's skeleton: every bone node (a skeleton node, or one a skin cluster uses), parent index (-1: none), in
// file order, and for each its world rest position; false with err when the file doesn't read.
struct FbxBone {
  std::string name;
  int parent = -1;
  float pos[3] = {};
  bool skinned = false;  // a skin cluster uses it (it has weights)
};
bool ReadFbxSkeleton(const std::string& path, std::vector<FbxBone>& out, std::string& err);

// FBX import (F3): the FBX's meshes posed into the 2011 bind pose (the template's A-pose: each mapped source bone turned
// to the template bone's direction, the hands rolled to its knuckle line; the mesh follows by its own skin), in game
// axes (10 cm, -Y up, -Z front, +X the character's left, hips at 0), scaled so the legs are the template's length
// (the feet on the floor; rep.scale), with the bones' weights moved to the 2011 bones (targets: one per ReadFbxSkeleton
// bone, "" = folds into its nearest mapped parent, none: root); its textures in one sheet. Writes <out_dir>/build.json
// (the input of tools/svr11_build.py), color.png and normal.png.
struct FbxImportReport {
  int meshes = 0, vertices = 0, triangles = 0, textures = 0, atlas = 0, aimed = 0, bones_used = 0;
  double scale = 1, height_cm = 0;  // (height: the model as it came, feet to the top)
  double aim_error_deg = 0;         // (test: the posed bones against the template's directions)
  std::string aim_worst;
  std::vector<std::string> warnings, skipped, mesh_lines;
};
// The user's choices per mesh (by FbxMeshInfo::name): on / off (default: the visible ones), its kind ("skin", "eye",
// "hair", "lash"; default by its name), a picture file in place of its material's.
struct FbxImportOptions {
  std::map<std::string, bool> include;
  std::map<std::string, std::string> kind, picture;
};
bool FbxToBuildInput(const std::string& fbx_path, const std::vector<std::string>& targets, const Bytes& template_pac,
                     const std::string& template_path, const std::string& out_dir, FbxImportReport& rep,
                     const FbxImportOptions& opts = {});
// The FBX's meshes as the import sees them: one per mesh node and material ("node / material").
struct FbxMeshInfo {
  std::string name, kind;  // (kind: the guess)
  int vertices = 0, triangles = 0;
  bool hidden = false, has_texture = false;
};
bool FbxListMeshes(const std::string& fbx_path, std::vector<FbxMeshInfo>& out, std::string& err);

// Test: the exported file read back (ufbx) against the pac - bones, bind matrices, vertices, weights; a line per check.
std::vector<std::string> CheckCharacterFbx(const Bytes& pac, int attire, const std::string& fbx_path);

}  // namespace svrfmt
