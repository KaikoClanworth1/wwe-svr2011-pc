// A superstar's model (ch.pac, EPK8) to FBX: the 2011 skeleton as named bones
// in their bind (rest) pose, every model of one attire (body, hair, lashes,
// attachments) as a skinned mesh with its weights, UVs and normals, and the
// textures as PNG files beside it. Blood overlays are left out.
#pragma once

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

// Test: the exported file read back (ufbx) against the pac - bones, bind matrices, vertices, weights; a line per check.
std::vector<std::string> CheckCharacterFbx(const Bytes& pac, int attire, const std::string& fbx_path);

}  // namespace svrfmt
