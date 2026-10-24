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
// 4 KB aligned, or 0. Freed by the same hook (CawHeapFree). `cpu`: from the virtual system heap instead (a CPU heap's
// blocks - the stage's heap, kStageHeap - not GPU memory: guest addresses below 0x80000000, as the game heaps' own).
uint32_t FallbackBlock(uint32_t size, bool cpu = false);
// Big models / arenas (model_mem.cpp): a full XMemAlloc allocator 128 (a wrestler's 768 KB vertex window, the
// stage's) or a wrestler's model heap gives way to the fallback memory - so their budget (the game's, the
// converters', the Mod Maker's) is ModelPageLimit() pages, not 192. Off: test aid SVR2011_TEST_BIGMEM_OFF=1 (the
// old limits, the game's own way).
bool BigModelMemory();
// The pages a wrestler's model may take: 1024 with BigModelMemory (4 MB, about 70,000 vertices), else 192; test aid
// SVR2011_TEST_PAGE_LIMIT=<pages>.
uint32_t ModelPageLimit();
// The stage's heap (6 MB, AB8C2E00-ABEBAE00 - SVR2011_TEST_HEAP_LOG=1 lists them): its models' per-mesh arrays
// (sub_826E84D0: +240, sub_826E75A8's matrices +176), lists and decode buffers. The game's own arenas fill most of
// it (bg16: 65,450 vertices); an arena with more 3D detail ran it out - the first failure a copy to 0 at lr
// 8215A008, the next ones whatever came after (sub_826E73F8, sub_826E8268, the decoder sub_826AEF10). With
// BigModelMemory a failed allocation in it takes the fallback memory (ring_rules.cpp's sub_8269A8D8 hook), given back
// by the heap's delete hook.
constexpr uint32_t kStageHeap = 10;
// The match's weapons' heap (1.2 MB, A8749E00-A8889E00): a block per weapon list entry, sized by its model (a table
// 76,944 bytes, a cup 15,488), then the 87,584 bytes sub_8227C760 asks for last (lr 8227CD88). The game's nine fill
// most of it; OPTIONS -> WEAPONS picks heavier in it (any picks: the list's free places are padded) ran it out -
// that block 0, the copy into it a crash at lr 8215A008 (PC; Android: the fault retried forever - a frozen
// loading screen, 24 Oct). With BigModelMemory a failed allocation in it takes the fallback memory too
// (ring_rules.cpp's sub_8269BA30 and sub_8269A8D8 hooks).
constexpr uint32_t kWeaponHeap = 38;
// A block CawHeapFallback gave is freed (operator delete's hook,
// frame_rate.cpp): true when it was one of them (freed here).
bool CawHeapFree(uint32_t ptr);

}  // namespace svr2011
