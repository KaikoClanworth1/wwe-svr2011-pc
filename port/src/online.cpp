// WWE SmackDown vs. Raw 2011 - online services (see online.h).

#include "online.h"

#include "generated/default/svr2011_init.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>


#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "caw_logos.h"
#include "entrance_media.h"
#include "online_net.h"
#include "paint_pages.h"

namespace svr2011 {

namespace {

// The AuthService URL (the game's own SSL engine; the server speaks http).
constexpr uint32_t kAuthUrl = 0x82087480;
constexpr char kAuthUrlHttps[] = "https://%s.auth.pubsvs.gamespy.com/AuthService/AuthService.asmx";
constexpr char kAuthUrlHttp[] = "http://%s.auth.pubsvs.gamespy.com/AuthService/AuthService.asmx";

// The RSA public key (hex modulus; exponent 010001) login certificates are
// checked against: GameSpy's, replaced by the one gamespy_server.py signs with.
constexpr uint32_t kCertKeyModulus = 0x82087370;
constexpr char kGameSpyModulus[] =
    "BF05D63E93751AD4A59A4A7389CF0BE8A22CCDEEA1E7F12C062D6E194472EFDA5184CCECEB4FBADF5EB1D7ABFE911814"
    "53972AA971F624AF9BA8F0F82E2869FB7D44BDE8D56EE50977898F3FEE75869622C4981F07506248BD3D092E8EA05C12"
    "B2FA37881176084C8F8B8756C4722CDC57D2AD28ACD3AD85934FB48D6B2D2027";
constexpr char kServerModulus[] =
    "EFD6AB4EF900EFFDD0634EE8B5C5E7355B06831E51DF9380E5D73B1516E63D7BE3F6A3323AC4D402CAEAE7250885D7DC"
    "C755F16B77DC7C1B2D14ED9D8CC926DE48F3675C63499910398B12D27B04FF858CB1F928F5CE0A6F2C4841AD3EA0DA13"
    "AEAA68893865509FEDB7F559F52A94DC0B26D145918CF8FEA1F84DF47506EA63";
static_assert(sizeof(kGameSpyModulus) == sizeof(kServerModulus));

// Replaces the string at address, if it is the expected one.
bool Patch(rex::memory::Memory* memory, uint32_t address, const char* expected, const char* text,
           size_t size) {
  char* p = memory->TranslateVirtual<char*>(address);
  if (std::memcmp(p, expected, std::strlen(expected) + 1) != 0) {
    REXLOG_WARN("online: unexpected data at {:08X} (not patched)", address);
    return false;
  }
  // (the image is read-only: writable for the copy)
  auto* heap = memory->LookupHeap(address);
  uint32_t old = 0;
  if (!heap || !heap->QueryProtect(address, &old) ||
      !heap->Protect(address, uint32_t(size), rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) {
    REXLOG_WARN("online: {:08X} can't be made writable (not patched)", address);
    return false;
  }
  std::memcpy(p, text, size);
  heap->Protect(address, uint32_t(size), old);
  return true;
}

}  // namespace

void PrepareOnlineSettings(const std::filesystem::path& config, const std::filesystem::path& exe_dir) {
  std::vector<std::string> lines;
  {
    std::ifstream in(config);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      lines.push_back(line);
    }
  }
  auto key_of = [](const std::string& line) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos || line.empty() || line[0] == '#' || line[0] == '[') return std::string();
    std::string key = line.substr(0, eq);
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
    return key;
  };
  // (top-level keys go before the first [section])
  auto set = [&](const std::string& key, const std::string& line) {
    size_t end = lines.size();
    for (size_t i = 0; i < lines.size(); ++i) {
      if (!lines[i].empty() && lines[i][0] == '[') {
        end = i;
        break;
      }
      if (key_of(lines[i]) == key) {
        lines[i] = line;
        return;
      }
    }
    while (end > 0 && lines[end - 1].empty()) --end;  // (before the blank lines)
    lines.insert(lines.begin() + end, line);
  };
  bool changed = false;
  const std::filesystem::path brought = exe_dir / "online.toml";
  if (std::filesystem::exists(brought)) {
    std::ifstream in(brought);
    for (std::string line; std::getline(in, line);) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      const std::string key = key_of(line);
      if (key == "online_enabled" || key == "online_name" || key == "online_server" || key == "online_token" ||
          key == "online_xuid") {  // (the PC's account comes along)
        set(key, line);
        changed = true;
      }
    }
    in.close();
    std::error_code ec;
    std::filesystem::remove(brought, ec);
    REXLOG_INFO("online: settings from {}", brought.string());
  }
  bool has_xuid = false;
  for (const auto& line : lines) has_xuid |= key_of(line) == "online_xuid";
  if (!has_xuid) {
    std::random_device rd;
    const uint64_t id = ((uint64_t(rd()) << 32) | rd()) & 0x0000FFFFFFFFFFFFull;
    char text[64];
    std::snprintf(text, sizeof(text), "online_xuid = \"%016llX\"", 0x0009000000000000ull | id);
    set("online_xuid", text);
    changed = true;
  }
  if (changed) {
    std::ofstream out(config, std::ios::trunc);
    for (const auto& line : lines) out << line << '\n';
  }
}

namespace {
uint32_t g_empty_string = 0;  // guest "" (removed labels)

// -- The Superstars' extra logos (Saves\.logos) and the server ----------------

std::filesystem::path g_saves;  // the saves folder (Saves\.logos: caw_logos.cpp)

uint64_t Fnv64(const std::string& data) {
  uint64_t h = 14695981039346656037ull;
  for (unsigned char c : data) h = (h ^ c) * 1099511628211ull;
  return h;
}

constexpr size_t kLogoSize = 1024 + 65536;  // a stored logo: palette + pixels (caw_logos.cpp)

// The server asks for the logos (Saves\.logos) of Superstars that were
// uploaded but whose logos it doesn't have: the ones this player has go up.
// (The game's own upload carries only the Superstar file.)
void LogoUploads() {
  for (;;) {
    auto wanted = net::ServerRequest("GET", "/api/logos/wanted");
    if (wanted && wanted->status == 200) {
      std::istringstream lines(wanted->body);
      for (std::string hash; std::getline(lines, hash);) {
        if (hash.size() != 16) continue;
        std::ifstream in(g_saves / ".logos" / (hash + ".bin"), std::ios::binary);
        if (!in) continue;
        const std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (data.size() != kLogoSize) continue;
        auto r = net::ServerRequest("POST", "/api/logo/" + hash, data, {{"Content-Type", "application/octet-stream"}});
        REXLOG_INFO("online: logo {} sent to the server ({})", hash,
                    r && r->headers["svr2011-result"] == "0" ? "stored" : "refused");
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(30));
  }
}

// A logo a Superstar shows but this PC doesn't have (one downloaded from
// Community Creations): from the server, checked against its hash, into the
// store.
bool FetchLogo(uint64_t hash, const std::filesystem::path& file) {
  char name[17];
  std::snprintf(name, sizeof(name), "%016llX", static_cast<unsigned long long>(hash));
  auto r = net::ServerRequest("GET", std::string("/api/logo/") + name);
  if (!r || r->status != 200) return false;
  const std::string* data = &r->body;
  if (data->size() != kLogoSize || Fnv64(*data) != hash) return false;
  std::error_code ec;
  std::filesystem::create_directories(file.parent_path(), ec);
  const auto tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(data->data(), std::streamsize(data->size()));
    if (!out) return false;
  }
  std::filesystem::rename(tmp, file, ec);
  return !ec;
}

void OnMissingLogo(uint64_t hash, const std::filesystem::path& file) {
  std::thread([hash, file] {
    const bool ok = FetchLogo(hash, file);
    REXLOG_INFO("online: logo {:016X} {}", hash, ok ? "downloaded" : "not on the server");
  }).detach();
}

// -- Community Creations downloads: checked, and what they replace backed up --
//
// The game's download (all content types) runs on one manager's state
// machine: [mgr+1176] state, [mgr+1172] content type (0 Created Superstar,
// 1 story, 3 Paint Tool logo, 4 highlight reel, 2/5 kept in SaveData.dat),
// [mgr+1424] the file size the server gave, the bytes in the 12 MB buffer at
// **0x82E3DD9C. State 46 (sub_824D8F58) has the whole file and only checks a
// 16-bit sum, then the game re-signs it and saves it over the slot the player
// picks. Here the file is checked first (a bad one gets the game's own
// "Download failed.", state 49, and nothing is written), and each save file a
// download replaces is copied to Saves\.backup first.

constexpr uint32_t kDownloadBuffer = 0x82E3DD9C;  // -> -> the 12 MB buffer
constexpr uint32_t kDownloadMax = 0xC00000;
constexpr uint32_t kCawFileSize = 1347532, kPaintFileSize = 394516, kReelFileSize = 1555802;
constexpr uint32_t kStoryMax = 0x6CCC94;

uint32_t Rd32(const uint8_t* base, uint32_t a) {
  const uint8_t* p = base + a;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void Wr32(uint8_t* base, uint32_t a, uint32_t v) {
  uint8_t* p = base + a;
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

std::atomic<bool> g_importing{false};  // a download is being saved

// Copies a save file before a download replaces it: Saves\.backup\<name>.<time>
// (the last 10 of each kept).
void BackupSave(const std::filesystem::path& file) {
  std::error_code ec;
  if (!std::filesystem::exists(file, ec)) return;
  const auto dir = g_saves / ".backup";
  std::filesystem::create_directories(dir, ec);
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  char stamp[32];
  std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
  const std::string name = file.filename().string();
  std::filesystem::copy_file(file, dir / (name + "." + stamp), std::filesystem::copy_options::overwrite_existing, ec);
  REXLOG_INFO("online: {} backed up before the download replaces it{}", name, ec ? " (failed: " + ec.message() + ")" : "");
  std::vector<std::filesystem::path> old;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().filename().string().rfind(name + ".", 0) == 0) old.push_back(e.path());
  }
  std::sort(old.begin(), old.end());
  for (size_t k = 0; k + 10 < old.size(); ++k) std::filesystem::remove(old[k], ec);
}

// Why a downloaded file can't be used, or "" if it can.
std::string BadDownload(PPCContext& ctx, uint8_t* base, uint32_t type, uint32_t buf, uint32_t size) {
  if (!buf || !size || size > kDownloadMax) return fmt::format("size {}", size);
  switch (type) {
    case 0:  // Created Superstar: its size and header (0x4B, 0x18, 7 in every .cas)
      if (size != kCawFileSize) return fmt::format("size {} (a Superstar is {})", size, kCawFileSize);
      if (Rd32(base, buf) != 0x4B || Rd32(base, buf + 4) != 0x18 || Rd32(base, buf + 8) != 7) {
        return fmt::format("not a Superstar (header {:08X} {:08X} {:08X})", Rd32(base, buf), Rd32(base, buf + 4),
                           Rd32(base, buf + 8));
      }
      return {};
    case 3:  // Paint Tool logo: one slot, 256x256
      if (size != kPaintFileSize) return fmt::format("size {} (a logo is {})", size, kPaintFileSize);
      if (Rd32(base, buf) != 3 || Rd32(base, buf + 36) != 256 || Rd32(base, buf + 40) != 256) {
        return "not a 256x256 Paint Tool logo";
      }
      return {};
    case 4:
      if (size != kReelFileSize) return fmt::format("size {} (a highlight reel is {})", size, kReelFileSize);
      return {};
    case 1:
      if (size > kStoryMax) return fmt::format("size {} (a story is at most {})", size, kStoryMax);
      return {};
    default:
      return {};
  }
}

// A downloaded Superstar's extra logos (caw_logos.cpp's 'XLG1' record, at the
// same place as in its .cas): fetched now, before it is first shown.
void PrefetchLogos(const uint8_t* caw) {
  constexpr uint32_t kExt = 20 + 160000;
  uint32_t magic;
  std::memcpy(&magic, caw + kExt, 4);
  if (magic != 0x584C4731) return;
  for (int k = 0; k < 10; ++k) {
    uint64_t hash;
    std::memcpy(&hash, caw + kExt + 24 + 8 * k, 8);
    if (!caw[kExt + 8 + k] || !hash) continue;
    char name[24];
    std::snprintf(name, sizeof(name), "%016llX.bin", static_cast<unsigned long long>(hash));
    const auto file = g_saves / ".logos" / name;
    std::error_code ec;
    if (std::filesystem::exists(file, ec)) continue;
    OnMissingLogo(hash, file);
  }
}

// The Paint Tool page / file a logo download goes to (paint_pages.h calls
// this just before the game's write).
bool OnPaintDownload(uint8_t* base, uint32_t screen, int page) {
  const uint32_t slot = Rd32(base, screen + 0x8344) % 20;  // (0-199 over the 10 pages)
  char name[32];
  if (page == 0) {
    BackupSave(g_saves / "00PaintTool.pt");
  } else if (slot < 20) {
    std::snprintf(name, sizeof(name), "p%02d_s%02u.bin", page + 1, slot + 1);
    BackupSave(g_saves / ".paint" / name);
  }
  return true;
}
}  // namespace

uint32_t OnlineString(uint32_t id) {
  switch (id) {
    case 0x5339:  // VIEW GAMER CARD (button help)
    case 0xB09B:
    case 0xB0C8:
    case 0xB0AE:  // Xbox LIVE Party
    case 0xB0B2:  // Invite Xbox LIVE Party
    case 0xB15D:  // Invite Xbox LIVE Party Member
      return g_empty_string;
    default:
      return 0;
  }
}

void InstallOnline(rex::memory::Memory* memory, const std::filesystem::path& saves) {
  g_empty_string = memory->SystemHeapAlloc(4);
  if (g_empty_string) std::memset(memory->TranslateVirtual<char*>(g_empty_string), 0, 4);
  if (!rex::cvar::Query<bool>("online_enabled")) return;
  g_saves = saves;
  net::StartRelay();
  SetMissingLogoHandler(OnMissingLogo);
  SetPaintDownloadCheck(OnPaintDownload);
  std::thread(LogoUploads).detach();
  const bool url = Patch(memory, kAuthUrl, kAuthUrlHttps, kAuthUrlHttp, sizeof(kAuthUrlHttp));
  const bool key = Patch(memory, kCertKeyModulus, kGameSpyModulus, kServerModulus, sizeof(kServerModulus));
  REXLOG_INFO("online: on, server {} (auth over http: {}, server key: {})",
              rex::cvar::Query<std::string>("online_server"), url, key);
}

}  // namespace svr2011

// Community Creations upload of a Paint Tool logo: the game refuses logos
// flagged "not made here" (data+32) - pictures loaded from outside the Paint
// Tool, which includes the launcher's Paint Tool imports, and logos drawn
// from them. Here only logos downloaded from Community Creations (data+13)
// are refused: players share the art they bring in.
REX_EXTERN(__imp__sub_828D1648);
REX_HOOK_RAW(sub_828D1648) {
  const uint32_t data = ctx.r3.u32;
  ctx.r3.u64 = base[data + 13] == 0 ? 1 : 0;
}

// Community Creations upload of a story: the game's check (sub_82629160,
// asked only by the upload confirm sub_82511C18) says "This Created Content
// was created by a different player" (string 50951) when the story's 15
// entries at +0x2801A5 (stride 0x4E9EC) have one present but not the
// player's own, when a scene (500 at +20112, stride 4056) has a created
// Superstar that isn't the player's (sub_8257BAC0: downloaded, other DLC
// parts...), or when the story itself was downloaded (+84+13). Stories made
// on another console or under another online identity - the PC's identity
// changes when the player signs in - were refused. Here only downloaded
// stories are.
REX_EXTERN(__imp__sub_82629160);
REX_HOOK_RAW(sub_82629160) {
  const uint32_t story = ctx.r3.u32;
  __imp__sub_82629160(ctx, base);
  if (ctx.r3.u32) return;
  const bool downloaded = base[story + 84 + 13] != 0;
  bool entries = true;
  for (uint32_t k = 0, p = story + 0x2801A5; k < 15; ++k, p += 0x4E9EC) {
    if (base[p - 0x4E9C7] && !base[p]) entries = false;
  }
  REXLOG_INFO("online: story upload: the game refused it ({}){}", downloaded ? "downloaded" :
              entries ? "a created Superstar in its scenes" : "one of its entries",
              downloaded ? "" : " - uploading it");
  if (!downloaded) ctx.r3.u64 = 1;
}

// The menus' button help bar: sub_82746358(table, bar id, out) fills out with
// three slots of two string ids (u16 at +0 and +2 of each 4 bytes: the
// button's and the label's; 0xFFFF: none), which sub_8274A458 shows. The PC's removed
// labels (OnlineString) go with their button.
REX_EXTERN(__imp__sub_82746358);
REX_HOOK_RAW(sub_82746358) {
  const uint32_t out = ctx.r5.u32;
  __imp__sub_82746358(ctx, base);
  // (the bar ends at its first empty slot: the ones after a removed one
  // move up)
  uint8_t* slots = base + out;
  uint32_t kept = 0;
  for (uint32_t slot = 0; slot < 3; ++slot) {
    const uint8_t* p = slots + slot * 4;
    const uint32_t icon = (uint32_t(p[0]) << 8) | p[1], label = (uint32_t(p[2]) << 8) | p[3];
    if ((icon != 0xFFFF && svr2011::OnlineString(icon)) || (label != 0xFFFF && svr2011::OnlineString(label))) {
      continue;
    }
    if (kept != slot) std::memmove(slots + kept * 4, p, 4);
    ++kept;
  }
  for (; kept < 3; ++kept) std::memset(slots + kept * 4, 0xFF, 4);
}

// The WWE SHOP: the PC has no marketplace and needs no Online Axxess (all
// content is licensed). sub_8247E330 makes a list item from a catalog entry
// (r6: the item; its type at +24 - 6 is Online Axxess, the only one): that
// one is left out. sub_82480068 shows "Unable to update the SHOP content
// list" once the marketplace offers came back empty ([+308] == 1, [+320]
// != 0): the shop lists the game's own catalog without it.
REX_EXTERN(__imp__sub_8247E330);
REX_HOOK_RAW(sub_8247E330) {
  const uint32_t item = ctx.r6.u32;
  __imp__sub_8247E330(ctx, base);
  if (ctx.r3.u32 && item) {
    const uint8_t* p = base + item + 24;
    const uint32_t type = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
    if (type == 6) ctx.r3.u64 = 0;
  }
}

// Online Axxess (THQ's online pass): always owned - it's 2026. sub_828C19D8(id)
// is the game's "is this unlocked" (ids 0, 1 and 5 through the DLC manager:
// category 5 = catalog types 5 and 6); id 5 is Online Axxess, asked by every
// gate: entering ONLINE (without it, a "trial period" from Xbox LIVE storage
// that can't be had: "Information related to your trial period couldn't be
// retrieved"), the ONLINE menu's greyed items, the shop.
REX_EXTERN(__imp__sub_828C19D8);
REX_HOOK_RAW(sub_828C19D8) {
  if (ctx.r3.u32 == 5) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_828C19D8(ctx, base);
}

// -- Community Creations limits lifted ----------------------------------------

// "You are unable to upload due to this created content containing
// downloadable content" (string 50999): the upload confirm step
// (sub_82511C18) tests a DLC flag per kind of content first - a Created
// Superstar's parts (sub_82579EB0: the part table's DLC byte), the +116 /
// +120 kinds (also sub_82579EB0), +128 (sub_8257AB78: bit 0x4) and stories
// (sub_82628FE0: the Superstars its scenes use). Each is asked only there:
// no DLC, always - DLC content uploads like any other.
REX_HOOK_RAW(sub_82579EB0) {
  (void)base;
  ctx.r3.u64 = 0;
}
REX_HOOK_RAW(sub_8257AB78) {
  (void)base;
  ctx.r3.u64 = 0;
}
REX_HOOK_RAW(sub_82628FE0) {
  (void)base;
  ctx.r3.u64 = 0;
}

// "To use a Created Superstar with Paint Tool Data in a match, you must first
// upload the data to the server ... Do you want to upload these?" (string
// 50940) on entering ONLINE: not asked - the answer is always YES. The online
// manager's check (sub_824F27E8) shows it through sub_824FC010(box, 50940)
// (its only use) and goes to state 1, whose handler (sub_824F18D0) waits for
// the box (sub_824FC1E8) and takes its answer (sub_824FC290; 1 = YES: the
// uploads of every attire's Paint Tool data, then "Upload complete").
namespace {
std::atomic<bool> g_paint_auto_yes{false};   // (the question was skipped)
thread_local bool g_paint_answering = false;  // (inside its state-1 handler)
}  // namespace

REX_EXTERN(__imp__sub_824FC010);
REX_HOOK_RAW(sub_824FC010) {
  if (ctx.r4.u32 == 50940) {
    g_paint_auto_yes = true;
    REXLOG_INFO("online: Paint Tool data to upload - uploading it (no question)");
    return;
  }
  __imp__sub_824FC010(ctx, base);
}

REX_EXTERN(__imp__sub_824F18D0);
REX_HOOK_RAW(sub_824F18D0) {
  if (!g_paint_auto_yes.exchange(false)) {
    __imp__sub_824F18D0(ctx, base);
    return;
  }
  g_paint_answering = true;
  __imp__sub_824F18D0(ctx, base);
  g_paint_answering = false;
}

REX_EXTERN(__imp__sub_824FC1E8);
REX_HOOK_RAW(sub_824FC1E8) {  // (the box has an answer)
  if (g_paint_answering) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_824FC1E8(ctx, base);
}

REX_EXTERN(__imp__sub_824FC290);
REX_HOOK_RAW(sub_824FC290) {  // (its answer: YES)
  if (g_paint_answering) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_824FC290(ctx, base);
}

REX_EXTERN(__imp__sub_82480068);
REX_HOOK_RAW(sub_82480068) {
  uint8_t* shop = base + ctx.r3.u32;
  auto load = [](const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  };
  if (load(shop + 308) == 1 && load(shop + 320) != 0) {
    shop[308] = shop[309] = shop[310] = shop[311] = 0;
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82480068(ctx, base);
}

// -- Downloads (see "Community Creations downloads" above) -------------------

// State 46: the downloaded file, complete.
REX_EXTERN(__imp__sub_824D8F58);
REX_HOOK_RAW(sub_824D8F58) {
  using namespace svr2011;
  const uint32_t mgr = ctx.r3.u32;
  {
    // (while its message box is up the game just waits: so do we)
    const auto saved = ctx;
    ctx.r3.u64 = mgr + 756;
    sub_824FC1C0(ctx, base);
    const bool busy = ctx.r3.u32 != 0;
    ctx = saved;
    if (busy) {
      __imp__sub_824D8F58(ctx, base);
      return;
    }
  }
  const uint32_t type = Rd32(base, mgr + 1172), size = Rd32(base, mgr + 1424);
  const uint32_t holder = Rd32(base, kDownloadBuffer);
  const uint32_t buf = holder ? Rd32(base, holder) : 0;
  const std::string bad = BadDownload(ctx, base, type, buf, size);
  if (!bad.empty()) {
    REXLOG_WARN("online: download (type {}) refused: {} - nothing written", type, bad);
    Wr32(base, mgr + 1176, 49);  // "Download failed."
    return;
  }
  REXLOG_INFO("online: download (type {}, {} bytes) checked", type, size);
  if (type == 0) PrefetchLogos(base + buf);
  __imp__sub_824D8F58(ctx, base);
}

// The save job of a download: sub_824DCA98(mgr, 1) starts it (0: the slot
// list), sub_824DCCE8 ends it.
REX_EXTERN(__imp__sub_824DCA98);
REX_HOOK_RAW(sub_824DCA98) {
  using namespace svr2011;
  g_importing = ctx.r4.u32 == 1;
  if (g_importing && Rd32(base, ctx.r3.u32 + 1172) == 0) {  // a Superstar: its entrance's song and movie
    const uint32_t holder = Rd32(base, kDownloadBuffer);
    PrepareDownloadedEntrance(base, holder ? Rd32(base, holder) : 0);
  }
  __imp__sub_824DCA98(ctx, base);
}

REX_EXTERN(__imp__sub_824DCCE8);
REX_HOOK_RAW(sub_824DCCE8) {
  __imp__sub_824DCCE8(ctx, base);
  svr2011::g_importing = false;
}

// The writes of a download (the slot the player picked at job+33604): the
// file they replace is backed up first.
namespace {
void BackupSlotFile(uint8_t* base, uint32_t job, const char* format) {
  if (!svr2011::g_importing) return;
  const uint32_t slot = svr2011::Rd32(base, job + 33604);
  if (slot >= 100) return;
  char name[48];
  std::snprintf(name, sizeof(name), format, slot);
  svr2011::BackupSave(svr2011::g_saves / name);
}
}  // namespace

REX_EXTERN(__imp__sub_82513240);  // Created Superstar
REX_HOOK_RAW(sub_82513240) {
  BackupSlotFile(base, ctx.r3.u32, "%02uCreateSuperStar.cas");
  __imp__sub_82513240(ctx, base);
}

REX_EXTERN(__imp__sub_82518B78);  // highlight reel
REX_HOOK_RAW(sub_82518B78) {
  BackupSlotFile(base, ctx.r3.u32, "%02uSceneDat.scn");
  __imp__sub_82518B78(ctx, base);
}

REX_EXTERN(__imp__sub_82518BF0);
REX_HOOK_RAW(sub_82518BF0) {
  BackupSlotFile(base, ctx.r3.u32, "%02uSceneDat.scn");
  __imp__sub_82518BF0(ctx, base);
}

REX_EXTERN(__imp__sub_82517AA8);  // kept in SaveData.dat
REX_HOOK_RAW(sub_82517AA8) {
  if (svr2011::g_importing) svr2011::BackupSave(svr2011::g_saves / "SaveData.dat");
  __imp__sub_82517AA8(ctx, base);
}

REX_EXTERN(__imp__sub_82519C60);
REX_HOOK_RAW(sub_82519C60) {
  if (svr2011::g_importing) svr2011::BackupSave(svr2011::g_saves / "SaveData.dat");
  __imp__sub_82519C60(ctx, base);
}
