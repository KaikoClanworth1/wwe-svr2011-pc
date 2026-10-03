// Superstar mods (arenas branch): up to 50 new playable characters from
// <game>/Mods/Superstars/<id>/ (manifest.txt: name=, short=, base=<character
// id>, song=, movie=; ch.pac: a character model pac; a "disabled" file turns
// one off). docs/SUPERSTAR_MODS.md has the whole story.
//
// The game's character database (from re_roster): ids 50..69 are the 20 DLC
// slots (real DLC uses 51..58) and many disc ids hold blank placeholder
// records; each has a record, profile and save room but nobody in it. A
// superstar mod takes one of those ids (kPool, kept in slots.txt):
// - files: the model pac's EMD entries are named "%06d%02d" (id, attire*10 +
//   kind); a copy with the slot's id in the names, and a pack of select
//   screen renders (SSFA/SSFB/SSFC "%04d" = attire*1000 + id) copied from
//   the base character's in DLC_HD.pac, both in Mods/SuperstarOverlay and
//   mounted with the game's own pac mount (sub_826A1780(path, 0)) once its
//   file system is up (end of sub_825953B0 / sub_82595428);
// - records: the base character's 260-byte roster record (0x82E407C0 +
//   index * 260, index from the u16 table at 0x82DB3610) and 1056-byte
//   profile (0x82E7C920 + index * 1056) copied to the slot, with the mod's
//   names, selectable (+221) and DLC (+257) set and its own id as "same
//   person" (+228); again after the loaders (sub_82B89E50, sub_82594C78), a
//   save load (sub_8257A218) and before every roster list is built
//   (sub_82736C68), since saves carry the records;
// - owned: DLC-flagged ids must be owned (sub_828C1288 / sub_82589198):
//   mods are.
// The select screen shows them in the EXTRA list (managers.cpp, the M tile).
#include "superstar_mods.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/pac.h"
#include "../modmaker/svrfmt/texture.h"
#include "music.h"
#include "user_movies.h"

namespace fs = std::filesystem;

namespace {

// The ids mods take (50): the free DLC slots (59-69; real DLC uses 51-58),
// then disc ids whose roster record is a blank placeholder ("0", loaded from
// CHAR/DAT, not selectable) with no model, select render, entrance or match
// data anywhere in the game's pacs. Each has a record, profile and save slot.
constexpr uint32_t kPool[] = {59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  111, 114,
                              121, 127, 128, 129, 130, 136, 141, 148, 149, 151, 152, 154, 155,
                              157, 162, 163, 167, 168, 172, 173, 181, 185, 189, 200, 202, 203,
                              204, 206, 207, 209, 213, 214, 220, 221, 223, 225, 227};
constexpr uint32_t kOwnId = 32, kOwnId2 = 218;  // u16 own id in the record
constexpr uint32_t kIdToIndex = 0x82DB3610;  // u16 per id
constexpr uint32_t kRecords = 0x82E407C0, kRecordSize = 260;
constexpr uint32_t kProfiles = 0x82E7C920, kProfileSize = 1056;
constexpr uint32_t kFullName = 34, kSecondName = 102, kShortName = 170, kNameLen = 32;
constexpr uint32_t kSelectable = 221, kDlc = 257, kSamePerson = 228;

struct Mod {
  std::string folder, name, short_name;
  uint32_t base = 0, slot = 0;
  std::string ch_guest, ssf_guest;  // the overlay files, as the game opens them
  std::string song;                 // its theme: a USER PLAYLIST name ("" = the base's)
  int movie = 0;                    // its entrance movie: a user movie id (0 = the base's)
};
std::vector<Mod> g_mods;
rex::memory::Memory* g_memory = nullptr;
fs::path g_game;
bool g_mounted = false;

uint32_t Rd16(const uint8_t* p) { return uint32_t(p[0]) << 8 | p[1]; }
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// A model pac's EMD entry names ("%06d%02d": character id, attire*10 + kind)
// get the slot's id, whichever character the pac was made for (the base's
// own or another one's). Same length: the directory is patched in place.
// (An EPK8 group header's count is in dwords: 4 per 16-byte entry.)
bool RenameModel(svrfmt::Bytes& d, uint32_t to) {
  if (d.size() < 0x4000 || std::memcmp(d.data(), "EPK8", 4)) return false;
  char b[8];
  std::snprintf(b, sizeof b, "%06u", to);
  int n = 0;
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!std::memcmp(&d[p], "\0\0\0\0", 4)) break;
    const bool emd = !std::memcmp(&d[p], "EMD ", 4);
    const uint32_t entries = (uint32_t(d[p + 4]) | uint32_t(d[p + 5]) << 8) / 4;
    p += 12;
    for (uint32_t k = 0; k < entries && p + 16 <= 0x4000; ++k, p += 16)
      if (emd && std::all_of(&d[p], &d[p + 8], [](uint8_t c) { return c >= '0' && c <= '9'; }))
        std::memcpy(&d[p], b, 6), ++n;
  }
  return n > 0;
}

// The base's select screen renders, renamed to the slot: an EPAC with
// SSFA / SSFB / SSFC.
bool BuildRenders(const fs::path& out, uint32_t base, uint32_t slot) {
  svrfmt::Bytes d;
  svrfmt::Epac src;
  if (!svrfmt::ReadFile((g_game / "pac" / "DLC_HD.pac").string(), d) || !svrfmt::EpacRead(d, src)) return false;
  svrfmt::Epac e;
  e.header = src.header;
  e.trailer = src.trailer;
  int n = 0;
  for (const auto& g : src.groups) {
    if (g.type != "SSFA" && g.type != "SSFB" && g.type != "SSFC") continue;
    svrfmt::EpacGroup og;
    og.type = g.type;
    for (const auto& en : g.entries) {
      const int key = std::atoi(en.name.c_str());
      if (key % 1000 != int(base)) continue;
      char nm[8];
      std::snprintf(nm, sizeof nm, "%04d", (key / 1000) * 1000 + int(slot));
      og.entries.push_back({nm, en.data});
      ++n;
    }
    if (!og.entries.empty()) e.groups.push_back(std::move(og));
  }
  if (!n) return false;
  svrfmt::Bytes o = svrfmt::EpacWrite(e);
  // (header +4: the directory's size - what the mount copies into the file
  // system's directory; DLC_HD's is far bigger)
  uint32_t toc = 0;
  for (const auto& g : e.groups) toc += 12 + 12 * uint32_t(g.entries.size());
  o[4] = uint8_t(toc), o[5] = uint8_t(toc >> 8), o[6] = uint8_t(toc >> 16), o[7] = uint8_t(toc >> 24);
  return svrfmt::WriteFile(out.string(), o);
}

// A mod's overlay file is rebuilt only when its source is newer.
bool Current(const fs::path& out, const fs::path& src) {
  std::error_code ec;
  return fs::exists(out, ec) && fs::last_write_time(out, ec) >= fs::last_write_time(src, ec);
}

// Each mod keeps its id (Mods/Superstars/slots.txt: "<folder>\t<id>"): the
// saves hold the records by id, so adding or removing a mod moves no one.
std::vector<std::pair<std::string, uint32_t>> ReadSlots(const fs::path& file) {
  std::vector<std::pair<std::string, uint32_t>> out;
  if (FILE* t = std::fopen(file.string().c_str(), "rb")) {
    char line[512];
    while (std::fgets(line, sizeof line, t)) {
      std::string l = line;
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      const size_t tab = l.rfind('\t');
      if (tab == std::string::npos) continue;
      const uint32_t id = uint32_t(std::atoi(l.c_str() + tab + 1));
      if (std::find(std::begin(kPool), std::end(kPool), id) != std::end(kPool)) out.push_back({l.substr(0, tab), id});
    }
    std::fclose(t);
  }
  return out;
}

// A mod's theme and entrance movie (manifest song= / movie=, files in its
// folder) go where USER PLAYLIST and USER MOVIES find them, as Community
// Creations downloads do: the song as "<Music>/<name>.<ext>" (a playlist named
// after the mod), the movie (a 320x320 .bik, the launcher's Movies tab makes
// them) as "<Custom Movies>/<name>.bik" with a user movie id.
std::string FileName(std::string s) {
  for (char& c : s)
    if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      c = '_';
  if (s.size() > 39) s.resize(39);  // (the entrance keeps 39 characters of the playlist name)
  return s;
}

bool CopyIfChanged(const fs::path& from, const fs::path& to) {
  std::error_code ec;
  if (fs::exists(to, ec) && fs::file_size(to, ec) == fs::file_size(from, ec)) return true;
  fs::create_directories(to.parent_path(), ec);
  return fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
}

void InstallMedia(Mod& m, const fs::path& folder, const std::string& song, const std::string& movie) {
  std::error_code ec;
  const std::string name = FileName(m.name);
  if (!song.empty() && fs::exists(folder / song, ec)) {
    const fs::path music = svr2011::UserMusicFolder();
    if (!music.empty() && CopyIfChanged(folder / song, music / (name + fs::path(song).extension().string())))
      m.song = name;
  }
  if (!movie.empty() && fs::exists(folder / movie, ec) && fs::path(movie).extension() == ".bik") {
    const fs::path movies = svr2011::UserMoviesFolder();
    const std::string file = name + ".bik";
    if (!movies.empty() && CopyIfChanged(folder / movie, movies / file)) {
      m.movie = svr2011::ReserveUserMovie(file);
      svr2011::FinishUserMovie(file);
    }
  }
  if (!m.song.empty() || m.movie)
    REXLOG_INFO("[svr2011] superstar mods: {}: theme \"{}\", movie {}", m.name, m.song, m.movie);
}

// The profile's entrance (profile +0x1C0, 284 bytes; re_entrance): music id
// u16 at +0x10 (254 = USER PLAYLIST, by the name at +0xCC, UTF-16BE), movie
// id u16 at +0x12. The base's motions and pyro stay (+0xC7 = 0: default).
constexpr uint32_t kEntrance = 0x1C0, kMusic = 0x10, kMovie = 0x12, kSongName = 0xCC, kUserPlaylist = 254;
void SetEntranceMedia(uint8_t* profile, const Mod& m) {
  uint8_t* e = profile + kEntrance;
  if (!m.song.empty()) {
    e[kMusic] = 0, e[kMusic + 1] = uint8_t(kUserPlaylist);
    for (uint32_t i = 0; i < 40; ++i) {
      const uint16_t c = i < m.song.size() && i < 39 ? uint8_t(m.song[i]) : 0;
      e[kSongName + 2 * i] = uint8_t(c >> 8), e[kSongName + 2 * i + 1] = uint8_t(c);
    }
  }
  if (m.movie) e[kMovie] = uint8_t(m.movie >> 8), e[kMovie + 1] = uint8_t(m.movie);
}

void LoadMods() {
  std::error_code ec;
  const fs::path dir = g_game / "Mods" / "Superstars";
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory() && fs::exists(e.path() / "ch.pac", ec) && !fs::exists(e.path() / "disabled", ec))
      folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  const fs::path overlay = g_game / "Mods" / "SuperstarOverlay";
  fs::create_directories(overlay, ec);
  auto slots = ReadSlots(dir / "slots.txt");
  const size_t known = slots.size();
  auto slot_of = [&](const std::string& folder) -> uint32_t {
    for (const auto& s : slots)
      if (s.first == folder) return s.second;
    for (uint32_t id : kPool) {
      bool taken = false;
      for (const auto& s : slots) taken |= s.second == id;
      if (taken) continue;
      slots.push_back({folder, id});
      return id;
    }
    return 0;
  };
  for (const auto& f : folders) {
    Mod m;
    m.folder = f.filename().string();
    m.name = m.short_name = m.folder;
    std::string song, movie;
    if (FILE* t = std::fopen((f / "manifest.txt").string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("name=", 0) == 0) m.name = l.substr(5);
        if (l.rfind("short=", 0) == 0) m.short_name = l.substr(6);
        if (l.rfind("base=", 0) == 0) m.base = uint32_t(std::atoi(l.c_str() + 5));
        if (l.rfind("song=", 0) == 0) song = l.substr(5);
        if (l.rfind("movie=", 0) == 0) movie = l.substr(6);
      }
      std::fclose(t);
    }
    if (m.base < 100 || m.base > 321) {
      REXLOG_WARN("[svr2011] superstar mods: {} has no base= character (100-321)", m.folder);
      continue;
    }
    InstallMedia(m, f, song, movie);
    m.slot = slot_of(m.folder);
    if (!m.slot) {
      REXLOG_WARN("[svr2011] superstar mods: only {} fit, {} left out (a line in slots.txt for a mod that is "
                  "gone can be removed)", std::size(kPool), m.folder);
      continue;
    }
    char nm[32];
    std::snprintf(nm, sizeof nm, "ch%03u.pac", m.slot);
    if (!Current(overlay / nm, f / "ch.pac")) {
      svrfmt::Bytes ch;
      if (!svrfmt::ReadFile((f / "ch.pac").string(), ch) || !RenameModel(ch, m.slot) ||
          !svrfmt::WriteFile((overlay / nm).string(), ch)) {
        REXLOG_WARN("[svr2011] superstar mods: {}: ch.pac is not a character model pac (EPK8 with EMD models)", m.folder);
        continue;
      }
    }
    m.ch_guest = std::string("smods:\\") + nm;
    std::snprintf(nm, sizeof nm, "ssf%03u.pac", m.slot);
    // (the renders depend on base=: rebuilt with the manifest)
    if (Current(overlay / nm, f / "manifest.txt") || BuildRenders(overlay / nm, m.base, m.slot))
      m.ssf_guest = std::string("smods:\\") + nm;
    else
      REXLOG_WARN("[svr2011] superstar mods: {}: base {} has no select render (not a playable superstar)", m.folder,
                  m.base);
    REXLOG_INFO("[svr2011] superstar mods: {} as id {} (from {})", m.name, m.slot, m.base);
    g_mods.push_back(std::move(m));
  }
  if (slots.size() != known) {
    if (FILE* t = std::fopen((dir / "slots.txt").string().c_str(), "wb")) {
      for (const auto& s : slots) std::fprintf(t, "%s\t%u\n", s.first.c_str(), s.second);
      std::fclose(t);
    }
  }
}

const Mod* ModOf(uint32_t id) {
  for (const auto& m : g_mods)
    if (m.slot == id) return &m;
  return nullptr;
}

// sub_826A1780(path, 0) for each overlay file; the path in a frame below the caller's.
// Each mount appends the pac's directory (header +4 bytes from 0x800) to the
// file system's directory buffer (vfs+56 start, vfs+60 end; vfs at
// 0x82ED5FEC), which has no room to spare for dozens more: it moves to a
// bigger buffer first. What was registered keeps pointing into the old one,
// which stays as it is.
constexpr uint32_t kVfs = 0x82ED5FEC, kDirStart = 56, kDirEnd = 60;
void GrowDirectory(uint8_t* base) {
  const uint32_t vfs = Rd32(base + kVfs);
  if (!vfs) return;
  const uint32_t start = Rd32(base + vfs + kDirStart), end = Rd32(base + vfs + kDirEnd);
  if (!start || end < start || end - start > (64u << 20)) return;
  uint32_t need = 0x10000;
  for (const auto& m : g_mods) need += 2 * 0x4000;  // (at most a directory each)
  const uint32_t at = g_memory->SystemHeapAlloc(end - start + need, 0x40);
  if (!at) return;
  std::memcpy(base + at, base + start, end - start);
  Wr32(base + vfs + kDirStart, at);
  Wr32(base + vfs + kDirEnd, at + (end - start));
  REXLOG_INFO("[svr2011] superstar mods: file system directory {} KB, moved with {} KB more", (end - start) >> 10,
              need >> 10);
}

void Mount(PPCContext& ctx, uint8_t* base) {
  if (g_mounted || g_mods.empty()) return;
  g_mounted = true;
  GrowDirectory(base);
  const auto saved = ctx;
  const uint32_t str = saved.r1.u32 - 0x300;
  for (const auto& m : g_mods)
    for (const std::string* p : {&m.ch_guest, &m.ssf_guest}) {
      if (p->empty()) continue;
      std::memcpy(base + str, p->c_str(), p->size() + 1);
      ctx = saved;
      ctx.r1.u64 = saved.r1.u32 - 0x400;
      ctx.r3.u64 = str;
      ctx.r4.u64 = 0;
      sub_826A1780(ctx, base);
      REXLOG_INFO("[svr2011] superstar mods: mounted {} ({})", *p, ctx.r3.u32);
    }
  ctx = saved;
}

void PutName(uint8_t* rec, uint32_t off, const std::string& s) {
  std::memset(rec + off, 0, kNameLen);
  std::memcpy(rec + off, s.data(), std::min<size_t>(s.size(), kNameLen - 1));
}

// The mods' records and profiles. A slot is set up from the base only when it
// isn't this mod's yet - a save's copy (with the player's edits) stays:
// - the record when its name isn't the mod's (a new mod, or a save from
//   before it): the base's, with the mod's names, own id, selectable and DLC;
// - the profile (moves, entrance, ...) while it is still the blank one
//   CHAR/PRO loaded for the slot (g_blank).
// Pool ids no mod has (a mod removed since the save) are not selectable.
// The game reloads CHAR/DAT and CHAR/PRO after a save has loaded (the DLC
// scan, sub_825A09B0), which puts the slots' placeholders back: what a slot
// held just before (the save's copy, the player's edits) is kept (Remember)
// and goes back instead of the base's.
std::vector<std::pair<uint32_t, svrfmt::Bytes>> g_blank;  // slot -> CHAR/PRO's profile
struct Kept {
  uint32_t slot = 0;
  svrfmt::Bytes record, profile;
};
std::vector<Kept> g_kept;

Kept& KeptFor(uint32_t slot) {
  for (auto& k : g_kept)
    if (k.slot == slot) return k;
  g_kept.push_back({slot, {}, {}});
  return g_kept.back();
}

bool IsBlank(uint32_t slot, const uint8_t* profile) {
  for (const auto& b : g_blank)
    if (b.first == slot) return !std::memcmp(profile, b.second.data(), kProfileSize);
  return true;  // (CHAR/PRO not loaded yet: nothing of the mod's there)
}

// Before CHAR/DAT or CHAR/PRO (re)loads: the mods' slots as they are.
void Remember(uint8_t* base) {
  for (const auto& m : g_mods) {
    const uint32_t si = Rd16(base + kIdToIndex + m.slot * 2);
    if (si >= 512) continue;
    const uint8_t* sr = base + kRecords + si * kRecordSize;
    if (std::strncmp(reinterpret_cast<const char*>(sr + kFullName), m.name.c_str(), kNameLen - 1)) continue;
    Kept& k = KeptFor(m.slot);
    k.record.assign(sr, sr + kRecordSize);
    const uint8_t* sp = base + kProfiles + si * kProfileSize;
    if (!IsBlank(m.slot, sp)) k.profile.assign(sp, sp + kProfileSize);
  }
}

void ApplyRecords(uint8_t* base) {
  for (uint32_t id : kPool) {
    if (ModOf(id)) continue;
    const uint32_t si = Rd16(base + kIdToIndex + id * 2);
    if (si < 512) base[kRecords + si * kRecordSize + kSelectable] = 0;
  }
  for (const auto& m : g_mods) {
    const uint32_t bi = Rd16(base + kIdToIndex + m.base * 2), si = Rd16(base + kIdToIndex + m.slot * 2);
    if (bi >= 512 || si >= 512 || bi == si) continue;
    uint8_t* br = base + kRecords + bi * kRecordSize;
    uint8_t* sr = base + kRecords + si * kRecordSize;
    if (!br[kFullName]) continue;  // (not loaded yet)
    uint8_t* bp = base + kProfiles + bi * kProfileSize;
    uint8_t* sp = base + kProfiles + si * kProfileSize;
    bool have_blank = false;
    for (const auto& b : g_blank) have_blank |= b.first == m.slot;
    if (have_blank && IsBlank(m.slot, sp)) {
      const Kept* k = nullptr;
      for (const auto& x : g_kept)
        if (x.slot == m.slot && !x.profile.empty()) k = &x;
      if (k) {
        std::memcpy(sp, k->profile.data(), kProfileSize);
      } else {
        std::memcpy(sp, bp, kProfileSize);
        SetEntranceMedia(sp, m);
        REXLOG_INFO("[svr2011] superstar mods: profile {} -> id {}", m.base, m.slot);
      }
    }
    if (!std::strncmp(reinterpret_cast<char*>(sr + kFullName), m.name.c_str(), kNameLen - 1)) {
      sr[kSelectable] = 1, sr[kDlc] = 1;
      static std::vector<std::pair<uint32_t, uint32_t>> told;  // (slot, its ratings when last logged)
      const uint32_t r = uint32_t(sr[0]) << 16 | sr[1] << 8 | sr[2];
      auto t = std::find_if(told.begin(), told.end(), [&](const auto& x) { return x.first == m.slot; });
      if (t == told.end() || t->second != r) {
        if (t == told.end()) told.push_back({m.slot, r});
        else t->second = r;
        REXLOG_INFO("[svr2011] superstar mods: id {} kept as loaded (ratings {} {} {} ...)", m.slot, sr[0], sr[1],
                    sr[2]);
      }
      continue;  // (already: as set up, or from a save)
    }
    const std::string was(reinterpret_cast<char*>(sr + kFullName), 0,
                          strnlen(reinterpret_cast<char*>(sr + kFullName), kNameLen));
    {
      const Kept* k = nullptr;
      for (const auto& x : g_kept)
        if (x.slot == m.slot && !x.record.empty()) k = &x;
      if (k) {  // (as it was before the game reloaded its records)
        std::memcpy(sr, k->record.data(), kRecordSize);
        continue;
      }
    }
    std::memcpy(sr, br, kRecordSize);
    PutName(sr, kFullName, m.name);
    PutName(sr, kSecondName, m.name);
    PutName(sr, kShortName, m.short_name);
    sr[kSelectable] = 1;
    sr[kDlc] = 1;
    for (uint32_t off : {kOwnId, kOwnId2, kSamePerson}) sr[off] = uint8_t(m.slot >> 8), sr[off + 1] = uint8_t(m.slot);
    // test aid: SVR2011_TEST_STAR_EDIT=<id> - that mod's ratings set to 20 (as
    // an edit made in the game would), to see them kept through a save
    if (const char* e = std::getenv("SVR2011_TEST_STAR_EDIT"); e && uint32_t(std::atoi(e)) == m.slot)
      for (uint32_t i = 0; i < 15; ++i) sr[i] = i == 7 ? sr[i] : 20;
    REXLOG_INFO("[svr2011] superstar mods: record {} -> id {} ({}; was \"{}\")", m.base, m.slot, m.name, was);
  }
}

// CHAR/PRO loaded: each slot's blank profile (before ApplyRecords).
void KeepBlankProfiles(uint8_t* base) {
  for (const auto& m : g_mods) {
    bool have = false;
    for (const auto& b : g_blank) have |= b.first == m.slot;
    const uint32_t si = Rd16(base + kIdToIndex + m.slot * 2);
    if (have || si >= 512) continue;
    const uint8_t* sp = base + kProfiles + si * kProfileSize;
    g_blank.push_back({m.slot, svrfmt::Bytes(sp, sp + kProfileSize)});
  }
}

// ---- The "MODDED" badge
// The select screen's panel shows a DLC-flagged character with the
// "DOWNLOADABLE CONTENT" badge (layout node cursor+312, sub_82466830), the
// texture DLCtex01 (menuHD.pac MENx/CHSI, 7001; 256 x 64 DXT5, one per
// language). Mods are DLC-flagged, so the same badge shows; while a panel
// hovers a mod, that texture's pixels (in guest memory, 16-bit words swapped)
// say "MODDED" (tools/make_modded_badge.py) and go back for real DLC. Both
// panels share the texture: a mod wins.
const uint8_t kModdedRgba[] = {
#include "modded_badge.inc"
};
constexpr uint32_t kBadgeW = 256, kBadgeH = 64;
constexpr uint32_t kBadgeCaller = 0x82466E40;  // sub_82466830: sub_828B6CD0(id) at the badge

struct Badge {
  svrfmt::Bytes data;  // swapped as in memory
  size_t sig = 0;      // a distinctive 64 bytes in it
};
std::vector<Badge> g_badges;  // [0]: MODDED, then the originals
std::mutex g_badge_mutex;
std::vector<std::pair<uint32_t, bool>> g_panel_mod;  // select screen cursor -> hovering a mod
int64_t g_badge_seen = 0;                            // last panel update (a select screen is up)
bool g_badge_thread = false;

svrfmt::Bytes Swap16(const uint8_t* p, size_t n) {
  svrfmt::Bytes o(p, p + n);
  for (size_t i = 0; i + 1 < n; i += 2) std::swap(o[i], o[i + 1]);
  return o;
}

size_t Signature(const svrfmt::Bytes& d) {
  for (size_t at = 0; at + 64 <= d.size(); at += 64) {
    int distinct = 0;
    for (int i = 0; i < 4; ++i) {
      bool seen = false;
      for (int j = 0; j < i; ++j) seen |= !std::memcmp(&d[at + i * 16], &d[at + j * 16], 16);
      distinct += !seen;
    }
    if (distinct >= 4) return at;
  }
  return 0;
}

// The originals from menuHD.pac (MENU, MENF, MENG, MENI, MENE: CHSI) and MODDED.
void LoadBadges() {
  FILE* f = std::fopen((g_game / "pac" / "menu" / "menuHD.pac").string().c_str(), "rb");
  if (!f) return;
  svrfmt::Bytes toc(0x4000);
  if (std::fread(toc.data(), 1, toc.size(), f) != toc.size() || std::memcmp(toc.data(), "EPAC", 4)) {
    std::fclose(f);
    return;
  }
  auto le32 = [&](size_t p) { return uint32_t(toc[p]) | toc[p + 1] << 8 | toc[p + 2] << 16 | uint32_t(toc[p + 3]) << 24; };
  std::vector<svrfmt::Bytes> found;
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!le32(p)) break;
    const uint32_t cnt = le32(p + 4) / 3;
    p += 12;
    for (uint32_t i = 0; i < cnt && p + 12 <= 0x4000; ++i, p += 12) {
      if (std::memcmp(&toc[p], "CHSI", 4)) continue;
      svrfmt::Bytes e(size_t(le32(p + 8)) * 0x100);
      std::fseek(f, long(0x4000 + size_t(le32(p + 4)) * 0x800), SEEK_SET);
      if (std::fread(e.data(), 1, e.size(), f) != e.size()) continue;
      std::vector<svrfmt::PachEntry> pe;
      if (!svrfmt::PachRead(e, pe)) continue;
      for (const auto& x : pe) {
        std::vector<svrfmt::BundleTexture> texs;
        if (!svrfmt::BundleRead(svrfmt::Unpack(x.data), texs)) continue;
        for (const auto& t : texs)
          if (t.name == "DLCtex01" && t.data.size() == 128 + kBadgeW * kBadgeH) found.push_back(t.data);
      }
    }
  }
  std::fclose(f);
  if (found.empty()) return;
  svrfmt::Image img;
  img.w = kBadgeW, img.h = kBadgeH;
  img.rgba.assign(kModdedRgba, kModdedRgba + sizeof kModdedRgba);
  const svrfmt::Bytes dds = svrfmt::DdsEncode(img, svrfmt::DxtFormat::kDxt5, false);
  g_badges.push_back({Swap16(dds.data() + 128, kBadgeW * kBadgeH), 0});
  for (const auto& o : found) {
    Badge b{Swap16(o.data() + 128, kBadgeW * kBadgeH), 0};
    b.sig = Signature(b.data);
    g_badges.push_back(std::move(b));
  }
  REXLOG_INFO("[svr2011] superstar mods: badge ({} languages)", found.size());
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool Committed(uint32_t at) {
  auto* heap = g_memory->LookupHeap(at);
  rex::memory::HeapAllocationInfo info{};
  return heap && heap->QueryRegionInfo(at & ~0xFFFu, &info) && info.region_size &&
         (info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead);
}

// Which badge the texture at guest holds (-1: none of them).
int BadgeAt(uint8_t* b, uint32_t guest) {
  if (!guest || !Committed(guest) || !Committed(guest + kBadgeW * kBadgeH - 1)) return -1;
  for (size_t i = 0; i < g_badges.size(); ++i)
    if (!std::memcmp(b + guest, g_badges[i].data.data(), g_badges[i].data.size())) return int(i);
  return -1;
}

void BadgeLoop() {
  uint32_t guest = 0;
  int original = 1;  // the language's original there
  int64_t last_scan = 0;
  int scans = 0;  // this visit of the select screen
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool want = false, active;
    {
      std::lock_guard lock(g_badge_mutex);
      active = NowMs() - g_badge_seen < 5000;
      if (!active) g_panel_mod.clear(), scans = 0;
      for (const auto& p : g_panel_mod) want |= p.second;
    }
    uint8_t* b = g_memory->virtual_membase();
    int now = BadgeAt(b, guest);
    if (now < 0) guest = 0;
    if (!guest) {
      if (!active || scans >= 10 || NowMs() - last_scan < 1000) continue;
      last_scan = NowMs();
      ++scans;
      // (the menu textures: physical memory, seen at 0xE0000000+ in the
      // VS screen search; then the other views)
      for (uint64_t lo : {0xE0000000ull, 0xA0000000ull}) {
        const uint64_t hi = lo == 0xE0000000ull ? 0xFFFF0000ull : 0xE0000000ull;
        for (uint64_t at = lo; at < hi && !guest;) {
          auto* heap = g_memory->LookupHeap(uint32_t(at));
          rex::memory::HeapAllocationInfo info{};
          if (!heap || !heap->QueryRegionInfo(uint32_t(at) & ~0xFFFu, &info) || !info.region_size) {
            at = (at & ~0xFFFull) + 0x10000;
            continue;
          }
          const uint64_t end = std::min<uint64_t>(uint64_t(info.base_address) + info.region_size, hi);
          if ((info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead))
            for (uint64_t a = at; a + 64 <= end && !guest; a += 16)
              for (size_t i = 0; i < g_badges.size(); ++i) {
                const auto& g = g_badges[i];
                if (a < lo + g.sig || std::memcmp(b + a, g.data.data() + g.sig, 64)) continue;
                if (BadgeAt(b, uint32_t(a - g.sig)) == int(i)) {
                  guest = uint32_t(a - g.sig);
                  break;
                }
              }
          at = std::max<uint64_t>(end, at + 0x1000);
        }
        if (guest) break;
      }
      if (!guest) continue;
      now = BadgeAt(b, guest);
      REXLOG_INFO("[svr2011] superstar mods: badge texture at {:08X}", guest);
    }
    if (now > 0) original = now;
    const int target = want ? 0 : original;
    if (now != target) std::memcpy(b + guest, g_badges[target].data.data(), g_badges[target].data.size());
  }
}

}  // namespace

// The select screen's panel: "is it DLC" for the badge (r26: the id; r31:
// the cursor).
REX_EXTERN(__imp__sub_828B6CD0);
REX_HOOK_RAW(sub_828B6CD0) {
  if (uint32_t(ctx.lr) == kBadgeCaller && !g_badges.empty()) {
    const uint32_t cursor = ctx.r31.u32;
    const bool mod = ModOf(ctx.r3.u32) != nullptr;
    std::lock_guard lock(g_badge_mutex);
    g_badge_seen = NowMs();
    bool found = false;
    for (auto& p : g_panel_mod)
      if (p.first == cursor) p.second = mod, found = true;
    if (!found) g_panel_mod.push_back({cursor, mod});
    if (!g_badge_thread) {
      g_badge_thread = true;
      std::thread(BadgeLoop).detach();
    }
  }
  __imp__sub_828B6CD0(ctx, base);
}

// The DLC tile's list filter (sub_8244A128(screen, id): DLC-flagged,
// selectable, owned): mods are in the EXTRA list instead.
REX_EXTERN(__imp__sub_8244A128);
REX_HOOK_RAW(sub_8244A128) {
  if (ModOf(ctx.r4.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_8244A128(ctx, base);
}

// Test aid: SVR2011_TEST_VFS_LOG=1 logs each virtual file lookup that finds
// nothing (sub_826B87B0(vfs, "/TYPE/NAME", out)), once per name.
REX_EXTERN(__imp__sub_826B87B0);
REX_HOOK_RAW(sub_826B87B0) {
  static const bool log = [] {
    const char* e = std::getenv("SVR2011_TEST_VFS_LOG");
    return e && *e == '1';
  }();
  if (!log) {
    __imp__sub_826B87B0(ctx, base);
    return;
  }
  const std::string name(reinterpret_cast<const char*>(base + ctx.r4.u32), 0,
                         strnlen(reinterpret_cast<const char*>(base + ctx.r4.u32), 64));
  const uint32_t caller = uint32_t(ctx.lr);
  __imp__sub_826B87B0(ctx, base);
  if (ctx.r3.u32) return;
  static std::mutex m;
  static std::vector<std::string> seen;
  std::lock_guard lock(m);
  if (std::find(seen.begin(), seen.end(), name) != seen.end()) return;
  seen.push_back(name);
  REXLOG_INFO("[svr2011] vfs miss: {} (from {:08X})", name, caller);
}

// The file system's pacs registered (from the ARC / from each pac's header).
REX_EXTERN(__imp__sub_825953B0);
REX_HOOK_RAW(sub_825953B0) {
  __imp__sub_825953B0(ctx, base);
  const uint64_t r3 = ctx.r3.u64;
  Mount(ctx, base);
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_82595428);
REX_HOOK_RAW(sub_82595428) {
  __imp__sub_82595428(ctx, base);
  const uint64_t r3 = ctx.r3.u64;
  Mount(ctx, base);
  ctx.r3.u64 = r3;
}

// The roster records loaded (CHAR/DAT), the profiles (CHAR/PRO), a save loaded.
REX_EXTERN(__imp__sub_82B89E50);
REX_HOOK_RAW(sub_82B89E50) {
  Remember(base);
  __imp__sub_82B89E50(ctx, base);
  const auto r3 = ctx.r3.u64;
  ApplyRecords(base);
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_82594C78);
REX_HOOK_RAW(sub_82594C78) {
  Remember(base);
  __imp__sub_82594C78(ctx, base);
  const auto r3 = ctx.r3.u64;
  KeepBlankProfiles(base);
  ApplyRecords(base);
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_8257A218);
REX_HOOK_RAW(sub_8257A218) {
  __imp__sub_8257A218(ctx, base);
  const auto r3 = ctx.r3.u64;
  ApplyRecords(base);
  ctx.r3.u64 = r3;
}
// A roster list is about to be built.
REX_EXTERN(__imp__sub_82736C68);
REX_HOOK_RAW(sub_82736C68) {
  ApplyRecords(base);
  __imp__sub_82736C68(ctx, base);
}

// Owned (DLC-flagged ids must be): mods are.
REX_EXTERN(__imp__sub_828C1288);
REX_HOOK_RAW(sub_828C1288) {
  if (ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_828C1288(ctx, base);
}
REX_EXTERN(__imp__sub_82589198);
REX_HOOK_RAW(sub_82589198) {
  if (ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_82589198(ctx, base);
}

namespace svr2011 {

void CopySuperstarMovies(const std::filesystem::path& movies) {
  std::error_code ec;
  const fs::path dir = rex::filesystem::GetExecutableFolder() / "Mods" / "Superstars";
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_directory() || fs::exists(e.path() / "disabled", ec)) continue;
    std::string name = e.path().filename().string(), movie;
    if (FILE* t = std::fopen((e.path() / "manifest.txt").string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("name=", 0) == 0) name = l.substr(5);
        if (l.rfind("movie=", 0) == 0) movie = l.substr(6);
      }
      std::fclose(t);
    }
    if (!movie.empty() && fs::path(movie).extension() == ".bik" && fs::exists(e.path() / movie, ec))
      CopyIfChanged(e.path() / movie, movies / (FileName(name) + ".bik"));
  }
}

void InstallSuperstarMods(rex::memory::Memory* memory, rex::filesystem::VirtualFileSystem* vfs) {
  g_memory = memory;
  g_game = rex::filesystem::GetExecutableFolder();
  LoadMods();
  // The overlay files as smods:\<file>: a device of their own, made after they
  // are written (a host device lists its folder once, so GAME: would not see
  // the files made this run).
  if (!g_mods.empty() && vfs) {
    auto device = std::make_unique<rex::filesystem::HostPathDevice>("\\SUPERSTARMODS",
                                                                    g_game / "Mods" / "SuperstarOverlay", true);
    if (!device->Initialize() || !vfs->RegisterDevice(std::move(device)) ||
        !vfs->RegisterSymbolicLink("smods:", "\\SUPERSTARMODS")) {
      REXLOG_WARN("[svr2011] superstar mods: could not mount Mods/SuperstarOverlay");
      g_mods.clear();
    }
  }
  if (!g_mods.empty()) LoadBadges();
  REXLOG_INFO("[svr2011] superstar mods: {}", g_mods.size());
}

std::vector<uint32_t> SuperstarModIds() {
  std::vector<uint32_t> ids;
  for (const auto& m : g_mods) ids.push_back(m.slot);
  return ids;
}

bool IsSuperstarMod(uint32_t id) { return ModOf(id) != nullptr; }

}  // namespace svr2011
