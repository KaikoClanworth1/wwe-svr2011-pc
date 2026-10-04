// WWE SmackDown vs. Raw 2011 - peer-to-peer online matches (see p2p.h).
//
// The session message decoding (XGI B0006-B001C buffers, search results in
// the caller's memory) follows PR #3, "Add groundwork for P2P online
// sessions", by gitSothib - its session directory is a protocol between the
// games here instead of a web service.
#include "p2p.h"
#include "leaderboards.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
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
#include <cerrno>
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

#include "frame_rate.h"
#include "online_net.h"

REXCVAR_DEFINE_BOOL(p2p_enabled, true, "Online", "Peer-to-peer online matches (with online_enabled)");
REXCVAR_DEFINE_UINT32(p2p_port, 36000, "Online",
                      "This game's port base for online matches (UDP; a second game on the same PC: 36100)");
REXCVAR_DEFINE_STRING(p2p_address, "", "Online",
                      "The LAN address this game gives peers (tests: a second game on this PC, e.g. 127.0.0.2)");
REXCVAR_DEFINE_STRING(p2p_peers, "", "Online",
                      "Addresses searched besides the LAN, comma-separated (host or host:port)");
REXCVAR_DEFINE_BOOL(p2p_relay, true, "Online",
                    "Matches through the online server's relay when players can't connect directly");
REXCVAR_DEFINE_STRING(p2p_stun, "stun.l.google.com:19302", "Online",
                      "The STUN server that tells this game its public address (off: none)");
REXCVAR_DEFINE_BOOL(p2p_force_relay, false, "Online", "Tests: every peer through the relay, never directly");
REXCVAR_DEFINE_BOOL(p2p_lockstep, true, "Online",
                    "Matches step the world once a frame on both games (as on the console); off: tests only");

namespace svr2011 {

namespace {

using Clock = std::chrono::steady_clock;
constexpr uint16_t kBasePort = 36000;            // (port bases are kBasePort + n * 100)
constexpr int kInstances = 4;                    // (searched on the LAN and this PC)
constexpr uint32_t kFakeNet = 0x0A400000;        // 10.64.0.0/16: peers as the game sees them
constexpr uint32_t kMagic = 0x53565250;          // "SVRP"
constexpr uint8_t kVersion = 1;
enum : uint8_t {
  kSearch = 1, kSessions = 2, kQosRequest = 3, kQosReply = 4, kHello = 5, kData = 6,
  kFileRequest = 7, kFilePart = 8  // (files peers fetch from each other: P2PFetch)
};

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
bool Reset() { return WSAGetLastError() == WSAECONNRESET; }  // (an ICMP "port unreachable" from an earlier send)
#else
using Socket = int;
const Socket kBadSocket = -1;
void CloseSocket(Socket s) { close(s); }
bool Reset() { return errno == ECONNREFUSED; }
#endif

Socket g_socket = kBadSocket;  // the P2P socket (this game's port base): everything but LAN game traffic

sockaddr_in Sin(uint32_t ip, uint16_t port) {
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = htonl(ip);
  to.sin_port = htons(port);
  return to;
}

void SendOn(Socket s, uint32_t ip, uint16_t port, const uint8_t* data, size_t size) {
  sockaddr_in to = Sin(ip, port);
  sendto(s, reinterpret_cast<const char*>(data), int(size), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}
void SendTo(uint32_t ip, uint16_t port, const std::vector<uint8_t>& data) {
  SendOn(g_socket, ip, port, data.data(), data.size());
}

// host or host:port -> address (0: unknown)
std::pair<uint32_t, uint16_t> Resolve(std::string item, uint16_t port) {
  item.erase(0, item.find_first_not_of(" \t"));
  item.erase(item.find_last_not_of(" \t") + 1);
  if (const size_t colon = item.rfind(':'); colon != std::string::npos) {
    port = uint16_t(std::atoi(item.c_str() + colon + 1));
    item.resize(colon);
  }
  if (item.empty()) return {0, 0};
  addrinfo hints = {}, *found = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  uint32_t ip = 0;
  if (getaddrinfo(item.c_str(), nullptr, &hints, &found) == 0 && found) {
    ip = ntohl(reinterpret_cast<sockaddr_in*>(found->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(found);
  }
  return {ip, port};
}

// This PC's address on its network (the interface the default route uses).
uint32_t LanIp() {
  Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kBadSocket) return 0x7F000001;
  sockaddr_in to = Sin(0x08080808, 53);  // (no packet is sent)
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
  uint32_t lan = 0x7F000001;
  std::atomic<uint32_t> online{0};       // the public address (STUN, or as the relay sees it)
  std::atomic<uint16_t> online_port{0};  // (the P2P socket's, outside the NAT)
  uint16_t base = kBasePort;
  Mac mac{};
  uint64_t xuid = 0;
} g_me;

// A game port's real port: + 20000 (the game's own, UDP 1001, is below 1024,
// which Android and Linux don't let apps bind) + the port base's offset (two
// games on one PC differ).
constexpr uint16_t kPortShift = 20000;
uint16_t Offset(uint16_t base) { return uint16_t(base - kBasePort + kPortShift); }

// abOnline: the player's xuid, then the P2P socket's public port.
XnAddr MyXnAddr() {
  XnAddr a{};
  const uint32_t online = g_me.online;
  Put32(a.data(), g_me.lan);
  Put32(a.data() + 4, online ? online : g_me.lan);
  Put16(a.data() + 8, g_me.base);
  std::memcpy(a.data() + 10, g_me.mac.data(), 6);
  Put64(a.data() + 16, g_me.xuid);
  Put16(a.data() + 24, g_me.online_port);
  return a;
}

// The online server's match relay (port/server/relay.py): two long requests
// (a pipe), so it works wherever the server's https does.
std::mutex g_pipe_mutex;
std::shared_ptr<net::Pipe> g_pipe;
std::atomic<bool> g_relay_up{false};  // (it answered)
std::atomic<int64_t> g_online_at{0};  // last session activity (steady clock, ms): the relay is wanted

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}
bool ForceRelay() { return REXCVAR_GET(p2p_force_relay); }

struct Peer {
  XnAddr xnaddr{};
  uint32_t fake = 0;        // what the game knows it as
  uint32_t route = 0;       // where its packets go: the address it was last heard from, or its own
  uint16_t route_port = 0;  // (and port: outside a NAT, not its port base)
  uint16_t base = kBasePort;
  bool relayed = false;     // its packets go through the relay
  bool tunnel = false;      // game traffic goes through the P2P socket (not on this network)
  Clock::time_point direct{};          // last heard from directly
  Clock::time_point punched{};         // last direct try while relayed
  uint32_t seen_ip = 0;                // its address as the relay sees it
  uint16_t seen_port = 0;
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

// The way to a peer not heard from yet: this PC, its LAN address (same network) or its public one.
uint32_t RouteTo(const uint8_t* xnaddr) {
  const uint32_t lan = Be32(xnaddr), online = Be32(xnaddr + 4);
  if (lan == g_me.lan) return lan;  // (a second game on this PC: the game binds its own address, not loopback)
  const uint32_t mine = g_me.online;
  if (!online || online == lan || online == (mine ? mine : g_me.lan)) return lan;
  return online;
}

// How a packet got here.
struct Via {
  uint32_t ip = 0;  // its sender as seen (relayed: as the relay saw it); 0: not heard from
  uint16_t port = 0;
  bool relayed = false;
};

// The peer at xnaddr (added if new), and how it was just heard from.
Peer& Learn(const uint8_t* xnaddr, Via via = {}) {
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
    p.route = RouteTo(xnaddr), p.route_port = p.base;
  }
  const auto now = Clock::now();
  if (via.relayed) {
    p.seen_ip = via.ip, p.seen_port = via.port;
    // (direct while that works: the relay only when nothing came directly lately)
    if (!p.relayed && now - p.direct > std::chrono::seconds(10)) {
      p.relayed = p.tunnel = true;
      REXLOG_INFO("p2p: peer {} through the relay (seen at {}:{})", Ip(p.fake), Ip(via.ip), via.port);
    }
  } else if (via.ip) {
    // (heard from: its address as this PC sees it; loopback: this PC, by its own address)
    const uint32_t from = via.ip == 0x7F000001 ? Be32(xnaddr) : via.ip;
    p.route = from, p.route_port = via.port, p.direct = now;
    if (from != Be32(xnaddr) && from != g_me.lan && !p.tunnel) {
      p.tunnel = true;  // (through a NAT: its game ports are not reachable, only this one)
      REXLOG_INFO("p2p: peer {} is outside this network ({}:{})", Ip(p.fake), Ip(from), via.port);
    }
    if (p.relayed) {
      p.relayed = false;
      REXLOG_INFO("p2p: peer {} direct at {}:{}", Ip(p.fake), Ip(from), via.port);
    }
  }
  if (ForceRelay()) p.relayed = p.tunnel = true;
  if (added) {
    REXLOG_INFO("p2p: peer {:02X}{:02X}{:02X}{:02X}{:02X}{:02X} at {}:{}{} is {}", mac[0], mac[1], mac[2], mac[3],
                mac[4], mac[5], Ip(p.route), p.route_port, p.relayed ? " (relay)" : "", Ip(p.fake));
  }
  return p;
}

// Where a peer's packets go (taken under g_mutex, used without it).
struct Path {
  Mac mac{};
  bool relayed = false;
  uint32_t ip = 0;
  uint16_t port = 0;
};
Path PathOf(const Mac& mac, const Peer& p) { return {mac, p.relayed, p.route, p.route_port ? p.route_port : p.base}; }

// "SVRR" relay packets (port/server/relay.py), each in a frame: a 16-bit length, then the packet.
constexpr uint32_t kRelayMagic = 0x53565252;
enum : uint8_t { kRegister = 1, kWelcome = 2, kForward = 3, kFrom = 4 };

void ToPipe(uint8_t type, const uint8_t* id, const uint8_t* data, size_t size) {
  std::shared_ptr<net::Pipe> pipe;
  {
    std::lock_guard lock(g_pipe_mutex);
    pipe = g_pipe;
  }
  if (!pipe || size > 60000) return;
  std::vector<uint8_t> out(2 + 12 + size);
  Put16(out.data(), uint16_t(12 + size));
  Put32(out.data() + 2, kRelayMagic), out[6] = 1, out[7] = type;
  std::memcpy(out.data() + 8, id, 6);
  if (size) std::memcpy(out.data() + 14, data, size);
  pipe->Send(out.data(), out.size());
}

void ToRelay(const Mac& to, const uint8_t* data, size_t size) {
  if (g_relay_up) ToPipe(kForward, to.data(), data, size);
}

void Send(const Path& path, const uint8_t* data, size_t size) {
  if (path.relayed) {
    ToRelay(path.mac, data, size);
  } else {
    SendOn(g_socket, path.ip, path.port, data, size);
  }
}
void Send(const Path& path, const std::vector<uint8_t>& data) { Send(path, data.data(), data.size()); }

// -- tunnelled game traffic -------------------------------------------------------------
//
// A peer outside this network is reached only through its P2P socket (the one
// port its NAT, port forward or the relay knows). Its game ports get a proxy
// socket each on 127.0.0.1 here: the game sends to the proxy, the proxy's
// packets go to the peer in kData messages, and the peer's come back out of
// the proxy to the game's own socket - from the address the game knows the
// peer's port as.

struct Proxy {
  Socket s = kBadSocket;
  Mac mac{};
  uint16_t remote = 0;  // the peer's game port (as its game knows it)
  uint16_t local = 0;   // the proxy's port
};
std::map<std::pair<Mac, uint16_t>, Proxy*> g_proxies;
std::map<uint16_t, Proxy*> g_proxy_ports;

std::vector<uint8_t> DataHeader(uint16_t to, uint16_t from) {
  Writer w;
  w.u32(kMagic), w.u8(kVersion), w.u8(kData);
  w.bytes(g_me.mac.data(), 6);
  w.u16(to), w.u16(from);
  return w.b;
}

void ProxyLoop(Proxy* proxy) {
  std::vector<uint8_t> buf(65536);
  for (;;) {
    sockaddr_in from = {};
    socklen_t len = sizeof(from);
    const int n = int(recvfrom(proxy->s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                               reinterpret_cast<sockaddr*>(&from), &len));
    if (n <= 0) {
      if (!Reset()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    // (from the game's own socket: its port as the game knows it)
    auto packet = DataHeader(proxy->remote, uint16_t(ntohs(from.sin_port) - Offset(g_me.base)));
    packet.insert(packet.end(), buf.begin(), buf.begin() + n);
    Path path;
    {
      std::lock_guard lock(g_mutex);
      auto it = g_peers.find(proxy->mac);
      if (it == g_peers.end()) continue;
      path = PathOf(proxy->mac, it->second);
    }
    Send(path, packet);
  }
}

// The proxy for the peer's port `remote` (under g_mutex).
Proxy* ProxyFor(const Mac& mac, uint16_t remote) {
  if (auto it = g_proxies.find({mac, remote}); it != g_proxies.end()) return it->second;
  if (g_proxies.size() >= 64) return nullptr;
  auto* proxy = new Proxy{socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP), mac, remote, 0};
  sockaddr_in at = Sin(0x7F000001, 0);
  socklen_t len = sizeof(at);
  if (proxy->s == kBadSocket || bind(proxy->s, reinterpret_cast<sockaddr*>(&at), sizeof(at)) != 0 ||
      getsockname(proxy->s, reinterpret_cast<sockaddr*>(&at), &len) != 0) {
    REXLOG_WARN("p2p: no proxy socket for a peer");
    if (proxy->s != kBadSocket) CloseSocket(proxy->s);
    delete proxy;
    return nullptr;
  }
  proxy->local = ntohs(at.sin_port);
  g_proxies[{mac, remote}] = proxy;
  g_proxy_ports[proxy->local] = proxy;
  std::thread(ProxyLoop, proxy).detach();  // (proxies live as long as the game)
  return proxy;
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
// An invite accepted (online_overlay.cpp): its session comes first in searches,
// its private slots open to this game (as the console's invites let in).
uint64_t g_invited = 0;
Clock::time_point g_invited_at{};

// A match is on while a peer-network session is (XSESSION_CREATE_USES_PEER_NETWORK;
// not the ONLINE menu's presence session): then the world steps exactly once a
// frame on both peers, as on the console, or locally simulated things (the
// referee...) drift apart (under g_mutex).
void UpdateLockstep() {
  static bool on = false;
  bool match = false;
  for (const auto& [obj, local] : g_sessions) match |= (local.session.flags & 0x20) != 0;
  match &= REXCVAR_GET(p2p_lockstep);
  if (match == on) return;
  on = match;
  svr2011::SetLockstep(on);
  REXLOG_INFO("p2p: lockstep {}", on ? "on (a match session)" : "off");
}

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

uint32_t NewNonce() {
  static std::atomic<uint32_t> next{uint32_t(std::random_device{}())};
  return ++next;
}

// A relayed peer: try its own address too now and then (both NATs letting
// the other in: direct from then on).
void Punch(const Mac& mac) {
  std::vector<std::pair<uint32_t, uint16_t>> to;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_peers.find(mac);
    if (it == g_peers.end() || !it->second.relayed || ForceRelay()) return;
    Peer& p = it->second;
    const auto now = Clock::now();
    if (now - p.punched < std::chrono::seconds(2)) return;
    p.punched = now;
    if (p.seen_ip && p.seen_port) to.emplace_back(p.seen_ip, p.seen_port);
    const uint32_t online = Be32(p.xnaddr.data() + 4);
    if (const uint16_t port = Be16(p.xnaddr.data() + 24); online && port && port != p.seen_port) to.emplace_back(online, port);
    if (online) to.emplace_back(online, p.base);
  }
  const auto hello = Header(kHello, NewNonce() | 1);
  for (const auto& [ip, port] : to) SendTo(ip, port, hello);
}

// A peer's game packet: to this game's port `to`, from its port `from`.
void OnData(const uint8_t* data, size_t size, Via via) {
  if (size < 16) return;
  Mac mac;
  std::memcpy(mac.data(), data + 6, 6);
  const uint16_t to = Be16(data + 12), from = Be16(data + 14);
  Socket s;
  {
    std::lock_guard lock(g_mutex);
    auto it = g_peers.find(mac);
    if (it == g_peers.end()) return;
    Peer& p = it->second;
    if (!via.relayed && via.ip) {  // (a NAT may move it: follow)
      if (via.ip != p.route || via.port != p.route_port) {
        const XnAddr xnaddr = p.xnaddr;
        Learn(xnaddr.data(), via);
      }
      p.direct = Clock::now();
    }
    Proxy* proxy = ProxyFor(mac, from);
    if (!proxy) return;
    s = proxy->s;
  }
  SendOn(s, 0x7F000001, uint16_t(to + Offset(g_me.base)), data + 16, size - 16);
  if (via.relayed) Punch(mac);
}

// -- files peers fetch from each other (P2PFetch: online_cas.cpp) -------------------
//
// A request names a file (kind + key) and a byte range; the peers that have it
// answer with parts of up to kChunk bytes (total 0xFFFFFFFF: "I haven't"). The
// asker re-requests what didn't arrive from the peer that answered.

constexpr uint32_t kChunk = 1024, kNone = 0xFFFFFFFF;
P2PFileSource g_file_source = nullptr;

struct FileFetch {
  uint8_t kind = 0;
  std::string key;
  std::string data;
  std::vector<bool> have;
  uint32_t total = 0, got = 0;
  int refusals = 0;
  bool known = false;
  Path source;
};
std::map<uint32_t, FileFetch*> g_fetches;  // by request nonce (under g_wait_mutex)

std::vector<uint8_t> FileRequest(uint32_t nonce, uint8_t kind, const std::string& key, uint32_t offset,
                                 uint32_t length) {
  Writer w;
  w.b = Header(kFileRequest, nonce);
  w.u8(kind), w.u8(uint8_t(key.size()));
  w.bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
  w.u32(offset), w.u32(length);
  return w.b;
}

// A peer asks for a file this game has: the parts of the range (or "I haven't").
void ServeFile(Reader& r, uint32_t nonce, const Path& back) {
  const uint8_t kind = r.u8(), len = r.u8();
  if (!r.need(len)) return;
  const std::string key(reinterpret_cast<const char*>(r.p + r.at), len);
  r.at += len;
  const uint32_t offset = r.u32(), length = r.u32();
  if (!r.ok || !g_file_source) return;
  // (the last few files asked for, so a transfer reads each once)
  static std::mutex cache_mutex;
  static std::vector<std::pair<std::string, std::shared_ptr<std::optional<std::string>>>> cache;
  std::shared_ptr<std::optional<std::string>> file;
  {
    std::lock_guard lock(cache_mutex);
    const std::string id = std::to_string(kind) + ":" + key;
    for (auto& [k, v] : cache) {
      if (k == id) file = v;
    }
    if (!file) {
      file = std::make_shared<std::optional<std::string>>(g_file_source(kind, key));
      cache.emplace_back(id, file);
      if (cache.size() > 8) cache.erase(cache.begin());
    }
  }
  auto part = [&](uint32_t total, uint32_t at, const char* bytes, uint16_t n) {
    Writer w;
    w.b = Header(kFilePart, nonce);
    w.u8(kind), w.u8(uint8_t(key.size()));
    w.bytes(reinterpret_cast<const uint8_t*>(key.data()), key.size());
    w.u32(total), w.u32(at), w.u16(n);
    w.bytes(reinterpret_cast<const uint8_t*>(bytes), n);
    Send(back, w.b);
  };
  if (!*file) return part(kNone, 0, nullptr, 0);
  const std::string& bytes = **file;
  const uint32_t total = uint32_t(bytes.size());
  const uint32_t end = uint32_t(std::min<uint64_t>(total, uint64_t(offset) + std::min<uint32_t>(length, 1u << 20)));
  if (total == 0) return part(0, 0, nullptr, 0);
  for (uint32_t at = offset; at < end; at += kChunk) {
    part(total, at, bytes.data() + at, uint16_t(std::min<uint32_t>(kChunk, end - at)));
  }
}

void OnFilePart(Reader& r, uint32_t nonce, const Path& back) {
  r.u8();
  const uint8_t len = r.u8();
  if (!r.need(len)) return;
  r.at += len;
  const uint32_t total = r.u32(), at = r.u32();
  const uint16_t n = r.u16();
  if (!r.ok || !r.need(n)) return;
  std::lock_guard lock(g_wait_mutex);
  auto it = g_fetches.find(nonce);
  if (it == g_fetches.end()) return;
  FileFetch& f = *it->second;
  if (total == kNone) {
    ++f.refusals;
  } else if (!f.known) {
    if (total > (64u << 20)) return;
    f.known = true, f.total = total, f.source = back;
    f.data.assign(total, '\0');
    f.have.assign((total + kChunk - 1) / kChunk, false);
  }
  if (total != kNone && f.known && total == f.total && at < total && at % kChunk == 0 && !f.have[at / kChunk]) {
    std::memcpy(f.data.data() + at, r.p + r.at, std::min<uint32_t>(n, total - at));
    f.have[at / kChunk] = true;
    ++f.got;
  }
  g_wait.notify_all();
}

void OnPacket(const uint8_t* data, size_t size, Via via) {
  Reader r{data, size};
  if (r.u32() != kMagic || r.u8() != kVersion) return;
  const uint8_t type = r.u8();
  if (type == kData) return OnData(data, size, via);
  const uint32_t nonce = r.u32();
  XnAddr sender{};
  r.bytes(sender.data(), 36);
  const Mac mac = MacOf(sender.data());
  if (!r.ok || mac == g_me.mac) return;  // (our own broadcast)
  if (ForceRelay() && !via.relayed) return;
  Path back;
  {
    std::lock_guard lock(g_mutex);
    back = PathOf(mac, Learn(sender.data(), via));
  }
  // (replies go back the way the request came)
  back.relayed = via.relayed;
  if (!via.relayed) back.ip = via.ip, back.port = via.port;
  if (via.relayed) Punch(mac);
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
      if (mine.empty() && via.relayed) break;  // (a search through the relay reaches every game)
      w.u16(uint16_t(mine.size()));
      for (const auto& s : mine) WriteSession(w, s);
      Send(back, w.b);
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
      Send(back, w.b);
      break;
    }
    case kHello:
      if (nonce & 1) Send(back, Header(kHello, nonce + 1));  // (the first of a pair: answered)
      break;
    case kFileRequest:
      ServeFile(r, nonce, back);
      break;
    case kFilePart:
      OnFilePart(r, nonce, back);
      break;
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

// STUN (RFC 5389) binding requests and answers: this socket's public address.
constexpr uint32_t kStunCookie = 0x2112A442;

bool OnStun(const uint8_t* data, size_t size) {
  if (size < 20 || Be16(data) != 0x0101 || Be32(data + 4) != kStunCookie) return false;
  for (size_t at = 20; at + 4 <= size;) {
    const uint16_t type = Be16(data + at), len = Be16(data + at + 2);
    if (at + 4 + len > size) break;
    if ((type == 0x0020 || type == 0x0001) && len >= 8 && data[at + 5] == 1) {  // (XOR-)MAPPED-ADDRESS, IPv4
      uint16_t port = Be16(data + at + 6);
      uint32_t ip = Be32(data + at + 8);
      if (type == 0x0020) port ^= uint16_t(kStunCookie >> 16), ip ^= kStunCookie;
      if (g_me.online != ip || g_me.online_port != port) {
        REXLOG_INFO("p2p: public address {}:{} (STUN)", Ip(ip), port);
      }
      g_me.online = ip, g_me.online_port = port;
      break;
    }
    at += 4 + ((len + 3) & ~3u);
  }
  return true;
}

void OnRelay(const uint8_t* data, size_t size) {
  if (size < 12 || data[4] != 1) return;
  const uint32_t ip = Be32(data + 6);
  const uint16_t port = Be16(data + 10);
  if (data[5] == kWelcome) {
    if (!g_relay_up.exchange(true)) REXLOG_INFO("p2p: relay up (this game seen from {})", Ip(ip));
    if (!g_me.online && ip) g_me.online = ip;  // (no STUN answer)
  } else if (data[5] == kFrom) {
    OnPacket(data + 12, size - 12, {ip, port, true});
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
      if (!Reset()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    const uint32_t ip = ntohl(from.sin_addr.s_addr);
    const uint16_t port = ntohs(from.sin_port);
    if (!OnStun(buf.data(), size_t(n))) {
      OnPacket(buf.data(), size_t(n), {ip, port, false});
    }
  }
}

// The relay's frames as they come (the pipe's thread).
void OnPipeData(const uint8_t* data, size_t size) {
  static std::vector<uint8_t> buf;  // (one pipe at a time)
  if (!data) {
    buf.clear();
    if (g_relay_up.exchange(false)) REXLOG_INFO("p2p: relay down");
    return;
  }
  buf.insert(buf.end(), data, data + size);
  size_t at = 0;
  while (buf.size() - at >= 2 && buf.size() - at >= 2u + Be16(buf.data() + at)) {
    const uint16_t len = Be16(buf.data() + at);
    if (len >= 6 && Be32(buf.data() + at + 2) == kRelayMagic) OnRelay(buf.data() + at + 2, len);
    at += 2u + len;
  }
  buf.erase(buf.begin(), buf.begin() + ptrdiff_t(at));
}

// STUN now and then; the relay's pipe (signed in) while the player plays
// online - a session open, or session activity in the last 5 minutes (the
// ONLINE menu's presence session comes and goes) - registered every 15 s
// (which also keeps its NAT's mapping open). Each pipe is two of the server's
// connections: games outside online play don't hold any.
void Keepalive() {
  using namespace std::chrono_literals;
  const bool relay = REXCVAR_GET(p2p_relay) && !rex::cvar::Query<std::string>("online_token").empty();
  const std::string stun = REXCVAR_GET(p2p_stun);
  char id[13];
  std::snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x", g_me.mac[0], g_me.mac[1], g_me.mac[2], g_me.mac[3],
                g_me.mac[4], g_me.mac[5]);
  Clock::time_point tried{}, registered{}, stunned{};
  for (;;) {
    const auto now = Clock::now();
    {
      std::lock_guard lock(g_mutex);
      if (!g_sessions.empty()) g_online_at = NowMs();
    }
    const int64_t at = g_online_at;
    const bool wanted = relay && at && NowMs() - at < 300000;
    std::shared_ptr<net::Pipe> pipe;
    {
      std::lock_guard lock(g_pipe_mutex);
      pipe = g_pipe;
    }
    const bool open = pipe && pipe->Open();
    if (wanted && !open && (tried == Clock::time_point{} || now - tried >= 30s)) {  // (again every 30 s while down)
      tried = now;
      auto opened = net::OpenPipe(std::string("/api/relay/") + id, OnPipeData);
      std::lock_guard lock(g_pipe_mutex);
      g_pipe = std::move(opened);
      registered = {};
    } else if (!wanted && pipe) {
      pipe->Close();
      std::lock_guard lock(g_pipe_mutex);
      g_pipe.reset();
      tried = {};
      REXLOG_INFO("p2p: relay closed (not online)");
    }
    if (!stun.empty() && stun != "off" && (stunned == Clock::time_point{} || now - stunned >= 300s)) {
      stunned = now;  // (every 5 minutes: a NAT may move it)
      const auto [ip, port] = Resolve(stun, 3478);
      if (ip) {
        uint8_t req[20] = {0, 1, 0, 0};
        Put32(req + 4, kStunCookie);
        for (int i = 8; i < 20; ++i) req[i] = uint8_t(NewNonce());
        SendOn(g_socket, ip, port, req, sizeof(req));
      }
    }
    if (wanted && (registered == Clock::time_point{} || now - registered >= 15s)) {
      registered = now;
      ToPipe(kRegister, g_me.mac.data(), nullptr, 0);
    }
    std::this_thread::sleep_for(1s);
  }
}

// Sends `request` (`send`) and collects the answers for `wait`.
std::vector<std::vector<uint8_t>> Ask(uint32_t nonce, const std::function<void()>& send,
                                      std::chrono::milliseconds wait, size_t enough = SIZE_MAX) {
  {
    std::lock_guard lock(g_wait_mutex);
    g_replies[nonce];
  }
  send();
  std::unique_lock lock(g_wait_mutex);
  g_wait.wait_for(lock, wait, [&] { return g_replies[nonce].size() >= enough; });
  auto replies = std::move(g_replies[nonce]);
  g_replies.erase(nonce);
  return replies;
}

// The places a search asks: the LAN, this PC, p2p_peers (ports: the bases)
// and everyone on the relay.
std::vector<std::pair<uint32_t, uint16_t>> SearchTargets() {
  std::vector<std::pair<uint32_t, uint16_t>> targets;
  if (ForceRelay()) return targets;
  for (int i = 0; i < kInstances; ++i) {
    const uint16_t port = uint16_t(kBasePort + 100 * i);
    targets.emplace_back(0xFFFFFFFF, port);
    targets.emplace_back(0x7F000001, port);
  }
  std::stringstream list(REXCVAR_GET(p2p_peers));
  for (std::string item; std::getline(list, item, ',');) {
    if (const auto target = Resolve(item, kBasePort); target.first) targets.push_back(target);
  }
  return targets;
}

std::vector<Session> Search() {
  g_online_at = NowMs();
  if (REXCVAR_GET(p2p_relay) && !rex::cvar::Query<std::string>("online_token").empty()) {
    for (int i = 0; i < 30 && !g_relay_up; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const uint32_t nonce = NewNonce();
  const auto request = Header(kSearch, nonce);
  const auto targets = SearchTargets();
  auto replies = Ask(nonce, [&] {
    for (const auto& [ip, port] : targets) SendTo(ip, port, request);
    if (g_relay_up) ToRelay(Mac{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}, request.data(), request.size());
  }, std::chrono::milliseconds(g_relay_up ? 1200 : 700));
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
  Path path;
  {
    std::lock_guard lock(g_mutex);
    path = PathOf(MacOf(host_xnaddr), Learn(host_xnaddr));
  }
  const uint32_t nonce = NewNonce() | 1;
  for (int i = 0; i < 3; ++i) Send(path, Header(kHello, nonce));
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
  if (p.tunnel) {
    if (Proxy* proxy = ProxyFor(it->second, port)) addr = 0x7F000001, port = proxy->local;
    Trace(p.relayed ? "send (tunnel, relay)" : "send (tunnel)", a, ap, Be32(p.xnaddr.data()), ap);
    return;
  }
  addr = p.route;
  port = uint16_t(port + Offset(p.base));
  Trace("send", a, ap, addr, port);
}

void ToGuest(uint32_t& addr, uint16_t& port) {
  std::lock_guard lock(g_mutex);
  if (addr == 0x7F000001) {
    if (auto it = g_proxy_ports.find(port); it != g_proxy_ports.end()) {  // (a tunnelled peer)
      const Proxy* proxy = it->second;
      addr = g_peers[proxy->mac].fake;
      port = proxy->remote;
      return;
    }
  }
  for (const auto& [mac, p] : g_peers) {
    if (p.route != addr || p.tunnel) continue;
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
    Path path;
    {
      std::lock_guard lock(g_mutex);
      path = PathOf(MacOf(targets[i].xnaddr.data()), Learn(targets[i].xnaddr.data()));
    }
    const uint32_t nonce = NewNonce();
    auto request = Header(kQosRequest, nonce);
    request.insert(request.end(), targets[i].xnkid.begin(), targets[i].xnkid.end());
    const auto t0 = Clock::now();
    auto replies = Ask(nonce, [&] { Send(path, request); }, std::chrono::milliseconds(path.relayed ? 1500 : 800), 1);
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
  if (message >= 0xB0010 && message <= 0xB001C) g_online_at = NowMs();  // (session messages: playing online)
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
      UpdateLockstep();
      return kOk;
    }
    case 0xB0011: {  // XSessionDelete
      std::lock_guard lock(g_mutex);
      if (auto it = g_sessions.find(u32(0)); it != g_sessions.end()) {
        g_qos.erase(it->second.session.id);
        REXLOG_INFO("p2p: session {:016X} deleted", it->second.session.id);
        g_sessions.erase(it);
        UpdateLockstep();
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
      UpdateLockstep();
      return kOk;
    }
    case 0xB0016:    // XSessionSearch
    case 0xB001C: {  // XSessionSearchEx
      const uint32_t size = u32(24), results = u32(28);
      if (!results) return std::nullopt;
      const auto sessions = Search();
      std::vector<Session> open;
      uint64_t invited = 0;
      {
        std::lock_guard lock(g_mutex);
        if (g_invited && Clock::now() - g_invited_at < std::chrono::minutes(15)) invited = g_invited;
      }
      for (const auto& s : sessions) {
        if (s.id == invited) {
          Session mine = s;  // (its private slots are this game's to take)
          mine.public_slots += mine.private_slots, mine.filled_public += mine.filled_private;
          mine.private_slots = mine.filled_private = 0;
          if (mine.filled_public < mine.public_slots) open.insert(open.begin(), mine);
          REXLOG_INFO("p2p: search: the invited session {:016X}{}", s.id, mine.filled_public < mine.public_slots ? "" : " is full");
        } else if (s.filled_public < s.public_slots) {
          open.push_back(s);
        }
      }
      if (open.size() > std::max<uint32_t>(1, u32(8))) open.resize(std::max<uint32_t>(1, u32(8)));
      REXLOG_INFO("p2p: search found {} session(s)", open.size());
      return WriteResults(results, size, open);
    }
    default:
      return LeaderboardsXgi(message, buffer, length);  // (the stats: leaderboards.h)
  }
}

}  // namespace

void SetP2PFileSource(P2PFileSource source) { g_file_source = source; }

bool P2PSessionInfo(uint8_t* out, uint32_t* slots) {
  std::lock_guard lock(g_mutex);
  const Local* best = nullptr;
  for (const auto& [obj, local] : g_sessions) {
    if (!(local.session.flags & 0x20)) continue;  // (a match's, not the ONLINE menu's presence session)
    if (!best || (local.host && !best->host)) best = &local;
  }
  if (!best) return false;
  WriteInfo(out, best->session);
  if (slots) *slots = best->session.public_slots + best->session.private_slots;
  return true;
}

void P2PExpectInvite(const uint8_t* info) {
  {
    std::lock_guard lock(g_mutex);
    g_invited = Be64(info);
    g_invited_at = Clock::now();
  }
  REXLOG_INFO("p2p: invited to session {:016X}: first in searches", Be64(info));
}

bool P2PReachHost(const uint8_t* info) {
  const uint8_t* host = info + 8;
  const Mac mac = MacOf(host);
  Path path;
  {
    std::lock_guard lock(g_mutex);
    path = PathOf(mac, Learn(host));
  }
  g_online_at = NowMs();  // (the relay is wanted now)
  if (REXCVAR_GET(p2p_relay) && !rex::cvar::Query<std::string>("online_token").empty()) {
    for (int i = 0; i < 30 && !g_relay_up; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const uint32_t nonce = NewNonce();
  const auto request = Header(kSearch, nonce);
  auto replies = Ask(nonce, [&] {
    if (!path.relayed && !ForceRelay()) Send(path, request);
    ToRelay(mac, request.data(), request.size());
  }, std::chrono::milliseconds(2500), 1);
  bool found = false;
  const uint64_t id = Be64(info);
  for (const auto& reply : replies) {
    Reader r{reply.data(), reply.size()};
    const uint16_t count = r.u16();
    for (uint16_t i = 0; i < count && r.ok && i < 25; ++i) {
      Session s;
      if (!ReadSession(r, s)) break;
      std::lock_guard lock(g_mutex);
      found |= s.id == id;
      g_found[s.id] = std::move(s);
    }
  }
  REXLOG_INFO("p2p: invite: host {} {}", Ip(path.ip), found ? "has the session" : replies.empty() ? "didn't answer" : "no longer has the session");
  return found;
}

std::optional<std::string> P2PFetch(uint8_t kind, const std::string& key, std::chrono::milliseconds timeout) {
  if (g_socket == kBadSocket || key.size() > 200) return std::nullopt;
  FileFetch fetch;
  fetch.kind = kind, fetch.key = key;
  const uint32_t nonce = NewNonce();
  std::vector<Path> peers;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [mac, p] : g_peers) peers.push_back(PathOf(mac, p));
  }
  if (peers.empty()) return std::nullopt;
  {
    std::lock_guard lock(g_wait_mutex);
    g_fetches[nonce] = &fetch;
  }
  const auto ask_all = FileRequest(nonce, kind, key, 0, kNone);
  for (const auto& p : peers) Send(p, ask_all);
  const auto deadline = Clock::now() + timeout;
  auto last = Clock::now();
  uint32_t last_got = 0;
  std::optional<std::string> result;
  std::unique_lock lock(g_wait_mutex);
  while (Clock::now() < deadline) {
    g_wait.wait_for(lock, std::chrono::milliseconds(200));
    if (fetch.known && fetch.got == fetch.have.size()) {
      result = std::move(fetch.data);
      break;
    }
    if (!fetch.known && fetch.refusals >= int(peers.size())) break;  // (nobody has it)
    if (fetch.got != last_got) last = Clock::now(), last_got = fetch.got;
    if (Clock::now() - last < std::chrono::milliseconds(800)) continue;
    last = Clock::now();
    // (what didn't come: asked again, a range at a time)
    std::vector<std::vector<uint8_t>> asks;
    if (!fetch.known) {
      asks.push_back(ask_all);
    } else {
      for (size_t i = 0; i < fetch.have.size() && asks.size() < 32;) {
        if (fetch.have[i]) { ++i; continue; }
        size_t j = i;
        while (j < fetch.have.size() && !fetch.have[j] && j - i < 64) ++j;
        asks.push_back(FileRequest(nonce, kind, key, uint32_t(i * kChunk), uint32_t((j - i) * kChunk)));
        i = j;
      }
    }
    const Path source = fetch.source;
    const bool known = fetch.known;
    lock.unlock();
    for (const auto& a : asks) {
      if (known) {
        Send(source, a);
      } else {
        for (const auto& p : peers) Send(p, a);
      }
    }
    lock.lock();
  }
  g_fetches.erase(nonce);
  lock.unlock();
  REXLOG_INFO("p2p: file {}:{} {}", kind, key,
              result ? fmt::format("fetched ({} bytes)", result->size()) : std::string("not fetched"));
  return result;
}

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
  std::thread(Keepalive).detach();
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
  REXLOG_INFO("p2p: on, {} port {}{} (id {:02X}{:02X}{:02X}{:02X}{:02X}{:02X})", Ip(g_me.lan), g_me.base, ForceRelay() ? ", relay only" : "", g_me.mac[0],
              g_me.mac[1], g_me.mac[2], g_me.mac[3], g_me.mac[4], g_me.mac[5]);
}

}  // namespace svr2011

