#include "ring_kit.h"

#include <cstdio>
#include <sstream>

#include "png.h"

namespace svrfmt {

namespace {

// Rope models per side in the four-model mode, in side order (ring +8..+20).
constexpr uint32_t kSideRope[4] = {962, 961, 960, 963};
constexpr uint32_t kRopeShadows[4] = {956, 957, 958, 959};
constexpr uint32_t kPads[4] = {964, 965, 966, 967};
constexpr uint32_t kTurnbuckle = 952;
constexpr uint32_t kPerRope = 900;  // 900 + side + 4 * rope

ArenaModel* Find(Arena& a, uint32_t id) {
  for (auto& m : a.models)
    if (m.id == id) return &m;
  return nullptr;
}

void Hide(Model& m) {
  for (auto& s : m.meshes)
    for (auto& st : s.strips) st.indices.clear();
}

// Material colours are f32x4 params (RGB, 1).
void Tint(Model& m, uint32_t rgb) {
  const float c[3] = {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f};
  for (auto& s : m.meshes)
    for (auto& p : s.params)
      if (p.type == 0x0d && p.value.size() >= 16 && (p.name == "g_f4MatAmbCol" || p.name == "g_f4MatDifCol"))
        for (int k = 0; k < 3; ++k) PutBeF(&p.value[4 * k], BeF(&p.value[4 * k]) * c[k]);
}

void SetTexture(Model& m, const std::string& name) {
  m.textures.assign(m.textures.size(), name);
}

}  // namespace

bool RingSpec::Default() const {
  for (const auto& r : ropes)
    if (!r.visible || r.tint != 0xFFFFFF || !r.texture.empty()) return false;
  return turnbuckles && pads && rope_base == 0.f && rope_gap == 0.f;
}

int RingSpec::VisibleRopes() const {
  int n = 0;
  for (const auto& r : ropes) n += r.visible;
  return n;
}

std::string RingSpec::ManifestLines() const {
  char buf[256];
  std::string s;
  std::snprintf(buf, sizeof buf, "ring.ropes=%d %d %d\n", ropes[0].visible, ropes[1].visible, ropes[2].visible);
  s += buf;
  std::snprintf(buf, sizeof buf, "ring.tints=%06x %06x %06x\n", ropes[0].tint, ropes[1].tint, ropes[2].tint);
  s += buf;
  std::snprintf(buf, sizeof buf, "ring.turnbuckles=%d\nring.pads=%d\n", turnbuckles, pads);
  s += buf;
  if (rope_base > 0.f || rope_gap > 0.f) {
    std::snprintf(buf, sizeof buf, "ring.rope_base=%.2f\nring.rope_gap=%.2f\n", rope_base, rope_gap);
    s += buf;
  }
  return s;
}

void RingSpec::FromManifest(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    std::istringstream vs(v);
    if (k == "ring.ropes") {
      for (auto& r : ropes) { int b = 1; vs >> b; r.visible = b != 0; }
    } else if (k == "ring.tints") {
      for (auto& r : ropes) { std::string h; vs >> h; if (!h.empty()) r.tint = uint32_t(std::stoul(h, nullptr, 16)); }
    } else if (k == "ring.turnbuckles") {
      turnbuckles = v != "0";
    } else if (k == "ring.pads") {
      pads = v != "0";
    } else if (k == "ring.rope_base") {
      rope_base = std::stof(v);
    } else if (k == "ring.rope_gap") {
      rope_gap = std::stof(v);
    }
  }
}

bool ApplyRing(Arena& a, const RingSpec& spec, RingReport& rep) {
  if (spec.Default()) return true;
  bool per_rope = false;
  for (const auto& r : spec.ropes)
    per_rope |= r.tint != spec.ropes[0].tint || r.texture != spec.ropes[0].texture || r.visible != spec.ropes[0].visible;
  const bool none = spec.VisibleRopes() == 0;

  // rope pictures -> textures in the arena's first bundle
  std::string tex_name[3];
  for (int r = 0; r < 3; ++r) {
    if (spec.ropes[r].texture.empty() || !spec.ropes[r].visible) continue;
    Image img;
    if (!LoadImageFile(spec.ropes[r].texture, img)) {
      rep.warnings.push_back("rope picture " + spec.ropes[r].texture + ": unreadable");
      continue;
    }
    tex_name[r] = "ar_ropk" + std::to_string(r);
    if (a.bundles.empty()) { rep.warnings.push_back("arena has no texture set"); break; }
    BundleTexture* t = a.FindTexture(tex_name[r]);
    if (!t) {
      a.bundles[0].textures.push_back({tex_name[r], "dds", {}});
      t = &a.bundles[0].textures.back();
      ++rep.textures_added;
    }
    // the shipped rope texture is small; keep new ones small too
    t->data = DdsEncode(Resize(img, 64, 64), DxtFormat::kDxt1, true);
    a.bundles[0].changed = true;
  }

  auto style = [&](Model& m, int r) {
    if (!spec.ropes[r].visible) { Hide(m); return; }
    if (spec.ropes[r].tint != 0xFFFFFF) Tint(m, spec.ropes[r].tint);
    if (!tex_name[r].empty()) SetTexture(m, tex_name[r]);
  };

  if (per_rope) {
    // one model per rope: 900 + side + 4 * rope
    for (int r = 0; r < 3; ++r)
      for (int side = 0; side < 4; ++side) {
        const ArenaModel* src = Find(a, kSideRope[side]);
        if (!src) { rep.warnings.push_back("no rope model " + std::to_string(kSideRope[side])); return false; }
        const uint32_t id = kPerRope + side + 4 * r;
        ArenaModel* dst = Find(a, id);
        if (!dst) {
          a.models.push_back(*src);
          dst = &a.models.back();
          dst->id = id;
          dst->added = true;
          ++rep.models_added;
        } else {
          dst->model = src->model;
        }
        style(dst->model, r);
        dst->changed = true;
      }
    // the four shared rope models are no longer loaded: keep them small
    for (uint32_t id : kSideRope)
      if (ArenaModel* m = Find(a, id)) { Hide(m->model); m->changed = true; }
  } else {
    for (uint32_t id : kSideRope)
      if (ArenaModel* m = Find(a, id)) { style(m->model, 0); m->changed = true; ++rep.models_changed; }
  }
  if (none)
    for (uint32_t id : kRopeShadows)
      if (ArenaModel* m = Find(a, id)) { Hide(m->model); m->changed = true; }
  if (!spec.pads)
    for (uint32_t id : kPads)
      if (ArenaModel* m = Find(a, id)) { Hide(m->model); m->changed = true; }
  if (!spec.turnbuckles)
    if (ArenaModel* m = Find(a, kTurnbuckle)) { Hide(m->model); m->changed = true; }
  return true;
}

}  // namespace svrfmt
