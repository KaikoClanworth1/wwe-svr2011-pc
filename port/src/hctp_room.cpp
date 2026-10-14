// HCTP backstage rooms (branch hctp-backstage): SmackDown! Here Comes the
// Pain's backstage areas played as backstage mods with their own row (see
// docs/BACKSTAGE_MODS.md). The room, its props, its climb spots (T21/T22 on
// 0xDF walls) and its converted HCTP motions are data (the mod's arena.pac
// and gimmick.pac); what 2011 has no data for is done here.
//
// Test aid (SVR2011_TEST_HCTP_LOG=1): the wall-attribute switch
// (sub_82306B10, fighter's touched attribute at +455), the walk-into-wall
// gimmick search (sub_824B6A70, T22) and the motion-key lookups it makes
// (sub_82394520(bank, key, x, y)) are logged.
#include <chrono>
#include <cstdint>
#include <cstdlib>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "arena_mods.h"
#include "match_types.h"

#include "generated/default/svr2011_init.h"

namespace {

const bool g_log = std::getenv("SVR2011_TEST_HCTP_LOG") != nullptr;
// (tests: SVR2011_TEST_HCTP_ROOF=1 the roof breaks at the first knock-down,
// =now 5 seconds into the match)
const char* const g_roof_env = std::getenv("SVR2011_TEST_HCTP_ROOF");
const bool g_roof_once = g_roof_env != nullptr;
const bool g_roof_now = g_roof_env && std::strcmp(g_roof_env, "now") == 0;
// (=crate: fighter 1 is shut in the equipment crate 5 seconds in)
const bool g_crate_now = g_roof_env && std::strcmp(g_roof_env, "crate") == 0;
// (=steam: both valves opened 5 seconds in)
const bool g_steam_now = g_roof_env && std::strcmp(g_roof_env, "steam") == 0;
// (=puff: a steam puff shown over fighter 0's head, to see one)
const bool g_puff_now = g_roof_env && std::strcmp(g_roof_env, "puff") == 0;
// (=props: the railings and the lead pipes knocked over 5 seconds in)
const bool g_props_now = g_roof_env && std::strcmp(g_roof_env, "props") == 0;
// (=nograb: no cage-wall grabs, so strikes reach the fence)
const bool g_no_grab = g_roof_env && std::strcmp(g_roof_env, "nograb") == 0;
// (=cart: the button cycle starts 5 seconds in)
const bool g_cart_now = g_roof_env && std::strcmp(g_roof_env, "cart") == 0;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
float RdF(const uint8_t* p) {
  const uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

// (at most n lines a second per site)
bool Allow(int& budget, std::chrono::steady_clock::time_point& t0, int n = 4) {
  const auto now = std::chrono::steady_clock::now();
  if (now - t0 > std::chrono::seconds(1)) {
    t0 = now;
    budget = n;
  }
  return budget-- > 0;
}

}  // namespace

// -- Climbing ----------------------------------------------------------------
//
// In HCTP a wrestler who walks into a climbable wall (collision attribute
// 0xFB crates, 0xFD crate top, 0xF4 ladder, 0xDF cage) climbs it. The room's
// converter gives those walls 0xDF and puts a T22 walk-into-wall spot (the
// HCTP climb motion as its key) in front of each. 2011 reaches its T22
// search (sub_824B6A70) only from the ring-style walk handler; backstage
// fighters walk in motion 20500, which never gets there. So: after a
// fighter's collision (sub_821E5910), when he has pressed into a 0xDF wall
// for a few frames while walking, the T22 search is asked here (it checks
// the spot's range, snaps and turns him and plays the key).
namespace {

constexpr uint32_t kChars = 0x82E3CC50;
constexpr uint32_t kWalkBackstage = 20500;

struct Climber {
  uint32_t fighter = 0;
  int pressing = 0;    // frames in a row against a 0xDF wall
  int cooldown = 0;    // frames before another try
};
Climber g_climbers[8];
// Touched attributes that climb besides 0xDF: the standing cage panels (their
// collision carries the panel's own tag, 0xA0 + prop, in place of 0xDF).
bool g_climb_attr[256];

bool HctpRoomActive() {
  const int r = svr2011::g_active_row;
  return r >= 0 && size_t(r) < svr2011::BackstageRows().size() && svr2011::BackstageRows()[size_t(r)].hctp > 0;
}

void Climb(PPCContext& ctx, uint8_t* base, uint32_t f) {
  int slot = -1;
  for (int i = 0; i < 8; ++i)
    if (Rd32(base + kChars + i * 4) == f) slot = i;
  if (slot < 0) return;
  Climber& c = g_climbers[slot];
  if (c.fighter != f) c = {f, 0, 0};
  if (c.cooldown > 0) --c.cooldown;
  // (the touched attribute is there every other frame while he pushes: the
  // collision pushes him out, the next frame he walks back in)
  // (walking, or running: +1286)
  const bool walking = Rd32(base + f + 212) == kWalkBackstage || base[f + 1286];
  const uint8_t touched = base[f + 455];
  if (!walking)
    c.pressing = 0;
  else if (touched == 0xDF || g_climb_attr[touched])
    c.pressing += 2;
  else if (c.pressing > 0)
    --c.pressing;
  if (c.pressing < 12 || c.cooldown > 0) return;
  const auto saved = ctx;
  ctx.r3.u64 = f;
  sub_824B6A70(ctx, base);
  const uint32_t started = ctx.r3.u32;
  ctx = saved;
  c.cooldown = 30;
  c.pressing = 0;
  REXLOG_INFO("hctp: fighter {} pressed into a climb wall at ({:.1f} {:.1f} {:.1f}): {}", slot,
              RdF(base + f + 288), RdF(base + f + 292), RdF(base + f + 296),
              started ? fmt::format("climbs (motion {})", Rd32(base + f + 212)) : std::string("no spot in range"));
}

}  // namespace

// A fighter's stage collision, once a frame (sub_821E5910(collision
// component); the fighter at component +32).
REX_EXTERN(__imp__sub_821E5910);
REX_HOOK_RAW(sub_821E5910) {
  const uint32_t comp = ctx.r3.u32;
  const uint32_t f = comp ? Rd32(base + comp + 32) : 0;
  __imp__sub_821E5910(ctx, base);
  if (f && HctpRoomActive()) Climb(ctx, base, f);
}

REX_EXTERN(__imp__sub_82306B10);
REX_HOOK_RAW(sub_82306B10) {
  const uint32_t f = ctx.r3.u32;
  if (g_log) {
    static int b = 0;
    static std::chrono::steady_clock::time_point t;
    if (Allow(b, t))
      REXLOG_INFO("hctp: wall switch: fighter {:08X} attr {:02X} motion {} running {}", f, base[f + 455],
                  Rd32(base + f + 212), base[f + 1286]);
  }
  __imp__sub_82306B10(ctx, base);
}

REX_EXTERN(__imp__sub_824B6A70);
REX_HOOK_RAW(sub_824B6A70) {
  const uint32_t f = ctx.r3.u32;
  __imp__sub_824B6A70(ctx, base);
  if (g_log) {
    static int b = 0;
    static std::chrono::steady_clock::time_point t;
    if (Allow(b, t))
      REXLOG_INFO("hctp: T22 search: fighter {:08X} -> {}", f, ctx.r3.u32);
  }
}

REX_EXTERN(__imp__sub_82394520);
REX_HOOK_RAW(sub_82394520) {
  const uint32_t key = ctx.r4.u32;
  __imp__sub_82394520(ctx, base);
  if (g_log && key >= 12500 && key < 12600) {
    static int b = 0;
    static std::chrono::steady_clock::time_point t;
    if (Allow(b, t)) REXLOG_INFO("hctp: motion key {} -> {:08X}", key, ctx.r3.u32);
  }
}

// -- The room's gimmicks -------------------------------------------------------
//
// HCTP's Arena Back Entrance gimmicks (HCTP's own code: CGmControl and its
// object classes), from the mod's hctp.txt (written by the converter):
//   prop <name> <hctp id> <kind> <attr> <x> <y> <z> <ry> recs=<GMPD records> tris=<HMD 1004 triangles>
//        box=<x0,x1,y0,y1,z0,z1> (its collision's extent)
//   weapon <hctp id> <model> <x> <y> <z>
//   key <key> <frames>
// Each prop's wall triangles carry the low attribute byte <attr>, so a
// fighter's touched attribute (+455) names the prop he hit.
//  - stack (cardboard boxes, barrels; HCTP gmAttackObjAndWeapon) and throw
//    (railings, lead pipes; CGmThrowObject): a fighter thrown / whipped /
//    running into it (touching it while not in the walk motion) knocks it
//    over: its fall motion, his hit reaction, its collision gone, then its
//    weapon (2011 stand-ins: trash can 63, tire iron 120); railings vanish.
//  - roof (two halves, CGmComeOffObject): a knock-down on the roof (motion
//    1000 starting there) shakes it, the second breaks both halves - their
//    collision goes and whoever stands there falls into the cage.
//  - wall (the cage's chain-link walls, CGmWireNetting): the action button (X)
//    at a wall grabs it (12507); about 11 presses tear it and its partner
//    down (12508); they vanish and stop blocking.
namespace {

constexpr uint32_t kGimmickMgr = 0x82E3DBCC, kGimmickBank = 0x82E3CC40, kStage = 0x82E35530;
constexpr uint32_t kMatchFrames = 0x82E3CD0C;
constexpr int kParkingTrip = 4;  // stage TRIP slot of rule 0x1B (HMD 1004)
// converted HCTP motion keys (hctp2ymks: HCTP act -> ours)
constexpr uint32_t kHitObject = 12515;  // HCTP 12014 hit-object reactions, variants 0-5
constexpr uint32_t kWallGrab = 12507, kWallBreak = 12508, kWallRelease = 12545;
constexpr uint32_t kLying = 1000;
constexpr uint32_t kButtonX = 0x80;  // (input +52: pressed this frame)

struct Prop {
  std::string name, kind;
  int hid = 0;
  uint8_t attr = 0;
  float x = 0, y = 0, z = 0, ry = 0;
  std::vector<int> recs, tris;
  float box[6] = {};
  bool has_box = false;
  float zone[6] = {};  // switch / valve: where the button press counts
  bool has_zone = false;
  // run time
  int state = 0;  // 0 at rest, 1 falling / broken, 2 done
  int timer = 0;
  int count = 0;  // roof knock-downs
  bool held = false;
  int cooldown = 0;
  std::vector<std::pair<float, float>> saved_y;  // collision y ranges, for RestoreCollision
};
// A steam jet (HCTP particle 32 / 34, toggled by a valve): from (x, y, z)
// along (dx, dz) for len, half width hw, half height hh.
struct Steam {
  int pid = 0, valve = 0;
  float x = 0, y = 0, z = 0, dx = 0, dz = 0, len = 0, hw = 0, hh = 0;
  bool on = false;
  int t = 0;
};
struct WeaponSpot {
  int hid, model;
  float x, y, z;
};
struct Room {
  std::string dir;
  std::vector<Prop> props;
  std::vector<WeaponSpot> weapons;
  std::map<uint32_t, int> frames;
  bool loaded = false;
  uint32_t last_frame = 0;
  int drop = 0;  // frames left of the roof's fall-in nudge
  std::vector<Steam> steam;
  int set_up_frames = 0;  // frames of object setup done (SetUp)
  // the switch cycle: 0 armed, 1 shutter opening, 2 cart rolling, 3 cart hit
  // something / arrived, 4 waiting, 5 shutter closing
  int cycle = 0, cycle_t = 0;
  float cart_z = 0;
  // the equipment crate: 0 ready, 1 victim shut in, 2 climbing out, 3 resting
  int crate = 0, crate_t = 0;
  uint32_t crate_victim = 0;
};
Room g_room;

struct FighterState {
  uint32_t fighter = 0, motion = 0;
  int wall = -1;  // the wall prop he holds
  int presses = 0;
  int idle = 0;   // frames since his last press
  int burn_cd = 0;  // frames before steam can burn him again
};
FighterState g_fs[8];

void WrF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

std::vector<int> Ints(const std::string& s) {
  std::vector<int> out;
  size_t at = 0;
  while (at < s.size()) {
    const size_t end = std::min(s.find(',', at), s.size());
    out.push_back(std::atoi(s.substr(at, end - at).c_str()));
    at = end + 1;
  }
  return out;
}

bool LoadRoom(const std::string& dir) {
  g_room = Room{};
  g_room.dir = dir;
  std::ifstream in(std::filesystem::path(dir) / "hctp.txt");
  if (!in) {
    REXLOG_WARN("hctp: no hctp.txt in {}", dir);
    return false;
  }
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream ls(line);
    std::string what;
    ls >> what;
    if (what == "prop") {
      Prop p;
      int attr = 0;
      ls >> p.name >> p.hid >> p.kind >> attr >> p.x >> p.y >> p.z >> p.ry;
      p.attr = uint8_t(attr);
      std::string kv;
      while (ls >> kv) {
        if (kv.rfind("recs=", 0) == 0) p.recs = Ints(kv.substr(5));
        if (kv.rfind("tris=", 0) == 0) p.tris = Ints(kv.substr(5));
        if (kv.rfind("zone=", 0) == 0)
          p.has_zone = std::sscanf(kv.c_str() + 5, "%f,%f,%f,%f,%f,%f", &p.zone[0], &p.zone[1], &p.zone[2],
                                   &p.zone[3], &p.zone[4], &p.zone[5]) == 6;
        if (kv.rfind("box=", 0) == 0)
          p.has_box = std::sscanf(kv.c_str() + 4, "%f,%f,%f,%f,%f,%f", &p.box[0], &p.box[1], &p.box[2], &p.box[3],
                                  &p.box[4], &p.box[5]) == 6;
      }
      g_room.props.push_back(std::move(p));
    } else if (what == "weapon") {
      WeaponSpot w{};
      ls >> w.hid >> w.model >> w.x >> w.y >> w.z;
      g_room.weapons.push_back(w);
    } else if (what == "steam") {
      Steam st;
      ls >> st.pid >> st.valve >> st.x >> st.y >> st.z >> st.dx >> st.dz >> st.len >> st.hw >> st.hh;
      g_room.steam.push_back(st);
    } else if (what == "key") {
      uint32_t k = 0;
      int n = 0;
      ls >> k >> n;
      g_room.frames[k] = n;
    }
  }
  g_room.loaded = true;
  std::fill(std::begin(g_climb_attr), std::end(g_climb_attr), false);
  for (const Prop& p : g_room.props)
    if (p.kind == "wall") g_climb_attr[p.attr] = true;
  REXLOG_INFO("hctp: room data: {} props, {} weapon spots, {} motions", g_room.props.size(), g_room.weapons.size(),
              g_room.frames.size());
  return true;
}

// The T23 element of a GMPD record id (0 if none).
uint32_t Element(PPCContext& ctx, uint8_t* base, int rec) {
  const uint32_t mgr = Rd32(base + kGimmickMgr);
  if (!mgr) return 0;
  const auto saved = ctx;
  ctx.r3.u64 = mgr;
  ctx.r4.u64 = uint32_t(rec);
  ctx.r5.u64 = 0;
  sub_824B0DC8(ctx, base);
  const uint32_t e = ctx.r3.u32;
  ctx = saved;
  return e && Rd32(base + e + 68) == 23 ? e : 0;
}

bool HasKey(PPCContext& ctx, uint8_t* base, uint32_t key, uint32_t x, uint32_t y) {
  const auto saved = ctx;
  ctx.r3.u64 = Rd32(base + kGimmickBank);
  ctx.r4.u64 = key, ctx.r5.u64 = x, ctx.r6.u64 = y;
  sub_82394520(ctx, base);
  const bool ok = ctx.r3.u32 != 0;
  ctx = saved;
  return ok;
}

// One record's objects play (key, x, y) from the room bank.
void PlayRec(PPCContext& ctx, uint8_t* base, int rec, uint32_t key, uint32_t x = 0, uint32_t y = 0) {
  const uint32_t e = Element(ctx, base, rec);
  if (!e) return;
  for (uint32_t at : {100u, 104u})  // (a moved object: its motion drives it again)
    if (const uint32_t obj = Rd32(base + e + at)) Wr32(base + obj + 1140, 0);
  const auto saved = ctx;
  ctx.r3.u64 = e;
  ctx.r4.u64 = key, ctx.r5.u64 = x, ctx.r6.u64 = y;
  sub_824B91C0(ctx, base);
  ctx = saved;
}

// A prop's objects play (key, x, y) from the room bank.
void PlayProp(PPCContext& ctx, uint8_t* base, const Prop& p, uint32_t key, uint32_t x = 0, uint32_t y = 0) {
  if (!HasKey(ctx, base, key, x, y)) {
    REXLOG_WARN("hctp: {}: no motion ({}, {}, {}) in the room bank", p.name, key, x, y);
    return;
  }
  for (int rec : p.recs) PlayRec(ctx, base, rec, key, x, y);
}

// One record's element placed at (x, y, z), turned (rx, ry, rz): its
// position and rotation, then sub_824B9C40 copies them to both objects and
// rebuilds their nodes. The objects' pose copy (sub_824B46F8, each frame)
// would put back the matrices their last motion made at the old place, so
// their motion component ('S3MW') is marked not running (+12 = 0; the next
// key sets it again) - 46F8 then only commits the rebuilt node. (Not +1140:
// that skips the commit too and the object isn't drawn where it is.)
void StopPoseCopy(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  const auto saved = ctx;
  ctx.r3.u64 = obj + 1088;
  ctx.r4.u64 = 0x53334D57;  // 'S3MW'
  sub_8269DFB0(ctx, base);
  const uint32_t c = ctx.r3.u32;
  ctx = saved;
  if (const uint32_t m = c ? Rd32(base + c + 24) : 0) Wr32(base + m + 12, 0);
  Wr32(base + obj + 1140, 0);
}

void MoveRec(PPCContext& ctx, uint8_t* base, int rec, float x, float y, float z, float rx = 0, float ry = 0,
             float rz = 0) {
  const uint32_t e = Element(ctx, base, rec);
  if (!e) return;
  WrF(base + e + 32, x), WrF(base + e + 36, y), WrF(base + e + 40, z);
  WrF(base + e + 48, rx), WrF(base + e + 52, ry), WrF(base + e + 56, rz);
  for (uint32_t at : {100u, 104u})
    if (const uint32_t obj = Rd32(base + e + at)) StopPoseCopy(ctx, base, obj);
  const auto saved = ctx;
  ctx.r3.u64 = e;
  sub_824B9C40(ctx, base);
  ctx = saved;
}

// A prop's objects freeze on their current pose (the end of a fall).
void HoldProp(PPCContext& ctx, uint8_t* base, const Prop& p) {
  for (int rec : p.recs)
    if (const uint32_t e = Element(ctx, base, rec))
      for (uint32_t at : {100u, 104u})
        if (const uint32_t obj = Rd32(base + e + at)) Wr32(base + obj + 1140, 1);
}

void ShowRec(PPCContext& ctx, uint8_t* base, int rec, bool show) {
  const uint32_t e = Element(ctx, base, rec);
  if (!e) return;
  // (hidden for good: the element inactive too - a playing key shows its
  // objects again otherwise)
  const auto saved_e = ctx;
  ctx.r3.u64 = e, ctx.r4.u64 = show ? 1 : 0, ctx.r5.u64 = show ? 1 : 0;
  sub_824B8FE0(ctx, base);
  ctx = saved_e;
  for (uint32_t at : {100u, 104u})
    if (const uint32_t obj = Rd32(base + e + at)) {
      const auto saved = ctx;
      ctx.r3.u64 = obj, ctx.r4.u64 = show ? 1 : 0;
      sub_824B4198(ctx, base);
      ctx.r3.u64 = obj, ctx.r4.u64 = show ? 1 : 0;
      sub_824B4150(ctx, base);
      ctx = saved;
    }
}

void ShowProp(PPCContext& ctx, uint8_t* base, const Prop& p, bool show) {
  for (int rec : p.recs) ShowRec(ctx, base, rec, show);
  if (g_log) REXLOG_INFO("hctp: {} {}", p.name, show ? "shown" : "hidden");
}

// The prop's triangles out of the parking room's collision (their y range
// moved far away: the broad phase never marks them near).
void DropCollision(uint8_t* base, Prop& p) {
  const uint32_t stage = Rd32(base + kStage);
  const uint32_t trip = stage ? Rd32(base + stage + 22516 + 4 * kParkingTrip) : 0;
  if (!trip) return;
  const uint32_t n = Rd32(base + trip + 176), recs = Rd32(base + trip + 180);
  int dropped = 0;
  for (int t : p.tris) {
    if (t < 0 || uint32_t(t) >= n) continue;
    const uint32_t r = Rd32(base + recs + 4 * uint32_t(t));
    if (!r) continue;
    if (RdF(base + r + 12) < 1.0e5f) p.saved_y.push_back({RdF(base + r + 12), RdF(base + r + 28)});
    WrF(base + r + 12, 1.0e6f);
    WrF(base + r + 28, 1.0e6f);
    ++dropped;
  }
  REXLOG_INFO("hctp: {}: {} collision triangles out", p.name, dropped);
}

// The prop's triangles back (what DropCollision took out).
void RestoreCollision(uint8_t* base, Prop& p) {
  const uint32_t stage = Rd32(base + kStage);
  const uint32_t trip = stage ? Rd32(base + stage + 22516 + 4 * kParkingTrip) : 0;
  if (!trip || p.saved_y.empty()) return;
  const uint32_t n = Rd32(base + trip + 176), recs = Rd32(base + trip + 180);
  size_t k = 0;
  for (int t : p.tris) {
    if (t < 0 || uint32_t(t) >= n || k >= p.saved_y.size()) continue;
    const uint32_t r = Rd32(base + recs + 4 * uint32_t(t));
    if (!r) continue;
    WrF(base + r + 12, p.saved_y[k].first);
    WrF(base + r + 28, p.saved_y[k].second);
    ++k;
  }
  p.saved_y.clear();
}

// A fighter plays (key, x, y) from the room bank, from frame 'start'.
void JumpAct(PPCContext& ctx, uint8_t* base, uint32_t f, uint32_t key, uint32_t x = 0, uint32_t y = 0,
             float start = 0.0f) {
  if (!HasKey(ctx, base, key, x, y)) return;
  const auto saved = ctx;
  ctx.r3.u64 = f;
  ctx.r4.u64 = key, ctx.r5.u64 = x, ctx.r6.u64 = y;
  ctx.f1.f64 = start;
  sub_82396E80(ctx, base);
  ctx = saved;
}

// A weapon lying at (x, y, z): sub_821D2070(any free slot, position,
// rotation, model; stack: motion 15010 lying, place 1 floor, flag 1).
void SpawnWeapon(PPCContext& ctx, uint8_t* base, int model, float x, float y, float z) {
  const auto saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  Wr32(base + ctx.r1.u32 + 84, 15010);
  Wr32(base + ctx.r1.u32 + 92, 1);
  Wr32(base + ctx.r1.u32 + 100, 1);
  ctx.r3.u64 = 0xFFFFFFFFu;
  ctx.f1.f64 = x, ctx.f2.f64 = y, ctx.f3.f64 = z;
  ctx.f4.f64 = 0, ctx.f5.f64 = 0, ctx.f6.f64 = 0;
  ctx.r7.u64 = uint32_t(model);
  sub_821D2070(ctx, base);
  const uint32_t w = ctx.r3.u32;
  ctx = saved;
  REXLOG_INFO("hctp: weapon {} at ({:.0f} {:.0f} {:.0f}): {}", model, x, y, z,
              w ? "made" : "not made (no free slot, or the model isn't loaded)");
}

int Frames(uint32_t key, int fallback) {
  const auto it = g_room.frames.find(key);
  return it == g_room.frames.end() ? fallback : it->second;
}

// A prop's own fall motion: its HCTP object act, converted as 12500 + act.
uint32_t FallKey(const Prop& p) {
  switch (p.hid) {
    case 202: return 12533;  // cardboard boxes collapse
    case 218: return 12526;  // barrels fall
    case 209: return 12524;  // lead pipes fall
    case 206: return 12528;  // a railing falls
    default: return 0;
  }
}

void KnockOver(PPCContext& ctx, uint8_t* base, Prop& p, uint32_t f) {
  p.state = 1;
  p.timer = 0;
  if (const uint32_t key = FallKey(p)) PlayProp(ctx, base, p, key);
  DropCollision(base, p);
  if (f) JumpAct(ctx, base, f, kHitObject, uint32_t(1 + std::rand() % 5));
  REXLOG_INFO("hctp: {} knocked over", p.name);
}

// The frame a prop's first object is on (its motion's +24), -1 if none.
float PropFrame(PPCContext& ctx, uint8_t* base, const Prop& p) {
  for (int rec : p.recs)
    if (const uint32_t e = Element(ctx, base, rec))
      for (uint32_t at : {100u, 104u})
        if (const uint32_t obj = Rd32(base + e + at)) return RdF(base + obj + 24);
  return -1.0f;
}

void UpdateFall(PPCContext& ctx, uint8_t* base, Prop& p) {
  if (p.state != 1) return;
  ++p.timer;
  const int n = Frames(FallKey(p), 40);
  // (motions run at 30 frames a second, the room at 60: hold the last pose
  // when the object's own frame gets there)
  if (!p.held && (PropFrame(ctx, base, p) >= float(n - 1) || p.timer > 3 * n)) {
    HoldProp(ctx, base, p);
    p.held = true;
  }
  // its weapon, when the stack has come down (pipes: HCTP's 60 frames)
  if (p.timer == 2 * std::max(n, p.hid == 209 ? 60 : 0))
    for (const WeaponSpot& w : g_room.weapons)
      if (w.hid == p.hid) SpawnWeapon(ctx, base, w.model, w.x, w.y, w.z);
  // a railing: gone after HCTP's 42 + 30 frames
  if (p.hid == 206 && p.timer == 2 * 72) ShowProp(ctx, base, p, false);
  if (p.held && p.timer > 2 * std::max(n, 72) + 10) p.state = 2;
}

// Inside a prop's collision extent (x, z) grown by m.
bool InBox(const Prop& p, float x, float z, float m) {
  return p.has_box && x > p.box[0] - m && x < p.box[1] + m && z > p.box[4] - m && z < p.box[5] + m;
}

// On a roof half: over its panel, on the roof (y < -25).
bool OnRoofHalf(const Prop& p, float x, float y, float z) {
  return y < -25.0f && InBox(p, x, z, 1.5f);
}

void BreakRoof(PPCContext& ctx, uint8_t* base) {
  for (Prop& r : g_room.props)
    if (r.kind == "roof" && r.state == 0) {
      r.state = 1;
      DropCollision(base, r);
      ShowProp(ctx, base, r, false);
    }
  g_room.drop = 20;
  REXLOG_INFO("hctp: the cage roof breaks");
}

// Whoever stands over the hole falls into the cage (HCTP CellCheck_BackOpenSpace:
// nudged toward the middle, ~0.4 a frame, and gravity takes him to the floor).
void DropIn(uint8_t* base) {
  if (g_room.drop <= 0) return;
  --g_room.drop;
  for (const Prop& r : g_room.props) {
    if (r.kind != "roof" || !r.has_box) continue;
    const float cx = (r.box[0] + r.box[1]) / 2, cz = (r.box[4] + r.box[5]) / 2;
    for (int i = 0; i < 8; ++i) {
      const uint32_t f = Rd32(base + kChars + i * 4);
      if (!f) continue;
      const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
      if (!OnRoofHalf(r, x, y, z)) continue;
      WrF(base + f + 288, x + std::clamp(cx - x, -0.4f, 0.4f));
      WrF(base + f + 296, z + std::clamp(cz - z, -0.4f, 0.4f));
      if (g_log && g_room.drop % 5 == 0)
        REXLOG_INFO("hctp: fighter {} over the hole at ({:.1f} {:.1f} {:.1f})", i, x, y, z);
    }
  }
}

// The wall's partner: HCTP tears down both panels of a side together
// (walls in placement order: 0, 1 one side; 2, 3 the other).
int WallPartner(int i) {
  std::vector<int> walls;
  for (size_t j = 0; j < g_room.props.size(); ++j)
    if (g_room.props[j].kind == "wall") walls.push_back(int(j));
  for (size_t k = 0; k < walls.size(); ++k)
    if (walls[k] == i && (k ^ 1) < walls.size()) return walls[k ^ 1];
  return -1;
}

void BreakWall(PPCContext& ctx, uint8_t* base, int i) {
  for (int j : {i, WallPartner(i)}) {
    if (j < 0) continue;
    Prop& w = g_room.props[size_t(j)];
    if (w.state) continue;
    w.state = 1;
    w.timer = 0;
    g_climb_attr[w.attr] = false;
    PlayProp(ctx, base, w, kWallBreak, 0, 1);
  }
}

void UpdateWall(PPCContext& ctx, uint8_t* base, Prop& w) {
  if (w.state != 1) return;
  if (++w.timer == 100) {  // (HCTP: it fades out from frame 50 - 30 a second)
    ShowProp(ctx, base, w, false);
    DropCollision(base, w);
    w.state = 2;
  }
}

// HCTP motion keys of the switch, valves, steam and crate (hctp2ymks)
constexpr uint32_t kPressButton = 12510;                     // HCTP 12035
constexpr uint32_t kValveOpen = 12511, kValveClose = 12512;  // 12036 / 12037 (the valve on track 1)
constexpr uint32_t kSteamBurn = 12509;                       // 12047
constexpr uint32_t kHitByObject = 12516;                     // 340
constexpr uint32_t kCrateIn = 12513, kCrateOut = 12514;      // 12034 (attacker 0, victim 1, crate 2) / 12043
constexpr uint32_t kShutterOpen = 12525, kShutterClose = 12530, kShutterRest = 12555;
constexpr uint32_t kCratesFall = 12529, kCratesIdle = 12531;
constexpr uint32_t kJumpDown = 12505;   // HCTP 12572
constexpr uint32_t kStepIn = 12506;     // HCTP 12092

Prop* FindKind(const char* kind, int nth = 0) {
  for (Prop& p : g_room.props)
    if (p.kind == kind && nth-- == 0) return &p;
  return nullptr;
}

bool InZone(const Prop& p, float x, float y, float z) {
  return p.has_zone && x > p.zone[0] - 1 && x < p.zone[1] + 1 && z > p.zone[4] - 1 && z < p.zone[5] + 1 &&
         std::fabs(y - p.zone[2]) < 5.0f;
}

// A fighter put at (x, y, z) facing yaw (his position and his physics body's).
// (After his act starts, as the game's own T22 snap does - sub_824B67D0: the
// act's root motion counts from where he is then.)
void PlaceFighter(uint8_t* base, uint32_t f, float x, float y, float z, float yaw) {
  WrF(base + f + 288, x), WrF(base + f + 292, y), WrF(base + f + 296, z);
  WrF(base + f + 276, yaw);
  if (const uint32_t body = Rd32(base + f + 304)) {
    WrF(base + body, x), WrF(base + body + 4, y), WrF(base + body + 8, z);
  }
}

// First frame: the closing shutter (HCTP syattar2) and the steam puffs hidden.
void SetUp(PPCContext& ctx, uint8_t* base) {
  // (again for the room's first frames: the elements may not be there yet
  // on the first one)
  if (g_room.set_up_frames >= 30) return;
  ++g_room.set_up_frames;
  if (Prop* sh = FindKind("shutter"); sh && sh->recs.size() > 1) ShowRec(ctx, base, sh->recs[1], false);
  // (the steam puffs wait under the floor: an object hidden and shown again
  // isn't drawn while no key plays on it)
  for (Prop& p : g_room.props)
    if (p.kind == "puff")
      for (int rec : p.recs) MoveRec(ctx, base, rec, p.x, 1000.0f, p.z);
  // (the cart has no rest key: until a move rebuilds its node it's drawn
  // off its spot, a grey plank by the cage - put it in place now)
  if (Prop* cart = FindKind("cart")) {
    g_room.cart_z = cart->z;
    for (int rec : cart->recs) MoveRec(ctx, base, rec, cart->x, cart->y, cart->z, 0, cart->ry, 0);
  }
}

// -- The green buttons, the shutter and the cart (HCTP CGmSwitch 208,
// CGmShutter 201, CGmCrateAndWinch 220): a button opens the garage shutter
// and the crate cart rolls out into the room; whoever it runs into is hit
// (340) and the crates fall off; 240 HCTP frames later the shutter closes
// and everything is set again.
void PressSwitch(PPCContext& ctx, uint8_t* base, uint32_t f, int i, const Prop& sw) {
  JumpAct(ctx, base, f, kPressButton);
  REXLOG_INFO("hctp: fighter {} presses {}", i, sw.name);
  if (g_room.cycle != 0) return;
  g_room.cycle = 1;
  g_room.cycle_t = 0;
  if (Prop* sh = FindKind("shutter")) PlayRec(ctx, base, sh->recs[0], kShutterOpen);
}

void UpdateCycle(PPCContext& ctx, uint8_t* base) {
  Prop* sh = FindKind("shutter");
  Prop* cart = FindKind("cart");
  if (!sh || !cart || g_room.cycle == 0) return;
  const int t = ++g_room.cycle_t;
  constexpr float kCartEnd = -429.0f;  // HCTP z -11
  switch (g_room.cycle) {
    case 1:  // the shutter rolls up (26 frames at 30 a second), then it's gone
      if (t == 2 * 26) {
        ShowRec(ctx, base, sh->recs[0], false);
        DropCollision(base, *cart);
        g_room.cycle = 2, g_room.cycle_t = 0;
        REXLOG_INFO("hctp: the shutter is up - the cart rolls");
      }
      break;
    case 2: {  // the cart rolls out (HCTP CObjMove: 5% of the way a frame at 30)
      const float step = std::clamp((kCartEnd - g_room.cart_z) * 0.025f, 0.3f, 1.6f);
      g_room.cart_z = std::min(g_room.cart_z + step, kCartEnd);
      for (int rec : cart->recs) MoveRec(ctx, base, rec, cart->x, cart->y, g_room.cart_z, 0, cart->ry, 0);
      const float dz = g_room.cart_z - cart->z;
      bool hit = false;
      for (int i = 0; i < 8; ++i) {
        const uint32_t f = Rd32(base + kChars + i * 4);
        if (!f) continue;
        const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
        if (y < -6.0f || x < cart->box[0] - 1 || x > cart->box[1] + 1 || z < cart->box[4] + dz - 1 ||
            z > cart->box[5] + dz + 1)
          continue;
        JumpAct(ctx, base, f, kHitByObject);
        REXLOG_INFO("hctp: the cart runs into fighter {}", i);
        hit = true;
      }
      if (hit || g_room.cart_z >= kCartEnd - 0.01f) {
        PlayRec(ctx, base, cart->recs[0], kCratesFall);
        g_room.cycle = 3, g_room.cycle_t = 0;
      }
      break;
    }
    case 3:  // the crates fall off; cart, shadow and crates fade (HCTP 60..160)
      if (t == 2 * 60) {
        ShowRec(ctx, base, cart->recs[1], false);
        if (cart->recs.size() > 2) ShowRec(ctx, base, cart->recs[2], false);
      }
      if (t == 2 * 130) {
        ShowRec(ctx, base, cart->recs[0], false);
        g_room.cycle = 4, g_room.cycle_t = 0;
      }
      break;
    case 4:  // 240 HCTP frames, then the shutter closes (syattar2, act 30)
      if (t == 2 * 240) {
        if (sh->recs.size() > 1) {
          ShowRec(ctx, base, sh->recs[1], true);
          PlayRec(ctx, base, sh->recs[1], kShutterClose);
        }
        g_room.cycle = 5, g_room.cycle_t = 0;
      }
      break;
    case 5:  // closed: the shutter and the cart back as they were
      if (t == 2 * 31) {
        if (sh->recs.size() > 1) ShowRec(ctx, base, sh->recs[1], false);
        ShowRec(ctx, base, sh->recs[0], true);
        PlayRec(ctx, base, sh->recs[0], kShutterRest);
        g_room.cart_z = cart->z;
        for (int rec : cart->recs) {
          ShowRec(ctx, base, rec, true);
          MoveRec(ctx, base, rec, cart->x, cart->y, cart->z, 0, cart->ry, 0);
        }
        PlayRec(ctx, base, cart->recs[0], kCratesIdle);
        RestoreCollision(base, *cart);
        g_room.cycle = 0;
        REXLOG_INFO("hctp: the shutter is down - the button works again");
      }
      break;
  }
}

// -- Valves and steam (HCTP CGmSwitchOnOffParticle 210 / 215, particles
// 32 / 34): the action button at a valve opens it (12036) and its pipe lets
// out steam, again closes it (12037); at most a toggle every 121 frames,
// off by itself after 250 (HCTP frames). Whoever is in the jet is burnt
// (12047). Valve 210 also brings the barrel stack down.
Steam* SteamOf(const Prop& valve) {
  for (Steam& st : g_room.steam)
    if (st.valve == valve.hid) return &st;
  return nullptr;
}

void SetSteam(PPCContext& ctx, uint8_t* base, Steam& st, bool on) {
  st.on = on;
  st.t = 0;
  if (!on)
    for (Prop& p : g_room.props)
      if (p.kind == "puff" && p.hid == st.pid)
        for (int rec : p.recs) MoveRec(ctx, base, rec, p.x, 1000.0f, p.z);
  REXLOG_INFO("hctp: steam {} {}", st.pid, on ? "on" : "off");
}

void TurnValve(PPCContext& ctx, uint8_t* base, uint32_t f, int i, Prop& v) {
  if (v.cooldown > 0) return;
  v.cooldown = 2 * 121;
  Steam* st = SteamOf(v);
  const bool open = v.state == 0;
  if (f) JumpAct(ctx, base, f, open ? kValveOpen : kValveClose);
  PlayProp(ctx, base, v, open ? kValveOpen : kValveClose, 0, 1);
  REXLOG_INFO("hctp: fighter {} {} {}", i, open ? "opens" : "closes", v.name);
  v.state = open ? 1 : 0;
  if (st) {
    if (open) {
      st->on = false;
      st->t = -2 * 20;  // (the steam comes as the valve turns)
    } else if (st->on) {
      SetSteam(ctx, base, *st, false);
    }
  }
  if (open && v.hid == 210)
    for (Prop& p : g_room.props)
      if (p.hid == 218 && p.state == 0) KnockOver(ctx, base, p, 0);
}

void UpdateSteam(PPCContext& ctx, uint8_t* base) {
  for (Prop& v : g_room.props)
    if (v.kind == "valve" && v.cooldown > 0) --v.cooldown;
  for (Steam& st : g_room.steam) {
    Prop* v = nullptr;
    for (Prop& p : g_room.props)
      if (p.kind == "valve" && p.hid == st.valve) v = &p;
    if (!v || v->state == 0) continue;
    if (!st.on) {
      if (++st.t >= 0) SetSteam(ctx, base, st, true);
      continue;
    }
    if (++st.t > 2 * 250) {  // off by itself
      SetSteam(ctx, base, st, false);
      v->state = 0;
      continue;
    }
    // the puffs: along the jet, each its own way along, turning
    if (g_log && st.t % 60 == 1)
      for (const Prop& p : g_room.props)
        if (p.kind == "puff" && p.hid == st.pid && !p.recs.empty())
          if (const uint32_t e = Element(ctx, base, p.recs[0])) {
            const uint32_t o = Rd32(base + e + 100);
            REXLOG_INFO("hctp: {} elem at ({:.1f} {:.1f} {:.1f}) obj {:08X} shown {} active {} freeze {} draw {} {}",
                        p.name, RdF(base + e + 32), RdF(base + e + 36), RdF(base + e + 40), o,
                        o ? Rd32(base + o + 1124) : 0, o ? Rd32(base + o + 1120) : 0, o ? Rd32(base + o + 1140) : 0,
                        o ? Rd32(base + o + 476 + 16) : 0, o ? Rd32(base + o + 780 + 16) : 0);
          }
    int k = 0;
    for (Prop& p : g_room.props) {
      if (p.kind != "puff" || p.hid != st.pid) continue;
      const float u = std::fmod(float(st.t) / 40.0f + float(k) / 5.0f, 1.0f);
      const float d = u * st.len;
      for (int rec : p.recs)
        MoveRec(ctx, base, rec, st.x + st.dx * d, st.y - 1.5f * u, st.z + st.dz * d, 0,
                float(st.t) * 0.02f + float(k), float(st.t) * 0.07f + float(k) * 1.3f);
      ++k;
    }
    // burns
    for (int i = 0; i < 8; ++i) {
      const uint32_t f = Rd32(base + kChars + i * 4);
      if (!f) continue;
      FighterState& s = g_fs[i];
      if (s.burn_cd > 0) continue;
      const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
      const float rx = x - st.x, rz = z - st.z;
      const float along = rx * st.dx + rz * st.dz, across = std::fabs(-rx * st.dz + rz * st.dx);
      // (his body: from his feet y up to y - 18)
      if (along < 0 || along > st.len || across > st.hw + 2 || y < st.y - st.hh || y - 18.0f > st.y + st.hh) continue;
      JumpAct(ctx, base, f, kSteamBurn);
      s.burn_cd = 2 * 103;
      REXLOG_INFO("hctp: steam {} burns fighter {}", st.pid, i);
    }
  }
  for (FighterState& s : g_fs)
    if (s.burn_cd > 0) --s.burn_cd;
}

// -- The equipment crate (HCTP CGmShutObject 207): Irish-whip a wrestler
// into it and he is shut in (12034: attacker, victim and the crate on one
// spot); he climbs out at HCTP frame 420 (12043); ready again at 901.
constexpr uint32_t kWhipRun = 59;  // 2011: the whipped wrestler's run

void ShutIn(PPCContext& ctx, uint8_t* base, Prop& cr, uint32_t victim, int vi) {
  // the attacker: the nearest other wrestler
  uint32_t att = 0;
  float best = 1e9f;
  for (int i = 0; i < 8; ++i) {
    const uint32_t f = Rd32(base + kChars + i * 4);
    if (!f || f == victim) continue;
    const float dx = RdF(base + f + 288) - RdF(base + victim + 288);
    const float dz = RdF(base + f + 296) - RdF(base + victim + 296);
    if (dx * dx + dz * dz < best) best = dx * dx + dz * dz, att = f;
  }
  if (!att || best > 60.0f * 60.0f) return;
  JumpAct(ctx, base, att, kCrateIn, 0, 0);
  JumpAct(ctx, base, victim, kCrateIn, 0, 1);
  PlaceFighter(base, att, cr.x, cr.y, cr.z, cr.ry);
  PlaceFighter(base, victim, cr.x, cr.y, cr.z, cr.ry);
  PlayProp(ctx, base, cr, kCrateIn, 0, 2);
  g_room.crate = 1, g_room.crate_t = 0, g_room.crate_victim = victim;
  REXLOG_INFO("hctp: fighter {} is shut in {}", vi, cr.name);
}

void UpdateCrate(PPCContext& ctx, uint8_t* base) {
  Prop* cr = FindKind("crate");
  if (!cr || g_room.crate == 0) return;
  const int t = ++g_room.crate_t;
  const uint32_t v = g_room.crate_victim;
  if (g_room.crate == 1) {
    // inside: he stays on the act's last pose until he climbs out
    const int n = Frames(kCrateIn, 170);
    if (t > 20 && (Rd32(base + v + 212) != kCrateIn || RdF(base + v + 24) >= float(n - 2))) {
      JumpAct(ctx, base, v, kCrateIn, 0, 1, float(n - 2));
      PlaceFighter(base, v, cr->x, cr->y, cr->z, cr->ry);
    }
    if (t >= 2 * 420) {
      JumpAct(ctx, base, v, kCrateOut, 0, 0);
      PlaceFighter(base, v, cr->x, cr->y, cr->z, cr->ry);
      PlayProp(ctx, base, *cr, kCrateOut, 0, 1);
      g_room.crate = 2;
      REXLOG_INFO("hctp: the crate opens - he climbs out");
    }
  } else if (t >= 2 * 901) {
    g_room.crate = 0;
  }
}

// -- Off the cage top (HCTP GapIn_BackOpenSpace, from the strike button):
// up on the cage (y <= -28) near its front or east edge, X while walking
// toward the edge puts him on the edge (HCTP SetGapPos) and he jumps down
// (12572). After the roof broke, X toward the hole steps in (12092) and he
// drops into the cage. 2011's facing: an act's local +z (and the way he
// faces) is (sin yaw, cos yaw) in the world.
bool EdgeJump(PPCContext& ctx, uint8_t* base, uint32_t f, int i, float x, float y, float z) {
  if (y > -28.0f || x < 100.0f || x > 166.0f || z < -505.0f || z > -452.0f) return false;
  const float yaw = RdF(base + f + 276), fx = std::sin(yaw), fz = std::cos(yaw);
  // the roof hole (both halves gone): step in
  for (const Prop& r : g_room.props)
    if (r.kind == "roof" && r.state && InBox(r, x, z, 6.0f) && !InBox(r, x, z, 0.0f)) {
      const float cx = (r.box[0] + r.box[1]) / 2 - x, cz = (r.box[4] + r.box[5]) / 2 - z;
      if ((cx * fx + cz * fz) / std::max(0.01f, std::sqrt(cx * cx + cz * cz)) < 0.5f) continue;
      JumpAct(ctx, base, f, kStepIn);
      PlaceFighter(base, f, std::clamp(x, r.box[0] + 1, r.box[1] - 1), y, std::clamp(z, r.box[4] + 1, r.box[5] - 1), yaw);
      g_room.drop = 60;
      REXLOG_INFO("hctp: fighter {} steps into the hole", i);
      return true;
    }
  // the front edge (z -460, jump toward +z) and the east edge (x 160, toward +x)
  // (his facing goes in before the act - its root motion turns with it -
  // his place after it, as the T22 snap does)
  if (z > -466.0f && fz > 0.7f) {
    WrF(base + f + 276, 0.0f);
    JumpAct(ctx, base, f, kJumpDown);
    PlaceFighter(base, f, x, y, -463.0f, 0.0f);
  } else if (x > 154.0f && fx > 0.7f) {
    WrF(base + f + 276, 1.5707963f);
    JumpAct(ctx, base, f, kJumpDown);
    PlaceFighter(base, f, 157.0f, y, z, 1.5707963f);
  } else {
    return false;
  }
  REXLOG_INFO("hctp: fighter {} jumps down off the cage (yaw before {:.2f})", i, yaw);
  return true;
}

void Fighters(PPCContext& ctx, uint8_t* base) {
  for (int i = 0; i < 8; ++i) {
    const uint32_t f = Rd32(base + kChars + i * 4);
    if (!f) continue;
    FighterState& s = g_fs[i];
    if (s.fighter != f) s = {f, Rd32(base + f + 212), -1, 0, 0};
    const uint32_t m = Rd32(base + f + 212);
    const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
    const uint8_t touched = base[f + 455];
    // thrown / whipped / running into a stack, railing or the lead pipes
    if (m != kWalkBackstage)
      for (Prop& p : g_room.props)
        if ((p.kind == "stack" || p.kind == "throw") && p.state == 0 && touched == p.attr) KnockOver(ctx, base, p, f);
    // a knock-down on the roof
    // (lying: 0 here - backstage standing is 20500 - or 1000)
    const bool down = m == 0 || m == kLying, was_down = s.motion == 0 || s.motion == kLying;
    if (down && !was_down)
      for (Prop& r : g_room.props)
        if (r.kind == "roof" && r.state == 0 && OnRoofHalf(r, x, y, z)) {
          if (++r.count >= (g_roof_once ? 1 : 2))
            BreakRoof(ctx, base);
          else
            REXLOG_INFO("hctp: the cage roof shakes (fighter {} down at {:.0f} {:.0f} {:.0f})", i, x, y, z);
          break;
        }
    // the cage walls: X next to a wall grabs it, mashing X tears it down
    // (the input object's +52 holds the buttons pressed this frame - X is 0x80;
    // +68 gets them two frames later, when the strike X starts is already on)
    const uint32_t input = svr2011::FighterInput(f);
    const bool x_pressed = input && (Rd32(base + input + 52) & kButtonX);
    if (g_log && i == 0 && input) {
      static std::string last;
      std::string hex;
      for (uint32_t at = 48; at < 112; ++at) hex += fmt::format("{}{:02X}", at % 4 ? "" : " ", base[input + at]);
      if (hex != last && (Rd32(base + input + 52) & 0xFFFF)) REXLOG_INFO("hctp: fighter 0 input +48:{} motion {}", hex, m);
      last = hex;
    }
    // (X is the strike button: the press has already started a strike this
    // frame, so it's last frame's motion that must be the walk; the grab
    // replaces the strike)
    bool used = false;
    if (s.wall < 0 && x_pressed && s.motion == kWalkBackstage) {
      // a green button or a valve (before the cage walls: the ground button
      // is within reach of the east wall)
      for (Prop& p : g_room.props) {
        if (used || !InZone(p, x, y, z)) continue;
        if (p.kind == "switch") PressSwitch(ctx, base, f, i, p), used = true;
        if (p.kind == "valve") TurnValve(ctx, base, f, i, p), used = true;
      }
      for (size_t j = 0; j < g_room.props.size() && !used && !g_no_grab; ++j) {
        const Prop& w = g_room.props[j];
        if (w.kind != "wall" || w.state || std::fabs(y) > 5.0f) continue;
        // (next to the panel: within 8 of its collision)
        if (InBox(w, x, z, 8.0f)) {
          s.wall = int(j);
          s.presses = 0;
          s.idle = 0;
          JumpAct(ctx, base, f, kWallGrab);
          PlayProp(ctx, base, w, kWallGrab, 0, 1);
          REXLOG_INFO("hctp: fighter {} grabs {}", i, w.name);
          used = true;
        }
      }
      if (!used) used = EdgeJump(ctx, base, f, i, x, y, z);
    } else if (s.wall >= 0) {
      if (g_log && s.idle % 60 == 0)
        REXLOG_INFO("hctp: fighter {} holds {}: motion {} presses {} idle {}", i, g_room.props[size_t(s.wall)].name, m,
                    s.presses, s.idle);
      // holding the wall (HCTP: shaking it while the button is mashed; he
      // lets go 180 frames after the last press; ~11 presses tear it down)
      ++s.idle;
      if (x_pressed) {
        ++s.presses;
        s.idle = 0;
      }
      if (m != kWallGrab && m != kWalkBackstage) {
        s.wall = -1;  // knocked off
      } else if (s.presses >= 11) {
        JumpAct(ctx, base, f, kWallBreak);
        BreakWall(ctx, base, s.wall);
        REXLOG_INFO("hctp: fighter {} tears {} down", i, g_room.props[size_t(s.wall)].name);
        s.wall = -1;
      } else if (s.idle > 180) {
        JumpAct(ctx, base, f, kWallRelease);  // let go (HCTP 12060)
        REXLOG_INFO("hctp: fighter {} lets go of {}", i, g_room.props[size_t(s.wall)].name);
        s.wall = -1;
      } else if (m != kWallGrab) {
        JumpAct(ctx, base, f, kWallGrab);  // (the shake again: he keeps hold)
        PlayProp(ctx, base, g_room.props[size_t(s.wall)], kWallGrab, 0, 1);
      }
    }
    // whipped into the equipment crate
    if (m == kWhipRun && g_room.crate == 0)
      if (Prop* cr = FindKind("crate"); cr && InBox(*cr, x, z, 2.0f) && std::fabs(y) < 5.0f) ShutIn(ctx, base, *cr, f, i);
    // (his motion now: an act started here this frame counts as last frame's)
    s.motion = Rd32(base + f + 212);
  }
}

// -- Hits through the cage fence (HCTP CGmWireNetting::CheckChar +
// GmAtariCheck): a wrestler inside the cage and one outside it can't hit
// or grab each other while the wall between them stands. Room 30's cage:
// x 100..160, z -500..-460; the east (x 160) and front (z -460) sides are
// the two panel pairs; west and back are solid walls. Wrestlers up high
// (y < -20: the roof, the walkway) are not in either cell.
int CageCell(uint32_t f, uint8_t* base, int* side) {
  const float x = RdF(base + f + 288), y = RdF(base + f + 292), z = RdF(base + f + 296);
  if (y < -20.0f) return 0;
  if (x > 100 && x < 160 && z > -500 && z < -460) return 1;
  if (x < 75 || x > 185 || z < -525 || z > -435) return 0;
  *side = x >= 160 ? 0 : z >= -460 ? 1 : 2;  // east panels, front panels, a solid side
  return 2;
}

bool SideOpen(int side) {
  if (side == 2) return false;
  for (const Prop& w : g_room.props)
    if (w.kind == "wall" && w.has_box && (side == 0) == (w.box[0] > 155.0f) && w.state == 0) return false;
  return true;
}

// The fighter a hit collider belongs to (as sub_82238F28 finds it).
uint32_t ColliderFighter(PPCContext& ctx, uint8_t* base, uint32_t collider) {
  const auto saved = ctx;
  ctx.r3.u64 = collider;
  sub_8269DF80(ctx, base);
  ctx.r4.u64 = 0x43484152;  // 'CHAR'
  sub_8269DFB0(ctx, base);
  const uint32_t c = ctx.r3.u32;
  ctx = saved;
  return c ? Rd32(base + c + 24) : 0;
}

bool FenceBlocks(PPCContext& ctx, uint8_t* base, uint32_t comp, uint32_t target) {
  if (!HctpRoomActive() || !g_room.loaded || !comp || !target) return false;
  const uint32_t att = Rd32(base + comp + 448), vic = ColliderFighter(ctx, base, target);
  if (!att || !vic || att == vic) return false;
  int sa = 2, sv = 2;
  const int ca = CageCell(att, base, &sa), cv = CageCell(vic, base, &sv);
  if (g_log && ca && cv && ca != cv) REXLOG_INFO("hctp: a hit across the fence (cells {} -> {}, side {})", ca, cv, ca == 2 ? sa : sv);
  if (!ca || !cv || ca == cv) return false;
  if (SideOpen(ca == 2 ? sa : sv)) return false;
  if (g_log) {
    static int b = 0;
    static std::chrono::steady_clock::time_point t;
    if (Allow(b, t)) REXLOG_INFO("hctp: the fence stops a hit ({:08X} -> {:08X})", att, vic);
  }
  return true;
}

}  // namespace

// A fighter's attack collider meets a target (the hit broker sub_823AC758
// calls vt[8] = sub_82238F28, then vt[10] = sub_8222F678); a hit or grab
// through the standing cage fence is dropped here.
REX_EXTERN(__imp__sub_82238F28);
REX_HOOK_RAW(sub_82238F28) {
  if (FenceBlocks(ctx, base, ctx.r3.u32, ctx.r5.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82238F28(ctx, base);
}

REX_EXTERN(__imp__sub_8222F678);
REX_HOOK_RAW(sub_8222F678) {
  if (FenceBlocks(ctx, base, ctx.r3.u32, ctx.r4.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8222F678(ctx, base);
}

namespace svr2011 {

void HctpRoomUpdate(PPCContext& ctx, uint8_t* base) {
  if (!HctpRoomActive()) {
    g_room.loaded = false;
    return;
  }
  const uint32_t frame = Rd32(base + kMatchFrames);
  const auto& row = BackstageRows()[size_t(g_active_row)];
  if (!g_room.loaded || g_room.dir != row.dir || frame < g_room.last_frame) {
    if (!LoadRoom(row.dir)) return;
    for (auto& s : g_fs) s = {};
  }
  g_room.last_frame = frame;
  if (!Rd32(base + kGimmickMgr) || !Rd32(base + kStage)) return;
  SetUp(ctx, base);
  Fighters(ctx, base);
  UpdateCycle(ctx, base);
  UpdateSteam(ctx, base);
  if (g_crate_now && frame >= 300 && frame < 400 && g_room.crate == 0)
    if (Prop* cr = FindKind("crate"); cr && Rd32(base + kChars + 4)) ShutIn(ctx, base, *cr, Rd32(base + kChars + 4), 1);
  UpdateCrate(ctx, base);
  if (g_cart_now && frame >= 300 && g_room.cycle == 0 && g_room.cycle_t == 0)
    if (Prop* sw = FindKind("switch")) PressSwitch(ctx, base, Rd32(base + kChars + 4), 1, *sw);
  if (g_puff_now && frame >= 300)
    if (Prop* p = FindKind("puff"); p && Rd32(base + kChars)) {
      const uint32_t f0 = Rd32(base + kChars);
      for (int rec : p->recs)
        MoveRec(ctx, base, rec, RdF(base + f0 + 288), RdF(base + f0 + 292) - 24.0f, RdF(base + f0 + 296), 0,
                float(frame) * 0.03f, 0);
    }
  if (g_props_now && frame >= 300)
    for (Prop& p : g_room.props)
      if ((p.kind == "throw" || p.kind == "stack") && p.state == 0) KnockOver(ctx, base, p, 0);
  if (g_steam_now && frame >= 300 && frame < 302)
    for (Prop& p : g_room.props)
      if (p.kind == "valve" && p.state == 0) TurnValve(ctx, base, 0, -1, p);
  if (g_roof_now && frame >= 300 &&
      std::any_of(g_room.props.begin(), g_room.props.end(), [](const Prop& p) { return p.kind == "roof" && !p.state; }))
    BreakRoof(ctx, base);
  DropIn(base);
  for (Prop& p : g_room.props) {
    if (p.kind == "stack" || p.kind == "throw") UpdateFall(ctx, base, p);
    if (p.kind == "wall") UpdateWall(ctx, base, p);
  }
}

}  // namespace svr2011
