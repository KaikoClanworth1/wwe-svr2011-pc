#include "arena_build.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace svrfmt {

namespace {

bool IsStaticMesh(const Mesh& s) { return s.palette.size() <= 1 && s.weights.size() <= 1; }

int DiffuseSlot(const Mesh& s) {
  for (const auto& p : s.params)
    if (p.type == 0x0f && p.name == "texDiffuse" && p.value.size() >= 4) return int(Be32(p.value.data()));
  return -1;
}

void SetDiffuseSlot(Mesh& s, int slot) {
  for (auto& p : s.params)
    if (p.type == 0x0f && p.name == "texDiffuse" && p.value.size() >= 4) PutBe32(p.value.data(), uint32_t(slot));
}

int TextureSlot(Model& m, const std::string& tex) {
  for (size_t i = 0; i < m.textures.size(); ++i)
    if (m.textures[i] == tex) return int(i);
  m.textures.push_back(tex);
  return int(m.textures.size() - 1);
}

void Bound(Mesh& s) {
  if (s.verts.empty()) return;
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const auto& v : s.verts)
    for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.pos[k]), hi[k] = std::max(hi[k], v.pos[k]);
  const float c[3] = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
  float r = 0;
  for (const auto& v : s.verts)
    r = std::max(r, (v.pos[0] - c[0]) * (v.pos[0] - c[0]) + (v.pos[1] - c[1]) * (v.pos[1] - c[1]) +
                        (v.pos[2] - c[2]) * (v.pos[2] - c[2]));
  s.sphere = {c[0], c[1], c[2], std::sqrt(r)};
}

bool IsFloor(const std::string& n) {
  return n.rfind("ar_ground", 0) == 0 || n.rfind("ar_ring_sita", 0) == 0 || n.rfind("ar_rin_sdw", 0) == 0;
}

size_t MeshBytes(const Mesh& s) {
  size_t n = s.verts.size() * (28 + 8) + s.weights.size() * s.verts.size() * 8;
  for (const auto& st : s.strips) n += st.indices.size() * 2;
  return n;
}

}  // namespace

void MakeEmpty(Arena& a, const EmptyOptions& opt, EmptyReport& rep) {
  std::set<std::string> used;
  for (auto& am : a.models) {
    const bool keep = am.zone == Zone::kRing || (opt.keep_ringside && am.zone == Zone::kRingside) ||
                      (opt.keep_floor && IsFloor(am.model.name));
    if (keep) {
      for (const auto& t : am.model.textures) used.insert(t);
      continue;
    }
    bool any = false;
    for (auto& s : am.model.meshes) {
      if (s.verts.size() <= 1) continue;
      rep.bytes_freed += MeshBytes(s);
      s.verts.resize(1);
      s.uvs.resize(1);
      for (auto& w : s.weights) w.resize(1);
      for (auto& st : s.strips) st.indices.clear();
      Bound(s);
      any = true;
    }
    if (any) {
      am.changed = true;
      ++rep.models_emptied;
    }
  }
  // textures only emptied models used: 4 x 4 (same name, same format)
  for (auto& b : a.bundles)
    for (auto& t : b.textures) {
      if (used.count(t.name)) continue;
      DdsInfo info;
      if (!DdsInfoOf(t.data, info) || (info.w <= 4 && info.h <= 4)) continue;
      Image img;
      img.w = img.h = 4;
      img.rgba.assign(4 * 4 * 4, 128);
      const size_t before = t.data.size();
      t.data = DdsEncode(img, info.format == DxtFormat::kArgb ? DxtFormat::kDxt5 : info.format, false);
      rep.bytes_freed += before > t.data.size() ? before - t.data.size() : 0;
      b.changed = true;
      ++rep.textures_shrunk;
    }
}

int HideCrowd(Arena& a) {
  int n = 0;
  for (auto& e : a.entries) {
    if (e.id != 0x4E20) continue;
    const bool packed = IsBpe(e.data);
    std::vector<PachEntry> subs;
    if (!PachRead(Unpack(e.data), subs)) continue;
    for (auto& sub : subs) {
      const Bytes raw = Unpack(sub.data);
      Model m;
      if (!IsJboy(raw) || !JboyRead(raw, m)) continue;
      for (auto& s : m.meshes)
        for (auto& st : s.strips) st.indices.clear();
      const Bytes w = JboyWrite(m);
      sub.data = IsBpe(sub.data) ? BpeEncode(w) : w;
      ++n;
    }
    const Bytes pach = PachWrite(subs);
    e.data = packed ? BpeEncode(pach) : pach;
  }
  return n;
}

std::vector<int> LibraryModels(const Arena& src) {
  std::vector<int> out;
  for (size_t i = 0; i < src.models.size(); ++i) {
    const auto& am = src.models[i];
    if (am.zone == Zone::kRing) continue;
    size_t tris = 0;
    for (const auto& s : am.model.meshes)
      for (const auto& st : s.strips) tris += st.indices.size();
    if (tris >= 3) out.push_back(int(i));
  }
  return out;
}

int CopyModelInto(Arena& dst, int host_model, const Arena& src, int src_model, std::string* error) {
  auto fail = [&](const std::string& why) {
    if (error) *error = why;
    return -1;
  };
  if (host_model < 0 || host_model >= int(dst.models.size())) return fail("no model to add to");
  if (src_model < 0 || src_model >= int(src.models.size())) return fail("no such model");
  Model& host = dst.models[host_model].model;
  const Mesh* tmpl = nullptr;
  for (const auto& s : host.meshes)
    if (IsStaticMesh(s)) { tmpl = &s; break; }
  if (!tmpl) return fail("the floor model has no plain mesh to copy a material from");
  if (dst.bundles.empty()) return fail("the arena has no texture set");
  const Model& from = src.models[src_model].model;
  const int first = int(host.meshes.size());
  std::vector<Mesh> add;
  for (const auto& sm : from.meshes) {
    bool empty = true;
    for (const auto& st : sm.strips) empty &= st.indices.size() < 3;
    if (empty || sm.verts.empty()) continue;
    // its texture, into this arena
    const int slot = DiffuseSlot(sm);
    std::string tex = slot >= 0 && slot < int(from.textures.size()) ? from.textures[slot] : "";
    const BundleTexture* st = nullptr;
    for (const auto& b : src.bundles)
      for (const auto& t : b.textures)
        if (t.name == tex) st = &t;
    if (st) {
      const BundleTexture* have = dst.FindTexture(tex);
      if (have && have->data != st->data) {
        // a different texture of that name: a new name
        char nm[32];
        for (int n = 0; n < 1000; ++n) {
          std::snprintf(nm, sizeof nm, "lb%03d_%.9s", n, tex.c_str());
          const BundleTexture* other = dst.FindTexture(nm);
          if (!other || other->data == st->data) break;
        }
        tex = nm;
        have = dst.FindTexture(tex);
      }
      if (!have) {
        dst.bundles[0].textures.push_back({tex, "dds", st->data});
        dst.bundles[0].changed = true;
      }
    }
    Mesh s = *tmpl;  // the host's plain material (its shader and params)
    s.verts = sm.verts;
    s.uvs = sm.uvs;
    s.uvs.resize(s.verts.size());
    s.weights.assign(1, std::vector<Weight>(s.verts.size(), Weight{{0, 0, 0, 0}, 1.f}));
    if (s.palette.empty()) s.palette = {0};
    s.strips = sm.strips;
    if (!tex.empty()) SetDiffuseSlot(s, TextureSlot(host, tex));
    Bound(s);
    add.push_back(std::move(s));
  }
  if (add.empty()) return fail("nothing visible in that model");
  for (auto& s : add) host.meshes.push_back(std::move(s));
  dst.models[host_model].changed = true;
  return first;
}

}  // namespace svrfmt
