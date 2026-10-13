// WWE SmackDown vs. Raw 2011 - Created Superstars' Paint Tool data in online
// matches, peer to peer (see online_cas.h).
#include "online_cas.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "online_net.h"
#include "p2p.h"

namespace svr2011 {

namespace {

constexpr size_t kCasDataSize = 187504;         // an attire's data (DataVersion 106)
constexpr uint32_t kCasHeader = 0x22C;          // then the .cas from its start
constexpr uint32_t kCawExt = 20 + 160000;       // the .cas's 'XLG1' record (caw_logos.cpp)
constexpr uint32_t kLocalIds = 0x40000000;      // ids of our own: 0x40000000 | owner << 16 | n
enum : uint8_t { kKindData = 1, kKindRecord = 2, kKindLogo = 3, kKindPicture = 4 };

std::filesystem::path g_saves;
std::mutex g_mutex;

std::filesystem::path Dir() { return g_saves / ".online"; }

std::string Name(uint32_t id, uint8_t kind) {
  return kind == kKindData ? std::to_string(id) + ".cas" : "r" + std::to_string(id) + ".txt";
}
std::filesystem::path CacheDir() { return Dir() / "cache"; }

std::optional<std::string> ReadFile(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream s;
  s << in.rdbuf();
  return s.str();
}

bool WriteFile(const std::filesystem::path& file, const std::string& data) {
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const auto tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(data.data(), std::streamsize(data.size()));
    if (!out) return false;
  }
  std::filesystem::rename(tmp, file, ec);
  return !ec;
}

// This player's part of the ids (14 bits of their online id).
uint32_t OwnerBits() {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : rex::cvar::Query<std::string>("online_xuid")) h = (h ^ c) * 1099511628211ull;
  return uint32_t(h ^ (h >> 32)) & 0x3FFF;
}

// A new id of this player's (the next free one).
uint32_t NewId() {
  const uint32_t prefix = kLocalIds | OwnerBits() << 16;
  uint32_t next = 1;
  std::error_code ec;
  for (const auto& e : std::filesystem::directory_iterator(Dir(), ec)) {
    const uint32_t id = uint32_t(std::strtoul(e.path().stem().string().c_str(), nullptr, 10));
    if ((id & 0xFFFF0000) == prefix) next = std::max(next, (id & 0xFFFF) + 1);
  }
  return prefix | (next & 0xFFFF);
}

// Saves\.online (this player's) or its cache (others'): an attire's data as
// <file id>.cas, its record as r<record id>.txt.
std::string Name(uint32_t id, uint8_t kind);

std::optional<std::string> Stored(uint32_t id, uint8_t kind) {
  const std::string name = Name(id, kind);
  if (auto d = ReadFile(Dir() / name)) return d;
  if (auto d = ReadFile(CacheDir() / name)) return d;
  if (kind == 2) return ReadFile(Dir() / (std::to_string(id) + ".txt"));  // (before records had the r)
  return std::nullopt;
}

// -- the record's fields: "name\ttype\tvalue" lines -----------------------------------

using Fields = std::map<std::string, std::pair<std::string, std::string>>;

Fields ParseFields(const std::string& text) {
  Fields f;
  std::istringstream in(text);
  for (std::string line; std::getline(in, line);) {
    const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
    if (b == std::string::npos) continue;
    f[line.substr(0, a)] = {line.substr(a + 1, b - a - 1), line.substr(b + 1)};
  }
  return f;
}

// The values of a SOAP CreateRecord (RecordField: name + typed value).
Fields SoapValues(const std::string& body) {
  static const std::regex field(
      R"(<(?:\w+:)?RecordField>\s*<(?:\w+:)?name>([^<]*)</(?:\w+:)?name>\s*<(?:\w+:)?value>\s*<(?:\w+:)?(\w+)>\s*(?:<(?:\w+:)?value>([^<]*)</(?:\w+:)?value>|<(?:\w+:)?value\s*/>))");
  Fields f;
  for (std::sregex_iterator it(body.begin(), body.end(), field), end; it != end; ++it) {
    f[(*it)[1]] = {(*it)[2], (*it)[3]};
  }
  return f;
}

std::vector<std::string> SoapList(const std::string& body, const char* list, const char* item) {
  std::vector<std::string> out;
  const std::regex section(std::string(R"(<(?:\w+:)?)") + list + R"(>([\s\S]*?)</(?:\w+:)?)" + list + ">");
  std::smatch m;
  if (!std::regex_search(body, m, section)) return out;
  const std::string inner = m[1];
  const std::regex one(std::string(R"(<(?:\w+:)?)") + item + R"(>([^<]*)</)");
  for (std::sregex_iterator it(inner.begin(), inner.end(), one), end; it != end; ++it) out.push_back((*it)[1]);
  return out;
}

std::string SoapText(const std::string& body, const char* tag) {
  const std::regex re(std::string(R"(<(?:\w+:)?)") + tag + R"(>([^<]*)</)");
  std::smatch m;
  return std::regex_search(body, m, re) ? std::string(m[1]) : std::string();
}

std::string Base64Be64(uint64_t v) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint8_t b[8];
  for (int i = 0; i < 8; ++i) b[i] = uint8_t(v >> (56 - 8 * i));
  std::string o;
  for (int i = 0; i < 8; i += 3) {
    const uint32_t n = uint32_t(b[i]) << 16 | (i + 1 < 8 ? uint32_t(b[i + 1]) << 8 : 0) | (i + 2 < 8 ? b[i + 2] : 0);
    o += t[n >> 18 & 63], o += t[n >> 12 & 63];
    o += i + 1 < 8 ? t[n >> 6 & 63] : '=';
    o += i + 2 < 8 ? t[n & 63] : '=';
  }
  return o;
}

std::string Esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '&': o += "&amp;"; break;
      case '<': o += "&lt;"; break;
      case '>': o += "&gt;"; break;
      default: o += c;
    }
  }
  return o;
}

std::string Http(const std::string& body, const std::string& extra_headers = {},
                 const char* type = "text/xml; charset=utf-8") {
  return "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(type) + "\r\n" + extra_headers +
         "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
}

std::string SakeReply(const std::string& func, const std::string& result, const std::string& extra = {}) {
  return Http(
      "<?xml version=\"1.0\" encoding=\"utf-8\"?><soap:Envelope "
      "xmlns:soap=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" xmlns:xsd=\"http://www.w3.org/2001/XMLSchema\">"
      "<soap:Body><" + func + "Response xmlns=\"http://gamespy.net/sake\"><" + func + "Result>" + result + "</" +
      func + "Result>" + extra + "</" + func + "Response></soap:Body></soap:Envelope>");
}

// -- fetching from the owner ----------------------------------------------------------

// The extra High Resolution logos an attire's data uses (its .cas's XLG1
// record), fetched in the background into Saves\.logos.
void FetchLogos(const std::string& data) {
  const size_t ext = kCasHeader + kCawExt;
  if (data.size() < ext + 24 + 80) return;
  uint32_t magic;
  std::memcpy(&magic, data.data() + ext, 4);
  if (magic != 0x584C4731) return;
  for (int k = 0; k < 10; ++k) {
    uint64_t hash;
    std::memcpy(&hash, data.data() + ext + 24 + 8 * k, 8);
    if (!data[ext + 8 + k] || !hash) continue;
    char name[24];
    std::snprintf(name, sizeof(name), "%016llX", static_cast<unsigned long long>(hash));
    const auto file = g_saves / ".logos" / (std::string(name) + ".bin");
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) continue;
    std::thread([key = std::string(name), file] {
      if (auto logo = P2PFetch(kKindLogo, key, std::chrono::seconds(20))) {
        WriteFile(file, *logo);
        REXLOG_INFO("online cas: logo {} from its owner", key);
      }
    }).detach();
  }
}

// A record / file of another player's: kept, or fetched from them now.
std::optional<std::string> Remote(uint32_t id, uint8_t kind) {
  if (auto d = Stored(id, kind)) return d;
  auto d = P2PFetch(kind, std::to_string(id), std::chrono::seconds(20));
  if (!d) return std::nullopt;
  std::lock_guard lock(g_mutex);
  WriteFile(CacheDir() / Name(id, kind), *d);
  if (kind == kKindData) FetchLogos(*d);
  return d;
}

// -- Created Superstar pictures in the lobby -------------------------------------------
//
// NNCreateSuperStar.cas: 4 attire blocks of 149528 bytes from 746340, each
// starting with the attire's 256 x 512 DXT5 render (as the game keeps it:
// 16-bit words swapped, untiled) - what the lists and the lobby show. A
// picture's key: "<owner's online id, 16 hex>:<superstar>:<attire>"; with
// ":h" its hash (16 hex) - fetched first, so a render is sent once per
// version (kept in Saves\.online\pictures\<hash>.bin).
constexpr uint32_t kCasSize = 0x148FCC;
constexpr uint32_t kAttireBlocks = 746340, kAttireBlock = 149528, kRender = 256 * 512;

std::filesystem::path CawFile(uint32_t id) {
  char name[32];
  std::snprintf(name, sizeof name, "%02uCreateSuperStar.cas", id);
  return g_saves / name;
}

uint64_t MyXuid() { return std::strtoull(rex::cvar::Query<std::string>("online_xuid").c_str(), nullptr, 16); }

std::string Hex16(uint64_t v) {
  char t[17];
  std::snprintf(t, sizeof t, "%016llX", static_cast<unsigned long long>(v));
  return t;
}

uint64_t Fnv(const std::string& d) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : d) h = (h ^ c) * 1099511628211ull;
  return h;
}

std::string PictureKey(uint64_t owner, uint32_t id, uint32_t attire) {
  return Hex16(owner) + ":" + std::to_string(id) + ":" + std::to_string(attire);
}

// This game's render of a Created Superstar's attire (nullopt: none).
std::optional<std::string> MyRender(uint32_t id, uint32_t attire) {
  if (id >= 50 || attire >= 4) return std::nullopt;
  std::ifstream in(CawFile(id), std::ios::binary);
  if (!in) return std::nullopt;
  in.seekg(kAttireBlocks + std::streamoff(attire) * kAttireBlock);
  std::string d(kRender, '\0');
  if (!in.read(d.data(), kRender)) return std::nullopt;
  return d;
}

// A peer asks for one of this game's pictures (or its hash).
std::optional<std::string> PictureSource(const std::string& key) {
  unsigned long long owner = 0;
  unsigned id = 0, attire = 0;
  char h = 0;
  const int n = std::sscanf(key.c_str(), "%16llX:%u:%u:%c", &owner, &id, &attire, &h);
  if (n < 3 || owner != MyXuid() || (n == 4 && h != 'h')) return std::nullopt;
  auto d = MyRender(id, attire);
  if (!d) return std::nullopt;
  if (n == 4) return Hex16(Fnv(*d));
  return d;
}

struct Picture {
  bool busy = false, ready = false;
  std::chrono::steady_clock::time_point tried{};
  std::string render;
};
std::map<std::string, Picture> g_pictures;  // by key (under g_mutex)

void FetchPicture(std::string key) {
  std::string render;
  const auto hash = P2PFetch(kKindPicture, key + ":h", std::chrono::seconds(10));
  if (hash && hash->size() == 16 && hash->find_first_not_of("0123456789ABCDEF") == std::string::npos) {
    const auto file = Dir() / "pictures" / (*hash + ".bin");
    if (auto kept = ReadFile(file); kept && kept->size() == kRender && Hex16(Fnv(*kept)) == *hash) {
      render = std::move(*kept);
      REXLOG_INFO("online cas: picture {} kept here ({})", key, *hash);
    } else if (auto d = P2PFetch(kKindPicture, key, std::chrono::seconds(20));
               d && d->size() == kRender && Hex16(Fnv(*d)) == *hash) {
      WriteFile(file, *d);
      render = std::move(*d);
      REXLOG_INFO("online cas: picture {} from its owner ({})", key, *hash);
    }
  }
  if (render.empty()) REXLOG_INFO("online cas: picture {} not had", key);
  std::lock_guard lock(g_mutex);
  Picture& p = g_pictures[key];
  p.busy = false;
  p.tried = std::chrono::steady_clock::now();
  if (!render.empty()) p.render = std::move(render), p.ready = true;
}

// What this game gives its peers.
std::optional<std::string> Source(uint8_t kind, const std::string& key) {
  if (kind == kKindPicture) return PictureSource(key);
  if (kind == kKindLogo) {
    if (key.size() != 16 || key.find_first_not_of("0123456789ABCDEF") != std::string::npos) return std::nullopt;
    return ReadFile(g_saves / ".logos" / (key + ".bin"));
  }
  const uint32_t id = uint32_t(std::strtoul(key.c_str(), nullptr, 10));
  if (!id || (kind != kKindData && kind != kKindRecord)) return std::nullopt;
  return Stored(id, kind);
}

// -- the server first -------------------------------------------------------------------

// The request as the relay would send it on; nullopt: no answer.
std::optional<net::Response> Forward(const std::string& method, const std::string& target,
                                     const std::map<std::string, std::string>& headers, const std::string& body) {
  net::Headers forward;
  for (const char* name : {"content-type", "soapaction"}) {
    if (auto it = headers.find(name); it != headers.end()) forward.emplace_back(name, it->second);
  }
  auto r = net::ServerRequest(method, "/game" + target, body, forward);
  if (r && r->status != 200) return std::nullopt;
  return r;
}

std::string HttpFrom(const net::Response& r) {
  std::string extra;
  for (const char* name : {"Sake-File-Result", "Sake-File-Id"}) {
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (auto it = r.headers.find(lower); it != r.headers.end()) extra += std::string(name) + ": " + it->second + "\r\n";
  }
  auto ct = r.headers.find("content-type");
  return Http(r.body, extra, ct != r.headers.end() ? ct->second.c_str() : "text/xml; charset=utf-8");
}

std::string RecordText(const Fields& values) {
  std::string text;
  for (const auto& [name, v] : values) text += name + "\t" + v.first + "\t" + v.second + "\n";
  return text;
}

// -- the relay's answers --------------------------------------------------------------

std::optional<std::string> Answer(const std::string& method, const std::string& target,
                                  const std::map<std::string, std::string>& headers, const std::string& body) {
  std::string path = target;
  std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) { return char(std::tolower(c)); });

  // An attire's data, uploaded: kept here under an id of ours.
  if (path.find("/sakefileserver/upload.aspx") != std::string::npos) {
    auto ct = headers.find("content-type");
    if (ct == headers.end()) return std::nullopt;
    const size_t b = ct->second.find("boundary=");
    if (b == std::string::npos) return std::nullopt;
    std::string boundary = ct->second.substr(b + 9);
    if (!boundary.empty() && boundary.front() == '"') boundary = boundary.substr(1, boundary.find('"', 1) - 1);
    const size_t start = body.find("\r\n\r\n", body.find("--" + boundary));
    const size_t end = start == std::string::npos ? start : body.find("\r\n--" + boundary, start + 4);
    if (end == std::string::npos || end - start - 4 != kCasDataSize) return std::nullopt;
    const std::string data = body.substr(start + 4, kCasDataSize);
    // (the server first: then the Superstar's data is everyone's, Community
    // Creations as it was - and a copy here, for peers when it's away)
    if (auto r = Forward(method, target, headers, body)) {
      auto res = r->headers.find("sake-file-result"), fid = r->headers.find("sake-file-id");
      if (res != r->headers.end() && res->second == "0" && fid != r->headers.end()) {
        const uint32_t id = uint32_t(std::strtoul(fid->second.c_str(), nullptr, 10));
        std::lock_guard lock(g_mutex);
        if (id) WriteFile(Dir() / Name(id, kKindData), data);
        REXLOG_INFO("online cas: Paint Tool data on the server as file {} (a copy kept here)", id);
        return HttpFrom(*r);
      }
    }
    std::lock_guard lock(g_mutex);
    const uint32_t id = NewId();
    if (!WriteFile(Dir() / Name(id, kKindData), data)) return std::nullopt;
    REXLOG_INFO("online cas: the server didn't take the Paint Tool data: kept here as {:08X}", id);
    return Http({}, "Sake-File-Result: 0\r\nSake-File-Id: " + std::to_string(id) + "\r\n", "text/html");
  }

  // Its file, for a game drawing the Superstar.
  if (path.find("/sakefileserver/download.aspx") != std::string::npos) {
    const size_t at = path.find("fileid=");
    const uint32_t id = at == std::string::npos ? 0 : uint32_t(std::strtoul(path.c_str() + at + 7, nullptr, 10));
    if (!id) return std::nullopt;
    if (id < kLocalIds) {
      if (auto r = Forward(method, target, headers, body)) {
        auto res = r->headers.find("sake-file-result");
        if (res != r->headers.end() && res->second == "0") {
          // (its extra High Resolution logos aren't on the server: from the owner)
          if (r->body.size() == kCasDataSize) {
            std::lock_guard lock(g_mutex);
            WriteFile(CacheDir() / Name(id, kKindData), r->body);
            FetchLogos(r->body);
          }
          return HttpFrom(*r);
        }
      }
      if (!Stored(id, kKindData)) REXLOG_INFO("online cas: the server doesn't have file {}: its owner?", id);
    }
    REXLOG_INFO("online cas: {} {} (file {:08X})", method, target, id);
    if (auto d = Remote(id, kKindData)) {
      return Http(*d, "Sake-File-Result: 0\r\n", "application/octet-stream");
    }
    return Http({}, "Sake-File-Result: 4\r\n", "text/html");
  }

  if (path.find("/sakestorageserver/") == std::string::npos || SoapText(body, "tableid") != "CASData") {
    return std::nullopt;
  }
  std::string func;
  if (auto sa = headers.find("soapaction"); sa != headers.end()) {
    for (const char* f : {"CreateRecord", "GetSpecificRecords"}) {
      if (sa->second.find(f) != std::string::npos) func = f;
    }
  }
  if (func == "CreateRecord") {
    Fields values = SoapValues(body);
    // (as the server does: the author is this player, whatever the game wrote)
    const uint64_t xuid = std::strtoull(rex::cvar::Query<std::string>("online_xuid").c_str(), nullptr, 16);
    if (values.count("AuthorName")) values["AuthorName"].second = std::to_string(int64_t(xuid));
    if (values.count("UserID")) values["UserID"] = {"binaryDataValue", Base64Be64(xuid)};
    values["ownerid"] = {"intValue", std::to_string(net::SignedInProfile())};
    const uint32_t file = uint32_t(std::strtoul(values["F00"].second.c_str(), nullptr, 10));
    if (file && file < kLocalIds) {  // (its data is on the server: so is the record, with a copy here)
      auto r = Forward(method, target, headers, body);
      if (!r) return std::nullopt;
      const uint32_t rid = uint32_t(std::strtoul(SoapText(r->body, "recordid").c_str(), nullptr, 10));
      if (rid && r->body.find("Success") != std::string::npos) {
        std::lock_guard lock(g_mutex);
        WriteFile(Dir() / Name(rid, kKindRecord), RecordText(values));
        REXLOG_INFO("online cas: record {} on the server (a copy kept here)", rid);
      }
      return HttpFrom(*r);
    }
    if (file < kLocalIds) return std::nullopt;
    std::lock_guard lock(g_mutex);
    if (!WriteFile(Dir() / Name(file, kKindRecord), RecordText(values))) return std::nullopt;
    REXLOG_INFO("online cas: record {:08X} kept here", file);
    return SakeReply(func, "Success", "<recordid>" + std::to_string(file) + "</recordid>");
  }
  if (func == "GetSpecificRecords") {
    const auto ids = SoapList(body, "recordids", "int");
    const auto fields = SoapList(body, "fields", "string");
    {
      std::string list;
      for (const auto& n : fields) list += " " + n;
      REXLOG_INFO("online cas: CASData asked for{} ({} record(s))", list, ids.size());
    }
    bool any_server = false;
    for (const auto& s : ids) any_server |= std::strtoul(s.c_str(), nullptr, 10) < kLocalIds;
    if (any_server) {
      if (auto r = Forward(method, target, headers, body)) {
        // (all of them, or the owners are asked: a record the server lost or never had)
        size_t rows = 0;
        for (size_t at = 0; (at = r->body.find("<ArrayOfRecordValue>", at)) != std::string::npos; ++at) ++rows;
        const bool complete = r->body.find("Success") != std::string::npos && rows >= ids.size();
        if (complete) return HttpFrom(*r);
      }
      REXLOG_INFO("online cas: the server can't give the records: their owners?");
    }
    std::string rows;
    for (const auto& s : ids) {
      const uint32_t id = uint32_t(std::strtoul(s.c_str(), nullptr, 10));
      const auto text = Remote(id, kKindRecord);
      if (!text) continue;
      Fields f = ParseFields(*text);
      // (its file too, now: the game asks for it next)
      if (const uint32_t file = uint32_t(std::strtoul(f["F00"].second.c_str(), nullptr, 10))) Remote(file, kKindData);
      rows += "<ArrayOfRecordValue>";
      for (const auto& name : fields) {
        std::string type = "intValue", value = "0";
        if (name == "recordid") {
          value = std::to_string(id);
        } else if (name == "ownerid") {
          value = f.count("ownerid") ? f["ownerid"].second : "0";
        } else if (const size_t dot = name.find('.'); dot != std::string::npos) {
          // (a file field's size, as the server gives it: 0 for no file)
          const std::string base = name.substr(0, dot);
          const bool has = f.count(base) && std::strtoul(f[base].second.c_str(), nullptr, 10) != 0;
          value = has ? std::to_string(kCasDataSize) : "0";
        } else if (auto it = f.find(name); it != f.end()) {
          type = it->second.first, value = it->second.second;
        }
        rows += "<RecordValue><" + type + "><value>" + Esc(value) + "</value></" + type + "></RecordValue>";
      }
      rows += "</ArrayOfRecordValue>";
    }
    return SakeReply(func, "Success", "<values>" + rows + "</values>");
  }
  return std::nullopt;
}

}  // namespace

const std::string* OnlinePicture(uint64_t owner, uint32_t id, uint32_t attire) {
  if (g_saves.empty() || !owner || id >= 50 || attire >= 4) return nullptr;
  const std::string key = PictureKey(owner, id, attire);
  std::lock_guard lock(g_mutex);
  Picture& p = g_pictures[key];
  if (p.ready) return &p.render;  // (never changed again: the pointer stays good)
  if (p.busy || std::chrono::steady_clock::now() - p.tried < std::chrono::seconds(15)) return nullptr;
  p.busy = true;
  std::thread(FetchPicture, key).detach();
  return nullptr;
}

int OnlineLocalCaw() {
  static int found = -1;
  static auto checked = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - checked < std::chrono::seconds(10)) return found;
  checked = now;
  found = -1;
  std::error_code ec;
  for (uint32_t id = 0; id < 50 && found < 0; ++id)
    if (std::filesystem::file_size(CawFile(id), ec) >= kCasSize && !ec) found = int(id);
  return found;
}

void InstallOnlineCas(const std::filesystem::path& saves) {
  if (!rex::cvar::Query<bool>("online_enabled")) return;
  g_saves = saves;
  std::error_code ec;
  std::filesystem::create_directories(CacheDir(), ec);
  // (other players' data: kept a month)
  const auto now = std::filesystem::file_time_type::clock::now();
  for (const auto& e : std::filesystem::directory_iterator(CacheDir(), ec)) {
    if (now - e.last_write_time(ec) > std::chrono::hours(24 * 30)) std::filesystem::remove(e.path(), ec);
  }
  net::SetLocalAnswer(Answer);
  SetP2PFileSource(Source);
}

}  // namespace svr2011
