// Per-mesh materials of a character model pac (model_materials.h).
#include "model_materials.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "app.h"
#include "svrfmt/jboy.h"
#include "svrfmt/pac.h"
#include "svrfmt/texture.h"

namespace mm {
using namespace svrfmt;

namespace {
bool Fail(std::string* error, const std::string& why) {
  if (error) *error = why;
  return false;
}
uint32_t RdBe(const Bytes& b, size_t o = 0) {
  return b.size() >= o + 4 ? uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3] : 0;
}
void WrBe(Bytes& b, size_t o, uint32_t v) {
  if (b.size() < o + 4) return;
  b[o] = uint8_t(v >> 24), b[o + 1] = uint8_t(v >> 16), b[o + 2] = uint8_t(v >> 8), b[o + 3] = uint8_t(v);
}
float FBe(const Bytes& b, size_t o = 0) {
  const uint32_t u = RdBe(b, o);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void FWr(Bytes& b, size_t o, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  WrBe(b, o, u);
}
Param* Find(Mesh& s, const char* name) {
  for (auto& p : s.params)
    if (p.name == name) return &p;
  return nullptr;
}
const Param* Find(const Mesh& s, const char* name) {
  for (const auto& p : s.params)
    if (p.name == name) return &p;
  return nullptr;
}
// The attire's EMD entry (name "......<attire-1><2 model | 4 gear>").
EpacEntry* AttireEntry(Epac& e, int attire, bool gear) {
  for (auto& g : e.groups)
    if (g.type == "EMD ")
      for (auto& en : g.entries)
        if (en.name.size() >= 8 && en.name[6] == char('0' + attire - 1) && en.name[7] == (gear ? '4' : '2')) return &en;
  return nullptr;
}
}  // namespace

bool ReadMaterials(const Bytes& pac, int attire, bool gear, std::vector<MeshMat>& out, std::string* error) {
  out.clear();
  Epac e;
  if (!EpacRead(pac, e)) return Fail(error, "not a model pac");
  EpacEntry* en = AttireEntry(e, attire, gear);
  if (!en) return Fail(error, "no such attire");
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(en->data), kids)) return Fail(error, "the attire is not a PACH");
  for (const auto& k : kids) {
    const Bytes raw = Unpack(k.data);
    Model m;
    if (!IsJboy(raw) || !JboyRead(raw, m)) continue;
    for (size_t i = 0; i < m.meshes.size(); ++i) {
      const Mesh& s = m.meshes[i];
      MeshMat mm;
      mm.model_id = k.id, mm.mesh = int(i), mm.model = m.name, mm.shader = s.shader;
      if (s.raw.size() >= 0x8C) mm.flags = RdBe(s.raw, 0x88);
      if (const Param* p = Find(s, "g_fSpecularLev"); p && p->value.size() >= 4) mm.spec = FBe(p->value), mm.has_spec = true;
      if (const Param* p = Find(s, "g_iSpecularPow"); p && p->value.size() >= 4) mm.spec_pow = float(int32_t(RdBe(p->value)));
      if (const Param* p = Find(s, "g_f4MatDifCol"); p && p->value.size() >= 12) {
        for (int c = 0; c < 3; ++c) mm.tint[c] = FBe(p->value, size_t(4 * c));
        mm.has_tint = true;
      }
      auto tex = [&](const char* name) {
        const Param* p = Find(s, name);
        const int slot = p && p->value.size() >= 4 ? int(int32_t(RdBe(p->value))) : -1;
        return slot >= 0 && slot < int(m.textures.size()) ? m.textures[size_t(slot)] : std::string();
      };
      mm.color_tex = tex("texDiffuse");
      mm.normal_slot = Find(s, "texNormal") != nullptr;
      mm.normal_tex = tex("texNormal");
      out.push_back(mm);
    }
  }
  return true;
}

bool PatchMaterials(Bytes& pac, int attire, bool gear, const std::map<std::pair<uint32_t, int>, MatEdit>& edits, std::string* error) {
  bool any = false;
  for (const auto& [k, ed] : edits) any |= !ed.Empty();
  if (!any) return true;
  Epac e;
  if (!EpacRead(pac, e)) return Fail(error, "not a model pac");
  EpacEntry* en = AttireEntry(e, attire, gear);
  if (!en) return Fail(error, "no attire " + std::to_string(attire));
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(en->data), kids)) return Fail(error, "the attire is not a PACH");
  // the attire's texture bundles (a new normal map joins the one holding the model's colour picture)
  std::vector<std::vector<BundleTexture>> bundles(kids.size());
  std::vector<bool> is_bundle(kids.size()), bundle_changed(kids.size());
  for (size_t i = 0; i < kids.size(); ++i) {
    const Bytes raw = Unpack(kids[i].data);
    if (IsTextureBundle(raw) && BundleRead(raw, bundles[i])) is_bundle[i] = true;
  }
  auto bundle_with = [&](const std::string& name) {
    int first = -1;
    for (size_t i = 0; i < kids.size(); ++i) {
      if (!is_bundle[i]) continue;
      if (first < 0) first = int(i);
      for (const auto& t : bundles[i])
        if (t.name == name) return int(i);
    }
    return first;
  };
  int flat_added = 0;
  for (size_t ki = 0; ki < kids.size(); ++ki) {
    Bytes raw = Unpack(kids[ki].data);
    Model m;
    if (!IsJboy(raw) || !JboyRead(raw, m)) continue;
    bool changed = false;
    for (size_t i = 0; i < m.meshes.size(); ++i) {
      const auto it = edits.find({kids[ki].id, int(i)});
      if (it == edits.end() || it->second.Empty()) continue;
      const MatEdit& ed = it->second;
      Mesh& s = m.meshes[i];
      if (ed.has_spec)
        if (Param* p = Find(s, "g_fSpecularLev"); p && p->value.size() >= 4) FWr(p->value, 0, std::clamp(ed.spec, 0.0f, 100.0f)), changed = true;
      if (ed.has_tint)
        if (Param* p = Find(s, "g_f4MatDifCol"); p && p->value.size() >= 12) {
          for (int c = 0; c < 3; ++c) FWr(p->value, size_t(4 * c), std::clamp(ed.tint[c], 0.0f, 4.0f));
          changed = true;
        }
      if ((ed.cutout >= 0 || ed.two_sided >= 0) && s.raw.size() >= 0x8C) {
        uint32_t f = RdBe(s.raw, 0x88);
        if (ed.cutout >= 0) f = ed.cutout ? f | 0x110 : f & ~0x110u;
        if (ed.two_sided >= 0) f = ed.two_sided ? f & ~0x2u : f | 0x2;
        WrBe(s.raw, 0x88, f), changed = true;
      }
      if (ed.normal != 0)
        if (Param* p = Find(s, "texNormal"); p && p->value.size() >= 4) {
          // the picture (or a flat normal) as a DXT5 texture of the bundle holding the colour picture
          Image img;
          if (ed.normal == 1 && !LoadPicture(ed.normal_file, img)) {
            if (error) *error = "can't read the normal map " + Utf8(ed.normal_file);
            return false;
          }
          if (ed.normal == 2) img.w = img.h = 4, img.rgba.assign(4 * 4 * 4, 0), [&] {
              for (size_t q = 0; q < img.rgba.size(); q += 4) img.rgba[q] = 128, img.rgba[q + 1] = 128, img.rgba[q + 2] = 255, img.rgba[q + 3] = 255;
            }();
          const Param* cp = Find(s, "texDiffuse");
          const int cslot = cp && cp->value.size() >= 4 ? int(int32_t(RdBe(cp->value))) : -1;
          const int b = bundle_with(cslot >= 0 && cslot < int(m.textures.size()) ? m.textures[size_t(cslot)] : "color");
          if (b < 0) {
            if (error) *error = "the attire has no texture bundle for the normal map";
            return false;
          }
          char name[16];
          if (ed.normal == 2) std::snprintf(name, sizeof name, "mm_flatn");
          else std::snprintf(name, sizeof name, "mm_n%u_%d", unsigned(kids[ki].id), int(i));
          bool have = false;
          for (auto& t : bundles[size_t(b)])
            if (t.name == name) have = true, t.data = ed.normal == 2 ? t.data : DdsEncode(img, DxtFormat::kDxt5);
          if (!have) bundles[size_t(b)].push_back({name, "dds", DdsEncode(img, DxtFormat::kDxt5)}), flat_added += ed.normal == 2;
          bundle_changed[size_t(b)] = true;
          int slot = -1;
          for (size_t t = 0; t < m.textures.size(); ++t)
            if (m.textures[t] == name) slot = int(t);
          if (slot < 0) m.textures.push_back(name), slot = int(m.textures.size()) - 1;
          WrBe(p->value, 0, uint32_t(slot));
          changed = true;
        }
    }
    if (changed) kids[ki].data = JboyWrite(m);
  }
  for (size_t i = 0; i < kids.size(); ++i)
    if (bundle_changed[i]) kids[i].data = BundleWrite(bundles[i]);
  en->data = PachWrite(kids);
  pac = EpacWrite(e);
  return true;
}

std::string MaterialsText(const std::map<MatKey, MatEdit>& m) {
  std::string t;
  char b[512];
  for (const auto& [k, e] : m) {
    if (e.Empty()) continue;
    std::snprintf(b, sizeof b, "%d %d %u %d|%d %g|%d|%d %g %g %g|%d|%d|%d|", std::get<0>(k), int(std::get<1>(k)), unsigned(std::get<2>(k)),
                  std::get<3>(k), int(e.has_spec), e.spec, e.normal, int(e.has_tint), e.tint[0], e.tint[1], e.tint[2], e.cutout,
                  e.two_sided, e.spec_alpha);
    t += b + Utf8(e.normal_file) + "\n";
  }
  return t;
}

std::map<MatKey, MatEdit> MaterialsFromText(const std::string& t) {
  std::map<MatKey, MatEdit> m;
  std::istringstream in(t);
  for (std::string line; std::getline(in, line);) {
    int a = 0, g = 0, mesh = 0, hs = 0, ht = 0;
    unsigned id = 0;
    MatEdit e;
    if (std::sscanf(line.c_str(), "%d %d %u %d|%d %g|%d|%d %g %g %g|%d|%d|%d|", &a, &g, &id, &mesh, &hs, &e.spec, &e.normal, &ht, &e.tint[0],
                    &e.tint[1], &e.tint[2], &e.cutout, &e.two_sided, &e.spec_alpha) != 14)
      continue;
    e.has_spec = hs != 0, e.has_tint = ht != 0;
    size_t bar = 0;
    for (int n = 0; n < 7 && bar != std::string::npos; ++n) bar = line.find('|', bar + 1);
    if (bar != std::string::npos) e.normal_file = Wide(line.substr(bar + 1));
    m[{a, g != 0, uint32_t(id), mesh}] = e;
  }
  return m;
}

}  // namespace mm
