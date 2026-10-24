# CPU difficulty: SvR 2011 and WWE '12

Research for "a harder mode" (24 Oct 2026). Everything here comes from the two games' own code and data: 2011's
generated code (`port/generated`), and WWE '12's `default.xex` from `WWE12 Extract`, decrypted (retail AES, basic
compression) and run through `rexglue codegen` for its asm. No downloads.

## Where the difficulty lives

| | SvR 2011 | WWE '12 |
|---|---|---|
| The setting | byte `+10342` of the game block `0x82E3DE00` (getter `sub_825740C8`) | byte `+12382` of its game block (getter `sub_8267CF38`) |
| Values | 0 EASY, 1 NORMAL, 2 HARD, 3 LEGEND (strings 0xA114-0xA117 in both) | the same |
| Code that reads it | 16 functions | 21 functions |
| Tuning numbers | a text file in `misc.pac` BATS/INIT entry 0 (`WALKSPEED 70 95 115 130`, ...) read into `0x82E36BA0` (values) / `0x82E373A0` (names) | the same names' table compiled into the exe (`sub_82276590` writes it at `0x831740BC`-`0x831741B8`), plus new runtime tables |

The difficulty applies only when the match is a single-player one: both games read it after a check
(`sub_82573F18()+4 == 0` in 2011, `sub_8267CD80()+4 == 0` in '12); otherwise they use NORMAL.

## The user sliders both games already have

OPTIONS -> GAMEPLAY BALANCE OPTIONS (2011; '12 calls it BALANCING), in the same game block:

| Byte (2011) | Row | Used as |
|---|---|---|
| +10388..+10393 | A.I. REVERSAL BALANCING: STANDING STRIKE / STANDING GRAPPLE / GROUND STRIKE / GROUND GRAPPLE / FINISHER / FOREIGN ATTACK REVERSAL RATE | `sub_82815738`.. `sub_82815990`: CPU reversal chance = slider x (charge) / 50 - 50 is 100 %, 0 never reverses |
| +10394..+10399 | DAMAGE BALANCING (per move kind) | damage x (slider x 0.01 + 0.5) (`sub_821F32C8`) |
| +10406, +10407 | GAME BALANCING (momentum / stamina rates) | `sub_821F4FA0`, `sub_821F5FC0`, `sub_821F4D90` |

So the "AI sliders" idea (c) partly exists in 2011 already: reversal rate per kind, damage per kind,
momentum/stamina rates. What it lacks is a slider for the difficulty-only numbers below.

## Side by side: each thing the difficulty changes

Values are EASY / NORMAL / HARD / LEGEND.

| What (2011 code -> '12 code) | SvR 2011 | WWE '12 | Notes |
|---|---|---|---|
| CPU reversal chance `PROB_EASY..PROB_LEGEND` (`sub_82835150` -> `sub_82990948`) | 30 / 55 / 80 / 100 | 30 / 55 / 80 / 100 | the same. Each CPU's reversal roll (`sub_82835200`, reached only from the reversal code `sub_828162C8`): a 0-99 roll from a fixed table must be under PROB x the move's value / 100 |
| `FRAME_EASY..FRAME_LEGEND` (7 / 5 / 2 / 1) | in the text file, **never read** | - | dead data in 2011: no reaction-delay setting exists |
| Damage scale with difficulty (`sub_821F32C8` -> `sub_82248730`) | 0.75 / 1.0 / 1.0 / 1.2 | 1.00 / 0.90 / 0.80 / 0.70 (`0x831740E0..D4` x 0.01) | **opposite trend** in '12; which side it applies to is to be confirmed in a match (proof runs) |
| Gain scale (momentum/stamina; option +10407 / '12 +12451) (`sub_821F4D90` -> `sub_8224A818`) | 1.0 / 0.9 / 0.8 / 0.7 | 1.0 / 0.9 / 0.8 / 0.7 | the same |
| Rate scale (option +10406) (`sub_821F4FA0` -> `sub_8224AA70`) | 0.2 / 0.3 / 0.4 / 0.4 | 0.2 / 0.3 / 0.4 / 0.4 | the same |
| Base rate (option +10406) (`sub_821F5FC0`) | 0.1 / 0.2 / 0.3 / 0.4 | (merged into the above in '12) | |
| Pin kick-out window (`sub_8221DD88` -> `sub_8223BF68`) | 15 / 13 / 11 / 9 | 17 / 13 / 11 / 11 (`0x83174124`) | '12 LEGEND is **more forgiving** here |
| Easy-only rules (`sub_82275C88`, `sub_822BC588` -> `sub_822F3008`) | EASY sets a help value 2 | the same | |
| LEGEND-only branches (`sub_82334FB8`, `sub_823355B0`) | extra rules at LEGEND | (to map) | |
| Temporary LEGEND in a special event (`sub_82195E08` -> `sub_822143D0`) | difficulty +1 for an event, put back after | the same | |
| CPU AI level (AI object +28/+32 -> +36/+40) (`sub_82812230`, `sub_82814B08` -> `sub_8297DB60`, `sub_829801C8`) | the difficulty | the difficulty | read by the AI's reversal code (`sub_82815AD0`: LEGEND checks; `sub_82816090`: EASY/NORMAL) |
| **New in '12** (`sub_822482E0` / `sub_82248368` / `sub_8224A088` / `sub_8224BB58`): a base value x difficulty, stored at +200 / +204 / +196 / +188 of the person | - | +200 20/20/20/20, +204 20/20/20/20, +196 20/16/12/8, +188 25/20/15/10 | new '12 fields (2011 has no readers); likely its new comeback / recovery timings |
| **New in '12** tag help (`sub_82249B80`, 2+ teammates) | - | 2.0 / 1.8 / 1.5 / 1.2 | |
| **New in '12** (`sub_8224B270`, `sub_82318F68`, `sub_823CB850`) | - | tables filled at run time (byte x 60, 7 shorts per level, a 4-120 frame clamp) | not readable without running '12 |

## What "port WWE '12's LEGEND" can and can't mean

- The values that sit at a place 2011 also has are mostly **equal** ('12 kept PROB, the gain and rate scales).
- Where they differ, '12's LEGEND is **not harder**: the pin window is 11 frames (2011: 9) and the damage scale is
  0.70 (2011: 1.2).
- What made '12 feel harder is in **new mechanics** (new person fields, its reversal system rework, runtime tables):
  porting those is porting code, not numbers.

## Restoring 2011's LEGEND

The stock values: PROB_LEGEND 100, FRAME_LEGEND 1 (misc.pac text file), damage scale 1.2 (`sub_821F32C8`), pin
window 9 (`sub_8221DD88`), gain 0.7, rate 0.4 / 0.4. Any port change overrides these in code (hooks / the parameter
table after it is read), never in the game's files.

## Tools (scratchpad of the research, not shipped)

`xex_image.py` (XEX2 -> image; checked: 3951/3952 pages equal to 2011's dumped image), `asm_index.py` (function
-> asm index of a codegen), `consts.py` (a function's constant loads resolved against an image), `live_reads.py`.

## The port's CPU tab (built: OPTIONS -> GRAPHICS -> CPU)

`src/cpu_difficulty.cpp`, settings in svr2011.toml (`-1` = AUTO):

| Row | Setting | What it changes |
|---|---|---|
| PRESET | - | AUTO (all rows AUTO) or LEGEND+ (every row at its hard end) |
| CPU REVERSAL CHANCE | `cpu_reversal_chance` 0-200 | PROB, set on each CPU's roll after it is made (`sub_82835150`) |
| CPU REVERSAL RATES | `cpu_reversal_rates` 0-10 | the game's six A.I. REVERSAL BALANCING sliders (x 10), in the charge getters' results (`sub_82815738`..`sub_82815990`); the saved options are not touched |
| DAMAGE | `cpu_damage_level` 0-3 | `sub_821F32C8` runs with the DIFFICULTY byte at that level |
| PIN KICK-OUT | `cpu_pin_level` 0-3 | `sub_8221DD88` the same |
| MOMENTUM | `cpu_momentum_level` 0-3 | `sub_821F4D90`, `sub_821F4FA0`, `sub_821F5FC0` the same |

LEGEND+ = reversal chance 200, rates 10 (the sliders' maximum), damage / pin / momentum LEGEND. 2011's own
LEGEND is unchanged. Online the host's values count for everyone: session property `0x1000F005` (reversal chance
in the top byte, then 4 bits each for rates, damage, pin, momentum, 0xC5 marker).

Test aids: `SVR2011_TEST_DIFFICULTY=<0-3>` (the match's difficulty), `SVR2011_TEST_CPU_LOG=1` (every 10 s: each
CPU's reversal rolls taken / made, its PROB, and the damage fields' total).
