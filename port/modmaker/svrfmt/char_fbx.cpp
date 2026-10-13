// A superstar's model to FBX (char_fbx.h). The model as the 3D view reads it
// (char_preview.cpp): an attire's EMD entry is a PACH whose JBOY children are
// the models (child 0 the body, with the whole skeleton; the others' nodes are
// matched to it by name), their vertices already in the body's model space
// (the bind pose); a mesh's weights name palette slots, the palette holds the
// model's node index + 1. A node's local matrix is T(t) R(r), R = Rz Ry Rx.
//
// FBX: the same axes turn as the arena export (180 degrees about X: game Y
// points down), units 10 cm (unit scale 10). Bones are LimbNodes whose
// Lcl Translation / Rotation are the bind pose (so the file's rest pose is the
// game's); each mesh model gets a Skin with a Cluster per bone it uses
// (TransformLink = the bone's bind world matrix, Transform = its inverse
// times the mesh's world, as Blender writes them), and a BindPose.
#include "char_fbx.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <functional>
#include <map>
#include <set>

#include "fbx_write.h"
#include "jboy.h"
#include "pac.h"
#include "png.h"
#include "texture.h"
#include "ufbx.h"

namespace svrfmt {

namespace fs = std::filesystem;

namespace {

std::string PathStr(const fs::path& p) {  // (UTF-8, as the file helpers take)
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

struct M {  // 3x3 rotation + translation, column vectors: p' = r p + t
  double r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  double t[3] = {0, 0, 0};
};
M Mul(const M& a, const M& b) {
  M o;
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
    o.t[i] = a.r[i][0] * b.t[0] + a.r[i][1] * b.t[1] + a.r[i][2] * b.t[2] + a.t[i];
  }
  return o;
}
M Inv(const M& a) {
  M o;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[j][i];
  for (int i = 0; i < 3; ++i) o.t[i] = -(o.r[i][0] * a.t[0] + o.r[i][1] * a.t[1] + o.r[i][2] * a.t[2]);
  return o;
}
M Local(const float t[3], const float e[3]) {  // T(t) Rz Ry Rx
  M m;
  const double cx = std::cos(e[0]), sx = std::sin(e[0]), cy = std::cos(e[1]), sy = std::sin(e[1]), cz = std::cos(e[2]), sz = std::sin(e[2]);
  m.r[0][0] = cz * cy, m.r[0][1] = cz * sy * sx - sz * cx, m.r[0][2] = cz * sy * cx + sz * sx;
  m.r[1][0] = sz * cy, m.r[1][1] = sz * sy * sx + cz * cx, m.r[1][2] = sz * sy * cx - cz * sx;
  m.r[2][0] = -sy, m.r[2][1] = cy * sx, m.r[2][2] = cy * cx;
  for (int k = 0; k < 3; ++k) m.t[k] = t[k];
  return m;
}
// R = Rz Ry Rx -> (x, y, z) in degrees (FBX's eEulerXYZ)
std::array<double, 3> EulerDeg(const double r[3][3]) {
  const double sy = std::clamp(-r[2][0], -1.0, 1.0);
  const double ey = std::asin(sy);
  double ex, ez;
  if (std::fabs(sy) < 0.99999) ex = std::atan2(r[2][1], r[2][2]), ez = std::atan2(r[1][0], r[0][0]);
  else ex = std::atan2(-r[1][2], r[1][1]), ez = 0;
  const double k = 180.0 / 3.14159265358979323846;
  return {ex * k, ey * k, ez * k};
}
// The axes turn (180 degrees about X): C = diag(1, -1, -1)
M Turn(const M& a) {
  M o = a;
  for (int j = 0; j < 3; ++j) o.r[1][j] = -a.r[1][j], o.r[2][j] = -a.r[2][j];
  o.t[1] = -a.t[1], o.t[2] = -a.t[2];
  return o;
}
std::vector<double> FbxMatrix(const M& m) {  // FBX stores the transpose (rows = basis vectors, translation last)
  return {m.r[0][0], m.r[1][0], m.r[2][0], 0, m.r[0][1], m.r[1][1], m.r[2][1], 0,
          m.r[0][2], m.r[1][2], m.r[2][2], 0, m.t[0],    m.t[1],    m.t[2],    1};
}

bool IsBundle(const Bytes& b) { return b.size() > 16 && Le32(&b[4]) == 0x100 && Le32(&b[12]) == 0x10; }
// A texture bundle: count, then 32-byte records {name[16], "dds", size, offset}
void ReadBundle(const Bytes& b, std::map<std::string, Bytes>& out) {
  const uint32_t n = Le32(&b[0]);
  for (uint32_t i = 0; i < n && 16 + 32 * (i + 1) <= b.size(); ++i) {
    const uint8_t* r = &b[16 + 32 * i];
    const std::string name(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 16));
    const uint32_t size = Le32(r + 20), off = Le32(r + 24);
    if (off + size <= b.size() && !out.count(name)) out[name] = Bytes(b.begin() + off, b.begin() + off + size);
  }
}

int DiffuseSlot(const Mesh& s) {
  for (const auto& p : s.params)
    if (p.type == 0x0f && p.name == "texDiffuse" && p.value.size() >= 4) return int(Be32(p.value.data()));
  return -1;
}

struct Bone {
  std::string name;
  int parent = -1;
  M local, world;  // (game axes)
};
struct Part {
  std::string name, texture;
  std::vector<std::array<double, 3>> pos, nrm;
  std::vector<std::array<double, 2>> uv;
  std::vector<std::array<std::pair<int, double>, 4>> inf;  // (bone, weight), weight 0 = none
  std::vector<std::array<int, 3>> tris;
};
struct Character {
  std::vector<Bone> bones;
  std::vector<Part> parts;
  std::map<std::string, Bytes> textures;  // name -> DDS
};

bool ReadCharacter(const Bytes& pac, int attire, Character& ch, std::string& err) {
  Epac e;
  if (!EpacRead(pac, e)) return err = "not a model pac", false;
  const char want = char('0' + std::clamp(attire, 1, 9) - 1);
  const EpacEntry* entry = nullptr;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name.size() < 8 || en.name[7] != '2' || en.data.empty()) continue;
      if (en.name[6] == want) entry = &en;
      std::vector<PachEntry> kids;  // (the textures of every attire: attire 2 can use attire 1's sheets)
      if (PachRead(Unpack(en.data), kids))
        for (const auto& k : kids)
          if (const Bytes raw = Unpack(k.data); IsBundle(raw)) ReadBundle(raw, ch.textures);
    }
  if (!entry) return err = "the pac has no attire " + std::to_string(attire), false;
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(entry->data), kids)) return err = "the attire is not a PACH", false;
  std::vector<std::pair<uint32_t, Model>> models;
  for (const auto& k : kids) {
    const Bytes raw = Unpack(k.data);
    Model m;
    if (IsJboy(raw) && JboyRead(raw, m)) models.push_back({k.id, std::move(m)});
  }
  if (models.empty() || models[0].second.nodes.empty()) return err = "no body model (child 0) with a skeleton", false;
  const Model& body = models[0].second;
  std::map<std::string, int> by_name;
  for (const auto& n : body.nodes) {
    Bone b;
    b.name = n.name;
    b.parent = n.parent >= 0 && n.parent < int(body.nodes.size()) ? n.parent : -1;
    b.local = Local(n.t, n.r);
    by_name.emplace(b.name, int(ch.bones.size()));
    ch.bones.push_back(b);
  }
  std::vector<bool> done(ch.bones.size());
  std::function<void(int)> world = [&](int i) {
    if (done[size_t(i)]) return;
    Bone& b = ch.bones[size_t(i)];
    if (b.parent >= 0 && b.parent != i) world(b.parent), b.world = Mul(ch.bones[size_t(b.parent)].world, b.local);
    else b.world = b.local;
    done[size_t(i)] = true;
  };
  for (int i = 0; i < int(ch.bones.size()); ++i) world(i);
  for (const auto& [id, m] : models) {
    if (m.name.rfind("blood", 0) == 0 || (id >= 0x7530 && id <= 0x7532)) continue;  // (blood overlays)
    for (size_t si = 0; si < m.meshes.size(); ++si) {
      const Mesh& s = m.meshes[si];
      Part p;
      p.name = (m.name.empty() ? "model" + std::to_string(id) : m.name) + (m.meshes.size() > 1 ? "_" + std::to_string(si) : "");
      const int slot = DiffuseSlot(s);
      p.texture = slot >= 0 && slot < int(m.textures.size()) ? m.textures[size_t(slot)] : "";
      auto bone_of = [&](int palette_slot) {
        if (palette_slot < 0 || palette_slot >= int(s.palette.size())) return 0;
        const int local = s.palette[size_t(palette_slot)] - 1;
        if (local < 0 || local >= int(m.nodes.size())) return 0;
        const auto it = by_name.find(m.nodes[size_t(local)].name);
        return it == by_name.end() ? 0 : it->second;
      };
      const size_t vc = s.verts.size();
      for (size_t k = 0; k < vc; ++k) {
        p.pos.push_back({s.verts[k].pos[0], s.verts[k].pos[1], s.verts[k].pos[2]});
        p.nrm.push_back({s.verts[k].normal[0], s.verts[k].normal[1], s.verts[k].normal[2]});
        p.uv.push_back(k < s.uvs.size() ? std::array<double, 2>{s.uvs[k][0], s.uvs[k][1]} : std::array<double, 2>{0, 0});
        std::array<std::pair<int, double>, 4> f{};
        int n = 0;
        double sum = 0;
        for (size_t j = 0; j < s.weights.size() && n < 4; ++j) {
          if (k >= s.weights[j].size() || s.weights[j][k].bones[0] == 255) continue;
          f[size_t(n)] = {bone_of(s.weights[j][k].bones[0]), s.weights[j][k].weight};
          sum += f[size_t(n++)].second;
        }
        if (n == 0 || sum <= 0) f = {}, f[0] = {bone_of(0), 1.0};
        else
          for (int j = 0; j < n; ++j) f[size_t(j)].second /= sum;
        p.inf.push_back(f);
      }
      for (const auto& st : s.strips)
        for (const auto& t : StripToTriangles(st.indices))
          if (t[0] < vc && t[1] < vc && t[2] < vc) p.tris.push_back({t[0], t[1], t[2]});
      if (!p.tris.empty()) ch.parts.push_back(std::move(p));
    }
  }
  if (ch.parts.empty()) return err = "the attire has no meshes", false;
  return true;
}

}  // namespace

bool ExportCharacterFbx(const Bytes& pac, int attire, const std::string& out_dir, const std::string& name, CharExportReport& rep) {
  Character ch;
  std::string err;
  if (!ReadCharacter(pac, attire, ch, err)) {
    rep.warnings.push_back(err);
    return false;
  }
  std::error_code ec;
  fs::create_directories(fs::u8path(out_dir) / "textures", ec);
  // textures -> PNG
  std::set<std::string> written;
  for (const auto& [tname, dds] : ch.textures) {
    Image img;
    if (DdsDecode(dds, img) && SavePng(PathStr(fs::u8path(out_dir) / "textures" / fs::u8path(tname + ".png")), img)) {
      written.insert(tname);
      ++rep.textures;
    } else {
      rep.warnings.push_back("texture " + tname + ": not written");
    }
  }
  fbx::Document doc(10.0, 1);
  // the skeleton: LimbNodes in the bind pose (roots turned with the axes)
  std::vector<int64_t> bone_id(ch.bones.size());
  std::vector<M> bind(ch.bones.size());
  for (size_t i = 0; i < ch.bones.size(); ++i) {
    const Bone& b = ch.bones[i];
    bind[i] = Turn(b.world);
    const M local = b.parent < 0 ? Turn(b.local) : b.local;
    const int64_t id = doc.NewId(), attr = doc.NewId();
    bone_id[i] = id;
    fbx::Node& na = doc.objects->Add("NodeAttribute");
    na.L(attr).S(fbx::ObjName(b.name, "NodeAttribute")).S("LimbNode");
    na.Add("TypeFlags").S("Skeleton");
    fbx::Node& mo = doc.objects->Add("Model");
    mo.L(id).S(fbx::ObjName(b.name, "Model")).S("LimbNode");
    mo.Add("Version").I(232);
    fbx::Node& p = mo.Add("Properties70");
    const auto e = EulerDeg(local.r);
    fbx::PVec(p, "Lcl Translation", "Lcl Translation", local.t[0], local.t[1], local.t[2]);
    fbx::PVec(p, "Lcl Rotation", "Lcl Rotation", e[0], e[1], e[2]);
    mo.Add("Shading").C(true);
    mo.Add("Culling").S("CullingOff");
    doc.Connect(attr, id);
  }
  for (size_t i = 0; i < ch.bones.size(); ++i) doc.Connect(bone_id[i], ch.bones[i].parent >= 0 ? bone_id[size_t(ch.bones[i].parent)] : 0);
  rep.bones = int(ch.bones.size());
  // the bind pose (every bone and mesh model's world matrix)
  fbx::Node& pose = doc.objects->Add("Pose");
  const int64_t pose_id = doc.NewId();
  pose.L(pose_id).S(fbx::ObjName("BindPose", "Pose")).S("BindPose");
  pose.Add("Type").S("BindPose");
  pose.Add("Version").I(100);
  fbx::Node& npn = pose.Add("NbPoseNodes");
  int pose_nodes = 0;
  for (size_t i = 0; i < ch.bones.size(); ++i) {
    fbx::Node& pn = pose.Add("PoseNode");
    pn.Add("Node").L(bone_id[i]);
    pn.Add("Matrix").Ad(FbxMatrix(bind[i]));
    ++pose_nodes;
  }
  // the meshes, each skinned
  for (const Part& part : ch.parts) {
    std::vector<double> verts, normals, uvs;
    std::vector<int32_t> poly, uv_index;
    for (size_t k = 0; k < part.pos.size(); ++k) {
      verts.insert(verts.end(), {part.pos[k][0], -part.pos[k][1], -part.pos[k][2]});
      uvs.insert(uvs.end(), {part.uv[k][0], 1.0 - part.uv[k][1]});
    }
    for (const auto& t : part.tris)
      for (int c = 0; c < 3; ++c) {
        poly.push_back(c == 2 ? -(t[size_t(c)] + 1) : t[size_t(c)]);
        const auto& n = part.nrm[size_t(t[size_t(c)])];
        normals.insert(normals.end(), {n[0], -n[1], -n[2]});
        uv_index.push_back(t[size_t(c)]);
      }
    const int64_t gid = doc.NewId(), mid = doc.NewId();
    fbx::Node& g = doc.objects->Add("Geometry");
    g.L(gid).S(fbx::ObjName(part.name, "Geometry")).S("Mesh");
    g.Add("GeometryVersion").I(124);
    g.Add("Vertices").Ad(verts);
    g.Add("PolygonVertexIndex").Ai(poly);
    {
      fbx::Node& n = g.Add("LayerElementNormal").I(0);
      n.Add("Version").I(101), n.Add("Name").S("");
      n.Add("MappingInformationType").S("ByPolygonVertex");
      n.Add("ReferenceInformationType").S("Direct");
      n.Add("Normals").Ad(normals);
    }
    {
      fbx::Node& n = g.Add("LayerElementUV").I(0);
      n.Add("Version").I(101), n.Add("Name").S("UVMap");
      n.Add("MappingInformationType").S("ByPolygonVertex");
      n.Add("ReferenceInformationType").S("IndexToDirect");
      n.Add("UV").Ad(uvs);
      n.Add("UVIndex").Ai(uv_index);
    }
    {
      fbx::Node& n = g.Add("LayerElementMaterial").I(0);
      n.Add("Version").I(101), n.Add("Name").S("");
      n.Add("MappingInformationType").S("AllSame");
      n.Add("ReferenceInformationType").S("IndexToDirect");
      n.Add("Materials").Ai({0});
    }
    {
      fbx::Node& l = g.Add("Layer").I(0);
      l.Add("Version").I(100);
      for (const char* t : {"LayerElementNormal", "LayerElementUV", "LayerElementMaterial"}) {
        fbx::Node& le = l.Add("LayerElement");
        le.Add("Type").S(t);
        le.Add("TypedIndex").I(0);
      }
    }
    fbx::Node& mo = doc.objects->Add("Model");
    mo.L(mid).S(fbx::ObjName(part.name, "Model")).S("Mesh");
    mo.Add("Version").I(232);
    mo.Add("Properties70");
    mo.Add("Shading").C(true);
    mo.Add("Culling").S("CullingOff");
    doc.Connect(mid, 0);
    doc.Connect(gid, mid);
    {
      fbx::Node& pn = pose.Add("PoseNode");
      pn.Add("Node").L(mid);
      pn.Add("Matrix").Ad(FbxMatrix(M()));
      ++pose_nodes;
    }
    // material + its texture
    const int64_t matid = doc.NewId();
    fbx::Node& mat = doc.objects->Add("Material");
    mat.L(matid).S(fbx::ObjName(part.name + "_mat", "Material")).S("");
    mat.Add("Version").I(102);
    mat.Add("ShadingModel").S("phong");
    mat.Add("MultiLayer").I(0);
    fbx::PVec(mat.Add("Properties70"), "DiffuseColor", "Color", 1, 1, 1);
    doc.Connect(matid, mid);
    std::string tex;  // (the mesh's texDiffuse, as named in the bundles: case may differ)
    for (const auto& w : written)
      if (!part.texture.empty() && (w == part.texture || std::equal(w.begin(), w.end(), part.texture.begin(), part.texture.end(),
                                                                     [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); })))
        tex = w;
    if (!tex.empty()) {
      const std::string rel = "textures/" + tex + ".png";
      const std::string abs = PathStr(fs::u8path(out_dir) / "textures" / fs::u8path(tex + ".png"));
      const int64_t tid = doc.NewId(), vid = doc.NewId();
      fbx::Node& tx = doc.objects->Add("Texture");
      tx.L(tid).S(fbx::ObjName(tex, "Texture")).S("");
      tx.Add("Type").S("TextureVideoClip");
      tx.Add("Version").I(202);
      tx.Add("TextureName").S(fbx::ObjName(tex, "Texture"));
      tx.Add("Media").S(fbx::ObjName(tex, "Video"));
      tx.Add("FileName").S(abs);
      tx.Add("RelativeFilename").S(rel);
      fbx::Node& vd = doc.objects->Add("Video");
      vd.L(vid).S(fbx::ObjName(tex, "Video")).S("Clip");
      vd.Add("Type").S("Clip");
      vd.Add("Properties70").Add("P").S("Path").S("KString").S("XRefUrl").S("").S(abs);
      vd.Add("UseMipMap").I(0);
      vd.Add("Filename").S(abs);
      vd.Add("RelativeFilename").S(rel);
      doc.Connect(vid, tid);
      doc.Connect(tid, matid, "DiffuseColor");
    }
    // the skin: a cluster per bone the mesh uses
    std::map<int, std::pair<std::vector<int32_t>, std::vector<double>>> by_bone;
    for (size_t k = 0; k < part.inf.size(); ++k)
      for (const auto& [b, w] : part.inf[k])
        if (w > 0) by_bone[b].first.push_back(int32_t(k)), by_bone[b].second.push_back(w);
    const int64_t skin = doc.NewId();
    fbx::Node& sk = doc.objects->Add("Deformer");
    sk.L(skin).S(fbx::ObjName(part.name, "Deformer")).S("Skin");
    sk.Add("Version").I(101);
    sk.Add("Link_DeformAcuracy").D(50);
    doc.Connect(skin, gid);
    for (auto& [b, iw] : by_bone) {
      const int64_t cl = doc.NewId();
      fbx::Node& c = doc.objects->Add("Deformer");
      c.L(cl).S(fbx::ObjName(ch.bones[size_t(b)].name, "SubDeformer")).S("Cluster");
      c.Add("Version").I(100);
      c.Add("UserData").S("").S("");
      c.Add("Indexes").Ai(iw.first);
      c.Add("Weights").Ad(iw.second);
      c.Add("Transform").Ad(FbxMatrix(Inv(bind[size_t(b)])));  // (inverse bind times the mesh's world: identity)
      c.Add("TransformLink").Ad(FbxMatrix(bind[size_t(b)]));
      doc.Connect(cl, skin);
      doc.Connect(bone_id[size_t(b)], cl);
    }
    ++rep.models;
    ++rep.meshes;
    rep.vertices += int(part.pos.size());
    rep.triangles += int(part.tris.size());
  }
  npn.I(pose_nodes);
  doc.Connect(pose_id, 0);
  if (!WriteFile(PathStr(fs::u8path(out_dir) / fs::u8path(name + ".fbx")), doc.Write())) {
    rep.warnings.push_back("could not write the FBX");
    return false;
  }
  return true;
}

bool ReadFbxSkeleton(const std::string& path, std::vector<FbxBone>& out, std::string& err) {
  ufbx_load_opts lo = {};
  lo.target_axes = ufbx_axes_right_handed_y_up;
  lo.target_unit_meters = 0.1f;  // (game units: 10 cm)
  ufbx_error ue;
  ufbx_scene* scene = ufbx_load_file(path.c_str(), &lo, &ue);
  if (!scene) return err = "ufbx: " + std::string(ue.description.data, ue.description.length), false;
  std::set<const ufbx_node*> skinned;
  for (size_t i = 0; i < scene->skin_clusters.count; ++i)
    if (scene->skin_clusters.data[i]->bone_node) skinned.insert(scene->skin_clusters.data[i]->bone_node);
  std::map<const ufbx_node*, int> index;
  for (size_t i = 0; i < scene->nodes.count; ++i) {
    const ufbx_node* n = scene->nodes.data[i];
    if (!n->bone && !skinned.count(n)) continue;
    index[n] = int(out.size());
    FbxBone b;
    b.name.assign(n->name.data, n->name.length);
    b.pos[0] = float(n->node_to_world.m03), b.pos[1] = float(n->node_to_world.m13), b.pos[2] = float(n->node_to_world.m23);
    b.skinned = skinned.count(n) != 0;
    out.push_back(b);
  }
  for (size_t i = 0; i < scene->nodes.count; ++i) {
    const ufbx_node* n = scene->nodes.data[i];
    const auto it = index.find(n);
    if (it == index.end()) continue;
    for (const ufbx_node* p = n->parent; p; p = p->parent)
      if (const auto pi = index.find(p); pi != index.end()) {
        out[size_t(it->second)].parent = pi->second;
        break;
      }
  }
  ufbx_free_scene(scene);
  if (out.empty()) return err = "the FBX has no skeleton (no bones, no skin)", false;
  return true;
}

std::vector<std::string> CheckCharacterFbx(const Bytes& pac, int attire, const std::string& fbx_path) {
  std::vector<std::string> out;
  Character ch;
  std::string err;
  if (!ReadCharacter(pac, attire, ch, err)) return {"FAILED: " + err};
  ufbx_load_opts lo = {};  // (as written: the file's own axes and units)
  ufbx_error ue;
  ufbx_scene* scene = ufbx_load_file(fbx_path.c_str(), &lo, &ue);
  if (!scene) return {"FAILED: ufbx: " + std::string(ue.description.data, ue.description.length)};
  // bones
  std::map<std::string, const ufbx_node*> nodes;
  for (size_t i = 0; i < scene->nodes.count; ++i) nodes[std::string(scene->nodes.data[i]->name.data)] = scene->nodes.data[i];
  int found = 0;
  double worst = 0;
  std::string worst_bone;
  for (const Bone& b : ch.bones) {
    const auto it = nodes.find(b.name);
    if (it == nodes.end()) continue;
    ++found;
    const M want = Turn(b.world);
    const ufbx_matrix& m = it->second->node_to_world;  // (the rest pose: the Lcl transforms)
    const double got[3][4] = {{m.m00, m.m01, m.m02, m.m03}, {m.m10, m.m11, m.m12, m.m13}, {m.m20, m.m21, m.m22, m.m23}};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c)
        if (std::fabs(got[r][c] - want.r[r][c]) > worst) worst = std::fabs(got[r][c] - want.r[r][c]), worst_bone = b.name;
      if (std::fabs(got[r][3] - want.t[r]) * 0.1 > worst) worst = std::fabs(got[r][3] - want.t[r]) * 0.1, worst_bone = b.name;
    }
  }
  out.push_back("bones: " + std::to_string(found) + " of " + std::to_string(ch.bones.size()) + " found; rest pose largest difference " +
                std::to_string(worst) + " (rotation, or translation / 10) at " + worst_bone);
  // clusters' bind matrices against the rest pose (Blender takes the bind pose from them)
  double cworst = 0;
  for (size_t i = 0; i < scene->skin_clusters.count; ++i) {
    const ufbx_skin_cluster* c = scene->skin_clusters.data[i];
    if (!c->bone_node) continue;
    const ufbx_matrix& a = c->bind_to_world;
    const ufbx_matrix& b = c->bone_node->node_to_world;
    const double d[] = {a.m00 - b.m00, a.m11 - b.m11, a.m22 - b.m22, (a.m03 - b.m03) * 0.1, (a.m13 - b.m13) * 0.1, (a.m23 - b.m23) * 0.1};
    for (double x : d) cworst = std::max(cworst, std::fabs(x));
  }
  out.push_back("clusters: " + std::to_string(scene->skin_clusters.count) + "; bind against rest largest difference " + std::to_string(cworst));
  // vertices and weights
  size_t verts = 0, want_verts = 0;
  for (const Part& p : ch.parts) want_verts += p.pos.size();
  for (size_t i = 0; i < scene->meshes.count; ++i) verts += scene->meshes.data[i]->num_vertices;
  double wworst = 0;
  size_t unweighted = 0;
  for (size_t i = 0; i < scene->skin_deformers.count; ++i) {
    const ufbx_skin_deformer* s = scene->skin_deformers.data[i];
    for (size_t v = 0; v < s->vertices.count; ++v) {
      double sum = 0;
      for (uint32_t k = 0; k < s->vertices.data[v].num_weights; ++k) sum += s->weights.data[s->vertices.data[v].weight_begin + k].weight;
      if (s->vertices.data[v].num_weights == 0) ++unweighted;
      else wworst = std::max(wworst, std::fabs(sum - 1));
    }
  }
  out.push_back("vertices: " + std::to_string(verts) + " (the pac: " + std::to_string(want_verts) + "); weight sums off by at most " +
                std::to_string(wworst) + ", " + std::to_string(unweighted) + " vertices without weights");
  out.push_back("meshes: " + std::to_string(scene->meshes.count) + ", materials: " + std::to_string(scene->materials.count) +
                ", textures: " + std::to_string(scene->textures.count));
  ufbx_free_scene(scene);
  return out;
}

}  // namespace svrfmt
