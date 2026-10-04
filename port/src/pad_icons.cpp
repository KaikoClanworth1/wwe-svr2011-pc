// WWE SmackDown vs. Raw 2011 - PlayStation and keyboard button pictures (see
// pad_icons.h).
//
// The game's fonts:
//   font +0 page count, +4 pages (24 bytes each: +0 texture width, +4 height,
//   +8 glyph count, +12 kerning count, +16 glyphs, +20 kerning);
//   glyph (24 bytes): +0 u0, +4 v0, +8 u1, +12 v1 (floats, of the page's
//   texture), +16 width, +18 height (pixels, u16), +20 code - sorted by code.
// Glyphs are found by sub_826AD450(font, code) -> the page, then a search of
// its glyphs (in sub_826AD608, the lookup, and inline in the text measuring
// and layout code). So the icon page's glyphs are rewritten where they are:
// each font keeps its versions (Xbox, PlayStation, pairs, keyboard; every v
// moved to its section of the taller picture) and the page holds the one the
// text being measured or drawn needs - set at each page search for an icon.
//
// Other pictures (the match HUD's prompts, character select's controllers):
// textures of their own, bound by SetTexture (sub_82917EC8: device, sampler,
// texture object). The native renderer tells which guest texture holds which
// picture (PadIconsTextureUploaded: by its data); for a PlayStation or
// keyboard player SetTexture gets a copy of the texture object pointing at
// that version's data instead.

#include "pad_icons.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/xenos.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"
#include "pad_types.h"

namespace svr2011 {
namespace {

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
void Wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
void Wr16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8), p[1] = uint8_t(v); }
float RdF(const uint8_t* p) {
  const uint32_t u = Rd32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}
void WrF(uint8_t* p, float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  Wr32(p, u);
}

constexpr uint32_t kGlyph = 24;
constexpr uint32_t kFirstCode = 0xE000;

enum Kind : uint8_t { kXbox, kPlayStation, kPair, kKeyboard, kKinds };

rex::memory::Memory* g_memory = nullptr;
uint8_t* g_base = nullptr;
bool g_enabled = false;
thread_local int t_owner = -1;

// The kind of pictures for the text / picture being drawn: its player's
// controller, or (shared by everyone) what the players use - PlayStation if
// all use one, pairs ("A / Cross") if they mix Xbox and PlayStation pads.
Kind CurrentKind(bool pairs) {
  auto of = [](PadType t) {
    return t == PadType::kPlayStation ? kPlayStation : t == PadType::kKeyboard ? kKeyboard : kXbox;
  };
  if (t_owner >= 0) return of(PlayerPadType(uint32_t(t_owner)));
  const uint32_t in_use = PadTypesInUse();
  const bool ps = in_use & (1u << uint32_t(PadType::kPlayStation));
  const bool xbox = in_use & (1u << uint32_t(PadType::kXbox));
  const bool kb = in_use & (1u << uint32_t(PadType::kKeyboard));
  if (ps) return xbox || kb ? (pairs ? kPair : kXbox) : kPlayStation;
  return kb && !xbox ? kKeyboard : kXbox;
}

// ---------------------------------------------------------------------------
// Keyboard: the key each button is bound to (the SDK's keybind_* settings,
// the first of each), shown as its keycap.

struct ButtonBind {
  uint32_t code;
  const char* setting;  // (or a special cap: WASD / ARROWS when bound so)
};
const ButtonBind kBinds[] = {
    {0xE000, "keybind_start"},          {0xE001, "keybind_back"},           {0xE002, "keybind_dpad_up"},
    {0xE003, "keybind_dpad_up"},        {0xE004, "keybind_dpad_down"},      {0xE005, "keybind_dpad_right"},
    {0xE006, "keybind_dpad_left"},      {0xE007, "keybind_y"},              {0xE008, "keybind_b"},
    {0xE009, "keybind_a"},              {0xE00A, "keybind_x"},              {0xE00B, "keybind_left_shoulder"},
    {0xE00C, "keybind_left_trigger"},   {0xE00D, "keybind_lstick_press"},   {0xE00E, "keybind_right_shoulder"},
    {0xE00F, "keybind_right_trigger"},  {0xE010, "keybind_rstick_press"},   {0xE011, "rstick"},
    {0xE012, "lstick"},                 {0xE013, "keybind_a"},              {0xE014, "keybind_b"},
    {0xE015, "keybind_y"},              {0xE016, "keybind_x"},              {0xE01B, "keybind_lstick_up"},
    {0xE01C, "keybind_lstick_down"},    {0xE01D, "keybind_lstick_right"},   {0xE01E, "keybind_lstick_left"},
    {0xE01F, "keybind_lstick_left"},    {0xE020, "keybind_lstick_up"},      {0xE021, "lstick"},
    {0xE022, "lstick"},                 {0xE023, "lstick"},                 {0xE024, "keybind_rstick_up"},
    {0xE025, "keybind_rstick_down"},    {0xE026, "keybind_rstick_right"},   {0xE027, "keybind_rstick_left"},
    {0xE028, "keybind_rstick_left"},    {0xE029, "keybind_rstick_up"},      {0xE02A, "rstick"},
    {0xE02B, "rstick"},                 {0xE02C, "rstick"},                 {0xE02D, "keybind_dpad_up"},
    {0xE02E, "keybind_dpad_down"},      {0xE02F, "keybind_dpad_right"},     {0xE030, "keybind_dpad_left"},
    {0xE031, "keybind_dpad_down"},      {0xE032, "keybind_dpad_up"},        {0xE033, "keybind_dpad_up"},
    {0xE034, "keybind_dpad_left"},      {0xE035, "keybind_dpad_up"},        {0xE036, "keybind_dpad_left"},
    {0xE037, "keybind_dpad_up"},
};

std::string FirstKey(const char* setting) {
  std::string v;
  try {
    v = rex::cvar::Query<std::string>(setting);
  } catch (...) {
    return {};
  }
  v = v.substr(0, v.find(','));
  while (!v.empty() && v.back() == ' ') v.pop_back();
  while (!v.empty() && v.front() == ' ') v.erase(0, 1);
  return v;
}

// code -> key name (the keycap's), and a version bumped when they change
// (checked at most once a second).
std::mutex g_keys_mutex;
std::map<uint32_t, std::string> g_keys;
std::atomic<uint32_t> g_keys_version{0};
std::chrono::steady_clock::time_point g_keys_checked{};

void CheckBinds() {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_keys_checked < std::chrono::seconds(1)) return;
  g_keys_checked = now;
  auto stick = [](const char* up, const char* left, const char* down, const char* right, const char* special) {
    const std::string u = FirstKey(up), l = FirstKey(left), d = FirstKey(down), r = FirstKey(right);
    if (u == "W" && l == "A" && d == "S" && r == "D") return std::string("WASD");
    if (u == "Up" && l == "Left" && d == "Down" && r == "Right") return std::string("ARROWS");
    return u.empty() ? std::string(special) : u;
  };
  std::map<uint32_t, std::string> keys;
  for (const ButtonBind& b : kBinds) {
    const std::string s = b.setting;
    keys[b.code] = s == "lstick" ? stick("keybind_lstick_up", "keybind_lstick_left", "keybind_lstick_down",
                                         "keybind_lstick_right", "WASD")
                 : s == "rstick" ? stick("keybind_rstick_up", "keybind_rstick_left", "keybind_rstick_down",
                                         "keybind_rstick_right", "ARROWS")
                                 : FirstKey(b.setting);
  }
  std::lock_guard lock(g_keys_mutex);
  if (keys != g_keys) {
    g_keys = std::move(keys);
    ++g_keys_version;
  }
}

// ---------------------------------------------------------------------------
// Fonts.

struct Rect {
  uint16_t x, y, w, h;
};
// One kind of icon page (pad_icons.txt "atlas" and the lines after it).
struct Atlas {
  uint64_t hash = 0;
  uint32_t width = 0, height = 0, total = 0;  // the page; the taller picture's height
  std::string file;
  std::map<uint32_t, Rect> xbox_id;  // (identifies the page in a font: code -> its Xbox rect)
  std::map<uint32_t, Rect> ps, pair;
  std::map<std::string, Rect> keys;  // keycaps by key name
  std::once_flag loaded;
  std::vector<uint8_t> rgba;
};
std::vector<std::unique_ptr<Atlas>> g_atlases;

// A font's icon page and its glyphs' versions (guest byte order). The page is
// known again by its glyphs' place and the first glyph's v1 (a font freed and
// another made at the same place is seen as new).
struct FontState {
  uint32_t page = 0, glyphs = 0, count = 0;
  const Atlas* atlas = nullptr;
  Kind applied = kXbox;
  uint32_t keys_version = ~0u;
  std::array<std::vector<uint8_t>, kKinds> sets;
  float FirstV1(Kind k) const { return RdF(sets[k].data() + 12); }
};
std::mutex g_fonts_mutex;
std::unordered_map<uint32_t, FontState> g_fonts;

// The atlas a font's icon page is, by its size and a few Xbox glyphs' places.
const Atlas* Identify(uint32_t pw, uint32_t ph, uint32_t glyphs, uint32_t count) {
  for (const auto& a : g_atlases) {
    if (a->width != pw || a->height != ph) continue;
    bool match = true;
    for (const auto& [code, r] : a->xbox_id) {
      bool found = false;
      for (uint32_t i = 0; i < count && !found; ++i) {
        const uint8_t* g = g_base + glyphs + i * kGlyph;
        if (Rd32(g + 20) != code) continue;
        found = Rd16(g + 16) == r.w && Rd16(g + 18) == r.h && std::lround(RdF(g) * pw) == r.x &&
                std::lround(RdF(g + 4) * ph) == r.y;
      }
      match &= found;
    }
    if (match) return a.get();
  }
  // (once per kind: a page we have no other pictures for)
  static std::set<std::string> logged;
  std::string id = fmt::format("{}x{}", pw, ph);
  for (uint32_t i = 0; i < count && i < 12; ++i) {
    const uint8_t* g = g_base + glyphs + i * kGlyph;
    id += fmt::format(" {:04X}:{},{},{},{}", Rd32(g + 20), std::lround(RdF(g) * pw), std::lround(RdF(g + 4) * ph),
                      Rd16(g + 16), Rd16(g + 18));
  }
  if (logged.insert(id).second) REXLOG_INFO("[svr2011] pad icons: icon page not known: {}", id);
  return nullptr;
}

void SetRect(const Atlas& a, uint8_t* g, const Rect& r) {
  WrF(g + 0, float(r.x) / a.width);
  WrF(g + 4, float(r.y) / a.total);
  WrF(g + 8, float(r.x + r.w) / a.width);
  WrF(g + 12, float(r.y + r.h) / a.total);
  Wr16(g + 16, r.w);
  Wr16(g + 18, r.h);
}

// A new font's icon page: its versions (the keyboard one: MakeKeys).
void Make(FontState& st) {
  const Atlas& a = *st.atlas;
  auto& x = st.sets[kXbox];
  x.assign(g_base + st.glyphs, g_base + st.glyphs + st.count * kGlyph);
  const float scale = float(a.height) / float(a.total);
  for (uint32_t i = 0; i < st.count; ++i) {
    uint8_t* g = x.data() + i * kGlyph;
    WrF(g + 4, RdF(g + 4) * scale);
    WrF(g + 12, RdF(g + 12) * scale);
  }
  for (const Kind k : {kPlayStation, kPair}) {
    auto& set = st.sets[k];
    set = x;
    const auto& rects = k == kPlayStation ? a.ps : a.pair;
    for (uint32_t i = 0; i < st.count; ++i) {
      uint8_t* g = set.data() + i * kGlyph;
      if (auto it = rects.find(Rd32(g + 20)); it != rects.end()) SetRect(a, g, it->second);
    }
  }
}

void MakeKeys(FontState& st) {
  const Atlas& a = *st.atlas;
  auto& set = st.sets[kKeyboard];
  set = st.sets[kXbox];
  std::lock_guard lock(g_keys_mutex);
  for (uint32_t i = 0; i < st.count; ++i) {
    uint8_t* g = set.data() + i * kGlyph;
    auto key = g_keys.find(Rd32(g + 20));
    if (key == g_keys.end()) continue;
    if (auto it = a.keys.find(key->second); it != a.keys.end()) SetRect(a, g, it->second);
  }
  st.keys_version = g_keys_version;
}

// Before a page search for an icon code: the font's icon page known, and
// holding the version the current text needs.
void Prepare(uint32_t font) {
  const uint8_t* f = g_base + font;
  const uint32_t pages = Rd32(f), array = Rd32(f + 4);
  if (!array || pages == 0 || pages > 8) return;
  uint32_t page = 0;
  for (uint32_t i = 0; i < pages && !page; ++i) {
    const uint32_t pg = array + i * 24;
    const uint32_t glyphs = Rd32(g_base + pg + 16), count = Rd32(g_base + pg + 8);
    if (glyphs && count && Rd32(g_base + glyphs + 20) >= kFirstCode) page = pg;
  }
  if (!page) return;
  const uint32_t glyphs = Rd32(g_base + page + 16), count = Rd32(g_base + page + 8);
  const Kind want = CurrentKind(true);
  if (want == kKeyboard) CheckBinds();
  std::lock_guard lock(g_fonts_mutex);
  FontState& st = g_fonts[font];
  const bool known = st.page == page && st.glyphs == glyphs && st.count == count &&
                     (!st.atlas || st.applied == kKinds || RdF(g_base + glyphs + 12) == st.FirstV1(st.applied));
  if (!known) {
    st = FontState{page, glyphs, count};
    st.atlas = Identify(Rd32(g_base + page), Rd32(g_base + page + 4), glyphs, count);
    if (!st.atlas) return;
    Make(st);
    st.applied = kKinds;  // (written below)
  }
  if (!st.atlas) return;
  if (want == kKeyboard && st.keys_version != g_keys_version) {
    MakeKeys(st);
    if (st.applied == kKeyboard) st.applied = kKinds;
  }
  if (st.applied == want) return;
  std::memcpy(g_base + glyphs, st.sets[want].data(), st.sets[want].size());
  st.applied = want;
}

// ---------------------------------------------------------------------------
// Other pictures.

struct Sprite {
  uint32_t width = 0, height = 0;
  bool dxt1 = false;
  std::array<std::vector<uint8_t>, kKinds> data;  // (by kind; empty: none)
};
std::unordered_map<uint64_t, Sprite> g_sprites;  // by the 360 picture's FNV-1a 64
std::set<std::pair<uint32_t, uint32_t>> g_sprite_sizes;
// What the renderer saw: physical base address -> the sprite there.
std::mutex g_seen_mutex;
std::unordered_map<uint32_t, const Sprite*> g_seen;
std::atomic<size_t> g_seen_count{0};
// Copies of texture objects for the other versions: (object, kind) -> copy.
struct Copy {
  uint32_t object = 0, fetch1 = 0, data = 0;
};
std::mutex g_copies_mutex;
std::map<std::pair<uint32_t, Kind>, Copy> g_copies;

bool Load() {
  const auto dir = rex::filesystem::GetExecutableFolder() / "pad_icons";
  std::ifstream in(dir / "pad_icons.txt");
  if (!in) return false;
  std::ifstream bin(dir / "sprites.bin", std::ios::binary);
  const std::vector<uint8_t> blob((std::istreambuf_iterator<char>(bin)), std::istreambuf_iterator<char>());
  std::string line;
  Atlas* a = nullptr;
  while (std::getline(in, line)) {
    std::istringstream s(line);
    std::string kind;
    if (!(s >> kind) || kind[0] == '#') continue;
    if (kind == "atlas") {
      g_atlases.push_back(std::make_unique<Atlas>());
      a = g_atlases.back().get();
      std::string hash;
      s >> hash >> a->width >> a->height >> a->total >> a->file;
      a->hash = std::stoull(hash, nullptr, 16);
      a->file = (dir / a->file).string();
    } else if (a && kind == "xbox") {
      std::string item;
      while (s >> item) {
        uint32_t code = 0;
        Rect r{};
        if (std::sscanf(item.c_str(), "%x:%hu,%hu,%hu,%hu", &code, &r.x, &r.y, &r.w, &r.h) == 5) a->xbox_id[code] = r;
      }
    } else if (a && (kind == "ps" || kind == "pair")) {
      std::string code;
      Rect r{};
      s >> code >> r.x >> r.y >> r.w >> r.h;
      (kind == "ps" ? a->ps : a->pair)[uint32_t(std::stoul(code, nullptr, 16))] = r;
    } else if (a && kind == "key") {
      std::string name;
      Rect r{};
      s >> name >> r.x >> r.y >> r.w >> r.h;
      a->keys[name] = r;
    } else if (kind == "sprite") {
      std::string version, hash, fourcc;
      uint32_t w = 0, h = 0;
      size_t offset = 0, size = 0;
      s >> version >> hash >> w >> h >> fourcc >> offset >> size;
      if (offset + size > blob.size()) continue;
      Sprite& sp = g_sprites[std::stoull(hash, nullptr, 16)];
      sp.width = w, sp.height = h, sp.dxt1 = fourcc == "DXT1";
      sp.data[version == "ps" ? kPlayStation : kKeyboard].assign(blob.begin() + offset, blob.begin() + offset + size);
      g_sprite_sizes.insert({w, h});
    }
  }
  return !g_atlases.empty();
}

// A copy of a texture object with its data replaced by `data` (the DDS's
// blocks, linear, little-endian): its rows padded to 256 bytes as the GPU
// wants them.
uint32_t MakeCopy(uint32_t object, const Sprite& sp, const std::vector<uint8_t>& data) {
  const uint32_t bpb = sp.dxt1 ? 8 : 16;
  const uint32_t blocks_x = std::max(1u, (sp.width + 3) / 4), blocks_y = std::max(1u, (sp.height + 3) / 4);
  const uint32_t row = blocks_x * bpb, pitch_bytes = (row + 255) & ~255u;
  // (a page more: the GPU reads the 0xE0000000 heap one page on - the data
  // goes where it reads, through the physical address)
  const uint32_t at = g_memory->SystemHeapAlloc(pitch_bytes * blocks_y + 4096, 4096, rex::memory::kSystemHeapPhysical);
  const uint32_t copy = g_memory->SystemHeapAlloc(64, 32);
  if (!at || !copy) return 0;
  uint8_t* dst = g_memory->TranslatePhysical<uint8_t*>(g_memory->GetPhysicalAddress(at));
  for (uint32_t y = 0; y < blocks_y; ++y) std::memcpy(dst + y * pitch_bytes, data.data() + size_t(y) * row, row);
  std::memcpy(g_base + copy, g_base + object, 52);
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch;
  uint32_t* words = &fetch.dword_0;
  for (int i = 0; i < 6; ++i) words[i] = Rd32(g_base + copy + 0x1C + i * 4);
  fetch.tiled = 0;
  fetch.pitch = (pitch_bytes / bpb * 4) >> 5;
  fetch.endianness = rex::graphics::xenos::Endian::kNone;
  fetch.base_address = at >> 12;
  fetch.mip_address = 0;
  fetch.mip_min_level = 0;
  fetch.mip_max_level = 0;
  fetch.packed_mips = 0;
  for (int i = 0; i < 6; ++i) Wr32(g_base + copy + 0x1C + i * 4, words[i]);
  REXLOG_INFO("[svr2011] pad icons: copy of {:08X} ({}x{}) -> {:08X}, data {:08X} (physical {:08X}), fetch {:08X} {:08X}",
              object, sp.width, sp.height, copy, at, g_memory->GetPhysicalAddress(at), words[0], words[1]);
  return copy;
}

}  // namespace

void InstallPadIcons(rex::memory::Memory* memory) {
  g_memory = memory;
  g_base = memory->TranslateVirtual<uint8_t*>(0);
  const std::string renderer = rex::cvar::Query<std::string>("native_renderer");
  if (renderer == "off") {
    REXLOG_INFO("[svr2011] pad icons: off (the emulated renderer)");
    return;
  }
  if (!Load()) {
    REXLOG_INFO("[svr2011] pad icons: no pad_icons/pad_icons.txt - Xbox icons only");
    return;
  }
  g_enabled = true;
  REXLOG_INFO("[svr2011] pad icons: {} icon pages, {} pictures", g_atlases.size(), g_sprites.size());
}

bool PadIconsCandidate(uint32_t width, uint32_t height) {
  if (!g_enabled) return false;
  for (const auto& a : g_atlases)
    if (a->width == width && a->height == height) return true;
  return g_sprite_sizes.count({width, height}) != 0;
}

PadIconsPicture PadIconsAtlas(uint32_t page_width, uint32_t page_height, uint64_t hash) {
  if (!g_enabled) return {};
  for (const auto& a : g_atlases) {
    if (a->hash != hash || a->width != page_width || a->height != page_height) continue;
    Atlas* p = a.get();
    std::call_once(p->loaded, [p] {
      std::ifstream f(p->file, std::ios::binary);
      std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      const size_t size = size_t(p->width) * p->total * 4;
      if (d.size() < 128 + size) {
        REXLOG_WARN("[svr2011] pad icons: {} is missing or short", p->file);
        return;
      }
      p->rgba.assign(d.begin() + 128, d.begin() + 128 + size);
      for (size_t i = 0; i < size; i += 4) std::swap(p->rgba[i], p->rgba[i + 2]);  // (BGRA -> RGBA)
    });
    if (p->rgba.empty()) return {};
    return {p->width, p->total, &p->rgba};
  }
  return {};
}

void PadIconsTextureUploaded(uint32_t physical_base, uint32_t width, uint32_t height, uint64_t hash) {
  if (!g_enabled) return;
  const auto it = hash ? g_sprites.find(hash) : g_sprites.end();
  const Sprite* sp = it != g_sprites.end() && it->second.width == width && it->second.height == height ? &it->second
                                                                                                     : nullptr;
  std::lock_guard lock(g_seen_mutex);
  if (sp) {
    g_seen[physical_base] = sp;
  } else {
    g_seen.erase(physical_base);
  }
  g_seen_count = g_seen.size();
}

void SetPadIconsOwner(int user_index) { t_owner = user_index; }

}  // namespace svr2011

// The page search: sub_826AD450(font, code) -> the page holding code (or -1).
REX_EXTERN(__imp__sub_826AD450);
REX_HOOK_RAW(sub_826AD450) {
  if (svr2011::g_enabled && ctx.r4.u32 >= svr2011::kFirstCode) svr2011::Prepare(ctx.r3.u32);
  __imp__sub_826AD450(ctx, base);
}

namespace svr2011 {
namespace {

// Whether a guest address can be read (the game's heaps; a field that holds
// something else is skipped, not followed).
bool Readable(uint32_t a) { return a >= 0x40000000u && a < 0xFFFF0000u && (a & 3) == 0; }

// The texture object for a picture's other version (0: none / not known).
uint32_t VersionOf(uint32_t object, Kind kind) {
  if (!Readable(object) || !g_seen_count.load(std::memory_order_relaxed)) return 0;
  const uint8_t* o = g_base + object;
  if ((Rd32(o) & 0xF) != 3) return 0;  // (D3D resource type: texture)
  const uint32_t size = Rd32(o + 0x1C + 8);  // (fetch dword 2: width - 1, height - 1)
  const uint32_t w = (size & 0x1FFF) + 1, h = ((size >> 13) & 0x1FFF) + 1;
  if (!g_sprite_sizes.count({w, h})) return 0;
  const uint32_t fetch1 = Rd32(o + 0x1C + 4);
  const uint32_t physical = g_memory->GetPhysicalAddress(fetch1 & 0xFFFFF000u);
  const Sprite* sp = nullptr;
  {
    std::lock_guard lock(g_seen_mutex);
    if (auto it = g_seen.find(physical); it != g_seen.end()) sp = it->second;
  }
  if (!sp || sp->data[kind].empty()) return 0;
  std::lock_guard lock(g_copies_mutex);
  Copy& c = g_copies[{object, kind}];
  if (c.object && c.fetch1 != fetch1) c = Copy{};  // (the object now holds another picture)
  if (!c.object) c = Copy{MakeCopy(object, *sp, sp->data[kind]), fetch1, 0};
  return c.object;
}

// The game's 2D texture records (vtable 0x8205CF48, 0x70 bytes; +80 the D3D
// texture object) and copies of them pointing at another version.
struct RecordCopy {
  uint32_t record = 0, object = 0;
};
std::mutex g_records_mutex;
std::map<std::pair<uint32_t, Kind>, RecordCopy> g_records;
thread_local uint32_t t_pair = 0;  // (a {handle, record} pair for the command)

uint32_t RecordVersion(uint32_t record, Kind kind) {
  constexpr uint32_t kTextureRecord = 0x8205CF48;  // (vtable)
  if (!Readable(record) || Rd32(g_base + record) != kTextureRecord || Rd32(g_base + record + 52) != 0) return 0;
  const uint32_t object = Rd32(g_base + record + 80);
  const uint32_t version = VersionOf(object, kind);
  if (!version) return 0;
  std::lock_guard lock(g_records_mutex);
  RecordCopy& c = g_records[{record, kind}];
  if (!c.record) c.record = g_memory->SystemHeapAlloc(0x70, 32);
  if (!c.record) return 0;
  if (c.object != version) {
    std::memcpy(g_base + c.record, g_base + record, 0x70);
    Wr32(g_base + c.record + 80, version);
    c.object = version;
  }
  return c.record;
}

// The player behind a character select slot: its setup record's controller
// port (+56; -1 for COM) - mgr *0x82EDE630 + 120 + *(mgr+13520) * 1352 +
// slot * 204.
int SlotPort(uint32_t slot) {
  const uint32_t mgr = Rd32(g_base + 0x82EDE630);
  if (mgr < 0x40000000u || mgr >= 0xF0000000u || slot > 5) return -1;
  const uint32_t mode = Rd32(g_base + mgr + 13520);
  if (mode > 8) return -1;  // (not set up)
  const uint32_t rec = mgr + 120 + mode * 1352 + slot * 204;
  const int32_t port = int32_t(Rd32(g_base + rec + 56));
  return port >= 0 && port < 4 ? port : -1;
}

// A wrestler HUD's player: its character (+80)'s controller port (+1152; 4
// and up: the CPU).
int HudOwner(uint32_t hud) {
  const uint32_t ch = Readable(hud) ? Rd32(g_base + hud + 80) : 0;
  const uint32_t port = Readable(ch) ? Rd32(g_base + ch + 1152) : ~0u;
  return port < 4 ? int(port) : -1;
}

// 2D layouts made by a wrestler's HUD for its prompts (drawn later, by the
// 2D layer): layout -> the HUD (its player is looked up at each draw - the
// HUD gets its character after it makes them, and tag partners swap).
thread_local uint32_t t_making = 0;
thread_local uint32_t t_drawing_hud = 0;  // (debug)
std::mutex g_layouts_mutex;
std::unordered_map<uint32_t, uint32_t> g_layouts;
// ... and their sprites (sub_82416618 makes one; its texture pair at +0x110 is
// what the "set texture" command gets): sprite -> the HUD.
std::unordered_map<uint32_t, uint32_t> g_sprites_hud;
// The wrestler HUDs drawn lately and their players (-1: the CPU's).
struct HudSeen {
  int owner;
  std::chrono::steady_clock::time_point at;
};
std::unordered_map<uint32_t, HudSeen> g_huds;

// A HUD's player; a CPU wrestler's HUD shows its prompts (the submission
// meter, ...) to the human against it: the match's only human, if one.
int ResolveHud(uint32_t hud) {
  const int owner = HudOwner(hud);
  if (owner >= 0) return owner;
  const auto now = std::chrono::steady_clock::now();
  int human = -1;
  for (const auto& [h, seen] : g_huds) {
    if (seen.owner < 0 || now - seen.at > std::chrono::seconds(2)) continue;
    if (human >= 0 && human != seen.owner) return -1;  // (more than one human: no one in particular)
    human = seen.owner;
  }
  return human;
}

struct OwnerScope {
  int saved;
  explicit OwnerScope(int owner) : saved(t_owner) { t_owner = owner; }
  ~OwnerScope() { t_owner = saved; }
};

}  // namespace
}  // namespace svr2011

// The 2D command "set texture" (render list type 24): sub_826DE6B0(cmd,
// pair = {handle, record}, sampler) - written now, played back on the render
// thread (SetTexture there knows nothing of whose picture it is). For a
// player's picture the command gets a copy of the record with the version
// for that player's controller.
REX_EXTERN(__imp__sub_826DE6B0);
REX_HOOK_RAW(sub_826DE6B0) {
  using namespace svr2011;
  // (debug: SVR2011_PAD_ICONS_LOG=1 - each record seen with an owner, once)
  static const bool debug = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
  if (debug && ctx.r4.u32 && Readable(ctx.r4.u32)) {
    static std::set<uint64_t> logged;
    const uint32_t record = Rd32(base + ctx.r4.u32 + 4);
    const uint32_t object = Readable(record) ? Rd32(base + record + 80) : 0;
    if (Readable(record) && (t_owner >= 0 || VersionOf(object, kPlayStation)) && logged.insert(uint64_t(record) << 8 | uint8_t(t_owner + 1)).second &&
        logged.size() < 2000) {
      REXLOG_INFO("[svr2011] pad icons: record {:08X} pair {:08X} owner {} (object {:08X} size {:08X} caller {:08X}){}", record, ctx.r4.u32,
                  t_owner, object, Readable(object) ? Rd32(base + object + 0x24) : 0, uint32_t(ctx.lr),
                  VersionOf(object, kPlayStation) ? " - has a PlayStation version" : "");
    }
  }
  // (a sprite a wrestler HUD made, drawn later by the 2D layer: its player)
  std::optional<OwnerScope> sprite_owner;
  if (g_enabled && t_owner < 0 && ctx.r4.u32) {
    std::lock_guard lock(g_layouts_mutex);
    if (auto it = g_sprites_hud.find(ctx.r4.u32 - 0x110); it != g_sprites_hud.end())
      sprite_owner.emplace(ResolveHud(it->second));
  }
  if (g_enabled && t_owner >= 0 && ctx.r4.u32) {
    const Kind kind = CurrentKind(false);
    if (kind == kPlayStation || kind == kKeyboard) {
      const uint32_t record = Readable(ctx.r4.u32) ? Rd32(base + ctx.r4.u32 + 4) : 0;
      if (const uint32_t copy = record ? RecordVersion(record, kind) : 0) {
        if (!t_pair) t_pair = g_memory->SystemHeapAlloc(8, 8);
        if (t_pair) {
          Wr32(base + t_pair, Rd32(base + ctx.r4.u32));
          Wr32(base + t_pair + 4, copy);
          ctx.r4.u64 = t_pair;
        }
      }
    }
  }
  __imp__sub_826DE6B0(ctx, base);
}

// Whose pictures are being drawn:
// - a wrestler's HUD (pins, submissions, button prompts, finishers, ...):
//   sub_82404F60(hud) draws, sub_824050C0(hud) updates - and makes the
//   prompts' layouts (sub_82402818 -> the layout), drawn later through their
//   node (+192, sub_82418B70);
REX_EXTERN(__imp__sub_82404F60);
REX_HOOK_RAW(sub_82404F60) {
  using namespace svr2011;
  int owner = -1;
  if (g_enabled) {
    std::lock_guard lock(g_layouts_mutex);
    g_huds[ctx.r3.u32] = HudSeen{HudOwner(ctx.r3.u32), std::chrono::steady_clock::now()};
    owner = ResolveHud(ctx.r3.u32);
  }
  OwnerScope scope(owner);
  const uint32_t saved = t_drawing_hud;
  t_drawing_hud = ctx.r3.u32;
  __imp__sub_82404F60(ctx, base);
  t_drawing_hud = saved;
}

// (the HUD made: sub_82403E48(hud, match manager + 376); updated, making
// more on demand: sub_824050C0(hud))
REX_EXTERN(__imp__sub_82403E48);
REX_HOOK_RAW(sub_82403E48) {
  using namespace svr2011;
  const uint32_t saved = t_making;
  t_making = g_enabled ? ctx.r3.u32 : 0;
  __imp__sub_82403E48(ctx, base);
  t_making = saved;
}

REX_EXTERN(__imp__sub_824050C0);
REX_HOOK_RAW(sub_824050C0) {
  using namespace svr2011;
  const uint32_t saved = t_making;
  t_making = g_enabled ? ctx.r3.u32 : 0;
  OwnerScope scope(g_enabled ? HudOwner(ctx.r3.u32) : -1);
  __imp__sub_824050C0(ctx, base);
  t_making = saved;
}

REX_EXTERN(__imp__sub_82402818);
REX_HOOK_RAW(sub_82402818) {
  using namespace svr2011;
  __imp__sub_82402818(ctx, base);
  if (!g_enabled || !ctx.r3.u32) return;
  std::lock_guard lock(g_layouts_mutex);
  if (t_making) {
    static const bool debug = std::getenv("SVR2011_PAD_ICONS_LOG") != nullptr;
    if (debug) REXLOG_INFO("[svr2011] pad icons: layout {:08X} made by wrestler HUD {:08X}", ctx.r3.u32, t_making);
    g_layouts[ctx.r3.u32] = t_making;
  } else {
    g_layouts.erase(ctx.r3.u32);  // (a layout of no one's, perhaps where one was)
  }
}

// (a sprite made: sub_82416618(sprite))
REX_EXTERN(__imp__sub_82416618);
REX_HOOK_RAW(sub_82416618) {
  using namespace svr2011;
  const uint32_t sprite = ctx.r3.u32;
  __imp__sub_82416618(ctx, base);
  if (!g_enabled) return;
  std::lock_guard lock(g_layouts_mutex);
  if (t_making) {
    g_sprites_hud[sprite] = t_making;
  } else {
    g_sprites_hud.erase(sprite);
  }
}

// (a layout drawn whole: sub_823D1C48(layout, node, ...))
REX_EXTERN(__imp__sub_823D1C48);
REX_HOOK_RAW(sub_823D1C48) {
  using namespace svr2011;
  int owner = t_owner;
  if (g_enabled) {
    std::lock_guard lock(g_layouts_mutex);
    if (auto it = g_layouts.find(ctx.r3.u32); it != g_layouts.end()) owner = ResolveHud(it->second);
  }
  OwnerScope scope(owner);
  __imp__sub_823D1C48(ctx, base);
}

REX_EXTERN(__imp__sub_82418B70);
REX_HOOK_RAW(sub_82418B70) {
  using namespace svr2011;
  int owner = t_owner;
  if (g_enabled) {
    std::lock_guard lock(g_layouts_mutex);
    if (auto it = g_layouts.find(ctx.r3.u32 - 192); it != g_layouts.end()) owner = ResolveHud(it->second);
  }
  OwnerScope scope(owner);
  __imp__sub_82418B70(ctx, base);
}

// - a character select panel: sub_82467C18(cursor + 28), the slot at +256;
REX_EXTERN(__imp__sub_82467C18);
REX_HOOK_RAW(sub_82467C18) {
  using namespace svr2011;
  OwnerScope scope(g_enabled ? SlotPort(Rd32(base + ctx.r3.u32 + 256)) : -1);
  __imp__sub_82467C18(ctx, base);
}

// - character select's bottom bar ("contro02_1P"): player 1's slot.
REX_EXTERN(__imp__sub_8244D158);
REX_HOOK_RAW(sub_8244D158) {
  using namespace svr2011;
  OwnerScope scope(g_enabled ? SlotPort(0) : -1);
  __imp__sub_8244D158(ctx, base);
}

// D3DDevice_SetTexture(device, sampler, texture object), on the render
// thread: pictures shared by everyone follow the players' controllers when
// they all use the same kind.
REX_EXTERN(__imp__sub_82917EC8);
REX_HOOK_RAW(sub_82917EC8) {
  using namespace svr2011;
  if (g_enabled && ctx.r5.u32) {
    const Kind kind = CurrentKind(false);
    if (kind == kPlayStation || kind == kKeyboard)
      if (const uint32_t version = VersionOf(ctx.r5.u32, kind)) ctx.r5.u64 = version;
  }
  __imp__sub_82917EC8(ctx, base);
}
