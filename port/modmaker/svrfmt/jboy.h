// JBOY arena models (Yuke's YOBJ, big-endian). Mirrors tools/jboy.py, which
// reads and rewrites all 8633 models in the 35 arena files (layout notes in
// that file and docs/ARENA_MOD_MAKER_PLAN.md).
#pragma once

#include <array>
#include <string>
#include <vector>

#include "bytes.h"

namespace svrfmt {

struct Vertex {
  float pos[3];
  float normal[3];
  uint32_t color;  // ARGB
};

struct Weight {
  uint8_t bones[4];  // palette slots, 255 = none
  float weight;
};

struct Param {
  std::string name;
  uint16_t type = 0;  // 0x0d f32x4, 0x0a f32, 0x05 int, 0x0f texture slot, 0x10 bool
  Bytes value;        // big-endian, as stored
};

struct Strip {
  uint32_t prim = 6;  // 6 = triangle strip
  std::vector<uint16_t> indices;
};

struct Mesh {
  Bytes raw;  // the 0xB4-byte descriptor as read (unknown fields kept)
  std::vector<int32_t> palette;
  uint32_t material = 0;  // +0x74
  std::string shader;
  uint32_t vfmt = 0;
  std::array<float, 4> sphere{};
  std::vector<Vertex> verts;
  std::vector<std::vector<Weight>> weights;  // blocks, each one per vertex
  std::vector<std::array<float, 2>> uvs;
  std::vector<Param> params;
  std::vector<Strip> strips;
};

struct Node {
  std::string name;
  float t[3], r[3];
  uint32_t u40 = 0;
  int32_t parent = -1;
  uint32_t u48[4] = {};
  float sphere[4];
};

struct Model {
  Bytes header;  // file bytes 8..0x48 (counts and pointers rewritten on write)
  std::string name;
  Bytes group;   // 16 bytes after the name
  std::vector<std::string> textures;
  std::vector<Node> nodes;
  std::vector<Mesh> meshes;
};

inline bool IsJboy(const Bytes& b) { return b.size() >= 0x48 && !std::memcmp(b.data(), "JBOY", 4); }
bool JboyRead(const Bytes& in, Model& out, std::string* error = nullptr);
Bytes JboyWrite(const Model& m);

// Triangle list from a degenerate-joined strip (winding alternates).
std::vector<std::array<uint16_t, 3>> StripToTriangles(const std::vector<uint16_t>& strip);
// Triangle list -> one strip (each triangle joined with degenerates).
std::vector<uint16_t> TrianglesToStrip(const std::vector<std::array<uint16_t, 3>>& tris);

}  // namespace svrfmt
