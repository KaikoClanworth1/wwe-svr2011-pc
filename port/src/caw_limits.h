// CREATE A SUPERSTAR without its limits (caw_limits.cpp).
#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallCawLimits(rex::memory::Memory* memory);

// The editor's part heap (heap 42, 45 MB: every layer of the CAW being edited
// loads its part there) is full: a block of `size` bytes from elsewhere (the
// emulator's physical system heap), or 0. Called by the allocators' hooks
// (ring_rules.cpp) on a failed heap-42 allocation.
uint32_t CawHeapFallback(uint32_t size);
// The same fallback memory for another full game heap (the file loader's cache, model_mem.cpp): `size` bytes,
// 4 KB aligned, or 0. Freed by the same hook (CawHeapFree).
uint32_t FallbackBlock(uint32_t size);
// Big models / arenas (model_mem.cpp): a full XMemAlloc allocator 128 (a wrestler's 768 KB vertex window, the
// stage's) or a wrestler's model heap gives way to the fallback memory - so their budget (the game's, the
// converters', the Mod Maker's) is ModelPageLimit() pages, not 192. Off: test aid SVR2011_TEST_BIGMEM_OFF=1 (the
// old limits, the game's own way).
bool BigModelMemory();
// The pages a wrestler's model may take: 1024 with BigModelMemory (4 MB, about 70,000 vertices), else 192; test aid
// SVR2011_TEST_PAGE_LIMIT=<pages>.
uint32_t ModelPageLimit();
// A block CawHeapFallback gave is freed (operator delete's hook,
// frame_rate.cpp): true when it was one of them (freed here).
bool CawHeapFree(uint32_t ptr);

}  // namespace svr2011
