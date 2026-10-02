// WWE SmackDown vs. Raw 2011 - peer-to-peer online matches (see p2p.h).
//
// The session message decoding (XGI B0006-B001C buffers, search results in
// the caller's memory) follows PR #3, "Add groundwork for P2P online
// sessions", by gitSothib - its session directory is a protocol between the
// games here instead of a web service.
#include "p2p.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <rex/cvar.h>
#include <rex/kernel/xam/online_hooks.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_BOOL(p2p_enabled, true, "Online", "Peer-to-peer online matches (with online_enabled)");
REXCVAR_DEFINE_UINT32(p2p_port, 36000, "Online",
                      "This game's port base for online matches (UDP; a second game on the same PC: 36100)");
REXCVAR_DEFINE_STRING(p2p_address, "", "Online",
                      "The LAN address this game gives peers (tests: a second game on this PC, e.g. 127.0.0.2)");
REXCVAR_DEFINE_STRING(p2p_peers, "", "Online",
                      "Addresses searched besides the LAN, comma-separated (host or host:port)");

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint16_t kBasePort = 36000;            // (port bases are kBasePort + n * 100)
constexpr int kInstances = 4;                    // (searched on the LAN and this PC)
constexpr uint32_t kFakeNet = 0x0A400000;        // 10.64.0.0/16: peers as the game sees them
constexpr uint32_t kMagic = 0x53565250;          // "SVRP"
constexpr uint8_t kVersion = 1;
enum : uint8_t { kSearch = 1, kSessions = 2, kQosRequest = 3, kQosReply = 4, kHello = 5 };

rex::memory::Memory* g_memory = nullptr;

// -- byte helpers --------------------------------------------------------------------

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint64_t Be64(const uint8_t* p) { return uint64_t(Be32(p)) << 32 | Be32(p + 4); }
void Put32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
void Put64(uint8_t* p, uint64_t v) { Put32(p, uint32_t(v >> 32)), Put32(p + 4, uint32_t(v)); }

uint8_t* Guest(uint32_t address) { return g_memory->TranslateVirtual<uint8_t*>(address); }

std::string Ip(uint32_t ip) {
  return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) +
         "." + std::to_string(ip & 255);
}

// A message: appended fields, read back in order.
struct Writer {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { b.push_back(uint8_t(v >> 8)), b.push_back(uint8_t(v)); }
  void u32(uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (8 * i))); }
  void u64(uint64_t v) { u32(uint32_t(v >> 32)), u32(uint32_t(v)); }
  void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
};
struct Reader {
  const uint8_t* p;
  size_t n, at = 0;
  bool ok = true;
  bool need(size_t k) { return ok = ok && at + k <= n; }
  uint8_t u8() { return need(1) ? p[at++] : 0; }
  uint16_t u16() { uint16_t v = need(2) ? Be16(p + at) : 0; at += ok ? 2 : 0; return v; }
  uint32_t u32() { uint32_t v = need(4) ? Be32(p + at) : 0; at += ok ? 4 : 0; return v; }
  uint64_t u64() { uint64_t v = need(8) ? Be64(p + at) : 0; at += ok ? 8 : 0; return v; }
  void bytes(uint8_t* out, size_t k) {
    if (need(k)) std::memcpy(out, p + at, k), at += k;
  }
};

// -- sockets -------------------------------------------------------------------------

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kBadSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
#else
using Socket = int;
const Socket kBadSocket = -1;
void CloseSocket(Socket s) { close(s); }
#endif

Socket g_socket = kBadSocket;  // the discovery socket (this game's port base)

void SendTo(uint32_t ip, uint16_t port, const std::vector<uint8_t>& data) {
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(ip);
  to.sin_port = htons(port);
  sendto(g_socket, reinterpret_cast<const char*>(data.data()), int(data.size()), 0, reinterpret_cast<sockaddr*>(&to),
         sizeof(to));
}

// This PC's address on its network (the interface the default route uses).
uint32_t LanIp() {
  Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBadSocket) return 0x7F000001;
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(0x08080808);  // (no packet is sent)
  to.sin_port = htons(53);
  uint32_t ip = 0x7F000001;
  if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
    sockaddr_in me = {};
    socklen_t len = sizeof(me);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0 && me.sin_addr.s_addr) ip = ntohl(me.sin_addr.s_addr);
  }
  CloseSocket(s);
  return ip;
}

// -- this game's address and its peers ----------------------------------------------

using Mac = std::array<uint8_t, 6>;
using XnAddr = std::array<uint8_t, 36>;  // in_addr ina, inaOnline, be16 port, abEnet[6], abOnline[20]

struct Me {
  uint32_t lan = 0x7F000001, online = 0;
  uint16_t base = kBasePort;
  Mac mac{};
  uint64_t xuid = 0;
} g_me;

uint16_t Offset(uint16_t base) { return uint16_t(base - kBasePort); }

XnAddr MyXnAddr() {
  XnAddr a{};
  Put32(a.data(), g_me.lan);
  Put32(a.data() + 4, g_me.online ? g_me.online : g_me.lan);
  Put16(a.data() + 8, g_me.base);
  std::memcpy(a.data() + 10, g_me.mac.data(), 6);
  Put64(a.data() + 16, g_me.xuid);
  return a;
}

struct Peer {
  XnAddr xnaddr{};
  uint32_t fake = 0;   // what the game knows it as
  uint32_t route = 0;  // where its packets go (the address it was last heard from, or its own)
  uint16_t base = kBasePort;
};

std::mutex g_mutex;
std::map<Mac, Peer> g_peers;
std::map<uint32_t, Mac> g_fake;  // fake address -> peer
std::set<uint16_t> g_bound;      // the game's own ports (as it knows them)

Mac MacOf(const uint8_t* xnaddr) {
  Mac m;
  std::memcpy(m.data(), xnaddr + 10, 6);
  return m;
}

// The way to a peer: this PC, its LAN address (same network) or its public one.
uint32_t RouteTo(const uint8_t* xnaddr) {
  const uint32_t lan = Be32(xnaddr), online = Be32(xnaddr + 4);
  if (lan == g_me.lan) return lan;  // (a second game on this PC: the game binds its own address, not loopback)
  if (!online || online == lan || online == (g_me.online ? g_me.online : g_me.lan)) return lan;
  return online;
}

// The peer at xnaddr (added if new); `from`: where it was just heard from.
Peer& Learn(const uint8_t* xnaddr, uint32_t from = 0) {
  const Mac mac = MacOf(xnaddr);
  auto [it, added] = g_peers.try_emplace(mac);
  Peer& p = it->second;
  std::memcpy(p.xnaddr.data(), xnaddr, 36);
  p.base = Be16(xnaddr + 8);
  if (added) {
    uint32_t fake = kFakeNet | (uint32_t(mac[4]) << 8 | mac[5]);
    while (g_fake.count(fake) || (fake & 0xFF) == 0 || (fake & 0xFF) == 255) fake = kFakeNet | ((fake + 1) & 0xFFFF);
    p.fake = fake;
    g_fake[fake] = mac;
  }
  // (heard from: its address as this PC sees it; loopback: this PC, by its own address)
  if (from == 0x7F000001) from = Be32(xnaddr);
  p.route = from ? from : (p.route ? p.route : RouteTo(xnaddr));
  if (added) {
    REXLOG_INFO("p2p: peer {:02X}{:02X}{:02X}{:02X}{:02X}{:02X} at {}:{} is {}", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5], Ip(p.route), p.base, Ip(p.fake));
  }
  return p;
}

// -- sessions ----------------------------------------------------------------------------

struct Property {
  uint32_t id = 0;
  uint8_t type = 0;
  std::vector<uint8_t> value;  // (guest byte order)
};

struct Session {
  uint64_t id = 0, nonce = 0;
  XnAddr host{};
  uint32_t flags = 0, public_slots = 0, private_slots = 0, filled_public = 0, filled_private = 0;
  std::map<uint32_t, uint32_t> contexts;
  std::vector<Property> properties;
};

struct Local {
  Session session;
  bool host = false;
  std::set<uint64_t> members_public, members_private;
};

std::map<uint32_t, uint32_t> g_contexts;      // the player's (B0006)
std::map<uint32_t, Property> g_properties;    // (B0007)
std::map<uint32_t, Local> g_sessions;         // by session object
std::map<uint64_t, std::vector<uint8_t>> g_qos;  // QoS data by session id (XNetQosListen)
std::map<uint64_t, Session> g_found;          // the last search's sessions

void WriteSession(Writer& w, const Session& s) {
  w.u64(s.id);
  w.bytes(s.host.data(), 36);
  w.u32(s.flags), w.u32(s.public_slots), w.u32(s.private_slots), w.u32(s.filled_public), w.u32(s.filled_private);
  w.u16(uint16_t(s.contexts.size()));
  for (const auto& [id, v] : s.contexts) w.u32(id), w.u32(v);
  w.u16(uint16_t(s.properties.size()));
  for (const auto& p : s.properties) {
    w.u32(p.id), w.u8(p.type), w.u16(uint16_t(p.value.size()));
    w.bytes(p.value.data(), p.value.size());
  }
}

bool ReadSession(Reader& r, Session& s) {
  s.id = r.u64();
  r.bytes(s.host.data(), 36);
  s.flags = r.u32(), s.public_slots = r.u32(), s.private_slots = r.u32(), s.filled_public = r.u32(),
  s.filled_private = r.u32();
  const uint16_t nc = r.u16();
  for (uint16_t i = 0; i < nc && r.ok && i < 64; ++i) {
    const uint32_t id = r.u32();
    s.contexts[id] = r.u32();
  }
  const uint16_t np = r.u16();
  for (uint16_t i = 0; i < np && r.ok && i < 64; ++i) {
    Property p;
    p.id = r.u32(), p.type = r.u8();
    const uint16_t len = r.u16();
    if (len > 4096 || !r.need(len)) return false;
    p.value.assign(r.p + r.at, r.p + r.at + len);
    r.at += len;
    s.properties.push_back(std::move(p));
  }
  return r.ok && s.id && s.public_slots + s.private_slots <= 32;
}

// -- the discovery protocol ------------------------------------------------------------

std::mutex g_wait_mutex;
std::condition_variable g_wait;
std::map<uint32_t, std::vector<std::vector<uint8_t>>> g_replies;  // by request nonce

std::vector<uint8_t> Header(uint8_t type, uint32_t nonce) {
  Writer w;
  w.u32(kMagic), w.u8(kVersion), w.u8(type), w.u32(nonce);
  w.bytes(MyXnAddr().data(), 36);
  return w.b;
}

void OnPacket(const uint8_t* data, size_t size, uint32_t from_ip, uint16_t from_port) {
  Reader r{data, size};
  if (r.u32() != kMagic || r.u8() != kVersion) return;
  const uint8_t type = r.u8();
  const uint32_t nonce = r.u32();
  XnAddr sender{};
  r.bytes(sender.data(), 36);
  if (!r.ok || MacOf(sender.data()) == g_me.mac) return;  // (our own broadcast)
  {
    std::lock_guard lock(g_mutex);
    Learn(sender.data(), from_ip);
  }
  switch (type) {
    case kSearch: {  // -> the sessions this game hosts
      Writer w;
      w.b = Header(kSessions, nonce);
      std::vector<Session> mine;
      {
        std::lock_guard lock(g_mutex);
        for (const auto& [obj, local] : g_sessions) {
          if (local.host) mine.push_back(local.session);
        }
      }
      w.u16(uint16_t(mine.size()));
      for (const auto& s : mine) WriteSession(w, s);
      SendTo(from_ip, from_port, w.b);
      break;
    }
    case kQosRequest: {
      const uint64_t id = r.u64();
      Writer w;
      w.b = Header(kQosReply, nonce);
      std::vector<uint8_t> qos;
      {
        std::lock_guard lock(g_mutex);
        if (auto it = g_qos.find(id); it != g_qos.end()) qos = it->second;
      }
      w.u64(id), w.u16(uint16_t(qos.size()));
      w.bytes(qos.data(), qos.size());
      SendTo(from_ip, from_port, w.b);
      break;
    }
    case kHello:
      break;  // (Learn: done)
    case kSessions:
    case kQosReply: {
      std::lock_guard lock(g_wait_mutex);
      if (auto it = g_replies.find(nonce); it != g_replies.end()) {
        it->second.emplace_back(data + r.at, data + size);
        g_wait.notify_all();
      }
      break;
    }
  }
}

void Receive() {
  std::vector<uint8_t> buf(65536);
  for (;;) {
    sockaddr_in from = {};
    socklen_t len = sizeof(from);
    const int n = int(recvfrom(g_socket, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &len));
    if (n <= 0) {
#if defined(_WIN32)
      if (WSAGetLastError() == WSAECONNRESET) continue;  // (an ICMP "port unreachable" from an earlier send)
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    OnPacket(buf.data(), size_t(n), ntohl(from.sin_addr.s_addr), ntohs(from.sin_port));
  }
}

uint32_t NewNonce() {
  static std::atomic<uint32_t> next{uint32_t(std::random_device{}())};
  return ++next;
}

// Sends `request` to each target and collects the answers for `wait`.
std::vector<std::vector<uint8_t>> Ask(const std::vector<uint8_t>& request, uint32_t nonce,
                                      const std::vector<std::pair<uint32_t, uint16_t>>& targets,
                                      std::chrono::milliseconds wait, size_t enough = SIZE_MAX) {
  {
    std::lock_guard lock(g_wait_mutex);
    g_replies[nonce];
  }
  for (const auto& [ip, port] : targets) SendTo(ip, port, request);
  std::unique_lock lock(g_wait_mutex);
  g_wait.wait_for(lock, wait, [&] { return g_replies[nonce].size() >= enough; });
  auto replies = std::move(g_replies[nonce]);
  g_replies.erase(nonce);
  return replies;
}

// The places a search asks: the LAN, this PC and p2p_peers (ports: the bases).
std::vector<std::pair<uint32_t, uint16_t>> SearchTargets() {
  std::vector<std::pair<uint32_t, uint16_t>> targets;
  for (int i = 0; i < kInstances; ++i) {
    const uint16_t port = uint16_t(kBasePort + 100 * i);
    targets.emplace_back(0xFFFFFFFF, port);
    targets.emplace_back(0x7F000001, port);
  }
  std::stringstream list(REXCVAR_GET(p2p_peers));
  for (std::string item; std::getline(list, item, ',');) {
    item.erase(0, item.find_first_not_of(" \t"));
    item.erase(item.find_last_not_of(" \t") + 1);
    if (item.empty()) continue;
    uint16_t port = kBasePort;
    if (const size_t colon = item.rfind(':'); colon != std::string::npos) {
      port = uint16_t(std::atoi(item.c_str() + colon + 1));
      item.resize(colon);
    }
    addrinfo hints = {}, *found = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(item.c_str(), nullptr, &hints, &found) == 0 && found) {
      targets.emplace_back(ntohl(reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr), port);
      freeaddrinfo(found);
    }
  }
  return targets;
}

std::vector<Session> Search() {
  const uint32_t nonce = NewNonce();
  auto replies = Ask(Header(kSearch, nonce), nonce, SearchTargets(), std::chrono::milliseconds(700));
  std::map<uint64_t, Session> found;
  for (const auto& reply : replies) {
    Reader r{reply.data(), reply.size()};
    const uint16_t count = r.u16();
    for (uint16_t i = 0; i < count && r.ok && i < 25; ++i) {
      Session s;
      if (!ReadSession(r, s)) break;
      found[s.id] = std::move(s);
    }
  }
  std::vector<Session> sessions;
  std::lock_guard lock(g_mutex);
  for (auto& [id, s] : found) {
    Learn(s.host.data());
    g_found[id] = s;
    sessions.push_back(std::move(s));
  }
  return sessions;
}

// Tells a host this game is coming (so its packets are recognised).
void Hello(const uint8_t* host_xnaddr) {
  uint32_t route;
  uint16_t base;
  {
    std::lock_guard lock(g_mutex);
    Peer& p = Learn(host_xnaddr);
    route = p.route, base = p.base;
  }
  const auto msg = Header(kHello, NewNonce());
  for (int i = 0; i < 3; ++i) SendTo(route, base, msg);
}

// -- XNet ------------------------------------------------------------------------------

uint32_t TitleXnAddr(uint8_t* out) {
  const XnAddr a = MyXnAddr();
  std::memcpy(out, a.data(), 36);
  // XNET_GET_XNADDR_STATIC | GATEWAY | DNS | ONLINE
  return 0x04 | 0x20 | 0x40 | 0x80;
}

uint32_t XnAddrToInAddr(const uint8_t* xnaddr) {
  if (MacOf(xnaddr) == g_me.mac) return 0x7F000001;
  bool known;
  uint32_t fake;
  {
    std::lock_guard lock(g_mutex);
    known = g_peers.count(MacOf(xnaddr)) != 0;
    fake = Learn(xnaddr).fake;
  }
  if (!known) Hello(xnaddr);
  return fake;
}

bool InAddrToXnAddr(uint32_t ina, uint8_t* out) {
  std::lock_guard lock(g_mutex);
  auto it = g_fake.find(ina);
  if (it == g_fake.end()) return false;
  std::memcpy(out, g_peers[it->second].xnaddr.data(), 36);
  return true;
}

// (the first few of each way: SVR2011_P2P_TRACE=1 logs them all)
void Trace(const char* way, uint32_t a, uint16_t ap, uint32_t b, uint16_t bp) {
  static const bool all = std::getenv("SVR2011_P2P_TRACE") != nullptr;
  static std::atomic<int> count{0};
  if (all || ++count <= 40) REXLOG_INFO("p2p: {} {}:{} -> {}:{}", way, Ip(a), ap, Ip(b), bp);
}

void ToHost(uint32_t& addr, uint16_t& port) {
  if ((addr & 0xFFFF0000) != kFakeNet) return;
  std::lock_guard lock(g_mutex);
  auto it = g_fake.find(addr);
  if (it == g_fake.end()) {
    Trace("send to unknown", addr, port, addr, port);
    return;
  }
  const Peer& p = g_peers[it->second];
  const uint32_t a = addr;
  const uint16_t ap = port;
  addr = p.route;
  port = uint16_t(port + Offset(p.base));
  Trace("send", a, ap, addr, port);
}

void ToGuest(uint32_t& addr, uint16_t& port) {
  std::lock_guard lock(g_mutex);
  for (const auto& [mac, p] : g_peers) {
    if (p.route != addr) continue;
    const uint16_t guest = uint16_t(port - Offset(p.base));
    // (two games on one PC: the one whose port base makes it one of the game's ports)
    if (g_bound.empty() || g_bound.count(guest)) {
      Trace("recv", addr, port, p.fake, guest);
      addr = p.fake;
      port = guest;
      return;
    }
  }
  if (port != 53 && addr != 0x7F000001) Trace("recv from unknown", addr, port, addr, port);
}

uint16_t BindPort(uint16_t port) {
  std::lock_guard lock(g_mutex);
  g_bound.insert(port);
  return uint16_t(port + Offset(g_me.base));
}

uint16_t GuestPort(uint16_t port) {
  std::lock_guard lock(g_mutex);
  const uint16_t guest = uint16_t(port - Offset(g_me.base));
  return g_bound.count(guest) ? guest : port;
}

uint32_t QosListen(const uint8_t* xnkid, const uint8_t* data, uint32_t size, uint32_t flags) {
  const uint64_t id = Be64(xnkid);
  std::lock_guard lock(g_mutex);
  if (flags & 0x10) {  // XNET_QOS_LISTEN_RELEASE
    g_qos.erase(id);
  } else if ((flags & 0x04) && data) {  // SET_DATA
    g_qos[id].assign(data, data + std::min<uint32_t>(size, 1024));
  } else {
    g_qos.try_emplace(id);
  }
  return 0;
}

std::vector<rex::kernel::xam::OnlineHooks::QosResult> QosLookup(
    const std::vector<rex::kernel::xam::OnlineHooks::QosTarget>& targets) {
  std::vector<rex::kernel::xam::OnlineHooks::QosResult> results(targets.size());
  for (size_t i = 0; i < targets.size(); ++i) {
    uint32_t route;
    uint16_t base;
    {
      std::lock_guard lock(g_mutex);
      Peer& p = Learn(targets[i].xnaddr.data());
      route = p.route, base = p.base;
    }
    const uint32_t nonce = NewNonce();
    auto request = Header(kQosRequest, nonce);
    request.insert(request.end(), targets[i].xnkid.begin(), targets[i].xnkid.end());
    const auto t0 = Clock::now();
    auto replies = Ask(request, nonce, {{route, base}}, std::chrono::milliseconds(800), 1);
    if (replies.empty()) continue;
    Reader r{replies[0].data(), replies[0].size()};
    r.u64();
    const uint16_t len = r.u16();
    auto& out = results[i];
    out.contacted = true;
    out.rtt_ms = uint16_t(std::max<int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count()));
    if (len && r.need(len)) out.data.assign(r.p + r.at, r.p + r.at + len);
  }
  return results;
}

// -- XGI session messages (app 0xFB) -------------------------------------------------

constexpr uint32_t kOk = 0, kInsufficientBuffer = 122;

Property ReadProperty(uint32_t address) {
  const uint8_t* b = Guest(address);
  Property p{Be32(b), b[8], {}};
  if (p.type == 4 || p.type == 6) {  // (string, binary)
    const uint32_t size = std::min<uint32_t>(Be32(b + 16), 4096), ptr = Be32(b + 20);
    if (size && ptr) p.value.assign(Guest(ptr), Guest(ptr) + size);
  } else {
    const size_t size = (p.type == 2 || p.type == 3 || p.type == 7) ? 8 : 4;
    p.value.assign(b + 16, b + 16 + size);
  }
  return p;
}

// XSESSION_INFO: XNKID, the host's XNADDR, its key (none here).
void WriteInfo(uint8_t* out, const Session& s) {
  std::memset(out, 0, 60);
  Put64(out, s.id);
  std::memcpy(out + 8, s.host.data(), 36);
}

// XSESSION_SEARCHRESULT_HEADER + results (92 bytes each) + their contexts and
// properties, in the caller's buffer.
uint32_t WriteResults(uint32_t address, uint32_t capacity, const std::vector<Session>& sessions) {
  size_t need = 8 + sessions.size() * 92;
  for (const auto& s : sessions) {
    need += s.contexts.size() * 8 + s.properties.size() * 24;
    for (const auto& p : s.properties) {
      if (p.type == 4 || p.type == 6) need += (p.value.size() + 3) & ~size_t(3);
    }
  }
  if (capacity < need) return kInsufficientBuffer;
  uint8_t* b = Guest(address);
  std::memset(b, 0, need);
  Put32(b, uint32_t(sessions.size()));
  Put32(b + 4, sessions.empty() ? 0 : address + 8);
  size_t tail = 8 + sessions.size() * 92;
  for (size_t i = 0; i < sessions.size(); ++i) {
    const Session& s = sessions[i];
    uint8_t* row = b + 8 + i * 92;
    WriteInfo(row, s);
    Put32(row + 60, s.public_slots - std::min(s.filled_public, s.public_slots));
    Put32(row + 64, s.private_slots - std::min(s.filled_private, s.private_slots));
    Put32(row + 68, s.filled_public), Put32(row + 72, s.filled_private);
    Put32(row + 76, uint32_t(s.properties.size())), Put32(row + 80, uint32_t(s.contexts.size()));
    if (!s.contexts.empty()) Put32(row + 88, uint32_t(address + tail));
    for (const auto& [id, value] : s.contexts) Put32(b + tail, id), Put32(b + tail + 4, value), tail += 8;
    if (!s.properties.empty()) Put32(row + 84, uint32_t(address + tail));
    size_t record = tail;
    tail += s.properties.size() * 24;
    for (const auto& p : s.properties) {
      Put32(b + record, p.id);
      b[record + 8] = p.type;
      if (p.type == 4 || p.type == 6) {
        Put32(b + record + 16, uint32_t(p.value.size()));
        Put32(b + record + 20, p.value.empty() ? 0 : uint32_t(address + tail));
        std::copy(p.value.begin(), p.value.end(), b + tail);
        tail += (p.value.size() + 3) & ~size_t(3);
      } else {
        std::copy(p.value.begin(), p.value.end(), b + record + 16);
      }
      record += 24;
    }
  }
  return kOk;
}

uint64_t NewSessionId() {
  std::random_device rd;
  return (uint64_t(rd()) << 32 | rd()) & 0x00FFFFFFFFFFFFFFull | 0xAE00000000000000ull;
}

std::optional<uint32_t> Xgi(uint32_t message, uint32_t buffer, uint32_t length) {
  if (!buffer) return std::nullopt;
  const uint8_t* b = Guest(buffer);
  auto u32 = [&](size_t at) { return Be32(b + at); };
  if (message != 0xB0006 && message != 0xB0007) {
    static std::mutex seen_mutex;
    static std::map<uint32_t, int> seen;
    std::lock_guard lock(seen_mutex);
    if (++seen[message] <= 20) {
      REXLOG_INFO("p2p: XGI {:05X} ({} bytes) {:08X} {:08X} {:08X} {:08X}", message, length, u32(0), u32(4),
                  length >= 12 ? u32(8) : 0, length >= 16 ? u32(12) : 0);
    }
  }
  switch (message) {
    case 0xB0006: {  // XUserSetContext: kept for the sessions this game hosts
      std::lock_guard lock(g_mutex);
      g_contexts[u32(16)] = u32(20);
      return std::nullopt;
    }
    case 0xB0007: {  // XUserSetProperty
      Property p{u32(16), uint8_t(u32(16) >> 28), {}};
      const uint32_t size = std::min<uint32_t>(u32(20), 4096), ptr = u32(24);
      if (size && ptr) p.value.assign(Guest(ptr), Guest(ptr) + size);
      std::lock_guard lock(g_mutex);
      g_properties[p.id] = std::move(p);
      return std::nullopt;
    }
    case 0xB0010: {  // XSessionCreate
      const uint32_t object = u32(0), flags = u32(4), info = u32(20), nonce = u32(24);
      Local local;
      local.host = (flags & 1) != 0;
      Session& s = local.session;
      s.flags = flags, s.public_slots = u32(8), s.private_slots = u32(12);
      if (local.host) {
        s.id = NewSessionId();
        s.nonce = NewSessionId();
        s.host = MyXnAddr();
        std::lock_guard lock(g_mutex);
        s.contexts = g_contexts;
        for (const auto& [id, p] : g_properties) s.properties.push_back(p);
        // X_PROPERTY_GAMER_HOSTNAME (LIVE filled it in: the host's name, UTF-16)
        Property name{0x40008109, 4, {}};
        for (char c : rex::cvar::Query<std::string>("online_name")) name.value.push_back(0), name.value.push_back(uint8_t(c));
        name.value.push_back(0), name.value.push_back(0);
        s.properties.push_back(std::move(name));
      } else if (info) {  // (joining: the session the game found)
        s.id = Be64(Guest(info));
        std::memcpy(s.host.data(), Guest(info) + 8, 36);
        Hello(s.host.data());
      }
      if (info && local.host) WriteInfo(Guest(info), s);
      if (nonce) Put64(Guest(nonce), s.nonce);
      REXLOG_INFO("p2p: session {:016X} {} (flags {:X}, {} + {} slots)", s.id, local.host ? "hosted" : "joined",
                  flags, s.public_slots, s.private_slots);
      std::lock_guard lock(g_mutex);
      g_sessions[object] = std::move(local);
      return kOk;
    }
    case 0xB0011: {  // XSessionDelete
      std::lock_guard lock(g_mutex);
      if (auto it = g_sessions.find(u32(0)); it != g_sessions.end()) {
        g_qos.erase(it->second.session.id);
        REXLOG_INFO("p2p: session {:016X} deleted", it->second.session.id);
        g_sessions.erase(it);
        return kOk;
      }
      return std::nullopt;
    }
    case 0xB0012:    // XSessionJoinLocal / Remote
    case 0xB0013: {  // XSessionLeaveLocal / Remote
      std::lock_guard lock(g_mutex);
      auto it = g_sessions.find(u32(0));
      if (it == g_sessions.end()) return std::nullopt;
      Local& local = it->second;
      const uint32_t count = std::min<uint32_t>(u32(4), 16), xuids = u32(8), indices = u32(12),
                     privates = message == 0xB0012 ? u32(16) : 0;
      for (uint32_t i = 0; i < count; ++i) {
        const uint64_t who = xuids ? Be64(Guest(xuids) + 8 * i) : (g_me.xuid ^ (indices ? Be32(Guest(indices) + 4 * i) : 0));
        const bool priv = privates && Be32(Guest(privates) + 4 * i);
        REXLOG_INFO("p2p: session {:016X} {} {} {:016X}{}", local.session.id, message == 0xB0012 ? "join" : "leave",
                    xuids ? "remote" : "local", who, priv ? " (private)" : "");
        if (message == 0xB0012) {
          (priv ? local.members_private : local.members_public).insert(who);
        } else {
          local.members_public.erase(who), local.members_private.erase(who);
        }
      }
      local.session.filled_public = uint32_t(local.members_public.size());
      local.session.filled_private = uint32_t(local.members_private.size());
      return kOk;
    }
    case 0xB0014:    // XSessionStart
    case 0xB0015: {  // XSessionEnd
      std::lock_guard lock(g_mutex);
      return g_sessions.count(u32(0)) ? std::optional<uint32_t>(kOk) : std::nullopt;
    }
    case 0xB0018: {  // XSessionModify
      std::lock_guard lock(g_mutex);
      auto it = g_sessions.find(u32(0));
      if (it == g_sessions.end()) return std::nullopt;
      it->second.session.flags = u32(4);
      it->second.session.public_slots = u32(8);
      it->second.session.private_slots = u32(12);
      return kOk;
    }
    case 0xB0016:    // XSessionSearch
    case 0xB001C: {  // XSessionSearchEx
      const uint32_t size = u32(24), results = u32(28);
      if (!results) return std::nullopt;
      const auto sessions = Search();
      std::vector<Session> open;
      for (const auto& s : sessions) {
        if (s.filled_public < s.public_slots) open.push_back(s);
      }
      if (open.size() > std::max<uint32_t>(1, u32(8))) open.resize(std::max<uint32_t>(1, u32(8)));
      REXLOG_INFO("p2p: search found {} session(s)", open.size());
      return WriteResults(results, size, open);
    }
    default:
      return std::nullopt;
  }
}

}  // namespace

void InstallP2P(rex::memory::Memory* memory) {
  if (!rex::cvar::Query<bool>("online_enabled") || !REXCVAR_GET(p2p_enabled)) return;
  g_memory = memory;
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  g_me.base = uint16_t(REXCVAR_GET(p2p_port));
  g_me.lan = LanIp();
  if (const std::string a = REXCVAR_GET(p2p_address); !a.empty()) {
    in_addr ina{};
    if (inet_pton(AF_INET, a.c_str(), &ina) == 1) g_me.lan = ntohl(ina.s_addr);
  }
  // the player's online id (and the port base: two games on one PC differ)
  const std::string id = rex::cvar::Query<std::string>("online_xuid") + ":" + std::to_string(g_me.base);
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : id) h = (h ^ c) * 1099511628211ull;
  g_me.mac = {0x02, uint8_t(h >> 32), uint8_t(h >> 24), uint8_t(h >> 16), uint8_t(h >> 8), uint8_t(h)};
  g_me.xuid = std::strtoull(rex::cvar::Query<std::string>("online_xuid").c_str(), nullptr, 16);

  g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  int yes = 1;
  setsockopt(g_socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
  sockaddr_in at = {};
  at.sin_family = AF_INET;
  at.sin_addr.s_addr = htonl(INADDR_ANY);
  at.sin_port = htons(g_me.base);
  if (g_socket == kBadSocket || bind(g_socket, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0) {
    REXLOG_ERROR("p2p: can't use UDP port {} (another program, or a second game: set p2p_port)", g_me.base);
    return;
  }
  std::thread(Receive).detach();

  rex::kernel::xam::OnlineHooks hooks;
  hooks.title_xnaddr = TitleXnAddr;
  hooks.xnaddr_to_inaddr = XnAddrToInAddr;
  hooks.inaddr_to_xnaddr = InAddrToXnAddr;
  hooks.qos_listen = QosListen;
  hooks.qos_lookup = QosLookup;
  hooks.to_host = ToHost;
  hooks.to_guest = ToGuest;
  hooks.bind_port = BindPort;
  hooks.guest_port = GuestPort;
  hooks.xgi = Xgi;
  rex::kernel::xam::SetOnlineHooks(std::move(hooks));
  REXLOG_INFO("p2p: on, {} port {} (id {:02X}{:02X}{:02X}{:02X}{:02X}{:02X})", Ip(g_me.lan), g_me.base, g_me.mac[0],
              g_me.mac[1], g_me.mac[2], g_me.mac[3], g_me.mac[4], g_me.mac[5]);
}

}  // namespace svr2011

