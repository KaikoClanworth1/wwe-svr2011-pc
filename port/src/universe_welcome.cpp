// The WWE Universe welcome ("EXPERIENCE THE WWE LIKE NEVER BEFORE... ACCEPT") kept as seen (universe_welcome.h).
//
// Player report: the welcome came back at every start. It shows while a flag byte of the save is 1 (SaveData.dat
// offset 0x7DBC7; the in-memory save, Rd32(0x82E407B4), drops the file's 0x18-byte header: save + 0x7DBAF). The game
// makes the save before the welcome (a new player: "create a new save game?") and ACCEPT clears the flag in memory
// only - the file gets it with the next save the game writes (a setting kept with "save these settings? YES", the
// Save Data Manager, ...). Quitting before that: the welcome again, start after start. Found 15 Oct by diffing the
// save before ACCEPT and after a manual save, then setting each changed byte alone in the before-file.
// So: when the flag goes 1 -> 0 in memory and the file still has 1, that byte goes into the file at once (its
// checksum redone: big-endian u16 at 0x1A = the sum of the bytes from 0x1C, mod 2^16). Nothing else in the file
// changes; the game's working copy in Saves\.mount gets it too when it isn't a hard link of the save. A write that fails is logged and written to <user data>\save_write_failed.txt (the launcher shows it).
#include "universe_welcome.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <rex/logging.h>

namespace svr2011 {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kSavePtr = 0x82E407B4;
constexpr uint32_t kHeader = 0x18;        // (the file's header the in-memory save leaves out)
constexpr uint32_t kFlag = 0x7DBC7;       // file offset: 1 = show the Universe welcome
constexpr uint32_t kSum = 0x1A, kSumFrom = 0x1C;

fs::path g_save;     // Saves\SaveData.dat
fs::path g_failed;   // <user data>\save_write_failed.txt
int g_last = -1;     // the flag in memory last frame (-1: no save in memory)

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

void Failed(const std::string& why) {
  REXLOG_WARN("[svr2011] saves: the game can't write {} ({}) - nothing it saves is kept", g_save.string(), why);
  std::ofstream o(g_failed, std::ios::trunc);
  if (o)
    o << "The game couldn't write its save file:\n" << g_save.string() << "\n(" << why
      << ")\nCheck that the Saves folder isn't read-only, blocked by Windows Controlled folder access or kept "
         "online-only by OneDrive.\n";
}

// The save files that hold SaveData.dat: Saves\SaveData.dat, and the game's working copy in Saves\.mount\SaveData.dat\
// when it isn't the same file (no hard links on that drive: the copy is moved back over the save at close).
std::vector<fs::path> SaveFiles() {
  std::vector<fs::path> out;
  std::error_code ec;
  if (fs::is_regular_file(g_save, ec)) out.push_back(g_save);
  const fs::path mount = g_save.parent_path() / ".mount" / g_save.filename();
  for (const auto& e : fs::directory_iterator(mount, ec)) {
    if (ec || !e.is_regular_file(ec)) continue;
    if (!out.empty() && fs::equivalent(e.path(), out[0], ec)) continue;
    out.push_back(e.path());
  }
  return out;
}

// `n` bytes at file offset `at` set to `v` in every save file that doesn't have them yet - in place, the checksum
// redone (BE u16 at 0x1A = the sum of the bytes from 0x1C). `only_if_zero`: a file whose bytes there aren't all 0 is
// left as it is (the game wrote its own). True: some file changed.
bool PatchSave(uint32_t at, const uint8_t* v, uint32_t n, bool only_if_zero, const char* what) {
  bool changed = false;
  for (const fs::path& file : SaveFiles()) {
    std::vector<char> d;
    {
      std::ifstream in(file, std::ios::binary);
      if (!in) {
        Failed("can't be read");
        continue;
      }
      d.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    if (d.size() < at + n) continue;
    if (std::memcmp(d.data() + at, v, n) == 0) continue;  // (already there)
    bool zero = true;
    for (uint32_t i = 0; i < n; ++i) zero &= d[at + i] == 0;
    if (only_if_zero && !zero) continue;
    uint32_t sum = 0;
    for (size_t i = kSumFrom; i < d.size(); ++i) sum += uint8_t(d[i]);
    if ((uint32_t(uint8_t(d[kSum])) << 8 | uint8_t(d[kSum + 1])) != (sum & 0xFFFF)) {
      REXLOG_WARN("[svr2011] saves: {}'s checksum isn't the one known - left alone", file.string());
      continue;
    }
    for (uint32_t i = 0; i < n; ++i) sum = sum - uint8_t(d[at + i]) + v[i], d[at + i] = char(v[i]);
    d[kSum] = char(sum >> 8 & 0xFF), d[kSum + 1] = char(sum & 0xFF);
    std::fstream f(file, std::ios::binary | std::ios::in | std::ios::out);  // (in place: a hard link stays one)
    if (!f) {
      Failed("can't be opened for writing");
      continue;
    }
    f.seekp(kSum);
    f.write(d.data() + kSum, 2);
    f.seekp(at);
    f.write(d.data() + at, n);
    f.flush();
    if (!f) {
      Failed("the write failed");
      continue;
    }
    REXLOG_INFO("[svr2011] saves: {} kept in {}", what, file.string());
    changed = true;
  }
  if (changed) {
    std::error_code ec;
    fs::remove(g_failed, ec);
  }
  return changed;
}

}  // namespace

void InstallUniverseWelcome(const fs::path& saves, const fs::path& user_data) {
  g_save = saves / "SaveData.dat";
  g_failed = user_data / "save_write_failed.txt";
  // Can the game write its saves at all? A Saves folder it can't write (read-only, Controlled folder access, OneDrive
  // online-only) makes the save's mount fail without a word: the game says "Load Failed", starts from a blank save -
  // the Universe welcome every start, nothing kept. Tried once at start: a small file made and removed in Saves.
  std::error_code ec;
  fs::create_directories(saves, ec);
  const fs::path probe = saves / ".write_test.tmp";
  bool ok = false;
  {
    std::ofstream o(probe, std::ios::trunc);
    ok = o && (o << "svr2011", o.flush(), bool(o));
  }
  fs::remove(probe, ec);
  if (!ok) {
    Failed("the Saves folder can't be written");
  } else if (fs::exists(g_failed, ec)) {
    fs::remove(g_failed, ec);
    REXLOG_INFO("[svr2011] saves: {} can be written again", saves.string());
  }
}

void UniverseWelcomeTick(uint8_t* base) {
  if (g_save.empty()) return;
  const uint32_t save = Rd32(base + kSavePtr);
  const int now = save ? base[save + kFlag - kHeader] : -1;
  if (now == 0 && g_last == 1) {  // (a save being loaded: the default one first, flag 1, then the file's)
    const uint8_t zero = 0;
    PatchSave(kFlag, &zero, 1, false, "the Universe welcome accepted");
  }
  g_last = now;
}

}  // namespace svr2011
