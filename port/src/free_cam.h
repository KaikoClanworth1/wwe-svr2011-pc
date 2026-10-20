// Free camera / photo mode (PC keyboard and controller).
//
//   F6  free camera on: it starts where the game's camera is (position,
//       direction, field of view) and flies from there; F6 again keeps it
//       where it is (the controls let go of it - the players play again)
//   F5  back to the game's own camera
//   F7  hide / show the 2D UI (the game's HUD and prompts, the port's overlays)
//   F8  pause / unpause the match while the free camera is on (not online);
//       the camera holds too while paused (no world update builds the view):
//       frame the shot, then F8
// Flying: W A S D move, Q / E down / up, the arrow keys look, Shift faster,
// Ctrl slower, [ ] zoom; a controller's left stick moves, the right stick
// looks, LB / RB down / up, the triggers zoom. While
// it flies, the game gets no controller input (players stand still). Keys
// only come while the game's window has the focus; the mouse is never taken.
//
// The game's camera keeps running underneath: the free one is written into
// the scene's view as the game builds it (0x82E3CC00, sub_82227150), so F5
// is the game's camera again at once.
#pragma once

#include <cstdint>

namespace rex::ui {
class Window;
}

namespace svr2011 {

void InstallFreeCam(rex::ui::Window* window);

// The game has built a view (match_types.cpp's hook of sub_82227150: view, eye,
// direction): the scene's gets the free camera's.
void FreeCamSceneView(uint8_t* base, uint32_t view, uint32_t eye, uint32_t dir);

// Once a frame after the world update (frame_rate.cpp): the first match's hint.
void FreeCamFrame(uint8_t* base);

// A pad reading for the game (XamInputGetState, frame_rate.cpp): while the
// free camera flies its sticks are read here and the game gets none.
void FreeCamPad(uint32_t user, uint8_t* state);

// F8: the world held (frame_rate.cpp runs no update passes).
bool FreeCamPaused();

// F7: the 2D UI hidden (native renderer: HUD sprites; the port's overlays).
bool UiHidden();

// Test aid (script input "hotkey <key> [ms]"): a key pressed, held that long
// (F5-F8, W A S D Q E, UP DOWN LEFT RIGHT, SHIFT, CTRL, [ ]).
bool FreeCamScriptKey(const char* name, int ms);

}  // namespace svr2011
