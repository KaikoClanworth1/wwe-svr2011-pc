// WWE SmackDown vs. Raw 2011 - the ONLINE overlay: friends, who's online and
// invites to a match, over the game (as the Xbox Guide was).
//
// A key (bind_online_menu, F1) or BACK + RB on the controller opens it, and so
// does the game's own INVITE FRIENDS (lobby, X: XamShowGameInviteUI). It shows
// the account's friends from the Community Creations server (/api/friends:
// In an online match / Online / Offline), who added the player, and invites
// received. In a match session (public or private) A invites an online friend
// (/api/invite: the session's XSESSION_INFO); an invite received is accepted
// with A - through the game's own Xbox LIVE invite path: the port reaches the
// host (p2p.h), posts XN_LIVE_INVITE_ACCEPTED and answers the game's
// XInviteGetAcceptedInfo (sub_8298BA80) with the session, and the game joins
// it itself. As on the console, the game then restarts itself
// (XamLoaderSetLaunchData + XamLoaderLaunchTitle) to join from its boot: the
// port saves the launch data and the invite (UserData\relaunch.bin), starts a
// new copy of the game with the same command line and lets this one end; the
// new one gives them back to the game. While it is open the game sees an idle
// controller. An invite arriving while it's closed shows a notice.
#pragma once

#include <cstdint>
#include <filesystem>

struct ImFont;

namespace rex::ui {
class ImGuiDrawer;
class Window;
}
namespace rex::input {
class InputSystem;
}
namespace rex::system {
class KernelState;
}

namespace svr2011 {

// Once, when the dialogs are created. `input` may be null.
// (`window`: its touches - a phone; `user_data`: where the relaunch file goes.)
void InstallOnlineOverlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, rex::input::InputSystem* input,
                          rex::system::KernelState* kernel, const std::filesystem::path& user_data);

// Fonts (null: ImGui's default).
void SetOnlineOverlayFonts(ImFont* menu, ImFont* title);

// Opens / closes it (any thread).
void ToggleOnlineOverlay();

// True while it is open or the button that closed it is still held.
bool OnlineOverlayHoldsInput();

// The game's pad 1 buttons as it reads them (frame_rate.cpp's XInputGetState
// hook, on the game's thread): BACK + RB opens the overlay.
void OnlineOverlayPad(uint16_t buttons);

}  // namespace svr2011
