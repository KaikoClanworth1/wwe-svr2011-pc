// WWE SmackDown vs. Raw 2011 - session lifecycle and original XGI bridge.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace rex::system { class KernelState; }

namespace svr2011::online {

// Win32/XAM result codes, also used by CompleteOverlappedImmediate.
constexpr uint32_t kSuccess = 0, kInvalidHandle = 6, kUnsupported = 50;
constexpr uint32_t kInvalidArgument = 87, kInsufficientBuffer = 122;
constexpr uint32_t kNotFound = 1168, kConnectionFailed = 12029, kFailure = 31;

struct Error : std::runtime_error {
  uint32_t code;
  Error(uint32_t code, const std::string& reason) : std::runtime_error(reason), code(code) {}
};

struct Property {
  uint32_t id = 0;
  uint8_t type = 0;
  // Scalar bytes and extended strings/binary retain guest big endian order.
  std::vector<uint8_t> value;
  bool operator==(const Property&) const = default;
};
using Contexts = std::map<uint32_t, uint32_t>;
using Properties = std::map<uint32_t, Property>;

struct Identity {
  uint64_t xuid = 0, machine_id = 0;
  std::string name, address, mac;
  uint16_t port = 36000;
};
struct Session {
  uint64_t id = 0, nonce = 0;
  uint32_t flags = 0, public_slots = 0, private_slots = 0;
  uint32_t open_public = 0, open_private = 0, filled_public = 0, filled_private = 0;
  Identity host;
  Contexts contexts;
  Properties properties;
  // REST query return order may differ from property ID order. Guest results
  // preserve it because retail matchmaking can read columns by position.
  std::vector<uint32_t> property_order;
};
struct Search {
  uint32_t query = 0, limit = 25, users = 1;
  Contexts contexts;
  Properties properties;
};
struct Member {
  uint64_t xuid = 0;
  uint32_t user_index = 0xFF;
  bool private_slot = false;
};

// Directory ownership and atomic capacity decisions belong on the server. No game
// packet, NAT, QoS, arbitration or match-start behavior is implemented here.
class Directory {
 public:
  virtual ~Directory() = default;
  virtual void Register(const Identity&) = 0;
  virtual void Create(const Session&) = 0;
  virtual std::vector<Session> Find(const Search&) = 0;
  virtual Session Get(uint64_t id) = 0;
  virtual void Join(uint64_t id, const std::vector<Member>&) = 0;
  virtual void Leave(uint64_t id, const std::vector<uint64_t>&) = 0;
  virtual void Delete(uint64_t id) = 0;
};

struct LocalSession {
  Session session;
  bool created = false, host = false;
  std::map<uint64_t, Member> members;
};

class SessionService {
 public:
  SessionService(Directory& directory, Identity identity,
                 std::function<uint64_t()> random);
  void Reserve(uint32_t handle);
  void Release(uint32_t handle);
  Session Create(uint32_t handle, uint32_t flags, uint32_t public_slots,
                 uint32_t private_slots, uint64_t join_id = 0);
  std::vector<Session> Find(const Search& search);
  void Join(uint32_t handle, const std::vector<Member>& members);
  void Leave(uint32_t handle, const std::vector<uint64_t>& members);
  void Delete(uint32_t handle);
  LocalSession Snapshot(uint32_t handle);
  void SetContext(uint32_t id, uint32_t value);
  void SetProperty(Property property);
  uint64_t local_xuid() const { return identity_.xuid; }

 private:
  LocalSession& Lookup(uint32_t handle);
  Directory& directory_;
  Identity identity_;
  std::function<uint64_t()> random_;
  bool registered_ = false;
  std::mutex mutex_;
  std::map<uint32_t, LocalSession> sessions_;
  Contexts contexts_;
  Properties properties_;
};

std::string Hex(uint64_t value, size_t width = 16);
uint64_t ParseHex(const std::string& value, size_t width = 16);
void ValidateProperty(const Property& property);
void ValidateAddress(const std::string& address);

class GuestMemory {
 public:
  virtual ~GuestMemory() = default;
  virtual std::span<uint8_t> Map(uint32_t address, size_t length, bool write = false) = 0;
  // Must resolve a real session object, not trust the handle in guest memory.
  virtual uint32_t SessionHandle(uint32_t object_address) = 0;
};
class GuestBridge {
 public:
  GuestBridge(SessionService& service, GuestMemory& memory) : service_(service), memory_(memory) {}
  // nullopt preserves SDK handling of unrelated XGI (achievements, stats, etc).
  std::optional<uint32_t> Dispatch(uint32_t message, uint32_t address, uint32_t length);
 private:
  Property ReadProperty(uint32_t address);
  void WriteResults(uint32_t address, uint32_t capacity, const std::vector<Session>&);
  SessionService& service_;
  GuestMemory& memory_;
};
void InstallSessions(rex::system::KernelState* kernel);

}  // namespace svr2011::online
