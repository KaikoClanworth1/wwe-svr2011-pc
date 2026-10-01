#include "online_directory.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>

namespace svr2011::online {
namespace {
using Json = nlohmann::json;
constexpr char kRoot[] = "/title/5451085D/sessions";
constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string Base64(const std::vector<uint8_t>& bytes) {
  std::string out;
  for (size_t i = 0; i < bytes.size(); i += 3) {
    uint32_t v = uint32_t(bytes[i]) << 16;
    if (i + 1 < bytes.size()) v |= uint32_t(bytes[i + 1]) << 8;
    if (i + 2 < bytes.size()) v |= bytes[i + 2];
    out += kBase64[v >> 18]; out += kBase64[(v >> 12) & 63];
    out += i + 1 < bytes.size() ? kBase64[(v >> 6) & 63] : '=';
    out += i + 2 < bytes.size() ? kBase64[v & 63] : '=';
  }
  return out;
}
std::vector<uint8_t> Unbase64(const std::string& s) {
  if (s.empty() || s.size() % 4 || s.size() > 5500)
    throw Error(kFailure, "Invalid property encoding");
  std::vector<uint8_t> out;
  for (size_t i = 0; i < s.size(); i += 4) {
    uint32_t v = 0; unsigned pad = 0;
    for (unsigned j = 0; j < 4; ++j) {
      if (s[i + j] == '=') {
        if (i + 4 != s.size() || j < 2) throw Error(kFailure, "Invalid padding");
        ++pad; v <<= 6;
      } else {
        auto p = std::find(std::begin(kBase64), std::end(kBase64) - 1, s[i + j]);
        if (pad || p == std::end(kBase64) - 1) throw Error(kFailure, "Invalid base64");
        v = (v << 6) | uint32_t(p - kBase64);
      }
    }
    if (pad > 2 || (pad == 2 && (v & 0xFFFF)) || (pad == 1 && (v & 0xFF)))
      throw Error(kFailure, "Noncanonical base64");
    out.push_back(uint8_t(v >> 16));
    if (pad < 2) out.push_back(uint8_t(v >> 8));
    if (!pad) out.push_back(uint8_t(v));
  }
  return out;
}
uint32_t U32(const Json& j, const char* key) {
  const auto& v = j.at(key);
  if (!v.is_number_unsigned() && !(v.is_number_integer() && v.get<int64_t>() >= 0))
    throw Error(kFailure, "Invalid directory integer");
  auto n = v.get<uint64_t>();
  if (n > UINT32_MAX) throw Error(kFailure, "Directory integer overflow");
  return uint32_t(n);
}
Session ReadSession(const Json& j) {
  Session s;
  s.id = ParseHex(j.at("id").get<std::string>());
  s.flags = U32(j, "flags");
  s.public_slots = U32(j, "publicSlotsCount"); s.private_slots = U32(j, "privateSlotsCount");
  s.open_public = U32(j, "openPublicSlotsCount"); s.open_private = U32(j, "openPrivateSlotsCount");
  s.filled_public = U32(j, "filledPublicSlotsCount"); s.filled_private = U32(j, "filledPrivateSlotsCount");
  auto port = U32(j, "port");
  if (!s.id || !port || port > 65535 || s.public_slots > 12 || s.private_slots > 12 ||
      s.public_slots + s.private_slots > 12 || s.open_public > s.public_slots ||
      s.open_private > s.private_slots || s.filled_public > s.public_slots ||
      s.filled_private > s.private_slots)
    throw Error(kFailure, "Invalid directory capacity/address");
  s.host.port = uint16_t(port);
  s.host.address = j.at("hostAddress").get<std::string>();
  ValidateAddress(s.host.address);
  s.host.mac = j.at("macAddress").get<std::string>(); ParseHex(s.host.mac, 12);
  return s;
}
Json Filters(const Search& s) {
  Json filters = Json::array();
  for (const auto& [id, value] : s.contexts) {
    Property p{id, 0, {uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)}};
    filters.push_back(EncodeProperty(p));
  }
  for (const auto& [id, p] : s.properties) filters.push_back(EncodeProperty(p));
  return filters;
}
}  // namespace

std::string EncodeProperty(const Property& p) {
  ValidateProperty(p);
  // Server property header is little endian; value bytes retain Xbox byte
  // order. This is NOT the 24-byte guest XUSER_PROPERTY memory layout.
  bool extended = p.type == 4 || p.type == 6;
  std::vector<uint8_t> wire(20 + (extended ? p.value.size() : 0), 0);
  for (unsigned i = 0; i < 4; ++i) wire[i] = uint8_t(p.id >> (i * 8));
  wire[4] = p.type;
  if (extended) {
    auto n = p.value.size();
    for (unsigned i = 0; i < 4; ++i) wire[12 + i] = uint8_t(n >> ((3 - i) * 8));
  }
  std::copy(p.value.begin(), p.value.end(), wire.begin() + (extended ? 20 : 12));
  return Base64(wire);
}
Property DecodeProperty(const std::string& s) {
  auto wire = Unbase64(s);
  if (wire.size() < 20) throw Error(kFailure, "Short property");
  Property p;
  for (unsigned i = 0; i < 4; ++i) p.id |= uint32_t(wire[i]) << (i * 8);
  p.type = wire[4];
  bool extended = p.type == 4 || p.type == 6;
  auto width = (p.type == 2 || p.type == 3 || p.type == 7) ? 8 : 4;
  if (!extended && wire.size() != 20) throw Error(kFailure, "Invalid scalar record");
  p.value.assign(wire.begin() + (extended ? 20 : 12), extended ? wire.end() : wire.begin() + 12 + width);
  ValidateProperty(p);
  return p;
}
std::string RestDirectory::Call(const std::string& method, const std::string& path, const std::string& body) {
  auto response = http_.Request(method, path, body);
  if (response.body.size() > 1024 * 1024) throw Error(kFailure, "Directory response too large");
  if (response.status == 404) throw Error(kNotFound, "Session not found");
  if (response.status < 200 || response.status >= 300)
    throw Error(kConnectionFailed, "Directory HTTP failure " + std::to_string(response.status));
  return response.body;
}
std::string RestDirectory::Path(uint64_t id) const { return std::string(kRoot) + '/' + Hex(id); }
void RestDirectory::Register(const Identity& i) {
  Call("POST", "/players", Json{{"xuid", Hex(i.xuid)}, {"gamertag", i.name},
      {"machineId", Hex(i.machine_id)}, {"hostAddress", i.address}, {"macAddress", i.mac}}.dump());
  xuid_ = i.xuid;
}
void RestDirectory::Create(const Session& s) {
  // The decoded retail media/version and compressed XLAST source are not yet
  // wired into startup. These placeholders identify a development session;
  // they do not claim cross-version or Xenia gameplay compatibility.
  Call("POST", kRoot, Json{{"xuid", Hex(s.host.xuid)}, {"title", "WWE SmackDown vs. Raw 2011"},
      {"mediaId", "00000000"}, {"version", "0.0.0.0"}, {"sessionId", Hex(s.id)}, {"flags", s.flags},
      {"publicSlotsCount", s.public_slots}, {"privateSlotsCount", s.private_slots},
      {"hostAddress", s.host.address}, {"macAddress", s.host.mac}, {"port", s.host.port}}.dump());
  try {
    if (!s.contexts.empty()) {
      Json contexts = Json::array();
      for (const auto& [id, value] : s.contexts) contexts.push_back({{"contextId", id}, {"value", value}});
      Call("POST", Path(s.id) + "/context", Json{{"contexts", contexts}}.dump());
    }
    if (!s.properties.empty()) {
      Json properties = Json::array();
      for (const auto& [id, p] : s.properties) properties.push_back(EncodeProperty(p));
      Call("POST", Path(s.id) + "/properties", Json{{"properties", properties}}.dump());
    }
  } catch (...) {
    // Best effort rollback. Server expiry/reconciliation is still required if
    // a response is lost or DELETE is refused by the source-address policy.
    try { Delete(s.id); } catch (...) {}
    throw;
  }
}
std::vector<Session> RestDirectory::Find(const Search& s) {
  auto j = Json::parse(Call("POST", std::string(kRoot) + "/search", Json{
      {"searchIndex", s.query}, {"resultsCount", s.limit}, {"numUsers", s.users},
      {"searcher_xuid", Hex(xuid_)}, {"filters", Filters(s)}}.dump()));
  if (!j.is_array() || j.size() > 25) throw Error(kFailure, "Invalid search result list");
  std::vector<Session> sessions;
  for (const auto& item : j) sessions.push_back(ReadSession(item));
  // Search DTO does not contain contexts/properties. Fetch query properties
  // individually; required by the retail lobby's names and match filters.
  for (auto& session : sessions) {
    auto props = Json::parse(Call("GET", Path(session.id) + "/properties/" + std::to_string(s.query)));
    for (const auto& value : props.at("properties")) {
      if (session.properties.size() >= 64) throw Error(kFailure, "Too many result properties");
      auto p = DecodeProperty(value.get<std::string>());
      if (session.properties.contains(p.id)) throw Error(kFailure, "Duplicate result property");
      session.property_order.push_back(p.id);
      session.properties.emplace(p.id, std::move(p));
    }
  }
  return sessions;
}
Session RestDirectory::Get(uint64_t id) {
  auto s = ReadSession(Json::parse(Call("GET", Path(id))));
  if (s.id != id) throw Error(kFailure, "Session ID mismatch");
  return s;
}
void RestDirectory::Join(uint64_t id, const std::vector<Member>& members) {
  Json ids = Json::array(), slots = Json::array();
  for (const auto& m : members) { ids.push_back(Hex(m.xuid)); slots.push_back(m.private_slot); }
  Call("POST", Path(id) + "/join", Json{{"xuids", ids}, {"privateSlots", slots}}.dump());
}
void RestDirectory::Leave(uint64_t id, const std::vector<uint64_t>& members) {
  Json ids = Json::array(); for (auto id : members) ids.push_back(Hex(id));
  Call("POST", Path(id) + "/leave", Json{{"xuids", ids}}.dump());
}
void RestDirectory::Delete(uint64_t id) {
  Call("DELETE", Path(id));
  // This server can acknowledge DELETE while refusing ownership. Verify the
  // record disappeared before discarding the local handle's session state.
  try { Get(id); } catch (const Error& e) { if (e.code == kNotFound) return; throw; }
  throw Error(kFailure, "Directory did not delete session (check host/source address)");
}
}  // namespace svr2011::online
