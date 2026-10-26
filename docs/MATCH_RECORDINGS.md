# Match recordings

Every match writes a folder of its own:

```
logs\matches\<YYYYMMDD-HHMMSS>_<people>\
```

The folder sits next to the log file, the same place `matchbox_N.txt` goes. `<people>` is the wrestlers' names
joined by `_vs_`, using letters, digits and `-` only, and cut at 80 characters. The newest
`match_recordings` folders are kept (cvar, group Logs: 20 by default, 0 turns recordings off). Older ones are
deleted when a new match starts.

The recording starts at the bell: the first world update after the clock resets, with a referee in the ring. The
menu's attract match has no referee, so it is not recorded. The files are written while the match runs, so a crash
leaves whatever was written up to that point. `done.txt` is written last, once the match's end task starts.

Code: `port/src/match_record.cpp` (`match_record.h`).

## Files

| File | What it holds |
|---|---|
| `match.json` | What the match was: enough to set it up again (below). `complete:false` until the end. |
| `inputs.bin` | Every pad's state as the game read it, one record each time a pad changes. |
| `box.txt` | The black box (`docs/LOG_DIAGNOSTICS.md`): each case as it is logged, then the full box at the end. |
| `trail.txt` | Once a second of the match clock (60 world updates) from the bell: each person's position and motion. |
| `loaded.txt` | At the bell: fallback memory, weapon slots (slot:model), the random table counters. At the end: fallback memory and its peak. |
| `mods.txt` | All installed mods, in load order, with on/off, used in this match, and a files hash. |
| `done.txt` | Empty. Written last: the folder is complete. |

A normal match comes to a few KB in total, well under 1 MB. In a 30 s CPU-vs-CPU test the folder was 8 KB,
with `inputs.bin` at 192 bytes.

## match.json

```json
{
  "format": 1,
  "complete": true,
  "game": {"version": "2.1.5", "commit": "87786a66 2026-10-25"},
  "start": "2026-10-26T18:47:03",
  "length_s": 34,
  "result": "ended",
  "rule": {"id": 0, "name": "SINGLE"},
  "arena": {"id": 1, "name": "RAW", "mod": null},
  "people": [
    {"index": 0, "person": 0, "name": "JOHN CENA", "superstar": 57, "cpu": false, "attire": 2, "mod": null, "caw": null}
  ],
  "frame_rate": 60,
  "online": false,
  "renderer": "any",
  "cpu_tab": {"reversal_chance": -1, "reversal_rates": -1, "damage_level": -1, "pin_level": -1, "momentum_level": -1},
  "weapons": {"picks": "mod:light_tube:3,4:1"},
  "inputs": {"file": "inputs.bin", "version": 0},
  "seeds": {"rand": "AC303B9C", "rand_sites": {"82302C40": 578, "82C2E2F4+": 85}, "table_load": [0, 0, 0, 0],
            "table_bell": [0, 0, 1, 60], "table_steps": [[], [87, 117, 147], [], [380, 385, 390]], "frames_load_to_bell": 857}
}
```

- `result` is one of:
  - `running`: the match is still going, or the game was closed during it;
  - `ended`: the match's end task started;
  - `crash`: the crash report was written while the match ran.
- `people` lists every character in the match, the referee (`person` 6) and the announcers included.
- `cpu` is true when the CPU plays the wrestler. That means the pick slot's controller is the CPU and no local
  player's pad plays them.
- `attire` is the picked attire, 1-4.
- `cpu` and `attire` are `null` for the referee and the announcers.
- `mod` is `null` for stock content. For a mod it is `{type, id, version, files}`, where `files` is the hash used
  in mods.txt.
- Rule and arena names are the game's own strings, in the game's language.
  - The rule's name comes from its rule record (+56, a string.pac string id).
  - A stock arena's name is string 0x9C42 + its id.
  - A mod arena uses the name from its manifest.
  - The fallbacks are `rule XX` and `arena N`. `port/server/match_names.py` has English names, tidied in places,
    for the dashboard.
- `cpu_tab` gives -1 for anything left at the game's own setting.

The launcher's report form (HCTP) and the server (Extras) read these keys. Tell both before renaming a key or
changing a separator.

## mods.txt

One line per mod, separated by tabs:

```
Type	id	version	on|off	used|-	files	name
```

- `files` is an FNV-1a 64-bit hash (16 hex digits). It covers the mod folder's files: the lower-case relative
  path with `/` separators, the size and the modification time, taken in path order.
  - Dot files are skipped.
  - Generated folders are skipped: `disabled`, ArenaOverlay, ArenaCrowdless, PacOverlay and ArenaTest.
  - It is a quick check for "the same mod files", not a content hash. The launcher computes its own SHA-256
    for uploads.
- `used` marks the match's superstar mods, its arena mod, and the weapon picks (`mod:<folder>`).

The list is built on a thread of its own, so a large Mods folder does not hold up the bell.

## inputs.bin (version 0)

All values are little-endian.

- Head, 16 bytes: `"SVRI"`, u32 version (0), u32 the frame at the bell (counted from the game's start), u32 0.
- Records, 22 bytes each:
  - u32: frames since the bell. A frame runs one world update, or two at 30 fps or on a slow PC;
  - u8: the user (0-7);
  - u8: 0 if the pad is connected, 1 if not;
  - 16 bytes: the XINPUT_STATE as the game got it (guest byte order).

  The packet number (the state's first 4 bytes) is written as 0. It goes up on every read, not only when the
  pad changes.

A record is written only when a pad's state changes. Every user gets one record at the bell. Extra world updates
at 30 fps or on slow frames reuse the cached state, so they add nothing.

## seeds

What an exact replay will need (see Replay below). Nothing reads it yet.

- `rand`: the match's rand() seed. From the match's load, every rand() call site draws from a stream of its own,
  seeded from this seed and the site, so the order threads call in doesn't change the draws. The streams start
  again from the seed at the bell.
- `rand_sites`: draws per call site since the bell. A `+` marks calls from a thread other than the logic thread
  (those sites have a second stream of their own).
- `table_load` and `table_bell`: the game's random table's four draw indexes (0x82E3689C) at the match's load and
  at the bell. Inlined draws all over the game advance them.
- `table_steps`: for each index, the frames after the bell of its first eight steps. Index 1 steps on a timer
  (about every 30 frames) that starts before the bell, so its phase shows here.
- `frames_load_to_bell`: frames from the match's load to the bell. The people load at disk speed, so this differs
  from run to run, and so does the pre-bell intro (a sneak attack or standing).

## The crash report

When the game crashes during a match, the crash report gets this line:

```
match recording: <folder> (unfinished: no done.txt)
```

match.json is rewritten with `result: "crash"`. The black box goes into the crash report as before.

## Replay (planned)

Version 0 records the inputs but cannot play them back yet. The plan:

- **Start state:** entrances are skipped by setup, and replay is anchored at the bell. A match that starts in a
  mid-brawl is marked as approximate.
- **rand():** the game's own rand() gets one state, with a recorded seed. The online lockstep's handling is
  reused.
- **Pads:** played back per world update from the bell.
- **Divergence report:** the replay is compared with `trail.txt`.

Planned switches: `SVR2011_REPLAY=<folder>` and an svrtest command.

Research switches already in the build:

| Switch | What it does |
|---|---|
| `SVR2011_TEST_SEED_LOG` | Logs each time the random table is seeded. |
| `SVR2011_TEST_RAND_LOG` | Logs who calls rand(), every 10 s. |
| `SVR2011_TEST_RAND_SEED=<n>` | Uses this rand() seed for every match. |
| `SVR2011_TEST_ENTRANCES=0\|1` | Turns the match's entrances off or on (live rules +38). |
| `SVR2011_TEST_MATCH=... exact=1` | Only the listed people: empties a later slot that the route's presses picked. |
| `SVR2011_TEST_TRAIL=<file>` | Writes a line per frame: the clock (`pre N` before the bell), the random table's indexes, then each person's position, motion and motion frame. |

## Testing

`SVR2011_TEST_BOX=crash,600` makes the game fault 10 s after the bell. The folder is left without done.txt,
match.json says `"result": "crash"`, and the crash report names the folder. This test aid now counts from the
bell, not from the entrances.
