# Porting moves to SvR 2011

How a move from another game becomes a move SvR 2011 plays, and the rules each fix taught. The converter is
`port/tools/moves_port.py` (`convert --game G --src <disc> --item <move id> --out <dir>`); the game merges its
packs into `Mods/PacOverlay` at start (`port/src/move_packs.cpp`). The Mod Maker's modder how-to builds on this
page (`port/docs/MOVE_MODDING.md`).

## 1. What a 2011 move is

- **Motions** in YMKs banks, keyed `(id, x, y)`: `x` the phase or variant (0 the move; 90-92 reversal branches),
  `y` the track - 0 the attacker, 1 the victim, 17/18 ropes, 30-32 extra parts, 50-57 cameras, 60 props.
  Every move has a keyframe copy in `m.pac` (`MOT/BM..`, `MOT/RR..`, the `MVMT/<category>` menu banks) and a
  **packed copy in `mpsp.pac MOTP/GAME`** - the one a match plays.
- **Records** in `misc.pac`: `MOVS/WAZE` (160 bytes: the first 16 the category bits that make it selectable),
  `WAZA/DATA` EXH (per-key move data), sound events, MBD.
- **Events** inside the motion frames (an op byte, its length from the bank's 256-byte table at 0x10). They carry
  hits, the face, the follow-up motion (op 49), timing and synchronisation. A ported move takes its events
  from its **template** (a 2011 move), retimed to the new length.

## 2. Rules

### 2.1 Frames and keys

- **Keyframe copies (BM/RR/MVMT): a key every frame, count 0** - the key at frame f is the pose of frame f+1.
- **Packed copies (MOTP/GAME): a key every 5 frames, count 4** - the key at frame f is the pose of frame f+5
  (2011's own: Attitude Adjustment 5010, 265 frames, 2971 bytes). A packed copy keyed every frame (4x the
  size) froze both people about 60 frames in while the motion's frame ran on past its end (2K15 F-5).
- A bank directory's `frames` = the motion stream's frame count.

### 2.2 Time

- **2011 plays a motion key every 2 match frames** (30 keys a second: the forced Last Ride, 265 keys, ran 526 match
  frames; the person's motion frame at +24 counts match frames). **2K14/2K15 YANM blocks have twice 2011's keys**:
  2K15's Attitude Adjustment 2 is 532 source frames, 2011's 265 - the converter samples every second one.

### 2.3 Where the two people are (2K15)

- **The victim (block person 1) starts at the origin; the attacker (person 0) is stored in its own frame:
  turned 180 degrees about the vertical and 1500 root units down -z.** Turned into the victim's frame
  (`w13_retarget.facing_frame`, root x -> -x, z -> -1500 - z, facing and IK together), 2K15's AA 2 - a copy
  of 2011's 5010 - matches 2011's attacker to a few units. Unturned, the victim played its part metres away.

### 2.4 Ids

- **Below every template bank's last id.** A bank's directory is sorted and ends at its highest id (16000 in
  `MOT/BM00/0` and its kin); the merge inserts only below it. 2K15's AA on record 17013 went into no bank and
  never started. Before 2.1.6 the game then skipped the **whole bank** - every pack's motions in it; now only
  that motion, with "id past this bank's last ... left out" in the log.
- **Not 9000-9999 for a keyframe move.** The game treats 9000-9779 as streamed (YMBs) motions by its flag
  table; a keyframe move there plays through the wrong path.
- The converter picks a free record (one 2011 keeps without motions) within those limits; `--id` chooses.

### 2.5 The template

- **The template brings its events, cameras, props and rope tracks.** Pick one of the same situation: a rope
  move's events wait on the ropes - 2K15's F-5 on Throat Thrust 4 (a rope move) froze in its carry.
- Coded categories (2K14+: `C000`, `E125`, ...) carry no meaning by name: the converter maps each to the 2011
  categories its moves' 2011 namesakes sit in (a vote - 2K15 E125/E127 standing front grapples, E136 behind,
  E160 diving, M050 ropes, M060 apron, M090 tag team), and uses the 2011 move of the same name when there is one.
- **Dives onto a lying victim are one-person moves in 2011.** The Frog Splash 3555 and its kind are the
  attacker's motion alone (x 0 y 0; the victim's x 50 y 1 is the miss) - the victim takes the hit with the
  game's reaction. A source whose attacker starts high (root y < -1150, the top rope) and whose victim starts
  lying (root y > -400) gets a one-person CORN template, picked by where the attacker ends, and its victim
  track is left out (2K15's Superfly Splash: Leap of Faith 3591; before, Corner Head Scissors Flip - a
  two-person corner move - and it was called off in 5 frames). Such a move can't be forced with
  `SVR2011_TEST_FORCE_MOVE` (it starts a pair: the victim waits in a motion with no track of his).

### 2.6 Face events

- Op 68 (`[68, n, frames]`) picks face n-1 from an 18-entry table; anything past 17 read wild memory and killed
  the match thread - the "frozen pair" (report 001255). The game skips such events since 2.1.6.

- **In-game each person's root plays in its own placed frame**: the bases (+288) stay where the paired start put
  them (the F-5: 1.5 m apart, unmoved), and a shared-frame viewer has to place the attacker's root turned to face
  the victim to match what the game draws.

### 2.7 Phases (multi-part moves)

- A source move with several block pairs (a submission: hold, struggle loops, escape) keeps one phase per pair:
  2K15 phase n (decimal in the block name; 0090-0092 are reversals) feeds 2011's x n of the template. Before,
  every x got phase 0's motion.

### 2.8 Submissions (YMBs templates)

- 2011's submissions (Ankle Lock 3 21305, STF 21350, Sharpshooter 21302) keep their body tracks in YMBs banks
  (MOT/BM02/4, MVMT/ANAS/17, MOTP/GAME/1a); the converter solves the source into YMBs channels per x
  (ymbs_retarget). The stock hold starts from a lying victim (the attacker walks to the feet, sits in, HOLD
  prompt) and the game's submission state runs the loops after x 0.
- The attacker of a ground hold is NOT turned (Patriot Lock: 1455 units as stored, 2955 turned): the turn is
  chosen per move by the closer start (2.3). Turned without the shift, a YMBs attacker held one pose.
- YMBs tracks are moved so the source attacker starts where the template's does (as the keyframe path).
- **A hold with phases is a submission**: a source with 3+ two-person phases under 90 (hold, struggle loops,
  escape, tap) gets a 2011 submission template (a YMBs move in MVMT/ANAS, most phases in common, then the
  closest length) before the category vote - a coded category mixes holds with other moves (2K15's M013 is
  mostly reversals: its vote said STAT and the Patriot Lock got Mule Kick Reversal 2; now Single Leg Boston
  Crab 21308).
- Open: a forced ported hold doesn't play out (25 Oct: it stays in the motion for the whole run, Ankle Lock 3
  or Boston Crab alike; before it played x 0 and ended): it doesn't run the hold / struggle loop the stock one
  does (the loop comes from the template's records and the game's submission state, not yet matched).

## 3. Testing a ported move

- **A forced move only animates when someone in the match has it in their moveset** - the match loads the
  motions of its people's movesets. Put the new id in a test superstar's `moves.txt` first.
- `SVR2011_TEST_FORCE_MOVE=<attacker>,<victim>,<every s>,<motion>` starts the move; `SVR2011_TEST_MOTION_LOG=1`
  logs each person's motion changes; `SVR2011_TEST_FRAME_SHOT=<f.ppm>` with `_SHOTS=<n>`, `_SHOT_AT=<match frame>`
  and `_SHOT_EVERY=<k>` saves n frames k apart (contact sheets); `SVR2011_TEST_STATE_LOG=212,24` logs a person's
  motion and its frame (+24, a float) as they change.
- **Placing the attacker**: `SVR2011_TEST_FORCE_PLACE=<x>,<z>,<yaw>[,<y>]` puts him there in the ring (y -12 the
  mat, yaw in radians) before each forced start - rope, apron and corner moves (forced where the AI happens to
  stand, the Rope Hung Suplex ran outside the ring and both sank under the mat).
- **A forced move is never reversed**: `SVR2011_TEST_FORCE_MOVE` starts it with sub_82191AA8, past the point
  where the game decides a reversal (the stock F-5 5010, forced on a person tapping RT, was never reversed).
  Reversals can only be seen in moves the CPU picks itself, and even then rarely - see 5, reversal test aid.
- **CPU use (e) and reversals (d)**: no forced move - a CPU vs CPU match with the ported moves in the CPU
  superstar's slots, `SVR2011_TEST_MOTION_LOG=1` and `SVR2011_TEST_CPU_LOG=1` (reversal rolls). In
  `SVR2011_TEST_MATCH` only person 1 is a CPU when person 0 is the first named: put the test superstar second.
  Put front grapples in the front grapple groups C/D/E (move block 0x98-0xB2, not the fixed 0xB4-0xBA); the
  rear slots 0xBC-0xC2 are rarely picked. Damage per CPU hit is in the CPU log's `damage n in k hits`.
- **Hidden test games**: `SVR2011_WINDOW_HIDDEN=1` (tools/test_guard.ps1 sets it) - the window is never shown;
  use frame shots, window screenshots of a hidden window are black.

## 4. Per game

### WWE 2K15
- Disc `WWE 2K15.iso`; moves in `mot07/mot08.pac` ZMVM (YANM records), records in `createmode.pac MOVS/WAZE`.
- Blocks `<phase><person>`: `0000 0000/0001` the move's attacker and victim for most grapples; `0030..` for
  springboard / diving moves.
- Pair layout 2.3; frames 2.2.
- Status of the hard moves (criteria a-g: sync, landing, damage, reversal, CPU use, no stuck pair / crash at
  60 fps, survives save / load):

  | Move | id | a-c | d reversal | e CPU picks it | f |
  |---|---|---|---|---|---|
  | Attitude Adjustment | 5743 | ok | pending tooling | yes (x2) | ok |
  | F-5 | 7564 | ok | pending tooling | not yet (forced: ok) | ok |
  | Triple German Suplex | 5188 | ok | pending tooling | yes (x3) | ok |
  | The Last Ride | 5872 | ok | pending tooling | yes | ok |

  (d) so far: each carries its template's reversal branches (x 90/91/92; the Last Ride's template has x 90
  only), and in 6 CPU picks at the default 55 % reversal chance none was reversed, none stuck (527-537 frames,
  the victim down after).
  (g) passed for the F-5: CREATE A MOVE-SET -> CREATED SUPERSTAR -> a CAW -> SPECIAL MOVES -> SIGNATURES ->
  `MOD: F-5 1`, ACCEPT CHANGES (saved); a new game on that save, a match with only that CAW holding the move
  (`SVR2011_TEST_MATCH=people=CAW0,...`): the F-5 played 9 times, the victim down each time, no stuck pair.
  The moveset keeps the id, so every pack move saves alike; menu shots of a hidden game come from
  `SVR2011_TEST_SHOT_REQUEST`.
- Needs a tag match: Batista Bomb & RKO (a double-team move) - not tested in a singles match.

## 5. Parked: a reversal test aid

Wanted: `SVR2011_TEST_REVERSE=<person>` - the next move on that person goes into its reversal branch (x 90-92),
for a contact sheet of the switch from the ported x 0 into the template's branch (the likely bug: a pose pop
where a reversal starts from the ported pose).

Known:
- Reversal branches are tracks of the move's own id at x 90 / 91 / 92 (the porter copies the template's).
- `SVR2011_TEST_FORCE_MOVE` calls sub_82191AA8(attacker, victim, motion), which starts x 0 directly - the
  reversal decision is upstream of it (the grapple's move choice), so forced moves are never reversed.
- The CPU's reversal roll object is made by sub_82835150 (PROB at +8) and rolled by sub_82835200 (from
  sub_828513D8), but `SVR2011_TEST_CPU_LOG` never logged a taken roll for a grapple - stock moves included -
  so grapple reversals are decided elsewhere.
- The test match's pick is racy: the scripted pad often controls no one (both people CPU); setting the slot's
  controller byte (slot -8 = 0) did not make a person the player's.

Where to look: the code that reads a motion's x - the YMKs lookup by (id, x, y) and its callers; find the
caller that asks for x 90 (log every lookup with x >= 90 in a CPU match: stock reversals happen) and walk up to
the decision; then start that path from the test aid.

