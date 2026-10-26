// WWE SmackDown vs. Raw 2011 - the GRAPHICS entry in MY WWE -> OPTIONS
// (see menu_hooks.cpp).

#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

namespace rex::memory {
class Memory;
}

namespace svr2011 {

// Once the game image is loaded: puts the entry's label in guest memory.
void InstallMenuHooks(rex::memory::Memory* memory);

// A string.pac string's text in the game's language (printable characters, trimmed; empty until the game has
// looked up its first string, or for a blank one) - match recordings' rule and arena names.
std::string GameString(PPCContext& ctx, uint8_t* base, uint32_t id);

}  // namespace svr2011
