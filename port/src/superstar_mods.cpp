// Superstar mods (arenas branch): up to 137 new playable characters from
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
#include "startup_pump.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <set>
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
#include "jukebox.h"
#include "media_mods.h"
#include "music.h"
#include "hd_movies.h"
#include "user_movies.h"

namespace fs = std::filesystem;

namespace {

// The ids mods take (137): disc ids whose roster record is a blank placeholder
// ("0", loaded from CHAR/DAT, not selectable) with no model, select render,
// entrance or match data anywhere in the game's pacs (61 of them; the same
// DAT / PRO entries in the disc's and the DLCs' chEtc), then the free DLC
// slots (59-69; real DLC uses 51-58). Each has a record, profile and save
// slot (the game indexes 242 ids, 0-321). (Disc ids first: a DLC slot's
// placeholder profile changes with the DLC mounted - IsBlank / RecordBlanks.)
// The 22 after 227 were added later (50 -> 72 slots); a mod keeps its id (slots.txt).
// Then 65 ids the game indexes but keeps "absent" (present flag 0x82DB3AE0 +
// id off, read only by sub_828B7078: the CHAR/DAT and CHAR/PRO loaders skip
// them, the roster getters give none): the same blank "0" placeholder records
// and profiles as the others, no model, render or titantron (108 and 109 have
// a model pac left: not used). A mod on one is made present (the hook on
// sub_828B7078 below); unused ones stay absent. 72 -> 137, the new ids after
// the old so existing mods and saves keep theirs.
constexpr uint32_t kPool[] = {111, 114, 121, 127, 128, 129, 130, 136, 141, 148, 149, 151, 152,
                              154, 155, 157, 162, 163, 167, 168, 172, 173, 181, 185, 189, 200,
                              202, 203, 204, 206, 207, 209, 213, 214, 220, 221, 223, 225, 227,
                              228, 229, 230, 232, 233, 246, 247, 252, 258, 260, 266, 269, 270,
                              273, 280, 285, 286, 302, 303, 304, 307, 320,
                              59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,
                              113, 116, 119, 120, 124, 133, 134, 138, 140, 142, 144, 146, 156,
                              159, 170, 171, 180, 183, 184, 193, 194, 196, 197, 198, 199, 210,
                              211, 212, 215, 216, 217, 222, 226, 235, 236, 237, 238, 239, 240,
                              241, 242, 243, 244, 245, 248, 249, 250, 253, 254, 259, 264, 298,
                              300, 308, 309, 310, 311, 312, 313, 314, 315, 316, 317, 318, 319};
static_assert(std::size(kPool) == 137);
constexpr uint32_t kOwnId = 32, kOwnId2 = 218;  // u16 own id in the record
constexpr uint32_t kSignIds = 210;               // u16[4]: the crowd signs its fans hold (id*10 + 1..4)
constexpr uint32_t kSelectTile = 226;            // u16: its tile in the select grid's portrait table
constexpr uint32_t kIdToIndex = 0x82DB3610;  // u16 per id
constexpr uint32_t kRecords = 0x82E407C0, kRecordSize = 260;
constexpr uint32_t kProfiles = 0x82E7C920, kProfileSize = 1056;
constexpr uint32_t kFullName = 34, kSecondName = 102, kShortName = 170, kNameLen = 32;
constexpr uint32_t kScale = 28;  // the superstar's height scale (4.12 fixed point)
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
  // its hometown for the ring announcer (manifest hometown=<CITY>[,<STATE>]:
  // the game's own clips, RA_*_HTC_<CITY>_0 / RA_*_HTS_<STATE>_0, e.g.
  // hometown=CAMERONNC or hometown=DEATHVALLEY,PARTSUNKNOWN): the CAW
  // introduction ("from <city>, <state> ... <name>") with its name calls
  std::string hometown_city, hometown_state;
  // its own moves over the base's (manifest moves=<file>: lines "0xOFF=id",
  // a byte offset in the profile's move block, 0..0x1BF, and a move id)
  std::vector<std::pair<uint16_t, uint16_t>> moves;
  int entrance = -1;            // entrance number (manifest entrance=; profile +0x1C0 +0x14..+0x18)
  int attire_entrance[4] = {-1, -1, -1, -1};  // per attire (manifest attire<N>_entrance=; EntranceForAttire)
  std::vector<uint8_t> abilities;  // (manifest abilities=a,b,...; record +230.., up to 8)
  // its height: a factor of the base's (manifest height=1.05; 0 = the base's)
  double height = 0;
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
  // (render_full.dds, optional: SSFB's own full-body picture; else SSFB is render.dds too)
  const svrfmt::Bytes full = OwnRender(folder / "render_full.dds", 512);
  auto own_for = [&](const std::string& type) -> const svrfmt::Bytes& {
    return type == "SSFC" ? small : (type == "SSFB" && !full.empty()) ? full : big;
  };
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
        const svrfmt::Bytes& own = own_for(g.type);
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
      const svrfmt::Bytes& own = own_for(g.type);
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

void InstallMedia(Mod& m, const fs::path& folder, const std::string& song, const std::string& movie, float song_gain) {
  std::error_code ec;
  const std::string name = FileName(m.name);
  if (!song.empty() && fs::exists(folder / song, ec)) {
    const fs::path music = svr2011::UserMusicFolder();
    const fs::path to = music / (name + fs::path(song).extension().string());
    if (!music.empty() && CopyIfChanged(folder / song, to)) {
      m.song = name;
      svr2011::SetSongGain(to, song_gain);
    }
  }
  // (an .mp4: played HD where the platform decodes it, else its .bik of the
  // same name - the Mod Maker makes both; hd_movies.h)
  const bool mp4 = !movie.empty() && fs::path(movie).extension() == ".mp4";
  const std::string bik = mp4 ? fs::path(movie).replace_extension(".bik").string() : movie;
  if (!movie.empty() && fs::path(bik).extension() == ".bik") {
    const fs::path movies = svr2011::UserMoviesFolder();
    const std::string file = name + ".bik";
    bool ok = !movies.empty() && fs::exists(folder / bik, ec) && CopyIfChanged(folder / bik, movies / file);
    if (mp4 && !movies.empty() && fs::exists(folder / movie, ec) && svr2011::RegisterHdMovie(file, folder / movie)) ok = true;
    if (ok) {
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
// A move the game has no usable record for (none, or one 2011 kept for a cut
// move: no category, no motion) stays the base's: such ids froze or crashed
// the game in matches and in CREATE A MOVESET (players' mods made elsewhere).
void SetOwnMoves(uint8_t* profile, const Mod& m) {
  std::string skipped;
  for (const auto& [off, id] : m.moves) {
    if (off + 1u >= kEntrance) continue;
    if (svr2011::MoveUsable(id) == 0) {
      skipped += fmt::format(" 0x{:03X}={}", off, id);
      continue;
    }
    profile[off] = uint8_t(id >> 8), profile[off + 1] = uint8_t(id);
  }
  if (!skipped.empty())
    REXLOG_WARN("[svr2011] superstar mods: {}: moves the game doesn't have, the base's kept:{}", m.folder, skipped);
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
  if (FILE* t = std::fopen((dir / "limit.txt").string().c_str(), "wb")) {  // (the launcher's count: Mods tab)
    std::fprintf(t, "%u\n", unsigned(std::size(kPool)));
    std::fclose(t);
  }
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory() && fs::exists(e.path() / "ch.pac", ec) && !fs::exists(e.path() / "disabled", ec))
      folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  const fs::path overlay = g_game / "Mods" / "SuperstarOverlay";
  fs::create_directories(overlay, ec);
  auto slots = ReadSlots(dir / "slots.txt");
  bool slots_changed = false;
  std::vector<std::string> left_out;  // (over the limit: Mods/Superstars/left_out.txt, for the launcher)
  auto slot_of = [&](const std::string& folder) -> uint32_t {
    for (const auto& s : slots)
      if (s.first == folder) return s.second;
    for (uint32_t id : kPool) {
      bool taken = false;
      for (const auto& s : slots) taken |= s.second == id;
      if (taken) continue;
      slots.push_back({folder, id});
      slots_changed = true;
      return id;
    }
    // All ids taken: one held by a mod whose folder is gone (removed by hand,
    // or by an older launcher that left its line) or switched off ("disabled":
    // the player's way to choose which mods play when there are more than
    // ids) is given to this one. Only then - a mod moved out for a while and
    // back keeps its id (and the edits saves hold for it) while there is room.
    // Mods are taken in folder order, those holding an id first in effect (an
    // id never moves from a mod that's on), so who waits is the same each start.
    for (auto& s : slots) {
      std::error_code e;
      const fs::path held = dir / fs::path(s.first);  // (folder names as written: ANSI code page)
      const bool gone = !fs::exists(held / "ch.pac", e), off = fs::exists(held / "disabled", e);
      if (!gone && !off) continue;
      REXLOG_INFO("[svr2011] superstar mods: id {} of {} (its folder is {}) goes to {}", s.second, s.first,
                  gone ? "gone" : "switched off", folder);
      s.first = folder;
      slots_changed = true;
      return s.second;
    }
    return 0;
  };
  for (const auto& f : folders) {
    svr2011::KeepWindowAlive();  // (startup_pump.h)
    Mod m;
    m.folder = f.filename().string();
    m.name = m.short_name = m.folder;
    std::string song, movie, moves;
    float song_gain = 0.0f;  // theme_gain=<dB>: the theme's level (-20..+6, the Mod Maker's loudness match)
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
        if (l.rfind("theme_gain=", 0) == 0) song_gain = std::clamp(float(std::atof(l.c_str() + 11)), -20.0f, 6.0f);
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
        if (l.rfind("hometown=", 0) == 0) {
          std::string* to = &m.hometown_city;
          for (char c : l.substr(9)) {
            if (c == ',') to = &m.hometown_state;
            else if (std::isalnum(uint8_t(c)) && to->size() < 40) to->push_back(char(std::toupper(uint8_t(c))));
          }
        }
        if (l.rfind("announcer=", 0) == 0) {
          for (char c : l.substr(10))
            if (std::isalnum(uint8_t(c)) && m.announcer.size() < 24) m.announcer.push_back(char(std::toupper(uint8_t(c))));
        }
        if (l.rfind("height=", 0) == 0) {
          const double h = std::atof(l.c_str() + 7);
          if (h > 0) m.height = std::clamp(h, 0.80, 1.25);
        }
        if (l.rfind("entrance=", 0) == 0) m.entrance = std::clamp(std::atoi(l.c_str() + 9), 0, 65535);
        if (l.rfind("attire", 0) == 0 && l.size() > 17 && l[6] >= '1' && l[6] <= '4' && !l.compare(7, 10, "_entrance="))
          m.attire_entrance[l[6] - '1'] = std::clamp(std::atoi(l.c_str() + 17), 0, 65535);
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
    InstallMedia(m, f, song, movie, song_gain);
    ReadApplied(m, f);
    m.slot = slot_of(m.folder);
    if (!m.slot) {
      REXLOG_WARN("[svr2011] superstar mods: only {} fit, {} left out (switch another mod off to make room)",
                  std::size(kPool), m.folder);
      left_out.push_back(m.folder);
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
    const std::vector<fs::path> render_srcs = {f / "manifest.txt", f / "render.dds", f / "render_small.dds",
                                               f / "render_full.dds"};
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
  {  // the mods left out at this start, one folder a line (none: no file)
    const fs::path lo = dir / "left_out.txt";
    if (left_out.empty()) {
      fs::remove(lo, ec);
    } else if (FILE* t = std::fopen(lo.string().c_str(), "wb")) {
      for (const auto& f : left_out) std::fprintf(t, "%s\n", f.c_str());
      std::fclose(t);
    }
  }
  if (slots_changed) {
    if (FILE* t = std::fopen((dir / "slots.txt").string().c_str(), "wb")) {
      for (const auto& s : slots) std::fprintf(t, "%s\t%u\n", s.first.c_str(), s.second);
      std::fclose(t);
    }
  }
}

const Mod* ModOf(uint32_t id);

// ---- Attire mods (D6): another attire for a stock superstar.
// <game>/Mods/Attires/<folder>/manifest.txt: type=attire, superstar=<id>,
// attire=<1-4>|new, name=, model=<a ch pac: its first attire is used>,
// entrance= (optional). The model pac's first attire is copied to the overlay
// with the superstar's id and the attire's digit in its EMD names
// (ch<id>_x<N>.pac), mounted with the other overlay pacs; the game finds
// models by entry name. "new" takes the first attire the superstar's own pac
// doesn't have. Four attires at most; of two mods on one attire the first
// folder (A-Z) is used. The count and the {name, unlock} pair answer for it
// (sub_828C4140 / sub_828C4060 below).
struct AttireMod {
  std::string folder, name;
  uint32_t id = 0, attire = 0;  // attire: 0-3 (the digit)
  int entrance = -1;
};
std::vector<AttireMod> g_attire_mods;
std::unordered_map<uint32_t, uint32_t> g_attire_pairs;  // id -> guest 4 x {name string id, unlock}
std::unordered_map<uint32_t, uint32_t> g_attire_own;    // id -> attires its own pac has (bit mask)
constexpr uint32_t kAttireModNameIds = 0xB400;           // string ids for attire mods' names

const AttireMod* AttireModOf(uint32_t id, uint32_t attire) {
  for (const auto& a : g_attire_mods)
    if (a.id == id && a.attire == attire) return &a;
  return nullptr;
}

void LoadAttireMods() {
  std::error_code ec;
  const fs::path dir = g_game / "Mods" / "Attires";
  const fs::path overlay = g_game / "Mods" / "SuperstarOverlay";
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory(ec) && !fs::exists(e.path() / "disabled", ec) && fs::exists(e.path() / "manifest.txt", ec))
      folders.push_back(e.path());
  std::sort(folders.begin(), folders.end());
  fs::create_directories(overlay, ec);
  for (const auto& f : folders) {
    AttireMod a;
    a.folder = f.filename().string();
    std::string type, model = "attire.pac", which;
    if (FILE* t = std::fopen((f / "manifest.txt").string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        const size_t eq = l.find('=');
        if (eq == std::string::npos || l[0] == '#') continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "type") type = v;
        else if (k == "superstar") a.id = uint32_t(std::atoi(v.c_str()));
        else if (k == "attire") which = v;
        else if (k == "name") a.name = v.substr(0, 31);
        else if (k == "model") model = v;
        else if (k == "entrance") a.entrance = std::clamp(std::atoi(v.c_str()), 0, 65535);
      }
      std::fclose(t);
    }
    if (type != "attire") continue;
    if (a.id < 100 || a.id > 321 || ModOf(a.id)) {
      REXLOG_WARN("[svr2011] attire mods: {}: superstar={} is not a stock superstar (a superstar mod takes attireN= in "
                  "its own manifest)", a.folder, a.id);
      continue;
    }
    char stock[32];
    std::snprintf(stock, sizeof stock, "ch%03u.pac", a.id);
    if (!g_attire_own.count(a.id)) g_attire_own[a.id] = AttireMask(g_game / "pac" / "ch" / stock, a.id);
    const uint32_t own = g_attire_own[a.id];
    uint32_t taken = own;
    for (const auto& o : g_attire_mods)
      if (o.id == a.id) taken |= 1u << o.attire;
    if (which == "new") {
      a.attire = 4;
      for (uint32_t k = 0; k < 4; ++k)
        if (!(taken >> k & 1)) {
          a.attire = k;
          break;
        }
      if (a.attire >= 4) {
        REXLOG_WARN("[svr2011] attire mods: {}: superstar {} has 4 attires already (attire=new needs a free one)", a.folder,
                    a.id);
        continue;
      }
    } else {
      const int n = std::atoi(which.c_str());
      if (n < 1 || n > 4) {
        REXLOG_WARN("[svr2011] attire mods: {}: attire={} (1-4 or new)", a.folder, which);
        continue;
      }
      a.attire = uint32_t(n - 1);
      if (AttireModOf(a.id, a.attire)) {
        REXLOG_WARN("[svr2011] attire mods: {}: superstar {} attire {} is taken by {} (first folder A-Z wins)", a.folder,
                    a.id, n, AttireModOf(a.id, a.attire)->folder);
        continue;
      }
    }
    char an[40];
    std::snprintf(an, sizeof an, "ch%03u_x%u.pac", a.id, a.attire + 1);
    const fs::path src = f / model;
    if (!Current(overlay / an, {src, f / "manifest.txt"})) {
      svrfmt::Bytes d;
      if (!svrfmt::ReadFile(src.string(), d) || !RenameModel(d, a.id, 0, int(a.attire)) ||
          !svrfmt::WriteFile((overlay / an).string(), d)) {
        REXLOG_WARN("[svr2011] attire mods: {}: {} is not a character model pac (EPK8 with EMD models)", a.folder, model);
        continue;
      }
      MarkCurrent(overlay / an, {src, f / "manifest.txt"});
    }
    g_extra_mounts.push_back(std::string("smods:\\") + an);
    REXLOG_INFO("[svr2011] attire mods: {} -> superstar {} attire {}{} ({})", a.folder, a.id, a.attire + 1,
                (own >> a.attire & 1) ? " (replaces the game's)" : " (new)", a.name);
    g_attire_mods.push_back(std::move(a));
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
  need += uint32_t(g_extra_mounts.size()) * 0x4000;
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
// (g_blank and g_kept grow in the loaders' hooks while CheckModProfiles reads
// them on the game thread)
std::mutex g_slots_mutex;
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

// A slot's placeholder profile can come in several versions: DLC Vol03
// ships its own chEtc.pac whose CHAR/PRO placeholders for ids 59-69 differ
// from the disc's, and the game reloads CHAR/PRO from it at each DLC scan.
// Every version the loader writes into a mod's slot is kept (RecordBlanks).
bool IsBlank(uint32_t slot, const uint8_t* profile) {
  bool any = false;
  for (const auto& b : g_blank)
    if (b.first == slot) {
      any = true;
      if (!std::memcmp(profile, b.second.data(), kProfileSize)) return true;
    }
  return !any;  // (CHAR/PRO not loaded yet: nothing of the mod's there)
}

// Before CHAR/DAT or CHAR/PRO (re)loads: the mods' slots as they are.
void Remember(uint8_t* base) {
  std::lock_guard lock(g_slots_mutex);
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

// A mod's record never has its base's select-grid tile (+226): the grid's
// tile -> record lookup takes the last record with the tile's number, the
// mod's copy (DLC, in the EXTRA list instead), and the base's tile picked
// nobody - Stone Cold, Matt Hardy, Mr. McMahon and William Regal couldn't be
// chosen (2.0.3 / 2.0.4). 0 = no tile, as the placeholders have.
// Its height (manifest height=): record +28 is the superstar's scale, 4.12
// fixed point (4096 = 1.0) - the character's skeleton is built with its bone
// lengths times it, and grapples / IK follow. Always from the base's value
// (never compounding over a kept or saved record); clamped to what the
// game's moves still fit.
void SetHeight(uint8_t* sr, const uint8_t* br, const Mod& m) {
  if (m.height <= 0) return;
  const long v = std::clamp(std::lround(Rd16(br + kScale) * m.height), 3300L, 4900L);
  if (Rd16(sr + kScale) != uint32_t(v))
    REXLOG_INFO("[svr2011] superstar mods: id {} height x{:.2f} ({} -> {})", m.slot, m.height, Rd16(br + kScale), v);
  sr[kScale] = uint8_t(v >> 8), sr[kScale + 1] = uint8_t(v);
}

void OwnSelectTile(uint8_t* sr, const uint8_t* br) {
  if (Rd16(sr + kSelectTile) && Rd16(sr + kSelectTile) == Rd16(br + kSelectTile)) sr[kSelectTile] = sr[kSelectTile + 1] = 0;
}

void ApplyRecords(uint8_t* base) {
  // Test aid: SVR2011_TEST_ID_TABLE=1 - once, the ids whose present flag
  // (0x82DB3AE0 + id, sub_828B7078) is off, with their record index and name.
  if (static bool told = false; !told && std::getenv("SVR2011_TEST_ID_TABLE")) {
    told = true;
    std::string absent;
    int n = 0;
    for (uint32_t id = 0; id < 322; ++id) {
      if (base[0x82DB3AE0 + id] == 1) continue;
      const uint32_t si = Rd16(base + kIdToIndex + id * 2);
      absent += fmt::format(" {}:{}:{}", id, si, si < 512 ? base[kRecords + si * kRecordSize + kFullName] : 0);
      ++n;
    }
    REXLOG_INFO("[svr2011] superstar mods: test - {} ids absent (id:index:name byte){}", n, absent);
  }
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
      OwnSelectTile(sr, br);  // (a save from before 2.0.5 has the base's)
      SetHeight(sr, br, m);
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
        OwnSelectTile(sr, br);
        SetHeight(sr, br, m);
        continue;
      }
    }
    // The slot changes hands (another mod's record there: its id given to this
    // one, slots.txt) - its profile, the moves and entrance a save kept for
    // the old mod, goes too: this mod's, as for a new slot.
    if (!was.empty() && !IsBlank(m.slot, sp)) {
      std::memcpy(sp, bp, kProfileSize);
      SetOwnMoves(sp, m);
      SetEntranceMedia(sp, m, bp);
      REXLOG_INFO("[svr2011] superstar mods: profile {} -> id {} (the slot held \"{}\")", m.base, m.slot, was);
    }
    std::memcpy(sr, br, kRecordSize);
    OwnSelectTile(sr, br);
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
    SetHeight(sr, br, m);

    // test aid: SVR2011_TEST_STAR_EDIT=<id> - that mod's ratings set to 20 (as
    // an edit made in the game would), to see them kept through a save
    if (const char* e = std::getenv("SVR2011_TEST_STAR_EDIT"); e && uint32_t(std::atoi(e)) == m.slot)
      for (uint32_t i = 0; i < 15; ++i) sr[i] = i == 7 ? sr[i] : 20;
    REXLOG_INFO("[svr2011] superstar mods: record {} -> id {} ({}; was \"{}\")", m.base, m.slot, m.name, was);
  }
}

// CHAR/PRO loaded: each slot's blank profile (before ApplyRecords).
// After a CHAR/PRO load: what the loader wrote into a mod's slot (changed
// from `before`) is a placeholder version of that slot - kept, unless known.
void RecordBlanks(uint8_t* base, const std::vector<svrfmt::Bytes>& before) {
  std::lock_guard lock(g_slots_mutex);
  for (size_t i = 0; i < g_mods.size() && i < before.size(); ++i) {
    const auto& m = g_mods[i];
    const uint32_t si = Rd16(base + kIdToIndex + m.slot * 2);
    if (si >= 512 || before[i].empty()) continue;
    const uint8_t* sp = base + kProfiles + si * kProfileSize;
    if (!std::memcmp(sp, before[i].data(), kProfileSize)) continue;  // (not rewritten)
    int n = 0;
    bool known = false;
    for (const auto& b : g_blank)
      if (b.first == m.slot) ++n, known |= !std::memcmp(sp, b.second.data(), kProfileSize);
    if (known) continue;
    g_blank.push_back({m.slot, svrfmt::Bytes(sp, sp + kProfileSize)});
    REXLOG_INFO("[svr2011] superstar mods: id {} placeholder profile #{} recorded", m.slot, n + 1);
  }
}

void KeepBlankProfiles(uint8_t* base) {
  std::lock_guard lock(g_slots_mutex);
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
// panels share the texture: while one shows a real DLC character (British
// Bulldog...), it stays the DLC badge and a mod's panel shows no badge (a mod
// on the other side made the DLC character "MODDED").
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
// select screen cursor -> what its panel shows: 0 no badge, 1 a mod, 2 real DLC
std::vector<std::pair<uint32_t, int>> g_panel_mod;
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
      bool dlc = false;
      for (const auto& p : g_panel_mod) want |= p.second == 1, dlc |= p.second == 2;
      want = want && !dlc;
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
  if (uint32_t(ctx.lr) != kBadgeCaller || g_badges.empty()) {
    __imp__sub_828B6CD0(ctx, base);
    return;
  }
  const uint32_t cursor = ctx.r31.u32;
  const bool mod = ModOf(ctx.r3.u32) != nullptr;
  __imp__sub_828B6CD0(ctx, base);
  const int shows = !ctx.r3.u32 ? 0 : mod ? 1 : 2;
  std::lock_guard lock(g_badge_mutex);
  g_badge_seen = NowMs();
  bool found = false, other_dlc = false;
  for (auto& p : g_panel_mod) {
    if (p.first == cursor) p.second = shows, found = true;
    else other_dlc |= p.second == 2;
  }
  if (!found) g_panel_mod.push_back({cursor, shows});
  if (shows == 1 && other_dlc) ctx.r3.u64 = 0;  // (the texture is the DLC badge now: none on the mod)
  if (!g_badge_thread) {
    g_badge_thread = true;
    std::thread(BadgeLoop).detach();
  }
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
constexpr uint32_t kIdCategories[] = {2, 4, 5, 8, 9, 40, 41, 42};
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
  for (uint32_t c = 1; c <= 45; ++c) {  // (the categories are 1..45: sub_825EA4E8)
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
// hometown=: the announcer's introduction scripts (sound.pac, sub_825EA590)
// are a Superstar's (+0 >= 50: his whole recorded introduction, category 16)
// or a Created Superstar's (... city (category 10, index +12), state (11,
// +16; left out when the city's clip names it), the nickname's name calls
// (12 / 13 / 43 / 44 / 45, and 3 "accompanied by", index +20)). A mod with a
// hometown takes the second with the city and state set, and its nickname is
// a virtual index (kNameRow + the row of its name calls by character id)
// that sub_825EA4E8 below points at those rows (8 / 9 / 40 / 41 / 42 / 2).
constexpr uint32_t kNameRow = 0x4000;  // (below 0x8000: the index is sign-extended)
constexpr uint32_t kNoState = 59;      // (a state with no clip: THE CARIBBEAN)

// The entry of category c whose name is "RA_JR_<prefix>_<token>_..." (-1: none).
int FindCall(uint8_t* base, uint32_t T, uint32_t c, const char* prefix, const std::string& token) {
  const uint32_t count = Rd32(base + T + 420 + c * 8), names = Rd32(base + T + 424 + c * 8);
  if (!names) return -1;
  const std::string want = std::string("RA_JR_") + prefix + "_" + token + "_";
  for (uint32_t i = 0; i < count; ++i)
    if (!std::strncmp(reinterpret_cast<const char*>(base + names + i * 64), want.c_str(), want.size())) return int(i);
  return -1;
}

bool Hometown(PPCContext& ctx, uint8_t* base, uint32_t out, const Mod& m) {
  const uint32_t T = CallTable(ctx, base);
  if (!T) return false;
  const int city = FindCall(base, T, 10, "HTC", m.hometown_city);
  const int state = m.hometown_state.empty() ? -1 : FindCall(base, T, 11, "HTS", m.hometown_state);
  if (city < 0 || (state < 0 && !m.hometown_state.empty())) {
    static bool told = false;
    if (!told) REXLOG_WARN("[svr2011] superstar mods: {}: no hometown clip {},{}", m.name, m.hometown_city, m.hometown_state);
    told = true;
    return false;
  }
  uint32_t nick;
  if (!m.announcer.empty()) {
    WriteAnnouncerNames(ctx, base, m);
    nick = kNameRow + m.slot;
  } else if (!m.voice.empty()) {
    WriteCallNames(ctx, base, m.slot);
    nick = kNameRow + m.slot;
  } else if (m.call >= 0) {
    nick = uint32_t(m.call);  // (a Created Superstar nickname's own calls)
  } else {
    nick = kNameRow + m.base;
  }
  Wr32(base + out, 0);  // (the Created Superstar script)
  Wr32(base + out + 12, uint32_t(city));
  Wr32(base + out + 16, state < 0 ? kNoState : uint32_t(state));
  base[out + 20] = uint8_t(nick >> 8), base[out + 21] = uint8_t(nick);
  if (NameCallLog()) REXLOG_INFO("[svr2011] name call: {} from city {} state {}, names row {:X}", m.name, city, state, nick);
  return true;
}

// sub_825EA4E8(_, &name, category, index): the announcer's event name. A
// virtual nickname (Hometown) in a nickname category: the name call of that
// row in the same kind of category by character id.
REX_EXTERN(__imp__sub_825EA4E8);
REX_HOOK_RAW(sub_825EA4E8) {
  const uint32_t name = ctx.r4.u32, c = ctx.r5.u32, i = ctx.r6.u32 & 0xFFFF;
  __imp__sub_825EA4E8(ctx, base);
  if (!name || i < kNameRow || i >= kNameRow + 0x1000) return;
  uint32_t to = 0;
  switch (c) {
    case 12: to = 8; break;
    case 13: to = 9; break;
    case 43: to = 40; break;
    case 44: to = 41; break;
    case 45: to = 42; break;
    case 3: to = 2; break;
    default: return;
  }
  const uint32_t T = CallTable(ctx, base);
  if (!T) return;
  const uint32_t row = i - kNameRow, count = Rd32(base + T + 420 + to * 8), names = Rd32(base + T + 424 + to * 8);
  if (names && row < count) Wr32(base + name, names + row * 64);
}

// A Created Superstar with a stock Superstar's name calls: <game>/Mods/
// caw_announcer.txt, lines "<his full name>=<NAME>" (a name call's clip,
// port/tools/announcer_calls.json's superstars, or one the banks still have:
// JEFFHARDY). The announcer then takes the Superstar introduction - NAME's
// own recorded one (his hometown, his name). A NAME with no row of its own
// gets the CAW's row (by its id, 0..49: "dummy", unused) written from Matt
// Hardy's with the name swapped (WriteAnnouncerNames).
constexpr uint32_t kTemplateCall = 112;  // (Matt Hardy: every category his name is in)
std::map<std::string, std::string> CawCalls() {
  static std::map<std::string, std::string> calls;
  static fs::file_time_type stamp{};
  const fs::path file = g_game / "Mods" / "caw_announcer.txt";
  std::error_code ec;
  const auto t = fs::last_write_time(file, ec);
  if (ec) {
    calls.clear();
    return calls;
  }
  if (t == stamp) return calls;
  stamp = t;
  calls.clear();
  if (FILE* f = std::fopen(file.string().c_str(), "rb")) {
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
      std::string l = line;
      while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
      const size_t eq = l.rfind('=');
      if (eq == std::string::npos || eq == 0) continue;
      std::string name;
      for (char c : l.substr(eq + 1))
        if (std::isalnum(uint8_t(c)) && name.size() < 24) name.push_back(char(std::toupper(uint8_t(c))));
      if (!name.empty()) calls[l.substr(0, eq)] = name;
    }
    std::fclose(f);
  }
  REXLOG_INFO("[svr2011] superstar mods: {} Created Superstars with stock name calls", calls.size());
  return calls;
}

// The Created Superstar in slot `slot`: his full name (roster record +34,
// UTF-16BE) and id (+32).
bool CawName(PPCContext& ctx, uint8_t* base, uint32_t slot, std::string* name, uint32_t* id) {
  const uint32_t roster = Rd32(base + 0x82EDEA88);
  if (!roster) return false;
  const auto saved = ctx;
  ctx.r3.u64 = roster;
  ctx.r4.u64 = slot;
  sub_828B5A18(ctx, base);
  const uint32_t rec = ctx.r3.u32;
  ctx = saved;
  if (!rec) return false;
  *id = uint32_t(base[rec + 32]) << 8 | base[rec + 33];
  name->clear();
  for (uint32_t k = 0; k < 40; ++k) {
    const uint16_t c = uint16_t(base[rec + 34 + k * 2]) << 8 | base[rec + 35 + k * 2];
    if (!c) break;
    name->push_back(c < 0x80 ? char(c) : '?');
  }
  return !name->empty();
}

void CawCall(PPCContext& ctx, uint8_t* base, uint32_t out, uint32_t slot) {
  const auto calls = CawCalls();
  if (calls.empty() && !NameCallLog()) return;
  std::string name;
  uint32_t id = 0;
  if (!CawName(ctx, base, slot, &name, &id) || id >= 50) return;
  if (NameCallLog()) REXLOG_INFO("[svr2011] name call: Created Superstar \"{}\" (id {})", name, id);
  const auto c = calls.find(name);
  if (c == calls.end()) return;
  const uint32_t T = CallTable(ctx, base);
  if (!T) return;
  if (const int s = FindCall(base, T, 8, "SSN", c->second); s >= 50) {
    Wr32(base + out, uint32_t(s));
    Wr32(base + out + 4, uint32_t(s));
  } else {
    Mod t;
    t.base = kTemplateCall, t.slot = id, t.announcer = c->second;
    WriteAnnouncerNames(ctx, base, t);
    Wr32(base + out, kTemplateCall);  // (the Superstar script; his rows at the CAW's id)
    Wr32(base + out + 4, id);
  }
  if (NameCallLog()) REXLOG_INFO("[svr2011] name call: Created Superstar {} (id {}) as {}", name, id, c->second);
}

REX_EXTERN(__imp__sub_825EA208);
REX_HOOK_RAW(sub_825EA208) {
  const uint32_t out = ctx.r3.u32, slot = ctx.r4.u32;
  __imp__sub_825EA208(ctx, base);
  if (!out) return;
  if (Rd32(base + out) < 50) {
    CawCall(ctx, base, out, slot);
    return;
  }
  if (NameCallLog())
    REXLOG_INFO("[svr2011] name call: announcer id {} call {} nickname {}", Rd32(base + out), Rd32(base + out + 4),
                Rd16(base + out + 20));
  const Mod* m = ModOf(Rd32(base + out));
  if (!m) return;
  if (!m->hometown_city.empty() && Hometown(ctx, base, out, *m)) return;
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
// ---- An entrance per attire (manifest attire<N>_entrance=<number>)
//
// The match takes a wrestler's entrance number from the entrance manager's
// copy of his profile block (S = mgr + 36 + idx * 280, idx = his match slot):
// sub_82756538(mgr, idx, part) the normal one (part 0: S+16 unless he has a
// created entrance, S+195 != 0), sub_82756718 / sub_82756768(mgr, idx) the
// champion and multi-title ones; sub_8275E480 then picks among them by the
// titles he holds. The stock game varies it by attire only in code (Rey
// Mysterio's "Evil" attire -> 512 in sub_8275E480). Here a mod's attire with
// its own entrance gets it from all three, so it is used with or without a
// title; a team entrance (sub_82756260) and a created entrance are left alone.
// The attire is the match slot's costume key (slot record
// *(0x82E3DDFC) + 132 + idx * 52, +16: id * 100 + attire * 10 + kind; mod
// attire N = attire digit N - 1). Research: docs/ENTRANCE_ATTIRES.md.
namespace {
constexpr uint32_t kSlotTable = 0x82E3DDFC, kSlotsAt = 132, kSlotSize = 52, kSlotKey = 16;
int EntranceForAttire(PPCContext& ctx, uint8_t* base, uint32_t mgr, uint32_t idx, bool part0) {
  if (idx >= 16) return -1;
  const uint32_t table = Rd32(base + kSlotTable);
  if (!table) return -1;
  const uint32_t key = Rd32(base + table + kSlotsAt + idx * kSlotSize + kSlotKey);
  const Mod* m = ModOf(key / 100);
  const uint32_t attire = key / 10 % 10;
  int own = -1;  // (an attire mod's entrance=)
  if (!m)
    if (const AttireMod* am = AttireModOf(key / 100, attire); am && am->entrance > 0) own = am->entrance;
  if (own <= 0 && (!m || attire >= 4 || m->attire_entrance[attire] <= 0)) return -1;
  if (part0 && base[mgr + 36 + idx * 280 + 195]) return -1;  // (a created entrance)
  const auto saved = ctx;
  ctx.r3.u64 = mgr;
  ctx.r4.u64 = idx;
  sub_82756260(ctx, base);  // (a team's entrance)
  const bool team = ctx.r3.u32 != 0;
  ctx = saved;
  if (team) return -1;
  static int logged = 0;
  const int n = own > 0 ? own : m->attire_entrance[attire];
  if (logged++ < 16)
    REXLOG_INFO("[svr2011] superstar mods: {} attire {}: entrance {}", m ? m->name : std::to_string(key / 100), attire + 1, n);
  return n;
}
}  // namespace
REX_EXTERN(__imp__sub_82756538);
REX_HOOK_RAW(sub_82756538) {
  const uint32_t mgr = ctx.r3.u32, idx = ctx.r4.u32, part = ctx.r5.u32;
  __imp__sub_82756538(ctx, base);
  if (part != 0 || ctx.r3.u32 == 0) return;
  const auto r = ctx.r3;
  if (const int n = EntranceForAttire(ctx, base, mgr, idx, true); n > 0) ctx.r3.u64 = uint32_t(n);
  else ctx.r3 = r;
}
REX_EXTERN(__imp__sub_82756718);
REX_HOOK_RAW(sub_82756718) {
  const uint32_t mgr = ctx.r3.u32, idx = ctx.r4.u32;
  __imp__sub_82756718(ctx, base);
  const auto r = ctx.r3;
  if (const int n = EntranceForAttire(ctx, base, mgr, idx, false); n > 0) ctx.r3.u64 = uint32_t(n);
  else ctx.r3 = r;
}
REX_EXTERN(__imp__sub_82756768);
REX_HOOK_RAW(sub_82756768) {
  const uint32_t mgr = ctx.r3.u32, idx = ctx.r4.u32;
  __imp__sub_82756768(ctx, base);
  const auto r = ctx.r3;
  if (const int n = EntranceForAttire(ctx, base, mgr, idx, false); n > 0) ctx.r3.u64 = uint32_t(n);
  else ctx.r3 = r;
}

// Audio events by name (sub_82BEC030(name, ...)): a mod's own announcer
// event ("Play_RA_xx_MOD_<id>_<category>", WriteCallNames) plays its
// recorded name instead (the game's sound engine has no such event: silent).
REX_EXTERN(__imp__sub_82BEC030);
REX_HOOK_RAW(sub_82BEC030) {
  if (ctx.r3.u32 >= 0x10000) {
    const char* e = reinterpret_cast<const char*>(base + ctx.r3.u32);
    // Entrance music with a live vocal: R-Truth's entrance posts
    // "Play_MUS_0272_2_0_RTruthVO" (Music.pck: two streamed sounds - the
    // theme, as "Play_MUS_0272_2_0" plays it, and his vocal), then at once
    // "Pause_start" on the same object and "Pause_Resume" a moment later. The
    // theme's stream is still opening when the pause comes and stays paused:
    // only the vocal played (B20; level 0.04 = crowd only, 0.07-0.08 with the
    // theme). The pause right after such an event is not passed on (the
    // pause/resume pair is a fraction of a second; a resume of a sound that
    // isn't paused does nothing): theme and vocal both play.
    static uint32_t vo_object = 0;
    static auto vo_at = std::chrono::steady_clock::time_point{};
    if (!std::strncmp(e, "Play_MUS_", 9)) {
      const size_t n = std::strlen(e);
      vo_object = n > 3 && !std::strcmp(e + n - 2, "VO") ? ctx.r4.u32 : 0;
      vo_at = std::chrono::steady_clock::now();
    } else if (vo_object && ctx.r4.u32 == vo_object && !std::strcmp(e, "Pause_start")) {
      vo_object = 0;
      if (std::chrono::steady_clock::now() - vo_at < std::chrono::milliseconds(500)) {
        REXLOG_INFO("[svr2011] entrance music: the pause right after the music with a vocal is not passed on");
        ctx.r3.u64 = 0x7E000002u;  // (a playing id: not 0)
        return;
      }
    }
    uint32_t id = 0;
    if (svr2011::JukeboxEvent(base, e, ctx.r4.u32)) {  // (jukebox.h: every menu song off)
      ctx.r3.u64 = 0x7E000001u;  // (a playing id: not 0)
      return;
    }
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
  const uint32_t id = ctx.r4.u32;
  __imp__sub_828C4140(ctx, base);
  if (g_attire_pairs.count(id)) {  // (attire mods: up to the last attire one of them gives)
    uint32_t n = ctx.r3.u32;
    for (const auto& a : g_attire_mods)
      if (a.id == id) n = std::max(n, a.attire + 1);
    ctx.r3.u64 = std::min(n, 4u);
  }
}
REX_EXTERN(__imp__sub_828C4060);
REX_HOOK_RAW(sub_828C4060) {
  if (const Mod* m = ModOf(ctx.r4.u32); m && m->attires > 1 && m->attire_pairs) {
    ctx.r3.u64 = ctx.r5.u32 < m->attires ? m->attire_pairs + ctx.r5.u32 * 8 : 0;
    return;
  }
  if (const auto it = g_attire_pairs.find(ctx.r4.u32); it != g_attire_pairs.end() && ctx.r5.u32 < 4 &&
      AttireModOf(ctx.r4.u32, ctx.r5.u32)) {
    ctx.r3.u64 = it->second + ctx.r5.u32 * 8;
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
// a mod, its base's. An id the table doesn't have (the managers - Stephanie,
// Teddy Long, Paul Bearer, Tiffany, The Hurricane, Hornswoggle - floated in
// the EXTRA list; a mod whose base is one of them too) takes the entry of a
// listed character of the same gender and the nearest height (record +28).
uint32_t FramingStandIn(uint8_t* base, uint32_t mgr, uint32_t layout, uint32_t id) {
  const auto rec = [&](uint32_t i) -> const uint8_t* {
    const uint32_t si = i < 512 ? Rd16(base + kIdToIndex + i * 2) : 0xFFFF;
    return si < 512 ? base + kRecords + si * kRecordSize : nullptr;
  };
  const uint8_t* r = rec(id);
  if (!r) return 0;
  const uint32_t count = Rd32(base + mgr + 788 + 44 * layout);
  const uint32_t entries = Rd32(base + mgr + 792 + 44 * layout);
  if (!entries || count > 1024) return 0;
  uint32_t best = 0, best_d = ~0u;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t c = Rd32(base + entries + i * 16);
    const uint8_t* cr = c >= 100 ? rec(c) : nullptr;  // (disc superstars; 50-69 are DLC/placeholders)
    if (!cr || cr[208] != r[208]) continue;
    const uint32_t d = uint32_t(std::abs(int(Rd16(cr + 28)) - int(Rd16(r + 28))));
    if (d < best_d) best = c, best_d = d;
  }
  return best;
}
REX_EXTERN(__imp__sub_8273D1D0);
REX_HOOK_RAW(sub_8273D1D0) {
  const uint32_t mgr = ctx.r3.u32, slot = mgr + ctx.r4.u32 * 116, layout = ctx.r5.u32;
  const uint32_t field = slot + (Rd32(base + slot + 20) == 1 ? 64 : 44);
  const uint32_t id = Rd32(base + field);
  const Mod* m = ModOf(id);
  const uint32_t look = m ? m->base : id;
  const uint64_t r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64;
  Wr32(base + field, look);
  __imp__sub_8273D1D0(ctx, base);
  if (ctx.r3.u32 == 0 && look >= 50 && layout < 14) {
    if (const uint32_t s = FramingStandIn(base, mgr, layout, look)) {
      static std::set<uint32_t> told;
      if (told.insert(look).second) REXLOG_INFO("[svr2011] select framing: id {} uses {}'s", look, s);
      ctx.r3.u64 = mgr, ctx.r4.u64 = r4, ctx.r5.u64 = r5, ctx.r6.u64 = r6;
      Wr32(base + field, s);
      __imp__sub_8273D1D0(ctx, base);
    }
  }
  Wr32(base + field, id);
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
bool g_pro_load_ran = false;  // (sub_82594C78 ran inside sub_82B89DC0)
REX_EXTERN(__imp__sub_82594C78);
REX_HOOK_RAW(sub_82594C78) {
  g_pro_load_ran = true;
  Remember(base);
  std::vector<svrfmt::Bytes> before;  // (the mods' slots before the load: RecordBlanks)
  for (const auto& m : g_mods) {
    const uint32_t si = Rd16(base + kIdToIndex + m.slot * 2);
    before.push_back(si < 512 ? svrfmt::Bytes(base + kProfiles + si * kProfileSize,
                                              base + kProfiles + (si + 1) * kProfileSize)
                              : svrfmt::Bytes());
  }
  __imp__sub_82594C78(ctx, base);
  const auto r3 = ctx.r3.u64;
  KeepBlankProfiles(base);
  RecordBlanks(base, before);
  ApplyRecords(base);
  // test aid: SVR2011_TEST_PRO_LOG=1 - each mod slot's entrance after the load
  // (music, movie, entrance number; a placeholder reads 255 255 4000)
  if (std::getenv("SVR2011_TEST_PRO_LOG"))
    for (const auto& m : g_mods) {
      const uint32_t si = Rd16(base + kIdToIndex + m.slot * 2);
      if (si >= 512) continue;
      const uint8_t* e = base + kProfiles + si * kProfileSize + kEntrance;
      REXLOG_INFO("[svr2011] superstar mods: CHAR/PRO loaded: id {} music {} movie {} entrance {}", m.slot,
                  Rd16(e + kMusic), Rd16(e + kMovie), Rd16(e + 0x14));
    }
  ctx.r3.u64 = r3;
}
// The DLC manager's restore after a DLC scan (sub_8259CAA8, its backup made
// by sub_8259C9A8): writes the DLC ids' records and profiles back - for a
// mod on 59-69 the placeholder - after the port set them up. Same as after a
// CHAR/PRO load: the new placeholder version is recorded, the mod's put back.
REX_EXTERN(__imp__sub_8259CAA8);
REX_HOOK_RAW(sub_8259CAA8) {
  Remember(base);
  std::vector<svrfmt::Bytes> before;
  for (const auto& m : g_mods) {
    const uint32_t si = Rd16(base + kIdToIndex + m.slot * 2);
    before.push_back(si < 512 ? svrfmt::Bytes(base + kProfiles + si * kProfileSize,
                                              base + kProfiles + (si + 1) * kProfileSize)
                              : svrfmt::Bytes());
  }
  __imp__sub_8259CAA8(ctx, base);
  const auto r3 = ctx.r3.u64;
  if (std::getenv("SVR2011_TEST_PRO_LOG")) REXLOG_INFO("[svr2011] superstar mods: DLC restore (sub_8259CAA8)");
  RecordBlanks(base, before);
  ApplyRecords(base);
  ctx.r3.u64 = r3;
}
// The CHAR/PRO loader's callback (sub_82B89DC0). For a DLC's chEtc (DLC
// Vol03 at each DLC scan) it writes the DLC ids' profiles - placeholders for
// a mod on 59-69 - without reaching sub_82594C78: then, as after a CHAR/PRO
// load, the new placeholder version is recorded and the mods put back. (When
// sub_82594C78 ran, its hook did that.)
REX_EXTERN(__imp__sub_82B89DC0);
REX_HOOK_RAW(sub_82B89DC0) {
  std::vector<svrfmt::Bytes> before;
  for (const auto& m : g_mods) {
    const uint32_t si = Rd16(base + kIdToIndex + m.slot * 2);
    before.push_back(si < 512 ? svrfmt::Bytes(base + kProfiles + si * kProfileSize,
                                              base + kProfiles + (si + 1) * kProfileSize)
                              : svrfmt::Bytes());
  }
  g_pro_load_ran = false;
  __imp__sub_82B89DC0(ctx, base);
  const auto r3 = ctx.r3.u64;
  if (!g_pro_load_ran) {
    if (std::getenv("SVR2011_TEST_PRO_LOG"))
      REXLOG_INFO("[svr2011] superstar mods: CHAR/PRO written without a load (a DLC's chEtc)");
    RecordBlanks(base, before);
    ApplyRecords(base);
  }
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_8259C9A8);
REX_HOOK_RAW(sub_8259C9A8) {
  if (std::getenv("SVR2011_TEST_PRO_LOG")) REXLOG_INFO("[svr2011] superstar mods: DLC backup (sub_8259C9A8)");
  __imp__sub_8259C9A8(ctx, base);
}
REX_EXTERN(__imp__sub_8257A218);
REX_HOOK_RAW(sub_8257A218) {
  __imp__sub_8257A218(ctx, base);
  const auto r3 = ctx.r3.u64;
  ApplyRecords(base);
  ctx.r3.u64 = r3;
}

// WWE Universe's title holders: 14 titles x 2 u16 ids (a tag title's two;
// 0x200 none) at save + 0x7CD70 (SaveData.dat 0x7CD88, after its 0x18-byte
// header). A mod has no place in Universe (above), so a title it holds can't
// be booked - "gave Kenny Omega the Intercontinental title: holder blank,
// nobody could fight for it" - yet its TITLE SETTINGS page gives it one.
// Checked once a frame: a holder that just became a mod gets the one before
// back; one a save already holds (none before) leaves the title vacant, as a
// title nobody holds.
namespace {
void KeepModsOffTitles(uint8_t* base) {
  constexpr uint32_t kSave = 0x82E407B4, kTitles = 0x7CD70, kSlots = 28, kNone = 0x200;
  static uint32_t last[kSlots], last_save = 0;
  const uint32_t save = Rd32(base + kSave);
  if (!save || g_mods.empty()) return;
  if (save != last_save) {
    std::fill(std::begin(last), std::end(last), kNone);
    last_save = save;
  }
  for (uint32_t i = 0; i < kSlots; ++i) {
    uint8_t* p = base + save + kTitles + i * 2;
    uint32_t id = Rd16(p);
    if (const Mod* m = ModOf(id)) {
      const uint32_t back = ModOf(last[i]) ? kNone : last[i];
      p[0] = uint8_t(back >> 8), p[1] = uint8_t(back);
      REXLOG_WARN("[svr2011] superstar mods: Universe title {} (holder {}) can't go to {} (id {}): {}", i / 2,
                  i % 2 + 1, m->name, id, back == kNone ? std::string("left vacant") : "back to id " + std::to_string(back));
      id = back;
    }
    last[i] = id;
  }
}
}  // namespace

// Once a frame (the game's pad reads, frame_rate.cpp): a mod's profile that
// is a placeholder again gets the mod's back. A save's copy of a slot can be
// the placeholder (saved before the mod was installed): loading it put Jeff
// Hardy's back to it after the hooks above ran, and CREATE A MOVESET - no
// roster list (below) on the way - read the placeholder's finisher 15271, a
// move that doesn't exist (crash, 2.0.3). A save's edited copy is no
// placeholder and stays.
namespace svr2011 {
void CheckModProfiles(uint8_t* base) {
  KeepModsOffTitles(base);
  std::lock_guard lock(g_slots_mutex);
  for (const auto& m : g_mods) {
    const uint32_t si = Rd16(base + kIdToIndex + m.slot * 2);
    if (si >= 512) continue;
    bool have_blank = false;
    for (const auto& b : g_blank) have_blank |= b.first == m.slot;
    if (have_blank && IsBlank(m.slot, base + kProfiles + si * kProfileSize)) {
      ApplyRecords(base);
      return;
    }
  }
  // Once the move records are loaded: a mod's own move the game has no usable
  // record for (SetOwnMoves may have run before they were) goes back to the
  // base's - only where the slot still holds the mod's id, never an edit.
  static bool checked = false;
  if (checked || MoveUsable(0) < 0) return;
  checked = true;
  for (const auto& m : g_mods) {
    const uint32_t si = Rd16(base + kIdToIndex + m.slot * 2), bi = Rd16(base + kIdToIndex + m.base * 2);
    if (si >= 512 || bi >= 512) continue;
    uint8_t* sp = base + kProfiles + si * kProfileSize;
    const uint8_t* bp = base + kProfiles + bi * kProfileSize;
    std::string fixed;
    for (const auto& [off, id] : m.moves) {
      if (off + 1u >= kEntrance || Rd16(sp + off) != id || MoveUsable(id) != 0) continue;
      sp[off] = bp[off], sp[off + 1] = bp[off + 1];
      fixed += fmt::format(" 0x{:03X}={}", off, id);
    }
    if (!fixed.empty())
      REXLOG_WARN("[svr2011] superstar mods: {}: moves the game doesn't have, the base's put back:{}", m.folder, fixed);
  }
}
}  // namespace svr2011

// The select lists' order: sub_82736E00 (the team editor's member list) and
// sub_82734560 (exhibition's) rank each id by its record's +226 (the grid
// tile number: rank = how many keys are smaller), and equal keys take the
// same row. Mods have no tile (0, OwnSelectTile): all of them took one row
// (the last mod won) and the rows they left kept id 0 - a nameless created
// superstar ("DEFAULT"): team members couldn't be picked as the player
// meant, exhibition showed generics (2.0.4). While those sorts run, each
// mod's record has a key of its own after every real one (0x7000 + n).
void ModSortKeys(uint8_t* base, bool on) {
  uint32_t n = 0;
  for (const auto& m : g_mods) {
    const uint32_t si = Rd16(base + kIdToIndex + m.slot * 2);
    if (si >= 512) continue;
    uint8_t* key = base + kRecords + si * kRecordSize + kSelectTile;
    const uint32_t mine = 0x7000 + n++;
    if (on && !Rd16(key)) key[0] = uint8_t(mine >> 8), key[1] = uint8_t(mine);
    else if (!on && Rd16(key) == mine) key[0] = key[1] = 0;
  }
}
REX_EXTERN(__imp__sub_82736E00);
REX_HOOK_RAW(sub_82736E00) {
  ModSortKeys(base, true);
  __imp__sub_82736E00(ctx, base);
  const auto r3 = ctx.r3.u64;
  ModSortKeys(base, false);
  ctx.r3.u64 = r3;
}
REX_EXTERN(__imp__sub_82734560);
REX_HOOK_RAW(sub_82734560) {
  ModSortKeys(base, true);
  __imp__sub_82734560(ctx, base);
  const auto r3 = ctx.r3.u64;
  ModSortKeys(base, false);
  ctx.r3.u64 = r3;
}

// A roster list is about to be built.
REX_EXTERN(__imp__sub_82736C68);
REX_HOOK_RAW(sub_82736C68) {
  ApplyRecords(base);
  __imp__sub_82736C68(ctx, base);
}

// Owned (DLC-flagged ids must be): mods are - but not for WWE Universe's
// rankings (sub_825AF0F0's loop) and its owned snapshot (sub_825CA488): see
// below.
constexpr uint32_t kUniverseOwnedCallers[] = {0x825AF190, 0x825CA518};
REX_EXTERN(__imp__sub_828C1288);
REX_HOOK_RAW(sub_828C1288) {
  if (ModOf(ctx.r3.u32)) {
    const uint32_t lr = uint32_t(ctx.lr);
    bool universe = false;
    for (uint32_t c : kUniverseOwnedCallers) universe |= lr == c;
    ctx.r3.u64 = universe ? 0 : 1;
    return;
  }
  __imp__sub_828C1288(ctx, base);
}

// ---- WWE Universe: no mods
// Universe keeps 150 superstars (P = *(0x82E407B4) + 0x364B4: 24-byte
// entries at P+16 - 0-49 created, 50-69 DLC, 70-149 a disc list of 80 ids at
// 0x82DACA40 - a rivalry byte per pair at P+3616, match cards from P+14942).
// sub_825B7660(id) gives an id's place: created and DLC ids their own number,
// disc-list ids 70 + their index. Mods passed sub_825B75A8 (eligible: owned,
// selectable, DLC-flagged) and took their id as place: mods 151-181 wrote
// past the 150 entries into the rivalry table and the first match card,
// 111-149 shared real superstars' entries ("Universe with the SvR 2010 mods
// crashes", 2.0.4/2.0.5). Mods stay out of Universe: not eligible, no place,
// no pair (sub_825B7B40); every caller handles "none" (-1 / 0).
// "Is this id present" (sub_828B7078(id): the flag 0x82DB3AE0 + id, read by
// the CHAR/DAT and CHAR/PRO loaders and the roster getters): a mod's id is,
// so the 65 ids the game keeps absent load their placeholder record and
// profile and show like the other mod ids when a mod takes one.
REX_EXTERN(__imp__sub_828B7078);
REX_HOOK_RAW(sub_828B7078) {
  if (ctx.r3.u32 < 322 && ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_828B7078(ctx, base);
}

REX_EXTERN(__imp__sub_825B75A8);
REX_HOOK_RAW(sub_825B75A8) {  // (id, need owned) -> in Universe
  if (ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_825B75A8(ctx, base);
}
REX_EXTERN(__imp__sub_825B7660);
REX_HOOK_RAW(sub_825B7660) {  // (id) -> Universe place, -1: none
  if (ModOf(ctx.r3.u32)) {
    ctx.r3.u64 = uint64_t(int64_t(-1));
    return;
  }
  __imp__sub_825B7660(ctx, base);
}
REX_EXTERN(__imp__sub_825B7B40);
REX_HOOK_RAW(sub_825B7B40) {  // (id, id) -> both in Universe
  if (ModOf(ctx.r3.u32) || ModOf(ctx.r4.u32)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_825B7B40(ctx, base);
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
    const std::string mp4 = !movie.empty() && fs::path(movie).extension() == ".mp4" ? movie : std::string();
    if (!mp4.empty()) movie = fs::path(movie).replace_extension(".bik").string();
    if (!movie.empty() && fs::path(movie).extension() == ".bik" && fs::exists(e.path() / movie, ec))
      CopyIfChanged(e.path() / movie, movies / (FileName(name) + ".bik"));  // (an .mp4's: its .bik fallback)
    // (an .mp4: its stub and probe now, before the mount - hd_movies.h)
    if (!mp4.empty() && fs::exists(e.path() / mp4, ec))
      svr2011::PrepareHdMovie(movies, FileName(name) + ".bik", e.path() / mp4);
  }
}

void InstallSuperstarMods(rex::memory::Memory* memory, rex::filesystem::VirtualFileSystem* vfs) {
  g_memory = memory;
  // test aid (SVR2011_TEST_PRO_LOG): log when a mod slot's entrance number
  // changes in the CHAR/PRO table (who overwrites a mod's profile, and when)
  if (std::getenv("SVR2011_TEST_PRO_LOG"))
    std::thread([memory] {
      std::vector<uint32_t> last(64, 0xFFFFFFFF);
      for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        uint8_t* base = memory->virtual_membase();
        for (size_t i = 0; i < g_mods.size() && i < last.size(); ++i) {
          const uint32_t si = Rd16(base + kIdToIndex + g_mods[i].slot * 2);
          if (si >= 512) continue;
          const uint32_t e = Rd16(base + kProfiles + si * kProfileSize + kEntrance + 0x14);
          if (e != last[i]) {
            REXLOG_INFO("[svr2011] superstar mods: watch: id {} entrance {} -> {}", g_mods[i].slot, last[i], e);
            last[i] = e;
          }
        }
      }
    }).detach();
  g_game = rex::filesystem::GetExecutableFolder();
  LoadMods();
  LoadAttireMods();
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
  // attire mods' pairs: their names (string ids kAttireModNameIds + k), always available
  for (size_t k = 0; k < g_attire_mods.size(); ++k) {
    const AttireMod& a = g_attire_mods[k];
    uint32_t& pairs = g_attire_pairs[a.id];
    if (!pairs) {
      pairs = memory->SystemHeapAlloc(32);
      for (uint32_t i = 0; i < 4; ++i) Wr32(b + pairs + i * 8, 9), Wr32(b + pairs + i * 8 + 4, 0xFFFFFFFFu);
    }
    const std::string text = a.name.empty() ? "ATTIRE " + std::to_string(a.attire + 1) : a.name;
    const uint32_t at = memory->SystemHeapAlloc(uint32_t(text.size() + 1));
    std::memcpy(b + at, text.c_str(), text.size() + 1);
    const uint32_t name = kAttireModNameIds + uint32_t(k);
    g_attire_names.push_back({name, at});
    Wr32(b + pairs + a.attire * 8, name);
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
std::string SuperstarModFolder(uint32_t id) {
  const Mod* m = ModOf(id);
  return m ? m->folder : std::string();
}
uint32_t SuperstarModBase(uint32_t id) {
  const Mod* m = ModOf(id);
  return m ? m->base : 0;
}

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
// Test aid: SVR2011_TEST_MOTION_SWAP=<id>=<new id>[,...] - a lookup of a motion
// with that id (any x, body tracks y 0/1) gets the new id's (x 0) instead: the
// select screen's model, or a stance, plays a chosen motion (an in-game
// motion viewer with a fixed camera).
const std::vector<std::pair<uint32_t, uint32_t>>& MotionSwaps() {
  static const std::vector<std::pair<uint32_t, uint32_t>> v = [] {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (const char* e = std::getenv("SVR2011_TEST_MOTION_SWAP"))
      for (const char* q = e; *q;) {
        char* end = nullptr;
        const unsigned long a = std::strtoul(q, &end, 10);
        if (end == q || *end != '=') break;
        const unsigned long b = std::strtoul(end + 1, &end, 10);
        out.push_back({uint32_t(a), uint32_t(b)});
        for (q = end; *q == ',' || *q == ' ';) ++q;
      }
    return out;
  }();
  return v;
}
// A bank whose keys are no longer all in order: a move pack's new YMBs motions
// go after the stock entries (move_packs.cpp: the game keeps YMBs motions by
// their place), and the game's binary search (sub_823941F8) then missed stock
// keys whose search path ran into that tail - with Sabu or The Sandman
// installed, Surfboard Stretch (21201) wasn't found: ground submissions froze
// in matches and in the Move-Set preview (camera left on the apron). A miss in
// such a bank is looked up again: binary search over the sorted part, then the
// tail one by one. Returns the bank's +272 (as the game's) and *index; 0 if
// none has the key.
namespace {
uint8_t* GuestAt(uint8_t* base, uint32_t a) { return base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
struct BankOrder { uint32_t count = 0, last = 0, sorted = 0; };  // sorted: entries in order from 0
std::mutex g_order_mutex;
std::unordered_map<uint32_t, BankOrder> g_order;  // bank -> its order (checked again when count or last key change)
uint32_t SortedPrefix(uint8_t* base, uint32_t bank, uint32_t count) {
  const uint8_t* e = GuestAt(base, bank + 276);
  const uint32_t last = Rd32(e + (count - 1) * 16);
  std::lock_guard lock(g_order_mutex);
  BankOrder& o = g_order[bank];
  if (o.count == count && o.last == last && o.sorted) return o.sorted;
  uint32_t n = 1;
  while (n < count && Rd32(e + n * 16) > Rd32(e + (n - 1) * 16)) ++n;
  o = {count, last, n};
  return n;
}
uint32_t FindInUnsortedBanks(uint8_t* base, uint32_t banks, uint32_t key, uint32_t out) {
  constexpr uint32_t kYmks = 0x734B4D59, kYmbs = 0x73424D59;  // (as the game reads them)
  for (uint32_t k = 0; k < 68; ++k) {
    uint32_t b = Rd32(GuestAt(base, banks + 4 * k));
    if (!b) continue;
    const uint32_t magic = Rd32(GuestAt(base, b));
    if (magic != kYmks && magic != kYmbs) b -= 16;
    const uint32_t count = Rd32(GuestAt(base, b + 272));
    if (!count || count > 20000) continue;
    const uint32_t sorted = SortedPrefix(base, b, count);
    if (sorted >= count) continue;  // (in order: the game's search was right)
    const uint8_t* e = GuestAt(base, b + 276);
    int lo = 0, hi = int(sorted) - 1, at = -1;
    while (lo <= hi) {
      const int mid = (lo + hi) / 2;
      const uint32_t v = Rd32(e + mid * 16);
      if (v == key) { at = mid; break; }
      if (v < key) lo = mid + 1; else hi = mid - 1;
    }
    for (uint32_t i = sorted; at < 0 && i < count; ++i)
      if (Rd32(e + i * 16) == key) at = int(i);
    if (at < 0) continue;
    if (out) {
      uint8_t* q = GuestAt(base, out);
      q[0] = uint8_t(uint32_t(at) >> 24), q[1] = uint8_t(uint32_t(at) >> 16), q[2] = uint8_t(uint32_t(at) >> 8), q[3] = uint8_t(at);
    }
    return b + 272;
  }
  return 0;
}
}  // namespace

REX_EXTERN(__imp__sub_823941F8);
REX_HOOK_RAW(sub_823941F8) {
  const auto& ids = MoveLogIds();
  uint32_t id = ctx.r4.u32 & 0xFFFF, x = ctx.r5.u32 & 0xFF;
  const uint32_t y = ctx.r6.u32 & 0xFF;
  for (const auto& [from, to] : MotionSwaps())
    if (id == from && y <= 1) {
      id = to, x = 0;
      ctx.r4.u64 = to;
      ctx.r5.u64 = 0;
    }
  const uint32_t banks = ctx.r3.u32, out = ctx.r7.u32;
  __imp__sub_823941F8(ctx, base);
  if (!ctx.r3.u32 && banks) {
    static const bool off = std::getenv("SVR2011_TEST_YMBS_SEARCH_OFF") != nullptr;  // (test aid: the game's search only)
    if (!off) ctx.r3.u64 = FindInUnsortedBanks(base, banks, id << 16 | x << 8 | y, out);
  }
  static const bool all = [] { const char* e = std::getenv("SVR2011_TEST_MOVE_LOG"); return e && !std::strcmp(e, "all"); }();
  if (all) {  // (SVR2011_TEST_MOVE_LOG=all: every lookup once, found or not)
    static std::mutex amu;
    static std::vector<uint32_t> atold;
    const uint32_t k = id << 16 | x << 8 | y;
    std::lock_guard lock(amu);
    if (std::find(atold.begin(), atold.end(), k) == atold.end()) {
      atold.push_back(k);
      REXLOG_INFO("[svr2011] move log: lookup {} x {} y {} {}", id, x, y, ctx.r3.u32 ? "found" : "NOT FOUND");
    }
    return;
  }
  // (SVR2011_TEST_MOVE_LOG=missing: every body-track motion - y 0 / 1 - no
  // loaded bank has, for any id: a move that snaps instead of playing)
  static const bool missing = [] { const char* e = std::getenv("SVR2011_TEST_MOVE_LOG"); return e && !std::strcmp(e, "missing"); }();
  if (missing ? (ctx.r3.u32 || y > 1) : (ids.empty() || std::find(ids.begin(), ids.end(), id) == ids.end())) return;
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

// Test aid (SVR2011_TEST_MOVE_LOG set): the per-superstar match bank's move id
// list (sub_8224D8C8(obj): u32 ids at obj+116, sorted, up to 267 read, 32767
// closing it) - how full it is.
REX_EXTERN(__imp__sub_8224CFD0);
REX_HOOK_RAW(sub_8224CFD0) {
  static const bool on = std::getenv("SVR2011_TEST_MOVE_LOG") != nullptr;
  if (on) {
    const uint32_t obj = ctx.r3.u32;
    __imp__sub_8224CFD0(ctx, base);
    uint32_t n = 0, last = 0, end = 0;
    for (; n < 400; ++n) {
      const uint32_t v = Rd32(base + obj + 116 + 4 * n);
      if (v == 32767) { end = 1; break; }
      if (v == 0xDEADBEEF) break;
      last = v;
    }
    REXLOG_INFO("[svr2011] move log: match bank id list {} entries{} (last {}), read limit 267", n,
                end ? " + 32767" : " (no 32767)", last);
    return;
  }
  __imp__sub_8224CFD0(ctx, base);
}
