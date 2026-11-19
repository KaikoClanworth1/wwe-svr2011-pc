// WWE SmackDown vs. Raw 2011 - blood on every superstar (see blood.h).

#include "blood.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "generated/default/svr2011_init.h"

#include "../modmaker/svrfmt/jboy.h"
#include "../modmaker/svrfmt/pac.h"
#include "../modmaker/svrfmt/texture.h"

REXCVAR_DEFINE_BOOL(blood_all, true, "Gameplay",
                    "Every superstar can bleed: mods without blood overlays (or with another model's) get them, "
                    "made from their own skin (false: only models with their own blood overlays bleed)");

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kFace = 0x7530, kBody = 0x7531, kHair = 0x7532;
constexpr const char* kSkin = "yBumpMapSS";

// The front projection of the shared blood textures (fitted on the stock models, blood.h) and the lift off the
// skin. Bands: the face from the head top to the chin, the body from the collarbones to the belly.
constexpr uint32_t kBloodFaceMask[32] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xC1FFFF83u, 0xC3FFFFC3u,
    0xC7FFFFE3u, 0xC7FFFFF3u, 0xDFFFFFF3u, 0xDFFFFFF3u,
    0xDFFFFFFBu, 0xDFFFFFFBu, 0xFFFFFFFBu, 0xFFFFFFFBu,
    0xFFFFFFFBu, 0xFFFFFFFBu, 0xDFFFFFFBu, 0xDFFFFFFBu,
    0xDFFFFFFBu, 0xDFFFFFF3u, 0xDFFFFFF3u, 0xFFFFFFF3u,
    0xFFFFFFF3u, 0xFFFFFFF3u, 0xDFFFFFF3u, 0xDFF3FFE3u,
    0xCFF3FFC3u, 0xCFF1FF83u, 0xC7C1FC03u, 0xC3C1DC03u,
    0xC3C1DC03u, 0xC001DC03u, 0xFFFFFFFFu, 0xFFFFFFFFu,
};
constexpr uint32_t kBloodBodyMask[32] = {
    0x00000000u, 0x0003C000u, 0x0003C000u, 0x003FFF00u,
    0x003FFF00u, 0x007FFFC0u, 0x00FFFFE0u, 0x01FFFFE0u,
    0x01FFFFE0u, 0x01FFFFE0u, 0x01FFFFE0u, 0x01FFFFC0u,
    0x01FFFFC0u, 0x00FFFFC0u, 0x0FFFFFC0u, 0x0FFFFFF0u,
    0x0FFFFFF0u, 0x0FFFFFF8u, 0x1FFFFFF8u, 0x1FFFFFF8u,
    0x1FFFFFF8u, 0x1FFFFFC0u, 0x1FFFFFF0u, 0x0FFFFFF0u,
    0x0FFFFFF0u, 0x0FFFFFE0u, 0x07FFFFE0u, 0x03FFFDE0u,
    0x03F80000u, 0x00000000u, 0x00000000u, 0x00000000u,
};

struct Projection {
  float u0, ux, v0, vy, lift;
  const uint32_t* mask;  // where the shared textures (any pattern, any stage) can show blood: 32 x 32 cells, a row a
                         // word (bit x), widened by a cell
  float vmid;            // (the bloodiest height, kept first when over the caps)
};
constexpr Projection kFaceMap{0.5f, 0.44f, -0.11f, 0.44f, 0.0023f, kBloodFaceMask, 0.35f};
constexpr Projection kBodyMap{0.53f, 0.19f, -0.8f, 0.24f, 0.002f, kBloodBodyMask, 0.4f};
// The largest stock overlay (ch234's blood_face: 2318 vertices, 6557 indices; 1135 / 3359 in one mesh)
constexpr size_t kMeshVerts = 1100, kMeshIndices = 3300, kModelVerts = 2300, kModelIndices = 6500;

// Stock ch100's blood_face / blood_body (header, textures, the meshes' descriptor and parameters) and its
// stand-in textures (8 x 8: bloodface, bloodbody and their _sp) for attires whose bundle has none.
struct Template {
  bool ok = false;
  svrfmt::Model face, body;
  std::vector<svrfmt::BundleTexture> standins;
};

const Template& Tmpl(const fs::path& game) {
  static Template t;
  static std::once_flag once;
  std::call_once(once, [&] {
    svrfmt::Bytes d;
    svrfmt::Epac e;
    if (!svrfmt::ReadFile((game / "pac" / "ch" / "ch100.pac").string(), d) || !svrfmt::EpacRead(d, e)) return;
    for (const auto& g : e.groups) {
      if (g.type != "EMD ") continue;
      for (const auto& en : g.entries) {
        if (en.name.size() < 8 || en.name[7] != '2') continue;
        std::vector<svrfmt::PachEntry> kids;
        if (!svrfmt::PachRead(svrfmt::Unpack(en.data), kids)) return;
        bool face = false, body = false;
        for (const auto& k : kids) {
          if (k.id == kFace) face = svrfmt::JboyRead(svrfmt::Unpack(k.data), t.face);
          if (k.id == kBody) body = svrfmt::JboyRead(svrfmt::Unpack(k.data), t.body);
          if (k.id == 0xA) {
            std::vector<svrfmt::BundleTexture> texs;
            if (svrfmt::BundleRead(svrfmt::Unpack(k.data), texs))
              for (auto& x : texs) {
                std::string n = x.name;
                for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
                if (n == "bloodface" || n == "bloodface_sp" || n == "bloodbody" || n == "bloodbody_sp")
                  t.standins.push_back(std::move(x));
              }
          }
        }
        t.ok = face && body && !t.face.meshes.empty() && !t.body.meshes.empty() && t.standins.size() == 4;
        return;
      }
    }
  });
  if (!t.ok) {
    static bool told = false;
    if (!told) REXLOG_WARN("[svr2011] blood: no stock template (pac/ch/ch100.pac attire 1) - mods keep their own");
    told = true;
  }
  return t;
}

using V3 = std::array<float, 3>;
V3 Sub(const V3& a, const V3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
float Dot(const V3& a, const V3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
V3 Mad(const V3& a, const V3& b, float s) { return {a[0] + b[0] * s, a[1] + b[1] * s, a[2] + b[2] * s}; }

// The closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
V3 Closest(const V3& p, const V3& a, const V3& b, const V3& c) {
  const V3 ab = Sub(b, a), ac = Sub(c, a), ap = Sub(p, a);
  const float d1 = Dot(ab, ap), d2 = Dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return a;
  const V3 bp = Sub(p, b);
  const float d3 = Dot(ab, bp), d4 = Dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return b;
  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) return Mad(a, ab, d1 / (d1 - d3));
  const V3 cp = Sub(p, c);
  const float d5 = Dot(ab, cp), d6 = Dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return c;
  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) return Mad(a, ac, d2 / (d2 - d6));
  const float va = d3 * d6 - d5 * d4;
  if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) return Mad(b, Sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6)));
  const float den = 1.0f / (va + vb + vc);
  return Mad(Mad(a, ab, vb * den), ac, vc * den);
}

struct Skin {
  std::vector<std::array<V3, 3>> tris;
  float top = 1e30f;  // the head top: the skin's smallest y (the models stand along -y)
};

Skin SkinOf(const svrfmt::Model& m) {
  Skin s;
  for (const auto& mesh : m.meshes) {
    if (mesh.shader != kSkin) continue;
    for (const auto& v : mesh.verts) s.top = std::min(s.top, v.pos[1]);
    for (const auto& st : mesh.strips)
      for (const auto& t : svrfmt::StripToTriangles(st.indices)) {
        if (t[0] >= mesh.verts.size() || t[1] >= mesh.verts.size() || t[2] >= mesh.verts.size()) continue;
        std::array<V3, 3> tri;
        for (int k = 0; k < 3; ++k) {
          const auto& p = mesh.verts[t[k]].pos;
          tri[size_t(k)] = {p[0], p[1], p[2]};
        }
        s.tris.push_back(tri);
      }
  }
  return s;
}

// How far an overlay sits off the skin: the median over some of its vertices (stock: 0.002 - 0.005).
float OffSkin(const svrfmt::Model& overlay, const Skin& skin) {
  std::vector<float> d;
  for (const auto& mesh : overlay.meshes)
    for (size_t k = 0; k < mesh.verts.size(); k += 7) {
      const V3 p{mesh.verts[k].pos[0], mesh.verts[k].pos[1], mesh.verts[k].pos[2]};
      float best = 1e30f;
      for (const auto& t : skin.tris) {
        const V3 q = Sub(Closest(p, t[0], t[1], t[2]), p);
        best = std::min(best, Dot(q, q));
      }
      d.push_back(std::sqrt(best));
    }
  if (d.empty()) return 1e30f;
  std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
  return d[d.size() / 2];
}

// An overlay from the body's skin: the triangles facing the front inside the projection's band, lifted along
// their normal, with the template's model header, textures and per-mesh descriptor / parameters.
bool MakeOverlay(const svrfmt::Model& body, float top, const svrfmt::Model& tmpl, const Projection& pr,
                 svrfmt::Model& out, size_t& tri_count, bool& capped) {
  out = tmpl;
  out.nodes = body.nodes;  // (the body's palettes are kept: node index + 1)
  out.meshes.clear();
  tri_count = 0;
  capped = false;
  size_t used_v = 0, used_i = 0;
  const svrfmt::Mesh& tm = tmpl.meshes[0];
  for (const auto& s : body.meshes) {
    if (s.shader != kSkin || s.verts.empty()) continue;
    const size_t n = s.verts.size();
    // the stored normals' sign: outward when they point away from the mesh's middle (most of them)
    V3 mid{0, 0, 0};
    for (const auto& v : s.verts) mid = {mid[0] + v.pos[0], mid[1] + v.pos[1], mid[2] + v.pos[2]};
    mid = {mid[0] / float(n), mid[1] / float(n), mid[2] / float(n)};
    double out_sum = 0;
    for (const auto& v : s.verts)
      out_sum += Dot({v.normal[0], v.normal[1], v.normal[2]}, Sub({v.pos[0], v.pos[1], v.pos[2]}, mid));
    const float sign = out_sum >= 0 ? 1.0f : -1.0f;
    std::vector<std::array<float, 2>> uv(n);
    std::vector<char> in(n);
    for (size_t k = 0; k < n; ++k) {
      const auto& v = s.verts[k];
      uv[k] = {pr.u0 + pr.ux * v.pos[0], pr.v0 + pr.vy * (v.pos[1] - top)};
      // inside the texture (a little margin) and facing the front (-z: the noses point that way)
      in[k] = uv[k][0] > -0.05f && uv[k][0] < 1.05f && uv[k][1] > -0.05f && uv[k][1] < 1.05f &&
              sign * v.normal[2] < -0.1f;
    }
    // the triangles to keep, as (strip, position in it): the source's own strip order, so runs of them stay
    // strips (one index a triangle) - inside the band, facing the front, and where the shared textures can
    // show blood (pr.mask, 32 x 32 cells of the texture)
    struct Tri {
      size_t strip, at;
      float pri;  // (lower: kept first when over the caps - nearest the texture's bloodiest middle)
    };
    std::vector<Tri> cand;
    for (size_t si = 0; si < s.strips.size(); ++si) {
      const auto& idx = s.strips[si].indices;
      for (size_t k = 0; k + 2 < idx.size(); ++k) {
        const uint16_t a = idx[k], b = idx[k + 1], c = idx[k + 2];
        if (a >= n || b >= n || c >= n || a == b || b == c || a == c || !in[a] || !in[b] || !in[c]) continue;
        const float cu = (uv[a][0] + uv[b][0] + uv[c][0]) / 3, cv = (uv[a][1] + uv[b][1] + uv[c][1]) / 3;
        const int gx = std::clamp(int(cu * 32), 0, 31), gy = std::clamp(int(cv * 32), 0, 31);
        if (!(pr.mask[gy] >> gx & 1)) continue;
        cand.push_back({si, k, (cu - 0.5f) * (cu - 0.5f) + (cv - pr.vmid) * (cv - pr.vmid)});
      }
    }
    if (cand.empty()) continue;
    // a mesh of the first `keep` candidates by priority (all of them: their source order)
    auto build = [&](size_t keep, svrfmt::Mesh& m, size_t& tris_out) {
      std::vector<Tri> use = cand;
      if (keep < use.size()) {
        std::nth_element(use.begin(), use.begin() + keep, use.end(), [](const Tri& x, const Tri& y) { return x.pri < y.pri; });
        use.resize(keep);
        std::sort(use.begin(), use.end(), [](const Tri& x, const Tri& y) { return x.strip != y.strip ? x.strip < y.strip : x.at < y.at; });
      }
      std::vector<uint16_t> strip;
      for (size_t r = 0; r < use.size();) {
        size_t e = r;  // a run: the same source strip, consecutive positions
        while (e + 1 < use.size() && use[e + 1].strip == use[r].strip && use[e + 1].at == use[e].at + 1) ++e;
        const auto& idx = s.strips[use[r].strip].indices;
        const size_t k0 = use[r].at, k1 = use[e].at;
        if (!strip.empty()) strip.push_back(strip.back()), strip.push_back(idx[k0]);  // (degenerate join)
        if (strip.size() % 2 != k0 % 2) strip.push_back(idx[k0]);  // (each triangle keeps its winding)
        for (size_t k = k0; k <= k1 + 2; ++k) strip.push_back(idx[k]);
        r = e + 1;
      }
      std::vector<int> remap(n, -1);
      m = svrfmt::Mesh{};
      m.raw = tm.raw;
      m.palette = s.palette;
      m.material = uint32_t(out.meshes.size());
      m.shader = tm.shader;
      m.vfmt = tm.vfmt;
      m.params = tm.params;
      m.weights.resize(s.weights.size());
      for (auto& i : strip) {
        if (remap[i] < 0) {
          remap[i] = int(m.verts.size());
          svrfmt::Vertex v = s.verts[i];
          for (int k = 0; k < 3; ++k) v.pos[k] += sign * v.normal[k] * pr.lift;
          v.color = 0xFFFFFFFF;
          m.verts.push_back(v);
          m.uvs.push_back(uv[i]);
          for (size_t j = 0; j < s.weights.size(); ++j)
            m.weights[j].push_back(i < s.weights[j].size() ? s.weights[j][i] : svrfmt::Weight{{255, 255, 255, 255}, 0});
        }
        i = uint16_t(remap[i]);
      }
      m.strips.push_back({6, std::move(strip)});
      tris_out = use.size();
    };
    // within the largest stock overlay's sizes (a bigger one overran the game's block heap - Mankind, 19 Nov):
    // per mesh and what is left of the whole overlay's
    const size_t max_v = std::min(kMeshVerts, kModelVerts - used_v), max_i = std::min(kMeshIndices, kModelIndices - used_i);
    svrfmt::Mesh m;
    size_t tris = 0;
    build(cand.size(), m, tris);
    if (m.verts.size() > max_v || m.strips[0].indices.size() > max_i) {
      size_t lo = 0, hi = cand.size();  // (the most triangles that fit)
      while (lo + 1 < hi) {
        const size_t mid = (lo + hi) / 2;
        build(mid, m, tris);
        (m.verts.size() <= max_v && m.strips[0].indices.size() <= max_i ? lo : hi) = mid;
      }
      if (!lo) break;
      build(lo, m, tris);
      capped = true;
    }
    used_v += m.verts.size(), used_i += m.strips[0].indices.size();
    svrfmt::Bound(m);
    tri_count += tris;
    out.meshes.push_back(std::move(m));
    if (used_v >= kModelVerts || used_i >= kModelIndices) break;
  }
  return !out.meshes.empty();
}

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

namespace svr2011 {

bool BloodForAll() { return REXCVAR_GET(blood_all); }

bool GiveBlood(svrfmt::Bytes& pac, const fs::path& game, std::string& notes) {
  if (!BloodForAll()) return false;
  svrfmt::Epac e;
  if (!svrfmt::EpacRead(pac, e)) return false;
  const Template* tp = nullptr;
  bool changed = false;
  for (auto& g : e.groups) {
    if (g.type != "EMD ") continue;
    for (auto& en : g.entries) {
      if (en.name.size() < 8 || en.name[7] != '2') continue;
      std::vector<svrfmt::PachEntry> kids;
      if (!svrfmt::PachRead(svrfmt::Unpack(en.data), kids)) continue;
      auto find = [&](uint32_t id) {
        return std::find_if(kids.begin(), kids.end(), [&](const svrfmt::PachEntry& c) { return c.id == id; });
      };
      const auto b = find(0);
      svrfmt::Model body;
      if (b == kids.end() || !svrfmt::JboyRead(svrfmt::Unpack(b->data), body)) continue;
      const Skin skin = SkinOf(body);
      if (skin.tris.empty()) continue;
      // which of the two are missing or off the skin (another model's)
      bool make[2] = {true, true};
      float off[2] = {0, 0};
      for (int i = 0; i < 2; ++i) {
        const auto it = find(i ? kBody : kFace);
        if (it == kids.end()) continue;
        svrfmt::Model ov;
        off[i] = svrfmt::JboyRead(svrfmt::Unpack(it->data), ov) ? OffSkin(ov, skin) : 1e30f;
        make[i] = off[i] > 0.015f;
      }
      if (!make[0] && !make[1]) continue;
      if (!tp) {
        tp = &Tmpl(game);
        if (!tp->ok) return changed;
      }
      std::string what;
      for (int i = 0; i < 2; ++i) {
        if (!make[i]) continue;
        svrfmt::Model ov;
        size_t tris = 0;
        bool capped = false;
        const bool had = find(i ? kBody : kFace) != kids.end();
        if (had) kids.erase(find(i ? kBody : kFace));
        if (!MakeOverlay(body, skin.top, i ? tp->body : tp->face, i ? kBodyMap : kFaceMap, ov, tris, capped)) continue;
        kids.push_back({i ? kBody : kFace, svrfmt::BpeEncodeStored(svrfmt::JboyWrite(ov))});
        size_t nv = 0, ni = 0;
        for (const auto& mm : ov.meshes) nv += mm.verts.size(), ni += mm.strips.empty() ? 0 : mm.strips[0].indices.size();
        char line[160];
        std::snprintf(line, sizeof line, " %s %s (%zu triangles, %zu vertices, %zu indices%s)", i ? "body" : "face",
                      had ? "replaced - another model's" : "made", tris, nv, ni, capped ? ", cut to the stock sizes" : "");
        what += line;
      }
      // (another model's hair blood floats too)
      if (make[0] && find(kFace) != kids.end() && off[0] > 0.015f && find(kHair) != kids.end()) {
        kids.erase(find(kHair));
        what += " hair blood left out (another model's)";
      }
      if (what.empty()) continue;
      // the stand-in textures the overlays name, where the attire's bundle has none
      const auto tb = find(0xA);
      if (tb != kids.end()) {
        std::vector<svrfmt::BundleTexture> texs;
        if (svrfmt::BundleRead(svrfmt::Unpack(tb->data), texs)) {
          size_t added = 0;
          for (const auto& sdn : tp->standins) {
            const std::string n = Lower(sdn.name);
            if (std::none_of(texs.begin(), texs.end(), [&](const svrfmt::BundleTexture& x) { return Lower(x.name) == n; }))
              texs.push_back(sdn), ++added;
          }
          if (added) tb->data = svrfmt::BpeEncodeStored(svrfmt::BundleWrite(texs));
        }
      }
      std::sort(kids.begin(), kids.end(), [](const svrfmt::PachEntry& x, const svrfmt::PachEntry& y) { return x.id < y.id; });
      en.data = svrfmt::PachWrite(kids);
      changed = true;
      notes += " attire " + std::to_string((en.name[6] - '0') + 1) + ":" + what + ";";
    }
  }
  if (changed) pac = svrfmt::EpacWrite(e);
  return changed;
}

}  // namespace svr2011

// ---- When the game bleeds (for the logs and the test aid)
// GAMEPLAY OPTIONS -> BLOOD (ON / OFF) is byte +10381 of the options block (sub_825740C8, 0x82E3DE00 - next to
// DIFFICULTY at +10342). Each wrestler gets a blood component at the match load (sub_822032F8(component,
// wrestler), registered as 'BBLD' on wrestler +1120: it finds the model's children 30000 / 30001 / 30002 and the
// textures named bloodface / bloodbody). sub_822028B8(component, part 0-4, stage) shows a part at a stage (the
// shared BloodFace<n>_<stage> swapped in, sub_822250E0(model, child, shown)) or hides it (-1); the wrestler's
// setup (sub_82195BA0) hides all five. Bleeding goes through sub_821957F0(wrestler, part, stage) - a move's or a
// hit's (sub_821D3F18), a saved level at a match's start (sub_82779498) - which makes every stage -1 with BLOOD
// OFF (the splatter effects, sub_821DDF28, need it too); only the near-head hard-hit check (sub_82202C90) shows
// blood without asking it. So with BLOOD OFF hardly anyone bleeds: a line in the log says so at each match load.
namespace {
int TestStage() {
  static const int stage = [] {
    const char* e = std::getenv("SVR2011_TEST_BLOOD");
    return e ? std::atoi(e) : 0;
  }();
  return stage >= 1 && stage <= 3 ? stage : 0;
}
constexpr uint32_t kBloodOption = 10381;
}  // namespace

REX_EXTERN(__imp__sub_822032F8);
REX_HOOK_RAW(sub_822032F8) {
  const uint32_t comp = ctx.r3.u32;
  const auto saved = ctx;
  __imp__sub_822032F8(ctx, base);
  // (once a match load: the first component after 20 s without one)
  static auto last = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  const bool first = now - last > std::chrono::seconds(20);
  last = now;
  if (!first || !comp) return;
  auto c = saved;
  sub_825740C8(c, base);
  const uint32_t opts = c.r3.u32;
  if (opts && !base[opts + kBloodOption])
    REXLOG_INFO("[svr2011] blood: GAMEPLAY OPTIONS -> BLOOD is OFF - superstars hardly bleed (the game's own "
                "option){}", TestStage() ? " (the test aid bleeds them anyway)" : "");
  else if (opts)
    REXLOG_INFO("[svr2011] blood: GAMEPLAY OPTIONS -> BLOOD is ON (face model {:08X}, body {:08X} - 0: none)",
                svrfmt::Rd32(base + comp + 88), svrfmt::Rd32(base + comp + 92));
}

// Test aid: SVR2011_TEST_BLOOD=<1-3> - every blood part of every wrestler at that stage, from the match's start
// (each stage the game sets, the setup's hiding included, becomes the test stage).
REX_EXTERN(__imp__sub_822028B8);
REX_HOOK_RAW(sub_822028B8) {
  if (const int stage = TestStage(); stage && int32_t(ctx.r5.u32) != stage - 1) {
    static int told = 0;
    if (told++ < 10)
      REXLOG_INFO("[svr2011] blood: test - component {:08X} part {} stage {} -> {} (SVR2011_TEST_BLOOD)",
                  ctx.r3.u32, ctx.r4.u32, int32_t(ctx.r5.u32), stage - 1);
    ctx.r5.u64 = uint64_t(stage - 1);
  }
  __imp__sub_822028B8(ctx, base);
}
