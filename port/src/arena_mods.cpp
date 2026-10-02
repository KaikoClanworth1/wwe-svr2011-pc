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
#include "arena_mods.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/vfs.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"

namespace {

constexpr const char* kGameDevice = "\\Device\\Harddisk0\\Partition1";
namespace fs = std::filesystem;

rex::memory::Memory* g_memory = nullptr;
rex::filesystem::VirtualFileSystem* g_fs = nullptr;
std::mutex g_mutex;
fs::path g_game, g_overlay;
bool g_overlay_ready = false;
std::string g_redirect[100];  // arena number -> relative file ("" = original)

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

// overlay/<name> := link to (or copy of) `source`
bool Place(const fs::path& source, const std::string& name) {
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

// Test aid: SVR2011_TEST_BPE_LOG=1 logs every BPE decode (the game's
// sub_826AEF10(src, dst)): caller, source, destination, unpacked size.
bool g_bpe_log = false;

}  // namespace

REX_EXTERN(__imp__sub_826AEF10);
REX_HOOK_RAW(sub_826AEF10) {
  if (g_bpe_log) {
    const uint8_t* s = base + ctx.r3.u32;
    const uint32_t size = s[12] | s[13] << 8 | s[14] << 16 | uint32_t(s[15]) << 24;
    REXLOG_INFO("[svr2011] bpe: lr {:08X} src {:08X} dst {:08X} size {:X} end {:08X}", uint32_t(ctx.lr),
                ctx.r3.u32, ctx.r4.u32, size, ctx.r4.u32 + size);
  }
  __imp__sub_826AEF10(ctx, base);
}

namespace svr2011 {

void RedirectArena(int arena, const std::string& relative_file) {
  if (!g_fs || arena < 0 || arena >= 100) return;
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

void InstallArenaMods(rex::memory::Memory* memory, rex::filesystem::VirtualFileSystem* fs) {
  g_memory = memory;
  g_fs = fs;
  g_game = rex::filesystem::GetExecutableFolder();
  g_overlay = g_game / "Mods" / "ArenaOverlay";
  g_bpe_log = std::getenv("SVR2011_TEST_BPE_LOG") != nullptr;
  // Test aid: SVR2011_TEST_ARENA_REDIRECT=<nn>=<file relative to the game folder>
  if (const char* v = std::getenv("SVR2011_TEST_ARENA_REDIRECT"); v && *v) {
    const std::string s = v;
    const size_t eq = s.find('=');
    if (eq != std::string::npos) RedirectArena(std::atoi(s.substr(0, eq).c_str()), s.substr(eq + 1));
  }
}

}  // namespace svr2011
