// The Full preview (full_preview.h): a whole entrance in the practice arena.
//
// An entrance n is a script EVP? n and a track bank EVT? n (port/docs/HCTP_ENTRANCES.md): in the
// project's own move pack, an installed one (pack.txt "pacentry <pac> EVPx <n> <file>") or the game's
// pac\evt\Nyujyo*.pac - the first found, as the game (a mod's entry wins). The script's objects: the
// entrant is the first character (type 01000000 00000000), the camera the first camera (00010000 0).
// A track's key in the bank is (25000 + object, x = slot, y); the segments are slots 0 / 5 / 10 / 15 / 20,
// played one after another (0 = the hidden intro: the entrant isn't drawn, the titantron plays). The
// entrant's y0 = body (char_preview::DecodeClip, with y2 / y3 the fingers); the camera's y1 (else y0; the game picks one of the two) =
// 29-byte mode-1 keys [count][f32 pos x3, 0.01 units][f32 rot x3, rad][f32 fov] (tools/ymks.py
// decode_camera): R = Rz Ry Rx, looking down the camera's local -x, its local -y the up; count = the
// frames it takes to get there from where it was (0: a cut). Op 112 `70 z r` zooms (target z / 10,
// rate r / 1000 a frame; 0 snaps): fov = 2 atan(0.625 / zoom) x 0.82, else the key's 2 atan(0.8 tan(F/2))
// (docs: HCTP_ENTRANCE_EVENTS.md section 3). Keys are 30 a second, the clock of every track.
// The theme: the mod's song= file, else the superstar's profile's (chEtc.pac CHAR/PRO, entrance block
// +0x1C0: +0x10 music, +0x12 movie): sound\Music.pck Play_MUS_<music>_0_0 through tools/wwise.py (Python +
// vgmstream + ffmpeg), cached as Mods\.convert\full_preview\music_<n>.mp3. The titantron: movie= else
// movies\titantron\<movie>.bik, shown in a corner (the practice arena has no screen).
// The scene: the entrant's and the camera's tracks share one frame (every stock camera key aims at the root
// as keyed) whose y 0 is the ring mat, the stage parts walking a floor-level stage; both move by the mat
// height (-12) into the arena's space, plus the arena's stage / ramp height under the entrant (the practice
// arena's stage is 1.8 m up: StageGrid from its meshes), as the game's own entrances there show. The view
// is left-handed like the game's (x flipped in the projection: the writing on shirts and barriers reads the
// right way round, checked against the game's shots of the same entrances, 14 Oct).
#ifndef NOMINMAX
#define NOMINMAX  // (std::min / std::max, not windows.h's macros)
#endif
#include "full_preview.h"

#include <d3d11.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "app.h"
#include "audio_out.h"
#include "char_preview.h"
#include "editor.h"
#include "imgui.h"
#include "svrfmt/arena.h"
#include "svrfmt/pac.h"
#include "tool_run.h"
#include "video_mf.h"
extern "C" {
#include "../launcher/bink_decode.h"
}

namespace mm {
namespace full_preview {
namespace {

constexpr int kSlots[5] = {0, 5, 10, 15, 20};
constexpr int kIdShift = 25000;
constexpr float kRootUnit = 0.01f;

struct CamKey {
  int frame = 0, count = 0;
  float pos[3] = {}, rot[3] = {}, fov = 0.7217f;
};
struct ZoomEvent {
  int frame = 0;
  float target = 1, rate = 0;
};
// A prop's state at a frame of its track: its own bones, its offset, shown, how it's held (op 180-191's value).
struct PropKey {
  int frame = 0;
  char_preview::PropBones bones;
  float pos[3] = {0, 0, 0};
  bool shown = true;
  int state = 1;
};
struct PropTrack {
  int prop = -1;  // char_preview's index
  bool belt = false;  // (a title belt: not drawn, see the view's callback)
  std::vector<PropKey> keys;  // one per keyed frame, in order (each holds the state reached)
};
struct Seg {
  int slot = 0, keys = 0;
  std::vector<PropTrack> props;
  std::shared_ptr<char_preview::Clip> body;
  std::vector<float> lift;  // per key: the y added to put the entrant on the practice arena's ground (Ground)
  std::vector<CamKey> cam;
  std::vector<ZoomEvent> zoom;
  int height_ref = -1;  // op 113 `71 c`: from this frame the camera follows the entrant's height (-1: not)
};
// A ringside object the game puts out for the match (the steel steps): bgEtc.pac STG/WPON pack <model>, placed by
// the ringside set's record (WPON 45010, key 1000: f32 position x3, f32 rotation x3 in degrees, s16 motion, u8
// place, u8 model; the steps are model 32 at the +x corners).
struct Static {
  int prop = -1;
  float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, t[3] = {0, 0, 0};
};
struct Loaded {
  std::vector<Static> statics;
  int number = -1;
  std::string name, source, error;
  std::string pyro;  // the entrance's own pyro set, in words ("" none): not drawn (PRTC isn't decoded)
  std::vector<Seg> segs;
  int total = 0;
  bool fingers = false;
  bool grounded = false;  // (Ground done, with the model loaded)
  std::unique_ptr<svrfmt::Arena> arena;
  int music = -1, movie = -1;  // the profile's (-1: none)
  float scale = 1;  // the superstar's size (chEtc.pac CHAR/DAT +28, 4.12 fixed point) x a mod's height=
};

// Where part `part` (0-4; 4 the walk to the ring) starts on the timeline: the keys of the parts before it (0 when it
// has no keys of its own).
float PartStart(const Loaded& L, int part) {
  if (part <= 0 || part >= 5) return 0;
  float k = 0;
  bool has = false;
  for (const Seg& sg : L.segs)
    if (sg.slot < kSlots[part]) k += float(sg.keys);
    else has = true;
  return has ? k : 0;
}

Request g_req;
bool g_test_view_on = false;  // (test aid --full-preview-view)
float g_test_view[16], g_test_fov = 30, g_test_xs = 1;  // (x sign of the projection)
FILE* g_dump = nullptr;  // (test aid --full-preview-dump: every key's camera and bones, then closed)
std::wstring g_dump_file;
float g_dump_done = -1;
bool g_open = false;
std::atomic<int> g_load_state{0};  // 0 idle, 1 loading, 2 loaded (g_next ready)
std::unique_ptr<Loaded> g_next, g_cur;
std::mutex g_mu;
int g_gen = 0;  // (an older load still running is dropped)

float g_key = 0;  // the timeline, in keys (30 a second) over every segment
bool g_playing = false;
bool g_use_camera = true, g_titantron_on = true, g_music_on = true;
int g_saved_attire = 1;
bool g_saved_gear = false;
float g_saved_height = 1;  // (the pages' model size, put back on Close)
std::string g_music_note;  // why there's no theme ("" = there is / loading)
int g_seg_now = 0;
float g_local_key = 0;

// the titantron
BinkReader* g_bink = nullptr;
ID3D11Texture2D* g_tt_tex = nullptr;
ID3D11ShaderResourceView* g_tt_srv = nullptr;
std::vector<uint8_t> g_tt_bgra;
int g_tt_shown = -1;
std::string g_tt_note;  // why the titantron can't show ("" = it shows, or there's none)
std::unique_ptr<video_mf::Reader> g_tt_mf;  // (a movie Bink 1 can't open: a mod's movie.mp4, through Media Foundation)
bool g_tt_screen = false;  // the arena has a titantron screen (tf_titan): the movie plays on it, not in a corner

// the test aid
bool g_test_pending = false;
int g_test_entrance = -1, g_test_star = -1;
float g_test_key = -1;
bool g_test_camera = true;
std::wstring g_test_song, g_test_movie;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
float LeF(const uint8_t* p) {
  const uint32_t u = Le32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// ---- finding the entrance

// A pack folder's EVP / EVT files for entrance n (its pack.txt pacentry lines).
bool FromPack(const fs::path& pack, int n, Bytes& evp, Bytes& evt) {
  Bytes t;
  if (!svrfmt::ReadFile(PathStr(pack / L"pack.txt"), t)) return false;
  const std::string s(t.begin(), t.end()), want = std::to_string(n);
  fs::path fp, ft;
  for (size_t at = 0; at < s.size();) {
    size_t e = s.find('\n', at);
    if (e == std::string::npos) e = s.size();
    std::string line = s.substr(at, e - at);
    at = e + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    char pac[260], group[16], name[64], file[260];
    if (std::sscanf(line.c_str(), "pacentry %259s %15s %63s %259s", pac, group, name, file) != 4 || want != name) continue;
    if (!std::strncmp(group, "EVP", 3)) fp = pack / Wide(file);
    if (!std::strncmp(group, "EVT", 3)) ft = pack / Wide(file);
  }
  if (fp.empty() || ft.empty()) return false;
  Bytes a, b;
  if (!svrfmt::ReadFile(PathStr(fp), a) || !svrfmt::ReadFile(PathStr(ft), b)) return false;
  evp = svrfmt::Unpack(a), evt = svrfmt::Unpack(b);
  return true;
}

bool FindEntrance(const std::wstring& game, const std::wstring& own_pack, int n, Bytes& evp, Bytes& evt, std::string& from) {
  std::error_code ec;
  if (!own_pack.empty() && FromPack(own_pack, n, evp, evt)) return from = "the project's moves", true;
  for (const wchar_t* where : {L"Moves", L"Superstars"})
    for (const auto& e : fs::directory_iterator(fs::path(game) / L"Mods" / where, ec)) {
      if (!e.is_directory(ec) || fs::exists(e.path() / L"disabled", ec)) continue;
      const fs::path pack = std::wstring(where) == L"Moves" ? e.path() : e.path() / L"moves";
      if (FromPack(pack, n, evp, evt)) return from = PathStr(fs::path(where) / e.path().filename()), true;
    }
  std::vector<fs::path> pacs;
  for (const auto& e : fs::directory_iterator(fs::path(game) / L"pac" / L"evt", ec))
    if (Lower(PathStr(e.path().filename())).rfind("nyujyo", 0) == 0) pacs.push_back(e.path());
  std::sort(pacs.begin(), pacs.end());
  for (const auto& p : pacs) {
    PacIndex idx;
    if (!ReadPacIndex(p, idx)) continue;
    const PacEntryInfo *ep = nullptr, *et = nullptr;
    for (const auto& e : idx.entries) {
      if (e.name.empty() || !std::isdigit(uint8_t(e.name[0])) || std::atoi(e.name.c_str()) != n) continue;  // (names padded)
      if (e.group.rfind("EVP", 0) == 0) ep = &e;
      if (e.group.rfind("EVT", 0) == 0) et = &e;
    }
    Bytes a, b;
    if (ep && et && ReadPacEntry(p, *ep, a) && ReadPacEntry(p, *et, b)) {
      evp = svrfmt::Unpack(a), evt = svrfmt::Unpack(b);
      return from = "pac\\evt\\" + PathStr(p.filename()), true;
    }
  }
  return false;
}

// A track's data in the bank (start, size, frames from its directory entry).
bool Track(const Bytes& b, int id, int x, int y, const uint8_t** d, size_t* size, int* frames) {
  if (b.size() < 0x114 || std::memcmp(b.data(), "YMKs", 4)) return false;
  const uint32_t n = Le32(&b[0x110]);
  const size_t base = 0x114 + 16 * size_t(n);
  if (base > b.size()) return false;
  const size_t dl = b.size() - base;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* e = &b[0x114 + 16 * i];
    if (e[0] != y || e[1] != x || Le16(e + 2) != id) continue;
    const uint32_t off = Le32(e + 4);
    if (off >= dl) return false;
    size_t end = dl;
    for (uint32_t j = 0; j < n; ++j) {
      const uint32_t o = Le32(&b[0x114 + 16 * j + 4]);
      if (o > off && o < end) end = o;
    }
    *d = &b[base + off], *size = end - off, *frames = int(Le32(e + 8));
    return true;
  }
  return false;
}

// A camera track: its keys (mode 1, 29 bytes; mode 0, 13 bytes) and its zoom events (op 112).
void DecodeCamera(const Bytes& bank, const uint8_t* d, size_t size, Seg& s) {
  int frame = 0;
  for (size_t p = 0; p < size;) {
    const uint8_t c = d[p];
    if (c < 0x60) { frame += c, ++p; continue; }
    size_t sz;
    if (c == 0x60) {
      if (p + 3 > size) break;
      sz = size_t(d[p + 1]) | size_t(d[p + 2]) << 8, p += 3;
    } else {
      sz = c - 0x60u, ++p;
    }
    if (p + sz > size) break;
    const uint8_t* r = d + p;
    p += sz;
    size_t q = 0;
    if (sz && (r[0] & 0x80)) {
      q = 1 + (r[0] & 0x7Fu);
      for (size_t e = 1; e < q && e < sz;) {
        const uint8_t op = r[e];
        const size_t n = bank[0x10 + op];  // (the bank's event-size table)
        if (!n || e + n > q) break;
        if (op == 112 && n >= 3) s.zoom.push_back({frame, float(r[e + 1]) / 10.0f, float(r[e + 2]) / 1000.0f});
        if (op == 113 && s.height_ref < 0) s.height_ref = frame;
        e += n;
      }
    }
    const uint8_t* k = r + q;
    const size_t left = sz - q;
    CamKey key;
    key.frame = frame;
    if (left == 29) {
      key.count = k[0];
      for (int a = 0; a < 3; ++a) key.pos[a] = LeF(k + 1 + 4 * a) * kRootUnit, key.rot[a] = LeF(k + 13 + 4 * a);
      key.fov = LeF(k + 25);
      s.cam.push_back(key);
    } else if (left == 13) {
      key.count = k[0];
      for (int a = 0; a < 3; ++a)
        key.pos[a] = float(int16_t(Le16(k + 1 + 2 * a))) * kRootUnit,
        key.rot[a] = float(int16_t(Le16(k + 7 + 2 * a))) * 6.2831853f / 65536.0f;
      s.cam.push_back(key);
    }
    ++frame;
  }
}

// A prop's track (evt object of type 00000100): per record the events - 80 / 81 `50 04` / `51 04` hide /
// show, 180-191 how it's held (182 = 1 follows its track ..., 183 = 4 worn on the head, 180 = 2) - and an
// object pose: sub-records 0x75 [count][s16 root x3][mask u24][s16 x3 per masked bone], 0x7e [count][s16 x3]
// (root), 0x7f [count][f32 x3], 0x7b [ref][count][s16 x3] (skipped), op < 21 [count][s16 x3] (one bone).
// Angles 2pi / 65536, positions 0.01.
void DecodeProp(const Bytes& bank, const uint8_t* d, size_t size, PropTrack& t) {
  constexpr float kAng = 6.2831853f / 65536.0f;
  PropKey cur;
  int frame = 0;
  for (size_t p = 0; p < size;) {
    const uint8_t c = d[p];
    if (c < 0x60) { frame += c, ++p; continue; }
    size_t sz;
    if (c == 0x60) {
      if (p + 3 > size) break;
      sz = size_t(d[p + 1]) | size_t(d[p + 2]) << 8, p += 3;
    } else {
      sz = c - 0x60u, ++p;
    }
    if (p + sz > size) break;
    const uint8_t* r = d + p;
    p += sz;
    size_t q = 0;
    if (sz && (r[0] & 0x80)) {
      q = 1 + (r[0] & 0x7Fu);
      for (size_t e = 1; e < q && e < sz;) {
        const uint8_t op = r[e];
        const size_t n = bank[0x10 + op];
        if (!n || e + n > q) break;
        if ((op == 80 || op == 81) && n >= 2) cur.shown = op == 81;
        if (op >= 180 && op <= 191) cur.state = op == 180 ? 2 : op == 181 ? 3 : op == 182 ? 1 : op == 183 ? 4 : op;
        e += n;
      }
    }
    cur.frame = frame;
    auto s16 = [](const uint8_t* b) { return float(int16_t(uint16_t(b[0] | b[1] << 8))); };
    auto bone = [&](int k, const uint8_t* b) {
      if (k < 0 || k >= 32) return;
      if (int(cur.bones.size()) <= k) cur.bones.resize(size_t(k) + 1);
      cur.bones[size_t(k)].on = true;
      for (int a = 0; a < 3; ++a) cur.bones[size_t(k)].e[a] = s16(b + 2 * a) * kAng;
    };
    for (size_t s = q; s < sz;) {
      const uint8_t op = r[s];
      if (op == 0x75 && s + 11 <= sz) {
        for (int a = 0; a < 3; ++a) cur.pos[a] = s16(r + s + 2 + 2 * a) * 0.01f;
        const uint32_t mask = r[s + 8] | r[s + 9] << 8 | r[s + 10] << 16;
        s += 11;
        for (int k = 0; k < 24 && s + 6 <= sz; ++k)
          if (mask >> k & 1) bone(k, r + s), s += 6;
      } else if (op == 0x7e && s + 8 <= sz) {
        for (int a = 0; a < 3; ++a) cur.pos[a] = s16(r + s + 2 + 2 * a) * 0.01f;
        s += 8;
      } else if (op == 0x7f && s + 14 <= sz) {
        for (int a = 0; a < 3; ++a) cur.pos[a] = LeF(r + s + 2 + 4 * a) * 0.01f;
        s += 14;
      } else if (op == 0x7b && s + 9 <= sz) {
        s += 9;
      } else if (op < 21 && s + 8 <= sz) {
        bone(op, r + s + 2), s += 8;
      } else {
        break;  // (an op not known here: the rest of the record is skipped)
      }
    }
    t.keys.push_back(cur);
    ++frame;
  }
}

// The steel steps (and only them: the announce tables, bell and belt of the set stay out) from the ringside set
// (char_preview::RingsideSteps).
void RingsideStatics(const std::wstring& game, std::vector<Static>& out) {
  std::vector<uint8_t> steps;
  std::vector<char_preview::StepPlace> places;
  if (!char_preview::RingsideSteps(Utf8((fs::path(game) / L"pac" / L"bg" / L"bgEtc.pac").wstring()), steps, places)) {
    Log("full preview: the ring steps (pac\\bg\\bgEtc.pac STG/WPON) didn't load");
    return;
  }
  std::string err;
  const int prop = char_preview::AddProp(steps, &err);
  if (prop < 0) return Log("full preview: the ring steps' model didn't load (" + err + ")");
  for (const auto& pl : places) {
    Static s;
    s.prop = prop;
    std::memcpy(s.r, pl.r, sizeof s.r);
    std::memcpy(s.t, pl.t, sizeof s.t);
    out.push_back(s);
  }
}

// The entrance's own pyro in words (prtcls.pac PCE6/<number>: emitters 0-99 are its own, 110+ the stage lights every
// set has): "12 emitters: fireworks, smoke". Each emitter is a PACH with a PRTC particle script, whose TexGroup names
// the textures; the kind is read from the names. "" when it has none.
std::string PyroNote(const std::wstring& game, int number) {
  if (number < 0) return {};
  char key[8];
  std::snprintf(key, sizeof key, "%04d", number);
  Bytes set;
  if (!ReadPacEntry(fs::path(game) / L"pac" / L"prtcls.pac", key, set, "PCE6")) return {};
  std::vector<svrfmt::PachEntry> kids;
  if (!svrfmt::PachRead(svrfmt::Unpack(set), kids)) return {};
  static const struct { const char* prefix; const char* words; } kKinds[] = {
      {"Hanabi", "fireworks"}, {"FireW", "fireworks"}, {"Fire0", "flames"}, {"Smoke", "smoke"}, {"Tsubu", "sparks"},
      {"Tama", "fireballs"}, {"Bo0", "jets"}, {"Cross", "flashes"}, {"Light", "flashes"}, {"LG0", "flashes"}};
  int emitters = 0;
  std::vector<std::string> kinds;
  for (const auto& k : kids) {
    if (k.id >= 100) continue;
    ++emitters;
    std::vector<svrfmt::PachEntry> parts;
    if (!svrfmt::PachRead(svrfmt::Unpack(k.data), parts)) continue;
    for (const auto& part : parts) {
      const Bytes prtc = svrfmt::Unpack(part.data);
      if (prtc.size() < 4 || std::memcmp(prtc.data(), "PRTC", 4)) continue;
      const std::string text(prtc.begin(), prtc.end());
      for (size_t at = text.find("TexGroup"); at != std::string::npos && at < text.size(); at = text.find("TexGroup", at + 8)) {
        for (const auto& kind : kKinds) {
          const size_t hit = text.find(kind.prefix, at + 8);
          if (hit != std::string::npos && hit < at + 0x400 &&
              std::find(kinds.begin(), kinds.end(), kind.words) == kinds.end())
            kinds.push_back(kind.words);
        }
      }
    }
  }
  if (!emitters) return {};
  std::string out = std::to_string(emitters) + (emitters == 1 ? " emitter" : " emitters");
  for (size_t i = 0; i < kinds.size(); ++i) out += (i ? ", " : ": ") + kinds[i];
  return out;
}

// The superstar's size: its CHAR/DAT record's +28 (after the record's 4-byte head), 4.12 fixed point - the game draws
// the model at it and its entrance cameras follow it (op 113: 19.75 x (1 - size) on the camera's y, measured in
// game: Big Show 1.121 -> -2.4, Cena 0.974 -> +0.5, Orton 1.010 -> -0.2). 1 when unknown.
float ProfileScale(const std::wstring& game, int star) {
  if (star < 0) return 1;
  Bytes dat;
  if (!ReadPacEntry(fs::path(game) / L"pac" / L"ch" / L"chEtc.pac", "DAT ", dat, "CHAR") &&
      !ReadPacEntry(fs::path(game) / L"pac" / L"ch" / L"chEtc.pac", "DAT", dat, "CHAR"))
    return 1;
  std::vector<svrfmt::PachEntry> kids;
  if (!svrfmt::PachRead(svrfmt::Unpack(dat), kids)) return 1;
  for (const auto& k : kids) {
    if (int(k.id) != star) continue;
    const Bytes r = svrfmt::Unpack(k.data);
    if (r.size() < 4 + 30) return 1;
    const int v = Le16(&r[4 + 28]);
    return v >= 2048 && v <= 8192 ? float(v) / 4096.0f : 1.0f;
  }
  return 1;
}

// The profile's entrance block of superstar `star` (chEtc.pac CHAR/PRO): music, movie numbers.
void ProfileMedia(const std::wstring& game, int star, int& music, int& movie) {
  music = movie = -1;
  if (star < 0) return;
  Bytes pro;
  const fs::path pac = fs::path(game) / L"pac" / L"ch" / L"chEtc.pac";
  if (!ReadPacEntry(pac, "PRO ", pro, "CHAR") && !ReadPacEntry(pac, "PRO", pro, "CHAR")) return;
  std::vector<svrfmt::PachEntry> kids;
  if (!svrfmt::PachRead(svrfmt::Unpack(pro), kids)) return;
  for (const auto& k : kids) {
    if (int(k.id) != star) continue;
    const Bytes r = svrfmt::Unpack(k.data);
    if (r.size() < 0x1C0 + 0x14) return;
    music = Le16(&r[0x1C0 + 0x10]), movie = Le16(&r[0x1C0 + 0x12]);
    return;
  }
}

std::unique_ptr<Loaded> LoadAll(const Request& req, const std::wstring& game) {
  auto L = std::make_unique<Loaded>();
  L->number = req.entrance;
  Bytes evp, evt;
  if (!FindEntrance(game, req.pack, req.entrance, evp, evt, L->source)) {
    L->error = "Entrance " + std::to_string(req.entrance) + " isn't in the game or in an installed pack.";
    return L;
  }
  if (evp.size() < 0x50 || std::memcmp(evp.data(), "0FOP", 4)) {
    L->error = "The entrance's script (EVP) isn't one the Mod Maker knows.";
    return L;
  }
  L->name.assign(reinterpret_cast<const char*>(&evp[0x22]), strnlen(reinterpret_cast<const char*>(&evp[0x22]), 30));
  for (char& c : L->name)
    if (c == '_') c = ' ';
  const uint32_t nobj = Be32(&evp[0x40]) & 0xFFFF, table = Be32(&evp[0x48]) + 8;
  int ent = -1, cam = -1;
  for (uint32_t i = 0; i < nobj; ++i) {
    const size_t q = table + 32 * size_t(i);
    if (q + 32 > evp.size()) break;
    const uint32_t t0 = Be32(&evp[q + 8]), t1 = Be32(&evp[q + 12]);
    if (ent < 0 && t0 == 0x01000000 && t1 == 0) ent = int(i);
    if (cam < 0 && t0 == 0x00010000 && t1 == 0) cam = int(i);
  }
  if (ent < 0) ent = 0;
  // the props (type 00000100): their evtobj.pac SOBJ models, one char_preview prop per model
  struct PropObj {
    int obj, prop;
    bool belt;
  };
  std::vector<PropObj> props;
  {
    std::map<uint32_t, int> loaded;
    const fs::path evtobj = fs::path(game) / L"pac" / L"evt" / L"evtobj.pac";
    for (uint32_t i = 0; i < nobj; ++i) {
      const size_t q = table + 32 * size_t(i);
      if (q + 32 > evp.size() || Be32(&evp[q + 8]) != 0x00000100 || Be32(&evp[q + 12]) != 0) continue;
      const uint32_t model = Be32(&evp[q + 4]);
      auto it = loaded.find(model);
      if (it == loaded.end()) {
        Bytes sobj;
        std::string err;
        int idx = -1;
        if (ReadPacEntry(evtobj, std::to_string(model).c_str(), sobj, "SOBJ")) idx = char_preview::AddProp(sobj, &err);
        if (idx < 0) Log("full preview: prop " + std::to_string(model) + " not shown (" + (err.empty() ? "not in evtobj.pac" : err) + ")");
        it = loaded.emplace(model, idx).first;
      }
      // (a title belt - evtobj 2002-2030, 2050-2052, 23xx, 25xx, 3xxx-5xxx: the champion's, from the belt models)
      const bool belt = (model >= 2002 && model <= 2030) || (model >= 2050 && model <= 2052) || (model >= 2300 && model <= 2600) ||
                        (model >= 3000 && model < 6000);
      if (it->second >= 0) props.push_back({int(i), it->second, belt});
    }
  }
  for (int k = 0; k < 5; ++k) {
    Seg s;
    s.slot = kSlots[k];
    for (const PropObj& po : props) {
      const uint8_t* d = nullptr;
      size_t size = 0;
      int frames = 0;
      if (!Track(evt, kIdShift + po.obj, s.slot, 0, &d, &size, &frames)) continue;
      PropTrack t;
      t.prop = po.prop, t.belt = po.belt;
      DecodeProp(evt, d, size, t);
      if (!t.keys.empty()) s.props.push_back(std::move(t));
    }
    const uint8_t* d = nullptr;
    size_t size = 0;
    int frames = 0;
    if (Track(evt, kIdShift + ent, s.slot, 0, &d, &size, &frames)) {
      std::string err;
      s.body = char_preview::DecodeClip(evt, kIdShift + ent, s.slot, &err);
      s.keys = std::max(s.keys, frames);
      if (s.body) s.keys = std::max(s.keys, char_preview::ClipKeys(*s.body)), L->fingers |= char_preview::ClipHasFingers(*s.body);
    }
    // (the camera's y1, else y0: the game plays one of the two per entrance - dumped in game, one run y0's keys
    // exactly, the next y1's)
    if (cam >= 0 && (Track(evt, kIdShift + cam, s.slot, 1, &d, &size, &frames) ||
                     Track(evt, kIdShift + cam, s.slot, 0, &d, &size, &frames))) {
      DecodeCamera(evt, d, size, s);
      s.keys = std::max(s.keys, frames);
    }
    if (s.keys > 0) L->total += s.keys, L->segs.push_back(std::move(s));
  }
  if (L->segs.empty()) {
    L->error = "Entrance " + std::to_string(req.entrance) + " has no tracks for its entrant.";
    return L;
  }
  RingsideStatics(game, L->statics);
  L->pyro = PyroNote(game, req.entrance);
  ProfileMedia(game, req.star, L->music, L->movie);
  L->scale = ProfileScale(game, req.star) * (req.height > 0 ? req.height : 1.0f);
  L->arena = std::make_unique<svrfmt::Arena>();
  std::string aerr;
  if (!L->arena->Load(PathStr(fs::path(game) / L"pac" / L"bg" / L"bg29.pac"), &aerr)) {
    L->arena.reset();
    Log("full preview: the practice arena (pac\\bg\\bg29.pac) didn't load: " + aerr);
  }
  return L;
}

// ---- the ground under the entrant

constexpr float kMatY = -12.0f, kApronOut = 31.9f;  // (editor.cpp's ring)
// An entrance's tracks - the entrant AND its camera - are in one frame whose y 0 is the ring mat: the ring
// part's feet stand at ~0 and the stage / ramp parts' at ~+12, the arena floor (2011 entrances walk a
// floor-level stage: root ~+2.5 there; every stock camera key aims exactly at the entrant's root as keyed).
// So the whole scene moves by the mat height into the arena's space; nothing is snapped per key.
// An arena whose stage is raised (the practice arena's: y -18, a ramp down to z ~100) lifts the entrant by
// the stage / ramp under him - seen in the game's own entrance there - and the camera with him (it aims at
// the root as keyed): StageGrid, the highest upward-facing arena surface (y -30..0) beyond the ringside.
constexpr float kSceneY = kMatY;

struct StageGrid {
  static constexpr float kX0 = -160, kZ0 = -400, kCell = 4;
  static constexpr int kNx = 80, kNz = 200;
  std::vector<float> h = std::vector<float>(size_t(kNx) * kNz, 0.0f);
  float At(float x, float z) const {
    if (std::fabs(z) < 64) return 0;  // (the ring and ringside: the frame's own)
    const int i = int((x - kX0) / kCell), j = int((z - kZ0) / kCell);
    return i >= 0 && j >= 0 && i < kNx && j < kNz ? h[size_t(j) * kNx + size_t(i)] : 0.0f;
  }
};

void BuildStageGrid(const svrfmt::Arena& a, StageGrid& g) {
  for (const auto& am : a.models)
    for (const auto& me : am.model.meshes)
      for (const auto& st : me.strips)
        for (const auto& t : svrfmt::StripToTriangles(st.indices)) {
          if (t[0] >= me.verts.size() || t[1] >= me.verts.size() || t[2] >= me.verts.size()) continue;
          const float* p[3] = {me.verts[t[0]].pos, me.verts[t[1]].pos, me.verts[t[2]].pos};
          const float ux = p[1][0] - p[0][0], uy = p[1][1] - p[0][1], uz = p[1][2] - p[0][2];
          const float vx = p[2][0] - p[0][0], vy = p[2][1] - p[0][1], vz = p[2][2] - p[0][2];
          const float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
          const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
          if (nl < 1e-6f || std::fabs(ny) / nl < 0.7f) continue;  // (walls)
          bool in = true;
          for (const float* q : p) in &= q[1] > -30.0f && q[1] < 0.5f;
          if (!in) continue;
          const float minx = std::min({p[0][0], p[1][0], p[2][0]}), maxx = std::max({p[0][0], p[1][0], p[2][0]});
          const float minz = std::min({p[0][2], p[1][2], p[2][2]}), maxz = std::max({p[0][2], p[1][2], p[2][2]});
          const float det = (p[1][0] - p[0][0]) * (p[2][2] - p[0][2]) - (p[2][0] - p[0][0]) * (p[1][2] - p[0][2]);
          if (std::fabs(det) < 1e-6f) continue;
          for (int j = std::max(0, int((minz - StageGrid::kZ0) / StageGrid::kCell));
               j <= std::min(StageGrid::kNz - 1, int((maxz - StageGrid::kZ0) / StageGrid::kCell)); ++j)
            for (int i = std::max(0, int((minx - StageGrid::kX0) / StageGrid::kCell));
                 i <= std::min(StageGrid::kNx - 1, int((maxx - StageGrid::kX0) / StageGrid::kCell)); ++i) {
              const float cx = StageGrid::kX0 + (float(i) + 0.5f) * StageGrid::kCell;
              const float cz = StageGrid::kZ0 + (float(j) + 0.5f) * StageGrid::kCell;
              const float b1 = ((cx - p[0][0]) * (p[2][2] - p[0][2]) - (p[2][0] - p[0][0]) * (cz - p[0][2])) / det;
              const float b2 = ((p[1][0] - p[0][0]) * (cz - p[0][2]) - (cx - p[0][0]) * (p[1][2] - p[0][2])) / det;
              if (b1 < 0 || b2 < 0 || b1 + b2 > 1) continue;
              const float y = p[0][1] + b1 * (p[1][1] - p[0][1]) + b2 * (p[2][1] - p[0][1]);
              float& h = g.h[size_t(j) * StageGrid::kNx + size_t(i)];
              h = std::min(h, y);  // (y down: the highest surface)
            }
        }
}

// per key: the arena's stage / ramp height under the entrant's root, eased so stepping up doesn't jump
void Ground(Seg& s, const StageGrid& g) {
  s.lift.clear();
  if (!s.body) return;
  const auto tr = char_preview::ClipTrace(*s.body);
  if (tr.empty()) return;
  const int n = int(tr.size()), ease = 8;
  std::vector<float> raw(static_cast<size_t>(n));
  for (int k = 0; k < n; ++k) raw[size_t(k)] = g.At(tr[size_t(k)].x, tr[size_t(k)].z);
  s.lift.resize(size_t(n));
  for (int k = 0; k < n; ++k) {
    float sum = 0;
    int c = 0;
    for (int j = std::max(0, k - ease); j <= std::min(n - 1, k + ease); ++j) sum += raw[size_t(j)], ++c;
    s.lift[size_t(k)] = sum / float(std::max(1, c));
  }
}

StageGrid g_stage;  // (the loaded arena's)

float LiftAt(const Seg& s, float key) {
  return s.lift.empty() ? 0.0f : s.lift[size_t(std::clamp(int(key), 0, int(s.lift.size()) - 1))];
}

// ---- the camera at a segment's key

struct Cam {
  float pos[3], rot[3], fov;
  bool zoomed = false;
  float zoom = 1;
};

float LerpAngle(float a, float b, float f) {
  float d = std::fmod(b - a, 6.2831853f);
  if (d > 3.1415927f) d -= 6.2831853f;
  if (d < -3.1415927f) d += 6.2831853f;
  return a + d * f;
}

bool CameraAt(const Seg& s, float t, Cam& out) {
  if (s.cam.empty()) return false;
  // each key: from where the camera was when it started (its frame) to the key, over `count` frames
  Cam from{}, to{};
  float start = 0, count = 0;
  auto set = [](Cam& c, const CamKey& k) {
    for (int a = 0; a < 3; ++a) c.pos[a] = k.pos[a], c.rot[a] = k.rot[a];
    c.fov = k.fov;
  };
  auto at = [&](float time) {
    Cam c = to;
    if (count > 0 && time < start + count) {
      const float f = std::clamp((time - start) / count, 0.0f, 1.0f);
      for (int a = 0; a < 3; ++a)
        c.pos[a] = from.pos[a] + (to.pos[a] - from.pos[a]) * f, c.rot[a] = LerpAngle(from.rot[a], to.rot[a], f);
      c.fov = from.fov + (to.fov - from.fov) * f;
    }
    return c;
  };
  set(to, s.cam[0]);
  from = to, start = float(s.cam[0].frame), count = 0;
  for (size_t i = 1; i < s.cam.size(); ++i) {
    if (float(s.cam[i].frame) > t) break;
    from = at(float(s.cam[i].frame));
    set(to, s.cam[i]);
    start = float(s.cam[i].frame), count = float(s.cam[i].count);
  }
  out = at(t);
  // the zoom: each event approaches its target by `rate` of the way a frame (0: at once)
  float z = 0;
  bool have = false;
  for (size_t i = 0; i < s.zoom.size(); ++i) {
    const ZoomEvent& e = s.zoom[i];
    if (float(e.frame) > t) break;
    if (!have || e.rate <= 0) {
      z = e.target, have = true;
    }
    const float until = i + 1 < s.zoom.size() ? std::min(t, float(s.zoom[i + 1].frame)) : t;
    if (e.rate > 0) z = e.target + (z - e.target) * std::pow(1.0f - std::min(e.rate, 1.0f), until - float(e.frame));
  }
  if (have && z > 0.01f) out.zoomed = true, out.zoom = z;
  return true;
}

void Matrices(const Cam& c, float aspect, float view[16], float proj[16]) {
  // R = Rz Ry Rx; the camera looks down its local -x, its local -y is up (stock 2011 cameras: R col 1 . (0,-1,0) = -1)
  const float cx = std::cos(c.rot[0]), sx = std::sin(c.rot[0]), cy = std::cos(c.rot[1]), sy = std::sin(c.rot[1]);
  const float cz = std::cos(c.rot[2]), sz = std::sin(c.rot[2]);
  const float R[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                         {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                         {-sy, cy * sx, cy * cx}};
  float f[3] = {-R[0][0], -R[1][0], -R[2][0]}, u[3] = {-R[0][1], -R[1][1], -R[2][1]};
  auto norm = [](float* v) {
    const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
  };
  auto cross = [](const float* a, const float* b, float* o) {
    o[0] = a[1] * b[2] - a[2] * b[1], o[1] = a[2] * b[0] - a[0] * b[2], o[2] = a[0] * b[1] - a[1] * b[0];
  };
  norm(f), norm(u);
  float x[3], y[3];
  cross(u, f, x), norm(x);
  cross(f, x, y);
  const float* e = c.pos;
  const float v[16] = {x[0], y[0], f[0], 0, x[1], y[1], f[1], 0, x[2], y[2], f[2], 0,
                       -(x[0] * e[0] + x[1] * e[1] + x[2] * e[2]), -(y[0] * e[0] + y[1] * e[1] + y[2] * e[2]),
                       -(f[0] * e[0] + f[1] * e[1] + f[2] * e[2]), 1};
  std::memcpy(view, v, sizeof v);
  // the game's field of view (vertical here): the zoom's, else the key's
  float fov = c.zoomed ? 2 * std::atan(0.625f / c.zoom) * 0.82f : 2 * std::atan(0.8f * std::tan(c.fov / 2));
  fov = std::clamp(fov, 0.05f, 2.6f);
  // (x flipped: the game's view is left-handed - with the plain basis above the shirts' and the barriers'
  // writing came out mirrored next to the game's own shots; the editor draws with no culling)
  const float zn = 1, zf = 5000, ys = 1 / std::tan(fov / 2), xs = -ys / aspect;
  const float p[16] = {xs, 0, 0, 0, 0, ys, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
  std::memcpy(proj, p, sizeof p);
}

// ---- media

void CloseTitantron() {
  editor::SetTextureOverride("tf_titan", nullptr);
  g_tt_mf.reset();
  if (g_bink) bink_close(g_bink), g_bink = nullptr;
  if (g_tt_srv) g_tt_srv->Release(), g_tt_srv = nullptr;
  if (g_tt_tex) g_tt_tex->Release(), g_tt_tex = nullptr;
  g_tt_shown = -1;
  g_tt_note.clear();
}

void OpenTitantron(const std::wstring& file) {
  CloseTitantron();
  if (file.empty()) return;
  wchar_t err[256] = L"";
  g_bink = bink_open(file.c_str(), err, 256);
  if (!g_bink) {  // (not Bink 1: Media Foundation - a mod's movie.mp4)
    auto mf = std::make_unique<video_mf::Reader>();
    std::string why;
    if (mf->Open(file, why)) {
      g_tt_mf = std::move(mf);
      g_tt_bgra.assign(size_t(g_tt_mf->Width()) * g_tt_mf->Height() * 4, 0);
      D3D11_TEXTURE2D_DESC td = {};
      td.Width = UINT(g_tt_mf->Width()), td.Height = UINT(g_tt_mf->Height()), td.MipLevels = 1, td.ArraySize = 1;
      td.Format = DXGI_FORMAT_B8G8R8A8_UNORM, td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DYNAMIC, td.BindFlags = D3D11_BIND_SHADER_RESOURCE, td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_tt_tex)) || FAILED(g_dev->CreateShaderResourceView(g_tt_tex, nullptr, &g_tt_srv)))
        CloseTitantron();
      return;
    }
    Log("full preview: the titantron movie didn't open: " + Utf8(file) + " (" + Utf8(err) + "; " + why + ")");
    Bytes head;
    if (svrfmt::ReadFile(PathStr(fs::path(file)), head) && head.size() >= 3 && !std::memcmp(head.data(), "KB2", 3))
      g_tt_note = "A Bink 2 movie (WWE 2K15 and later):\nthe preview and the game play it\nonce it's converted (Other games\npage: Bink 2 tool).";
    else
      g_tt_note = "The titantron movie didn't open\n(see the log).";
    return;
  }
  const int w = bink_width(g_bink), h = bink_height(g_bink);
  g_tt_bgra.assign(size_t(w) * h * 4, 0);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = UINT(w), td.Height = UINT(h), td.MipLevels = 1, td.ArraySize = 1;
  td.Format = DXGI_FORMAT_B8G8R8A8_UNORM, td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DYNAMIC, td.BindFlags = D3D11_BIND_SHADER_RESOURCE, td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_tt_tex)) || FAILED(g_dev->CreateShaderResourceView(g_tt_tex, nullptr, &g_tt_srv)))
    CloseTitantron();
}

// The titantron frame for `seconds` into the entrance (looped), decoded forward (back: from the start).
void TitantronAt(double seconds) {
  if (g_tt_mf && g_tt_tex) {
    if (!g_tt_mf->FrameAt(seconds, g_tt_bgra)) return;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(g_ctx->Map(g_tt_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
      const int w = g_tt_mf->Width(), h = g_tt_mf->Height();
      for (int y = 0; y < h; ++y)
        std::memcpy(static_cast<uint8_t*>(ms.pData) + size_t(y) * ms.RowPitch, &g_tt_bgra[size_t(y) * w * 4], size_t(w) * 4);
      g_ctx->Unmap(g_tt_tex, 0);
    }
    return;
  }
  if (!g_bink || !g_tt_tex) return;
  const int n = std::max(1, bink_frames(g_bink));
  const double ft = double(std::max<LONGLONG>(1, bink_frame_time(g_bink))) / 1e7;
  const int want = int(seconds / ft) % n;
  if (want == g_tt_shown) return;
  if (want < bink_position(g_bink) - 1 || bink_position(g_bink) == 0) bink_rewind(g_bink);
  int guard = 0;
  while (bink_position(g_bink) <= want && guard++ < n) bink_next(g_bink);
  bink_bgra(g_bink, g_tt_bgra.data());
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(g_ctx->Map(g_tt_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
    const int w = bink_width(g_bink), h = bink_height(g_bink);
    for (int y = 0; y < h; ++y)
      std::memcpy(static_cast<uint8_t*>(ms.pData) + size_t(y) * ms.RowPitch, &g_tt_bgra[size_t(y) * w * 4], size_t(w) * 4);
    g_ctx->Unmap(g_tt_tex, 0);
  }
  g_tt_shown = want;
}

void StartMusic(const Loaded& L) {
  g_music_note.clear();
  if (!g_req.song.empty()) {
    audio_out::Load(g_req.song);
    return;
  }
  if (L.music <= 0 || L.music >= 254) {  // (254: the player's playlist; 999: none)
    g_music_note = "no theme (the superstar's profile has none of the game's own)";
    return;
  }
  const fs::path work = fs::path(g_game) / L"Mods" / L".convert" / L"full_preview";
  const fs::path mp3 = work / (L"music_" + std::to_wstring(L.music) + L".mp3");
  std::error_code ec;
  if (fs::exists(mp3, ec) && fs::file_size(mp3, ec) > 0) {
    audio_out::Load(mp3.wstring());
    return;
  }
  fs::create_directories(work, ec);
  char ev[32];
  std::snprintf(ev, sizeof ev, "Play_MUS_%04d_0_0", L.music);
  g_music_note = "getting the theme from the game's sound files...";
  const int gen = g_gen;
  const bool started = RunToolJson(L"wwise.py", {L"theme", (fs::path(g_game) / L"sound" / L"Music.pck").wstring(), Wide(ev), mp3.wstring()},
                                   [mp3, gen](int code, const std::string& text) {
                                     if (gen != g_gen || !g_open) return;
                                     std::error_code e2;
                                     if (code == 0 && fs::exists(mp3, e2)) {
                                       g_music_note.clear();
                                       audio_out::Load(mp3.wstring());
                                       if (g_playing) audio_out::Play(g_key / 30.0);
                                     } else {
                                       g_music_note = "no theme (the game's theme couldn't be read: see the log)";
                                       Log("full preview: wwise.py: " + text);
                                     }
                                   });
  if (!started) g_music_note = "no theme: the game's themes need the Mod Maker tools (the launcher's Setup page)";
}

void SetPlaying(bool on) {
  g_playing = on;
  if (on && g_music_on && audio_out::State() == 1) audio_out::Play(g_key / 30.0);
  else audio_out::Pause();
}

void Apply(std::unique_ptr<Loaded> L) {
  g_cur = std::move(L);
  if (!g_cur->error.empty()) return;
  char_preview::SetHeight(g_cur->scale), char_preview::SetScaleAtRoot(true);  // (the superstar's size, as the game draws it)
  Log("full preview: the superstar's size x" + std::to_string(g_cur->scale));
  if (g_cur->arena) editor::ViewArena(g_cur->arena.get(), "Practice arena", "");
  g_stage = StageGrid();
  if (g_cur->arena) BuildStageGrid(*g_cur->arena, g_stage);
  g_tt_screen = g_cur->arena && g_cur->arena->FindTexture("tf_titan") != nullptr;
  editor::SetViewHint(false);
  editor::SetViewExtra([](ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, int w, int h, const float* view, const float* proj) {
    if (!g_cur) return;
    static const char_preview::PropBones none;
    for (const Static& st : g_cur->statics)  // (the ring steps: the arena's space)
      char_preview::RenderProp(rtv, dsv, w, h, view, proj, st.prop, st.r, st.t, none);
    const int si = std::clamp(g_seg_now, 0, int(g_cur->segs.size()) - 1);
    const Seg& s = g_cur->segs[size_t(si)];
    if (s.slot == 0 || !s.body) {  // (segment 0 is the hidden intro: no entrant)
      if (g_dump) {
        std::fprintf(g_dump, "K %.0f S %d L %.2f lift 0 V", g_key, s.slot, g_local_key);
        for (int i = 0; i < 16; ++i) std::fprintf(g_dump, " %.5f", view[i]);
        std::fprintf(g_dump, " P %.5f %.5f B\n", proj[0], proj[5]);
        g_dump_done = g_key;
      }
      return;
    }
    const float last = float(std::max(0, char_preview::ClipKeys(*s.body) - 1)), key = std::min(g_local_key, last);
    const float off[3] = {0, kSceneY + LiftAt(s, key), 0};  // (the camera gets the same)
    char_preview::RenderInto(rtv, dsv, w, h, view, proj, s.body.get(), key, off);
    if (g_dump) {  // (test aid: this key's camera and the entrant's bones, as drawn)
      std::fprintf(g_dump, "K %.0f S %d L %.2f lift %.3f V", g_key, s.slot, key, off[1]);
      for (int i = 0; i < 16; ++i) std::fprintf(g_dump, " %.5f", view[i]);
      std::fprintf(g_dump, " P %.5f %.5f B", proj[0], proj[5]);
      for (const auto& b : char_preview::EntrantBones()) std::fprintf(g_dump, " %s %.3f %.3f %.3f", b.name.c_str(), b.t[0], b.t[1], b.t[2]);
      std::fputc('\n', g_dump);
      g_dump_done = g_key;
    }
    // the props at this key: worn on the head (state 4), held (state 1: on the right hand), else keyed in the
    // entrance's frame. Not the title belt: the game places a champion's belt itself (its track keeps one
    // pose, two 0x7b refs, while the belt goes from hand to shoulder in the game) - not known yet
    for (const PropTrack& t : s.props) {
      if (t.belt) continue;
      const PropKey* k = &t.keys.front();
      for (const PropKey& x : t.keys)
        if (float(x.frame) <= key) k = &x;
      if (!k->shown) continue;
      float r[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, p[3] = {k->pos[0], k->pos[1], k->pos[2]};
      const char* bone = k->state == 4 ? "atama" : k->state == 1 ? "r_te" : nullptr;
      if (bone) {
        float br[9], bt[3];
        if (!char_preview::EntrantBone(bone, br, bt)) continue;
        for (int a = 0; a < 3; ++a) p[a] = bt[a] + br[a * 3] * k->pos[0] + br[a * 3 + 1] * k->pos[1] + br[a * 3 + 2] * k->pos[2];
        std::memcpy(r, br, sizeof r);
      }
      char_preview::RenderProp(rtv, dsv, w, h, view, proj, t.prop, r, p, k->bones, off);
    }
  });
  std::wstring movie = g_req.movie;
  if (movie.empty() && g_cur->movie >= 0 && g_cur->movie < 700) {
    wchar_t name[16];
    std::swprintf(name, 16, L"%03d.bik", g_cur->movie);
    movie = (fs::path(g_game) / L"movies" / L"titantron" / name).wstring();
    std::error_code ec;
    if (!fs::exists(movie, ec)) movie.clear();
  }
  OpenTitantron(movie);
  StartMusic(*g_cur);
  g_key = PartStart(*g_cur, g_req.start_part);
  if (g_test_key >= 0) g_key = std::min(g_test_key, float(g_cur->total - 1)), g_use_camera = g_test_camera, g_test_key = -1;
  else SetPlaying(true);
}

std::string Clock(float keys) {
  const int s = int(keys / 30);
  char b[16];
  std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
  return b;
}

}  // namespace

void Open(const Request& r) {
  if (g_open) Close();
  g_req = r;
  g_open = true;
  g_playing = false;
  g_key = 0;
  g_cur.reset();
  g_music_note.clear();
  audio_out::SetMuted(g_behind);
  char_preview::ClearProps();
  // the superstar as it enters: the attire (its entrance gear when it has some)
  g_saved_attire = char_preview::Attire(), g_saved_gear = char_preview::Gear(), g_saved_height = char_preview::Height();
  if (!r.model.empty()) char_preview::SetModel(r.model, g_game);
  char_preview::SetAttire(std::max(1, r.attire));
  char_preview::SetGear(r.gear);
  const int gen = ++g_gen;
  g_load_state = 1;
  const Request req = r;
  const std::wstring game = g_game;
  std::thread([req, game, gen] {
    auto L = LoadAll(req, game);
    std::lock_guard lock(g_mu);
    if (gen != g_gen) return;
    g_next = std::move(L);
    g_load_state = 2;
  }).detach();
  Log("full preview: entrance " + std::to_string(r.entrance) + " for " + r.title);
}

bool IsOpen() { return g_open; }

void Close() {
  if (!g_open) return;
  g_open = false;
  ++g_gen;
  g_load_state = 0;
  audio_out::Unload();
  CloseTitantron();
  editor::SetViewExtra(nullptr);
  editor::ClearViewCamera();
  editor::SetViewHint(true);
  editor::ClearView();
  installed_page::RestoreView();  // (the Installed page's arena, if it had one in that view)
  g_cur.reset();
  {
    std::lock_guard lock(g_mu);
    g_next.reset();
  }
  char_preview::SetGear(g_saved_gear);
  char_preview::SetHeight(g_saved_height), char_preview::SetScaleAtRoot(false);
  char_preview::SetAttire(g_saved_attire);
  char_preview::ClearProps();
}

void TestSong(const std::wstring& file) { g_test_song = file; }
void TestMovie(const std::wstring& file) { g_test_movie = file; }
void TestDump(const std::wstring& file) { g_dump_file = file; }
void TestView(const std::wstring& spec) {
  float v[18];
  int n = 0;
  for (const wchar_t* p = spec.c_str(); *p && n < 18;) {
    wchar_t* end = nullptr;
    v[n] = std::wcstof(p, &end);
    if (end == p) break;
    ++n;
    for (p = end; *p == L',' || *p == L' ';) ++p;
  }
  if (n < 17) return;
  std::memcpy(g_test_view, v, sizeof g_test_view);
  g_test_fov = v[16], g_test_view_on = true;
  if (n >= 18) g_test_xs = v[17];
}

void TestOpen(int entrance, int star, float key, bool camera) {
  g_test_pending = true, g_test_entrance = entrance, g_test_star = star, g_test_key = key, g_test_camera = camera;
}

void Draw() {
  if (g_test_pending && !g_game.empty()) {
    g_test_pending = false;
    Request r;
    r.entrance = g_test_entrance, r.star = g_test_star;
    r.model = (fs::path(g_game) / L"pac" / L"ch" / (L"ch" + std::to_wstring(g_test_star) + L".pac")).wstring();
    r.gear = true;
    r.song = g_test_song;
    r.movie = g_test_movie;
    const StarInfo* si = StarById(g_test_star);
    r.title = si ? si->name : "superstar " + std::to_string(g_test_star);
    Open(r);
  }
  if (!g_open) return;
  if (g_load_state == 2) {
    std::unique_ptr<Loaded> L;
    {
      std::lock_guard lock(g_mu);
      L = std::move(g_next);
      g_load_state = 0;
    }
    if (L) Apply(std::move(L));
  }
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x * 0.9f, 1100 * g_scale), std::min(vp->Size.y * 0.9f, 720 * g_scale)), ImGuiCond_Appearing);
  bool open = true;
  const std::string title = "Full preview: " + g_req.title + "###full_preview";
  if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
    if (!g_cur) {
      ImGui::TextUnformatted("Reading the entrance and the practice arena...");
    } else if (!g_cur->error.empty()) {
      ImGui::TextWrapped("%s", g_cur->error.c_str());
    } else {
      Loaded& L = *g_cur;
      if (char_preview::ModelReady() && !L.grounded) {  // (the entrant's path over the arena's stage / ramp)
        for (Seg& sg : L.segs) Ground(sg, g_stage);
        L.grounded = true;
      }
      // the clock
      if (!g_dump_file.empty() && !g_dump && L.grounded) {  // (the dump: from key 0, one key a frame)
        g_dump = _wfopen(g_dump_file.c_str(), L"w"), g_key = 0, g_dump_done = -1, SetPlaying(false);
        Log("full preview: dumping every key into " + Utf8(g_dump_file));
      }
      if (g_dump && g_dump_done >= g_key) {
        if (g_key + 1 >= float(L.total)) {
          std::fclose(g_dump), g_dump = nullptr, g_dump_file.clear();
          Log("full preview: dump done");
        } else {
          g_key += 1;
        }
      }
      if (g_playing) {
        g_key += ImGui::GetIO().DeltaTime * 30.0f;
        if (g_key >= float(L.total - 1)) g_key = float(L.total - 1), SetPlaying(false);
      }
      if (g_playing && g_music_on && audio_out::State() == 1 && !audio_out::Playing() && g_key / 30.0 < audio_out::Length())
        audio_out::Play(g_key / 30.0);  // (the song became ready while playing)
      // which segment
      float rest = g_key;
      int si = 0;
      while (si + 1 < int(L.segs.size()) && rest >= float(L.segs[size_t(si)].keys)) rest -= float(L.segs[size_t(si)].keys), ++si;
      g_seg_now = si, g_local_key = rest;
      const Seg& s = L.segs[size_t(si)];
      // the view
      const float ctl_h = ImGui::GetFrameHeightWithSpacing() * 3 + 8 * g_scale;
      const ImVec2 avail = ImGui::GetContentRegionAvail();
      const float w = std::max(64.0f, avail.x), h = std::max(64.0f, avail.y - ctl_h);
      Cam c;
      if (g_test_view_on) {  // (test aid: the game's own camera, as dumped - view row-vector, fov in degrees)
        float proj[16] = {};
        const float xs = 1 / std::tan(g_test_fov * 3.14159265f / 360.0f), zn = 1, zf = 5000;  // (the fov: horizontal)
        proj[0] = g_test_xs * xs, proj[5] = -xs * (w / h), proj[10] = zf / (zf - zn), proj[11] = 1, proj[14] = -zn * zf / (zf - zn);
        editor::SetViewCamera(g_test_view, proj);
      } else if (g_use_camera && CameraAt(s, rest, c)) {
        c.pos[1] += kSceneY + LiftAt(s, rest);  // (the entrance's frame -> the arena's, as the entrant)
        if (s.height_ref >= 0 && rest >= float(s.height_ref)) c.pos[1] += 19.75f * (1 - L.scale);  // (op 113)
        float view[16], proj[16];
        Matrices(c, w / h, view, proj);
        editor::SetViewCamera(view, proj);
      } else {
        editor::ClearViewCamera();
      }
      const ImVec2 at = ImGui::GetCursorScreenPos();
      // the titantron on the arena's screen (its tf_titan texture), else in a corner
      const bool tt = g_titantron_on && (g_bink || g_tt_mf) && g_tt_srv;
      if (tt) TitantronAt(g_key / 30.0);
      editor::SetTextureOverride("tf_titan", tt && g_tt_screen ? g_tt_srv : nullptr);
      if (L.arena) {
        editor::DrawView(w, h);
      } else {
        ImGui::Dummy(ImVec2(w, h));
        ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 8, at.y + 8), IM_COL32(220, 160, 160, 255), "The practice arena (pac\\bg\\bg29.pac) didn't load.");
      }
      ImDrawList* dl = ImGui::GetWindowDrawList();
      if (!char_preview::ModelReady() && s.slot != 0)
        dl->AddText(ImVec2(at.x + 8, at.y + 8), IM_COL32(230, 230, 235, 230), "Loading the superstar...");
      if (s.slot == 0) dl->AddText(ImVec2(at.x + 8, at.y + 8), IM_COL32(230, 230, 235, 200), "The intro (the superstar comes out after it)");
      // the titantron in a corner where the arena has no screen
      if (g_titantron_on && !g_tt_note.empty()) {  // (one that can't play: why)
        const ImVec2 ts = ImGui::CalcTextSize(g_tt_note.c_str());
        const ImVec2 a(at.x + w - ts.x - 22 * g_scale, at.y + 10 * g_scale), b(at.x + w - 10 * g_scale, a.y + ts.y + 12 * g_scale);
        dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 200));
        dl->AddText(ImVec2(a.x + 6 * g_scale, a.y + 6 * g_scale), IM_COL32(230, 200, 140, 255), g_tt_note.c_str());
      }
      if (tt && !g_tt_screen) {
        const int vw = g_bink ? bink_width(g_bink) : g_tt_mf->Width(), vh = g_bink ? bink_height(g_bink) : g_tt_mf->Height();
        const float tw = std::min(w * 0.24f, 240 * g_scale), th = tw * float(vh) / float(std::max(1, vw));
        const ImVec2 a(at.x + w - tw - 10 * g_scale, at.y + 10 * g_scale), b(a.x + tw, a.y + th);
        dl->AddRectFilled(ImVec2(a.x - 3, a.y - 3), ImVec2(b.x + 3, b.y + 3), IM_COL32(0, 0, 0, 200));
        dl->AddImage(ImTextureID(reinterpret_cast<uintptr_t>(g_tt_srv)), a, b);
        dl->AddText(ImVec2(a.x, b.y + 4), IM_COL32(200, 200, 210, 200), "Titantron");
      }
      // the controls
      if (ImGui::Button(g_playing ? "Pause" : "Play", ImVec2(90 * g_scale, 0))) {
        if (!g_playing && g_key >= float(L.total - 1)) g_key = 0;
        SetPlaying(!g_playing);
      }
      ImGui::SameLine();
      if (ImGui::Button("Restart", ImVec2(90 * g_scale, 0))) g_key = 0, SetPlaying(true);
      ImGui::SameLine();
      const float ring = PartStart(L, 4);
      ImGui::BeginDisabled(ring <= 0);
      if (ImGui::Button("Skip to the ring", ImVec2(130 * g_scale, 0))) g_key = ring, SetPlaying(true);
      ImGui::EndDisabled();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Plays from the walk to the ring (the last part).");
      ImGui::SameLine();
      ImGui::SetNextItemWidth(std::max(120.0f, ImGui::GetContentRegionAvail().x - 120 * g_scale));
      float k = g_key;
      if (ImGui::SliderFloat("##time", &k, 0, float(L.total - 1), (Clock(g_key) + " / " + Clock(float(L.total))).c_str())) {
        g_key = k;
        if (g_playing && g_music_on) audio_out::Play(g_key / 30.0);
      }
      ImGui::SameLine();
      ImGui::Text("part %d of %d", si + 1, int(L.segs.size()));
      if (ImGui::Checkbox("The entrance's camera", &g_use_camera) && !g_use_camera) editor::ClearViewCamera();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: drag in the view to look around yourself (right drag turns, wheel zooms).");
      ImGui::SameLine();
      if (ImGui::Checkbox("Theme music", &g_music_on)) SetPlaying(g_playing);
      ImGui::SameLine();
      ImGui::Checkbox("Titantron", &g_titantron_on);
      if (char_preview::HasGear() || char_preview::Gear()) {  // (the attire has an entrance outfit: kind 4)
        ImGui::SameLine();
        bool gear = char_preview::Gear();
        if (ImGui::Checkbox("Entrance gear", &gear)) char_preview::SetGear(gear);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The outfit the attire wears for its entrance (on), or the match attire (off).");
      }

      const std::string theme = audio_out::State() == 1 ? " - theme " + Clock(float(audio_out::Length() * 30)) :
                                audio_out::State() == -1 ? " - no theme: " + audio_out::Error() :
                                g_music_note.empty() ? std::string() : " - " + g_music_note;
      ImGui::TextDisabled("Entrance %d %s - from %s%s%s", L.number, L.name.c_str(), L.source.c_str(),
                          L.fingers ? ", with its finger tracks" : "", theme.c_str());
      if (!L.pyro.empty()) {
        ImGui::TextDisabled("Pyro plays in game, not here: %s (prtcls.pac PCE6/%04d)", L.pyro.c_str(), L.number);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("The entrance's own particle set. The preview doesn't draw particles yet.");
      }
    }
  }
  ImGui::End();
  if (!open) Close();
}

}  // namespace full_preview
}  // namespace mm
