// An arena's visibility tree (0xC364): models it leaves out given nodes (arena_vis.h).
#include "arena_vis.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

namespace svrfmt {

bool VisBounds(const Model& m, VisModel& out) {
  out.name = m.name;
  for (int j = 0; j < 3; ++j) out.lo[j] = 1e30f, out.hi[j] = -1e30f;
  for (const auto& me : m.meshes)
    for (const auto& v : me.verts)
      for (int j = 0; j < 3; ++j) out.lo[j] = std::min(out.lo[j], v.pos[j]), out.hi[j] = std::max(out.hi[j], v.pos[j]);
  return !m.name.empty() && out.lo[0] <= out.hi[0];
}

int AddVisNodes(PachEntry& vis, const std::vector<VisModel>& models) {
  const Bytes raw = Unpack(vis.data);
  std::string t(raw.begin(), raw.end());
  std::set<std::string> listed;
  const std::string kNode = "NODE MY_NAME = ";
  for (size_t at = t.find(kNode); at != std::string::npos; at = t.find(kNode, at + 1)) {
    size_t e = at + kNode.size();
    while (e < t.size() && !std::isspace(uint8_t(t[e]))) ++e;
    listed.insert(t.substr(at + kNode.size(), e - at - kNode.size()));
  }
  // VT_ROOT: "NODE MY_NAME = VT_ROOT" eol "{" eol <box> eol "}" eol <children>
  const size_t root = t.find(kNode + "VT_ROOT");
  if (root == std::string::npos) return 0;
  const size_t brace = t.find('{', root);
  const size_t box_at = brace == std::string::npos ? brace : t.find('\n', brace);
  const size_t box_end = box_at == std::string::npos ? box_at : t.find('\n', box_at + 1);
  const size_t close = box_end == std::string::npos ? box_end : t.find('}', box_end);
  const size_t count_at = close == std::string::npos ? close : t.find_first_of("0123456789", close);
  if (count_at == std::string::npos) return 0;
  size_t count_end = count_at;
  while (count_end < t.size() && std::isdigit(uint8_t(t[count_end]))) ++count_end;
  float rb[6] = {};
  {
    std::istringstream box(t.substr(box_at + 1, box_end - box_at - 1));
    for (float& f : rb) box >> f;
    if (!box) return 0;
  }
  const bool crlf = t.find("\r\n") != std::string::npos;
  const std::string nl = crlf ? "\r\n" : "\n";
  std::string tail;
  int added = 0;
  auto fmt6 = [](const float b[6]) {
    char line[200];
    std::snprintf(line, sizeof line, "\t%g %g %g %g %g %g", b[0], b[1], b[2], b[3], b[4], b[5]);
    return std::string(line);
  };
  for (const auto& m : models) {
    if (!listed.insert(m.name).second) continue;
    const float b[6] = {m.lo[0] / 10, m.lo[2] / 10, -m.hi[1] / 10, m.hi[0] / 10, m.hi[2] / 10, -m.lo[1] / 10};
    for (int j = 0; j < 3; ++j) rb[j] = std::min(rb[j], b[j]), rb[j + 3] = std::max(rb[j + 3], b[j + 3]);
    tail += kNode + m.name + " PARENT_NAME = VT_ROOT" + nl + "{" + nl + fmt6(b) + nl + "}" + nl + "0" + nl + nl;
    ++added;
  }
  if (!added) return 0;
  const int children = std::atoi(t.c_str() + count_at) + added;
  // (from the end: the earlier offsets stay valid)
  t.replace(count_at, count_end - count_at, std::to_string(children));
  size_t box_len = box_end - box_at - 1;
  if (box_len && t[box_end - 1] == '\r') --box_len;  // (its '\r' kept)
  t.replace(box_at + 1, box_len, fmt6(rb));
  while (!t.empty() && (t.back() == '\0')) t.pop_back();
  if (!t.empty() && t.back() != '\n') t += nl;
  t += tail;
  vis.data = BpeEncode(Bytes(t.begin(), t.end()));
  return added;
}

std::set<uint32_t> XFlagged(const std::vector<PachEntry>& entries) {
  std::set<uint32_t> out;
  for (const auto& e : entries) {
    if (e.id != 0xC351) continue;
    const Bytes raw = Unpack(e.data);
    std::istringstream lines(std::string(raw.begin(), raw.end()));
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream words(line);
      std::string w;
      if (!(words >> w) || !std::isdigit(uint8_t(w[0]))) continue;
      const uint32_t id = uint32_t(std::strtoul(w.c_str(), nullptr, 10));
      while (words >> w)
        if (w == "x") out.insert(id);
    }
  }
  return out;
}

int ListAllInVisibility(std::vector<PachEntry>& entries) {
  PachEntry* vis = nullptr;
  std::vector<VisModel> models;
  const std::set<uint32_t> x = XFlagged(entries);
  for (auto& e : entries) {
    if (e.id == 0xC364) vis = &e;
    if (e.id >= 0x190 || !x.count(e.id)) continue;
    Bytes m = Unpack(e.data);
    if (IsPach(m)) {  // (a model with its own parts: the JBOY is part 0)
      std::vector<PachEntry> parts;
      Bytes jb;
      if (PachRead(m, parts))
        for (const auto& p : parts)
          if (p.id == 0) jb = Unpack(p.data);
      m = std::move(jb);
    }
    Model model;
    VisModel v;
    if (IsJboy(m) && JboyRead(m, model) && VisBounds(model, v)) models.push_back(v);
  }
  return vis ? AddVisNodes(*vis, models) : 0;
}

}  // namespace svrfmt
