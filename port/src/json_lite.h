// WWE SmackDown vs. Raw 2011 - a little JSON: the Community Creations
// server's answers (online_overlay.cpp, leaderboards.cpp).
#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace svr2011 {

struct Json {
  enum Type { kNull, kBool, kNumber, kString, kArray, kObject } type = kNull;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> items;
  std::vector<std::pair<std::string, Json>> fields;

  const Json* operator[](const char* key) const {
    for (const auto& [k, v] : fields)
      if (k == key) return &v;
    return nullptr;
  }
  std::string Str(const char* key) const {
    const Json* v = (*this)[key];
    return v && v->type == kString ? v->s : std::string();
  }
};

struct JsonReader {
  const std::string& t;
  size_t i = 0;
  void Space() {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
  }
  bool String(std::string& out) {
    if (i >= t.size() || t[i] != '"') return false;
    for (++i; i < t.size() && t[i] != '"'; ++i) {
      char c = t[i];
      if (c == '\\' && i + 1 < t.size()) {
        c = t[++i];
        if (c == 'n') c = '\n';
        else if (c == 't') c = '\t';
        else if (c == 'u' && i + 4 < t.size()) {
          const unsigned v = unsigned(std::strtoul(t.substr(i + 1, 4).c_str(), nullptr, 16));
          i += 4;
          c = v < 128 ? char(v) : '?';
        }
      }
      out += c;
    }
    ++i;
    return true;
  }
  bool Value(Json& v, int depth = 0) {
    Space();
    if (i >= t.size() || depth > 16) return false;
    const char c = t[i];
    if (c == '{') {
      v.type = Json::kObject;
      ++i;
      for (;;) {
        Space();
        if (i < t.size() && t[i] == '}') return ++i, true;
        std::string key;
        if (!String(key)) return false;
        Space();
        if (i >= t.size() || t[i] != ':') return false;
        ++i;
        Json item;
        if (!Value(item, depth + 1)) return false;
        v.fields.emplace_back(std::move(key), std::move(item));
        Space();
        if (i < t.size() && t[i] == ',') ++i;
      }
    }
    if (c == '[') {
      v.type = Json::kArray;
      ++i;
      for (;;) {
        Space();
        if (i < t.size() && t[i] == ']') return ++i, true;
        Json item;
        if (!Value(item, depth + 1)) return false;
        v.items.push_back(std::move(item));
        Space();
        if (i < t.size() && t[i] == ',') ++i;
      }
    }
    if (c == '"') return v.type = Json::kString, String(v.s);
    if (t.compare(i, 4, "true") == 0) return i += 4, v.type = Json::kBool, v.b = true, true;
    if (t.compare(i, 5, "false") == 0) return i += 5, v.type = Json::kBool, true;
    if (t.compare(i, 4, "null") == 0) return i += 4, true;
    char* end = nullptr;
    v.n = std::strtod(t.c_str() + i, &end);
    if (end == t.c_str() + i) return false;
    i = size_t(end - t.c_str());
    v.type = Json::kNumber;
    return true;
  }
};

inline std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    if (uint8_t(c) >= 0x20) out += c;
  }
  return out + "\"";
}

}  // namespace svr2011
