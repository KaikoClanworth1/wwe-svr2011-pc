// WWE SmackDown vs. Raw 2011 - MY WWE -> JUKEBOX: which of the game's menu
// songs play (a menu entry the port adds: tools/patch_menu.py, menu_hooks.cpp).
//
// The menu music is one sound-engine event, Play_Menu_Music: a random
// container of 19 songs (10 original entrance themes, 9 menu themes) that
// moves on to another song when one ends. The page turns songs on and off
// (cvar jukebox_off); the container then only plays songs that are on - a song
// turned off while it plays stops, and another starts. With every song off the
// menus are silent. PREVIEW plays the chosen song now (on or off), then the
// menus go on with the songs that are on. Driven by the controller (D-pad /
// left stick: choose, A: on / off, X: preview, Y: all on / all off, LB / RB:
// page, B: back) or the keyboard (arrows, Enter / Space, P, Page Up / Down,
// Esc). While it is open the game sees an idle controller.
//
// MY MUSIC (LEFT / RIGHT: the page's second tab): the player's songs from the
// Music folder (music.h) join the shuffle, each on or off (cvar
// jukebox_my_music), with PREVIEW and ALL ON / ALL OFF too. A user song plays
// over the game's menu music muted under it, as the console's music player
// did, and stops when the game stops its menu music (a match, CREATE AN
// ENTRANCE's theme...). With no user song on the jukebox works as before.

#pragma once

#include <cstdint>

struct ImFont;
struct PPCContext;

namespace rex::memory {
class Memory;
}
namespace rex::ui {
class ImGuiDrawer;
}
namespace rex::input {
class InputSystem;
}

namespace svr2011 {

// Once, at start.
void InstallJukebox(rex::memory::Memory* memory);

// Every audio event the game posts by name (superstar_mods.cpp's
// sub_82BEC030 hook), before anything else sees it. True: the event is not
// posted (the menu music with every song off).
bool JukeboxEvent(uint8_t* base, const char* e, uint32_t object);

// Stops a MY MUSIC song playing in the menus (the game's own user-music
// player starting: CREATE AN ENTRANCE -> USER PLAYLIST, music.cpp).
void JukeboxStopMyMusic();

// Once a frame, from the game's world update (frame_rate.cpp): what the page
// asked for - the song stopped and another started, or a preview.
void JukeboxUpdate(PPCContext& ctx, uint8_t* base);

// Once, when the dialogs are created. `input` may be null.
void InstallJukeboxPage(rex::ui::ImGuiDrawer* drawer, rex::input::InputSystem* input);

// Fonts for the page (null: ImGui's default).
void SetJukeboxPageFonts(ImFont* menu, ImFont* title);

// Opens the page (any thread; the guest's menu hook).
void OpenJukeboxPage();

// True while the page is open or the button that closed it is still held.
bool JukeboxPageHoldsInput();

}  // namespace svr2011
