// WWE SmackDown vs. Raw 2011 - test aid: the solved pose of a motion, for the
// Mod Maker's animation viewer to be checked against (Animations rework,
// phase B).
//
// SVR2011_TEST_POSE_DUMP=<motion>[,<motion>...]: while a character plays one
// of those motions - its newest motion player (ch+3248 + 832 * ch[3232]: id
// +598 u16, bit 15 mirrored; fighter +212 is only a request) - every IK solve
// (sub_823A4DD8, the hook in small_rig.cpp) writes one line to
// SVR2011_TEST_POSE_DUMP_FILE (default pose_dump.txt in the working folder):
//   F <match frame> P <person> M <motion> S <pass> Rq <fighter +212>
//   Q <players sampled (weight >= 0.001), the newest first: id x y mirrored time weight ...>
//   A <fighter pos +288> E <fighter Euler +272>
//   R <pose root +1760+64> O <origin Euler +1760+112> <origin pos +1760+128> Y <extra yaw +1760+472>
//   B <33 parts: the local matrix each part pointer (ch+1336+4i) holds - 3 rotation rows and the
//      translation row, 12 floats, row-vector form; "-" for none>
// The parts (0x82DA0CF0): root koshi mune kubi atama, l_sakotsu ninoude kote te, r_ the same, l_momo sune
// ashi tsumasaki, r_ the same, then the twist / helper bones (27/28 l/r_kote_x, 31/32 kote_x2).
// Lines are gathered here and appended by a worker thread (never on the game thread). Unset: nothing runs.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <rex/logging.h>
#include <rex/ppc.h>

namespace svr2011 {
namespace {

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
float RdF(const uint8_t* p) {
  const uint32_t v = Rd32(p);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
bool Pointer(uint32_t a) { return a >= 0x40000000 && a < 0xC0000000 && !(a & 3); }

constexpr uint32_t kPose = 1760, kParts = 1336, kMatchFrames = 0x82E3CD0C;
constexpr uint32_t kPlayers = 3248, kPlayerSize = 832, kNewest = 3232;  // (motion players; the newest one's index)

uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

struct Config {
  bool on = false, all = false, camera = false;
  std::set<uint32_t> motions;
  std::string file = "pose_dump.txt";
};
const Config& Cfg() {
  static const Config c = [] {
    Config c;
    const char* e = std::getenv("SVR2011_TEST_POSE_DUMP");
    if (!e) return c;
    if (!std::strncmp(e, "all", 3)) c.all = true;  // (every motion: an entrance's, whose id isn't known up front)
    for (const char* p = e; *p;) {
      char* end = nullptr;
      const unsigned long n = std::strtoul(p, &end, 10);
      if (end == p) break;
      c.motions.insert(uint32_t(n));
      for (p = end; *p == ',' || *p == ' ';) ++p;
    }
    if (const char* f = std::getenv("SVR2011_TEST_POSE_DUMP_FILE")) c.file = f;
    c.on = !c.motions.empty() || c.all;
    c.camera = std::getenv("SVR2011_TEST_CAMERA_DUMP") != nullptr;
    return c;
  }();
  return c;
}

std::mutex g_mutex;
std::string g_buffer;
uint32_t g_tick = 0;  // (camera updates seen: the frame both kinds of line are matched by)

void Flush(bool force) {
  std::string out;
  {
    std::lock_guard lock(g_mutex);
    if (g_buffer.empty() || (!force && g_buffer.size() < (256u << 10))) return;
    out.swap(g_buffer);
  }
  std::thread([out = std::move(out), file = Cfg().file] {
    static std::mutex file_mutex;
    std::lock_guard lock(file_mutex);
    if (FILE* f = std::fopen(file.c_str(), "ab")) {
      std::fwrite(out.data(), 1, out.size(), f);
      std::fclose(f);
      REXLOG_INFO("[svr2011] pose dump: {} bytes written to {}", out.size(), file);
    } else {
      REXLOG_INFO("[svr2011] pose dump: can't open {}", file);
    }
  }).detach();
}

void Vec(std::string& s, const uint8_t* p, int n) {
  char b[24];
  for (int i = 0; i < n; ++i) {
    std::snprintf(b, sizeof b, " %.4f", RdF(p + 4 * i));
    s += b;
  }
}

}  // namespace

// " A <fighter pos> E <Euler> R <pose root> O <origin> Y <yaw> B <33 parts>": a pose line's end, for one character
void Tail(uint8_t* base, uint32_t ch, uint32_t fighter, std::string& s, bool parts = true) {
  const uint8_t* P = base + ch + kPose;
  s += " A";
  Vec(s, base + fighter + 288, 3);
  s += " E";
  Vec(s, base + fighter + 272, 3);
  s += " R";
  Vec(s, P + 64, 3);
  s += " O";
  Vec(s, P + 112, 3);
  Vec(s, P + 128, 3);
  s += " Y";
  Vec(s, P + 472, 1);
  if (!parts) return;  // (the camera dump's entrance lines: a remembered character's part pointers can be stale)
  s += " B";
  for (int i = 0; i < 33; ++i) {
    const uint32_t m = Rd32(base + ch + kParts + 4 * i);
    if (!Pointer(m)) {
      s += " -";
      continue;
    }
    Vec(s, base + m, 3);        // row 0
    Vec(s, base + m + 16, 3);   // row 1
    Vec(s, base + m + 32, 3);   // row 2
    Vec(s, base + m + 48, 3);   // translation
  }
}

struct Seen {
  uint32_t ch = 0, fighter = 0;
};
Seen g_seen[16];  // (each person's character as the solves last saw it: the camera dump's entrance lines)

// Called by the IK hook (small_rig.cpp) after each solve: ch = the solved character, pass = r4 (0 / 1).
void PoseDump(uint8_t* base, uint32_t ch, uint32_t pass) {
  const Config& c = Cfg();
  if (!c.on || !Pointer(ch)) return;
  static bool said = false;
  if (!said) {
    said = true;
    REXLOG_INFO("[svr2011] pose dump: on, {} motion(s), into {}", c.motions.size(), c.file);
  }
  // the fighter: the character node's 3rd parent (+16), its +32 (as small_rig.cpp)
  uint32_t n = ch;
  for (int i = 0; i < 3 && Pointer(n); ++i) n = Rd32(base + n + 16);
  static int misses = 0;
  if (!Pointer(n)) {
    if (misses++ < 5) REXLOG_INFO("[svr2011] pose dump: ch {:08X} - no 3rd parent", ch);
    return;
  }
  const uint32_t fighter = Rd32(base + n + 32);
  if (!Pointer(fighter) || std::memcmp(base + fighter + 4, "CHAR", 4)) {
    if (misses++ < 5) REXLOG_INFO("[svr2011] pose dump: ch {:08X} - node {:08X} +32 {:08X} isn't a CHAR", ch, n, fighter);
    return;
  }
  uint32_t newest = Rd32(base + ch + kNewest);
  const uint32_t person = Rd32(base + fighter + 1156);
  static int notes = 0, range_notes = 0;
  if (newest >= 6) {  // (-1 in matches: the sampled player with the most weight)
    float best = 0.001f;
    for (uint32_t i = 0; i < 6; ++i)
      if (const float w = RdF(base + ch + kPlayers + kPlayerSize * i + 528); w >= best) best = w, newest = i;
    if (newest >= 6) {
      if (range_notes++ < 4) {
        std::string raw;
        for (uint32_t i = 0; i < 6; ++i) {
          const uint8_t* pl = base + ch + kPlayers + kPlayerSize * i;
          char t[48];
          std::snprintf(t, sizeof t, " [%u: id %u w %g]", i, Rd16(pl + 598), RdF(pl + 528));
          raw += t;
        }
        REXLOG_INFO("[svr2011] pose dump: person {} ch {:08X} - no player sampled (newest index {}):{}", person, ch, Rd32(base + ch + kNewest), raw);
      }
      return;
    }
  }
  const uint8_t* player = base + ch + kPlayers + kPlayerSize * newest;
  const uint32_t motion = Rd16(player + 598) & 0x7FFF;
  {  // (what the players hold, each time it changes - so a wrong field shows)
    static uint32_t seen[16] = {};
    const uint32_t key = motion << 8 | player[600];
    if (person < 16 && seen[person] != key && notes < 200) {
      seen[person] = key, ++notes;
      std::string all;
      for (uint32_t i = 0; i < 6; ++i) {
        const uint8_t* pl = base + ch + kPlayers + kPlayerSize * i;
        char t[64];
        std::snprintf(t, sizeof t, " [%u: %u x%u y%u t%.1f w%.2f]", i, Rd16(pl + 598), pl[600], pl[601], RdF(pl + 584), RdF(pl + 528));
        all += t;
      }
      REXLOG_INFO("[svr2011] pose dump: person {} newest {} plays {} (request {}):{}", person, newest, motion, Rd32(base + fighter + 212), all);
    }
  }
  static uint32_t playing[16] = {};
  const uint32_t pi = person < 16 ? person : 15;
  g_seen[pi] = {ch, fighter};
  if (!c.all && !c.motions.count(motion)) {
    if (playing[pi]) {
      REXLOG_INFO("[svr2011] pose dump: person {} motion {} ended", person, playing[pi]);
      playing[pi] = 0, Flush(true);  // (that motion ended: written out)
    }
    return;
  }
  if (playing[pi] != motion) REXLOG_INFO("[svr2011] pose dump: person {} motion {} (ch {:08X}, fighter {:08X})", person, motion, ch, fighter);
  playing[pi] = motion;
  const uint8_t* P = base + ch + kPose;
  std::string s;
  char b[96];
  std::snprintf(b, sizeof b, "F %u T %u P %u M %u S %u Rq %u Q", Rd32(base + kMatchFrames), g_tick, person, motion, pass,
                Rd32(base + fighter + 212));
  s += b;
  for (uint32_t k = 0; k < 6; ++k) {  // the newest first, then the others still blended in
    const uint32_t i = (newest + k) % 6;
    const uint8_t* pl = base + ch + kPlayers + kPlayerSize * i;
    const float w = RdF(pl + 528);
    if (k && !(w >= 0.001f)) continue;
    const uint16_t id = Rd16(pl + 598);
    std::snprintf(b, sizeof b, " %u %u %u %u %.2f %.3f", id & 0x7FFF, pl[600], pl[601], id >> 15, RdF(pl + 584), w);
    s += b;
  }
  Tail(base, ch, fighter, s);
  s += '\n';
  {
    std::lock_guard lock(g_mutex);
    g_buffer += s;
  }
  Flush(false);
}

// SVR2011_TEST_CAMERA_DUMP (with SVR2011_TEST_POSE_DUMP, into the same file): after each camera update
// (sub_8239C5C0: cam+80 = the fov in degrees, from +240; +16..+111 = the pose copied from +352..+447) one line
//   C <tick> F <match frame> <cam> fov <+80> z <+240> zm <+1320> ms <wall clock> V <+16..+111: 24 floats>
void CameraDump(uint8_t* base, uint32_t cam) {
  const Config& c = Cfg();
  if (!c.camera || !Pointer(cam)) return;
  ++g_tick;
  std::string s;
  char b[128];
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::system_clock::now().time_since_epoch()).count();  // (screenshots' file times -> ticks)
  std::snprintf(b, sizeof b, "C %u F %u %08X fov %.4f z %.4f zm %u ms %lld V", g_tick, Rd32(base + kMatchFrames), cam,
                RdF(base + cam + 80), RdF(base + cam + 240), Rd32(base + cam + 1320), ms);
  s += b;
  Vec(s, base + cam + 16, 24);
  s += '\n';
  for (uint32_t p = 0; p < 2; ++p)  // (an entrance doesn't solve: the first two people's parts as they are now)
    if (Pointer(g_seen[p].ch) && Pointer(g_seen[p].fighter)) {
      std::snprintf(b, sizeof b, "E %u P %u", g_tick, p);
      s += b;
      Tail(base, g_seen[p].ch, g_seen[p].fighter, s, false);
      s += '\n';
    }
  {
    std::lock_guard lock(g_mutex);
    g_buffer += s;
  }
  Flush(g_tick % 30 == 0);  // (often: a test that ends by closing the game keeps what it saw)
}

}  // namespace svr2011

// (the camera's update: the dump after it)
REX_EXTERN(__imp__sub_8239C5C0);
REX_HOOK_RAW(sub_8239C5C0) {
  const uint32_t cam = ctx.r3.u32;
  __imp__sub_8239C5C0(ctx, base);
  svr2011::CameraDump(base, cam);
}
