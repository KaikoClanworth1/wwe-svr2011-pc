# Climbing on the announce tables (research, 2026-10-08)

**On hold: waiting for how SvR 2010 did it** (button, position, what happens on top, CPU use).

Request: players want to climb on top of the announce tables "as in SvR 2010"
(gone in SvR 2011). Research only so far; nothing built.

## What was checked, and what it showed

**Move records (misc.pac WAZE).** Neither game names a move after an
announce table, a desk or a climb. The only "Table" names are Table 1-4
(9045, 9068, 9125, 9273, 9694), the same in both games.

**Motions (m.pac / mpsp.pac).** 1036 motion ids exist in SvR 2010 and not in
2011; 77 of them have no move record (actions). Almost all are Road to
WrestleMania story motions (MOT/RU* banks). The ten in the match banks:
7, 165, 640, 641, 881, 1967, 3479, 5990, 6090, 23000.

- No 2010-only motion keeps the character raised for its whole length
  (standing on something), in either game.
- Only one 2010-only motion goes from the floor up by about a table's height:
  **881** (MOT/BM03/0, 74 frames, two people: the attacker rises ~6.7 units, the
  opponent ends lying ~28 units away). Its meaning is not established.
- 2011 still has five climb-up motions of the same height (155, 157, 160, 711,
  11508) and step-down ones (3651/3652 x50, 3798, 3849, 11508). They exist in
  both games, so they are shared actions (apron, steps or similar), not a
  removed table climb.
- Character root heights: standing on the floor ~ -8.7, so a table top would
  be ~ -15.5 (about 6.7 units up).

**2011 code.** `sub_8218D9F8` (generated svr2011_recomp.79.cpp) acts when a
wrestler's state (+212) is **165** or **1966**, and then plays **1967** (random
variant) and **1968** on a linked character (+2224). 165, 1966 and 1967 exist only
in SvR 2010's motion data (MOT/BM03/0 and /6), so this is leftover code for a
removed 2010 action. From stick-figure views, 165 and 1967 look like
celebration / crowd poses (165 stands about 3.5 units higher than the floor);
nothing ties them to the announce table yet.

**Arena collision (bg PAC HMD, entries 0x3e4 / 0x3e7).** Records are 96 bytes
(orientation, position, half-sizes). SvR 2010's SmackDown arena has two
different lists (106 and 252 boxes); 2011 ships the same 130-box list twice.
The extra 2010 boxes are scenery (stand walls 77 units tall, stage pieces), not
a table top: the two games' arenas are simply built differently. The announce
table itself is not in these lists in either game.

## What is still open

- Whether SvR 2010 really had a climb onto the announce table, and how it was
  done (button, position, what happens on top: stand, taunt, dive). Nothing in
  the data shows a dedicated climb, so either it used shared motions plus
  engine code that only exists in SvR 2010's executable (not recompiled here),
  or the feature was in another game.
- What 165 / 1966 / 1967 / 881 are (2010-only).
- Where 2011 keeps the announce table's object data (it is breakable: table
  spots); not in the bg collision lists.

## Routes (to choose once the 2010 behaviour is known)

- (a) Flag at load: only if 2011's code still has the climb and the table only
  lost a flag. No sign of that so far.
- (b) Motions through a move pack: possible for any 2010 motion (the Sabu /
  SvR 2010 move pack path), but motions alone do nothing without the state that
  plays them.
- (c) New interaction code: a "near the table + button" check, a climb motion,
  a raised floor height while on top, a step-down, and dives that start from
  the table height. The raised standing would reuse 2011's own standing
  motions; climb and dives exist in 2011 (155/157/160/711, the 3480-3482 and
  3651-3653 x50 dives). This is the likely route if SvR 2010 had no data-level
  version, and it is new game code (hooks on the movement / action code).

AI: unknown until the 2010 behaviour is pinned down.
