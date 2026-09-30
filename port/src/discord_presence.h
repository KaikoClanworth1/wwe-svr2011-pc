// WWE SmackDown vs. Raw 2011 - Discord Rich Presence.
//
// Shows "Playing WWE SmackDown vs. Raw 2011" in Discord with what the player
// is doing (menus, entrances, a match) and the time played, through the
// Discord client's local IPC pipe (\\.\pipe\discord-ipc-N) - no Discord SDK.
// Windows only (the Steam Deck under Proton and phones: nothing is sent).
// Does nothing when Discord isn't running; reconnects if it starts later.
//
// Settings: discord_presence (on), discord_app_id (the Discord application
// whose name and icon are shown).

#pragma once

namespace svr2011 {

// Once at startup.
void StartDiscordPresence();

// Game state (any thread).
enum class DiscordScene { kMenus, kEntrances, kMatch };
void SetDiscordScene(DiscordScene scene);

}  // namespace svr2011
