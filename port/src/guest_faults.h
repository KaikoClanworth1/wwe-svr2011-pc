// WWE SmackDown vs. Raw 2011 - guest memory faults no handler took (a write
// to guest address 0 ...): where they happen, and no endless retry.
#pragma once

namespace svr2011 {

// After the runtime's memory is set up (its fault handler first). Android /
// Linux only; elsewhere nothing.
void InstallGuestFaultLog();

}  // namespace svr2011
