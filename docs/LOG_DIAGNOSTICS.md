# Log diagnostics: the match black box

From 2.1.6, every match keeps a small **black box** (`port/src/match_box.cpp`). It lets us fix a match bug from a
player's report without reproducing it: which motion each wrestler was in, which move asked for it, whether that
move came from a mod pack, and what happened in the match around it. It's always on, and it costs a few reads per
world update.

## What the box holds

- **Context:**
  - the rule id (the match type: `docs/MATCH_TYPES_RESEARCH.md`);
  - the arena number, and the arena mod's name if one is played;
  - the frame rate setting (30 / 60);
  - online or offline;
  - the CPU tab values (`cpu_reversal_chance`, `cpu_reversal_rates`, `cpu_damage_level`, `cpu_pin_level`,
    `cpu_momentum_level`; -1 = the game's own).
- **Per person** (0-5 wrestlers, 6 the referee, 7 / 8 the announcers):
  - name, and the superstar mod's folder if it is one;
  - the current motion and state (`lying` = motions 430-448, `standing` = 20500, else `-`);
  - position;
  - **motions**: the last 32 motion changes as `motion@clock`;
  - **moves**: the last distinct moves asked when a motion changed, as `move slot <slot> game|pack <folder>`. The slot
    is the moveset offset in hex (`tools/pro_slots.json` `off`); `pack <folder>` names the move pack or superstar mod
    the move comes from.
- **Events** (the last 64, `clock text`):
  - `rule NN (was MM)`: the rule changed (e.g. a THREE STAGES OF HELL fall);
  - `referee motion N`: each referee motion change. A pin count shows here as the referee's count motions.
  - `weapon slot N made (model M)`, `... picked up by person P`, `... dropped by person P`, `weapon slot N gone`
    (broken or removed). Models are `bgEtc` STG/WPON pack ids.
  - `motion N missed by every bank (person P)`: a person is in a motion no bank has (see detectors).
- The **clock** is the match clock: 60 a second, from the bell.

## Where it goes

| When | Where | How much |
| --- | --- | --- |
| Every match end (also QUIT MATCH) | `logs\matchbox_1.txt` (newest; `_2`, `_3` older) | The whole box |
| A detector fires, the world update gets stuck, two wrestlers are held 10 s, no move asked for 30 s | The game log, lines `[svr2011] box: ...` | Compact: 8 motions each, 12 events; at most 3 boxes a match, each reason once |
| The game crashes | `UserData\crashes\crash_*.txt`, after the stack and registers (and so in the log's `crash:` lines) | The whole box |

The launcher's, the Mod Maker's and the Android launcher's **Report a problem** zips take the newest three
`matchbox_*.txt` (`logs/`, or `game/logs/` in the Mod Maker's).

## Detectors

Each one logs the box with its reason as the first line: `[svr2011] box: match box (<reason>), match <n>, clock <c>`.

| Reason | Meaning | Limit |
| --- | --- | --- |
| `person P stood up from lying L with no get-up` | Lying (430-448) went straight to standing 20500: the "falls, then pops up standing" reports | 2 per person per match |
| `the referee's motions repeat A / B four times` | The referee's last 8 motion changes are one pair four times over: the "count repeats 2" reports | 2 per match |
| `person P is in motion M, which no bank has` | A body-track look-up of M missed every bank, and a person is now in M (T-pose / freeze) | 1 per person |
| `person P at a NaN / far position (x, y, z)` | Not finite, or beyond 2000 / 1000 from the ring | 1 per person |
| `two wrestlers held in one motion frame for 10 s` | A frozen grapple pair (`match_diag.cpp`; also its own `match state` lines) | 1 per match |
| `no wrestler asked for a move for 30 s` | Both stopped (`match_diag.cpp`) | 1 per match |
| `world update stuck` | `frame_rate.cpp`'s watchdog (also its thread dump lines) | 1 per match |

## Other match lines (older, still logged)

- `[svr2011] match: rule NN, arena A - <people>`: the match as it starts.
- `[svr2011] weapons: placed - slot:model@x,y,z ...`: the weapons as placed at the start.
- `[svr2011] match state (...)`: each wrestler's last move, its age and its mod (`match_diag.cpp`).
- `frame rate: world update stuck N s in pass P ...`: the stuck update's object and job list.

## Test aids (hidden test runs only)

- `SVR2011_TEST_BOX=<what>[,<clock>]`: once a match, at that match clock (default 600), fakes one detector's case for
  person 0 and logs the box:
  - `teleport`: lying 440, then standing;
  - `refloop`: the referee's count step repeating;
  - `nan`: a NaN position;
  - `missing`: its motion missed by every bank;
  - `crash`: a fault on the world update's thread, for a crash file with the box.
- `SVR2011_TEST_MATCH_STATE=<s>`: a `match state` dump `s` seconds into the match.
