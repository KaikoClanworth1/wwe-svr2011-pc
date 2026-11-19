#include "arena_build.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <map>
#include <set>

namespace svrfmt {

namespace {

bool IsStaticMesh(const Mesh& s) { return s.palette.size() <= 1 && s.weights.size() <= 1; }

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

namespace {
bool IsAudienceLightmap(const BundleTexture& t) {
  std::string n = t.name;
  for (char& c : n) c = char(std::tolower(uint8_t(c)));
  return n == "aud_lightmap" && t.data.size() > 128 && !std::memcmp(t.data.data(), "DDS ", 4) &&
         (!std::memcmp(&t.data[84], "DXT1", 4) || !std::memcmp(&t.data[84], "DXT5", 4));
}
}  // namespace

bool HasAudienceLightmap(const Arena& a) {
  for (const auto& b : a.bundles)
    for (const auto& t : b.textures)
      if (IsAudienceLightmap(t)) return true;
  return false;
}

bool TintAudienceLightmap(Arena& a, const float rgb[3], float bright) {
  bool done = false;
  auto to565 = [&](uint16_t c) {
    const float r = float(c >> 11) / 31, g = float((c >> 5) & 63) / 63, b = float(c & 31) / 31;
    const float y = 0.299f * r + 0.587f * g + 0.114f * b;
    const float o[3] = {rgb ? y * rgb[0] : r, rgb ? y * rgb[1] : g, rgb ? y * rgb[2] : b};  // (null: its own colour)
    auto q = [&](float v, int top) { return int(std::lround(std::clamp(v * bright, 0.0f, 1.0f) * top)); };
    return uint16_t(q(o[0], 31) << 11 | q(o[1], 63) << 5 | q(o[2], 31));
  };
  for (auto& b : a.bundles)
    for (auto& t : b.textures) {
      if (!IsAudienceLightmap(t)) continue;
      const bool dxt5 = !std::memcmp(&t.data[84], "DXT5", 4);
      const size_t step = dxt5 ? 16 : 8, at = dxt5 ? 8 : 0;  // (DXT5: the colour half after the alpha half)
      for (size_t p = 128; p + step <= t.data.size(); p += step) {
        uint8_t* blk = &t.data[p + at];
        const uint16_t c0 = uint16_t(blk[0] | blk[1] << 8), c1 = uint16_t(blk[2] | blk[3] << 8);
        uint16_t g0 = to565(c0), g1 = to565(c1);
        if (c0 > c1) {  // (4-colour mode needs c0 > c1 still: equal would make index 3 transparent black)
          if (g0 < g1) std::swap(g0, g1), blk[4] ^= 0x55, blk[5] ^= 0x55, blk[6] ^= 0x55, blk[7] ^= 0x55;
          if (g0 == g1) g0 < 0xFFFF ? ++g0 : --g1;
        } else if (g0 > g1) {  // (3-colour mode: c0 <= c1; swapping the ends swaps indices 0 and 1)
          std::swap(g0, g1);
          for (int k = 4; k < 8; ++k) {
            uint8_t v = blk[k], o = 0;
            for (int s = 0; s < 8; s += 2) {
              const uint8_t i = (v >> s) & 3;
              o |= uint8_t((i == 0 ? 1 : i == 1 ? 0 : i) << s);
            }
            blk[k] = o;
          }
        }
        blk[0] = uint8_t(g0), blk[1] = uint8_t(g0 >> 8), blk[2] = uint8_t(g1), blk[3] = uint8_t(g1 >> 8);
      }
      b.changed = true;
      done = true;
    }
  return done;
}

bool CopyAudienceLightmap(Arena& to, const Arena& from) {
  const BundleTexture* src = nullptr;
  for (const auto& b : from.bundles)
    for (const auto& t : b.textures)
      if (IsAudienceLightmap(t)) src = &t;
  if (!src) return false;
  bool done = false;
  for (auto& b : to.bundles)
    for (auto& t : b.textures)
      if (IsAudienceLightmap(t) && t.data.size() == src->data.size()) t.data = src->data, b.changed = true, done = true;
  return done;
}

int HideCrowd(Arena& a) {
  int n = 0;
  for (auto& e : a.entries) {
    if (e.id != 0x4E20 && e.id != 0x4E2A) continue;  // (both: the low-detail pack is the one weak phones draw)
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

// ------------------------------------------------------------------ crowd parts (suggestion 3dcea6)

namespace {

struct SeatLine {
  std::string text;  // without its line end
  std::string end;   // "\r\n", "\n" or ""
};

std::vector<SeatLine> SeatLines(const std::string& t) {
  std::vector<SeatLine> v;
  size_t p = 0;
  while (p < t.size()) {
    size_t nl = t.find('\n', p);
    SeatLine l;
    if (nl == std::string::npos) {
      l.text = t.substr(p);
      p = t.size();
    } else {
      l.text = t.substr(p, nl - p);
      l.end = "\n";
      p = nl + 1;
    }
    if (!l.text.empty() && l.text.back() == '\r') l.text.pop_back(), l.end = "\r" + l.end;
    v.push_back(std::move(l));
  }
  return v;
}

std::string TrimSeat(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  return s.substr(a, b - a);
}

// "A (32.0, 80.0, 10.0, 0.0); 3" -> "A" ("" when it isn't a seat row)
std::string SeatRowId(const std::string& tl) {
  const size_t paren = tl.find('(');
  if (tl.empty() || tl[0] == ';' || paren == std::string::npos || paren == 0) return "";
  const std::string id = TrimSeat(tl.substr(0, paren));
  for (char c : id)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return "";
  return id;
}

// A seat file: rows under stand comments in its first { } and people groups ("[n] count") after it.
// Text from its first byte (the crowd's binary people models can hold '{', '[' and '(' too).
bool IsSeatFile(const std::string& t) {
  if (t.empty() || t[0] != ';' || t.find('\0') != std::string::npos) return false;
  return t.find('{') != std::string::npos && t.find("\n[") != std::string::npos && t.find('(') != std::string::npos;
}

struct SeatParse {
  std::vector<std::string> stand_keys;               // in this file's order
  std::map<std::string, std::pair<int, int>> rows;   // row id -> (stand index in stand_keys, its place from the ring)
  std::vector<std::vector<std::string>> stand_rows;
  size_t people_from = 0;                            // the first line after the seat block
};

SeatParse ParseSeats(const std::vector<SeatLine>& lines) {
  SeatParse s;
  int depth = 0, stand = -1;
  bool opened = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    const std::string tl = TrimSeat(lines[i].text);
    if (!opened) {
      if (tl.rfind("{", 0) == 0) opened = true, depth = 1;
      continue;
    }
    if (tl.rfind("}", 0) == 0) {
      s.people_from = i + 1;
      break;
    }
    if (tl.size() > 1 && tl[0] == ';') {
      const std::string key = TrimSeat(tl.substr(1));
      if (key.empty()) continue;
      stand = int(s.stand_keys.size());
      s.stand_keys.push_back(key);
      s.stand_rows.emplace_back();
      continue;
    }
    const std::string id = SeatRowId(tl);
    if (id.empty()) continue;
    if (stand < 0) {  // (rows before any stand comment: a stand of their own)
      stand = 0;
      s.stand_keys.push_back("");
      s.stand_rows.emplace_back();
    }
    s.rows[id] = {stand, int(s.stand_rows[size_t(stand)].size())};
    s.stand_rows[size_t(stand)].push_back(id);
  }
  (void)depth;
  return s;
}

// A person's line: "C00(b03)\t;comment" / "M07" / a left-out one "; off C00(b03)". Its token, the row id, its "bNN".
struct Person {
  bool off = false;
  std::string token, row, tex, rest;
};

bool ParsePerson(const std::string& text, Person& p) {
  std::string tl = TrimSeat(text);
  p = Person();
  if (tl.rfind("; off ", 0) == 0) p.off = true, tl = TrimSeat(tl.substr(6));
  else if (tl.empty() || tl[0] == ';' || tl[0] == '{' || tl[0] == '}' || tl[0] == '[') return false;
  size_t e = 0;
  while (e < tl.size() && tl[e] != ' ' && tl[e] != '\t' && tl[e] != ';') ++e;
  p.token = tl.substr(0, e);
  p.rest = p.off ? std::string() : tl.substr(e);
  size_t r = 0;
  while (r < p.token.size() && !std::isdigit(static_cast<unsigned char>(p.token[r]))) ++r;
  if (r == 0 || r == p.token.size()) return false;
  p.row = p.token.substr(0, r);
  const size_t a = p.token.find('('), b = p.token.find(')');
  if (a != std::string::npos && b != std::string::npos && b > a) p.tex = p.token.substr(a + 1, b - a - 1);
  return true;
}

std::string PersonSeat(const Person& p) {  // the token without its "(bNN)"
  const size_t a = p.token.find('(');
  return a == std::string::npos ? p.token : p.token.substr(0, a);
}

const char* StandName(const std::string& key, int index, std::string& own) {
  static const struct { const char* key; const char* name; } kNames[] = {
      {"\x89\x45\x89\x9C", "Far side, right"}, {"\x8D\xB6\x89\x9C", "Far side, left"},
      {"\x89\x45\x89\xA1", "Right side"},      {"\x8D\xB6\x89\xA1", "Left side"},
      {"\x8E\xE8\x91\x4F", "Near side (the camera's)"}};
  for (const auto& n : kNames)
    if (key == n.key) return n.name;
  bool plain = !key.empty();
  for (char c : key) plain = plain && static_cast<unsigned char>(c) >= 32 && static_cast<unsigned char>(c) < 127;
  own = plain ? key : "Stand " + std::to_string(index + 1);
  return own.c_str();
}

}  // namespace

std::string CropSeatText(const std::string& text, const std::vector<std::string>& stand_keys, const std::map<int, int>& keep,
                         int* left_out) {
  std::vector<SeatLine> lines = SeatLines(text);
  const SeatParse s = ParseSeats(lines);
  // row id -> kept?
  std::map<std::string, bool> row_kept;
  for (const auto& [id, at] : s.rows) {
    bool kept = true;
    const std::string& key = s.stand_keys[size_t(at.first)];
    for (size_t k = 0; k < stand_keys.size(); ++k)
      if (stand_keys[k] == key) {
        const auto it = keep.find(int(k));
        if (it != keep.end()) kept = at.second < it->second;
      }
    row_kept[id] = kept;
  }
  int out_count = 0;
  size_t group_line = std::string::npos;  // the "[n] count" line of the group being read
  int active = 0;
  std::string tex, shown_tex;  // the texture the next untagged person wears (as first written; as the file now reads)
  std::vector<SeatLine> out;
  for (size_t i = 0; i < lines.size(); ++i) {
    SeatLine l = lines[i];
    const std::string tl = TrimSeat(l.text);
    if (i < s.people_from) {
      out.push_back(l);
      continue;
    }
    if (!tl.empty() && tl[0] == '[') {
      group_line = out.size(), active = 0, tex.clear(), shown_tex.clear();
      out.push_back(l);
      continue;
    }
    if (!tl.empty() && tl[0] == '}' && group_line != std::string::npos) {  // the group's count: its people seated now
      std::string& g = out[group_line].text;
      const size_t rb = g.find(']');
      if (rb != std::string::npos) {
        size_t a = rb + 1;
        while (a < g.size() && (g[a] == ' ' || g[a] == '\t')) ++a;
        size_t b = a;
        while (b < g.size() && std::isdigit(static_cast<unsigned char>(g[b]))) ++b;
        if (b > a) g = g.substr(0, a) + std::to_string(active) + g.substr(b);
      }
      group_line = std::string::npos;
      out.push_back(l);
      continue;
    }
    Person p;
    if (group_line == std::string::npos || !ParsePerson(l.text, p)) {
      out.push_back(l);
      continue;
    }
    if (!p.tex.empty()) tex = p.tex;
    const auto rk = row_kept.find(p.row);
    const bool kept = rk == row_kept.end() || rk->second;
    if (kept) {
      // (its texture said where the file before it no longer says it: a left-out person wore it)
      std::string token = p.token;
      if (p.tex.empty() && !tex.empty() && tex != shown_tex) token = PersonSeat(p) + "(" + tex + ")";
      shown_tex = tex;
      const size_t lead = l.text.find_first_not_of(" \t");
      l.text = (lead == std::string::npos ? std::string() : l.text.substr(0, lead)) + token + p.rest;
      ++active;
    } else {
      // (its own "(bNN)" kept in the comment: the person comes back as written)
      std::string token = p.token;
      if (p.tex.empty() && !tex.empty() && !p.off) token = PersonSeat(p) + "(" + tex + ")";
      l.text = "; off " + token;
      if (!p.off) ++out_count;
    }
    out.push_back(l);
  }
  if (left_out) *left_out += out_count;
  std::string r;
  for (const auto& l : out) r += l.text + l.end;
  return r;
}

std::vector<CrowdStand> CrowdStands(const Arena& a) {
  std::vector<CrowdStand> v;
  for (const auto& e : a.entries) {
    if (e.id != 0x4E20) continue;
    std::vector<PachEntry> subs;
    if (!PachRead(Unpack(e.data), subs)) continue;
    for (const auto& sub : subs) {
      const Bytes raw = Unpack(sub.data);
      const std::string t(raw.begin(), raw.end());
      if (!IsSeatFile(t)) continue;
      const std::vector<SeatLine> lines = SeatLines(t);
      const SeatParse s = ParseSeats(lines);
      for (size_t k = 0; k < s.stand_keys.size(); ++k) {
        CrowdStand c;
        c.key = s.stand_keys[k];
        std::string own;
        c.name = StandName(c.key, int(k), own);
        c.rows = s.stand_rows[k];
        v.push_back(std::move(c));
      }
      bool in_group = false;
      for (size_t i = s.people_from; i < lines.size(); ++i) {
        const std::string tl = TrimSeat(lines[i].text);
        if (!tl.empty() && tl[0] == '[') in_group = true;
        Person p;
        if (!in_group || !ParsePerson(lines[i].text, p)) continue;
        if (const auto it = s.rows.find(p.row); it != s.rows.end()) ++v[size_t(it->second.first)].people;
      }
      return v;  // (the first seat file: the others seat the same stands)
    }
  }
  return v;
}

int CropCrowd(Arena& a, const std::map<int, int>& keep) {
  std::vector<std::string> keys;
  for (const auto& s : CrowdStands(a)) keys.push_back(s.key);
  if (keys.empty()) return 0;
  int left_out = 0;
  for (auto& e : a.entries) {
    if (e.id != 0x4E20 && e.id != 0x4E2A) continue;
    const bool packed = IsBpe(e.data);
    std::vector<PachEntry> subs;
    if (!PachRead(Unpack(e.data), subs)) continue;
    bool changed = false;
    for (auto& sub : subs) {
      const Bytes raw = Unpack(sub.data);
      const std::string t(raw.begin(), raw.end());
      if (!IsSeatFile(t)) continue;
      const std::string c = CropSeatText(t, keys, keep, &left_out);
      if (c == t) continue;
      const Bytes w(c.begin(), c.end());
      sub.data = IsBpe(sub.data) ? BpeEncode(w) : w;
      changed = true;
    }
    if (!changed) continue;
    const Bytes pach = PachWrite(subs);
    e.data = packed ? BpeEncode(pach) : pach;
  }
  return left_out;
}

}  // namespace svrfmt
