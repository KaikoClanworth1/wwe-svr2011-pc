// WWE SmackDown vs. Raw 2011 - test aid: the solved pose of a motion, for the
// Mod Maker's animation viewer to be checked against (Animations rework,
// phase B).
//
// SVR2011_TEST_POSE_DUMP=<motion>[,<motion>...]: while a fighter plays one of
// those motions (fighter +212), every IK solve (sub_823A4DD8, the hook in
// small_rig.cpp) writes one line to SVR2011_TEST_POSE_DUMP_FILE (default
// pose_dump.txt in the working folder):
//   F <match frame> P <person> M <motion> S <pass> A <fighter pos +288> E <fighter Euler +272>
//   R <pose root +1760+64> O <origin Euler +1760+112> <origin pos +1760+128> Y <extra yaw +1760+472>
//   B <33 parts: the local matrix each part pointer (ch+1336+4i) holds - 3 rotation rows and the
//      translation row, 12 floats, row-vector form; "-" for none>
// The parts (0x82DA0CF0): root koshi mune kubi atama, l_sakotsu ninoude kote te, r_ the same, l_momo sune
// ashi tsumasaki, r_ the same, then the twist / helper bones (27/28 l/r_kote_x, 31/32 kote_x2).
// Lines are gathered here and appended by a worker thread (never on the game thread). Unset: nothing runs.

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

struct Config {
  bool on = false;
  std::set<uint32_t> motions;
  std::string file = "pose_dump.txt";
};
const Config& Cfg() {
  static const Config c = [] {
    Config c;
    const char* e = std::getenv("SVR2011_TEST_POSE_DUMP");
    if (!e) return c;
    for (const char* p = e; *p;) {
      char* end = nullptr;
      const unsigned long n = std::strtoul(p, &end, 10);
      if (end == p) break;
      c.motions.insert(uint32_t(n));
      for (p = end; *p == ',' || *p == ' ';) ++p;
    }
    if (const char* f = std::getenv("SVR2011_TEST_POSE_DUMP_FILE")) c.file = f;
    c.on = !c.motions.empty();
    return c;
  }();
  return c;
}

std::mutex g_mutex;
std::string g_buffer;

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
  const uint32_t motion = Rd32(base + fighter + 212), person = Rd32(base + fighter + 1156);
  static uint32_t playing[16] = {};
  const uint32_t pi = person < 16 ? person : 15;
  if (!c.motions.count(motion)) {
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
  std::snprintf(b, sizeof b, "F %u P %u M %u S %u A", Rd32(base + kMatchFrames), person, motion, pass);
  s += b;
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
  s += '\n';
  {
    std::lock_guard lock(g_mutex);
    g_buffer += s;
  }
  Flush(false);
}

}  // namespace svr2011
