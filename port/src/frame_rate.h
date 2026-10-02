// The game's frame rate: frames a second up to the chosen one (setting
// frame_rate: 30 or 60), with the game's world always at its normal speed.
//
// The game's world advances in steps of 60 Hz (its timing block,
// sub_826E1AE8 SetFrameRate, stays at the game's own 60). Each frame the
// world update runs once per 60 Hz tick of real time - twice a frame at 30
// fps, or when the PC can't make 60 - and the picture is drawn once. The game
// draws at most 60 frames a second: its world and its menus are made in
// those steps.
#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallFrameRate(rex::memory::Memory* memory);

// After the game sets its frame rate (sub_826E1AE8, sub_826E1D28): its 60
// again (menu_hooks.cpp).
void ApplyFrameRate(uint8_t* base);

// The chosen frame rate (30 or 60), and the frames the game draws for it.
int TargetFrameRate();
int FrameRateNow();

// Entrances and cutscenes at 30 frames a second (the original's; setting
// unlock_30fps off): on while one plays (menu_hooks.cpp).
void SetSceneThirtyFps(bool on);

// Online lockstep (p2p.cpp, during a match session): exactly one world update
// per frame at a 60 Hz frame clock, as on the console - both peers step the
// world alike; a device that can't make 60 runs slower instead of catching up.
void SetLockstep(bool on);

// Chooses the frame rate, at once (GRAPHICS -> FRAME RATE).
void SetTargetFrameRate(int fps);

// Writes the game's timing block for `fps` frames a second.
void WriteTiming(uint8_t* base, int fps);

}  // namespace svr2011
