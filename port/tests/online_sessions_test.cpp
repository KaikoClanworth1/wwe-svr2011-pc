#include "online_sessions.h"
#include "online_directory.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <iostream>
#include <set>

using namespace svr2011::online;
using Json = nlohmann::json;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("CHECK failed: " #x); } while (0)
template<class F> void Fails(uint32_t code, F f) {
  try { f(); } catch (const Error& e) { CHECK(e.code == code); return; }
  throw std::runtime_error("Expected failure");
}
Identity Peer(uint64_t id) { return {id, id, "Player", "127.0.0.1", Hex(id, 12), 36000}; }

// Shared authoritative directory with two independent client services. Checks
// ownership, capacity and rollback rather than mirroring client internals.
class FakeDirectory final : public Directory {
 public:
  std::map<uint64_t, Session> sessions;
  std::map<uint64_t, std::map<uint64_t, bool>> members;
  bool fail = false;
  unsigned joins = 0, leaves = 0, deletes = 0;
  void MaybeFail() { if (fail) throw Error(kConnectionFailed, "Simulated outage"); }
  void Register(const Identity&) override { MaybeFail(); }
  void Create(const Session& s) override { MaybeFail(); CHECK(!sessions.contains(s.id)); sessions[s.id] = s; }
  std::vector<Session> Find(const Search&) override {
    MaybeFail(); std::vector<Session> out;
    for (const auto& [id, value] : sessions) out.push_back(Get(id));
    return out;
  }
  Session Get(uint64_t id) override {
    MaybeFail(); if (!sessions.contains(id)) throw Error(kNotFound, "Missing session");
    auto s = sessions.at(id); s.filled_public = s.filled_private = 0;
    for (const auto& [xuid, slot] : members[id]) slot ? ++s.filled_private : ++s.filled_public;
    s.open_public = s.public_slots - s.filled_public; s.open_private = s.private_slots - s.filled_private;
    return s;
  }
  void Join(uint64_t id, const std::vector<Member>& additions) override {
    MaybeFail(); auto s = Get(id); unsigned pub = 0, priv = 0;
    for (const auto& m : additions) if (!members[id].contains(m.xuid)) m.private_slot ? ++priv : ++pub;
    if (pub > s.open_public || priv > s.open_private) throw Error(kFailure, "Full session");
    for (const auto& m : additions) members[id][m.xuid] = m.private_slot;
    ++joins;
  }
  void Leave(uint64_t id, const std::vector<uint64_t>& removals) override {
    MaybeFail(); for (auto xuid : removals) members[id].erase(xuid); ++leaves;
  }
  void Delete(uint64_t id) override { MaybeFail(); sessions.erase(id); members.erase(id); ++deletes; }
};
class Memory final : public GuestMemory {
 public:
  std::vector<uint8_t> bytes = std::vector<uint8_t>(32768, 0xCC);
  std::span<uint8_t> Map(uint32_t address, size_t size, bool = false) override {
    if (!address || !size || uint64_t(address) + size > bytes.size()) throw Error(kInvalidArgument, "Out of bounds");
    return {bytes.data() + address, size};
  }
  uint32_t SessionHandle(uint32_t p) override { if (p != 256) throw Error(kInvalidHandle, "Wrong object"); return 1; }
  void Put(size_t p, uint64_t v, unsigned size = 4) { for (unsigned i = 0; i < size; ++i) bytes[p + i] = uint8_t(v >> ((size - i - 1) * 8)); }
  uint64_t Get(size_t p, unsigned size = 4) { uint64_t v = 0; for (unsigned i = 0; i < size; ++i) v = (v << 8) | bytes[p + i]; return v; }
};
class ScriptHttp final : public HttpTransport {
 public:
  struct Step { std::string method, path; HttpResponse response; std::function<void(const Json&)> check; };
  std::vector<Step> steps; size_t cursor = 0;
  HttpResponse Request(const std::string& method, const std::string& path, const std::string& body) override {
    CHECK(cursor < steps.size()); auto& step = steps[cursor++]; CHECK(method == step.method); CHECK(path == step.path);
    if (step.check) step.check(body.empty() ? Json() : Json::parse(body));
    return step.response;
  }
};
Json Dto(uint64_t id) {
  return {{"id", Hex(id)}, {"flags", 0x29}, {"hostAddress", "127.0.0.1"}, {"macAddress", "020000000001"},
      {"port", 36000}, {"publicSlotsCount", 2}, {"privateSlotsCount", 0}, {"openPublicSlotsCount", 2},
      {"openPrivateSlotsCount", 0}, {"filledPublicSlotsCount", 0}, {"filledPrivateSlotsCount", 0}};
}
void Lifecycle() {
  FakeDirectory directory; uint64_t random = 1;
  SessionService host(directory, Peer(1), [&] { return random++; });
  SessionService guest(directory, Peer(2), [&] { return random++; });
  host.Reserve(1); guest.Reserve(2);
  Fails(kInvalidHandle, [&] { host.Reserve(1); });
  Fails(kInvalidArgument, [&] { host.Create(1, 0x29, 0, 0); });
  Fails(kUnsupported, [&] { host.Create(1, 0x39, 2, 0); });
  directory.fail = true; Fails(kConnectionFailed, [&] { host.Create(1, 0x29, 2, 0); });
  CHECK(!host.Snapshot(1).created); directory.fail = false;
  auto s = host.Create(1, 0x29, 2, 0); CHECK((s.id >> 56) == 0xAE);
  host.Join(1, {{1, 0, false}}); CHECK(guest.Find({}).size() == 1);
  guest.Create(2, 0x28, 2, 0, s.id);
  directory.fail = true; Fails(kConnectionFailed, [&] { guest.Join(2, {{2, 0, false}}); });
  CHECK(guest.Snapshot(2).members.empty()); directory.fail = false;
  guest.Join(2, {{2, 0, false}}); auto joins = directory.joins;
  guest.Join(2, {{2, 0, false}}); CHECK(directory.joins == joins); CHECK(guest.Find({}).empty());
  Fails(kFailure, [&] { host.Join(1, {{3, 0xFF, false}}); });
  CHECK(directory.joins == joins); CHECK(host.Snapshot(1).members.size() == 1);
  directory.fail = true; Fails(kConnectionFailed, [&] { guest.Leave(2, {2}); });
  CHECK(guest.Snapshot(2).members.size() == 1); directory.fail = false;
  guest.Delete(2); CHECK(directory.deletes == 0); CHECK(directory.Get(s.id).open_public == 1);
  Fails(kUnsupported, [&] { host.SetContext(0x800A, 1); });
  directory.fail = true; Fails(kConnectionFailed, [&] { host.Delete(1); }); CHECK(host.Snapshot(1).created);
  directory.fail = false; host.Delete(1); CHECK(directory.sessions.empty());
  host.Release(1); Fails(kInvalidHandle, [&] { host.Snapshot(1); });
  Fails(kInvalidArgument, [&] { SessionService bad(directory, {3,3,"P","999.1.1.1","020000000003",36000}, [] { return 1; }); });
}
void GuestBuffers() {
  FakeDirectory directory; Memory memory; SessionService service(directory, Peer(1), [] { return 7; });
  service.Reserve(1); GuestBridge bridge(service, memory);
  CHECK(!bridge.Dispatch(0xB0008, 0, 0));
  memory.Put(512, 256); memory.Put(516, 0x29); memory.Put(520, 2); memory.Put(524, 0); memory.Put(528, 0);
  CHECK(!bridge.Dispatch(0xB0014, 512, 16)); // existing offline/stat behavior
  memory.Put(516, 4);
  CHECK(!bridge.Dispatch(0xB0010, 512, 28)); CHECK(directory.sessions.empty());
  memory.Put(516, 0x29);
  memory.Put(532, 1024); memory.Put(536, 32764);
  CHECK(bridge.Dispatch(0xB0010, 512, 28) == kInvalidArgument); CHECK(directory.sessions.empty());
  memory.Put(536, 1088); CHECK(bridge.Dispatch(0xB0010, 512, 27) == kInvalidArgument);
  CHECK(bridge.Dispatch(0xB0010, 512, 0) == kSuccess); CHECK(memory.Get(1024, 8) == 0xAE00000000000007ull);
  CHECK(bridge.Dispatch(0xB0014, 512, 16) == kUnsupported); // do not start a peer match
  CHECK(memory.Get(1040, 2) == 36000); CHECK(memory.Get(1088, 8) == 7);
  CHECK(memory.Get(1068, 8) == 0); // security key remains zero
  auto& stored = directory.sessions.begin()->second;
  stored.properties = {{0x1000000A,{0x1000000A,1,{0,0,0,9}}}, {0x40008109,{0x40008109,4,{0,65,0,0}}}};
  stored.property_order = {0x40008109, 0x1000000A};
  memory.Put(600, 0); memory.Put(604, 0); memory.Put(608, 25); memory.Put(612, 0); memory.Put(616, 0); memory.Put(620, 0);
  memory.Put(624, 8); memory.Put(628, 2048);
  auto before = memory.bytes;
  CHECK(bridge.Dispatch(0xB0016, 600, 32) == kInsufficientBuffer); CHECK(memory.bytes == before);
  memory.Put(624, 1024); CHECK(bridge.Dispatch(0xB0016, 600, 32) == kSuccess);
  CHECK(memory.Get(2048) == 1); CHECK(memory.Get(2052) == 2056);
  auto props = memory.Get(2056 + 84); CHECK(memory.Get(props) == 0x40008109);
  CHECK(memory.Get(props + 20) == props + 48); CHECK(memory.Get(props + 48, 2) == 65);
  CHECK(memory.Get(props + 24) == 0x1000000A); CHECK(memory.Get(props + 40) == 9);
  memory.Put(712, 0xFFFFFFFF); // malformed remote XUID pointer
  memory.Put(704, 256); memory.Put(708, 1); memory.Put(720, 900);
  CHECK(bridge.Dispatch(0xB0012, 704, 20) == kInvalidArgument); CHECK(directory.joins == 0);
  memory.Put(712, 0); memory.Put(716, 896); memory.Put(896, 0); memory.Put(900, 0);
  CHECK(bridge.Dispatch(0xB0012, 704, 20) == kSuccess); CHECK(directory.joins == 1);
  CHECK(bridge.Dispatch(0xB0013, 704, 20) == kSuccess); CHECK(directory.leaves == 1);
  CHECK(bridge.Dispatch(0xB0013, 704, 20) == kSuccess); CHECK(directory.leaves == 1);
  CHECK(bridge.Dispatch(0xB0011, 704, 16) == kSuccess); CHECK(directory.sessions.empty());
  CHECK(!bridge.Dispatch(0xB0014, 704, 16));
}
void RestContract() {
  Property scalar{0x1000000A, 1, {0,0,0,42}}, text{0x40008109,4,{0,65,0,0}};
  CHECK(DecodeProperty(EncodeProperty(scalar)) == scalar); CHECK(DecodeProperty(EncodeProperty(text)) == text);
  Fails(kFailure, [] { DecodeProperty("!!!!"); });
  constexpr uint64_t id = 0xAE00000000000001ull;
  std::string root = "/title/5451085D/sessions", path = root + '/' + Hex(id);
  ScriptHttp http;
  http.steps = {
    {"POST","/players",{201,""},[](const Json& j) { CHECK(j.at("xuid") == "0000000000000001"); CHECK(j.contains("machineId")); }},
    {"POST",root,{201,""},[](const Json& j) { CHECK(j.at("flags") == 0x29); CHECK(j.at("publicSlotsCount") == 2); }},
    {"POST",root+"/search",{201,Json::array({Dto(id)}).dump()},[](const Json& j) { CHECK(j.at("searcher_xuid") == "0000000000000001"); CHECK(j.at("filters").size() == 1); }},
    {"GET",path+"/properties/0",{200,Json{{"properties",Json::array({EncodeProperty(text),EncodeProperty(scalar)})}}.dump()},{}},
    {"GET",path,{200,Dto(id).dump()},{}},
    {"POST",path+"/join",{201,""},[](const Json& j) { CHECK(j.at("xuids")[0] == "0000000000000002"); CHECK(j.at("privateSlots")[0] == false); }},
    {"POST",path+"/leave",{201,""},[](const Json& j) { CHECK(j.at("xuids")[0] == "0000000000000002"); }},
    {"DELETE",path,{200,""},{}}, {"GET",path,{404,""},{}}
  };
  RestDirectory rest(http); rest.Register(Peer(1)); Session s; s.id=id; s.flags=0x29; s.public_slots=2; s.host=Peer(1);
  rest.Create(s); Search query; query.contexts[0x800A]=1;
  auto found = rest.Find(query); CHECK(found.size()==1); CHECK(found[0].property_order[0]==text.id);
  CHECK(rest.Get(id).host.port==36000); rest.Join(id,{{2,0,false}}); rest.Leave(id,{2}); rest.Delete(id);
  CHECK(http.cursor==http.steps.size());
  // Metadata failure rolls back creation, and a refused DELETE is not success.
  ScriptHttp failure; failure.steps={{"POST",root,{201,""},{}},{"POST",path+"/context",{500,""},{}},
      {"DELETE",path,{200,""},{}},{"GET",path,{404,""},{}}};
  RestDirectory rollback(failure); s.contexts[0x800A]=1;
  Fails(kConnectionFailed,[&]{rollback.Create(s);}); CHECK(failure.cursor==4);
  ScriptHttp refused; refused.steps={{"DELETE",path,{200,""},{}},{"GET",path,{200,Dto(id).dump()},{}}};
  RestDirectory ownership(refused); Fails(kFailure,[&]{ownership.Delete(id);});
}
int main() {
  try { Lifecycle(); GuestBuffers(); RestContract(); std::cout << "Session lifecycle, guest ABI and REST contract checks passed\n"; }
  catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
