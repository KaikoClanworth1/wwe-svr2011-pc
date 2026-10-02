#include "fbx_write.h"

#include <cstring>

namespace svrfmt::fbx {

Node& Node::Add(const std::string& n) {
  children.push_back(std::make_unique<Node>());
  children.back()->name = n;
  return *children.back();
}

std::string ObjName(const std::string& name, const std::string& cls) {
  std::string s = name;
  s.push_back('\0');
  s.push_back('\x01');
  s += cls;
  return s;
}

void P(Node& p70, const std::string& name, const std::string& type, const std::string& sub, const std::string& flags) {
  p70.Add("P").S(name).S(type).S(sub).S(flags);
}
void PInt(Node& p70, const std::string& name, int32_t v) { p70.Add("P").S(name).S("int").S("Integer").S("").I(v); }
void PDouble(Node& p70, const std::string& name, double v) { p70.Add("P").S(name).S("double").S("Number").S("").D(v); }
void PVec(Node& p70, const std::string& name, const std::string& type, double x, double y, double z,
          const std::string& flags) {
  p70.Add("P").S(name).S(type).S("").S(flags).D(x).D(y).D(z);
}
void PString(Node& p70, const std::string& name, const std::string& v, bool custom) {
  p70.Add("P").S(name).S("KString").S("").S(custom ? "A+U" : "").S(v);
}
void PCustomInt(Node& p70, const std::string& name, int32_t v) {
  p70.Add("P").S(name).S("int").S("Integer").S("A+U").I(v);
}

namespace {

void W32(std::vector<uint8_t>& o, uint32_t v) { for (int k = 0; k < 4; ++k) o.push_back(uint8_t(v >> (8 * k))); }
void W64(std::vector<uint8_t>& o, uint64_t v) { for (int k = 0; k < 8; ++k) o.push_back(uint8_t(v >> (8 * k))); }
template <class T> void WRaw(std::vector<uint8_t>& o, const T& v) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
  o.insert(o.end(), p, p + sizeof(T));
}

void WriteProp(std::vector<uint8_t>& o, const Prop& p) {
  o.push_back(uint8_t(p.type));
  switch (p.type) {
    case 'C': o.push_back(p.i ? 1 : 0); break;
    case 'Y': { int16_t v = int16_t(p.i); WRaw(o, v); break; }
    case 'I': { int32_t v = int32_t(p.i); WRaw(o, v); break; }
    case 'L': WRaw(o, p.i); break;
    case 'F': { float v = float(p.d); WRaw(o, v); break; }
    case 'D': WRaw(o, p.d); break;
    case 'S': case 'R':
      W32(o, uint32_t(p.s.size()));
      o.insert(o.end(), p.s.begin(), p.s.end());
      break;
    case 'i':
      W32(o, uint32_t(p.ai.size())); W32(o, 0); W32(o, uint32_t(p.ai.size() * 4));
      for (int32_t v : p.ai) WRaw(o, v);
      break;
    case 'l':
      W32(o, uint32_t(p.al.size())); W32(o, 0); W32(o, uint32_t(p.al.size() * 8));
      for (int64_t v : p.al) WRaw(o, v);
      break;
    case 'd':
      W32(o, uint32_t(p.ad.size())); W32(o, 0); W32(o, uint32_t(p.ad.size() * 8));
      for (double v : p.ad) WRaw(o, v);
      break;
    case 'f':
      W32(o, uint32_t(p.af.size())); W32(o, 0); W32(o, uint32_t(p.af.size() * 4));
      for (float v : p.af) WRaw(o, v);
      break;
  }
}

// FBX 7.4: u32 end offset, u32 prop count, u32 prop bytes, u8 name length.
void WriteNode(std::vector<uint8_t>& o, const Node& n) {
  const size_t start = o.size();
  W32(o, 0); W32(o, uint32_t(n.props.size())); W32(o, 0);
  o.push_back(uint8_t(n.name.size()));
  o.insert(o.end(), n.name.begin(), n.name.end());
  const size_t props_at = o.size();
  for (const auto& p : n.props) WriteProp(o, p);
  const uint32_t prop_len = uint32_t(o.size() - props_at);
  std::memcpy(&o[start + 8], &prop_len, 4);
  if (!n.children.empty()) {
    for (const auto& c : n.children) WriteNode(o, *c);
    o.insert(o.end(), 13, 0);  // null record
  }
  const uint32_t end = uint32_t(o.size());
  std::memcpy(&o[start], &end, 4);
}

}  // namespace

Document::Document(double unit_scale_cm, int up_axis) {
  Node& hdr = root.Add("FBXHeaderExtension");
  hdr.Add("FBXHeaderVersion").I(1003);
  hdr.Add("FBXVersion").I(7400);
  hdr.Add("Creator").S("SvR2011 Mod Maker");
  root.Add("Creator").S("SvR2011 Mod Maker");
  Node& gs = root.Add("GlobalSettings");
  gs.Add("Version").I(1000);
  Node& p70 = gs.Add("Properties70");
  // Y-up: up = Y, front = Z (parity +1), coord = X. Z-up: up Z, front -Y.
  if (up_axis == 1) {
    PInt(p70, "UpAxis", 1); PInt(p70, "UpAxisSign", 1);
    PInt(p70, "FrontAxis", 2); PInt(p70, "FrontAxisSign", 1);
  } else {
    PInt(p70, "UpAxis", 2); PInt(p70, "UpAxisSign", 1);
    PInt(p70, "FrontAxis", 1); PInt(p70, "FrontAxisSign", -1);
  }
  PInt(p70, "CoordAxis", 0); PInt(p70, "CoordAxisSign", 1);
  PInt(p70, "OriginalUpAxis", up_axis); PInt(p70, "OriginalUpAxisSign", 1);
  PDouble(p70, "UnitScaleFactor", unit_scale_cm);
  PDouble(p70, "OriginalUnitScaleFactor", unit_scale_cm);
  Node& defs = root.Add("Definitions");
  defs.Add("Version").I(100);
  objects = &root.Add("Objects");
  connections = &root.Add("Connections");
}

void Document::Connect(int64_t child, int64_t parent, const std::string& property) {
  Node& c = connections->Add("C");
  c.S(property.empty() ? "OO" : "OP").L(child).L(parent);
  if (!property.empty()) c.S(property);
}

std::vector<uint8_t> Document::Write() const {
  std::vector<uint8_t> o;
  const char magic[] = "Kaydara FBX Binary  ";
  o.insert(o.end(), magic, magic + 20);
  o.push_back(0x00); o.push_back(0x1A); o.push_back(0x00);
  W32(o, 7400);
  for (const auto& c : root.children) WriteNode(o, *c);
  o.insert(o.end(), 13, 0);  // end of top level
  // footer: id, padding to 16, version, 120 zero bytes, magic
  static const uint8_t foot_id[16] = {0xfa, 0xbc, 0xab, 0x09, 0xd0, 0xc8, 0xd4, 0x66,
                                      0xb1, 0x76, 0xfb, 0x83, 0x1c, 0xf7, 0x26, 0x7e};
  o.insert(o.end(), foot_id, foot_id + 16);
  o.insert(o.end(), 4, 0);
  while (o.size() % 16) o.push_back(0);
  W32(o, 7400);
  o.insert(o.end(), 120, 0);
  static const uint8_t foot_magic[16] = {0xf8, 0x5a, 0x8c, 0x6a, 0xde, 0xf5, 0xd9, 0x7e,
                                         0xec, 0xe9, 0x0c, 0xe3, 0x75, 0x8f, 0x29, 0x0b};
  o.insert(o.end(), foot_magic, foot_magic + 16);
  return o;
}

}  // namespace svrfmt::fbx
