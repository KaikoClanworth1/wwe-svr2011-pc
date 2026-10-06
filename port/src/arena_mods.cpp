// Arena mods (arenas branch).
//
// How an arena file is opened: the game preloads its pac files through the
// XDK file cache (sub_826A7328 -> sub_8290A668 with the name table at
// 0x82DAC7C0) and opens an arena when it loads, as D:\PAC\BG\BGnn.PAC (not
// held open). That path resolves through symbolic links to
// \Device\Harddisk0\Partition1\PAC\BG\BGnn.PAC (the game folder). The
// file system resolves the directory (D:\PAC\BG) and then looks the file up
// in it, so the redirect works on the directory: \PAC\BG is linked to an
// overlay folder (<game>/Mods/ArenaOverlay) that holds hard links to every
// original arena file (copies where hard links are not possible), and a
// redirected arena's entry there is a link to the mod's file instead. Arena
// files are only open while they load, so entries can be swapped between
// matches. The originals are never written.
// (Changing the name table or hooking the game's CreateFile wrappers does not
// reach this open; the XDK file cache library code does it.)
//
// Arena select: the 5 x 4 grid is a generic grid widget updated by
// sub_823D4FC0(widget): +0 vtable, +28 row, +32 rows, +36 row wrap, +44
// column, +48 columns, +52 column wrap, +64 buttons, +88 moved (1 up, 2 down,
// 4 left, 8 right). The 20 banners are DXT5 256 x 128 textures from
// menu/MatchHD.pac (MENU/MASI), kept in guest physical memory (4 KB view)
// untiled, with 16-bit words byte-swapped.
#include "arena_mods.h"
#include "crowd_off.h"
#include "move_packs.h"
#include "ring_rules.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

#include <atomic>
#include <cfloat>

#include <fmt/format.h>
#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>
#include <rex/ui/imgui_dialog.h>

#include "generated/default/svr2011_init.h"
#include "../modmaker/svrfmt/pac.h"
#include "../modmaker/svrfmt/texture.h"

namespace {

constexpr const char* kGameDevice = "\\Device\\Harddisk0\\Partition1";
namespace fs = std::filesystem;

rex::memory::Memory* g_memory = nullptr;
rex::filesystem::VirtualFileSystem* g_fs = nullptr;
std::mutex g_mutex;
fs::path g_game, g_overlay;
bool g_overlay_ready = false;
std::string g_redirect[100];  // arena number -> relative file ("" = original)
std::string g_default[100];   // arena number -> its file when no custom arena is on its tile (media mods: screens)

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// Makes the file system see overlay/<name> as it is now: the host device
// indexes a folder created after startup lazily, and keeps a file's size
// from when it first saw it.
void Refresh(const std::string& name) {
  const std::string path = std::string(kGameDevice) + "\\Mods\\ArenaOverlay\\" + name;
  if (auto* e = g_fs->ResolvePath(path)) e->update();
}

std::map<std::string, fs::path> g_placed;  // overlay name -> the file asked for

// overlay/<name> := link to (or copy of) `source` - with the crowd off
// (crowd_off.h) its crowd-less copy once made.
bool Place(const fs::path& requested, const std::string& name) {
  g_placed[name] = requested;
  const fs::path source = svr2011::crowd::Serve(requested, g_game / "Mods" / "ArenaCrowdless", [requested, name] {
    std::lock_guard lock(g_mutex);
    if (g_placed[name] == requested) Place(requested, name);  // (now the copy)
  });
  std::error_code ec;
  const fs::path dst = g_overlay / name;
  // An overlay entry may be a hard link to an original: never write through
  // it. If it cannot be removed, give up instead of copying over it.
  fs::remove(dst, ec);
  if (fs::exists(dst, ec)) {
    REXLOG_WARN("[svr2011] arena mods: cannot replace {}", name);
    return false;
  }
  fs::create_hard_link(source, dst, ec);
  if (ec) {
    fs::copy_file(source, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      REXLOG_WARN("[svr2011] arena mods: cannot place {} ({})", name, ec.message());
      return false;
    }
  }
  Refresh(name);
  return true;
}

// The overlay mirrors pac/bg, then \PAC\BG is linked to it (once).
bool PrepareOverlay() {
  if (g_overlay_ready) return true;
  std::error_code ec;
  const fs::path bg = g_game / "pac" / "bg";
  fs::create_directories(g_overlay, ec);
  if (ec || !fs::is_directory(bg, ec)) return false;
  for (const auto& e : fs::directory_iterator(bg, ec)) {
    if (!e.is_regular_file()) continue;
    const std::string name = Upper(e.path().filename().string());
    const fs::path dst = g_overlay / name;
    // keep an existing link to the same original (same size and time)
    std::error_code e1, e2;
    if (fs::exists(dst, e1) && fs::file_size(dst, e1) == e.file_size() &&
        fs::last_write_time(dst, e2) == e.last_write_time()) {
      Refresh(name);
      continue;
    }
    if (!Place(e.path(), name)) return false;
  }
  g_fs->RegisterSymbolicLink(std::string(kGameDevice) + "\\PAC\\BG",
                             std::string(kGameDevice) + "\\Mods\\ArenaOverlay");
  g_overlay_ready = true;
  REXLOG_INFO("[svr2011] arena mods: overlay ready ({})", g_overlay.string());
  return true;
}

// Guest memory region that is committed (and readable).
bool Committed(uint32_t at, rex::memory::HeapAllocationInfo& info) {
  auto* heap = g_memory->LookupHeap(at);
  info = {};
  return heap && heap->QueryRegionInfo(at & ~0xFFFu, &info) && info.region_size &&
         (info.state & rex::memory::kMemoryAllocationCommit) && (info.protect & rex::memory::kMemoryProtectRead);
}

// Calls fn(start, end) for each committed region in [lo, hi).
template <class F>
void ForEachRegion(uint64_t lo, uint64_t hi, F fn) {
  for (uint64_t at = lo; at < hi;) {
    rex::memory::HeapAllocationInfo info;
    if (!Committed(uint32_t(at), info)) {
      at = (at & ~0xFFFull) + (info.region_size ? info.region_size : 0x1000);
      continue;
    }
    const uint64_t end = std::min<uint64_t>(uint64_t(info.base_address) + info.region_size, hi);
    fn(at, end);
    at = end;
  }
}

// Test aid: SVR2011_TEST_BPE_LOG=1 logs every BPE decode (the game's
// sub_826AEF10(src, dst)): caller, source, destination, unpacked size.
bool g_bpe_log = false;

// Test aid: SVR2011_TEST_SCAN=1 - each time <game>/scan.txt changes, its text
// is a request: "<decimal>" narrows a cheat-search for that value (big-endian
// u32 and byte) kept across requests; "hex:<bytes>" finds a byte pattern.
void StartScan() {
  std::thread([] {
    const fs::path trigger = g_game / "scan.txt";
    std::string last_text;
    std::vector<uint32_t> cand32, cand8;
    bool first = true;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      std::error_code ec;
      if (!fs::exists(trigger, ec)) continue;
      std::string text;
      if (FILE* f = std::fopen(trigger.string().c_str(), "rb")) {
        char b[256] = {};
        std::fread(b, 1, sizeof b - 1, f);
        std::fclose(f);
        text = b;
      }
      if (text.empty() || text == last_text) continue;
      last_text = text;
      uint8_t* b = g_memory->virtual_membase();
      if (text.rfind("hex:", 0) == 0) {
        std::vector<uint8_t> pat;
        for (size_t i = 4; i + 1 < text.size(); i += 2)
          pat.push_back(uint8_t(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16)));
        std::string hits;
        int n = 0;
        if (!pat.empty())
          ForEachRegion(0x40000000, 0xFFFF0000, [&](uint64_t at, uint64_t end) {
            for (uint64_t a = at; a + pat.size() <= end; ++a)
              if (b[a] == pat[0] && !std::memcmp(b + a, pat.data(), pat.size()) && ++n <= 40)
                hits += fmt::format(" {:08X}", uint32_t(a));
          });
        REXLOG_INFO("[svr2011] find {}: {} hits [{}]", text, n, hits);
        continue;
      }
      const uint32_t want = uint32_t(std::strtoul(text.c_str(), nullptr, 10));
      auto be32 = [&](uint32_t a) { return uint32_t(b[a]) << 24 | b[a + 1] << 16 | b[a + 2] << 8 | b[a + 3]; };
      auto readable = [&](uint32_t a) {
        rex::memory::HeapAllocationInfo info;
        return Committed(a, info);
      };
      if (first) {
        ForEachRegion(0x40000000, 0xFFFF0000, [&](uint64_t at, uint64_t end) {
          for (uint64_t a = at; a + 4 <= end; a += 4)
            if (be32(uint32_t(a)) == want) cand32.push_back(uint32_t(a));
          for (uint64_t a = at; a < end; ++a)
            if (b[a] == want && cand8.size() < 20000000) cand8.push_back(uint32_t(a));
        });
        first = false;
      } else {
        std::vector<uint32_t> k32, k8;
        for (uint32_t a : cand32)
          if (readable(a) && be32(a) == want) k32.push_back(a);
        for (uint32_t a : cand8)
          if (readable(a) && b[a] == want) k8.push_back(a);
        cand32.swap(k32);
        cand8.swap(k8);
      }
      std::string s32, s8;
      for (size_t i = 0; i < cand32.size() && i < 40; ++i) s32 += fmt::format(" {:08X}", cand32[i]);
      for (size_t i = 0; i < cand8.size() && i < 40; ++i) s8 += fmt::format(" {:08X}", cand8[i]);
      REXLOG_INFO("[svr2011] scan {}: u32 {} [{}] u8 {} [{}]", want, cand32.size(), s32, cand8.size(), s8);
    }
  }).detach();
}

// ---- arena select pages
//
// Page 1 is the game's own 20 arenas. Pages 2+ show the installed custom
// arenas in the same tiles (Mods/Arenas/<id>/: arena.pac, banner.dds = DXT5
// 256 x 128, manifest.txt name=...; a "disabled" file turns one off).
// Moving past the right edge goes to the next page, past the left edge to
// the previous one (as the Paint Tool grid); LB / RB change the page too.
// A custom arena borrows its tile's arena as host: while its tile is the one
// under the cursor, the host's file is the custom arena's
// (RedirectArena), so the match that follows loads it.

struct Banner {
  std::string name;    // "arena_SD"
  svrfmt::Bytes data;  // DXT5 blocks as the game keeps them (16-bit swapped)
  // where the game keeps it while the screen is open: every place holding the
  // whole banner (a 64-byte match alone can take a wrong place - a 2.0.3
  // player's pages showed banners on the wrong tiles, half drawn)
  std::vector<uint32_t> guest;
};
std::vector<Banner> g_banners;  // the 20 originals

struct CustomArena {
  std::string id, name, file;  // file: relative to the game folder
  std::string manifest;        // its text (ring.* keys: ring_rules.h)
  std::string base;            // the arena it was made from (manifest base=arena_SS)
  svrfmt::Bytes banner;        // DXT5 blocks, 16-bit swapped (empty = none)
  // its VS screen pictures (vs/<name>.dds): name -> DXT5 blocks, swapped
  std::vector<std::pair<std::string, svrfmt::Bytes>> vs;
  // its loading screen (load.dds, 1024 x 512 DXT5): blocks, swapped
  svrfmt::Bytes load;
};
std::vector<CustomArena> g_customs;

uint32_t g_select_widget = 0;
std::atomic<int64_t> g_select_seen{0};  // when the arena grid was last updated (ms)
std::atomic<int> g_cursor_custom{-1};    // custom arena under the cursor (label)
int g_page = 0;              // 0 = the game's arenas
std::atomic<uint16_t> g_shoulders[8];  // each player's LB / RB as last read (ArenaSelectPad)
int g_redirected_host = -1;  // host arena currently pointed at a custom one

constexpr size_t kBannerBlocks = 64 * 32;  // 256 x 128 DXT5, 16 bytes a block
// The arena select grid widget: 5 columns, 5 rows (the 5th row is the
// DOWNLOADED ARENAS button).
constexpr uint32_t kSelectVtable = 0x8201D200;
// Tiles, row by row: banner name and arena number (the table at 0x8201D030
// pairs numbers with names; the order is the screen's).
struct Tile {
  const char* banner;
  int arena;
};
constexpr Tile kTiles[20] = {
    {"arena_SD", 0},   {"arena_RAW", 1},   {"arena_WM26", 11},  {"arena_slam", 4}, {"arena_SS", 17},
    {"arena_HinC", 6}, {"arena_Judg", 13}, {"arena_BASH", 15},  {"arena_NoC", 14}, {"arena_BP", 5},
    {"arena_RR", 9},   {"arena_BR", 18},   {"arena_Series", 7}, {"arena_TLC", 8},  {"arena_ttt", 3},
    {"arena_EC", 10},  {"arena_back", 12}, {"arena_EXT", 16},   {"arena_ECW", 2},  {"arena_drui", 19},
};

// DDS block data -> the layout the game keeps (16-bit words byte-swapped)
svrfmt::Bytes Swap16(const uint8_t* p, size_t n) {
  svrfmt::Bytes o(p, p + n);
  for (size_t i = 0; i + 1 < n; i += 2) std::swap(o[i], o[i + 1]);
  return o;
}

// The top level of a 256 x 128 DXT5 DDS, swapped (empty if it is not that).
svrfmt::Bytes BannerBlocks(const svrfmt::Bytes& dds) {
  svrfmt::DdsInfo info;
  if (!svrfmt::DdsInfoOf(dds, info) || info.w != 256 || info.h != 128 || info.format != svrfmt::DxtFormat::kDxt5 ||
      dds.size() < 128 + kBannerBlocks * 16)
    return {};
  return Swap16(dds.data() + 128, kBannerBlocks * 16);
}

void LoadBanners() {
  svrfmt::Bytes d;
  svrfmt::Epac e;
  if (!svrfmt::ReadFile((g_game / "pac" / "menu" / "MatchHD.pac").string(), d) || !svrfmt::EpacRead(d, e)) return;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name != "MASI") continue;
      std::vector<svrfmt::PachEntry> ents;
      if (!svrfmt::PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        const svrfmt::Bytes raw = svrfmt::Unpack(pe.data);
        std::vector<svrfmt::BundleTexture> texs;
        if (!svrfmt::BundleRead(raw, texs)) continue;
        for (auto& t : texs)
          if (t.name.rfind("arena_", 0) == 0)
            if (svrfmt::Bytes blocks = BannerBlocks(t.data); !blocks.empty())
              g_banners.push_back({t.name, std::move(blocks), {}});
      }
    }
}

// Custom arenas on the pages: each on the tile of the arena it was made
// from (manifest base=), so it plays in that arena's place - the file the
// game loads for that tile, with the memory room and VS screen style it was
// made for. More than one on the same arena: pages 3, 4... Arenas without a
// base fill the free tiles.
std::vector<std::array<int, 20>> g_layout;  // pages 2.. : tile -> index in g_customs (-1)

int TileOf(const std::string& banner) {
  for (int k = 0; k < 20; ++k)
    if (banner == kTiles[k].banner) return k;
  return -1;
}

void LayOutPages() {
  g_layout.clear();
  auto place = [](size_t i, int tile) {
    for (auto& page : g_layout)
      if (tile >= 0 ? page[tile] < 0 : true) {
        if (tile >= 0) { page[tile] = int(i); return; }
        for (int& t : page)
          if (t < 0) { t = int(i); return; }
      }
    std::array<int, 20> page;
    page.fill(-1);
    page[tile >= 0 ? tile : 0] = int(i);
    g_layout.push_back(page);
  };
  for (size_t i = 0; i < g_customs.size(); ++i)
    if (TileOf(g_customs[i].base) >= 0) place(i, TileOf(g_customs[i].base));
  for (size_t i = 0; i < g_customs.size(); ++i)
    if (TileOf(g_customs[i].base) < 0) place(i, -1);
}

void LoadCustomArenas() {
  std::error_code ec;
  const fs::path dir = g_game / "Mods" / "Arenas";
  if (!fs::is_directory(dir, ec)) return;
  std::vector<fs::path> folders;
  for (const auto& e : fs::directory_iterator(dir, ec))
    if (e.is_directory() && fs::exists(e.path() / "arena.pac", ec) && !fs::exists(e.path() / "disabled", ec))
      folders.push_back(e.path());  // ("disabled": turned off in the launcher's Mods tab)
  std::sort(folders.begin(), folders.end());
  for (const auto& f : folders) {
    CustomArena c;
    c.id = f.filename().string();
    c.name = c.id;
    c.file = "Mods/Arenas/" + c.id + "/arena.pac";
    const fs::path manifest = fs::exists(f / "manifest.txt", ec) ? f / "manifest.txt" : f / "info.txt";
    if (FILE* t = std::fopen(manifest.string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("name=", 0) == 0) c.name = l.substr(5);
        if (l.rfind("base=", 0) == 0) c.base = l.substr(5);
        c.manifest += l;
        c.manifest += '\n';
      }
      std::fclose(t);
    }
    svrfmt::Bytes dds;
    if (svrfmt::ReadFile((f / "banner.dds").string(), dds)) c.banner = BannerBlocks(dds);
    if (c.banner.empty()) REXLOG_WARN("[svr2011] arena mods: {} has no 256 x 128 DXT5 banner.dds", c.id);
    if (fs::is_directory(f / "vs", ec))
      for (const auto& v : fs::directory_iterator(f / "vs", ec)) {
        svrfmt::Bytes vdds;
        svrfmt::DdsInfo info;
        if (v.path().extension() != ".dds" || !svrfmt::ReadFile(v.path().string(), vdds) ||
            !svrfmt::DdsInfoOf(vdds, info) || info.format != svrfmt::DxtFormat::kDxt5)
          continue;
        const size_t n = size_t((info.w + 3) / 4) * ((info.h + 3) / 4) * 16;
        if (vdds.size() >= 128 + n) c.vs.push_back({v.path().stem().string(), Swap16(vdds.data() + 128, n)});
      }
    if (svrfmt::Bytes ldds; svrfmt::ReadFile((f / "load.dds").string(), ldds)) {
      svrfmt::DdsInfo info;
      if (svrfmt::DdsInfoOf(ldds, info) && info.w == 1024 && info.h == 512 && info.format == svrfmt::DxtFormat::kDxt5 &&
          ldds.size() >= 128 + 1024 * 512)
        c.load = Swap16(ldds.data() + 128, 1024 * 512);
      else
        REXLOG_WARN("[svr2011] arena mods: {} load.dds is not 1024 x 512 DXT5", c.id);
    }
    g_customs.push_back(std::move(c));
  }
  REXLOG_INFO("[svr2011] arena mods: {} custom arenas", g_customs.size());
  LayOutPages();
}

int Pages() { return 1 + int(g_layout.size()); }

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// Finds each banner in guest physical memory (4 KB view) by a run of blocks.
void LocateBanners() {
  uint8_t* b = g_memory->virtual_membase();
  for (auto& bn : g_banners) bn.guest.clear();
  // one pass: block 300's first 8 bytes -> the banners starting so there
  std::unordered_map<uint64_t, std::vector<Banner*>> keys;
  for (auto& bn : g_banners) {
    uint64_t k;
    std::memcpy(&k, bn.data.data() + 16 * 300, 8);
    keys[k].push_back(&bn);
  }
  int partial = 0;
  constexpr uint64_t hi = 0xFFFF0000;
  ForEachRegion(0xE0000000, hi, [&](uint64_t at, uint64_t end) {
    for (uint64_t a = at + 16 * 300; a + 64 <= end; a += 16) {
      uint64_t k;
      std::memcpy(&k, b + a, 8);
      const auto it = keys.find(k);
      if (it == keys.end()) continue;
      const uint64_t start = a - 16 * 300;
      for (Banner* bn : it->second) {
        // (a texture can run on into the next region: the whole of it mapped)
        rex::memory::HeapAllocationInfo info;
        const uint64_t last = start + bn->data.size() - 1;
        const bool mapped = last < end || (last < hi && Committed(uint32_t(last), info));
        if (mapped && !std::memcmp(b + start, bn->data.data(), bn->data.size()))
          bn->guest.push_back(uint32_t(start));
        else if (!std::memcmp(b + a, bn->data.data() + 16 * 300, 64))
          ++partial;
      }
    }
  });
  int found = 0, copies = 0;
  for (const auto& bn : g_banners) found += !bn.guest.empty(), copies += int(bn.guest.size());
  REXLOG_INFO("[svr2011] arena select: {} of {} banners found ({} places, {} partial matches left alone)", found,
              g_banners.size(), copies, partial);
}

Banner* BannerOf(const char* name) {
  for (auto& bn : g_banners)
    if (bn.name == name) return &bn;
  return nullptr;
}

// The custom arena on tile k of the current page (nullptr: none).
const CustomArena* CustomAt(int k) {
  if (g_page == 0 || g_page - 1 >= int(g_layout.size())) return nullptr;
  const int i = g_layout[g_page - 1][k];
  return i >= 0 ? &g_customs[i] : nullptr;
}

// Draws the current page into the 20 banner textures.
void ShowPage(uint8_t* base) {
  static svrfmt::Bytes empty;
  if (empty.empty()) {  // a dark grey tile for unused places on custom pages
    const uint8_t blk[16] = {0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0xE7, 0x39, 0xE7, 0x39, 0, 0, 0, 0};
    for (size_t k = 0; k < kBannerBlocks; ++k) empty.insert(empty.end(), blk, blk + 16);
    empty = Swap16(empty.data(), empty.size());
  }
  for (int k = 0; k < 20; ++k) {
    Banner* bn = BannerOf(kTiles[k].banner);
    if (!bn || bn->guest.empty()) continue;
    const svrfmt::Bytes* src = &bn->data;
    if (g_page > 0) {
      const CustomArena* c = CustomAt(k);
      src = c && !c->banner.empty() ? &c->banner : &empty;
    }
    for (const uint32_t at : bn->guest) std::memcpy(base + at, src->data(), src->size());
  }
  REXLOG_INFO("[svr2011] arena select: page {} / {}", g_page + 1, Pages());
}

// -- The VS screen theme ------------------------------------------------------
//
// Each arena has a VS screen theme (menu/MatchHD.pac group M<nn>I, DXT5
// textures); a custom arena's vs/<name>.dds replace its base arena's
// pictures of that name (see the loading and VS screens below).
const void* g_redirected_custom = nullptr;

// The theme textures of arena `number`: name -> top level blocks, swapped.
std::vector<std::pair<std::string, svrfmt::Bytes>> VsTheme(int number) {
  std::vector<std::pair<std::string, svrfmt::Bytes>> out;
  svrfmt::Bytes d;
  svrfmt::Epac e;
  if (!svrfmt::ReadFile((g_game / "pac" / "menu" / "MatchHD.pac").string(), d) || !svrfmt::EpacRead(d, e)) return out;
  char group[8];
  std::snprintf(group, sizeof group, "M%02dI", number);
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      if (en.name != group) continue;
      std::vector<svrfmt::PachEntry> ents;
      if (!svrfmt::PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        std::vector<svrfmt::BundleTexture> texs;
        if (!svrfmt::BundleRead(svrfmt::Unpack(pe.data), texs)) continue;
        for (const auto& t : texs) {
          svrfmt::DdsInfo info;
          if (!svrfmt::DdsInfoOf(t.data, info) || info.format != svrfmt::DxtFormat::kDxt5) continue;
          const size_t n = size_t((info.w + 3) / 4) * ((info.h + 3) / 4) * 16;
          if (t.data.size() >= 128 + n) out.push_back({t.name, Swap16(t.data.data() + 128, n)});
        }
      }
    }
  return out;
}

// -- The loading and VS screens ---------------------------------------------
//
// The NOW LOADING screens (menu/LoadHD.pac, LOAD group) are DXT5 pictures,
// either one 1024 x 512 or two 512 x 512 halves (the set's first half on the
// left); which set shows depends on the mode (a Universe show, an
// exhibition, a tip...). The VS screen theme (menu/MatchHD.pac M<nn>I) is
// picked in sub_8271D868: the match's arena number, or 60-66 for some match
// types, and M00I (SmackDown's) when that theme's file is missing.
//
// While a custom arena is chosen: its VS theme is always its base arena's
// (the theme path is rewritten when the game checks it exists, in
// sub_826A17D0), and every picture the game unpacks (sub_826AEF10, the BPE
// decoder) that is one of the loading pictures or of that theme gets the
// arena's own (load.dds, vs/<name>.dds) before the game uses it - no memory
// scan, so it can't be late or missed.
// The loading pictures are swapped only while a match loads (from its set-up
// to its people being placed, match_types.cpp): other screens show the same
// pictures - the WWE SHOP background is LoadHD's Online_tex00/01 - and got the
// arena's loading picture after a match on it. The VS theme's pictures (only
// on the VS screen) are swapped while the arena is chosen.
struct DecodeSwap {
  std::string name;        // the texture's name in its set
  svrfmt::Bytes original;  // top level blocks as in the file
  svrfmt::Bytes mine;
  bool loading = false;    // a loading picture (only while a match loads)
};
std::atomic<bool> g_match_loading{false};
struct DecodeSwaps {
  std::vector<DecodeSwap> swaps;
  std::unordered_map<uint64_t, std::vector<size_t>> keys;  // middle 32 bytes -> swaps
  std::vector<size_t> sizes;                               // the swaps' top level sizes (distinct)
  int theme = -1;                                          // VS theme forced (-1 none)
};
std::mutex g_decode_mutex;
std::shared_ptr<const DecodeSwaps> g_decode;

uint64_t Fnv(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

struct LoadPic {
  std::string name;
  svrfmt::Bytes original;  // top level blocks as in the file
  int part = 0;            // 0 whole, 1 left half, 2 right half
};
std::vector<LoadPic> g_load_pics;  // every loading picture of the game (distinct)

void LoadLoadingPictures() {
  svrfmt::Bytes d;
  svrfmt::Epac e;
  if (!svrfmt::ReadFile((g_game / "pac" / "menu" / "LoadHD.pac").string(), d) || !svrfmt::EpacRead(d, e)) return;
  std::unordered_map<uint64_t, int> seen;
  for (const auto& g : e.groups)
    for (const auto& en : g.entries) {
      std::vector<svrfmt::PachEntry> ents;
      if (!svrfmt::PachRead(en.data, ents)) continue;
      for (const auto& pe : ents) {
        std::vector<svrfmt::BundleTexture> texs;
        if (!svrfmt::BundleRead(svrfmt::Unpack(pe.data), texs)) continue;
        std::vector<const svrfmt::BundleTexture*> halves;
        for (const auto& t : texs) {
          svrfmt::DdsInfo info;
          if (!svrfmt::DdsInfoOf(t.data, info) || info.format != svrfmt::DxtFormat::kDxt5) continue;
          if (info.w == 1024 && info.h == 512 && t.data.size() >= 128 + 1024 * 512) {
            LoadPic lp{t.name, svrfmt::Bytes(t.data.begin() + 128, t.data.begin() + 128 + 1024 * 512), 0};
            if (!seen[Fnv(lp.original.data(), lp.original.size()) ^ Fnv(reinterpret_cast<const uint8_t*>(lp.name.data()), lp.name.size())]++)
              g_load_pics.push_back(std::move(lp));
          } else if (info.w == 512 && info.h == 512 && t.data.size() >= 128 + 512 * 512) {
            halves.push_back(&t);
          }
        }
        std::sort(halves.begin(), halves.end(), [](auto* a, auto* b) { return a->name < b->name; });
        for (size_t k = 0; k < halves.size() && k < 2; ++k) {
          LoadPic lp{halves[k]->name, svrfmt::Bytes(halves[k]->data.begin() + 128, halves[k]->data.begin() + 128 + 512 * 512),
                     int(k) + 1};
          if (!seen[Fnv(lp.original.data(), lp.original.size()) ^ Fnv(reinterpret_cast<const uint8_t*>(lp.name.data()), lp.name.size())]++)
            g_load_pics.push_back(std::move(lp));
        }
      }
    }
  REXLOG_INFO("[svr2011] arena mods: {} loading screen pictures", g_load_pics.size());
}

// The custom arena's picture cut to fit (whole, or a half: 1024 x 512 DXT5
// rows are 256 blocks of 16 bytes).
svrfmt::Bytes LoadPart(const svrfmt::Bytes& load, int part) {
  if (part == 0) return load;
  svrfmt::Bytes o;
  o.reserve(512 * 512);
  for (size_t row = 0; row < 128; ++row) {
    const auto from = load.begin() + row * 4096 + (part == 2 ? 2048 : 0);
    o.insert(o.end(), from, from + 2048);
  }
  return o;
}

void SetDecodeSwaps(const CustomArena* c, int host) {
  auto t = std::make_shared<DecodeSwaps>();
  if (c) {
    t->theme = host;
    auto add = [&](const std::string& name, svrfmt::Bytes original, svrfmt::Bytes mine, bool loading) {
      if (original.size() != mine.size() || original.size() < 64) return;
      t->keys[Fnv(original.data() + original.size() / 2, 32)].push_back(t->swaps.size());
      if (std::find(t->sizes.begin(), t->sizes.end(), original.size()) == t->sizes.end()) t->sizes.push_back(original.size());
      t->swaps.push_back({name, std::move(original), std::move(mine), loading});
    };
    if (!c->load.empty()) {
      const svrfmt::Bytes load = Swap16(c->load.data(), c->load.size());  // (kept swapped: back to file order)
      for (const auto& lp : g_load_pics) add(lp.name, lp.original, LoadPart(load, lp.part), true);
    }
    if (!c->vs.empty()) {
      const auto theme = VsTheme(host);
      for (const auto& [name, mine] : c->vs)
        for (const auto& [tname, orig] : theme)
          if (tname == name) add(tname, Swap16(orig.data(), orig.size()), Swap16(mine.data(), mine.size()), false);
    }
  }
  REXLOG_INFO("[svr2011] arena mods: loading / VS pictures {} for {} (theme {:02})", t->swaps.size(),
              c ? c->id : std::string("-"), host);
  std::lock_guard lock(g_decode_mutex);
  g_decode = c ? std::move(t) : nullptr;
}

std::shared_ptr<const DecodeSwaps> CurrentDecodeSwaps() {
  std::lock_guard lock(g_decode_mutex);
  return g_decode;
}

// Texture names compare without case (the set holds up to 16 characters).
bool NameIs(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
  return true;
}

// A texture set the game just unpacked (u32 count, 16-byte header, 32-byte
// records: name[16], "dds", size, offset): its pictures that have a swap.
void SwapDecoded(uint8_t* d, uint32_t size) {
  const auto t = CurrentDecodeSwaps();
  if (!t || t->swaps.empty() || size < 48) return;
  const uint32_t n = d[0] | d[1] << 8 | d[2] << 16 | uint32_t(d[3]) << 24;
  if (!n || n > 512 || 16 + 32ull * n > size) return;
  for (uint32_t i = 0; i < n; ++i) {
    const uint8_t* r = d + 16 + 32 * i;
    if (std::memcmp(r + 16, "dds", 3)) return;  // not a texture set
    const uint32_t sz = r[20] | r[21] << 8 | r[22] << 16 | uint32_t(r[23]) << 24;
    const uint32_t off = r[24] | r[25] << 8 | r[26] << 16 | uint32_t(r[27]) << 24;
    if (uint64_t(off) + sz > size || sz < 128 + 64 || std::memcmp(d + off, "DDS ", 4)) continue;
    uint8_t* top = d + off + 128;
    const std::string name(reinterpret_cast<const char*>(r), strnlen(reinterpret_cast<const char*>(r), 16));
    for (size_t level0 : t->sizes) {
      if (128 + level0 > sz) continue;
      auto it = t->keys.find(Fnv(top + level0 / 2, 32));
      if (it == t->keys.end()) continue;
      for (size_t k : it->second) {
        const auto& sw = t->swaps[k];
        if (sw.original.size() != level0 || (sw.loading && !g_match_loading) || !NameIs(name, sw.name) ||
            std::memcmp(top, sw.original.data(), level0))
          continue;
        std::memcpy(top, sw.mine.data(), level0);
        static int logged = 0;
        if (logged++ < 40) REXLOG_INFO("[svr2011] arena mods: picture {} swapped as it was unpacked", name);
        break;
      }
    }
  }
}

// "/MENU/M<nn>I" or "/MENU/M<nn>L" (the VS theme): the custom arena's base theme.
void ForceTheme(char* path) {
  const auto t = CurrentDecodeSwaps();
  if (!t || t->theme < 0 || t->theme > 99) return;
  if (std::strncmp(path, "/MENU/M", 7) || !std::isdigit(uint8_t(path[7])) || !std::isdigit(uint8_t(path[8])) ||
      (path[9] != 'I' && path[9] != 'L') || path[10])
    return;
  const char a = char('0' + t->theme / 10), b = char('0' + t->theme % 10);
  if (path[7] == a && path[8] == b) return;
  REXLOG_INFO("[svr2011] arena mods: VS theme {} -> {}{}", path, a, b);
  path[7] = a;
  path[8] = b;
}

// The host arena of the tile under the cursor plays the custom arena there.
void FollowCursor(int row, int col) {
  const CustomArena* c = row < 4 ? CustomAt(row * 5 + col) : nullptr;
  g_cursor_custom = c ? int(c - g_customs.data()) : -1;
  // the tile's arena: the one the game loads (LayOutPages puts each custom
  // arena on the tile of the arena it was made from)
  const int host = c ? kTiles[row * 5 + col].arena : -1;
  if (c == g_redirected_custom && host == g_redirected_host) return;
  g_redirected_custom = c;
  SetDecodeSwaps(c, host);
  if (g_redirected_host >= 0) svr2011::RedirectArena(g_redirected_host, "");
  g_redirected_host = -1;
  svr2011::SetRingRules(c ? c->manifest : std::string());
  if (c) {
    svr2011::RedirectArena(host, c->file);
    g_redirected_host = host;
    REXLOG_INFO("[svr2011] arena select: {} on {}", c->name, kTiles[row * 5 + col].banner);
  }
}

}  // namespace

REX_EXTERN(__imp__sub_826AEF10);
REX_HOOK_RAW(sub_826AEF10) {
  if (g_bpe_log) {
    const uint8_t* s = base + ctx.r3.u32;
    const uint32_t size = s[12] | s[13] << 8 | s[14] << 16 | uint32_t(s[15]) << 24;
    REXLOG_INFO("[svr2011] bpe: lr {:08X} src {:08X} dst {:08X} size {:X} end {:08X}", uint32_t(ctx.lr),
                ctx.r3.u32, ctx.r4.u32, size, ctx.r4.u32 + size);
  }
  const uint32_t src = ctx.r3.u32, dst = ctx.r4.u32;
  const uint8_t* s = base + src;
  const uint32_t size = s[12] | s[13] << 8 | s[14] << 16 | uint32_t(s[15]) << 24;
  __imp__sub_826AEF10(ctx, base);
  if (dst && size) SwapDecoded(base + dst, size);
}

// Does a file exist (r3 path): a custom arena's VS theme instead of the one
// the game would pick (see SetDecodeSwaps).
REX_EXTERN(__imp__sub_826A17D0);
REX_HOOK_RAW(sub_826A17D0) {
  if (ctx.r3.u32) ForceTheme(reinterpret_cast<char*>(base + ctx.r3.u32));
  __imp__sub_826A17D0(ctx, base);
}

// The grid widget update: arena select pages.
REX_EXTERN(__imp__sub_823D4FC0);
REX_HOOK_RAW(sub_823D4FC0) {
  const uint32_t w = ctx.r3.u32;
  uint8_t* p = base + w;
  auto rd = [&](uint32_t o) { return uint32_t(p[o]) << 24 | p[o + 1] << 16 | p[o + 2] << 8 | p[o + 3]; };
  if (g_customs.empty() || rd(0) != kSelectVtable) {
    __imp__sub_823D4FC0(ctx, base);
    return;
  }
  if (w != g_select_widget) {  // the screen opened: page 1, find the banners
    g_select_widget = w;
    g_page = 0;
    if (g_banners.empty()) LoadBanners();
    LocateBanners();
    FollowCursor(4, 0);  // (nothing redirected)
  }
  g_select_seen = NowMs();
  const uint32_t row = rd(28), col = rd(44), cols = rd(48);
  __imp__sub_823D4FC0(ctx, base);
  const uint32_t new_col = rd(44), new_row = rd(28);
  // Past an edge: the cursor jumped from the last column to the first or back
  // in its row (the game wraps it), whatever moved it - a check of the button
  // field's direction bits missed a 2.0.3 player's presses. Any player's
  // LB / RB page too (not in that field: read with the game's pad reads).
  static uint32_t held = 0;
  uint32_t shoulders = 0;
  for (const auto& s : g_shoulders) shoulders |= s.load();
  const uint32_t pressed = shoulders & ~held;
  held = shoulders;
  int page = g_page;
  const bool wrapped = row < 4 && new_row == row && cols > 2;
  if ((wrapped && col + 1 == cols && new_col == 0) || (pressed & 0x200))
    page = (g_page + 1) % Pages();
  else if ((wrapped && col == 0 && new_col + 1 == cols) || (pressed & 0x100))
    page = (g_page + Pages() - 1) % Pages();
  if (page != g_page) {
    g_page = page;
    ShowPage(base);
  }
  FollowCursor(int(new_row), int(new_col));
}

namespace svr2011 {

// Backstage areas (Mods/Backstage/<id>: manifest type=backstage, area=<0-6>,
// arena.pac = bg78 with that room rebuilt; Mod Maker): bg78 holds all seven
// backstage brawl rooms, so the mod's file is bg78 from then on. One at a
// time: a later one (by folder name) wins.
// A mod with row=<label> in its manifest is an area of its own instead: it
// gets a menu row (BACKSTAGE, after its room's row; match_types.cpp) and its
// bg78 plays only in matches started from that row (UseBackstageRow).
std::vector<BackstageRow> g_backstage_rows;

void LoadBackstage() {
  std::error_code ec;
  std::vector<fs::path> found;
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(g_game / "Mods" / "Backstage", ec))
    if (e.is_directory() && fs::exists(e.path() / "arena.pac", ec) && !fs::exists(e.path() / "disabled", ec))
      dirs.push_back(e.path());
  std::sort(dirs.begin(), dirs.end());
  for (const auto& d : dirs) {
    BackstageRow r;
    r.file = fs::relative(d / "arena.pac", g_game, ec).generic_string();
    if (FILE* t = std::fopen((d / "manifest.txt").string().c_str(), "rb")) {
      char line[512];
      while (std::fgets(line, sizeof line, t)) {
        std::string l = line;
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        if (l.rfind("row=", 0) == 0) r.label = l.substr(4);
        if (l.rfind("area=", 0) == 0) r.area = std::atoi(l.c_str() + 5);
        if (l.rfind("gimmick=", 0) == 0) r.gimmick = l.substr(8);
        if (l.rfind("camera=", 0) == 0) r.camera = float(std::atof(l.c_str() + 7));
        if (l.rfind("camera_height=", 0) == 0) r.camera_height = float(std::atof(l.c_str() + 14));
        if (l.rfind("box=", 0) == 0)
          r.has_box = std::sscanf(l.c_str() + 4, "%f,%f,%f,%f", &r.box[0], &r.box[1], &r.box[2], &r.box[3]) == 4;
      }
      std::fclose(t);
    }
    if (r.label.empty()) {
      found.push_back(d);
    } else if (r.area >= 0 && r.area < 7 && g_backstage_rows.size() < 8) {
      REXLOG_INFO("[svr2011] arena mods: backstage area '{}' ({}; its own row after room {})", r.label, r.file, r.area);
      // Its own gimmicks (cars and their hot spots): gimmick.pac, a small pac
      // with GMGB/<gimmick> (4 chars, e.g. 78P8), mounted as one more pac
      // (move_packs.cpp) and played in place of the room's GMGB package in its
      // matches (sub_824B0090 below). Only the first such mod brings one.
      static bool have_gm = false;
      if (r.gimmick.size() == 4 && fs::exists(d / "gimmick.pac", ec) && !have_gm) {
        have_gm = true;
        REXLOG_INFO("[svr2011] arena mods: {} plays its gimmicks GMGB/{}", r.label, r.gimmick);
      } else {
        if (!r.gimmick.empty()) REXLOG_WARN("[svr2011] arena mods: {}: gimmicks ignored", r.label);
        r.gimmick.clear();
      }
      g_backstage_rows.push_back(std::move(r));
    }
  }
  if (found.empty()) return;
  std::sort(found.begin(), found.end());
  if (found.size() > 1)
    REXLOG_WARN("[svr2011] arena mods: {} backstage areas installed, only {} plays (one at a time)", found.size(),
                found.back().filename().string());
  const fs::path rel = fs::relative(found.back() / "arena.pac", g_game, ec);
  svr2011::SetArenaDefault(78, rel.generic_string());
}

const std::vector<BackstageRow>& BackstageRows() { return g_backstage_rows; }

int g_active_row = -1;

void UseBackstageRow(int i) {
  if (i >= int(g_backstage_rows.size())) i = -1;
  if (i == g_active_row) return;
  g_active_row = i;
  RedirectArena(78, i < 0 ? "" : g_backstage_rows[size_t(i)].file);
}

void SetMatchLoading(bool loading) { g_match_loading = loading; }

void ArenaSelectPad(uint32_t user, uint16_t buttons) {
  if (user < 8) g_shoulders[user] = buttons & 0x300;
}

void SetArenaDefault(int arena, const std::string& relative_file) {
  if (arena < 0 || arena >= 100) return;
  g_default[arena] = relative_file;
  RedirectArena(arena, relative_file);
}

void RedirectArena(int arena, const std::string& file) {
  if (!g_fs || arena < 0 || arena >= 100) return;
  const std::string& relative_file = file.empty() ? g_default[arena] : file;
  std::lock_guard lock(g_mutex);
  if (!PrepareOverlay()) return;
  char name[16];
  std::snprintf(name, sizeof name, "BG%02d.PAC", arena);
  const fs::path source = relative_file.empty() ? g_game / "pac" / "bg" / name : g_game / fs::u8path(relative_file);
  std::error_code ec;
  if (!fs::exists(source, ec)) {
    REXLOG_WARN("[svr2011] arena mods: {} not found", source.string());
    return;
  }
  if (!Place(source, name)) return;
  g_redirect[arena] = relative_file;
  REXLOG_INFO("[svr2011] arena mods: BG{:02} -> {}", arena, relative_file.empty() ? "original" : relative_file);
}

// Arena memory: the game splits one block (physical 0x03A00000-0x0F000000)
// into 54 heaps from a table of (heap, KB) pairs at 0x82DAC2F8, read once at
// start-up by sub_8258ED50 -> sub_8269AD08. Heap 16 (20 MB, its size at
// 0x82DAC37C) holds the loaded arena file beside other stage data: an arena
// file much bigger than shipped doesn't fit (the load retries forever). Heap
// 53 is the rest of the block (25 MB). Experiment (SVR2011_ARENA_HEAP_EXTRA_MB):
// moving room from 53 to 16 did not let a bigger arena load - heap 16 is
// shared with menus and other stage data, which then failed instead - so
// the default is off and custom arenas keep their slot's size.
constexpr uint32_t kHeapTable = 0x82DAC2F8, kHeap16Size = 0x82DAC37C;
uint32_t g_arena_heap_extra_kb = 0;

void GrowArenaHeap(rex::memory::Memory* memory) {
  if (const char* v = std::getenv("SVR2011_ARENA_HEAP_EXTRA_MB")) g_arena_heap_extra_kb = uint32_t(std::atoi(v)) * 1024;
  uint8_t* b = memory->virtual_membase();
  auto rd = [&](uint32_t a) { return uint32_t(b[a]) << 24 | b[a + 1] << 16 | b[a + 2] << 8 | b[a + 3]; };
  if (rd(kHeap16Size - 4) != 16 || rd(kHeap16Size) != 20480) {
    REXLOG_WARN("[svr2011] arena mods: heap table not as expected, arena heap left as is");
    return;
  }
  if (!g_arena_heap_extra_kb) return;
  if (auto* heap = memory->LookupHeap(kHeapTable))
    heap->Protect(kHeapTable & ~0xFFFu, 0x2000, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite);
  const uint32_t kb = 20480 + g_arena_heap_extra_kb;
  b[kHeap16Size] = uint8_t(kb >> 24), b[kHeap16Size + 1] = uint8_t(kb >> 16), b[kHeap16Size + 2] = uint8_t(kb >> 8),
  b[kHeap16Size + 3] = uint8_t(kb);
  REXLOG_INFO("[svr2011] arena mods: arena heap {} MB (+{} MB)", kb / 1024, g_arena_heap_extra_kb / 1024);
}

void InstallArenaMods(rex::memory::Memory* memory, rex::filesystem::VirtualFileSystem* fs) {
  g_memory = memory;
  GrowArenaHeap(memory);
  g_fs = fs;
  g_game = rex::filesystem::GetExecutableFolder();
  g_overlay = g_game / "Mods" / "ArenaOverlay";
  g_bpe_log = std::getenv("SVR2011_TEST_BPE_LOG") != nullptr;
  if (crowd::Off()) {
    // (the arenas come through the overlay from the start: their crowd-less copies)
    std::lock_guard lock(g_mutex);
    PrepareOverlay();
    REXLOG_INFO("[svr2011] crowd: off - arenas without their crowd");
  }
  LoadCustomArenas();
  LoadBackstage();
  LoadLoadingPictures();
  // Test aid: SVR2011_TEST_LOADING=<custom arena id>:<theme>: its loading and
  // VS screens from the start (as when it is chosen on its base arena's tile)
  if (const char* v = std::getenv("SVR2011_TEST_LOADING"); v && *v) {
    const std::string sv = v;
    const size_t colon = sv.find(':');
    for (const auto& c : g_customs)
      if (c.id == sv.substr(0, colon)) SetDecodeSwaps(&c, colon == std::string::npos ? 1 : std::atoi(sv.c_str() + colon + 1));
  }
  if (std::getenv("SVR2011_TEST_SCAN")) StartScan();
  // Test aid: SVR2011_TEST_ARENA_REDIRECT=<nn>=<file relative to the game folder>
  if (const char* v = std::getenv("SVR2011_TEST_ARENA_REDIRECT"); v && *v) {
    const std::string s = v;
    const size_t eq = s.find('=');
    if (eq != std::string::npos) RedirectArena(std::atoi(s.substr(0, eq).c_str()), s.substr(eq + 1));
  }
  // Test aid: SVR2011_TEST_FILE_LINK=<path>=<path>;... (game folder relative,
  // e.g. PAC\M.PAC=Mods\PacOverlay\m.pac): file-level symbolic links.
  if (const char* v = std::getenv("SVR2011_TEST_FILE_LINK"); v && *v) {
    std::string s = v;
    for (size_t a = 0; a < s.size();) {
      size_t e = s.find(';', a);
      if (e == std::string::npos) e = s.size();
      const std::string item = s.substr(a, e - a);
      if (const size_t eq = item.find('='); eq != std::string::npos) {
        const std::string from = std::string(kGameDevice) + "\\" + item.substr(0, eq);
        const std::string to = std::string(kGameDevice) + "\\" + item.substr(eq + 1);
        fs->RegisterSymbolicLink(from, to);
        REXLOG_INFO("[svr2011] file link: {} -> {}", from, to);
      }
      a = e + 1;
    }
  }
}

}  // namespace svr2011

// -- The page label ---------------------------------------------------------

namespace {

// "<  2 / 3  >" above the arena grid (as the Paint Tool's), and on custom
// pages the name of the arena under the cursor beside it.
class ArenaPageLabel final : public rex::ui::ImGuiDialog {
 public:
  explicit ArenaPageLabel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (g_customs.empty() || NowMs() - g_select_seen.load() > 250) return;
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    const float gw = std::min(w, h * 16.0f / 9.0f), gh = gw * 9.0f / 16.0f;
    const float x0 = (w - gw) * 0.5f, y0 = (h - gh) * 0.5f;
    const float scale = gh / 720.0f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImFont* font = ImGui::GetFont();
    const ImU32 white = IM_COL32(255, 255, 255, 255);
    char text[16];
    std::snprintf(text, sizeof(text), "%d / %d", g_page + 1, Pages());
    const float size = 28.0f * scale;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const ImVec2 c(x0 + gw * 0.5f, y0 + gh * 0.075f);
    const float arrow = 11.0f * scale, gap = 26.0f * scale;
    const float half = ts.x * 0.5f + gap + arrow;
    const ImVec2 pad(16.0f * scale, 6.0f * scale);
    const ImVec2 a(c.x - half - pad.x, c.y - ts.y * 0.5f - pad.y);
    const ImVec2 b(c.x + half + pad.x, c.y + ts.y * 0.5f + pad.y);
    dl->AddRectFilled(a, b, IM_COL32(10, 12, 18, 210), 6.0f * scale);
    dl->AddRect(a, b, IM_COL32(220, 220, 225, 255), 6.0f * scale, 0, 2.0f * scale);
    dl->AddText(font, size, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), white, text);
    const float lx = c.x - half, rx = c.x + half;
    dl->AddTriangleFilled(ImVec2(lx, c.y), ImVec2(lx + arrow, c.y - arrow), ImVec2(lx + arrow, c.y + arrow), white);
    dl->AddTriangleFilled(ImVec2(rx, c.y), ImVec2(rx - arrow, c.y + arrow), ImVec2(rx - arrow, c.y - arrow), white);
    // the custom arena under the cursor
    const int ci = g_cursor_custom.load();
    if (ci < 0 || ci >= int(g_customs.size())) return;
    const std::string& name = g_customs[ci].name;
    const float ns = 22.0f * scale;
    const ImVec2 nt = font->CalcTextSizeA(ns, FLT_MAX, 0.0f, name.c_str());
    // beside the page label (below the grid is the DOWNLOADED ARENAS bar)
    const ImVec2 nc(b.x + 14.0f * scale + pad.x + nt.x * 0.5f, c.y);
    const ImVec2 na(nc.x - nt.x * 0.5f - pad.x, nc.y - nt.y * 0.5f - pad.y);
    const ImVec2 nb(nc.x + nt.x * 0.5f + pad.x, nc.y + nt.y * 0.5f + pad.y);
    dl->AddRectFilled(na, nb, IM_COL32(10, 12, 18, 220), 6.0f * scale);
    dl->AddText(font, ns, ImVec2(nc.x - nt.x * 0.5f, nc.y - nt.y * 0.5f), IM_COL32(255, 210, 60, 255), name.c_str());
  }
};

}  // namespace

namespace svr2011 {

void InstallArenaModsOverlay(rex::ui::ImGuiDrawer* drawer) { new ArenaPageLabel(drawer); }

}  // namespace svr2011

// -- The pac list from another folder (move packs) ---------------------------
//
// With a pac overlay (move_packs.h: PacListFolder) the game reads its pac list
// (D:\pac\plist360.h - sub_826B9280(obj, path)) from that folder instead: a
// list there names other files for some pacs ("mods\pacoverlay\m.pac").
// (The pre-built directory GAME:\PLIST360.ARC is not used then: the pacs are
// mounted from their own tables, superstar_mods.cpp.)
namespace {
uint32_t TestPlistPath(uint32_t guest_path) {
  const std::string& f = svr2011::PacListFolder();
  const char* folder = f.c_str();
  if (f.empty() || !g_memory || !guest_path) return 0;
  const char* p = reinterpret_cast<const char*>(g_memory->TranslateVirtual(guest_path));
  std::string s(p, strnlen(p, 260));
  const size_t slash = s.rfind('\\');
  const std::string name = Upper(slash == std::string::npos ? s : s.substr(slash + 1));
  if (name != "PLIST360.H" && name != "PLIST360_4X3.H" && name != "PLIST360.ARC" && name != "PLIST360_4X3.ARC")
    return 0;
  const std::string to = (s.rfind("GAME:", 0) == 0 ? std::string("GAME:\\") : std::string("D:\\")) + folder + "\\" +
                         (slash == std::string::npos ? s : s.substr(slash + 1));
  static std::vector<std::pair<std::string, uint32_t>> made;
  for (const auto& m : made)
    if (m.first == to) return m.second;
  const uint32_t g = g_memory->SystemHeapAlloc(uint32_t(to.size() + 1));
  if (!g) return 0;
  std::memcpy(g_memory->TranslateVirtual(g), to.c_str(), to.size() + 1);
  made.push_back({to, g});
  REXLOG_INFO("[svr2011] pac list: {} -> {}", s, to);
  return g;
}
}  // namespace
REX_EXTERN(__imp__sub_826B9280);
REX_HOOK_RAW(sub_826B9280) {
  if (const uint32_t g = TestPlistPath(ctx.r4.u32)) ctx.r4.u64 = g;
  __imp__sub_826B9280(ctx, base);
}
REX_EXTERN(__imp__sub_826A7108);
REX_HOOK_RAW(sub_826A7108) {
  if (const uint32_t g = TestPlistPath(ctx.r4.u32)) ctx.r4.u64 = g;
  __imp__sub_826A7108(ctx, base);
}

// A stage's gimmick package (cars, crates and their hot spots): sub_824B0090
// (kind, arena, out path) writes its name, "/GMGB/78PK" for the parking lot.
// In a match of a backstage row with its own gimmicks, the room's package
// name points at the mod's entry instead (same length; the group must stay
// GMGB - the loader unwraps the two-part package only for that group).
REX_EXTERN(__imp__sub_824B0090);
REX_HOOK_RAW(sub_824B0090) {
  const uint32_t out = ctx.r5.u32;
  __imp__sub_824B0090(ctx, base);
  const int k = svr2011::g_active_row;
  const auto& rows = svr2011::BackstageRows();
  if (k < 0 || size_t(k) >= rows.size() || rows[size_t(k)].gimmick.size() != 4 || !out) return;
  char* path = reinterpret_cast<char*>(base + out);
  if (std::strncmp(path, "/GMGB/78", 8) || std::strlen(path) != 10) return;
  const std::string before(path);
  std::memcpy(path + 6, rows[size_t(k)].gimmick.data(), 4);
  REXLOG_INFO("[svr2011] arena mods: gimmicks {} -> {}", before, path);
}

// Gimmick element type 25 (a prop the package places; init sub_824BB4E0,
// factory sub_824B8AB8): idx = int(param 1) - 11500 picks a prop in the
// table at [0x82E3BEC0] + (idx + 14652) * 4, written through without a
// check. A package from another game (SvR 2008's parking lot) can name a
// prop 2011 doesn't have: an empty slot, a write at 0x24 - the element is
// skipped instead.
REX_EXTERN(__imp__sub_824BB4E0);
REX_HOOK_RAW(sub_824BB4E0) {
  const auto rd = [&](uint32_t a) {
    return uint32_t(base[a]) << 24 | uint32_t(base[a + 1]) << 16 | uint32_t(base[a + 2]) << 8 | base[a + 3];
  };
  const uint32_t p = ctx.r3.u32 ? rd(ctx.r3.u32 + 84) : 0;
  const uint32_t tbl = rd(0x82E3BEC0);
  if (p && tbl) {
    const uint32_t bits = rd(p + 4);
    float f;
    std::memcpy(&f, &bits, 4);
    const int idx = int(f) - 11500;
    if (idx < 0 || idx > 60 || !rd(tbl + uint32_t(idx + 14652) * 4)) {
      static int warned = 0;
      if (warned++ < 8) REXLOG_WARN("[svr2011] arena mods: gimmick prop {} not in the game - skipped", int(f));
      return;
    }
  }
  __imp__sub_824BB4E0(ctx, base);
}
