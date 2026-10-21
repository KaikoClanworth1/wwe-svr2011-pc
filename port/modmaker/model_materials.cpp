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
bool SkinShineOf(const Mesh& s, uint32_t flags) {
  const Param* t = Find(s, "texTilingMap");
  const Param* spa = Find(s, "g_fSweatSPA");
  return SkinShine(s.shader, flags, t && t->value.size() >= 4 && int32_t(RdBe(t->value)) >= 0,
                   spa && spa->value.size() >= 4 ? FBe(spa->value) : 0.0f);
}
}  // namespace

bool SkinShine(const std::string& shader, uint32_t flags, bool has_tiling, float sweat_spa) {
  return shader == "yBumpMapSS" && (flags & 0x2) && has_tiling && sweat_spa != 0;
}

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
      mm.skin_shine = SkinShineOf(s, mm.flags);
      out.push_back(mm);
    }
  }
  return true;
}

bool StripBlood(const Bytes& in, Bytes& out, std::string* error) {
  Epac e;
  if (!EpacRead(in, e)) return Fail(error, "not a model pac");
  int dropped = 0;
  for (auto& g : e.groups)
    for (auto& en : g.entries) {
      if (en.name.size() < 8 || (en.name[7] != '2' && en.name[7] != '4')) continue;
      std::vector<PachEntry> kids;
      if (!PachRead(Unpack(en.data), kids)) continue;
      const size_t before = kids.size();
      kids.erase(std::remove_if(kids.begin(), kids.end(), [](const PachEntry& k) { return k.id >= 0x7530 && k.id <= 0x7532; }), kids.end());
      if (kids.size() == before) continue;
      dropped += int(before - kids.size());
      en.data = PachWrite(kids);
    }
  if (!dropped) return Fail(error, "it has no blood overlays");
  out = EpacWrite(e);
  return true;
}

std::string DumpMaterials(const Bytes& pac) {
  std::string o;
  Epac e;
  if (!EpacRead(pac, e)) return "not a model pac\n";
  for (int a = 1; a <= 9; ++a)
    for (const bool gear : {false, true}) {
      EpacEntry* en = AttireEntry(e, a, gear);
      std::vector<PachEntry> kids;
      if (!en || !PachRead(Unpack(en->data), kids)) continue;
      for (const auto& k : kids) {
        const Bytes raw = Unpack(k.data);
        Model m;
        if (!IsJboy(raw) || !JboyRead(raw, m)) continue;
        for (size_t i = 0; i < m.meshes.size(); ++i) {
          const Mesh& s = m.meshes[i];
          char line[160];
          std::snprintf(line, sizeof line, "attire %d%s id %u %s mesh %zu shader %s flags %08X verts %zu\n", a, gear ? " gear" : "",
                        unsigned(k.id), m.name.c_str(), i, s.shader.c_str(), s.raw.size() >= 0x8C ? RdBe(s.raw, 0x88) : 0u, s.verts.size());
          o += line;
          o += "  strips";
          for (const Strip& st : s.strips) o += " " + std::to_string(st.indices.size());
          o += "\n";
          for (const Param& p : s.params) {
            o += "  " + p.name + " t" + std::to_string(p.type) + ":";
            for (size_t b = 0; b + 4 <= p.value.size() && b < 16; b += 4) {
              if (p.type == 0x0d || p.type == 0x0a) std::snprintf(line, sizeof line, " %g", FBe(p.value, b));
              else std::snprintf(line, sizeof line, " %d", int(int32_t(RdBe(p.value, b))));
              o += line;
            }
            if (p.type == 0x0f && p.value.size() >= 4) {
              const int slot = int(int32_t(RdBe(p.value)));
              if (slot >= 0 && slot < int(m.textures.size())) o += " " + m.textures[size_t(slot)];
            }
            o += "\n";
          }
        }
      }
    }
  return o;
}

bool AlphaFixture(Bytes& pac, const std::vector<std::pair<uint32_t, int>>& meshes, std::string* error) {
  Epac e;
  if (!EpacRead(pac, e)) return Fail(error, "not a model pac");
  EpacEntry* en = AttireEntry(e, 1, false);
  if (!en) return Fail(error, "no attire 1");
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(en->data), kids)) return Fail(error, "the attire is not a PACH");
  // the UV boxes, by colour picture name
  std::map<std::string, std::vector<std::array<float, 4>>> boxes;
  for (const auto& k : kids) {
    const Bytes raw = Unpack(k.data);
    Model m;
    if (!IsJboy(raw) || !JboyRead(raw, m)) continue;
    for (const auto& [id, mi] : meshes) {
      if (id != k.id || mi < 0 || mi >= int(m.meshes.size())) continue;
      const Mesh& s = m.meshes[size_t(mi)];
      const Param* cp = Find(s, "texDiffuse");
      const int slot = cp && cp->value.size() >= 4 ? int(int32_t(RdBe(cp->value))) : -1;
      if (slot < 0 || slot >= int(m.textures.size()) || s.uvs.empty()) continue;
      std::array<float, 4> b = {1e9f, 1e9f, -1e9f, -1e9f};
      for (const auto& uv : s.uvs)
        b[0] = std::min(b[0], uv[0]), b[1] = std::min(b[1], uv[1]), b[2] = std::max(b[2], uv[0]), b[3] = std::max(b[3], uv[1]);
      boxes[m.textures[size_t(slot)]].push_back(b);
    }
  }
  if (boxes.empty()) return Fail(error, "none of the meshes found");
  static const uint8_t kStripe[8][3] = {{255, 0, 0}, {255, 128, 0}, {255, 255, 0}, {0, 255, 0}, {0, 255, 255}, {0, 0, 255}, {160, 0, 255}, {255, 0, 255}};
  int done = 0;
  for (auto& k : kids) {
    const Bytes raw = Unpack(k.data);
    std::vector<BundleTexture> b;
    if (!IsTextureBundle(raw) || !BundleRead(raw, b)) continue;
    bool changed = false;
    for (auto& t : b) {
      const auto it = boxes.find(t.name);
      Image img;
      if (it == boxes.end() || !DdsDecode(t.data, img)) continue;
      for (const auto& bx : it->second) {
        const int x0 = std::clamp(int(bx[0] * img.w), 0, img.w - 1), x1 = std::clamp(int(bx[2] * img.w), 0, img.w - 1);
        const int y0 = std::clamp(int(bx[1] * img.h), 0, img.h - 1), y1 = std::clamp(int(bx[3] * img.h), 0, img.h - 1);
        const int split = x0 + (x1 - x0) * 6 / 10, cell = std::max(4, (split - x0) / 8);
        for (int y = y0; y <= y1; ++y)
          for (int x = x0; x <= x1; ++x) {
            uint8_t* p = &img.rgba[(size_t(y) * size_t(img.w) + size_t(x)) * 4];
            if (x < split) {
              p[3] = (((x - x0) / cell + (y - y0) / cell) & 1) ? 255 : 0;
            } else {
              const int s = std::min(7, (x - split) * 8 / std::max(1, x1 - split + 1));
              p[0] = kStripe[s][0], p[1] = kStripe[s][1], p[2] = kStripe[s][2], p[3] = uint8_t(16 + 32 * s);
            }
          }
      }
      t.data = DdsEncode(img, DxtFormat::kDxt5), changed = true, ++done;
    }
    if (changed) k.data = BundleWrite(b);
  }
  if (!done) return Fail(error, "the colour picture wasn't found");
  en->data = PachWrite(kids);
  pac = EpacWrite(e);
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
        // (the skin shaders never write the picture's alpha, so they can't cut out - seen in game: a cut-out skin
        // part stays whole. g_fSweatSPA 0 gets it the colour-alpha shader, which does - as stock skin without sweat)
        if (ed.cutout == 1 && SkinShineOf(s, f))
          if (Param* p = Find(s, "g_fSweatSPA"); p && p->value.size() >= 4) FWr(p->value, 0, 0.0f);
        WrBe(s.raw, 0x88, f), changed = true;
      }
      if (ed.flip == 1) {  // (faces turned over: each strip one index longer - its first again - flips every triangle's
                           // winding (the strip's odd / even turns swap), and the normals point the other way)
        for (auto& st : s.strips) {
          if (st.indices.empty()) continue;
          if (st.prim == 6) st.indices.insert(st.indices.begin(), st.indices.front());
          else
            for (size_t k = 0; k + 2 < st.indices.size(); k += 3) std::swap(st.indices[k + 1], st.indices[k + 2]);  // (a triangle list)
        }
        for (auto& v : s.verts)
          for (float& c : v.normal) c = -c;
        changed = true;
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
          // (a skin mesh's shine mask is the normal map's alpha: kept from the old map - flat: the old map's size,
          // flat bumps; a picture without an alpha of its own: the old alpha scaled to it). One texture per
          // (picture, kept mask): meshes sharing both share it.
          const int oslot = int(int32_t(RdBe(p->value)));
          const std::string old_name = oslot >= 0 && oslot < int(m.textures.size()) ? m.textures[size_t(oslot)] : "";
          const bool keep_mask = !old_name.empty() && SkinShineOf(s, s.raw.size() >= 0x8C ? RdBe(s.raw, 0x88) : 0);
          const Param* cp = Find(s, "texDiffuse");
          const int cslot = cp && cp->value.size() >= 4 ? int(int32_t(RdBe(cp->value))) : -1;
          const int b = bundle_with(cslot >= 0 && cslot < int(m.textures.size()) ? m.textures[size_t(cslot)] : "color");
          if (b < 0) {
            if (error) *error = "the attire has no texture bundle for the normal map";
            return false;
          }
          char name[16];
          if (ed.normal == 2 && !keep_mask) std::snprintf(name, sizeof name, "mm_flatn");  // (one 4x4 flat map for the rest)
          else {
            uint32_t h = 2166136261u;
            for (const char c : (ed.normal == 2 ? std::string("flat") : Utf8(ed.normal_file)) + "|" + (keep_mask ? old_name : ""))
              h = (h ^ uint8_t(c)) * 16777619u;
            std::snprintf(name, sizeof name, "mm_n%08x", h);
          }
          bool have = false;
          for (const auto& t : bundles[size_t(b)]) have |= t.name == name;
          if (!have) {
            if (keep_mask) {
              Image old;
              for (size_t bi = 0; bi < kids.size() && !old.w; ++bi)
                for (const auto& t : bundles[bi])
                  if (is_bundle[bi] && t.name == old_name && DdsDecode(t.data, old)) break;
              bool opaque = true;
              for (size_t q = 3; q < img.rgba.size() && opaque; q += 4) opaque = img.rgba[q] == 255;
              if (old.w > 0 && ed.normal == 2) {
                img = old;
                for (size_t q = 0; q < img.rgba.size(); q += 4) img.rgba[q] = 128, img.rgba[q + 1] = 128, img.rgba[q + 2] = 255;
              } else if (old.w > 0 && opaque) {
                const Image a = Resize(old, img.w, img.h);
                for (size_t q = 3; q < img.rgba.size(); q += 4) img.rgba[q] = a.rgba[q];
              }
            }
            bundles[size_t(b)].push_back({name, "dds", DdsEncode(img, DxtFormat::kDxt5)});
            flat_added += ed.normal == 2 && !keep_mask;
          }
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
    t += b + Utf8(e.normal_file);
    // (after a \x1d: older lines have none; not a tab - a project joins these lines with tabs)
    if (e.flip >= 0 || !e.shine_mask.empty()) t += "\x1d" + std::to_string(e.flip) + "\x1d" + Utf8(e.shine_mask);
    t += "\n";
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
    if (bar != std::string::npos) {
      std::string rest = line.substr(bar + 1);
      if (const size_t tab = rest.find('\x1d'); tab != std::string::npos) {
        const std::string more = rest.substr(tab + 1);
        rest.resize(tab);
        e.flip = std::atoi(more.c_str());
        if (const size_t t2 = more.find('\x1d'); t2 != std::string::npos) e.shine_mask = Wide(more.substr(t2 + 1));
      }
      e.normal_file = Wide(rest);
    }
    m[{a, g != 0, uint32_t(id), mesh}] = e;
  }
  return m;
}

}  // namespace mm
