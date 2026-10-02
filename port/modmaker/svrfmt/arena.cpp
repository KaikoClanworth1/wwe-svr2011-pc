#include "arena.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

#include "fbx_write.h"
#include "png.h"

namespace fs = std::filesystem;

namespace svrfmt {

namespace {
std::string PathStr(const fs::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}
}  // namespace

const char* ZoneName(Zone z) {
  switch (z) {
    case Zone::kRing: return "ring";
    case Zone::kRingside: return "ringside";
    case Zone::kEntrance: return "entrance";
    default: return "free";
  }
}

Zone ZoneOf(const std::string& n) {
  auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
  if (starts("ar_post") || starts("ar_tb") || n == "ar_ring" || starts("ar_rope") || starts("ar_cover") ||
      starts("bs_ar_epron") || starts("rop_"))
    return Zone::kRing;
  if (starts("ar_fen") || starts("ar_dwnmt") || starts("ar_sak") || starts("ji_") || starts("ar_case") ||
      starts("ar_ring_sita"))
    return Zone::kRingside;
  if (starts("slo_") || starts("sd_titan") || starts("St_stair"))
    return Zone::kEntrance;
  return Zone::kFree;
}

bool Arena::Load(const std::string& path, std::string* error) {
  auto fail = [&](const std::string& why) { if (error) *error = why; return false; };
  Bytes d;
  if (!ReadFile(path, d)) return fail("cannot read " + path);
  if (!EpacRead(d, epac)) return fail("not an arena archive (EPAC)");
  bool found = false;
  for (size_t g = 0; g < epac.groups.size() && !found; ++g)
    for (size_t e = 0; e < epac.groups[g].entries.size() && !found; ++e)
      if (IsPach(epac.groups[g].entries[e].data)) { group = g; entry = e; found = true; }
  if (!found || !PachRead(epac.groups[group].entries[entry].data, entries)) return fail("no stage index (PACH)");
  models.clear();
  bundles.clear();
  for (const auto& e : entries) {
    const Bytes raw = Unpack(e.data);
    if (IsJboy(raw)) {
      ArenaModel am;
      am.id = e.id;
      std::string err;
      if (!JboyRead(raw, am.model, &err)) return fail("model " + std::to_string(e.id) + ": " + err);
      am.zone = ZoneOf(am.model.name);
      models.push_back(std::move(am));
    } else if (IsTextureBundle(raw)) {
      ArenaTextureSet ts;
      ts.id = e.id;
      if (BundleRead(raw, ts.textures)) bundles.push_back(std::move(ts));
    }
  }
  return true;
}

Bytes Arena::Save() const {
  std::vector<PachEntry> out = entries;
  for (auto& e : out) {
    for (const auto& m : models)
      if (m.id == e.id && m.changed) e.data = BpeEncodeStored(JboyWrite(m.model));
    for (const auto& b : bundles)
      if (b.id == e.id && b.changed) e.data = BpeEncodeStored(BundleWrite(b.textures));
  }
  Epac e = epac;
  e.groups[group].entries[entry].data = PachWrite(out);
  return EpacWrite(e);
}

BundleTexture* Arena::FindTexture(const std::string& name) {
  for (auto& b : bundles)
    for (auto& t : b.textures)
      if (t.name == name) return &t;
  return nullptr;
}

// ------------------------------------------------------------ export

namespace {

std::string JsonStr(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
    else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else if (c >= 0x80) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }  // latin-1 byte
    else o += char(c);
  }
  return o + "\"";
}

std::string Hex4(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%04x", v); return b; }

// Object names must be unique and readable: name__id (ids are unique).
std::string ObjectName(const ArenaModel& m) { return m.model.name + "__" + Hex4(m.id); }

// Texture slot of a mesh's diffuse map, or -1.
int DiffuseSlot(const Mesh& s) {
  for (const auto& p : s.params)
    if (p.type == 0x0f && p.name == "texDiffuse" && p.value.size() >= 4) return int32_t(Be32(p.value.data()));
  return -1;
}

const char* FormatName(DxtFormat f) {
  switch (f) {
    case DxtFormat::kDxt1: return "DXT1";
    case DxtFormat::kDxt3: return "DXT3";
    case DxtFormat::kDxt5: return "DXT5";
    case DxtFormat::kArgb: return "ARGB";
    default: return "unknown";
  }
}

}  // namespace

bool ExportArena(const Arena& a, const std::string& out_dir, const std::string& title, ExportReport& rep) {
  std::error_code ec;
  fs::create_directories(fs::u8path(out_dir) / "textures", ec);
  if (ec) { rep.warnings.push_back("cannot create " + out_dir); return false; }

  // textures -> PNG (first bundle wins on duplicate names)
  std::set<std::string> tex_done;
  std::string tex_json;
  for (const auto& b : a.bundles)
    for (const auto& t : b.textures) {
      if (!tex_done.insert(t.name).second) continue;
      DdsInfo info;
      Image img;
      const bool ok = DdsInfoOf(t.data, info) && DdsDecode(t.data, img);
      if (!ok) {
        ++rep.textures_failed;
        rep.warnings.push_back("texture " + t.name + ": unreadable, kept as is");
      } else if (!SavePng(PathStr(fs::u8path(out_dir) / "textures" / (t.name + ".png")), img)) {
        ++rep.textures_failed;
        rep.warnings.push_back("texture " + t.name + ": PNG write failed");
      } else {
        ++rep.textures;
      }
      if (!tex_json.empty()) tex_json += ",\n";
      tex_json += "    " + JsonStr(t.name) + ": {\"bundle\": " + std::to_string(b.id) + ", \"format\": \"" +
                  FormatName(info.format) + "\", \"width\": " + std::to_string(info.w) + ", \"height\": " +
                  std::to_string(info.h) + ", \"mips\": " + std::to_string(info.mips) + "}";
    }

  // models -> FBX. Game units are 10 cm; game Y points down (see below).
  fbx::Document doc(10.0, 1);
  std::string model_json;
  for (const auto& am : a.models) {
    const Model& m = am.model;
    std::vector<double> verts, normals, uvs, colors;
    std::vector<int32_t> poly, uv_index, col_index, mat_index;
    std::vector<std::string> mat_names, mat_textures;
    int base = 0;
    for (size_t si = 0; si < m.meshes.size(); ++si) {
      const Mesh& s = m.meshes[si];
      for (size_t k = 0; k < s.verts.size(); ++k) {
        const Vertex& v = s.verts[k];
        // game axes: Y points down; a 180-degree turn about X makes it Y-up
        // without mirroring (import applies the same turn back)
        verts.insert(verts.end(), {v.pos[0], -v.pos[1], -v.pos[2]});
        uvs.insert(uvs.end(), {double(s.uvs[k][0]), 1.0 - double(s.uvs[k][1])});
        colors.insert(colors.end(), {(v.color >> 16 & 255) / 255.0, (v.color >> 8 & 255) / 255.0,
                                     (v.color & 255) / 255.0, (v.color >> 24 & 255) / 255.0});
      }
      for (const auto& st : s.strips)
        for (const auto& t : StripToTriangles(st.indices)) {
          for (int c = 0; c < 3; ++c) {
            const int vi = base + t[c];
            poly.push_back(c == 2 ? -(vi + 1) : vi);
            const Vertex& v = s.verts[t[c]];
            normals.insert(normals.end(), {v.normal[0], -v.normal[1], -v.normal[2]});
            uv_index.push_back(vi);
            col_index.push_back(vi);
          }
          mat_index.push_back(int32_t(si));
          ++rep.triangles;
        }
      base += int(s.verts.size());
      const int slot = DiffuseSlot(s);
      mat_names.push_back(ObjectName(am) + "_m" + std::to_string(si));
      mat_textures.push_back(slot >= 0 && slot < int(m.textures.size()) ? m.textures[slot] : "");
      ++rep.meshes;
    }
    if (poly.empty()) continue;
    ++rep.models;
    const std::string oname = ObjectName(am);
    const int64_t gid = doc.NewId(), mid = doc.NewId();
    fbx::Node& g = doc.objects->Add("Geometry");
    g.L(gid).S(fbx::ObjName(oname, "Geometry")).S("Mesh");
    g.Add("GeometryVersion").I(124);
    g.Add("Vertices").Ad(verts);
    g.Add("PolygonVertexIndex").Ai(poly);
    {
      fbx::Node& n = g.Add("LayerElementNormal").I(0);
      n.Add("Version").I(101); n.Add("Name").S("");
      n.Add("MappingInformationType").S("ByPolygonVertex");
      n.Add("ReferenceInformationType").S("Direct");
      n.Add("Normals").Ad(normals);
    }
    {
      fbx::Node& n = g.Add("LayerElementUV").I(0);
      n.Add("Version").I(101); n.Add("Name").S("UVMap");
      n.Add("MappingInformationType").S("ByPolygonVertex");
      n.Add("ReferenceInformationType").S("IndexToDirect");
      n.Add("UV").Ad(uvs);
      n.Add("UVIndex").Ai(uv_index);
    }
    {
      fbx::Node& n = g.Add("LayerElementColor").I(0);
      n.Add("Version").I(101); n.Add("Name").S("Col");
      n.Add("MappingInformationType").S("ByPolygonVertex");
      n.Add("ReferenceInformationType").S("IndexToDirect");
      n.Add("Colors").Ad(colors);
      n.Add("ColorIndex").Ai(col_index);
    }
    {
      fbx::Node& n = g.Add("LayerElementMaterial").I(0);
      n.Add("Version").I(101); n.Add("Name").S("");
      n.Add("MappingInformationType").S("ByPolygon");
      n.Add("ReferenceInformationType").S("IndexToDirect");
      n.Add("Materials").Ai(mat_index);
    }
    {
      fbx::Node& l = g.Add("Layer").I(0);
      l.Add("Version").I(100);
      for (const char* t : {"LayerElementNormal", "LayerElementUV", "LayerElementColor", "LayerElementMaterial"}) {
        fbx::Node& le = l.Add("LayerElement");
        le.Add("Type").S(t);
        le.Add("TypedIndex").I(0);
      }
    }
    fbx::Node& mo = doc.objects->Add("Model");
    mo.L(mid).S(fbx::ObjName(oname, "Model")).S("Mesh");
    mo.Add("Version").I(232);
    {
      fbx::Node& p = mo.Add("Properties70");
      fbx::PCustomInt(p, "svr_id", int32_t(am.id));
      fbx::PString(p, "svr_name", m.name, true);
      fbx::PString(p, "svr_zone", ZoneName(am.zone), true);
    }
    mo.Add("Shading").C(true);
    mo.Add("Culling").S("CullingOff");
    doc.Connect(mid, 0);
    doc.Connect(gid, mid);
    for (size_t si = 0; si < mat_names.size(); ++si) {
      const int64_t matid = doc.NewId();
      fbx::Node& mat = doc.objects->Add("Material");
      mat.L(matid).S(fbx::ObjName(mat_names[si], "Material")).S("");
      mat.Add("Version").I(102);
      mat.Add("ShadingModel").S("phong");
      mat.Add("MultiLayer").I(0);
      fbx::Node& p = mat.Add("Properties70");
      fbx::PVec(p, "DiffuseColor", "Color", 1, 1, 1);
      fbx::PString(p, "svr_shader", m.meshes[si].shader, true);
      doc.Connect(matid, mid);
      if (!mat_textures[si].empty() && tex_done.count(mat_textures[si])) {
        const std::string rel = "textures/" + mat_textures[si] + ".png";
        const std::string abs = PathStr(fs::u8path(out_dir) / "textures" / (mat_textures[si] + ".png"));
        const int64_t tid = doc.NewId(), vid = doc.NewId();
        fbx::Node& tx = doc.objects->Add("Texture");
        tx.L(tid).S(fbx::ObjName(mat_textures[si], "Texture")).S("");
        tx.Add("Type").S("TextureVideoClip");
        tx.Add("Version").I(202);
        tx.Add("TextureName").S(fbx::ObjName(mat_textures[si], "Texture"));
        tx.Add("Media").S(fbx::ObjName(mat_textures[si], "Video"));
        tx.Add("FileName").S(abs);
        tx.Add("RelativeFilename").S(rel);
        fbx::Node& vd = doc.objects->Add("Video");
        vd.L(vid).S(fbx::ObjName(mat_textures[si], "Video")).S("Clip");
        vd.Add("Type").S("Clip");
        fbx::Node& vp = vd.Add("Properties70");
        vp.Add("P").S("Path").S("KString").S("XRefUrl").S("").S(abs);
        vd.Add("UseMipMap").I(0);
        vd.Add("Filename").S(abs);
        vd.Add("RelativeFilename").S(rel);
        doc.Connect(vid, tid);
        doc.Connect(tid, matid, "DiffuseColor");
      }
    }
    // arena.json entry
    std::string meshes;
    for (size_t si = 0; si < m.meshes.size(); ++si) {
      const Mesh& s = m.meshes[si];
      if (!meshes.empty()) meshes += ", ";
      meshes += "{\"shader\": " + JsonStr(s.shader) + ", \"format\": " + std::to_string(s.vfmt) +
                ", \"texture\": " + JsonStr(mat_textures[si]) + ", \"skinned\": " +
                (s.palette.size() > 1 ? "true" : "false") + "}";
    }
    if (!model_json.empty()) model_json += ",\n";
    model_json += "    " + JsonStr(oname) + ": {\"id\": " + std::to_string(am.id) + ", \"name\": " + JsonStr(m.name) +
                  ", \"zone\": \"" + ZoneName(am.zone) + "\", \"nodes\": " + std::to_string(m.nodes.size()) +
                  ", \"meshes\": [" + meshes + "]}";
  }
  if (!WriteFile(PathStr(fs::u8path(out_dir) / "arena.fbx"), doc.Write())) {
    rep.warnings.push_back("cannot write arena.fbx");
    return false;
  }
  const std::string json = "{\n  \"format\": \"svr2011-arena-export\",\n  \"version\": 1,\n  \"arena\": " +
                           JsonStr(title) + ",\n  \"units\": \"1 game unit = 10 cm (FBX unit scale 10); FBX = game axes turned 180 degrees "
                           "about X (game Y points down)\",\n" +
                           "  \"models\": {\n" + model_json + "\n  },\n  \"textures\": {\n" + tex_json + "\n  }\n}\n";
  Bytes jb(json.begin(), json.end());
  WriteFile(PathStr(fs::u8path(out_dir) / "arena.json"), jb);
  return true;
}

}  // namespace svrfmt
