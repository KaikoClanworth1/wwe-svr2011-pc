// WWE SmackDown vs. Raw 2011 - the port's menu entries: GRAPHICS in MY WWE ->
// OPTIONS, ACHIEVEMENTS in MY WWE and EXIT in the main menu.
//
// tools/patch_menu.py (and game_files.cpp) add the entries to the menu table
// (menu.pac MFLO/0000) with their own label string ids. Here:
//  - the string lookup returns their labels (the game's string tables have no
//    such strings),
//  - choosing an entry opens the port's page instead of the game's screen
//    (graphics_page.h, achievements_page.h) or closes the game.

#include "frame_rate.h"
#include "menu_hooks.h"

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"
#include "achievements_page.h"
#include "graphics_page.h"
#include "discord_presence.h"
#include "online.h"
#include "touch_controls.h"
#include "native/native_renderer.h"

namespace {

constexpr uint32_t kGraphicsLabelId = 0xAFC0;  // keep in sync with tools/patch_menu.py
constexpr uint32_t kExitLabelId = 0xAFC1;
constexpr uint32_t kAchievementsLabelId = 0xAFC2;
constexpr uint32_t kAchievementsTextId = 0xAFC3;  // its description line
constexpr uint32_t kLanguageLabelId = 0xAFC4;
// Character select: the name beside the "?" tile ("Random", looked up by the
// panel code in sub_82465728 for tile kind 4) - that tile opens the managers
// (src/managers.cpp), so it reads "Extra" there.
constexpr uint32_t kRandomTileId = 0x0116;
constexpr uint32_t kRandomTileCaller = 0x82465970;
constexpr uint32_t kMainMenuGroup = 0x01;
constexpr uint32_t kMyWweGroup = 0x05;
constexpr uint32_t kAchievementsRow = 4;       // MY WWE: after OPTIONS
constexpr uint32_t kOptionsGroup = 0x11;       // MY WWE -> OPTIONS
constexpr uint32_t kGraphicsRow = 5;           // its 6th entry
constexpr uint32_t kLanguageRow = 6;           // after GRAPHICS
constexpr uint32_t kExitRow = 7;               // main menu: after SHOP (row 6)
// Main-menu object fields (sub_82447210 and friends).
constexpr uint32_t kMenuGroup = 380;   // current group (< 1000) or screen id
constexpr uint32_t kMenuCursor = 360;  // cursor row within the group

uint32_t g_label = 0;       // guest address of "GRAPHICS"
uint32_t g_exit_label = 0;  // and of "EXIT"
uint32_t g_ach_label = 0;   // "ACHIEVEMENTS" and its description
uint32_t g_ach_text = 0;
uint32_t g_language_label = 0;  // "LANGUAGE"
uint32_t g_extra_label = 0;     // "Extra" (the "?" tile opens the managers: src/managers.cpp)

uint32_t Be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

}  // namespace

namespace svr2011 {

void InstallMenuHooks(rex::memory::Memory* memory) {
  // The labels in the game's language (user_language: the Xbox 360 language
  // ids; the game's own text follows it).
  struct Labels {
    uint32_t language;
    const char* graphics;
    const char* exit;
    const char* achievements;
    const char* achievements_text;
    const char* language_label;
  };
  static const Labels kLabels[] = {
      {1, "GRAPHICS", "EXIT", "ACHIEVEMENTS",
       "View your achievements: what you've unlocked and how to earn the rest.", "LANGUAGE"},
      {4, "GRAPHISMES", "QUITTER", "SUCC\xC3\x88S",
       "Consultez vos succ\xC3\xA8s : ceux d\xC3\xA9" "bloqu\xC3\xA9s et comment obtenir les autres.", "LANGUE"},
      {3, "GRAFIK", "BEENDEN", "ERFOLGE",
       "Deine Erfolge: was du freigeschaltet hast und wie du den Rest bekommst.", "SPRACHE"},
      {5, "GR\xC3\x81" "FICOS", "SALIR", "LOGROS",
       "Mira tus logros: los que has desbloqueado y c\xC3\xB3mo conseguir el resto.", "IDIOMA"},
      {6, "GRAFICA", "ESCI", "OBIETTIVI",
       "I tuoi obiettivi: quelli sbloccati e come ottenere gli altri.", "LINGUA"},
  };
  const uint32_t language = rex::cvar::Query<uint32_t>("user_language");
  const Labels* l = &kLabels[0];
  for (const Labels& k : kLabels)
    if (k.language == language) l = &k;
  const size_t n_graphics = std::strlen(l->graphics) + 1, n_exit = std::strlen(l->exit) + 1,
               n_ach = std::strlen(l->achievements) + 1, n_text = std::strlen(l->achievements_text) + 1,
               n_language = std::strlen(l->language_label) + 1;
  g_label = memory->SystemHeapAlloc(uint32_t(n_graphics + n_exit + n_ach + n_text + n_language + 6 + 16));
  if (!g_label) {
    REXLOG_WARN("menu: no guest memory for the GRAPHICS label");
    return;
  }
  std::memcpy(memory->TranslateVirtual<char*>(g_label), l->graphics, n_graphics);
  g_exit_label = g_label + uint32_t(n_graphics);
  std::memcpy(memory->TranslateVirtual<char*>(g_exit_label), l->exit, n_exit);
  g_ach_label = g_exit_label + uint32_t(n_exit);
  std::memcpy(memory->TranslateVirtual<char*>(g_ach_label), l->achievements, n_ach);
  g_ach_text = g_ach_label + uint32_t(n_ach);
  std::memcpy(memory->TranslateVirtual<char*>(g_ach_text), l->achievements_text, n_text);
  g_language_label = g_ach_text + uint32_t(n_text);
  std::memcpy(memory->TranslateVirtual<char*>(g_language_label), l->language_label, n_language);
  g_extra_label = g_language_label + uint32_t(n_language);
  std::memcpy(memory->TranslateVirtual<char*>(g_extra_label), "Extra", 6);  // (the same in all five; shown in capitals)
}

}  // namespace svr2011

// String lookup: sub_82153EF8(manager, id) -> (r3) the string.
REX_EXTERN(__imp__sub_82153EF8);
REX_HOOK_RAW(sub_82153EF8) {
  if (const uint32_t s = svr2011::OnlineString(ctx.r4.u32)) {  // (online.h)
    ctx.r3.u64 = s;
    return;
  }
  if (g_label && ctx.r4.u32 == kGraphicsLabelId) {
    ctx.r3.u64 = g_label;
    return;
  }
  if (g_exit_label && ctx.r4.u32 == kExitLabelId) {
    ctx.r3.u64 = g_exit_label;
    return;
  }
  if (g_ach_label && ctx.r4.u32 == kAchievementsLabelId) {
    ctx.r3.u64 = g_ach_label;
    return;
  }
  if (g_ach_text && ctx.r4.u32 == kAchievementsTextId) {
    ctx.r3.u64 = g_ach_text;
    return;
  }
  if (g_language_label && ctx.r4.u32 == kLanguageLabelId) {
    ctx.r3.u64 = g_language_label;
    return;
  }
  if (g_extra_label && ctx.r4.u32 == kRandomTileId && uint32_t(ctx.lr) == kRandomTileCaller &&
      rex::cvar::Query<bool>("managers_tile")) {
    ctx.r3.u64 = g_extra_label;
    return;
  }
  // Debug: SVR2011_DUMP_STRINGS=<file> writes all the loaded strings (ids
  // 0-0xFFFF) once the menu tables are loaded (first lookup of PLAY).
  static bool dumped = false;
  if (!dumped && ctx.r4.u32 == 0xA029) {
    dumped = true;
    char* path = nullptr;
    size_t n = 0;
    if (_dupenv_s(&path, &n, "SVR2011_DUMP_STRINGS") == 0 && path) {
      if (FILE* f = std::fopen(path, "wb")) {
        const auto saved = ctx;
        for (uint32_t id = 0; id < 0x10000; ++id) {
          ctx.r3.u64 = saved.r3.u64;
          ctx.r4.u64 = id;
          __imp__sub_82153EF8(ctx, base);
          const uint32_t s = ctx.r3.u32;
          if (s) std::fprintf(f, "%04X\t%.120s\n", id, reinterpret_cast<const char*>(base + s));
        }
        std::fclose(f);
        ctx = saved;
      }
      free(path);
    }
  }
  // Debug: SVR2011_LOG_STRINGS=<text> - lookups returning a string with that
  // text, with the caller (to find the code behind a label).
  static const std::string log_text = [] {
    char* v = nullptr;
    size_t n = 0;
    std::string s;
    if (_dupenv_s(&v, &n, "SVR2011_LOG_STRINGS") == 0 && v) {
      s = v;
      free(v);
    }
    return s;
  }();
  if (!log_text.empty()) {
    const uint32_t manager = ctx.r3.u32, id = ctx.r4.u32, caller = uint32_t(ctx.lr);
    __imp__sub_82153EF8(ctx, base);
    if (ctx.r3.u32) {
      const char* s = reinterpret_cast<const char*>(base + ctx.r3.u32);
      if (std::strstr(s, log_text.c_str()))
        REXLOG_INFO("[svr2011] string {:04X} (manager {:08X}, caller {:08X}): {:.80}", id, manager, caller, s);
    }
    return;
  }
  __imp__sub_82153EF8(ctx, base);
}

// Main menu: go into the selected entry - sub_82447210(menu, by_player);
// by_player (r4) is 1 when the player pressed A.
REX_EXTERN(__imp__sub_82447210);
REX_HOOK_RAW(sub_82447210) {
  if (ctx.r4.u32 == 1) {
    const uint8_t* menu = base + ctx.r3.u32;
    const uint32_t group = Be32(menu + kMenuGroup), cursor = Be32(menu + kMenuCursor);
    REXLOG_INFO("[svr2011] menu select: group {:X} row {}", group, cursor);
    svr2011::TouchGameInMatch(false);  // (the touch controller's MENU layout)
    svr2011::native::SetMatchScene(false);  // (wide screens: menus at 16:9)
    svr2011::SetDiscordScene(svr2011::DiscordScene::kMenus);
    if (group == kOptionsGroup && cursor == kGraphicsRow) {
      svr2011::OpenGraphicsPage();
      return;
    }
    if (group == kOptionsGroup && cursor == kLanguageRow) {
      svr2011::OpenLanguagePage();
      return;
    }
    if (group == kMyWweGroup && cursor == kAchievementsRow) {
      svr2011::OpenAchievementsPage();
      return;
    }
    if (group == kMainMenuGroup && cursor == kExitRow) {
      REXLOG_INFO("[svr2011] EXIT chosen - closing");
      svr2011::RequestExit();
      return;
    }
  }
  __imp__sub_82447210(ctx, base);
}

// ---------------------------------------------------------------------------
// 60 fps where the game drops to 30 (entrances, some cutscenes).
//
// The game's frame rate is one setting: sub_826E1AE8(fps) - SetFrameRate - or
// sub_826E1C88 (halve it, what entrances use) -
// stores it (a global at 0x82EDDBF0: 60, or 30 / 25 in entrances) with the
// timing constants that go with it, and ~100 places read it (a frame step of
// 2 at 30 fps, the D3D present interval through sub_826D8A90 ->
// SetPresentInterval sub_8291D1D0, ...). It always stays at the game's own
// 60 (frame_rate.h: the world runs in 60 Hz steps); these scenes are drawn at
// 30 frames a second only with the setting off. (Only forcing the present
// interval to 1 made entrances play ~1.7x too fast.)
REXCVAR_DEFINE_BOOL(unlock_30fps, true, "GPU",
                    "Run the game's 30 fps scenes (entrances, cutscenes) at 60 fps");

namespace {
void ArmWatch(uint8_t* base);
}

// Entrances halve the rate instead: sub_826E1C88 (60 -> 30, 50 -> 25, with
// the matching constants; sub_826E1D28 restores it). Never done: the game's
// world stays at its 60 Hz steps (frame_rate.h); with the setting off the
// entrance is drawn at 30 frames a second instead, as the original.
REX_EXTERN(__imp__sub_826E1C88);
REX_HOOK_RAW(sub_826E1C88) {
  svr2011::TouchGameInMatch(true);  // entrances: a match (the touch controller's MATCH layout)
  svr2011::native::SetMatchScene(true);  // (wide screens: full width)
  svr2011::SetDiscordScene(svr2011::DiscordScene::kEntrances);
  if (!REXCVAR_GET(unlock_30fps)) svr2011::SetSceneThirtyFps(true);
}

// The entrances' end: sub_826E1D28 restores the frame rate - the match starts.
REX_EXTERN(__imp__sub_826E1D28);
REX_HOOK_RAW(sub_826E1D28) {
  REXLOG_INFO("[svr2011] entrances over");
  svr2011::SetDiscordScene(svr2011::DiscordScene::kMatch);
  __imp__sub_826E1D28(ctx, base);
  svr2011::SetSceneThirtyFps(false);
  svr2011::ApplyFrameRate(base);  // (frame_rate.h)
}

REX_EXTERN(__imp__sub_826E1AE8);
REX_HOOK_RAW(sub_826E1AE8) {
  REXLOG_INFO("[svr2011] SetFrameRate({}){}", ctx.r3.u32, ctx.r3.u32 != 60 ? " - kept at 60" : "");
  __imp__sub_826E1AE8(ctx, base);
  svr2011::ApplyFrameRate(base);  // the game's 60 again (frame_rate.h)
  ArmWatch(base);  // debug (SVR2011_WATCH_ADDR)
}

// ---------------------------------------------------------------------------
#if !defined(_WIN32)
namespace {
void ArmWatch(uint8_t*) {}  // (the watch below is Windows only)
}  // namespace
#else
// Debug: SVR2011_WATCH_ADDR=<guest hex address> logs the host stack of every
// write to that guest word (armed once the game has set its frame rate; the
// page is write-protected, other writes to it are single-stepped through).
namespace {
// The guest's physical memory is visible at 0xA0000000, 0xC0000000 and
// 0xE0000000 (separate host mappings): watch the word in all three.
constexpr int kViews = 3;
uintptr_t g_watch_host[kViews] = {};
uintptr_t g_watch_page[kViews] = {};
thread_local int g_watch_stepping = -1;
DWORD g_watch_protect = PAGE_READONLY;  // PAGE_NOACCESS with SVR2011_WATCH_READ (reads too)

void ProtectAll(DWORD protect) {
  DWORD old;
  for (uintptr_t page : g_watch_page)
    if (page) VirtualProtect(reinterpret_cast<void*>(page), 0x1000, protect, &old);
}

LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2 &&
      (ep->ExceptionRecord->ExceptionInformation[0] == 1 || g_watch_protect == PAGE_NOACCESS)) {
    const bool write = ep->ExceptionRecord->ExceptionInformation[0] == 1;
    const uintptr_t addr = ep->ExceptionRecord->ExceptionInformation[1];
    int v = -1;
    for (int i = 0; i < kViews; i++)
      if (g_watch_page[i] && (addr & ~uintptr_t(0xFFF)) == g_watch_page[i]) v = i;
    if (v < 0) return EXCEPTION_CONTINUE_SEARCH;
    if (addr >= g_watch_host[v] && addr < g_watch_host[v] + 4) {
      void* frames[16];
      const USHORT n = CaptureStackBackTrace(0, 16, frames, nullptr);
      HANDLE process = GetCurrentProcess();
      static bool sym = SymInitialize(process, nullptr, TRUE);
      (void)sym;
      std::string stack;
      for (USHORT i = 0; i < n; ++i) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* info = reinterpret_cast<SYMBOL_INFO*>(buf);
        info->SizeOfStruct = sizeof(SYMBOL_INFO);
        info->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(process, DWORD64(frames[i]), &disp, info)) {
          stack += std::string(" < ") + info->Name;
        }
      }
      REXLOG_INFO("[svr2011] watch: {} (view {}) +{}{}", write ? "write" : "read", v, uint32_t(addr - g_watch_host[v]), stack);
    }
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(g_watch_page[v]), 0x1000, PAGE_READWRITE, &old);
    ep->ContextRecord->EFlags |= 0x100;  // single-step the write, then re-arm
    g_watch_stepping = v;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (code == EXCEPTION_SINGLE_STEP && g_watch_stepping >= 0) {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(g_watch_page[g_watch_stepping]), 0x1000,
                   g_watch_protect, &old);
    g_watch_stepping = -1;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

void ArmWatch(uint8_t* base) {
  static bool armed = false;
  if (armed) return;
  armed = true;
  char* v = nullptr;
  size_t n = 0;
  if (_dupenv_s(&v, &n, "SVR2011_WATCH_ADDR") != 0 || !v) return;
  const uint32_t guest = uint32_t(std::strtoul(v, nullptr, 16));
  free(v);
  const uint32_t phys = guest & 0x1FFFFFFF;
  const uint32_t views[kViews] = {0xA0000000u | phys, 0xC0000000u | phys,
                                  (0xE0000000u | phys) + 0x1000};  // E view is offset
  for (int i = 0; i < kViews; i++) {
    g_watch_host[i] = reinterpret_cast<uintptr_t>(base) + views[i];
    g_watch_page[i] = g_watch_host[i] & ~uintptr_t(0xFFF);
  }
  if (_dupenv_s(&v, &n, "SVR2011_WATCH_READ") == 0 && v) {
    g_watch_protect = PAGE_NOACCESS;
    free(v);
  }
  AddVectoredExceptionHandler(1, WatchHandler);
  ProtectAll(g_watch_protect);
  REXLOG_INFO("[svr2011] watch: armed on {:08X} (all views)", guest);
  // The guest heap resets the pages' protection when it (re)commits them:
  // re-arm now and then.
  CreateThread(
      nullptr, 0,
      [](void*) -> DWORD {
        for (;;) {
          Sleep(50);
          ProtectAll(g_watch_protect);
        }
      },
      nullptr, 0, nullptr);
}
}  // namespace
#endif  // _WIN32
