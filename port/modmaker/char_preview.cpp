// The 3D character view (char_preview.h): the superstar page's preview and
// the Animations page.
//
// Model: ch.pac is EPK8; its EMD group has one PACH per attire and kind
// (entry "%06d%02d", attire*10 + kind, kind 2 = the model). The PACH holds
// JBOY models (child 0 the body with the whole skeleton, hair, eyelash,
// attachments; blood overlays named blood_* are skipped) and texture bundles.
// Every model's vertices are already in the body's model space (bind pose),
// so a vertex is skinned with pose(j) * inverse(bind(j)), its nodes matched
// to the body's by name. -Y is up; the character faces -Z.
//
// Idle: m.pac EPAC group MVMT, entry STAT, PACH child 3 is a YMBs bank;
// motion id 20000 x 10 y 0 is a one second loop. Any YMBs motion plays the
// same way (SetMotion); its track 1 (the victim) goes on the second
// character. A motion has 21 rotation
// channels (the bones in kBones, Euler z*y*x, replacing the node's own
// rotation), a root move, and for each arm and leg an IK target and a pole
// angle: the upper and lower limb bones carry no data and are solved here
// (two bones, the knee or elbow turned by the pole angle). Channels are
// Yamaha style ADPCM. Rotations are keyed 30 a second, moves twice that.
#define NOMINMAX
#include "char_preview.h"

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "imgui.h"
#include "svrfmt/jboy.h"
#include "svrfmt/pac.h"
#include "svrfmt/texture.h"

namespace char_preview {
namespace {

namespace fs = std::filesystem;
using namespace svrfmt;

// ---------------------------------------------------------------- math
// 3x4 affine matrices, column vectors: p' = R p + t.

struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Norm(V3 a) {
  const float l = Len(a);
  return l > 1e-9f ? a * (1.0f / l) : a;
}

struct Xf {
  float r[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  V3 t;
};
Xf Mul(const Xf& a, const Xf& b) {
  Xf o;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[i][0] * b.r[0][j] + a.r[i][1] * b.r[1][j] + a.r[i][2] * b.r[2][j];
  o.t = {a.r[0][0] * b.t.x + a.r[0][1] * b.t.y + a.r[0][2] * b.t.z + a.t.x,
         a.r[1][0] * b.t.x + a.r[1][1] * b.t.y + a.r[1][2] * b.t.z + a.t.y,
         a.r[2][0] * b.t.x + a.r[2][1] * b.t.y + a.r[2][2] * b.t.z + a.t.z};
  return o;
}
Xf Inverse(const Xf& a) {  // (rotation only: no scale in a skeleton)
  Xf o;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o.r[i][j] = a.r[j][i];
  o.t = {-(o.r[0][0] * a.t.x + o.r[0][1] * a.t.y + o.r[0][2] * a.t.z),
         -(o.r[1][0] * a.t.x + o.r[1][1] * a.t.y + o.r[1][2] * a.t.z),
         -(o.r[2][0] * a.t.x + o.r[2][1] * a.t.y + o.r[2][2] * a.t.z)};
  return o;
}
V3 Apply(const Xf& a, V3 p) {
  return {a.r[0][0] * p.x + a.r[0][1] * p.y + a.r[0][2] * p.z + a.t.x,
          a.r[1][0] * p.x + a.r[1][1] * p.y + a.r[1][2] * p.z + a.t.y,
          a.r[2][0] * p.x + a.r[2][1] * p.y + a.r[2][2] * p.z + a.t.z};
}
V3 Rotate(const Xf& a, V3 p) {
  return {a.r[0][0] * p.x + a.r[0][1] * p.y + a.r[0][2] * p.z, a.r[1][0] * p.x + a.r[1][1] * p.y + a.r[1][2] * p.z,
          a.r[2][0] * p.x + a.r[2][1] * p.y + a.r[2][2] * p.z};
}
// R = Rz * Ry * Rx (x applied first)
void Euler(const float e[3], float r[3][3]) {
  const float cx = std::cos(e[0]), sx = std::sin(e[0]), cy = std::cos(e[1]), sy = std::sin(e[1]);
  const float cz = std::cos(e[2]), sz = std::sin(e[2]);
  r[0][0] = cz * cy, r[0][1] = cz * sy * sx - sz * cx, r[0][2] = cz * sy * cx + sz * sx;
  r[1][0] = sz * cy, r[1][1] = sz * sy * sx + cz * cx, r[1][2] = sz * sy * cx - cz * sx;
  r[2][0] = -sy, r[2][1] = cy * sx, r[2][2] = cy * cx;
}
// A frame whose x points along `x_dir` and whose z is `d` (sign: arms +1,
// legs -1) with its x part removed.
void Aim(V3 x_dir, V3 d, float sign, float r[3][3]) {
  const V3 x = Norm(x_dir);
  const V3 z = Norm((d - x * Dot(d, x)) * sign), y = Cross(z, x);
  r[0][0] = x.x, r[1][0] = x.y, r[2][0] = x.z;
  r[0][1] = y.x, r[1][1] = y.y, r[2][1] = y.z;
  r[0][2] = z.x, r[1][2] = z.y, r[2][2] = z.z;
}
void MulR(const float a[3][3], const float b[3][3], float o[3][3]) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
}
void TransposeMul(const float a[3][3], const float b[3][3], float o[3][3]) {  // a^T b
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) o[i][j] = a[0][i] * b[0][j] + a[1][i] * b[1][j] + a[2][i] * b[2][j];
}

// ---------------------------------------------------------------- the model

struct Influence {
  int node[4] = {0, 0, 0, 0};  // body skeleton
  float w[4] = {1, 0, 0, 0};
};
struct PMesh {
  std::vector<V3> pos, nrm;
  std::vector<std::array<float, 2>> uv;
  std::vector<Influence> inf;
  std::vector<uint32_t> idx;
  std::string tex;  // lower case
  bool cutout = false;  // (hair, lashes: alpha tested)
};
struct Bone {
  std::string name;
  float t[3], r[3];
  int parent;
};
struct Character {
  std::vector<Bone> bones;
  std::vector<PMesh> meshes;
  std::map<std::string, Bytes> textures;  // lower case name -> DDS
  size_t verts = 0;
};

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

int DiffuseSlot(const Mesh& s) {
  for (const auto& p : s.params)
    if (p.type == 0x0f && p.name == "texDiffuse" && p.value.size() >= 4) return int(Be32(p.value.data()));
  return -1;
}

bool LoadCharacter(const Bytes& pac, Character& ch, std::string& err) {
  if (pac.size() < 0x4000 || std::memcmp(pac.data(), "EPK8", 4)) {
    err = "not a character model pac (EPK8)";
    return false;
  }
  // the EMD group, the attire 1 model (name "......02"), else any model
  Bytes emd;
  bool attire1 = false;
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!Le32(&pac[p])) break;
    const bool is_emd = !std::memcmp(&pac[p], "EMD ", 4);
    const uint32_t cnt = Le16(&pac[p + 4]);
    p += 12;
    for (uint32_t i = 0; i < cnt && p + 16 <= 0x4000; ++i, p += 16) {
      const std::string name(reinterpret_cast<const char*>(&pac[p]), 8);
      const size_t off = 0x4000 + size_t(Le32(&pac[p + 8])) * 0x800, size = size_t(Le32(&pac[p + 12])) * 0x100;
      if (!is_emd || name[0] == 0 || name[7] != '2' || off + size > pac.size()) continue;
      if (emd.empty() || (name[6] == '0' && !attire1)) {
        emd.assign(pac.begin() + off, pac.begin() + off + size);
        attire1 = name[6] == '0';
      }
    }
  }
  if (emd.empty()) {
    err = "no model in it (EMD)";
    return false;
  }
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(emd), kids)) {
    err = "the model is not a PACH";
    return false;
  }
  std::vector<Model> models;
  for (const auto& k : kids) {
    Bytes raw = Unpack(k.data);
    if (IsJboy(raw)) {
      Model m;
      if (JboyRead(raw, m) && m.name.rfind("blood", 0) != 0) {
        if (k.id == 0) models.insert(models.begin(), std::move(m));
        else models.push_back(std::move(m));
      }
    } else if (IsTextureBundle(raw)) {
      std::vector<BundleTexture> texs;
      if (BundleRead(raw, texs))
        for (auto& t : texs) ch.textures[Lower(t.name)] = std::move(t.data);
    }
  }
  if (models.empty() || models[0].nodes.empty()) {
    err = "no skeleton in the model";
    return false;
  }
  std::map<std::string, int> by_name;
  for (const auto& n : models[0].nodes) {
    Bone b;
    b.name = n.name;
    std::memcpy(b.t, n.t, 12);
    std::memcpy(b.r, n.r, 12);
    b.parent = n.parent >= 0 && n.parent < int(models[0].nodes.size()) ? n.parent : -1;
    by_name.emplace(b.name, int(ch.bones.size()));
    ch.bones.push_back(b);
  }
  for (const Model& m : models) {
    const bool cutout = m.name.find("hair") != std::string::npos || m.name.find("eyelash") != std::string::npos;
    for (const Mesh& s : m.meshes) {
      PMesh pm;
      const int slot = DiffuseSlot(s);
      pm.tex = slot >= 0 && slot < int(m.textures.size()) ? Lower(m.textures[slot]) : "color";
      pm.cutout = cutout;
      const size_t vc = s.verts.size();
      pm.pos.resize(vc), pm.nrm.resize(vc), pm.uv.resize(vc), pm.inf.resize(vc);
      auto node_of = [&](int palette_slot) {
        if (palette_slot < 0 || palette_slot >= int(s.palette.size())) return 0;
        const int local = s.palette[palette_slot] - 1;  // (1-based)
        if (local < 0 || local >= int(m.nodes.size())) return 0;
        auto it = by_name.find(m.nodes[local].name);
        return it == by_name.end() ? 0 : it->second;
      };
      for (size_t k = 0; k < vc; ++k) {
        pm.pos[k] = {s.verts[k].pos[0], s.verts[k].pos[1], s.verts[k].pos[2]};
        pm.nrm[k] = {s.verts[k].normal[0], s.verts[k].normal[1], s.verts[k].normal[2]};
        pm.uv[k] = k < s.uvs.size() ? s.uvs[k] : std::array<float, 2>{0, 0};
        Influence& f = pm.inf[k];
        f = {};
        f.node[0] = node_of(0);
        int n = 0;
        float sum = 0;
        for (size_t j = 0; j < s.weights.size() && n < 4; ++j) {
          if (k >= s.weights[j].size() || s.weights[j][k].bones[0] == 255) continue;
          f.node[n] = node_of(s.weights[j][k].bones[0]);
          f.w[n] = s.weights[j][k].weight;
          sum += f.w[n++];
        }
        if (n == 0 || sum <= 0) f.w[0] = 1, f.w[1] = f.w[2] = f.w[3] = 0;
        else
          for (int j = 0; j < 4; ++j) f.w[j] = j < n ? f.w[j] / sum : 0;
      }
      for (const auto& st : s.strips)
        for (const auto& t : StripToTriangles(st.indices))
          if (t[0] < vc && t[1] < vc && t[2] < vc) pm.idx.insert(pm.idx.end(), {t[0], t[1], t[2]});
      if (pm.idx.empty()) continue;
      ch.verts += vc;
      ch.meshes.push_back(std::move(pm));
    }
  }
  if (ch.meshes.empty()) {
    err = "the model has no meshes";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- the idle

const char* const kBones[21] = {"root",      "koshi",     "mune",    "kubi",    "atama",   "l_sakotsu", "l_ninoude",
                                "l_kote",    "l_te",      "r_sakotsu", "r_ninoude", "r_kote", "r_te",    "l_momo",
                                "l_sune",    "l_ashi",    "l_tsumasaki", "r_momo", "r_sune", "r_ashi", "r_tsumasaki"};

struct Channel {
  float scale = 1;
  std::vector<int32_t> v;  // keys
};
struct Motion {
  int keys = 0;  // rotation keys (30 a second)
  std::array<Channel, 3> root;
  std::array<std::array<Channel, 3>, 4> ik;  // l_te, r_te, l_ashi, r_ashi
  std::array<Channel, 4> pole;
  std::array<std::array<Channel, 3>, 21> rot;  // empty: that axis is 0
  std::array<bool, 21> has{};
};

const int kStep[8] = {57, 57, 57, 57, 77, 102, 128, 153};

struct Reader {
  const Bytes& d;
  size_t p;
  bool ok = true;
  uint8_t U8(size_t at) {
    if (at >= d.size()) { ok = false; return 0; }
    return d[at];
  }
};

// codes -> init + running sum (key 0 = init)
std::vector<int32_t> Adpcm(int32_t init, int step, const std::vector<int>& codes, bool four_bits) {
  std::vector<int32_t> out{init};
  int32_t v = init;
  for (int c : codes) {
    int mag, d, idx;
    bool neg;
    if (four_bits) mag = c & 7, neg = c & 8, d = ((mag * 2 + 1) * step) >> 3, idx = mag;
    else mag = c & 0x1F, neg = c & 0x20, d = ((mag * 2 + 1) * step) >> 5, idx = std::min(mag >> 2, 7);
    v = neg ? v - d : v + d;
    step = std::clamp((step * kStep[idx]) >> 6, 127, 24576);
    out.push_back(v);
  }
  return out;
}
std::vector<int> Nibbles(Reader& r, size_t at, size_t n) {
  std::vector<int> out(n);
  for (size_t i = 0; i < n; ++i) {
    const uint8_t b = r.U8(at + (i >> 1));
    out[i] = (i & 1) ? (b & 15) : (b >> 4);
  }
  return out;
}
int32_t Wrap16(int32_t x) { return int32_t(int16_t(uint16_t(x))); }
// translation: s24 init, step byte, 2K nibbles (K + 4 bytes)
void Ch32(Reader& r, int K, Channel& c) {
  const size_t p = r.p;
  int32_t init = r.U8(p) | r.U8(p + 1) << 8 | r.U8(p + 2) << 16;
  if (init & 0x800000) init -= 0x1000000;
  c.v = Adpcm(init, r.U8(p + 3) * 127, Nibbles(r, p + 4, size_t(2 * K)), true);
  r.p += size_t(K) + 4;
}
// rotation: 'r' raw s16 (2K), '6' 6-bit (K + 3), 'n' nibbles ((K+1)/2 + 3)
void Ch16(Reader& r, int K, char mode, Channel& c) {
  const size_t p = r.p;
  if (mode == 'r') {
    c.v.resize(size_t(K));
    for (int i = 0; i < K; ++i) c.v[size_t(i)] = Wrap16(r.U8(p + 2 * i) | r.U8(p + 2 * i + 1) << 8);
    r.p += size_t(2 * K);
    return;
  }
  const int32_t init = Wrap16(r.U8(p) | r.U8(p + 1) << 8);
  const int step = r.U8(p + 2) * 127;
  if (mode == '6') {
    std::vector<int> codes(static_cast<size_t>(K));
    for (int i = 0; i < K; ++i) codes[size_t(i)] = r.U8(p + 3 + i);
    c.v = Adpcm(init, step, codes, false);
    r.p += size_t(K) + 3;
  } else {
    c.v = Adpcm(init, step, Nibbles(r, p + 3, size_t(K)), true);
    r.p += size_t((K + 1) / 2) + 3;
  }
  for (auto& x : c.v) x = Wrap16(x);
}

bool DecodeMotion(const Bytes& b, int id, int x, int y, Motion& m, std::string& err) {
  if (b.size() < 0x114 || std::memcmp(b.data(), "YMBs", 4)) {
    err = "the bank is not YMBs (match moves are YMKs, which only the game plays)";
    return false;
  }
  const uint8_t* T = &b[0x10];
  const uint32_t n = Le32(&b[0x110]);
  if (0x114 + size_t(n) * 16 + 4 + size_t(n) * 20 > b.size()) {
    err = "the bank is cut short";
    return false;
  }
  const size_t base = 0x114 + size_t(n) * 16;
  int k = -1;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = &b[0x114 + 16 * i];
    if (e[0] == y && e[1] == x && Le16(e + 2) == id) { k = int(i); break; }
  }
  if (k < 0) {
    err = "that track of the motion is not in the bank";
    return false;
  }
  const uint8_t* e = &b[0x114 + 16 * size_t(k)];
  const uint8_t* h = &b[base + 4 + 20 * size_t(k)];
  const int type = h[0], nseg = h[1], interval = h[2] ? h[2] : 1;
  const uint32_t frames = Le32(e + 8);
  if (nseg || type > 1) {
    err = "the motion's layout (segments / type) isn't understood yet";
    return false;
  }
  const int K = int((frames + uint32_t(interval) - 1) / uint32_t(interval));
  m = {};
  m.keys = K;
  Reader r{b, base + Le32(e + 4)};
  const uint8_t* list = T + T[type + 1];
  const uint8_t* desc = T + T[T[0] + 1];
  int rot = 0, ik = 0, pole = 0;
  for (int c = 0; c < list[0]; ++c) {
    const uint8_t* ds = desc + 4 * (list[1 + c] - 1);
    const uint8_t f = ds[0], codec = ds[1];
    const float scale = float(ds[2]);
    if (f & 0x80) {
      if (f & 0x40) {
        for (int a = 0; a < 3; ++a) {
          Channel& ch = m.ik[size_t(std::min(ik, 3))][size_t(a)];
          Ch32(r, K, ch);
          ch.scale = scale;
        }
        ++ik;
      } else {
        Channel& ch = m.pole[size_t(std::min(pole, 3))];
        Ch16(r, K, 'n', ch);
        ch.scale = scale;
        ++pole;
      }
    } else if (f & 0x40) {
      for (int a = 0; a < 3; ++a) {
        Ch32(r, K, m.root[size_t(a)]);
        m.root[size_t(a)].scale = scale;
      }
    } else if (f & 0x20) {
      Channel skip;
      Ch16(r, K, (codec & 15) == 2 ? '6' : 'n', skip);
    } else {
      if (rot < 21) m.has[size_t(rot)] = (f & 7) != 0;
      for (int a = 0; a < 3; ++a) {
        if (!(f & (1 << a))) continue;
        const char mode = (codec & 0xF0) == 0x40 ? 'r' : (codec & 15) == 2 ? '6' : 'n';
        Channel& ch = m.rot[size_t(std::min(rot, 20))][size_t(a)];
        Ch16(r, K, mode, ch);
        ch.scale = scale;
      }
      ++rot;
    }
  }
  if (!r.ok || rot != 21 || ik != 4 || pole != 4) {
    err = "the motion did not decode";
    return false;
  }
  return true;
}

bool ReadWide(const std::wstring& path, Bytes& out) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  _fseeki64(f, 0, SEEK_END);
  const int64_t n = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  out.resize(n > 0 ? size_t(n) : 0);
  const bool ok = std::fread(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

// One entry of a big EPAC without reading the whole file.
bool EpacEntry(const fs::path& file, const char* group, const char* name, Bytes& out) {
  FILE* f = _wfopen(file.c_str(), L"rb");
  if (!f) return false;
  Bytes idx(0x4000);
  bool ok = std::fread(idx.data(), 1, idx.size(), f) == idx.size() && !std::memcmp(idx.data(), "EPAC", 4);
  bool found = false;
  for (size_t p = 0x800; ok && !found && p + 12 <= 0x4000;) {
    if (!Le32(&idx[p])) break;
    const bool g = !std::memcmp(&idx[p], group, 4);
    const uint32_t cnt = Le32(&idx[p + 4]) / 3;
    p += 12;
    for (uint32_t i = 0; i < cnt && p + 12 <= 0x4000; ++i, p += 12) {
      if (!g || std::memcmp(&idx[p], name, 4)) continue;
      out.resize(size_t(Le32(&idx[p + 8])) * 0x100);
      found = _fseeki64(f, 0x4000 + int64_t(Le32(&idx[p + 4])) * 0x800, SEEK_SET) == 0 &&
              std::fread(out.data(), 1, out.size(), f) == out.size();
      break;
    }
  }
  std::fclose(f);
  return found;
}

bool LoadIdle(const std::wstring& game, Motion& m, std::string& err) {
  Bytes stat;
  if (!EpacEntry(fs::path(game) / L"pac" / L"m.pac", "MVMT", "STAT", stat)) {
    err = "no idle motion (pac/m.pac MVMT/STAT)";
    return false;
  }
  std::vector<PachEntry> kids;
  if (!PachRead(Unpack(stat), kids)) {
    err = "the idle motions are not a PACH";
    return false;
  }
  for (const auto& k : kids)
    if (k.id == 3) return DecodeMotion(Unpack(k.data), 20000, 10, 0, m, err);
  err = "no idle bank in m.pac";
  return false;
}

// ---------------------------------------------------------------- posing

float Key(const Channel& c, float at, bool angle, float unit) {
  if (c.v.empty()) return 0;
  const int n = int(c.v.size());
  const int i0 = std::clamp(int(std::floor(at)), 0, n - 1), i1 = std::min(i0 + 1, n - 1);
  const float f = std::clamp(at - float(i0), 0.0f, 1.0f);
  float a = float(c.v[size_t(i0)]), b = float(c.v[size_t(i1)]);
  if (angle) b = a + float(Wrap16(int32_t(b - a)));
  return (a + (b - a) * f) * c.scale * unit;
}

// World transforms of every bone for the motion at `key` (rotation keys).
std::vector<Xf> Pose(const Character& ch, const Motion& m, float key) {
  const float kAng = 6.2831853f / 65536.0f, kMove = 1e-4f;
  const size_t nb = ch.bones.size();
  std::vector<Xf> local(nb);
  for (size_t i = 0; i < nb; ++i) {
    Euler(ch.bones[i].r, local[i].r);
    local[i].t = {ch.bones[i].t[0], ch.bones[i].t[1], ch.bones[i].t[2]};
  }
  std::map<std::string, int> idx;
  for (size_t i = 0; i < nb; ++i) idx.emplace(ch.bones[i].name, int(i));
  auto find = [&](const char* n) {
    auto it = idx.find(n);
    return it == idx.end() ? -1 : it->second;
  };
  float rot[21][3] = {};
  for (int c = 0; c < 21; ++c)
    for (int a = 0; a < 3; ++a) rot[c][a] = Key(m.rot[size_t(c)][size_t(a)], key, true, kAng);
  for (int c = 0; c < 21; ++c) {
    const int i = find(kBones[c]);
    if (i >= 0 && m.has[size_t(c)]) Euler(rot[c], local[size_t(i)].r);
  }
  const int root = find("root");
  if (root >= 0)
    local[size_t(root)].t = {Key(m.root[0], 2 * key, false, kMove), Key(m.root[1], 2 * key, false, kMove),
                             Key(m.root[2], 2 * key, false, kMove)};
  std::vector<Xf> w(nb);
  auto fk = [&] {
    for (size_t i = 0; i < nb; ++i) {
      const int p = ch.bones[i].parent;
      w[i] = p >= 0 && size_t(p) < i ? Mul(w[size_t(p)], local[i]) : local[i];
    }
  };
  fk();
  if (root < 0) return w;
  const V3 root_p = w[size_t(root)].t, fwd{0, 0, -1};
  struct Limb {
    const char *up, *lo, *end;
    float sign;
    int roll;  // the kote channel (x): the forearm's roll, or -1
  };
  const Limb limbs[4] = {{"l_ninoude", "l_kote", "l_te", 1, 7},
                         {"r_ninoude", "r_kote", "r_te", 1, 11},
                         {"l_momo", "l_sune", "l_ashi", -1, -1},
                         {"r_momo", "r_sune", "r_ashi", -1, -1}};
  for (int l = 0; l < 4; ++l) {
    const int iu = find(limbs[l].up), il = find(limbs[l].lo), ie = find(limbs[l].end);
    if (iu < 0 || il < 0 || ie < 0) continue;
    const int pu = ch.bones[size_t(iu)].parent, pl = ch.bones[size_t(il)].parent;
    if (pu < 0 || pl < 0) continue;
    const V3 S = w[size_t(iu)].t;
    const float L1 = Len(w[size_t(il)].t - S), L2 = Len(w[size_t(ie)].t - w[size_t(il)].t);
    const V3 target = root_p + V3{Key(m.ik[size_t(l)][0], 2 * key, false, kMove),
                                  Key(m.ik[size_t(l)][1], 2 * key, false, kMove),
                                  Key(m.ik[size_t(l)][2], 2 * key, false, kMove)};
    V3 a = target - S;
    const float dist = std::max(1e-4f, std::min(Len(a), (L1 + L2) * 0.9999f));
    a = Norm(a);
    const V3 d0 = Norm(fwd - a * Dot(fwd, a));
    const float th = Key(m.pole[size_t(l)], key, true, kAng);
    const V3 d = d0 * std::cos(th) + Cross(a, d0) * std::sin(th);
    const float cosA = std::clamp((L1 * L1 + dist * dist - L2 * L2) / (2 * L1 * dist), -1.0f, 1.0f);
    const float sinA = std::sqrt(std::max(0.0f, 1 - cosA * cosA));
    const V3 J = S + a * (L1 * cosA) + d * (L1 * sinA), E = S + a * dist;
    float R[3][3];
    Aim(J - S, d, limbs[l].sign, R);
    TransposeMul(w[size_t(pu)].r, R, local[size_t(iu)].r);
    fk();
    Aim(E - J, d, limbs[l].sign, R);
    if (limbs[l].roll >= 0) {
      const float e[3] = {rot[limbs[l].roll][0], 0, 0};
      float rx[3][3], tmp[3][3];
      Euler(e, rx);
      MulR(R, rx, tmp);
      std::memcpy(R, tmp, sizeof R);
    }
    TransposeMul(w[size_t(pl)].r, R, local[size_t(il)].r);
    fk();
  }
  return w;
}

// ---------------------------------------------------------------- GPU

struct GpuVertex {
  float pos[3], n[3], uv[2];
};
struct Cb {
  float viewproj[16];
  float flags[4];  // x: alpha test
};

const char* kShader = R"(
cbuffer C : register(b0) { row_major float4x4 viewproj; float4 flags; };
struct VI { float3 p : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD; };
struct VO { float4 p : SV_Position; float3 n : NORMAL; float2 uv : TEXCOORD; };
VO vs(VI i) { VO o; o.p = mul(float4(i.p, 1), viewproj); o.n = i.n; o.uv = i.uv; return o; }
Texture2D t : register(t0);
SamplerState s : register(s0);
float4 ps(VO i) : SV_Target {
  float4 tx = t.Sample(s, i.uv);
  if (flags.x > 0.5 && tx.a < 0.3) discard;
  float3 n = normalize(i.n + 1e-5);
  float key = saturate(dot(n, normalize(float3(-0.4, 0.6, 0.7))));
  float rim = saturate(dot(n, normalize(float3(0.6, 0.3, -0.7))));
  return float4(tx.rgb * (0.38 + 0.62 * key + 0.25 * rim), 1);
}
)";

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader* g_ps = nullptr;
ID3D11InputLayout* g_layout = nullptr;
ID3D11Buffer* g_cb = nullptr;
ID3D11SamplerState* g_sampler = nullptr;
ID3D11RasterizerState* g_raster = nullptr;
ID3D11DepthStencilState* g_depth = nullptr;
ID3D11ShaderResourceView* g_white = nullptr;
ID3D11Texture2D* g_rt = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11ShaderResourceView* g_rt_srv = nullptr;
ID3D11DepthStencilView* g_dsv = nullptr;
int g_rt_w = 0, g_rt_h = 0;

struct GpuMesh {
  ID3D11Buffer* vb = nullptr;
  ID3D11Buffer* ib = nullptr;
  UINT count = 0;
  ID3D11ShaderResourceView* tex = nullptr;
  bool cutout = false;
};

// state: slot 0 the model (track 0), slot 1 the dummy (track 1)
struct Slot {
  std::wstring file;
  std::atomic<int> generation{0};
  std::shared_ptr<Character> pending;  // loaded, not uploaded yet
  std::shared_ptr<Character> ch;
  std::vector<GpuMesh> gpu;
  std::map<std::string, ID3D11ShaderResourceView*> textures;
  std::vector<Xf> bind_inv;
  std::shared_ptr<Motion> motion;  // its track of the chosen motion (nullptr: the idle / bind pose)
};
std::mutex g_mutex;
std::wstring g_game;
Slot g_slots[2];
std::shared_ptr<Motion> g_idle;
std::string g_status, g_motion_status;
Playback g_play;
std::shared_ptr<const Bytes> g_bank;
float g_yaw = 0.35f, g_pitch = 0.08f, g_zoom = 1;
V3 g_center{0, 0, 0}, g_pan{0, 0, 0};
float g_height = 20;
bool g_frame = false;  // (frame the camera on the next posed frame)

void Release(IUnknown* p) {
  if (p) p->Release();
}

void FreeCharacter(Slot& sl) {
  for (auto& g : sl.gpu) Release(g.vb), Release(g.ib);
  sl.gpu.clear();
  for (auto& [n, t] : sl.textures) Release(t);
  sl.textures.clear();
  sl.ch.reset();
}

ID3D11ShaderResourceView* MakeTexture(const Bytes& dds) {
  DdsInfo info;
  if (!DdsInfoOf(dds, info)) return nullptr;
  DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
  int block = 0;
  switch (info.format) {
    case DxtFormat::kDxt1: f = DXGI_FORMAT_BC1_UNORM, block = 8; break;
    case DxtFormat::kDxt3: f = DXGI_FORMAT_BC2_UNORM, block = 16; break;
    case DxtFormat::kDxt5: f = DXGI_FORMAT_BC3_UNORM, block = 16; break;
    case DxtFormat::kArgb: f = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    default: return nullptr;
  }
  std::vector<D3D11_SUBRESOURCE_DATA> levels;
  size_t off = 128;
  int w = info.w, h = info.h;
  for (int l = 0; l < info.mips; ++l) {
    const size_t pitch = block ? size_t(std::max(1, (w + 3) / 4)) * size_t(block) : size_t(w) * 4;
    const size_t rows = block ? size_t(std::max(1, (h + 3) / 4)) : size_t(h);
    if (off + pitch * rows > dds.size()) break;
    levels.push_back({dds.data() + off, UINT(pitch), 0});
    off += pitch * rows;
    w = std::max(1, w / 2), h = std::max(1, h / 2);
  }
  if (levels.empty()) return nullptr;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = UINT(info.w);
  td.Height = UINT(info.h);
  td.MipLevels = UINT(levels.size());
  td.ArraySize = 1;
  td.Format = f;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  if (SUCCEEDED(g_dev->CreateTexture2D(&td, levels.data(), &tex))) {
    g_dev->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
  }
  return srv;
}

void Upload(Slot& sl, std::shared_ptr<Character> ch) {
  FreeCharacter(sl);
  sl.ch = std::move(ch);
  std::shared_ptr<Character>& g_char = sl.ch;
  std::vector<GpuMesh>& g_gpu = sl.gpu;
  std::map<std::string, ID3D11ShaderResourceView*>& g_textures = sl.textures;
  std::vector<Xf>& g_bind_inv = sl.bind_inv;
  for (const PMesh& pm : g_char->meshes) {
    GpuMesh g;
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = UINT(pm.pos.size() * sizeof(GpuVertex));
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g.vb);
    D3D11_BUFFER_DESC ib = {};
    ib.Usage = D3D11_USAGE_IMMUTABLE;
    ib.ByteWidth = UINT(pm.idx.size() * 4);
    ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd = {pm.idx.data(), 0, 0};
    g_dev->CreateBuffer(&ib, &sd, &g.ib);
    g.count = UINT(pm.idx.size());
    g.cutout = pm.cutout;
    auto it = g_textures.find(pm.tex);
    if (it == g_textures.end()) {
      auto src = g_char->textures.find(pm.tex);
      if (src == g_char->textures.end()) src = g_char->textures.find("color");
      it = g_textures.emplace(pm.tex, src == g_char->textures.end() ? nullptr : MakeTexture(src->second)).first;
    }
    g.tex = it->second ? it->second : g_white;
    g_gpu.push_back(g);
  }
  // bind pose (for skinning) and framing
  const size_t nb = g_char->bones.size();
  std::vector<Xf> w(nb);
  for (size_t i = 0; i < nb; ++i) {
    Xf l;
    Euler(g_char->bones[i].r, l.r);
    l.t = {g_char->bones[i].t[0], g_char->bones[i].t[1], g_char->bones[i].t[2]};
    const int p = g_char->bones[i].parent;
    w[i] = p >= 0 && size_t(p) < i ? Mul(w[size_t(p)], l) : l;
  }
  g_bind_inv.resize(nb);
  for (size_t i = 0; i < nb; ++i) g_bind_inv[i] = Inverse(w[i]);
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const PMesh& pm : g_char->meshes)
    for (const V3& p : pm.pos) {
      const float v[3] = {p.x, p.y, p.z};
      for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
    }
  // (shown y up: y and z negated)
  if (&sl == &g_slots[0]) {
    g_center = {(lo[0] + hi[0]) / 2, -(lo[1] + hi[1]) / 2, -(lo[2] + hi[2]) / 2};
    g_height = std::max(1.0f, hi[1] - lo[1]);
    g_frame = true;
  }
}

bool CreatePipeline() {
  ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
  if (FAILED(D3DCompile(kShader, std::strlen(kShader), "char", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
      FAILED(D3DCompile(kShader, std::strlen(kShader), "char", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
    if (err) g_status = std::string("preview shader: ") + static_cast<const char*>(err->GetBufferPointer());
    return false;
  }
  g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs);
  g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps);
  const D3D11_INPUT_ELEMENT_DESC il[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  g_dev->CreateInputLayout(il, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_layout);
  vsb->Release();
  psb->Release();
  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(Cb);
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  g_dev->CreateBuffer(&bd, nullptr, &g_cb);
  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_ANISOTROPIC;
  sd.MaxAnisotropy = 8;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  g_dev->CreateSamplerState(&sd, &g_sampler);
  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  rd.DepthClipEnable = TRUE;
  g_dev->CreateRasterizerState(&rd, &g_raster);
  D3D11_DEPTH_STENCIL_DESC dd = {};
  dd.DepthEnable = TRUE;
  dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  g_dev->CreateDepthStencilState(&dd, &g_depth);
  const uint32_t white = 0xFFB0B0B0u;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = td.Height = 1;
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA srd = {&white, 4, 0};
  ID3D11Texture2D* tex = nullptr;
  g_dev->CreateTexture2D(&td, &srd, &tex);
  g_dev->CreateShaderResourceView(tex, nullptr, &g_white);
  tex->Release();
  return true;
}

void ResizeTarget(int w, int h) {
  if (w == g_rt_w && h == g_rt_h && g_rt) return;
  Release(g_rt_srv), Release(g_rtv), Release(g_rt), Release(g_dsv);
  g_rt_srv = nullptr, g_rtv = nullptr, g_rt = nullptr, g_dsv = nullptr;
  g_rt_w = w, g_rt_h = h;
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = UINT(w);
  td.Height = UINT(h);
  td.MipLevels = td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  g_dev->CreateTexture2D(&td, nullptr, &g_rt);
  g_dev->CreateRenderTargetView(g_rt, nullptr, &g_rtv);
  g_dev->CreateShaderResourceView(g_rt, nullptr, &g_rt_srv);
  td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
  ID3D11Texture2D* depth = nullptr;
  g_dev->CreateTexture2D(&td, nullptr, &depth);
  g_dev->CreateDepthStencilView(depth, nullptr, &g_dsv);
  depth->Release();
}

// row-major, row vectors (as the editor)
void ViewProj(float aspect, float out[16]) {
  const float dist = g_height * 1.85f * g_zoom;
  const V3 at = g_center + g_pan;
  const float cp = std::cos(g_pitch), sp = std::sin(g_pitch);
  const V3 eye = at + V3{std::sin(g_yaw) * dist * cp, dist * sp, std::cos(g_yaw) * dist * cp};
  const V3 z = Norm(at - eye), x = Norm(Cross(V3{0, 1, 0}, z)), y = Cross(z, x);
  const float view[16] = {x.x, y.x, z.x, 0, x.y, y.y, z.y, 0, x.z, y.z, z.z, 0, -Dot(x, eye), -Dot(y, eye), -Dot(z, eye), 1};
  const float zn = 0.5f, zf = dist * 4, ys = 1.0f / std::tan(0.30f), xs = ys / aspect;
  const float proj[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += view[i * 4 + k] * proj[k * 4 + j];
      out[i * 4 + j] = s;
    }
}

float g_frame_lo[3], g_frame_hi[3];  // the bounds gathered over the slots skinned this frame

void Skin(Slot& sl, const std::vector<Xf>& pose) {
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  std::vector<Xf> skin(pose.size());
  for (size_t i = 0; i < pose.size(); ++i) skin[i] = Mul(pose[i], sl.bind_inv[i]);
  const bool frame = g_frame;
  for (size_t m = 0; m < sl.gpu.size(); ++m) {
    const PMesh& pm = sl.ch->meshes[m];
    ID3D11Buffer* vb = sl.gpu[m].vb;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (!vb || FAILED(g_ctx->Map(vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
    auto* out = static_cast<GpuVertex*>(ms.pData);
    for (size_t k = 0; k < pm.pos.size(); ++k) {
      V3 p, n;
      const Influence& f = pm.inf[k];
      for (int j = 0; j < 4; ++j) {
        if (f.w[j] <= 0) continue;
        const Xf& s = skin[size_t(f.node[j])];
        p = p + Apply(s, pm.pos[k]) * f.w[j];
        n = n + Rotate(s, pm.nrm[k]) * f.w[j];
      }
      // y up for the view: y and z negated (a half turn about x)
      out[k] = {{p.x, -p.y, -p.z}, {n.x, -n.y, -n.z}, {pm.uv[k][0], pm.uv[k][1]}};
      if (frame)
        for (int c = 0; c < 3; ++c) lo[c] = std::min(lo[c], out[k].pos[c]), hi[c] = std::max(hi[c], out[k].pos[c]);
    }
    g_ctx->Unmap(vb, 0);
  }
  if (frame && lo[0] <= hi[0])
    for (int c = 0; c < 3; ++c) g_frame_lo[c] = std::min(g_frame_lo[c], lo[c]), g_frame_hi[c] = std::max(g_frame_hi[c], hi[c]);
}

void Render(int w, int h) {
  ResizeTarget(w, h);
  const float clear[4] = {0.11f, 0.12f, 0.14f, 1};
  g_ctx->ClearRenderTargetView(g_rtv, clear);
  g_ctx->ClearDepthStencilView(g_dsv, D3D11_CLEAR_DEPTH, 1, 0);
  if (!g_slots[0].ch || !g_vs) return;
  // the clock: the chosen motion's keys (track 0's length), else the idle's
  const Motion* clock = g_slots[0].motion ? g_slots[0].motion.get() : g_slots[1].motion ? g_slots[1].motion.get() : g_idle.get();
  g_play.keys = clock ? clock->keys : 0;
  if (g_play.playing && g_play.keys > 0) {
    g_play.key += ImGui::GetIO().DeltaTime * 30.0f * g_play.speed;
    if (g_play.key >= float(g_play.keys)) g_play.key = g_play.loop ? std::fmod(g_play.key, float(g_play.keys)) : float(g_play.keys - 1);
  }
  for (int c = 0; c < 3; ++c) g_frame_lo[c] = 1e30f, g_frame_hi[c] = -1e30f;
  for (Slot& sl : g_slots) {
    if (!sl.ch) continue;
    const Motion* m = sl.motion ? sl.motion.get() : (&sl == &g_slots[0] || !g_bank) ? g_idle.get() : nullptr;
    if (m) {
      const float key = m == clock ? g_play.key : std::fmod(g_play.key, float(std::max(1, m->keys)));
      Skin(sl, Pose(*sl.ch, *m, key));
    } else {
      std::vector<Xf> bind(sl.bind_inv.size());
      for (size_t i = 0; i < bind.size(); ++i) bind[i] = Inverse(sl.bind_inv[i]);
      Skin(sl, bind);
    }
  }
  if (g_frame && g_frame_lo[0] <= g_frame_hi[0]) {  // framed as posed (the idle stands on the floor)
    const float* lo = g_frame_lo;
    const float* hi = g_frame_hi;
    g_center = {(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
    const float spread = std::max({hi[0] - lo[0], hi[2] - lo[2]});
    g_height = std::max({1.0f, hi[1] - lo[1], spread * 0.8f});
    g_pan = {};
    g_frame = false;
  }
  D3D11_VIEWPORT vp = {0, 0, float(w), float(h), 0, 1};
  g_ctx->RSSetViewports(1, &vp);
  g_ctx->OMSetRenderTargets(1, &g_rtv, g_dsv);
  g_ctx->OMSetDepthStencilState(g_depth, 0);
  g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  g_ctx->RSSetState(g_raster);
  g_ctx->IASetInputLayout(g_layout);
  g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_ctx->VSSetShader(g_vs, nullptr, 0);
  g_ctx->PSSetShader(g_ps, nullptr, 0);
  g_ctx->VSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetConstantBuffers(0, 1, &g_cb);
  g_ctx->PSSetSamplers(0, 1, &g_sampler);
  float viewproj[16];
  ViewProj(float(w) / float(h), viewproj);
  for (const Slot& sl : g_slots)
  for (const GpuMesh& g : sl.gpu) {
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(g_ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) continue;
    Cb* c = static_cast<Cb*>(ms.pData);
    std::memcpy(c->viewproj, viewproj, 64);
    c->flags[0] = g.cutout ? 1.0f : 0.0f;
    g_ctx->Unmap(g_cb, 0);
    const UINT stride = sizeof(GpuVertex), offset = 0;
    g_ctx->IASetVertexBuffers(0, 1, &g.vb, &stride, &offset);
    g_ctx->IASetIndexBuffer(g.ib, DXGI_FORMAT_R32_UINT, 0);
    g_ctx->PSSetShaderResources(0, 1, &g.tex);
    g_ctx->DrawIndexed(g.count, 0, 0);
  }
  ID3D11RenderTargetView* none = nullptr;
  g_ctx->OMSetRenderTargets(1, &none, nullptr);
}

}  // namespace

void Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
  g_dev = dev;
  g_ctx = ctx;
  CreatePipeline();
}

void Shutdown() {
  for (Slot& sl : g_slots) FreeCharacter(sl);
  Release(g_rt_srv), Release(g_rtv), Release(g_rt), Release(g_dsv);
  Release(g_vs), Release(g_ps), Release(g_layout), Release(g_cb), Release(g_sampler), Release(g_raster);
  Release(g_depth), Release(g_white);
}

void LoadSlot(int which, const std::wstring& ch_pac) {
  Slot& sl = g_slots[which];
  {
    std::lock_guard lock(g_mutex);
    if (ch_pac == sl.file) return;
    sl.file = ch_pac;
    sl.pending.reset();
    if (which == 0) g_status = ch_pac.empty() ? "" : "Loading the model...";
  }
  const int gen = ++sl.generation;
  if (ch_pac.empty()) return;
  const bool need_idle = !g_idle;
  const std::wstring game = g_game;
  std::thread([which, ch_pac, game, gen, need_idle] {
    auto ch = std::make_shared<Character>();
    std::string err;
    Bytes pac;
    bool ok = ReadWide(ch_pac, pac) && LoadCharacter(pac, *ch, err);
    if (!ok && err.empty()) err = "could not read the file";
    std::shared_ptr<Motion> idle;
    std::string idle_err;
    if (ok && need_idle) {
      idle = std::make_shared<Motion>();
      if (!LoadIdle(game, *idle, idle_err)) idle.reset();
    }
    std::lock_guard lock(g_mutex);
    Slot& sl = g_slots[which];
    if (gen != sl.generation) return;
    if (!ok) {
      if (which == 0) g_status = "The model did not load: " + err + ".";
      return;
    }
    sl.pending = ch;
    if (idle) g_idle = idle;
    if (which == 0) {
      char s[160];
      std::snprintf(s, sizeof s, "%zu vertices, %zu bones%s", ch->verts, ch->bones.size(),
                    g_bank ? "" : g_idle ? ", playing the idle stance" : "");
      g_status = s;
      if (!g_idle && !idle_err.empty()) g_status += " (no idle: " + idle_err + ")";
    }
  }).detach();
}

void SetModel(const std::wstring& ch_pac, const std::wstring& game) {
  {
    std::lock_guard lock(g_mutex);
    g_game = game;
  }
  LoadSlot(0, ch_pac);
}

void SetDummy(const std::wstring& ch_pac) { LoadSlot(1, ch_pac); }

bool SetMotion(std::shared_ptr<const Bytes> bank, int id, int x, std::string* err) {
  std::lock_guard lock(g_mutex);
  g_bank = bank;
  g_slots[0].motion.reset();
  g_slots[1].motion.reset();
  g_play.key = 0;
  g_play.id = bank ? id : -1;
  g_play.x = x;
  if (!bank) return true;
  std::string e0, e1;
  auto m0 = std::make_shared<Motion>(), m1 = std::make_shared<Motion>();
  const bool ok0 = DecodeMotion(*bank, id, x, 0, *m0, e0), ok1 = DecodeMotion(*bank, id, x, 1, *m1, e1);
  if (ok0) g_slots[0].motion = m0;
  if (ok1) g_slots[1].motion = m1;
  if (!ok0 && !ok1) {
    if (err) *err = e0;
    g_bank.reset();
    g_play.id = -1;
    return false;
  }
  g_frame = true;
  return true;
}

void ClearMotion() { SetMotion(nullptr, -1, 0); }

Playback& Play() { return g_play; }

void ResetCamera() {
  g_yaw = 0.35f, g_pitch = 0.08f, g_zoom = 1;
  g_pan = {};
  g_frame = true;
}

void Draw(float w, float h) {
  {
    std::lock_guard lock(g_mutex);
    for (Slot& sl : g_slots) {
      if (sl.pending) Upload(sl, std::move(sl.pending)), sl.pending.reset();
      if (sl.file.empty() && sl.ch) FreeCharacter(sl);
    }
  }
  if (!g_slots[0].ch) {
    ImGui::Dummy(ImVec2(w, h));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(a, b, IM_COL32(28, 31, 36, 255));
    const std::string s = Status().empty() ? "Pick a model to see it here." : Status();
    const ImVec2 ts = ImGui::CalcTextSize(s.c_str(), nullptr, false, w - 16);
    ImGui::GetWindowDrawList()->AddText(nullptr, 0, ImVec2(a.x + (w - ts.x) / 2, a.y + (h - ts.y) / 2),
                                        IM_COL32(170, 170, 170, 255), s.c_str(), nullptr, w - 16);
    return;
  }
  Render(std::max(16, int(w)), std::max(16, int(h)));
  ImGui::Image(ImTextureID(reinterpret_cast<uintptr_t>(g_rt_srv)), ImVec2(w, h));
  if (ImGui::IsItemHovered()) {
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) g_yaw -= io.MouseDelta.x * 0.01f;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
      g_yaw -= io.MouseDelta.x * 0.01f;
      g_pitch = std::clamp(g_pitch + io.MouseDelta.y * 0.01f, -0.6f, 1.2f);
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle)) {
      // pan in the view plane
      const float k = g_height * g_zoom * 0.0025f;
      const V3 right{std::cos(g_yaw), 0, -std::sin(g_yaw)};
      g_pan = g_pan - right * (io.MouseDelta.x * k) + V3{0, io.MouseDelta.y * k, 0};
    }
    if (io.MouseWheel != 0) g_zoom = std::clamp(g_zoom * (io.MouseWheel > 0 ? 0.88f : 1.14f), 0.25f, 4.0f);
    if (!ImGui::IsAnyMouseDown() && io.MouseWheel == 0)
      ImGui::SetTooltip("Left drag: turn   Right drag: tilt   Middle drag: pan   Wheel: zoom");
  }
}

std::string Status() {
  std::lock_guard lock(g_mutex);
  return g_status;
}

}  // namespace char_preview
