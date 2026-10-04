// WWE SmackDown vs. Raw 2011 - PlayStation button pictures (see pad_icons.h).
//
// The game's fonts:
//   font +0 page count, +4 pages (24 bytes each: +0 texture width, +4 height,
//   +8 glyph count, +12 kerning count, +16 glyphs, +20 kerning);
//   glyph (24 bytes): +0 u0, +4 v0, +8 u1, +12 v1 (floats, of the page's
//   texture), +16 width, +18 height (pixels, u16), +20 code - sorted by code.
// Glyphs are found by sub_826AD450(font, code) -> the page, then a search of
// its glyphs (in sub_826AD608, the lookup, and inline in the text measuring
// and layout code). So the icon page's glyphs are rewritten where they are:
// each font keeps its three versions (Xbox, PlayStation, pairs; every v moved
// to its section of the taller picture) and the page holds the one the text
// being measured or drawn needs - set at each page search for an icon code.

#include "pad_icons.h"

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
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

enum Kind : uint8_t { kXbox, kPlayStation, kPair, kKinds };

// One kind of icon page (pad_icons.txt "atlas" and the lines after it).
struct Rect {
  uint16_t x, y, w, h;
};
struct Atlas {
  uint64_t hash = 0;
  uint32_t width = 0, height = 0, total = 0;  // the page; the taller picture's height
  std::string file;
  std::map<uint32_t, Rect> xbox_id;  // (identifies the page in a font: code -> its Xbox rect)
  std::map<uint32_t, Rect> ps, pair;
  std::once_flag loaded;
  std::vector<uint8_t> rgba;
};
std::vector<std::unique_ptr<Atlas>> g_atlases;
bool g_enabled = false;
uint8_t* g_base = nullptr;

thread_local int t_owner = -1;

// A font's icon page and its glyphs' three versions (guest byte order). The
// page is known again by its glyphs' place and the first glyph's v1 (a font
// freed and another made at the same place is seen as new).
struct FontState {
  uint32_t page = 0, glyphs = 0, count = 0;
  const Atlas* atlas = nullptr;
  Kind applied = kXbox;
  std::array<std::vector<uint8_t>, kKinds> sets;
  float FirstV1(Kind k) const { return RdF(sets[k].data() + 12); }
};
std::mutex g_fonts_mutex;
std::unordered_map<uint32_t, FontState> g_fonts;

bool Load() {
  const auto dir = rex::filesystem::GetExecutableFolder() / "pad_icons";
  std::ifstream in(dir / "pad_icons.txt");
  if (!in) return false;
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
    }
  }
  return !g_atlases.empty();
}

Kind CurrentKind() {
  auto of = [](PadType t) { return t == PadType::kPlayStation ? kPlayStation : kXbox; };
  if (t_owner >= 0) return of(PlayerPadType(uint32_t(t_owner)));
  // Shared: what the players use (the keyboard shows the Xbox pictures).
  const uint32_t in_use = PadTypesInUse();
  const bool ps = in_use & (1u << uint32_t(PadType::kPlayStation));
  const bool other = in_use & (1u << uint32_t(PadType::kXbox) | 1u << uint32_t(PadType::kKeyboard));
  return ps ? (other ? kPair : kPlayStation) : kXbox;
}

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
  // (once per kind: a page we have no PlayStation pictures for)
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

// A new font's icon page: its three versions.
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
      auto it = rects.find(Rd32(g + 20));
      if (it == rects.end()) continue;
      const Rect& r = it->second;
      WrF(g + 0, float(r.x) / a.width);
      WrF(g + 4, float(r.y) / a.total);
      WrF(g + 8, float(r.x + r.w) / a.width);
      WrF(g + 12, float(r.y + r.h) / a.total);
      Wr16(g + 16, r.w);
      Wr16(g + 18, r.h);
    }
  }
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
  const Kind want = CurrentKind();
  std::lock_guard lock(g_fonts_mutex);
  FontState& st = g_fonts[font];
  const bool known = st.page == page && st.glyphs == glyphs && st.count == count &&
                     (!st.atlas || RdF(g_base + glyphs + 12) == st.FirstV1(st.applied));
  if (!known) {
    st = FontState{page, glyphs, count};
    st.atlas = Identify(Rd32(g_base + page), Rd32(g_base + page + 4), glyphs, count);
    if (!st.atlas) return;
    Make(st);
    st.applied = Kind(kKinds);  // (written below)
  }
  if (!st.atlas || st.applied == want) return;
  std::memcpy(g_base + glyphs, st.sets[want].data(), st.sets[want].size());
  st.applied = want;
}

}  // namespace

void InstallPadIcons(rex::memory::Memory* memory) {
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
  REXLOG_INFO("[svr2011] pad icons: {} icon pages with PlayStation pictures", g_atlases.size());
}

bool PadIconsCandidate(uint32_t width, uint32_t height) {
  if (!g_enabled) return false;
  for (const auto& a : g_atlases)
    if (a->width == width && a->height == height) return true;
  return false;
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

void SetPadIconsOwner(int user_index) { t_owner = user_index; }

}  // namespace svr2011

// The page search: sub_826AD450(font, code) -> the page holding code (or -1).
REX_EXTERN(__imp__sub_826AD450);
REX_HOOK_RAW(sub_826AD450) {
  if (svr2011::g_enabled && ctx.r4.u32 >= svr2011::kFirstCode) svr2011::Prepare(ctx.r3.u32);
  __imp__sub_826AD450(ctx, base);
}
