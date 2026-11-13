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

// The menus' table was read (match_types.cpp, sub_82BAA6E8): the menus are up. After a match whose exit makes no menu
// select (the Universe's menus), this puts the menus' settings back - the touch MENU layout, menus' frame rate, 16:9 -
// and drops an armed match start (the Universe's exit loads a match structure, which armed one that fired in the menus).
void MenusShown();

// A string.pac string's text in the game's language (printable characters, trimmed; empty until the game has
// looked up its first string, or for a blank one) - match recordings' rule and arena names.
std::string GameString(PPCContext& ctx, uint8_t* base, uint32_t id);

}  // namespace svr2011
