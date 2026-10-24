# Move modding: port a move, check it, fix it

This is how to bring a move from another game into SvR 2011 with the Mod
Maker, check that it will work in a match, and fix the usual problems.

- **How the porter works** (source formats, per-game conversions, its
  choices): [MOVE_PORTING.md](MOVE_PORTING.md).
- **What a move pack is on disk** (banks, `misc.pac` records, how the game
  loads a pack): [MOVE_PACKS.md](MOVE_PACKS.md).

## 1. Port a move

1. **Mod Maker -> From another game -> Moves.** Pick the game, then the move.
2. **Template** (optional). This is the 2011 move the port is built on. The
   port takes its move record (which moveset slots it may go in, whether the
   CPU uses it) and its per-key data from the template. If you leave it
   empty, the porter picks the closest stock move and says which one it
   picked ("Built on the 2011 move ... picked automatically").
   - Pick a template of the same kind: a corner move for a corner move, a
     top-rope move for a top-rope move, a grapple from the same side.
3. **Convert.** The result names the template and the record id the move
   got: a free id, so it never replaces a stock move.
4. Under the result, open **Inspect and check move N**. Do this before you
   use the move (see section 2).
5. **Open it to finish** opens the pack on the **Moves** page. Watch the move
   there, then put it in a superstar's moveset (**Put it in**, or a
   superstar's Moveset section).

A move pack is a folder that holds `pack.txt` and `motions\`. A superstar
mod can carry it, or it can be its own mod.

## 2. Check it: "Will it work in-game?"

The **Moves** page and the Port page's result both have a collapsed panel,
**Inspect and check move N**. It shows:

- **Will it work in-game? Yes / No.** No means the game will not use the
  move (it treats it as unusable) or the move breaks in a match. Each line is
  an ERROR or a warning with a **Fix:** hint.
- **What the move is.** The fields are explained in section 4.
- **Watch it.** Plays the move on the Animations page: both people, all
  phases and follow-ups, at match speed.
- **Export report...** Saves all of the above as a text file. Attach it to a
  bug report about a move.

The check uses the same rules as the game (`src/move_packs.cpp` MoveUsable).
A move is usable when two things hold:
- it has a move record with category bits;
- every body motion file it names is there.

The game skips an unusable move silently. A moveset slot that names one
falls back to the slot's stock move, and the CPU never picks it.

## 3. Common failures and fixes

| The check says | What you see in a match | Fix |
|---|---|---|
| no move record / record has no category bits | The move never happens. The slot does the stock move; the CPU never uses it. | Port again with a **Template** of the same kind, or add a `wazecopy <id> <stock id>` line to `pack.txt`. |
| motion file ... is missing | Same as above: the game counts the move as unusable. | Copy the pack's `motions\` folder in full (a shared or zipped pack often loses files), or port again. |
| no body motion (no y 0 / y 1 track) | Nothing to play. | Port again. A move needs at least the attacker's track. |
| phase x N has no partner motion after a paired phase | The opponent freezes or T-poses in the middle of the move. | Port again, or pick a template whose phases match the source move. |
| goes on to motion N, which the game doesn't have | The opponent stands up at once, or freezes at the end. | Port again. For a victim (y 1) track, its last event op 49 should name a down motion: 443-448 or 473-478. |
| track is 0 frames | Nothing plays for that track. | Port again. |
| over 1800 frames (warning) | The move runs very long. | Usually a wrong timing scale or the wrong motion: watch it. |
| attacker and partner lengths far apart (warning) | One person holds a pose while the other keeps moving. | Watch it. If it looks wrong, port again with a different template. |
| pack.txt frames differ from the motion's (warning) | Timing glitches, or a jump at the end of the move. | Port again so `pack.txt` and the files agree. |
| a kind this game doesn't read | The track doesn't play. | Port again with a template in the same bank kind (match moves are YMKs). |

**When the preview looks right but the match doesn't:**
1. Export the report.
2. Check that the superstar's moveset names the move's id (see "Used in" in
   the report).
3. Check that the mod is on.
4. Send the report with the game's log.

## 4. What each field means

**Record**
- `waze <bits>`: the pack carries its own record.
- `copy of <id>`: a `wazecopy` line copies a stock move's record.
- `the game's`: the id already has a record in `misc.pac`.
- `none`: no record.

A record needs category bits to be usable. 2011 kept the records of the
moves it cut but cleared their bits.

**Phases (x).** A move plays in steps:
- x 0 is often the run-up, played by the attacker alone;
- then the paired part, at x 10 / 20 / 50, or as 0 > 1 > 2 for chains;
- x of 100 and above are alternative endings, not steps.

**Tracks (x, y).** One motion per person or object in a phase:

| y | Who |
|---|---|
| 0 | The attacker. |
| 1 | The opponent (the partner). |
| 2-49 | Props and objects. |
| 50-52 | Cameras. |
| 60+ | Extras. |

Each track line shows:
- **frames:** the length that `pack.txt` gives. The game runs 60 frames a
  second; the preview runs 30 keys a second, so it plays at the same speed.
- **file:** the motion file in the pack. MISSING when it isn't there.
- **banks:** where the pack puts the motion in the game's files (`m.pac`
  `MOT/BM..`, `MOT/RR..`, `MVMT/<category>`, `mpsp.pac` `MOTP/GAME`).
- **keys:** the length as decoded. It should match frames.
- **then N:** the motion that follows when this track ends (its last event
  op 49). 0 or none means the game goes back to stance or the get-up. For
  a victim lying down this is normally a down motion.
- **hits key@group:** hit windows (events 87/94 open, 40/41 close). A strike
  connects only inside one.
- **events op x count:** the motion's events, counted. For example, 21 is
  hand poses and 49 is the follow-up.

**exh / evt / mbd.** The move's per-key rows in `misc.pac` `WAZA/DATA`:
- exh: 36-byte move data (damage, momentum, the reversal entry and similar);
- evt: sounds and effects at a key;
- mbd: body data.

Each row starts with the move id (u16, little-endian), then the phase. A
port copies these from its template, adjusted to the source move.

**In moveset.** The superstar mods (`Mods\Superstars\*\moves.txt`) whose
movesets name this move, and the slot each one uses.

**Source.** The pack's notes about the move and the porter's `report.json`:
the source game, the move id there, and the porter's choices.

## 5. For testers (hidden runs)

```
"SvR2011 Mod Maker.exe" --game <game folder> --behind --move-check <pack folder>,<move id>,<out.txt>
```

This writes the same report the panel exports, then quits. `--behind` keeps
the window off the screen.

`--watch <star>,<dummy>,<move>` with `--watch-strip <png>,<frames>` saves a
strip of preview frames, to compare with in-game frames.
