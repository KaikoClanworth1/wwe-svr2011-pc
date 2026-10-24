// WWE SmackDown vs. Raw 2011 - the CPU's difficulty, part by part (OPTIONS -> GRAPHICS, CPU tab;
// docs/CPU_DIFFICULTY.md).
#pragma once

namespace svr2011 {

// The port's CPU settings (svr2011.toml; -1 = AUTO: the game's DIFFICULTY / its own sliders decide):
//  reversal - the CPU's reversal chance (the game's PROB: EASY 30, NORMAL 55, HARD 80, LEGEND 100), 0-200 %;
//  rates    - the game's A.I. REVERSAL BALANCING sliders for every kind, 0-10 (x 10: the slider's value; the
//             game's default 50 = 5, its maximum 100 = 10);
//  damage   - the damage scale of a level (EASY 0.75, NORMAL / HARD 1.0, LEGEND 1.2), level 0-3;
//  pin      - the pin kick-out window of a level (EASY 15, NORMAL 13, HARD 11, LEGEND 9), level 0-3;
//  momentum - the momentum / stamina rates of a level, level 0-3.
// The levels apply where the game uses its difficulty (single-player matches); the reversal chance and rates in
// every match - online the host's.
struct CpuTuning {
  int reversal = -1, rates = -1, damage = -1, pin = -1, momentum = -1;
  bool operator==(const CpuTuning&) const = default;
};
// LEGEND+ (the page's preset): everything at the hard end.
inline constexpr CpuTuning kLegendPlus{200, 10, 3, 3, 3};

CpuTuning LocalCpuTuning();
// The values a match uses: online the host's (its session carries them), else this game's.
CpuTuning EffectiveCpuTuning();
// The settings changed (the page): saved, and the sessions this game hosts carry them.
void SetCpuTuning(const CpuTuning& t);

}  // namespace svr2011
