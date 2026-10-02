// The game's frame rate: 30, 60, 120, 144 or 240 frames a second, all at the
// game's normal speed (setting frame_rate).
//
// The game advances one step per frame and sizes each step by its timing
// block (sub_826E1AE8 SetFrameRate: 60 normally, 30 in entrances), which it
// only fills for 25 / 30 / 50 / 60 fps. The port writes the block for the
// chosen rate and runs the game's frame clock (the guest vblank,
// guest_vblank_hz) at it.
#pragma once

#include <cstdint>

namespace rex::memory {
class Memory;
}

namespace svr2011 {

void InstallFrameRate(rex::memory::Memory* memory);

// After the game sets its frame rate (sub_826E1AE8, sub_826E1D28): the
// chosen rate's timing instead (menu_hooks.cpp).
void ApplyFrameRate(uint8_t* base);

// The chosen frame rate (a cap), and the one the game runs at now (the frames
// the PC makes, up to the cap: the game's speed stays right).
int TargetFrameRate();
int FrameRateNow();

// Runs the game's frame clock for that rate (guest_vblank_hz).
void SetFrameClock(int fps);

// Chooses the frame rate, at once (GRAPHICS -> FRAME RATE).
void SetTargetFrameRate(int fps);

// Writes the game's timing block for `fps` frames a second.
void WriteTiming(uint8_t* base, int fps);

}  // namespace svr2011
