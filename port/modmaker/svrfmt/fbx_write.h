// Minimal binary FBX 7.4 writer: a node tree with typed properties, enough
// for meshes, materials, textures, custom properties and connections as
// Blender (and most DCC tools) import them.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace svrfmt::fbx {

struct Prop {
  char type;  // I L D F S R C (Y short), arrays i l d f
  int64_t i = 0;
  double d = 0;
  std::string s;  // S / R
  std::vector<int32_t> ai;
  std::vector<int64_t> al;
  std::vector<double> ad;
  std::vector<float> af;
};

struct Node {
  std::string name;
  std::vector<Prop> props;
  std::vector<std::unique_ptr<Node>> children;

  Node& Add(const std::string& n);
  Node& I(int32_t v) { Prop p{'I'}; p.i = v; props.push_back(p); return *this; }
  Node& L(int64_t v) { Prop p{'L'}; p.i = v; props.push_back(p); return *this; }
  Node& D(double v) { Prop p{'D'}; p.d = v; props.push_back(p); return *this; }
  Node& C(bool v) { Prop p{'C'}; p.i = v; props.push_back(p); return *this; }
  Node& S(const std::string& v) { Prop p{'S'}; p.s = v; props.push_back(p); return *this; }
  Node& Ai(std::vector<int32_t> v) { Prop p{'i'}; p.ai = std::move(v); props.push_back(p); return *this; }
  Node& Ad(std::vector<double> v) { Prop p{'d'}; p.ad = std::move(v); props.push_back(p); return *this; }
  Node& Af(std::vector<float> v) { Prop p{'f'}; p.af = std::move(v); props.push_back(p); return *this; }
};

// "Name\x00\x01Class" as FBX object names are stored.
std::string ObjName(const std::string& name, const std::string& cls);

// Properties70 helpers ("P" records).
void P(Node& p70, const std::string& name, const std::string& type, const std::string& sub, const std::string& flags);
void PInt(Node& p70, const std::string& name, int32_t v);
void PDouble(Node& p70, const std::string& name, double v);
void PVec(Node& p70, const std::string& name, const std::string& type, double x, double y, double z,
          const std::string& flags = "A");
void PString(Node& p70, const std::string& name, const std::string& v, bool custom = false);
void PCustomInt(Node& p70, const std::string& name, int32_t v);

// A document with the standard header nodes; add objects and connections.
struct Document {
  Node root;      // children are the top-level nodes
  Node* objects;
  Node* connections;
  int64_t next_id = 1000000;
  explicit Document(double unit_scale_cm, int up_axis /*0 x,1 y,2 z*/);
  int64_t NewId() { return next_id++; }
  void Connect(int64_t child, int64_t parent, const std::string& property = "");
  std::vector<uint8_t> Write() const;
};

}  // namespace svrfmt::fbx
