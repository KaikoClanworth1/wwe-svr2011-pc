// WWE SmackDown vs. Raw 2011 - portable sessions; no match transport.
#include "online_sessions.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <set>
#include <sstream>

namespace svr2011::online {

std::string Hex(uint64_t value, size_t width) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(static_cast<int>(width)) << value;
  return out.str();
}

uint64_t ParseHex(const std::string& value, size_t width) {
  if (value.size() != width || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
    throw Error(kInvalidArgument, "Invalid hexadecimal identifier");
  return std::stoull(value, nullptr, 16);
}

void ValidateProperty(const Property& property) {
  size_t size = property.value.size();
  if (!property.id || size > 4096 ||
      ((property.type == 0 || property.type == 1 || property.type == 5) && size != 4) ||
      ((property.type == 2 || property.type == 3 || property.type == 7) && size != 8) ||
      (property.type == 4 && (size % 2)) || property.type > 7)
    throw Error(kInvalidArgument, "Unsupported property format");
}

void ValidateAddress(const std::string& address) {
  size_t start = 0;
  for (unsigned i = 0; i < 4; ++i) {
    auto end = address.find('.', start);
    if (end == std::string::npos) end = address.size();
    unsigned value = 0;
    auto parsed = std::from_chars(address.data() + start, address.data() + end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != address.data() + end || value > 255 ||
        (i < 3 && end == address.size()) || (i == 3 && end != address.size()))
      throw Error(kInvalidArgument, "Host address must be an IPv4 literal");
    start = end + 1;
  }
}

SessionService::SessionService(Directory& directory, Identity identity,
                               std::function<uint64_t()> random)
    : directory_(directory), identity_(std::move(identity)), random_(std::move(random)) {
  if (!identity_.xuid || !identity_.machine_id || identity_.address.empty() || !identity_.port)
    throw Error(kInvalidArgument, "Explicit peer identity/address/port required");
  ParseHex(identity_.mac, 12);
  ValidateAddress(identity_.address);
  if (!random_) throw Error(kInvalidArgument, "Session ID generator required");
}

void SessionService::Reserve(uint32_t handle) {
  std::lock_guard lock(mutex_);
  if (!handle || !sessions_.try_emplace(handle).second)
    throw Error(kInvalidHandle, "Duplicate or null session handle");
}

void SessionService::Release(uint32_t handle) {
  std::lock_guard lock(mutex_);
  // Handle destruction only releases local state. Explicit XSessionDelete owns
  // network cleanup; a process crash needs server expiry in a later milestone.
  sessions_.erase(handle);
}

LocalSession& SessionService::Lookup(uint32_t handle) {
  auto it = sessions_.find(handle);
  if (it == sessions_.end()) throw Error(kInvalidHandle, "Unknown session handle");
  return it->second;
}

Session SessionService::Create(uint32_t handle, uint32_t flags, uint32_t public_slots,
                               uint32_t private_slots, uint64_t join_id) {
  std::lock_guard lock(mutex_);
  auto& local = Lookup(handle);
  if (local.created || public_slots + private_slots == 0 ||
      public_slots > 12 || private_slots > 12 || public_slots + private_slots > 12)
    throw Error(kInvalidArgument, "Invalid capacity or already-created session");
  // Only unranked peer sessions. Offline/stat-only and arbitration stay out of
  // this adapter so a fabricated success cannot start an unsupported match.
  if (!(flags & 0x20) || (flags & 0x10)) throw Error(kUnsupported, "Only unranked peer sessions supported");
  if (!registered_) { directory_.Register(identity_); registered_ = true; }
  Session session;
  if (flags & 1) {
    session.id = (random_() & 0x00FFFFFFFFFFFFFFull) | 0xAE00000000000000ull;
    session.nonce = random_();
    session.flags = flags;
    session.public_slots = session.open_public = public_slots;
    session.private_slots = session.open_private = private_slots;
    session.host = identity_;
    session.contexts = contexts_;
    session.properties = properties_;
    directory_.Create(session);
  } else {
    if (!join_id) throw Error(kInvalidArgument, "Joining create requires discovered session ID");
    session = directory_.Get(join_id);
    if (!(session.flags & 0x20) || (session.flags & 0x10))
      throw Error(kUnsupported, "Only unranked peer sessions supported");
  }
  // Commit only after the directory acknowledges the operation.
  local = {session, true, bool(flags & 1), {}};
  return session;
}

std::vector<Session> SessionService::Find(const Search& search) {
  std::lock_guard lock(mutex_);
  if (!search.limit || search.limit > 25 || !search.users || search.users > 12)
    throw Error(kInvalidArgument, "Invalid search bounds");
  if (search.contexts.size() > 64 || search.properties.size() > 64)
    throw Error(kInvalidArgument, "Too many search filters");
  for (const auto& [id, p] : search.properties) ValidateProperty(p);
  if (!registered_) { directory_.Register(identity_); registered_ = true; }
  auto sessions = directory_.Find(search);
  sessions.erase(std::remove_if(sessions.begin(), sessions.end(), [&](const Session& s) {
    return !(s.flags & 0x20) || (s.flags & 0x10) || s.open_public + s.open_private < search.users;
  }), sessions.end());
  if (sessions.size() > search.limit) sessions.resize(search.limit);
  return sessions;
}

void SessionService::Join(uint32_t handle, const std::vector<Member>& members) {
  std::lock_guard lock(mutex_);
  auto& local = Lookup(handle);
  if (!local.created) throw Error(kInvalidHandle, "Session not created");
  if (members.empty() || members.size() > 12) throw Error(kInvalidArgument, "Invalid member count");
  std::set<uint64_t> seen;
  std::vector<Member> additions;
  for (const auto& member : members) {
    if (!member.xuid || !seen.insert(member.xuid).second)
      throw Error(kInvalidArgument, "Null or duplicate member");
    auto it = local.members.find(member.xuid);
    if (it == local.members.end()) additions.push_back(member);
    else if (it->second.private_slot != member.private_slot)
      throw Error(kInvalidArgument, "Existing membership cannot change slot type");
  }
  if (additions.empty()) return;
  auto fresh = directory_.Get(local.session.id);
  uint32_t public_needed = 0, private_needed = 0;
  for (const auto& member : additions) member.private_slot ? ++private_needed : ++public_needed;
  if (public_needed > fresh.open_public || private_needed > fresh.open_private)
    throw Error(kFailure, "Session has insufficient open slots");
  // This preflight avoids obvious overfill. Atomic admission remains a server
  // requirement: concurrent joins can race between GET and POST.
  directory_.Join(local.session.id, additions);
  for (const auto& member : additions) local.members.emplace(member.xuid, member);
}

void SessionService::Leave(uint32_t handle, const std::vector<uint64_t>& members) {
  std::lock_guard lock(mutex_);
  auto& local = Lookup(handle);
  if (!local.created) throw Error(kInvalidHandle, "Session not created");
  if (members.empty() || members.size() > 12) throw Error(kInvalidArgument, "Invalid member count");
  std::set<uint64_t> seen;
  std::vector<uint64_t> removals;
  for (auto xuid : members) {
    if (!xuid || !seen.insert(xuid).second) throw Error(kInvalidArgument, "Null or duplicate member");
    if (local.members.contains(xuid)) removals.push_back(xuid);
  }
  if (removals.empty()) return;
  directory_.Leave(local.session.id, removals);
  for (auto xuid : removals) local.members.erase(xuid);
}

void SessionService::Delete(uint32_t handle) {
  std::lock_guard lock(mutex_);
  auto& local = Lookup(handle);
  if (!local.created) return;
  if (local.host) directory_.Delete(local.session.id);
  else {
    std::vector<uint64_t> own;
    for (const auto& [id, member] : local.members)
      if (member.user_index != 0xFF) own.push_back(id);
    if (!own.empty()) directory_.Leave(local.session.id, own);
  }
  local = {};
}

LocalSession SessionService::Snapshot(uint32_t handle) {
  std::lock_guard lock(mutex_);
  return Lookup(handle);
}

void SessionService::SetContext(uint32_t id, uint32_t value) {
  std::lock_guard lock(mutex_);
  // Updating advertised metadata is a separate operation, not a silent cache
  // update. Until it is implemented, refuse changes while hosting.
  for (const auto& [handle, local] : sessions_)
    if (local.created && local.host) throw Error(kUnsupported, "Active metadata updates not implemented");
  if (contexts_.size() >= 64 && !contexts_.contains(id)) throw Error(kInvalidArgument, "Too many contexts");
  contexts_[id] = value;
}

void SessionService::SetProperty(Property property) {
  ValidateProperty(property);
  std::lock_guard lock(mutex_);
  for (const auto& [handle, local] : sessions_)
    if (local.created && local.host) throw Error(kUnsupported, "Active metadata updates not implemented");
  if (properties_.size() >= 64 && !properties_.contains(property.id)) throw Error(kInvalidArgument, "Too many properties");
  properties_[property.id] = std::move(property);
}

namespace {
uint64_t Read(std::span<const uint8_t> b, size_t offset, size_t width = 4) {
  uint64_t v = 0; for (size_t i = 0; i < width; ++i) v = (v << 8) | b[offset + i]; return v;
}
void Write(std::span<uint8_t> b, size_t offset, uint64_t value, size_t width = 4) {
  for (size_t i = 0; i < width; ++i) b[offset + i] = uint8_t(value >> ((width - 1 - i) * 8));
}
std::array<uint8_t, 4> IPv4(const std::string& ip) {
  std::array<uint8_t, 4> result{};
  size_t start = 0;
  for (unsigned i = 0; i < 4; ++i) {
    auto end = ip.find('.', start); if (end == std::string::npos) end = ip.size();
    unsigned v = 0;
    auto parsed = std::from_chars(ip.data() + start, ip.data() + end, v);
    if (parsed.ec != std::errc{} || parsed.ptr != ip.data() + end || v > 255 ||
        (i < 3 && end == ip.size()) || (i == 3 && end != ip.size()))
      throw Error(kInvalidArgument, "Session host must be an IPv4 literal");
    result[i] = uint8_t(v); start = end + 1;
  }
  return result;
}
void WriteInfo(std::span<uint8_t> b, size_t offset, const Session& s) {
  auto ip = IPv4(s.host.address); auto mac = ParseHex(s.host.mac, 12);
  std::fill(b.begin() + offset, b.begin() + offset + 60, 0);
  Write(b, offset, s.id, 8);
  std::copy(ip.begin(), ip.end(), b.begin() + offset + 8);
  std::copy(ip.begin(), ip.end(), b.begin() + offset + 12);
  Write(b, offset + 16, s.host.port, 2); Write(b, offset + 18, mac, 6);
  // abOnline and exchange key deliberately remain zero: no XNet security or
  // gameplay transport is provided in this groundwork. Do not start matches.
}
}  // namespace

Property GuestBridge::ReadProperty(uint32_t address) {
  auto b = memory_.Map(address, 24);
  Property p{uint32_t(Read(b, 0)), b[8], {}};
  if (p.type == 4 || p.type == 6) {
    auto size = Read(b, 16); if (size > 4096) throw Error(kInvalidArgument, "Property too large");
    if (size) { auto data = memory_.Map(uint32_t(Read(b, 20)), size); p.value.assign(data.begin(), data.end()); }
  } else {
    auto size = p.type == 2 || p.type == 3 || p.type == 7 ? 8 : 4;
    p.value.assign(b.begin() + 16, b.begin() + 16 + size);
  }
  ValidateProperty(p); return p;
}

void GuestBridge::WriteResults(uint32_t address, uint32_t capacity, const std::vector<Session>& sessions) {
  size_t required = 8 + sessions.size() * 92;
  for (const auto& s : sessions) {
    if (s.contexts.size() > 64 || s.properties.size() > 64) throw Error(kFailure, "Oversized result metadata");
    IPv4(s.host.address); ParseHex(s.host.mac, 12);
    required += s.contexts.size() * 8 + s.properties.size() * 24;
    std::vector<uint32_t> order = s.property_order;
    if (order.empty()) for (const auto& [id, p] : s.properties) order.push_back(id);
    if (order.size() != s.properties.size()) throw Error(kFailure, "Invalid result property order");
    auto unique = order; std::sort(unique.begin(), unique.end());
    if (std::adjacent_find(unique.begin(), unique.end()) != unique.end())
      throw Error(kFailure, "Duplicate result property order");
    for (auto id : order) {
      const auto& p = s.properties.at(id);
      ValidateProperty(p);
      if (p.type == 4 || p.type == 6) required += (p.value.size() + 3) & ~size_t(3);
    }
  }
  if (capacity < required) throw Error(kInsufficientBuffer, "Search result buffer too small");
  if (uint64_t(address) + required > uint64_t(UINT32_MAX) + 1) throw Error(kInvalidArgument, "Result address overflow");
  auto b = memory_.Map(address, required, true);
  std::fill(b.begin(), b.end(), 0);
  Write(b, 0, sessions.size()); Write(b, 4, sessions.empty() ? 0 : address + 8);
  size_t tail = 8 + sessions.size() * 92;
  for (size_t i = 0; i < sessions.size(); ++i) {
    const auto& s = sessions[i]; auto row = 8 + i * 92;
    WriteInfo(b, row, s);
    Write(b, row + 60, s.open_public); Write(b, row + 64, s.open_private);
    Write(b, row + 68, s.filled_public); Write(b, row + 72, s.filled_private);
    Write(b, row + 76, s.properties.size()); Write(b, row + 80, s.contexts.size());
    if (!s.contexts.empty()) Write(b, row + 88, address + tail);
    for (const auto& [id, value] : s.contexts) { Write(b, tail, id); Write(b, tail + 4, value); tail += 8; }
    if (!s.properties.empty()) Write(b, row + 84, address + tail);
    size_t record = tail; tail += s.properties.size() * 24;
    std::vector<uint32_t> order = s.property_order;
    if (order.empty()) for (const auto& [id, p] : s.properties) order.push_back(id);
    for (auto id : order) {
      const auto& p = s.properties.at(id);
      Write(b, record, id); b[record + 8] = p.type;
      if (p.type == 4 || p.type == 6) {
        Write(b, record + 16, p.value.size()); Write(b, record + 20, p.value.empty() ? 0 : address + tail);
        std::copy(p.value.begin(), p.value.end(), b.begin() + tail);
        tail += (p.value.size() + 3) & ~size_t(3);
      } else std::copy(p.value.begin(), p.value.end(), b.begin() + record + 16);
      record += 24;
    }
  }
}

std::optional<uint32_t> GuestBridge::Dispatch(uint32_t message, uint32_t address, uint32_t length) {
  size_t expected = 0;
  bool unsupported_state = false;
  switch (message) {
    case 0xB0006: expected = 24; break;
    case 0xB0007: expected = 28; break;
    case 0xB0010: expected = 28; break;
    case 0xB0011: expected = 16; break;
    case 0xB0012: case 0xB0013: expected = 20; break;
    case 0xB0016: expected = 32; break;
    case 0xB001C: expected = 36; break;
    // These require lobby/state/QoS work. A successful SDK stub would be
    // misleading once this provider owns sessions, so fail explicitly.
    case 0xB0014: case 0xB0015: case 0xB0018: case 0xB001A:
    case 0xB001D: case 0xB001E: case 0xB001F: case 0xB0025:
    case 0xB0026: expected = 4; unsupported_state = true; break;
    case 0xB0019: case 0xB001B: case 0xB0060: case 0xB0065:
      return kUnsupported;
    default: return std::nullopt;
  }
  try {
    if (!unsupported_state && length && length != expected)
      throw Error(kInvalidArgument, "Unexpected XGI buffer length");
    auto b = memory_.Map(address, expected);
    auto u32 = [&](size_t n) { return uint32_t(Read(b, n)); };
    if (unsupported_state) {
      // Offline/stat-only sessions continue through the existing SDK. Only
      // this provider's created peer sessions must reject match operations.
      if (!service_.Snapshot(memory_.SessionHandle(u32(0))).created) return std::nullopt;
      return kUnsupported;
    }
    if (message == 0xB0010 && !(u32(4) & 0x20)) return std::nullopt;
    if (message == 0xB0011 || message == 0xB0012 || message == 0xB0013) {
      if (!service_.Snapshot(memory_.SessionHandle(u32(0))).created) return std::nullopt;
    }
    auto user = [&](uint32_t index) { if (index != 0) throw Error(kUnsupported, "Only local user zero supported"); };
    if (message == 0xB0006) { user(u32(0)); service_.SetContext(u32(16), u32(20)); }
    else if (message == 0xB0007) {
      user(u32(0)); auto id = u32(16), size = u32(20);
      if (size > 4096) throw Error(kInvalidArgument, "Property too large");
      Property p{id, uint8_t(id >> 28), {}};
      if (size) { auto v = memory_.Map(u32(24), size); p.value.assign(v.begin(), v.end()); }
      service_.SetProperty(std::move(p));
    } else if (message == 0xB0010) {
      user(u32(16)); auto handle = memory_.SessionHandle(u32(0));
      auto info = memory_.Map(u32(20), 60, true); auto nonce = memory_.Map(u32(24), 8, true);
      auto join_id = Read(info, 0, 8);
      auto session = service_.Create(handle, u32(4), u32(8), u32(12), join_id);
      WriteInfo(info, 0, session); Write(nonce, 0, session.nonce, 8);
    } else if (message == 0xB0011) service_.Delete(memory_.SessionHandle(u32(0)));
    else if (message == 0xB0012 || message == 0xB0013) {
      auto handle = memory_.SessionHandle(u32(0)), count = u32(4);
      if (!count || count > 12) throw Error(kInvalidArgument, "Invalid member count");
      auto ids = memory_.Map(u32(8) ? u32(8) : u32(12), count * (u32(8) ? 8 : 4));
      std::span<uint8_t> slots;
      if (message == 0xB0012) slots = memory_.Map(u32(16), count * 4);
      std::vector<Member> members; std::vector<uint64_t> xuids;
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = 0xFF; uint64_t xuid;
        if (u32(8)) xuid = Read(ids, i * 8, 8);
        else { index = uint32_t(Read(ids, i * 4)); user(index); xuid = service_.local_xuid(); }
        uint32_t slot = slots.empty() ? 0 : uint32_t(Read(slots, i * 4));
        if (slot > 1) throw Error(kInvalidArgument, "Invalid private slot flag");
        members.push_back({xuid, index, bool(slot)}); xuids.push_back(xuid);
      }
      if (message == 0xB0012) service_.Join(handle, members); else service_.Leave(handle, xuids);
    } else {
      user(u32(4)); Search s; s.query = u32(0); s.limit = u32(8); s.users = expected == 36 ? u32(32) : 1;
      auto props = Read(b, 12, 2), contexts = Read(b, 14, 2);
      if (props > 64 || contexts > 64 || u32(24) > 1024 * 1024)
        throw Error(kInvalidArgument, "Search bounds exceeded");
      // Validate the caller's output span before issuing any backend request.
      if (u32(24) < 8) throw Error(kInsufficientBuffer, "Search needs a result header");
      memory_.Map(u32(28), u32(24), true);
      if (props) {
        memory_.Map(u32(16), props * 24);
        for (uint32_t i = 0; i < props; ++i) { auto p = ReadProperty(u32(16) + i * 24); s.properties[p.id] = std::move(p); }
      }
      if (contexts) {
        auto c = memory_.Map(u32(20), contexts * 8);
        for (uint32_t i = 0; i < contexts; ++i) s.contexts[uint32_t(Read(c, i * 8))] = uint32_t(Read(c, i * 8 + 4));
      }
      WriteResults(u32(28), u32(24), service_.Find(s));
    }
    return kSuccess;
  } catch (const Error& e) { return e.code; }
  catch (...) { return kFailure; }
}
}  // namespace svr2011::online
