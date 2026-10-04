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
#include "move_packs.h"

#include <algorithm>
#include <cctype>
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
#include "crowd_signs.h"
#include "media_mods.h"
#include "music.h"
#include "user_movies.h"

namespace fs = std::filesystem;

namespace {

// The ids mods take (50): disc ids whose roster record is a blank placeholder
// ("0", loaded from CHAR/DAT, not selectable) with no model, select render,
// entrance or match data anywhere in the game's pacs, then the free DLC slots
// (59-69; real DLC uses 51-58). Each has a record, profile and save slot.
// A mod in a DLC slot never gets its entrance and the match doesn't start
// (tested 59 and 60: the game takes those ids' entrances from DLC content),
// so they come last and a mod in one moves to a free disc id (MoveOffDlc).
constexpr uint32_t kPool[] = {111, 114, 121, 127, 128, 129, 130, 136, 141, 148, 149, 151, 152,
                              154, 155, 157, 162, 163, 167, 168, 172, 173, 181, 185, 189, 200,
                              202, 203, 204, 206, 207, 209, 213, 214, 220, 221, 223, 225, 227,
                              59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69};
bool IsDlcSlot(uint32_t id) { return id >= 59 && id <= 69; }
constexpr uint32_t kOwnId = 32, kOwnId2 = 218;  // u16 own id in the record
constexpr uint32_t kSignIds = 210;               // u16[4]: the crowd signs its fans hold (id*10 + 1..4)
constexpr uint32_t kIdToIndex = 0x82DB3610;  // u16 per id
constexpr uint32_t kRecords = 0x82E407C0, kRecordSize = 260;
constexpr uint32_t kProfiles = 0x82E7C920, kProfileSize = 1056;
constexpr uint32_t kFullName = 34, kSecondName = 102, kShortName = 170, kNameLen = 32;
constexpr uint32_t kSelectable = 221, kDlc = 257, kSamePerson = 228, kAbilities = 230;

struct Mod {
  std::string folder, name, short_name;
  uint32_t base = 0, slot = 0;
  std::string ch_guest, ssf_guest;  // the overlay files, as the game opens them
  std::vector<std::string> attire_guests;  // extra attires' pacs (manifest attire<N>=)
  uint32_t attires = 1;                    // attires it has (its pacs' EMD models)
  uint32_t signs = 0;                      // its own crowd signs (sign1..4.dds; crowd_signs.h)
  std::vector<fs::path> sign_files;
  std::string attire_names[4];             // (manifest attire<N>_name=; attire 1: the game's ORIGINAL ATTIRE)
  uint32_t attire_pairs = 0;               // guest: 4 x {u32 name string id, s32 unlock} (sub_828C4060)
  std::string song;                 // its theme: a USER PLAYLIST name ("" = the base's)
  int movie = 0;                    // its entrance movie: a user movie id (0 = the base's)
  // every theme / movie the port has set for it (<folder>/.applied): a slot
  // still holding one of them (or the base's) follows the manifest; anything
  // else was the player's choice in the game and stays
  std::vector<std::string> songs_set;
  std::vector<int> movies_set;
  int call = -1;  // name call: -1 the base's, else a Created Superstar nickname (0-83)
  // made with a fighting style (Mod Maker "Create a new superstar"): base= is
  // only its template - its own attributes, a nickname name call (The
  // Superstar unless call=) and the generic select render (unless its own)
  bool styled = false;
  int ratings[7] = {-1, -1, -1, -1, -1, -1, -1};
  fs::path voice;  // its own recorded name for the ring announcer (manifest voice=)
  // a superstar the game's sound banks still have (manifest announcer=<NAME>,
  // e.g. JEFFHARDY): the ring announcer says it with the game's own
  // RA_*_<NAME>_* clips, in the categories where the base says its own name
  std::string announcer;
  // its own moves over the base's (manifest moves=<file>: lines "0xOFF=id",
  // a byte offset in the profile's move block, 0..0x1BF, and a move id)
  std::vector<std::pair<uint16_t, uint16_t>> moves;
  int entrance = -1;            // entrance number (manifest entrance=; profile +0x1C0 +0x14..+0x18)
  std::vector<uint8_t> abilities;  // (manifest abilities=a,b,...; record +230.., up to 8)
};
std::vector<Mod> g_mods;
std::vector<std::string> g_extra_mounts;  // other overlay pacs (smods:\<file>; AddOverlayMount)
constexpr uint32_t kAttireNameIds = 0xB100;                   // our string ids for attire names
std::vector<std::pair<uint32_t, uint32_t>> g_attire_names;  // string id -> guest text
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
// `only` >= 0: an extra attire's pac - its attire `only` (usually 0) becomes
// attire `as`, the rest are put out of the way (id 999999). `drop`: attires
// (bit mask) of the main pac other pacs replace, put out of the way too.
bool RenameModel(svrfmt::Bytes& d, uint32_t to, int only = -1, int as = 0, uint32_t drop = 0) {
  if (d.size() < 0x4000 || std::memcmp(d.data(), "EPK8", 4)) return false;
  char b[8];
  std::snprintf(b, sizeof b, "%06u", to);
  int n = 0;
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!std::memcmp(&d[p], "\0\0\0\0", 4)) break;
    const bool emd = !std::memcmp(&d[p], "EMD ", 4);
    const uint32_t entries = (uint32_t(d[p + 4]) | uint32_t(d[p + 5]) << 8) / 4;
    p += 12;
    for (uint32_t k = 0; k < entries && p + 16 <= 0x4000; ++k, p += 16) {
      if (!emd || !std::all_of(&d[p], &d[p + 8], [](uint8_t c) { return c >= '0' && c <= '9'; })) continue;
      const int attire = d[p + 6] - '0';
      const bool keep = only >= 0 ? attire == only : !(drop >> attire & 1);
      std::memcpy(&d[p], keep ? b : "999999", 6);
      if (keep && only >= 0) d[p + 6] = uint8_t('0' + as);
      n += keep;
    }
  }
  return n > 0;
}

// The base's select screen renders, renamed to the slot: an EPAC with
// SSFA / SSFB / SSFC.
// A mod's own select picture (render.dds 512 x 512 and render_small.dds
// 256 x 256, DXT5; the Mod Maker makes them) replaces the base's for every
// attire: SSFA and SSFB are the 512 picture, SSFC the small one. The game's
// entries are BPE-packed DDS; so are ours.
svrfmt::Bytes OwnRender(const fs::path& file, int size) {
  svrfmt::Bytes d;
  svrfmt::DdsInfo info;
  if (!svrfmt::ReadFile(file.string(), d) || !svrfmt::DdsInfoOf(d, info) || info.w != size || info.h != size)
    return {};
  return svrfmt::BpeEncode(d);  // (compressed: the render loader's buffer is sized for that)
}

bool BuildRenders(const fs::path& out, uint32_t base, uint32_t slot, const fs::path& folder, bool generic = false) {
  svrfmt::Bytes d;
  svrfmt::Epac src;
  if (!svrfmt::ReadFile((g_game / "pac" / "DLC_HD.pac").string(), d) || !svrfmt::EpacRead(d, src)) return false;
  const svrfmt::Bytes big = OwnRender(folder / "render.dds", 512), small = OwnRender(folder / "render_small.dds", 256);
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
      if (generic) {  // (the game's silhouette, 9000, for every attire)
        if (key != 9000) continue;
        const svrfmt::Bytes& own = g.type == "SSFC" ? small : big;
        for (int a = 0; a < 4; ++a) {
          char gn[8];
          std::snprintf(gn, sizeof gn, "%04d", a * 1000 + int(slot));
          og.entries.push_back({gn, own.empty() ? en.data : own});
          ++n;
        }
        continue;
      }
      if (key % 1000 != int(base)) continue;
      char nm[8];
      std::snprintf(nm, sizeof nm, "%04d", (key / 1000) * 1000 + int(slot));
      const svrfmt::Bytes& own = g.type == "SSFC" ? small : big;
      og.entries.push_back({nm, own.empty() ? en.data : own});
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

// A mod's overlay file is rebuilt when its sources change: <out>.src holds
// each source's name, size and time as they were built from (times alone
// won't do: an installed mod's files carry the zip's old dates).
std::string Stamp(const std::vector<fs::path>& srcs) {
  std::string st;
  std::error_code ec;
  for (const auto& p : srcs) {
    if (!fs::exists(p, ec)) continue;
    st += p.filename().string() + " " + std::to_string(static_cast<unsigned long long>(fs::file_size(p, ec))) + " " +
          std::to_string(static_cast<long long>(fs::last_write_time(p, ec).time_since_epoch().count())) + "\n";
  }
  return st;
}

bool Current(const fs::path& out, const std::vector<fs::path>& srcs) {
  std::error_code ec;
  if (!fs::exists(out, ec)) return false;
  std::string old;
  if (FILE* t = std::fopen((out.string() + ".src").c_str(), "rb")) {
    char buf[1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, t)) > 0) old.append(buf, n);
    std::fclose(t);
  }
  return old == Stamp(srcs);
}

void MarkCurrent(const fs::path& out, const std::vector<fs::path>& srcs) {
  if (FILE* t = std::fopen((out.string() + ".src").c_str(), "wb")) {
    const std::string st = Stamp(srcs);
    std::fwrite(st.data(), 1, st.size(), t);
    std::fclose(t);
  }
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
  std::error_code ec, e1, e2;
  // (same size and not older: kept - a re-encoded theme can keep its size)
  if (fs::exists(to, ec) && fs::file_size(to, ec) == fs::file_size(from, ec) &&
      fs::last_write_time(to, e1) >= fs::last_write_time(from, e2))
    return true;
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
std::string SongOf(const uint8_t* e) {  // the USER PLAYLIST name (ASCII part)
  std::string n;
  for (uint32_t i = 0; i < 40; ++i) {
    const uint16_t c = uint16_t(e[kSongName + 2 * i] << 8 | e[kSongName + 2 * i + 1]);
    if (!c) break;
    n.push_back(c < 128 ? char(c) : '?');
  }
  return n;
}

// A fresh profile (the base's copied): the mod's own moves (big-endian u16
// move ids at byte offsets in the move block, below the entrance) and its
// entrance number (+0x14, and the alternates +0x16, +0x18).
void SetOwnMoves(uint8_t* profile, const Mod& m) {
  for (const auto& [off, id] : m.moves)
    if (off + 1u < kEntrance) profile[off] = uint8_t(id >> 8), profile[off + 1] = uint8_t(id);
  if (m.entrance >= 0)
    for (uint32_t k : {0x14u, 0x16u, 0x18u})
      profile[kEntrance + k] = uint8_t(m.entrance >> 8), profile[kEntrance + k + 1] = uint8_t(m.entrance);
}

// The profile's entrance music and movie follow the mod's manifest while they
// are still what the port set before or the base's (`base_profile`): a new
// theme or movie added to an installed mod reaches a slot a save already has.
void SetEntranceMedia(uint8_t* profile, const Mod& m, const uint8_t* base_profile) {
  uint8_t* e = profile + kEntrance;
  uint8_t before[0x100];
  std::memcpy(before, e, sizeof before);
  const uint8_t* b = base_profile + kEntrance;
  const uint16_t music = uint16_t(e[kMusic] << 8 | e[kMusic + 1]);
  const bool ours = music == kUserPlaylist &&
                    std::find(m.songs_set.begin(), m.songs_set.end(), SongOf(e)) != m.songs_set.end();
  const bool bases = !std::memcmp(e + kMusic, b + kMusic, 2) &&
                     (music != kUserPlaylist || !std::memcmp(e + kSongName, b + kSongName, 80));
  if (ours || bases) {
    if (!m.song.empty()) {
      e[kMusic] = 0, e[kMusic + 1] = uint8_t(kUserPlaylist);
      for (uint32_t i = 0; i < 40; ++i) {
        const uint16_t c = i < m.song.size() && i < 39 ? uint8_t(m.song[i]) : 0;
        e[kSongName + 2 * i] = uint8_t(c >> 8), e[kSongName + 2 * i + 1] = uint8_t(c);
      }
    } else {
      std::memcpy(e + kMusic, b + kMusic, 2);
      std::memcpy(e + kSongName, b + kSongName, 80);
    }
  }
  const int movie = e[kMovie] << 8 | e[kMovie + 1];
  if (std::find(m.movies_set.begin(), m.movies_set.end(), movie) != m.movies_set.end() ||
      !std::memcmp(e + kMovie, b + kMovie, 2)) {
    if (m.movie) e[kMovie] = uint8_t(m.movie >> 8), e[kMovie + 1] = uint8_t(m.movie);
    else std::memcpy(e + kMovie, b + kMovie, 2);
  }
  if (std::memcmp(before, e, sizeof before))
    REXLOG_INFO("[svr2011] superstar mods: id {} entrance: music {}{}, movie {}", m.slot,
                int(e[kMusic] << 8 | e[kMusic + 1]), m.song.empty() ? "" : " (" + m.song + ")",
                int(e[kMovie] << 8 | e[kMovie + 1]));
}

// <folder>/.applied: "song=<name>" / "movie=<id>" lines, all ever set.
void ReadApplied(Mod& m, const fs::path& folder) {
  if (FILE* t = std::fopen((folder / ".applied").string().c_str(), "rb")) {
    char line[256];
    while (std::fgets(line, sizeof line, t)) {
      std::string l = line;
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      if (l.rfind("song=", 0) == 0) m.songs_set.push_back(l.substr(5));
      if (l.rfind("movie=", 0) == 0) m.movies_set.push_back(std::atoi(l.c_str() + 6));
    }
    std::fclose(t);
  }
  bool add_song = !m.song.empty() && std::find(m.songs_set.begin(), m.songs_set.end(), m.song) == m.songs_set.end();
  bool add_movie = m.movie && std::find(m.movies_set.begin(), m.movies_set.end(), m.movie) == m.movies_set.end();
  if (!add_song && !add_movie) return;
  if (FILE* t = std::fopen((folder / ".applied").string().c_str(), "ab")) {
    if (add_song) std::fprintf(t, "song=%s\n", m.song.c_str()), m.songs_set.push_back(m.song);
    if (add_movie) std::fprintf(t, "movie=%d\n", m.movie), m.movies_set.push_back(m.movie);
    std::fclose(t);
  }
}

// Which attires (bit mask) a model pac has for character `id` (its EMD names).
uint32_t AttireMask(const fs::path& pac, uint32_t id) {
  uint32_t mask = 0;
  FILE* f = std::fopen(pac.string().c_str(), "rb");
  if (!f) return 0;
  svrfmt::Bytes d(0x4000);
  const bool ok = std::fread(d.data(), 1, d.size(), f) == d.size();
  std::fclose(f);
  if (!ok || std::memcmp(d.data(), "EPK8", 4)) return 0;
  char want[8];
  std::snprintf(want, sizeof want, "%06u", id);
  for (size_t p = 0x800; p + 12 <= 0x4000;) {
    if (!std::memcmp(&d[p], "\0\0\0\0", 4)) break;
    const bool emd = !std::memcmp(&d[p], "EMD ", 4);
    const uint32_t entries = (uint32_t(d[p + 4]) | uint32_t(d[p + 5]) << 8) / 4;
    p += 12;
    for (uint32_t k = 0; k < entries && p + 16 <= 0x4000; ++k, p += 16)
      if (emd && !std::memcmp(&d[p], want, 6) && d[p + 6] >= '0' && d[p + 6] <= '3' && d[p + 7] == '2')
        mask |= 1u << (d[p + 6] - '0');
  }
  return mask;
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
  size_t known = slots.size();
  // mods on a DLC slot move to a free disc id (see kPool)
  for (auto& s : slots) {
    if (!IsDlcSlot(s.second)) continue;
    for (uint32_t id : kPool) {
      if (IsDlcSlot(id)) break;
      bool taken = false;
      for (const auto& o : slots) taken |= o.second == id;
      if (taken) continue;
      REXLOG_INFO("[svr2011] superstar mods: {} moves from DLC slot {} to id {}", s.first, s.second, id);
      s.second = id;
      known = size_t(-1);  // (write slots.txt)
      break;
    }
  }
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
    std::string song, movie, moves;
    std::string attires[4];  // attire<N>= (1-4): another pac's first attire as attire N
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
        if (l.rfind("voice=", 0) == 0 && l.size() > 6) m.voice = f / l.substr(6);
        if (l.rfind("style=", 0) == 0) m.styled = true;
        if (l.rfind("ratings=", 0) == 0) {
          const char* q = l.c_str() + 8;
          for (int k = 0; k < 7 && *q; ++k) {
            m.ratings[k] = std::clamp(std::atoi(q), 1, 99);
            while (*q && *q != ',') ++q;
            if (*q == ',') ++q;
          }
        }
        if (l.rfind("attire", 0) == 0 && l.size() > 8 && l[6] >= '1' && l[6] <= '4' && l[7] == '=')
          attires[l[6] - '1'] = l.substr(8);
        if (l.rfind("attire", 0) == 0 && l.size() > 12 && l[6] >= '1' && l[6] <= '4' && !l.compare(7, 6, "_name="))
          m.attire_names[l[6] - '1'] = l.substr(13);
        if (l.rfind("call=", 0) == 0 && l.size() > 5 && std::isdigit(uint8_t(l[5])))
          m.call = std::clamp(std::atoi(l.c_str() + 5), 0, 83);
        if (l.rfind("announcer=", 0) == 0) {
          for (char c : l.substr(10))
            if (std::isalnum(uint8_t(c)) && m.announcer.size() < 24) m.announcer.push_back(char(std::toupper(uint8_t(c))));
        }
        if (l.rfind("entrance=", 0) == 0) m.entrance = std::clamp(std::atoi(l.c_str() + 9), 0, 65535);
        if (l.rfind("abilities=", 0) == 0) {
          for (const char* q = l.c_str() + 10; *q && m.abilities.size() < 8;) {
            if (const int a = std::atoi(q); a > 0 && a < 256) m.abilities.push_back(uint8_t(a));
            while (*q && *q != ',') ++q;
            if (*q == ',') ++q;
          }
        }
        if (l.rfind("moves=", 0) == 0 && l.size() > 6) moves = l.substr(6);
      }
      std::fclose(t);
    }
    if (m.base < 100 || m.base > 321) {
      REXLOG_WARN("[svr2011] superstar mods: {} has no base= character (100-321)", m.folder);
      continue;
    }
    if (!m.voice.empty() && !fs::exists(m.voice, ec)) m.voice.clear();
    if (!moves.empty())
      if (FILE* t = std::fopen((f / moves).string().c_str(), "rb")) {
        char line[256];
        while (std::fgets(line, sizeof line, t)) {
          char* eq = std::strchr(line, '=');
          if (line[0] == '#' || !eq) continue;
          const long off = std::strtol(line, nullptr, 0), id = std::strtol(eq + 1, nullptr, 0);
          if (off >= 0 && off < 0x1C0 && !(off & 1) && id > 0 && id < 65536)
            m.moves.push_back({uint16_t(off), uint16_t(id)});
        }
        std::fclose(t);
      }
    if (m.styled && m.call < 0) m.call = 77;  // (The Superstar: never the template's name)
    {
      std::vector<fs::path> signs;
      for (int k = 1; k <= 4; ++k)
        if (fs::exists(f / ("sign" + std::to_string(k) + ".dds"), ec))
          signs.push_back(f / ("sign" + std::to_string(k) + ".dds"));
      m.signs = uint32_t(signs.size());
      // (added below, once the mod has its id)
      m.sign_files = std::move(signs);
    }
    InstallMedia(m, f, song, movie);
    ReadApplied(m, f);
    m.slot = slot_of(m.folder);
    if (!m.slot) {
      REXLOG_WARN("[svr2011] superstar mods: only {} fit, {} left out (a line in slots.txt for a mod that is "
                  "gone can be removed)", std::size(kPool), m.folder);
      continue;
    }
    char nm[32];
    uint32_t drop = 0;
    for (int a = 0; a < 4; ++a)
      if (!attires[a].empty() && fs::exists(f / attires[a], ec)) drop |= 1u << a;
    std::snprintf(nm, sizeof nm, "ch%03u.pac", m.slot);
    if (!Current(overlay / nm, {f / "ch.pac", f / "manifest.txt"})) {
      svrfmt::Bytes ch;
      if (!svrfmt::ReadFile((f / "ch.pac").string(), ch) || !RenameModel(ch, m.slot, -1, 0, drop) ||
          !svrfmt::WriteFile((overlay / nm).string(), ch)) {
        REXLOG_WARN("[svr2011] superstar mods: {}: ch.pac is not a character model pac (EPK8 with EMD models)", m.folder);
        continue;
      }
      MarkCurrent(overlay / nm, {f / "ch.pac", f / "manifest.txt"});
    }
    m.ch_guest = std::string("smods:\\") + nm;
    for (int a = 0; a < 4; ++a) {
      if (!(drop >> a & 1)) continue;
      char an[32];
      std::snprintf(an, sizeof an, "ch%03u_a%d.pac", m.slot, a + 1);
      const fs::path src = f / attires[a];
      if (!Current(overlay / an, {src, f / "manifest.txt"})) {
        svrfmt::Bytes d;
        if (!svrfmt::ReadFile(src.string(), d) || !RenameModel(d, m.slot, 0, a) ||
            !svrfmt::WriteFile((overlay / an).string(), d)) {
          REXLOG_WARN("[svr2011] superstar mods: {}: {} is not a character model pac", m.folder, attires[a]);
          continue;
        }
        MarkCurrent(overlay / an, {src, f / "manifest.txt"});
      }
      m.attire_guests.push_back(std::string("smods:\\") + an);
    }
    {  // the attires its models give (contiguous from 1)
      uint32_t mask = 0;
      std::vector<fs::path> pacs = {overlay / nm};  // (nm: chNNN.pac still)
      for (int a = 0; a < 4; ++a)
        if (drop >> a & 1) {
          char an[32];
          std::snprintf(an, sizeof an, "ch%03u_a%d.pac", m.slot, a + 1);
          pacs.push_back(overlay / an);
        }
      for (const auto& pp : pacs) mask |= AttireMask(pp, m.slot);
      m.attires = 1;
      while (m.attires < 4 && (mask >> m.attires & 1)) ++m.attires;
    }
    std::snprintf(nm, sizeof nm, "ssf%03u.pac", m.slot);
    // (the renders depend on base=: rebuilt with the manifest)
    const std::vector<fs::path> render_srcs = {f / "manifest.txt", f / "render.dds", f / "render_small.dds"};
    if (Current(overlay / nm, render_srcs) ||
        (BuildRenders(overlay / nm, m.base, m.slot, f, m.styled) && (MarkCurrent(overlay / nm, render_srcs), true)))
      m.ssf_guest = std::string("smods:\\") + nm;
    else
      REXLOG_WARN("[svr2011] superstar mods: {}: base {} has no select render (not a playable superstar)", m.folder,
                  m.base);
    if (m.signs) svr2011::AddCharacterSigns(m.slot, m.sign_files);
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
  for (const auto& m : g_mods) need += uint32_t(2 + m.attire_guests.size()) * 0x4000;  // (a directory each, at most)
  const uint32_t at = g_memory->SystemHeapAlloc(end - start + need, 0x40);
  if (!at) return;
  std::memcpy(base + at, base + start, end - start);
  Wr32(base + vfs + kDirStart, at);
  Wr32(base + vfs + kDirEnd, at + (end - start));
  REXLOG_INFO("[svr2011] superstar mods: file system directory {} KB, moved with {} KB more", (end - start) >> 10,
              need >> 10);
}

void Mount(PPCContext& ctx, uint8_t* base) {
  if (g_mounted || (g_mods.empty() && g_extra_mounts.empty())) return;
  g_mounted = true;
  GrowDirectory(base);
  const auto saved = ctx;
  const uint32_t str = saved.r1.u32 - 0x300;
  for (const auto& f : g_extra_mounts) {
    std::memcpy(base + str, f.c_str(), f.size() + 1);
    ctx = saved;
    ctx.r1.u64 = saved.r1.u32 - 0x400;
    ctx.r3.u64 = str;
    ctx.r4.u64 = 0;
    sub_826A1780(ctx, base);
    REXLOG_INFO("[svr2011] superstar mods: mounted {} ({})", f, ctx.r3.u32);
  }
  for (const auto& m : g_mods) {
    std::vector<const std::string*> files = {&m.ch_guest, &m.ssf_guest};
    for (const auto& a : m.attire_guests) files.push_back(&a);
    for (const std::string* p : files) {
      if (p->empty()) continue;
      std::memcpy(base + str, p->c_str(), p->size() + 1);
      ctx = saved;
      ctx.r1.u64 = saved.r1.u32 - 0x400;
      ctx.r3.u64 = str;
      ctx.r4.u64 = 0;
      sub_826A1780(ctx, base);
      REXLOG_INFO("[svr2011] superstar mods: mounted {} ({})", *p, ctx.r3.u32);
    }
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

// The record's crowd signs: the mod's own (crowd_signs.cpp adds them as
// id*10 + 1..4, repeated to fill 4), else its base's.
void SetSigns(uint8_t* sr, const uint8_t* br, const Mod& m) {
  for (uint32_t k = 0; k < 4; ++k) {
    if (m.signs) {
      const uint32_t v = m.slot * 10 + 1 + k % m.signs;
      sr[kSignIds + k * 2] = uint8_t(v >> 8), sr[kSignIds + k * 2 + 1] = uint8_t(v);
    } else {
      sr[kSignIds + k * 2] = br[kSignIds + k * 2], sr[kSignIds + k * 2 + 1] = br[kSignIds + k * 2 + 1];
    }
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
        SetOwnMoves(sp, m);
        REXLOG_INFO("[svr2011] superstar mods: profile {} -> id {}{}", m.base, m.slot,
                    m.moves.empty() && m.entrance < 0 ? "" : " (its own moves / entrance)");
      }
    }
    if (have_blank) SetEntranceMedia(sp, m, bp);
    SetSigns(sr, br, m);
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
    SetSigns(sr, br, m);
    for (int k = 0; k < 7; ++k)  // (its attributes: +0..+6 and the copy at +8)
      if (m.ratings[k] > 0) sr[k] = sr[8 + k] = uint8_t(m.ratings[k]);
    if (!m.abilities.empty())
      for (uint32_t k = 0; k < 8; ++k) sr[kAbilities + k] = k < m.abilities.size() ? m.abilities[k] : 0;

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
  // a media mod's replacement (renders: media_mods.h) - the first match in
  // the archive list wins, so the game's own would: asked for by another name
  if (const uint32_t alias = svr2011::MediaAlias(reinterpret_cast<const char*>(base + ctx.r4.u32))) {
    ctx.r4.u64 = alias;
    if (log) {
      __imp__sub_826B87B0(ctx, base);
      REXLOG_INFO("[svr2011] media mods: {:.16} resolved {}", reinterpret_cast<const char*>(base + alias), ctx.r3.u32);
      return;
    }
  }
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

// ---- The name call
// The ring announcer and the commentary pick a Superstar's name call by its
// character id (scratchpad re_namecall): mod ids have none (the announcer's
// table says "dummy", the commentary banks don't exist). A mod gets its
// base's call, or (call=) a Created Superstar nickname, as a CAW's.
// Announcer: sub_825EA208(out, slot) fills {+0 id, +4 call index, ..., +20
// u16 nickname (read when +0 < 50)}.
// Test aid: SVR2011_TEST_NAMECALL=1 logs the announcer's ids, the commentary
// banks and the audio events posted ("Play_...").
bool NameCallLog() {
  static const bool on = [] {
    const char* e = std::getenv("SVR2011_TEST_NAMECALL");
    return e && *e == '1';
  }();
  return on;
}
// The announcer's name table (from sound.pac): 45 categories of 64-byte
// event names ("RA_JR_SSP_<NAME>_0"); T = *(*(0x82EC4C18) + 32) + 4, count
// of category c at T+420+c*8, its names at *(T+424+c*8). Categories 2, 4, 5,
// 8, 40, 41, 42 are by character id ("dummy" for the mods' ids).
constexpr uint32_t kSoundMgr = 0x82EC4C18;
constexpr uint32_t kIdCategories[] = {2, 4, 5, 8, 40, 41, 42};
// The table: T from sub_825FDDC0(*(sub_825E5E68() + 32), &T), as
// sub_825EA4E8 reads it.
uint32_t CallTable(PPCContext& ctx, uint8_t* base) {
  const auto saved = ctx;
  ctx.r1.u64 = saved.r1.u32 - 0x200;
  sub_825E5E68(ctx, base);
  uint32_t T = 0;
  if (const uint32_t obj = ctx.r3.u32 ? Rd32(base + ctx.r3.u32 + 32) : 0) {
    const uint32_t out = saved.r1.u32 - 0x100;
    Wr32(base + out, 0);
    ctx.r3.u64 = obj;
    ctx.r4.u64 = out;
    sub_825FDDC0(ctx, base);
    T = Rd32(base + out);
  }
  ctx = saved;
  return T;
}

void WriteCallNames(PPCContext& ctx, uint8_t* base, uint32_t id) {
  const uint32_t T = CallTable(ctx, base);
  if (NameCallLog()) REXLOG_INFO("[svr2011] name call: table {:08X}", T);
  if (!T) return;
  for (uint32_t c : kIdCategories) {
    const uint32_t count = Rd32(base + T + 420 + c * 8), names = Rd32(base + T + 424 + c * 8);
    if (!names || id >= count) continue;
    char n[64];
    std::snprintf(n, sizeof n, "RA_JR_MOD_%u_%u", id, c);
    if (!std::strcmp(reinterpret_cast<const char*>(base + names + id * 64), n)) continue;  // (already)
    if (NameCallLog())
      REXLOG_INFO("[svr2011] name call: table {} [{}] was \"{:.60}\"", c, id,
                  reinterpret_cast<const char*>(base + names + id * 64));
    std::memset(base + names + id * 64, 0, 64);
    std::memcpy(base + names + id * 64, n, std::strlen(n));
  }
}
// announcer=<NAME>: every category where the base's entry says the base's own
// name ("RA_JR_SSN_MATTHARDY_1": the name from category 8) gets the same
// entry with NAME instead ("RA_JR_SSN_JEFFHARDY_1"), at the mod's id. The
// game picks the RA_TC_ announcer's from the RA_JR_ name itself.
void WriteAnnouncerNames(PPCContext& ctx, uint8_t* base, const Mod& m) {
  const uint32_t T = CallTable(ctx, base);
  if (!T) return;
  auto entry = [&](uint32_t c, uint32_t id) -> char* {
    const uint32_t count = Rd32(base + T + 420 + c * 8), names = Rd32(base + T + 424 + c * 8);
    return names && id < count ? reinterpret_cast<char*>(base + names + id * 64) : nullptr;
  };
  const char* b8 = entry(8, m.base);  // "RA_JR_SSN_<BASE NAME>_<n>"
  if (!b8 || std::strncmp(b8, "RA_JR_SSN_", 10)) return;
  std::string token(b8 + 10, strnlen(b8 + 10, 50));
  if (const size_t u = token.rfind('_'); u != std::string::npos) token.resize(u);
  if (token.empty()) return;
  for (uint32_t c = 0; c < 45; ++c) {
    const char* b = entry(c, m.base);
    char* d = entry(c, m.slot);
    if (!b || !d) continue;
    std::string n(b, strnlen(b, 63));
    const size_t at = n.find("_" + token + "_");
    if (at == std::string::npos) continue;
    n.replace(at + 1, token.size(), m.announcer);
    if (n.size() > 63 || !std::strcmp(d, n.c_str())) continue;
    if (NameCallLog()) REXLOG_INFO("[svr2011] name call: table {} [{}] \"{:.60}\" -> {}", c, m.slot, d, n);
    std::memset(d, 0, 64);
    std::memcpy(d, n.data(), n.size());
  }
}
REX_EXTERN(__imp__sub_825EA208);
REX_HOOK_RAW(sub_825EA208) {
  const uint32_t out = ctx.r3.u32;
  __imp__sub_825EA208(ctx, base);
  if (!out) return;
  if (NameCallLog())
    REXLOG_INFO("[svr2011] name call: announcer id {} call {} nickname {}", Rd32(base + out), Rd32(base + out + 4),
                Rd16(base + out + 20));
  const Mod* m = ModOf(Rd32(base + out));
  if (!m) return;
  if (!m->announcer.empty()) {  // (the game's own clips of that name)
    WriteAnnouncerNames(ctx, base, *m);
    return;
  }
  if (!m->voice.empty()) {  // (its own recording: the mod's own, silent, event names; PlayVoice)
    WriteCallNames(ctx, base, m->slot);
    return;
  }
  if (m->call < 0) {
    Wr32(base + out + 4, m->base);
  } else {
    Wr32(base + out, 0);  // (a Created Superstar's: the nickname is used)
    base[out + 20] = uint8_t(m->call >> 8), base[out + 21] = uint8_t(m->call);
  }
}
// Commentary: the bank "Comm_<name>" for n = the id (ids 50-69: n + 9950;
// CAWs: nickname + 1, the CAS_* names) - sub_8261EAB0(buf, n) makes the
// name, sub_825FDCD8(n) leaves out ids without commentary (0).
const Mod* ModOfCommentary(uint32_t n) { return ModOf(n >= 10000 && n < 10070 ? n - 9950 : n); }
REX_EXTERN(__imp__sub_8261EAB0);
REX_HOOK_RAW(sub_8261EAB0) {
  const uint32_t n = ctx.r4.u32, buf = ctx.r3.u32;
  if (const Mod* m = ModOfCommentary(ctx.r4.u32)) ctx.r4.u64 = m->call < 0 ? m->base : uint32_t(m->call + 1);
  const uint32_t used = ctx.r4.u32;
  __imp__sub_8261EAB0(ctx, base);
  if (NameCallLog() && buf)
    REXLOG_INFO("[svr2011] name call: commentary {} -> {} \"{:.40}\"", n, used, reinterpret_cast<const char*>(base + buf));
}
// Audio events by name (sub_82BEC030(name, ...)): a mod's own announcer
// event ("Play_RA_xx_MOD_<id>_<category>", WriteCallNames) plays its
// recorded name instead (the game's sound engine has no such event: silent).
REX_EXTERN(__imp__sub_82BEC030);
REX_HOOK_RAW(sub_82BEC030) {
  if (ctx.r3.u32 >= 0x10000) {
    const char* e = reinterpret_cast<const char*>(base + ctx.r3.u32);
    uint32_t id = 0;
    if (svr2011::MediaEvent(base, e, ctx.r4.u32, &id)) {  // (a media mod's sound instead: media_mods.h)
      ctx.r3.u64 = id;
      return;
    }
    if (!std::strncmp(e, "Play_", 5)) {
      if (NameCallLog()) REXLOG_INFO("[svr2011] name call: event {:.60}", e);
      if (!std::strncmp(e, "Play_RA_", 8))
        if (const char* mod = std::strstr(e, "_MOD_"))
          if (const Mod* m = ModOf(uint32_t(std::atoi(mod + 5))); m && !m->voice.empty()) {
            REXLOG_INFO("[svr2011] superstar mods: {}: name call {}", m->name, m->voice.filename().string());
            svr2011::PlayClip(m->voice);
          }
    }
  }
  const bool log_ra = NameCallLog() && ctx.r3.u32 >= 0x10000 &&
                      !std::strncmp(reinterpret_cast<const char*>(base + ctx.r3.u32), "Play_RA_", 8);
  std::string ev = log_ra ? std::string(reinterpret_cast<const char*>(base + ctx.r3.u32), 0, 60) : "";
  __imp__sub_82BEC030(ctx, base);
  if (log_ra) REXLOG_INFO("[svr2011] name call: {} -> playing id {}", ev, ctx.r3.u32);
}
REX_EXTERN(__imp__sub_825FDCD8);
REX_HOOK_RAW(sub_825FDCD8) {
  if (const Mod* m = ModOfCommentary(ctx.r3.u32)) {
    if (m->call >= 0) {  // (a nickname: the CAS_* banks all have commentary)
      ctx.r3.u64 = 1;
      return;
    }
    ctx.r3.u64 = m->base;  // (as the base would be)
  }
  __imp__sub_825FDCD8(ctx, base);
}

// ---- Attires
// How many attires a character has, and each one's name and unlock, come from
// misc.pac's COS table (BATS/INIT entry 5, by character id; 80 records, most
// taken): the mods aren't in it, so the select screen offered no CHANGE
// ATTIRE. Its lookups answer for them instead: sub_828C4140(mgr, id, mode)
// the count, sub_828C4060(mgr, id, attire) the {name string id, unlock}
// pair (unlock -1: always; sub_828C2240's "available" reads it).
REX_EXTERN(__imp__sub_828C4140);
REX_HOOK_RAW(sub_828C4140) {
  if (const Mod* m = ModOf(ctx.r4.u32); m && m->attires > 1 && m->attire_pairs) {
    ctx.r3.u64 = m->attires;
    return;
  }
  __imp__sub_828C4140(ctx, base);
}
REX_EXTERN(__imp__sub_828C4060);
REX_HOOK_RAW(sub_828C4060) {
  if (const Mod* m = ModOf(ctx.r4.u32); m && m->attires > 1 && m->attire_pairs) {
    ctx.r3.u64 = ctx.r5.u32 < m->attires ? m->attire_pairs + ctx.r5.u32 * 8 : 0;
    return;
  }
  __imp__sub_828C4060(ctx, base);
}

// ---- The select screen's model framing
// Where the panel's 3D model stands comes from a placement table by character
// id (DLC_HD/SD.pac MMDL/BIND "MMPS": per layout {id, dx, dy, dz}; scratchpad
// re_framing). Ids not in it get no offsets: a mod stood too high, its head
// cut off at the top. sub_8273D1D0(mgr, slot, layout, out) looks the slot's
// id up (slot = mgr + slot * 116: the id at +64 when +20 == 1, else +44): for
// a mod, its base's.
REX_EXTERN(__imp__sub_8273D1D0);
REX_HOOK_RAW(sub_8273D1D0) {
  const uint32_t slot = ctx.r3.u32 + ctx.r4.u32 * 116;
  const uint32_t field = slot + (Rd32(base + slot + 20) == 1 ? 64 : 44);
  const uint32_t id = Rd32(base + field);
  const Mod* m = ModOf(id);
  if (m) Wr32(base + field, m->base);
  __imp__sub_8273D1D0(ctx, base);
  if (m) Wr32(base + field, id);
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
  // (a pac overlay - move packs: mount the pacs by reading each one's own
  // table - sub_825953B0, which mounts ours too - not the pre-built directory)
  if (!svr2011::PacListFolder().empty()) {
    REXLOG_INFO("[svr2011] pac list: mounting from the pacs' own tables");
    sub_825953B0(ctx, base);
    return;
  }
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
  // the attires' COS pairs and names (string ids kAttireNameIds + mod * 4 + attire)
  uint8_t* b = memory->virtual_membase();
  for (size_t i = 0; i < g_mods.size(); ++i) {
    Mod& m = g_mods[i];
    if (m.attires < 2) continue;
    m.attire_pairs = memory->SystemHeapAlloc(32);
    for (uint32_t a = 0; a < 4; ++a) {
      uint32_t name = 9;  // ORIGINAL ATTIRE
      if (a) {
        const std::string text = m.attire_names[a].empty() ? "ATTIRE " + std::to_string(a + 1) : m.attire_names[a];
        const uint32_t at = memory->SystemHeapAlloc(uint32_t(text.size() + 1));
        std::memcpy(b + at, text.c_str(), text.size() + 1);
        name = kAttireNameIds + uint32_t(i) * 4 + a;
        g_attire_names.push_back({name, at});
      }
      Wr32(b + m.attire_pairs + a * 8, name);
      Wr32(b + m.attire_pairs + a * 8 + 4, 0xFFFFFFFFu);
    }
    REXLOG_INFO("[svr2011] superstar mods: {}: {} attires", m.name, m.attires);
  }
  // The overlay files as smods:\<file>: a device of their own, made after they
  // are written (a host device lists its folder once, so GAME: would not see
  // the files made this run).
  if ((!g_mods.empty() || !g_extra_mounts.empty()) && vfs) {
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

void AddOverlayMount(const std::string& file) { g_extra_mounts.push_back("smods:\\" + file); }

uint32_t SuperstarModString(uint32_t id) {
  for (const auto& n : g_attire_names)
    if (n.first == id) return n.second;
  return 0;
}

}  // namespace svr2011

// Test aid: SVR2011_TEST_MOVE_LOG=<id>,<id>,... logs (once per key and
// result) each motion lookup of those move ids - sub_823941F8(banks, id, x,
// y, out index): key id<<16 | x<<8 | y, 0 when no loaded bank has it. Shows
// whether a ported move's motions are found in a match.
namespace {
const std::vector<uint32_t>& MoveLogIds() {
  static const std::vector<uint32_t> ids = [] {
    std::vector<uint32_t> v;
    if (const char* e = std::getenv("SVR2011_TEST_MOVE_LOG"))
      for (const char* q = e; *q;) {
        if (const long n = std::strtol(q, nullptr, 10); n > 0) v.push_back(uint32_t(n));
        while (*q && *q != ',') ++q;
        if (*q == ',') ++q;
      }
    return v;
  }();
  return ids;
}
}  // namespace
REX_EXTERN(__imp__sub_823941F8);
REX_HOOK_RAW(sub_823941F8) {
  const auto& ids = MoveLogIds();
  const uint32_t id = ctx.r4.u32 & 0xFFFF, x = ctx.r5.u32 & 0xFF, y = ctx.r6.u32 & 0xFF;
  __imp__sub_823941F8(ctx, base);
  if (ids.empty() || std::find(ids.begin(), ids.end(), id) == ids.end()) return;
  static std::mutex mu;
  static std::vector<uint64_t> told;
  const uint64_t k = uint64_t(id) << 17 | x << 9 | y << 1 | (ctx.r3.u32 ? 1 : 0);
  std::lock_guard lock(mu);
  if (std::find(told.begin(), told.end(), k) != told.end()) return;
  told.push_back(k);
  REXLOG_INFO("[svr2011] move log: motion {} x {} y {} {}", id, x, y, ctx.r3.u32 ? "found" : "NOT FOUND");
}
// (with SVR2011_TEST_MOVE_LOG: allocations that fail, and what sub_8258EB90
// - a data file read into memory - asked for)
REX_EXTERN(__imp__sub_8269BBA0);
REX_HOOK_RAW(sub_8269BBA0) {
  const uint32_t size = ctx.r3.u32, lr = uint32_t(ctx.lr);
  __imp__sub_8269BBA0(ctx, base);
  if (!MoveLogIds().empty() && !ctx.r3.u32) REXLOG_WARN("[svr2011] move log: alloc {} bytes failed (lr {:08X})", size, lr);
}
REX_EXTERN(__imp__sub_8258EB90);
REX_HOOK_RAW(sub_8258EB90) {
  if (!MoveLogIds().empty()) REXLOG_INFO("[svr2011] move log: data read {:08X} size {}", ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8258EB90(ctx, base);
}
