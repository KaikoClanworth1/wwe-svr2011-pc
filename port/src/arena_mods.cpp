// Arena mods (arenas branch). The game names its files in a pointer table at
// 0x82DAC7C0 ("GAME:\PAC\BG\BG00.PAC" ...), and opens an arena's file when the
// arena loads (it is not held open). Pointing a host arena's entry at another
// path makes the game load that file instead.
#include "arena_mods.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/xmemory.h>

#include "generated/default/svr2011_init.h"

namespace {

constexpr uint32_t kFileTable = 0x82DAC7C0;
// Arena file table slots (pointer index in kFileTable) of BG00..BG20.
constexpr uint32_t kBg00Slot = 29;

uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void Wr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

rex::memory::Memory* g_memory = nullptr;

// The slot whose string is GAME:\PAC\BG\BG<nn>.PAC, or 0.
uint32_t SlotOf(uint8_t* base, int arena) {
  char want[32];
  std::snprintf(want, sizeof want, "GAME:\\PAC\\BG\\BG%02d.PAC", arena);
  for (uint32_t i = 0; i < 160; ++i) {
    const uint32_t p = Rd32(base + kFileTable + 4 * i);
    if (p >= 0x82000000 && p < 0x84000000 && !std::strcmp(reinterpret_cast<const char*>(base + p), want))
      return kFileTable + 4 * i;
  }
  return 0;
}

// Points arena <arena>'s table entry at <path> (a guest path, e.g.
// GAME:\Mods\x.pac). Returns false if the arena has no table entry.
bool Redirect(int arena, const std::string& path) {
  uint8_t* base = g_memory->virtual_membase();
  const uint32_t slot = SlotOf(base, arena);
  if (!slot) return false;
  const uint32_t str = g_memory->SystemHeapAlloc(uint32_t(path.size() + 1));
  if (!str) return false;
  std::memcpy(base + str, path.c_str(), path.size() + 1);
  Wr32(base + slot, str);
  return true;
}

}  // namespace

// Test aid: SVR2011_TEST_BPE_LOG=1 logs every BPE decode (the game's
// sub_826AEF10(src, dst)): caller, source, destination, unpacked size.
bool g_bpe_log = false;

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

void InstallArenaMods(rex::memory::Memory* memory) {
  g_memory = memory;
  (void)kBg00Slot;
  g_bpe_log = std::getenv("SVR2011_TEST_BPE_LOG") != nullptr;
  // Test aid: SVR2011_TEST_ARENA_REDIRECT=<nn>=<guest path>
  if (const char* v = std::getenv("SVR2011_TEST_ARENA_REDIRECT"); v && *v) {
    const std::string s = v;
    const size_t eq = s.find('=');
    if (eq != std::string::npos) {
      const int arena = std::atoi(s.substr(0, eq).c_str());
      const std::string path = s.substr(eq + 1);
      if (Redirect(arena, path)) REXLOG_INFO("[svr2011] arena mods: BG{:02} -> {} (test)", arena, path);
      else REXLOG_WARN("[svr2011] arena mods: BG{:02} has no file table entry", arena);
    }
  }
}

}  // namespace svr2011
