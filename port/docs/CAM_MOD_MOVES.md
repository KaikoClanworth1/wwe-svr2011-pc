# CREATE A MOVE-SET: mod moves

Moves that move packs add (Mods\Moves\<pack>, a superstar mod's moves\) can be put in any superstar's
or CAW's moveset like the game's own. Code: `src/cam_mod_moves.cpp`, plus `src/move_packs.cpp`
(PackMove, CategoryDefaultMove, MoveMissing, LoadMoveTables).

## What the player sees

- Every move list ends with the packs' moves, each row named `MOD: <name>` (CREATE A MOVE-SET's 26
  tab lists, re-ordered after the game fills them, sub_82378A68). Only the list rows carry the
  prefix: the move record's own name - the one the match HUD shows - stays as it is (a superstar
  mod's ported finisher is "Mandible Claw", not "MOD: Mandible Claw").
- Picking, previewing and saving work as for any move: the moveset keeps the move's id.

## A pack removed later

- CREATE A MOVE-SET keeps the id (the row shows nothing until the pack is back).
- Saving a moveset whose tag-team finisher is such a move no longer crashes (sub_8237E9E0 read the
  finisher's record without a check; sub_8237D4C8 gets record 0 for that caller).
- In a match the slot plays its category's default move instead (first game move of the category
  the game always shows), with a log line `cam mods: slot S move M has no record (pack removed?) - D
  in this match`. Only the match changes: with the pack back, the move is back.
- "Missing" is narrow on purpose: a move a pack gave once (ids kept in
  `Mods\PacOverlay\pack_moves.txt`), no pack gives now, and the stock game can't play (no stock
  record with category bits, no stock EXH data). Stock movesets name moves without records, bits or
  EXH data that play fine, so nothing wider is safe. Profiles from another PC with that PC's pack
  ids are not covered.

## How it works

- Move records: misc.pac MOVS/WAZE (160 bytes: +0 / +8 category bits, +0x10 name, +0x78 unlock,
  +0x90 id). The menu's move table (*(0x82E3C214): +60 records, +72 + cat * 4 category lists, +840 +
  list * 4 the 26 CAM tab lists) exists only in menus - a match has none, so the match side uses
  tables `LoadMoveTables` reads from misc.pac at start-up (and refreshes at each MOVS/WAZE load).
- CAM list rows: sub_82BA1AC8(page, tab, first row) fills 13 rows; row element
  `*(*(page + 620) + 8) + ((row + *(page + 1112)) << 8)` keeps the name pointer at +216 - a pack
  move's row gets a persistent `MOD: <name>` string.
- A match asks a slot's move through sub_8218E600(fighter, slot) (88 callers) and sub_8218E580; both
  hooked. Slot -> category: sub_8237A430 (table `kSlotCategory`, 155 CAM slots).

## Tested (2026-10-13/14)

- CAM -> Alicia Fox -> SPECIAL MOVES -> SIGNATURES -> ALL: ends with MOD: HURRICANRANA 9 ... MOD: SS
  CAMEL CLUTCH ... MOD: SNAPMARE & DROPKICK 2 (65 pack moves grouped).
- A normal match: no move replaced. Test aid `SVR2011_TEST_MISSING_MOVE=all:28888` (a move id with no
  record in every slot): 64 replaced, the match plays. Sabu's move pack hidden: his 5 pack moves
  replaced (slot 81 move 3305 -> 5360, ...), the match plays, no crash.
- Test aids: `SVR2011_TEST_MISSING_MOVE=<slot>:<id>` / `all:<id>`, `SVR2011_TEST_MOVE_LOG=1` (the first
  200 slot / move pairs a match asks for).
