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
// A block CawHeapFallback gave is freed (operator delete's hook,
// frame_rate.cpp): true when it was one of them (freed here).
bool CawHeapFree(uint32_t ptr);

}  // namespace svr2011
