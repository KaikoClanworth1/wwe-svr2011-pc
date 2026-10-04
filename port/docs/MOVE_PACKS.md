# Move packs (arenas branch)

Moves the game doesn't have, added by mods. The first pack is the 31 SvR 2010
moves Jeff Hardy needs, including Swanton Bomb 1, Extreme Twist of Fate and
Extreme Neckbreaker Drop. The game's own files are never written.

- **Game:** `src/move_packs.cpp`.
- **Making a pack:**
  - `tools/svr10_moves.py` (ports SvR 2010 moves).
  - `tools/movepack.py` (writes the folder a mod carries).
- **Research:** scratchpad `svr10/re_moveport.md`.

## What a move is

- **Motions:** YMKs banks keyed (id, x = phase or variant, y = track).
  - Tracks: 0 is the attacker, 1 the victim; other tracks are props and
    cameras. The victim's reaction lives under the same move id.
  - Where the same key is stored:
    - `m.pac`: `MOT/BM..` (the whole set), `MOT/RR..` (the per-wrestler
      match bank is gathered from these), `MVMT/<category>` (menus and the
      moveset preview), `RU/RURR` (rumble).
    - `mpsp.pac` `MOTP/GAME`: the banks every match loads for everyone.
  - A bank: "YMKs", a 256-byte event-size table at 0x10, u32 count at
    0x110, then entries {u8 y, u8 x, u16 id, u32 offset, u32 frames, u32
    runtime}, sorted by key. The motion data follows in directory order; an
    entry's size is the next offset minus its own.
- **`misc.pac` `MOVS/WAZE`:** 160-byte move records (id at +0x90). The first
  16 bytes are category bits that decide which moveset slots a move may go
  in. 2011 kept the records of the moves it cut but cleared those bits.
- **`misc.pac` `WAZA/DATA`:** children 0 (EXH: 36-byte per-key move data),
  1 (per-key sound events) and 2 (MBD).
  - Each child is a PACH of groups, sorted by (u16 id, u8, u8).
  - `BATS/INIT` and `BATH/INIT` hold the same three as children 10-12.

2010 data needs one conversion: event op 87 has bit 7 set in 2010 and never
in 2011. `svr10_moves.py` clears it. The MOTP copies are carried over
unchanged.

## A pack

`pack.txt`, one item per line:

```
motion <m.pac|mpsp.pac> <TYPE/NAME/child/...> <id> <x> <y> <frames> <file>
waze <id> <16 bytes hex>            the move's category bits
exh <group> <36 bytes hex>
evt <group> <16 bytes hex> <events hex>
mbd <group> <8 bytes hex>
```

The motion files go in `motions/`.

Where a pack goes:
- in a superstar mod: `Mods/Superstars/<mod>/moves/`
- on its own: `Mods/Moves/<pack>/`

A `disabled` file in the mod or pack folder turns it off. Keys the game
already has are left alone.

## How the game uses them

1. **At start-up** (`InstallMovePacks`, before the game mounts its pacs):
   - Every enabled pack is merged into copies of `m.pac`, `misc.pac` and
     `mpsp.pac` in `<game>/Mods/PacOverlay`.
   - Unchanged entries are streamed from the originals. Changed ones are
     rebuilt and compressed for real (BPE).
   - `stamp.txt` records the packs and the game files. The merge runs again
     only when one of them changes; it takes about 3 s.
   - With no packs the overlay files are deleted.
2. **The pac list:** the overlay holds copies of `pac/plist360.h` and
   `plist360_4x3.h` that name the merged pacs (`mods\pacoverlay\m.pac`).
   The game reads its pac list from there (`sub_826B9280`, `arena_mods.cpp`).
3. **Mounting:**
   - The game normally finds pac entries through the pre-built directory
     `plist360.arc`, a copy of every listed pac's table. It only knows the
     original files.
   - Grown entries make loads fail: a crash at start-up, or a hang while a
     match loads.
   - With an overlay the game instead mounts the pacs by reading each one's
     own table: `sub_825953B0` in place of `sub_82595428`
     (`superstar_mods.cpp`).
   - `tools/plist_fix.py` can refresh an arc instead, for tests on patched
     files.

## Tests

- **`SVR2011_TEST_MOVE_LOG=<id>,...`:** logs the motion lookups
  (`sub_823941F8`) of those move ids, found or not, and failed allocations.
  - Lookups on a loading thread at match start only check the global
    banks.
  - Lookups on the match thread are the moves actually played.
  - Lookups of tracks 50-100 that aren't found are the game probing
    optional tracks.
- **`SVR2011_TEST_PLIST=<folder>`:** use a pac list from that folder (an
  overlay made by hand) instead of merging.
- **`superstar_test.ps1 -Com -ComDown 5 -Down 4`:** the computer plays the
  Jeff Hardy mod (list entry 5) against The Hurricane.

Verified (2026-10-04), against the computer as Jeff Hardy, with the game's
files untouched:
- The overlay merged by the game is byte-identical to the
  `svr10_moves.py` build.
- Ported moves play in matches: Snap Jab 2 (3088), ECW Feint Wheel Kick
  (3274), and Hurricanrana 8 (7573, both tracks).

Not verified:
- How the ported motions look move by move.
- The moveset lists in Create-A-Moveset.
- The Royal Rumble move (8414).
