// WWE SmackDown vs. Raw 2011 - test aid SVR2011_TEST_MEMTRACE=1: which free gives back each block of the two areas
// that cap big models and arenas (mem_trace.cpp), so their fallback memory is handed back on the right path.
#pragma once

#include <cstdint>

namespace svr2011 {

bool MemTraceOn();
// XMemAlloc (sub_82158C70) gave ptr for allocator 128 (models' vertex / index buffers).
void MemTraceXAlloc(uint32_t ptr, uint32_t size, uint32_t attr, uint32_t caller);
// XMemFree (sub_82158940) was asked to free ptr.
void MemTraceXFree(uint32_t ptr, uint32_t attr, uint32_t caller);
// The game heap's new (sub_8269BBA0) gave ptr from heap `heap`.
void MemTraceNew(uint32_t ptr, uint32_t size, uint32_t heap, uint32_t caller);
// The game heap's delete (sub_8269B2D0) was asked to free ptr.
void MemTraceDelete(uint32_t ptr, uint32_t caller);

}  // namespace svr2011
