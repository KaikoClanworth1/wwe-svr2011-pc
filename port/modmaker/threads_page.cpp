// Superstar Threads parts editor (the Superstar group's "Threads parts" tab): an attire's colour masks - the parts
// Superstar Threads lets players recolour - shown over its colour texture, each part named after one of the game's
// own part names and given its default colour, then saved into the project's model (Mask1-3: A4R4G4B4, the same
// bytes tools/threads_parts.py writes) and threads.txt (the mod's "attire.part=id,colour" lines).
//
// A part p is channel (p-1) % 4 (R G B A) of Mask((p-1) / 4 + 1) - up to 12. A model without masks gets them from
// threads_parts.py --make-masks (run on a copy), or starts empty.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <array>
#include <map>

#include "app.h"
#include "json.h"
#include "star_page_internal.h"
#include "svrfmt/jboy.h"
#include "svrfmt/pac.h"
#include "svrfmt/texture.h"
#include "tool_run.h"

namespace mm {
namespace star_page {

std::pair<std::wstring, int> AttireSource(int a);  // (star_page.cpp)
std::vector<int> AttireCount();

namespace {

using namespace svrfmt;

constexpr int kParts = 12;
constexpr int kLevel = 11;  // a painted texel's mask level (of 15), as threads_parts.py MASK_LEVEL

struct Part {
  bool used = false, shown = true;
  std::vector<uint8_t> v;  // per mask texel, 0..255
  int id = 0;              // the game's part name (string id)
  std::string name;
  uint32_t rgb = 0xC0C0C0;  // default colour
};

struct Editor {
  std::wstring pac;  // the model file the attire is in, and its attire there
  int in = 1, attire = 1;
  bool loaded = false, edited = false;
  std::string error;
  Image color;   // the attire's colour texture
  int w = 0, h = 0;  // the masks' size
  int masks = 0;     // Mask1..3 present
  Part parts[kParts];
  int sel = 0;
  Picture view;
  bool view_dirty = true;
  bool naming = false;  // threads_parts.py running
  // the model's triangles on the texture, rastered at the masks' size: each texel's UV island and mesh (-1 none)
  std::vector<Model> models;
  std::vector<std::string> mesh_names;
  std::vector<int32_t> island, mesh_of;
  int geo_w = 0, geo_h = 0;
  // painting
  int tool = 0;  // kBrush ...
  int radius = 12, tolerance = 40;
  float zoom = 1, pan_x = 0, pan_y = 0;  // (zoom 1: the texture fits; pan in texels)
  int pick_mesh = 0;
  struct Stroke {
    std::vector<uint32_t> idx;
    std::vector<std::array<uint8_t, 12>> before, after;
  };
  std::vector<Stroke> undo, redo;
  Stroke stroke;               // the one being painted
  std::vector<uint8_t> touched;  // (this stroke's texels, recorded once)
  bool painting = false;
  double last_compose = 0;
} g;

enum Tool { kBrush, kEraser, kFillColour, kIsland, kMeshPick, kTools };
const char* const kToolNames[kTools] = {"Brush", "Eraser", "Fill by colour", "Fill UV island", "Fill a mesh"};

std::vector<std::pair<std::string, int>> g_names;  // the game's part names (threads_parts.py --names)
bool g_names_asked = false;

fs::path WorkDir() { return fs::path(g_game) / L"Mods" / L".convert" / L"threads_edit"; }

// A mask DDS -> its 4 channels (R G B A), 0..255. A4R4G4B4 directly (the game's own and ours), the rest decoded.
bool ReadMask(const Bytes& dds, int& w, int& h, std::vector<uint8_t> ch[4]) {
  if (dds.size() < 128 || std::memcmp(dds.data(), "DDS ", 4)) return false;
  h = int(Le32(&dds[12])), w = int(Le32(&dds[16]));
  const uint32_t bits = Le32(&dds[88]), rmask = Le32(&dds[92]);
  for (int c = 0; c < 4; ++c) ch[c].assign(size_t(w) * h, 0);
  if (bits == 16 && rmask == 0x0F00 && dds.size() >= 128 + size_t(w) * h * 2) {
    const uint8_t* p = &dds[128];
    for (size_t i = 0; i < size_t(w) * h; ++i) {
      const uint16_t x = uint16_t(p[2 * i] | p[2 * i + 1] << 8);
      ch[0][i] = uint8_t(((x >> 8) & 15) * 17), ch[1][i] = uint8_t(((x >> 4) & 15) * 17);
      ch[2][i] = uint8_t((x & 15) * 17), ch[3][i] = uint8_t(((x >> 12) & 15) * 17);
    }
    return true;
  }
  Image img;
  if (!DdsDecode(dds, img) || img.w != w || img.h != h) return false;
  for (size_t i = 0; i < size_t(w) * h; ++i)
    for (int c = 0; c < 4; ++c) ch[c][i] = img.rgba[4 * i + c];
  return true;
}

// The parts -> Mask<k> (A4R4G4B4, "SVRT" hints none), as threads_parts.mask_bundles.
Bytes MaskDds(int k) {
  Bytes dds(128, 0);
  std::memcpy(dds.data(), "DDS ", 4);
  const uint32_t head[7] = {124, 0x1 | 0x2 | 0x4 | 0x1000, uint32_t(g.h), uint32_t(g.w), uint32_t(g.w * 2), 0, 0};
  for (int i = 0; i < 7; ++i) PutLe32(&dds[4 + 4 * i], head[i]);
  std::memcpy(&dds[32], "SVRT\xFF\xFF\xFF\xFF", 8);
  const uint32_t pf[8] = {32, 0x41, 0, 16, 0x0F00, 0x00F0, 0x000F, 0xF000};
  for (int i = 0; i < 8; ++i) PutLe32(&dds[76 + 4 * i], pf[i]);
  PutLe32(&dds[108], 0x1000);
  static const int kShift[4] = {8, 4, 0, 12};  // (R G B A)
  dds.resize(128 + size_t(g.w) * g.h * 2, 0);
  for (size_t i = 0; i < size_t(g.w) * g.h; ++i) {
    uint16_t x = 0;
    for (int c = 0; c < 4; ++c) {
      const Part& p = g.parts[4 * k + c];
      if (p.used && !p.v.empty()) x |= uint16_t(std::min(15, (p.v[i] * 15 + 127) / 255) << kShift[c]);
    }
    dds[128 + 2 * i] = uint8_t(x), dds[129 + 2 * i] = uint8_t(x >> 8);
  }
  return dds;
}

// The attire's EMD entry: <id><attire - 1>2 (the first such).
EpacEntry* AttireEntry(Epac& e, int in) {
  for (auto& gr : e.groups)
    if (gr.type.rfind("EMD", 0) == 0)
      for (auto& en : gr.entries)
        if (en.name.size() == 8 && en.name[7] == '2' && en.name[6] == char('0' + in - 1)) return &en;
  return nullptr;
}

uint32_t ParseRgb(const std::string& s) { return uint32_t(std::strtoul(s.c_str(), nullptr, 16)) & 0xFFFFFF; }
std::string RgbText(uint32_t c) {
  char b[8];
  std::snprintf(b, sizeof b, "%06x", c & 0xFFFFFF);
  return b;
}

// The project's threads.txt lines for this attire -> names and colours.
bool ApplyProjectLines() {
  bool any = false;
  const std::string& t = g_star.threads;
  for (size_t at = 0; at < t.size();) {
    size_t e = t.find('\n', at);
    if (e == std::string::npos) e = t.size();
    const std::string l = t.substr(at, e - at);
    at = e + 1;
    int a = 0, p = 0, id = 0;
    char col[16] = {};
    if (std::sscanf(l.c_str(), "%d.%d=%d,%15[0-9a-fA-F]", &a, &p, &id, col) == 4 && a == g.attire && p >= 1 && p <= kParts) {
      Part& part = g.parts[p - 1];
      part.used = true, part.id = id, part.rgb = ParseRgb(col);
      if (const size_t hash = l.find("# "); hash != std::string::npos) part.name = l.substr(hash + 2);
      any = true;
    }
  }
  return any;
}

void Load(const std::wstring& pac, int in, int attire) {
  g = Editor();
  g.pac = pac, g.in = in, g.attire = attire, g.loaded = true;
  Bytes raw;
  Epac e;
  if (!ReadFile(PathStr(pac), raw) || !EpacRead(raw, e)) return void(g.error = "The model file can't be read.");
  EpacEntry* en = AttireEntry(e, in);
  if (!en) return void(g.error = "This attire has no model in the file.");
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(en->data), kids)) return void(g.error = "The attire's model can't be read.");
  for (const auto& k : kids) {
    std::vector<BundleTexture> texs;
    const Bytes u = Unpack(k.data);
    if (IsJboy(u) && k.id < 0x4e20) {  // (the body and the hair: their meshes on the colour texture)
      Model m;
      if (JboyRead(u, m)) g.models.push_back(std::move(m));
      continue;
    }
    if (!IsTextureBundle(u) || !BundleRead(u, texs)) continue;
    for (const auto& t : texs) {
      const std::string n = Lower(t.name);
      if (k.id == 0xc && n == "color") DdsDecode(t.data, g.color);
      for (int m = 0; m < 3; ++m)
        if (n == "mask" + std::to_string(m + 1)) {
          std::vector<uint8_t> ch[4];
          int w = 0, h = 0;
          if (!ReadMask(t.data, w, h, ch) || (g.w && (w != g.w || h != g.h))) continue;
          g.w = w, g.h = h, g.masks = std::max(g.masks, m + 1);
          for (int c = 0; c < 4; ++c) {
            Part& p = g.parts[4 * m + c];
            p.v = std::move(ch[c]);
            p.used = std::any_of(p.v.begin(), p.v.end(), [](uint8_t x) { return x > 8; });
          }
        }
    }
  }
  if (!g.color.w) return void(g.error = "This attire has no colour texture.");
  ApplyProjectLines();
}

// Names and colours for the parts the masks have (threads_parts.py on a copy of the model): the project's lines win.
void AutoName(bool make_masks) {
  const fs::path work = WorkDir() / L"work";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  fs::copy_file(g.pac, work / L"ch.pac", fs::copy_options::overwrite_existing, ec);
  if (ec) return Status("The model couldn't be copied for naming (" + PathStr(work) + ").");
  std::vector<std::wstring> args = {work.wstring(), L"--json", L"--game", g_game};
  if (make_masks) args.push_back(L"--make-masks");
  g.naming = true;
  Status(make_masks ? "Making colour masks for the model ..." : "Naming the parts ...");
  const std::wstring pac = g.pac;
  const int in = g.in, attire = g.attire;
  RunToolJson(L"threads_parts.py", args, [work, pac, in, attire, make_masks](int code, const std::string& out) {
    g.naming = false;
    Json j;
    if (!ParseJson(out, j) || (code != 0 && code != 3)) return Status("The parts couldn't be named (see the log).");
    if (code == 3) return Status("The model has no colour masks: make them, or start empty and paint them.");
    if (make_masks) {  // (the copy has the masks now: the editor works on it, saved into the project on Save)
      Load((work / L"ch.pac").wstring(), in, attire);
      g.edited = true;
    }
    for (const auto& a : j["attires"].items) {
      if (a["attire"].Int() != in) continue;
      for (const auto& p : a["parts"].items) {
        const int k = p["part"].Int() - 1;
        if (k < 0 || k >= kParts || !g.parts[k].name.empty()) continue;
        g.parts[k].used = true, g.parts[k].id = p["id"].Int(), g.parts[k].name = p["name"].Str();
        g.parts[k].rgb = ParseRgb(p["colour"].Str());
      }
    }
    ApplyProjectLines();
    g.view_dirty = true;
    Status(make_masks ? "Colour masks made: check the parts' names and colours, then Save." : "The parts are named.");
  });
}

// The overlay: the colour texture with each shown part tinted (the picked one brighter).
uint32_t PartTint(int k) {
  static const uint32_t kTints[kParts] = {0xFF4040, 0x40FF40, 0x4080FF, 0xFFD040, 0xFF40FF, 0x40FFFF,
                                          0xFF8040, 0x80FF40, 0x8040FF, 0xFFFFFF, 0x40A0A0, 0xA040A0};
  return kTints[k];
}
void Compose() {
  g.view_dirty = false;
  Image img = g.color;
  if (g.w && g.h)
    for (int y = 0; y < img.h; ++y)
      for (int x = 0; x < img.w; ++x) {
        const size_t mi = size_t(y * g.h / img.h) * g.w + size_t(x * g.w / img.w);
        uint8_t* px = &img.rgba[4 * (size_t(y) * img.w + x)];
        for (int k = 0; k < kParts; ++k) {
          const Part& p = g.parts[k];
          if (!p.used || !p.shown || p.v.empty() || !p.v[mi]) continue;
          const float a = p.v[mi] / 255.0f * (k == g.sel ? 0.85f : 0.6f);
          const uint32_t t = PartTint(k);
          for (int c = 0; c < 3; ++c) px[c] = uint8_t(px[c] * (1 - a) + ((t >> (16 - 8 * c)) & 255) * a);
        }
      }
  g.view.Set(std::move(img));
}

// ---- the model on the texture (islands, meshes) and the painting

// The triangles of the meshes that sample the colour texture (texDiffuse -> "color") -> the island map (triangles
// sharing a vertex join one) and the mesh map, rastered at the masks' size (u, v wrapped; v down, as the texture).
void BuildGeometry() {
  g.geo_w = g.w, g.geo_h = g.h;
  g.island.assign(size_t(g.w) * g.h, -1), g.mesh_of.assign(size_t(g.w) * g.h, -1), g.mesh_names.clear();
  int base_island = 0, mesh_no = 0;
  for (size_t mi = 0; mi < g.models.size(); ++mi) {
    const Model& m = g.models[mi];
    for (size_t si = 0; si < m.meshes.size(); ++si) {
      const Mesh& me = m.meshes[si];
      int tex = -1;
      for (const auto& pr : me.params)
        if (pr.name == "texDiffuse" && pr.value.size() >= 4) tex = int32_t(Be32(pr.value.data()));
      if (tex < 0 || tex >= int(m.textures.size()) || Lower(m.textures[size_t(tex)]) != "color" || me.uvs.empty()) continue;
      const int id = mesh_no++;
      g.mesh_names.push_back((mi ? "hair " : "") + std::to_string(si + 1) + "  " + me.shader + "  (" + std::to_string(me.verts.size()) + " vertices)");
      std::vector<std::array<uint16_t, 3>> tris;
      for (const auto& st : me.strips)
        for (size_t k = 2; k < st.indices.size(); ++k) {
          const uint16_t a = st.indices[k - 2], b = st.indices[k - 1], c = st.indices[k];
          if (a == b || b == c || a == c || a >= me.uvs.size() || b >= me.uvs.size() || c >= me.uvs.size()) continue;
          tris.push_back({a, b, c});
        }
      // islands: union-find over the vertices
      std::vector<int> parent(me.uvs.size());
      for (size_t k = 0; k < parent.size(); ++k) parent[k] = int(k);
      auto find = [&](int x) {
        while (parent[size_t(x)] != x) x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
        return x;
      };
      for (const auto& t : tris) parent[size_t(find(t[1]))] = find(t[0]), parent[size_t(find(t[2]))] = find(t[0]);
      std::map<int, int> root_id;
      for (const auto& t : tris) {
        const int r = find(t[0]);
        auto it = root_id.find(r);
        const int isl = it != root_id.end() ? it->second : (root_id[r] = base_island + int(root_id.size()));
        float u[3], v[3];
        for (int c = 0; c < 3; ++c) {
          float x = me.uvs[t[c]][0], y = me.uvs[t[c]][1];
          u[c] = (x - std::floor(x)) * g.w, v[c] = (y - std::floor(y)) * g.h;
        }
        // (a triangle across the wrap: its corners brought to one side)
        for (int c = 1; c < 3; ++c) {
          if (u[c] - u[0] > g.w / 2.0f) u[c] -= g.w;
          if (u[0] - u[c] > g.w / 2.0f) u[c] += g.w;
          if (v[c] - v[0] > g.h / 2.0f) v[c] -= g.h;
          if (v[0] - v[c] > g.h / 2.0f) v[c] += g.h;
        }
        const int x0 = int(std::floor(std::min({u[0], u[1], u[2]}))), x1 = int(std::ceil(std::max({u[0], u[1], u[2]})));
        const int y0 = int(std::floor(std::min({v[0], v[1], v[2]}))), y1 = int(std::ceil(std::max({v[0], v[1], v[2]})));
        const float d = (v[1] - v[2]) * (u[0] - u[2]) + (u[2] - u[1]) * (v[0] - v[2]);
        if (std::fabs(d) < 1e-6f || x1 - x0 > g.w || y1 - y0 > g.h) continue;
        for (int y = y0; y <= y1; ++y)
          for (int x = x0; x <= x1; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float a = ((v[1] - v[2]) * (px - u[2]) + (u[2] - u[1]) * (py - v[2])) / d;
            const float b = ((v[2] - v[0]) * (px - u[2]) + (u[0] - u[2]) * (py - v[2])) / d;
            if (a < -0.01f || b < -0.01f || a + b > 1.01f) continue;
            const size_t i = size_t(((y % g.h) + g.h) % g.h) * g.w + size_t(((x % g.w) + g.w) % g.w);
            g.island[i] = isl, g.mesh_of[i] = id;
          }
      }
      base_island += int(root_id.size());
    }
  }
  Log("threads parts: " + std::to_string(mesh_no) + " mesh(es), " + std::to_string(base_island) + " UV island(s) on the texture");
}
void NeedGeometry() {
  if (!g.w) g.w = g.color.w, g.h = g.color.h;
  if (g.geo_w != g.w || g.geo_h != g.h || g.island.empty()) BuildGeometry();
}

// One texel set for the picked part (the others cleared there: a texel is one part's) or erased - recorded for undo.
void SetTexel(size_t i, bool on) {
  if (g.touched.size() != size_t(g.w) * g.h) g.touched.assign(size_t(g.w) * g.h, 0);
  std::array<uint8_t, 12> old{};
  for (int k = 0; k < kParts; ++k) old[k] = g.parts[k].v.empty() ? 0 : g.parts[k].v[i];
  Part& sel = g.parts[g.sel];
  if (sel.v.size() != size_t(g.w) * g.h) sel.v.assign(size_t(g.w) * g.h, 0);
  if (on) {
    for (int k = 0; k < kParts; ++k)
      if (k != g.sel && !g.parts[k].v.empty()) g.parts[k].v[i] = 0;
    sel.v[i] = uint8_t(kLevel * 17);
  } else {
    sel.v[i] = 0;
  }
  if (!g.touched[i]) g.touched[i] = 1, g.stroke.idx.push_back(uint32_t(i)), g.stroke.before.push_back(old);
}
void EndStroke() {
  if (g.stroke.idx.empty()) return;
  for (size_t n = 0; n < g.stroke.idx.size(); ++n) {
    std::array<uint8_t, 12> now{};
    for (int k = 0; k < kParts; ++k) now[k] = g.parts[k].v.empty() ? 0 : g.parts[k].v[g.stroke.idx[n]];
    g.stroke.after.push_back(now);
    g.touched[g.stroke.idx[n]] = 0;
  }
  g.undo.push_back(std::move(g.stroke));
  if (g.undo.size() > 30) g.undo.erase(g.undo.begin());
  g.redo.clear();
  g.stroke = Editor::Stroke();
  g.edited = true, g.view_dirty = true;
}
void ApplyStroke(const Editor::Stroke& st, bool forward) {
  for (int k = 0; k < kParts; ++k)
    if (g.parts[k].v.size() != size_t(g.w) * g.h) g.parts[k].v.assign(size_t(g.w) * g.h, 0);
  for (size_t n = 0; n < st.idx.size(); ++n) {
    const auto& vals = forward ? st.after[n] : st.before[n];
    for (int k = 0; k < kParts; ++k) g.parts[k].v[st.idx[n]] = vals[k];
  }
  g.edited = true, g.view_dirty = true;
}
void Undo() {
  if (g.undo.empty()) return;
  ApplyStroke(g.undo.back(), false);
  g.redo.push_back(std::move(g.undo.back()));
  g.undo.pop_back();
}
void Redo() {
  if (g.redo.empty()) return;
  ApplyStroke(g.redo.back(), true);
  g.undo.push_back(std::move(g.redo.back()));
  g.redo.pop_back();
}

void Brush(int cx, int cy, bool on) {
  const int r = std::max(1, g.radius);
  for (int y = cy - r; y <= cy + r; ++y)
    for (int x = cx - r; x <= cx + r; ++x)
      if (x >= 0 && y >= 0 && x < g.w && y < g.h && (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) SetTexel(size_t(y) * g.w + x, on);
}
// The colour texture's colour at a mask texel.
const uint8_t* ColourAt(size_t i) {
  const int x = int(i % size_t(g.w)), y = int(i / size_t(g.w));
  return &g.color.rgba[4 * (size_t(y * g.color.h / g.h) * g.color.w + size_t(x * g.color.w / g.w))];
}
// Fill: the texels of the island under (x, y) - with `by_colour`, only those connected to it within the tolerance.
void Fill(int cx, int cy, bool by_colour) {
  NeedGeometry();
  if (cx < 0 || cy < 0 || cx >= g.w || cy >= g.h) return;
  const size_t start = size_t(cy) * g.w + cx;
  const int isl = g.island[start];
  if (!by_colour) {
    if (isl < 0) return Status("No part of the model is on that spot of the texture.");
    for (size_t i = 0; i < g.island.size(); ++i)
      if (g.island[i] == isl) SetTexel(i, true);
    return;
  }
  const uint8_t* c0 = ColourAt(start);
  const int r0 = c0[0], g0 = c0[1], b0 = c0[2];
  std::vector<uint8_t> seen(g.island.size(), 0);
  std::vector<size_t> todo = {start};
  seen[start] = 1;
  while (!todo.empty()) {
    const size_t i = todo.back();
    todo.pop_back();
    SetTexel(i, true);
    const int x = int(i % size_t(g.w)), y = int(i / size_t(g.w));
    const int nb[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
    for (const auto& n : nb) {
      if (n[0] < 0 || n[1] < 0 || n[0] >= g.w || n[1] >= g.h) continue;
      const size_t j = size_t(n[1]) * g.w + n[0];
      if (seen[j] || (isl >= 0 && g.island[j] != isl)) continue;
      const uint8_t* c = ColourAt(j);
      if (std::abs(c[0] - r0) + std::abs(c[1] - g0) + std::abs(c[2] - b0) > g.tolerance * 3) continue;
      seen[j] = 1, todo.push_back(j);
    }
  }
}
void FillMesh(int mesh) {
  NeedGeometry();
  for (size_t i = 0; i < g.mesh_of.size(); ++i)
    if (g.mesh_of[i] == mesh) SetTexel(i, true);
}

// The texture with the parts, zoomed and panned; the picked tool on a click / drag.
void Canvas() {
  if (g.view_dirty && (!g.painting || ImGui::GetTime() - g.last_compose > 0.1)) Compose(), g.last_compose = ImGui::GetTime();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  if (g.view.Empty() || avail.x < 10 || avail.y < 10) return;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("canvas", avail, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
  const bool hover = ImGui::IsItemHovered();
  const float fit = std::min(avail.x / g.view.image.w, avail.y / g.view.image.h);
  const float k = fit * g.zoom;  // screen pixels per texture pixel
  if (hover && ImGui::GetIO().MouseWheel != 0) {  // (zoom about the cursor)
    const ImVec2 m = ImGui::GetMousePos();
    const float tx = (m.x - at.x) / k + g.pan_x, ty = (m.y - at.y) / k + g.pan_y;
    g.zoom = std::clamp(g.zoom * (ImGui::GetIO().MouseWheel > 0 ? 1.25f : 0.8f), 1.0f, 32.0f);
    const float k2 = fit * g.zoom;
    g.pan_x = tx - (m.x - at.x) / k2, g.pan_y = ty - (m.y - at.y) / k2;
  }
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Right))
    g.pan_x -= ImGui::GetIO().MouseDelta.x / k, g.pan_y -= ImGui::GetIO().MouseDelta.y / k;
  const float vw = avail.x / k, vh = avail.y / k;
  g.pan_x = std::clamp(g.pan_x, 0.0f, std::max(0.0f, g.view.image.w - vw));
  g.pan_y = std::clamp(g.pan_y, 0.0f, std::max(0.0f, g.view.image.h - vh));
  const ImVec2 uv0(g.pan_x / g.view.image.w, g.pan_y / g.view.image.h);
  const ImVec2 uv1(std::min(1.0f, (g.pan_x + vw) / g.view.image.w), std::min(1.0f, (g.pan_y + vh) / g.view.image.h));
  const ImVec2 size((uv1.x - uv0.x) * g.view.image.w * k, (uv1.y - uv0.y) * g.view.image.h * k);
  ImGui::GetWindowDrawList()->AddImage(g.view.Id(), at, ImVec2(at.x + size.x, at.y + size.y), uv0, uv1);
  if (!hover && !g.painting) return;
  const ImVec2 m = ImGui::GetMousePos();
  const int tx = int(((m.x - at.x) / k + g.pan_x) * g.w / g.view.image.w), ty = int(((m.y - at.y) / k + g.pan_y) * g.h / g.view.image.h);
  if (g.tool == kBrush || g.tool == kEraser) {  // (the brush's circle)
    const float rr = g.radius * k * g.view.image.w / g.w;
    ImGui::GetWindowDrawList()->AddCircle(m, rr, IM_COL32(255, 255, 255, 200), 32, 1.5f);
  }
  if (!g.parts[g.sel].used) return;
  if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    g.painting = true;
    if (g.tool == kFillColour) Fill(tx, ty, true);
    if (g.tool == kIsland) Fill(tx, ty, false);
    if (g.tool == kMeshPick) {
      NeedGeometry();
      if (tx >= 0 && ty >= 0 && tx < g.w && ty < g.h && g.mesh_of[size_t(ty) * g.w + tx] >= 0) FillMesh(g.mesh_of[size_t(ty) * g.w + tx]);
    }
    g.view_dirty = true;
  }
  if (g.painting && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (g.tool == kBrush || g.tool == kEraser))
    Brush(tx, ty, g.tool == kBrush), g.view_dirty = true;
  if (g.painting && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) g.painting = false, EndStroke();
}

void Tools() {
  ImGui::TextUnformatted("Paint the picked part:");
  for (int t = 0; t < kTools; ++t) {
    if (t) ImGui::SameLine();
    if (ImGui::RadioButton(kToolNames[t], g.tool == t)) g.tool = t;
  }
  if (g.tool == kBrush || g.tool == kEraser) {
    ImGui::SetNextItemWidth(200 * g_scale);
    ImGui::SliderInt("size", &g.radius, 1, 128);
  }
  if (g.tool == kFillColour) {
    ImGui::SetNextItemWidth(200 * g_scale);
    ImGui::SliderInt("tolerance", &g.tolerance, 1, 128);
  }
  if (g.tool == kMeshPick) {
    NeedGeometry();
    ImGui::SetNextItemWidth(300 * g_scale);
    if (ImGui::BeginCombo("##mesh", g.pick_mesh < int(g.mesh_names.size()) ? g.mesh_names[size_t(g.pick_mesh)].c_str() : "(no mesh)")) {
      for (int k = 0; k < int(g.mesh_names.size()); ++k)
        if (ImGui::Selectable(g.mesh_names[size_t(k)].c_str(), g.pick_mesh == k)) g.pick_mesh = k;
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Fill it") && g.parts[g.sel].used) FillMesh(g.pick_mesh), EndStroke();
    ImGui::SameLine();
    ImGui::TextDisabled("(or click the mesh on the texture)");
  }
  ImGui::BeginDisabled(g.undo.empty());
  if (ImGui::Button("Undo")) Undo();
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(g.redo.empty());
  if (ImGui::Button("Redo")) Redo();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Clear the part") && g.parts[g.sel].used) {
    for (size_t i = 0; i < g.parts[g.sel].v.size(); ++i)
      if (g.parts[g.sel].v[i]) SetTexel(i, false);
    EndStroke();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Wheel: zoom, right drag: move");
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::GetIO().KeyCtrl) {
    if (ImGui::IsKeyPressed(ImGuiKey_Z)) Undo();
    if (ImGui::IsKeyPressed(ImGuiKey_Y)) Redo();
  }
}

// Save: the masks into a copy of the model (the project's model then points at it), the parts' lines into the
// project's threads.txt (this attire's replaced).
void Save() {
  Bytes raw;
  Epac e;
  if (!ReadFile(PathStr(g.pac), raw) || !EpacRead(raw, e)) return Status("The model file can't be read.");
  EpacEntry* en = AttireEntry(e, g.in);
  std::vector<PachEntry> kids;
  if (!en || !PachRead(Unpack(en->data), kids)) return Status("The attire's model can't be read.");
  int last = -1;
  for (int k = 0; k < kParts; ++k)
    if (g.parts[k].used) last = k;
  const int masks = std::max(g.masks, last / 4 + 1);
  if (!g.w) g.w = g.color.w, g.h = g.color.h;
  for (int m = 0; m < masks; ++m) {
    BundleTexture t;
    t.name = "Mask" + std::to_string(m + 1), t.ext = "dds", t.data = MaskDds(m);
    const Bytes bundle = BpeEncode(BundleWrite({t}));
    auto it = std::find_if(kids.begin(), kids.end(), [m](const PachEntry& k) { return k.id == uint32_t(0x0d + m); });
    if (it != kids.end()) it->data = bundle;
    else kids.push_back({uint32_t(0x0d + m), bundle});
  }
  std::sort(kids.begin(), kids.end(), [](const PachEntry& a, const PachEntry& b) { return a.id < b.id; });
  en->data = PachWrite(kids);
  std::error_code ec;
  fs::create_directories(WorkDir(), ec);
  const fs::path out = WorkDir() / (fs::path(g.pac).stem().wstring() + L"_threads_" + std::to_wstring(std::time(nullptr)) + L".pac");
  if (!WriteFile(PathStr(out), EpacWrite(e))) return Status("The model couldn't be written (" + PathStr(out) + ").");
  // the project's model / attire file: the copy with the masks
  if (g.attire > 1 && !g_star.attires[g.attire - 1].empty()) g_star.attires[g.attire - 1] = out.wstring();
  else g_star.model = out.wstring();
  // threads.txt: the other attires' lines kept, this one's written
  std::string text;
  const std::string& t = g_star.threads;
  for (size_t at = 0; at < t.size();) {
    size_t e2 = t.find('\n', at);
    if (e2 == std::string::npos) e2 = t.size();
    const std::string l = t.substr(at, e2 - at);
    at = e2 + 1;
    if (l.empty() || l.rfind(std::to_string(g.attire) + ".", 0) == 0) continue;
    text += l + "\n";
  }
  if (text.find("format=") == std::string::npos)
    text = "# Superstar Threads parts (Mod Maker): attire.part = name id, default colour  # name\nformat=1\n" + text;
  for (int k = 0; k < kParts; ++k) {
    const Part& p = g.parts[k];
    if (!p.used || !p.id) continue;
    text += std::to_string(g.attire) + "." + std::to_string(k + 1) + "=" + std::to_string(p.id) + "," + RgbText(p.rgb) + "  # " + p.name + "\n";
  }
  g_star.threads = text;
  g.pac = out.wstring(), g.masks = masks, g.edited = false;
  Touch();
  Log("threads parts: attire " + std::to_string(g.attire) + " saved (" + std::to_string(masks) + " mask(s)) -> " + PathStr(out));
  Status("The Threads parts of attire " + std::to_string(g.attire) + " are in your superstar now (Save as mod / Install carries them).");
}

void NamePicker(Part& p, int k) {
  ImGui::SetNextItemWidth(220 * g_scale);
  if (ImGui::BeginCombo(("##name" + std::to_string(k)).c_str(), p.name.empty() ? "(name it)" : p.name.c_str(), ImGuiComboFlags_HeightLarge)) {
    static char find[32] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##find", "find", find, sizeof find);
    const std::string f = Lower(find);
    for (const auto& [n, id] : g_names) {
      if (!f.empty() && Lower(n).find(f) == std::string::npos) continue;
      if (ImGui::Selectable((n + "##" + std::to_string(id)).c_str(), p.id == id)) p.name = n, p.id = id, g.edited = true;
    }
    ImGui::EndCombo();
  }
}

}  // namespace

void ThreadsTestTick();
void ThreadsDraw() {
  ThreadsTestTick();
  Heading("Superstar Threads parts", "The parts of each attire that Superstar Threads lets players recolour: where they are on "
                                     "the attire's texture, their names and their default colours.");
  if (g_star.model.empty()) {
    ImGui::TextWrapped("Pick your superstar's model on the Superstar page first.");
    return;
  }
  if (!g_names_asked) {  // (the game's part names, once)
    g_names_asked = true;
    RunToolJson(L"threads_parts.py", {L"--names", L"--game", g_game}, [](int code, const std::string& out) {
      Json j;
      if (code == 0 && ParseJson(out, j))
        for (const auto& x : j.items)
          if (x.items.size() >= 2) g_names.push_back({x.items[0].Str(), x.items[1].Int()});
    });
  }
  ImGui::TextUnformatted("Attire:");
  for (int a : AttireCount()) {
    ImGui::SameLine();
    if (ImGui::RadioButton(std::to_string(a).c_str(), g.attire == a) && g.attire != a) {
      const auto [pac, in] = AttireSource(a);
      Load(pac, in, a);
    }
  }
  {
    const auto [pac, in] = AttireSource(g.attire);
    if (!g.loaded || (!g.edited && (pac != g.pac || in != g.in))) Load(pac, in, g.attire);
  }
  if (!g.error.empty()) {
    ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", g.error.c_str());
    return;
  }
  const bool any = std::any_of(std::begin(g.parts), std::end(g.parts), [](const Part& p) { return p.used; });
  ImGui::BeginDisabled(g.naming || Busy());
  if (!g.masks && !any) {
    ImGui::TextWrapped("This attire has no colour masks yet: Superstar Threads can't recolour it.");
    if (ImGui::Button("Make them for me")) AutoName(true);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("The masks painted from the model's body areas and its texture's colours, and named.");
    ImGui::SameLine();
    if (ImGui::Button("Start empty")) g.parts[0].used = true, g.parts[0].v.assign(size_t(g.color.w) * g.color.h, 0),
                                      g.w = g.color.w, g.h = g.color.h, g.edited = true, g.view_dirty = true;
  } else {
    if (ImGui::Button("Name the parts for me")) AutoName(false);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Names and colours from the body areas each part covers (the names you set stay).");
  }
  ImGui::EndDisabled();
  ImGui::Spacing();
  const float list_w = 470 * g_scale;
  ImGui::BeginChild("parts", ImVec2(list_w, 0), false);
  for (int k = 0; k < kParts; ++k) {
    Part& p = g.parts[k];
    if (!p.used) continue;
    ImGui::PushID(k);
    const uint32_t t = PartTint(k);
    ImGui::ColorButton("##tint", ImVec4(((t >> 16) & 255) / 255.f, ((t >> 8) & 255) / 255.f, (t & 255) / 255.f, 1),
                       ImGuiColorEditFlags_NoTooltip, ImVec2(14 * g_scale, 14 * g_scale));
    ImGui::SameLine();
    if (ImGui::RadioButton(("Part " + std::to_string(k + 1)).c_str(), g.sel == k)) g.sel = k, g.view_dirty = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("##shown", &p.shown)) g.view_dirty = true;
    ImGui::SameLine();
    NamePicker(p, k);
    ImGui::SameLine();
    float c[3] = {((p.rgb >> 16) & 255) / 255.f, ((p.rgb >> 8) & 255) / 255.f, (p.rgb & 255) / 255.f};
    if (ImGui::ColorEdit3("##rgb", c, ImGuiColorEditFlags_NoInputs))
      p.rgb = uint32_t(c[0] * 255 + 0.5f) << 16 | uint32_t(c[1] * 255 + 0.5f) << 8 | uint32_t(c[2] * 255 + 0.5f), g.edited = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) p = Part(), g.edited = true, g.view_dirty = true;
    ImGui::PopID();
  }
  int free_part = -1;
  for (int k = 0; k < kParts && free_part < 0; ++k)
    if (!g.parts[k].used) free_part = k;
  ImGui::BeginDisabled(free_part < 0 || !g.w);
  if (ImGui::Button("Add a part") && free_part >= 0)
    g.parts[free_part].used = true, g.parts[free_part].v.assign(size_t(g.w) * g.h, 0), g.sel = free_part, g.edited = true;
  ImGui::EndDisabled();
  ImGui::Spacing();
  ImGui::BeginDisabled(!g.edited || g.naming);
  if (ImGui::Button("Save the parts into my superstar", ImVec2(300 * g_scale, 0))) Save();
  ImGui::EndDisabled();
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginGroup();
  Tools();
  Canvas();
  ImGui::EndGroup();
}

// Test aid --threads-edit <attire>[,make|name][,save][,out=<file.svrmod>]: the tab on that attire, the auto masks /
// names, then saved (and the project written as a mod, the Mod Maker closed).
std::string g_threads_test;
void ThreadsTest(const std::string& spec) { g_threads_test = spec; }
void ThreadsTestTick() {
  if (g_threads_test.empty() || Busy() || g.naming) return;
  static int stage = 0;
  const int attire = std::atoi(g_threads_test.c_str());
  if (stage == 0) {
    const auto [pac, in] = AttireSource(std::max(1, attire));
    Load(pac, in, std::max(1, attire));
    stage = 1;
    if (g_threads_test.find("make") != std::string::npos) return AutoName(true);
    if (g_threads_test.find("name") != std::string::npos) return AutoName(false);
  }
  if (stage == 1) {
    stage = 2;
    int n = 0;
    std::string cov;
    for (const auto& p : g.parts) {
      n += p.used;
      if (p.used) {
        size_t on = 0;
        uint8_t top = 0;
        for (uint8_t x : p.v) on += x > 8, top = std::max(top, x);
        cov += " " + std::to_string(p.v.empty() ? 0 : on * 1000 / p.v.size()) + "/1000 (max " + std::to_string(top) + ")";
      }
    }
    Log("threads edit test: coverage" + cov + "; colour " + std::to_string(g.color.w) + "x" + std::to_string(g.color.h));
    Log("threads edit test: attire " + std::to_string(g.attire) + ", " + std::to_string(n) + " part(s), masks " + std::to_string(g.masks) +
        ", " + std::to_string(g.w) + "x" + std::to_string(g.h));
    if (const size_t o = g_threads_test.find("ops="); o != std::string::npos) {  // ("ops=a;b;...": the painting, then)
      std::string ops = g_threads_test.substr(o + 4);
      ops = ops.substr(0, ops.find(','));
      for (size_t at = 0; at < ops.size();) {
        size_t e = ops.find(';', at);
        if (e == std::string::npos) e = ops.size();
        const std::string op = ops.substr(at, e - at);
        at = e + 1;
        int part = 0, a = 0, b = 0, c = 0;
        char name[16] = {};
        const int n = std::sscanf(op.c_str(), "%15[a-z]:%d:%d:%d:%d", name, &part, &a, &b, &c);
        const std::string what = name;
        if (n >= 2 && part >= 1 && part <= kParts) {
          g.sel = part - 1;
          if (!g.parts[g.sel].used) g.parts[g.sel].used = true;
        }
        if (what == "brush" || what == "erase") g.radius = c ? c : g.radius, Brush(a, b, what == "brush");
        if (what == "fill") Fill(a, b, true);
        if (what == "island") Fill(a, b, false);
        if (what == "mesh") FillMesh(a);
        if (what == "clear")
          for (size_t i = 0; i < g.parts[g.sel].v.size(); ++i)
            if (g.parts[g.sel].v[i]) SetTexel(i, false);
        if (what != "undo" && what != "redo") EndStroke();
        if (what == "undo") Undo();
        if (what == "redo") Redo();
        size_t on = 0;
        for (uint8_t x : g.parts[g.sel].v) on += x > 8;
        Log("threads edit test: " + op + " -> part " + std::to_string(g.sel + 1) + " has " + std::to_string(on) + " texel(s)");
      }
    }
    if (g_threads_test.find("save") != std::string::npos) Save();
    if (const size_t o = g_threads_test.find("out="); o != std::string::npos) {  // (then the project as a .svrmod, and quit)
      TestSaveMod(Wide(g_threads_test.substr(o + 4)));
      PostMessageW(g_wnd, WM_CLOSE, 0, 0);
    }
    g_threads_test.clear();
  }
}

}  // namespace star_page
}  // namespace mm
