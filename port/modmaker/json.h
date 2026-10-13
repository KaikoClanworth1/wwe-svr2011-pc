// A small JSON reader for the converters' output (the port_page wizard: the
// `games` / `list` / `convert` answers of tools/<kind>_port.py). Values:
// null, bool, number, string, array, object (keys in file order).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mm {

struct Json {
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject } type = Type::kNull;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<Json> items;                           // array
  std::vector<std::pair<std::string, Json>> fields;  // object

  bool IsNull() const { return type == Type::kNull; }
  // an object's field (a null value when absent)
  const Json& operator[](const char* key) const {
    static const Json kNull;
    if (type == Type::kObject)
      for (const auto& [k, v] : fields)
        if (k == key) return v;
    return kNull;
  }
  std::string Str() const {  // a string, a number or a bool as text
    if (type == Type::kString) return s;
    if (type == Type::kNumber) {
      char b2[32];
      if (n == static_cast<double>(static_cast<long long>(n))) std::snprintf(b2, sizeof b2, "%lld", (long long)n);
      else std::snprintf(b2, sizeof b2, "%g", n);
      return b2;
    }
    if (type == Type::kBool) return b ? "yes" : "no";
    return "";
  }
  int Int(int fallback = 0) const { return type == Type::kNumber ? int(n) : type == Type::kString && !s.empty() ? std::atoi(s.c_str()) : fallback; }
  bool Truthy() const { return type == Type::kBool ? b : type == Type::kNumber ? n != 0 : type == Type::kString ? !s.empty() : type != Type::kNull; }
};

namespace json_detail {

inline void Skip(const std::string& t, size_t& p) {
  while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) ++p;
}

inline void PutUtf8(std::string& out, unsigned cp) {
  if (cp < 0x80) out += char(cp);
  else if (cp < 0x800) out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 0x3F));
  else if (cp < 0x10000) out += char(0xE0 | cp >> 12), out += char(0x80 | (cp >> 6 & 0x3F)), out += char(0x80 | (cp & 0x3F));
  else out += char(0xF0 | cp >> 18), out += char(0x80 | (cp >> 12 & 0x3F)), out += char(0x80 | (cp >> 6 & 0x3F)), out += char(0x80 | (cp & 0x3F));
}

inline bool String(const std::string& t, size_t& p, std::string& out) {
  if (p >= t.size() || t[p] != '"') return false;
  ++p;
  while (p < t.size() && t[p] != '"') {
    if (t[p] != '\\') { out += t[p++]; continue; }
    if (++p >= t.size()) return false;
    const char c = t[p++];
    switch (c) {
      case 'n': out += '\n'; break;
      case 't': out += '\t'; break;
      case 'r': out += '\r'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'u': {
        if (p + 4 > t.size()) return false;
        unsigned cp = unsigned(std::strtoul(t.substr(p, 4).c_str(), nullptr, 16));
        p += 4;
        if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
          const unsigned lo = unsigned(std::strtoul(t.substr(p + 2, 4).c_str(), nullptr, 16));
          if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00), p += 6;
        }
        PutUtf8(out, cp);
        break;
      }
      default: out += c; break;  // \" \\ \/
    }
  }
  if (p >= t.size()) return false;
  ++p;
  return true;
}

inline bool Value(const std::string& t, size_t& p, Json& v, int depth) {
  if (depth > 64) return false;
  Skip(t, p);
  if (p >= t.size()) return false;
  const char c = t[p];
  if (c == '{') {
    v.type = Json::Type::kObject;
    ++p;
    Skip(t, p);
    if (p < t.size() && t[p] == '}') return ++p, true;
    for (;;) {
      Skip(t, p);
      std::string key;
      if (!String(t, p, key)) return false;
      Skip(t, p);
      if (p >= t.size() || t[p] != ':') return false;
      ++p;
      Json item;
      if (!Value(t, p, item, depth + 1)) return false;
      v.fields.emplace_back(std::move(key), std::move(item));
      Skip(t, p);
      if (p < t.size() && t[p] == ',') { ++p; continue; }
      if (p < t.size() && t[p] == '}') return ++p, true;
      return false;
    }
  }
  if (c == '[') {
    v.type = Json::Type::kArray;
    ++p;
    Skip(t, p);
    if (p < t.size() && t[p] == ']') return ++p, true;
    for (;;) {
      Json item;
      if (!Value(t, p, item, depth + 1)) return false;
      v.items.push_back(std::move(item));
      Skip(t, p);
      if (p < t.size() && t[p] == ',') { ++p; continue; }
      if (p < t.size() && t[p] == ']') return ++p, true;
      return false;
    }
  }
  if (c == '"') return v.type = Json::Type::kString, String(t, p, v.s);
  if (!t.compare(p, 4, "true")) return v.type = Json::Type::kBool, v.b = true, p += 4, true;
  if (!t.compare(p, 5, "false")) return v.type = Json::Type::kBool, v.b = false, p += 5, true;
  if (!t.compare(p, 4, "null")) return v.type = Json::Type::kNull, p += 4, true;
  char* end = nullptr;
  v.n = std::strtod(t.c_str() + p, &end);
  if (end == t.c_str() + p) return false;
  v.type = Json::Type::kNumber;
  p = size_t(end - t.c_str());
  return true;
}

}  // namespace json_detail

// The JSON in text (the last top-level value when a tool printed more than one
// line before it is fine: parsing starts at the first '{' or '[').
inline bool ParseJson(const std::string& text, Json& out) {
  size_t p = text.find_first_of("[{");
  if (p == std::string::npos) return false;
  out = Json();
  return json_detail::Value(text, p, out, 0);
}

}  // namespace mm
