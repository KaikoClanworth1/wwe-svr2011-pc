// WWE SmackDown vs. Raw 2011 - the GRAPHICS entry in MY WWE -> OPTIONS
// (see menu_hooks.cpp).

#pragma once

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// Once the game image is loaded: puts the entry's label in guest memory.
void InstallMenuHooks(rex::memory::Memory* memory);

}  // namespace svr2011
