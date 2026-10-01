# Unused match types - research

Status (2026-10-01): research done; several match types are now in the menus (see Implemented). Sources: the original game data (`Extract GameFiles`), the string table dump (`port/runs/strings_all.txt`), and the recompiled game code.

## Summary

Every match the game can play is a **rule record**. There are 119 (ids 0x00-0x76) in `pac/misc.pac`, chunk `RUL\0`. Each exhibition menu row points at one record by id. **36 ids have no menu row.** Several of them are complete, ordinary matches that the game's own code already handles, so they can likely be restored by adding a menu row, with no code changes:

| Restore | Ids | Effort | Confidence |
|---|---|---|---|
| **Falls Count Anywhere**: 1v1, tornado tag, triple threat, fatal-4-way | 0x2B, 0x2E, 0x2F, 0x30 | menu rows only | high: full records, code checks the ids, and the menu labels survive in the cut rows' slots |
| **Steel Cage "pin & give up"**: 1v1, triple threat, fatal-4-way | 0x40, 0x41, 0x42 | menu rows only | high: the records differ from the shipped cage only in one rule flag |
| **Royal Rumble 15-man and 25-man** | 0x15, 0x17 | menu rows only | high: the rumble-size code handles 10/15/20/25/30 |
| **Battle Royal "over the top rope"**: 4-man and 6-man | 0x24, 0x25 | menu rows only | high: own rule flag and code check |
| **Gauntlet, 5 people** | 0x58 | menu row | medium: used by story code too |
| **Special Referee** | 0x53 | menu row + character select for the referee slot | low-medium: untested |
| **Lumberjack** | 0x55 | menu row + checks of what story code sets up | low: looks Road to WrestleMania only |
| Single/tag **with manager** | 0x01, 0x02, 0x05, 0x06, 0x2C, 0x2D | menu rows + character select for manager slots | low: overlaps the "managers playable" work |

**Cut, with only names left (no rule record, so these would need new game logic):**
- I Quit
- Ultimate Submission
- Beat the Clock Gauntlet
- Iron Man Anywhere / Hell in a Cell / Table, and their triple-threat versions (Iron Man exists only as 1v1, 0x36)
- Diva Royal Rumble
- Hardcore (as a match), Hardcore Triple Threat Tornado
- Mask Pull-Off, Pipe Meter Challenge, Finisher Marathon, Grab Bag
- Parking Lot Brawl (the shipped "PARKING LOT" backstage area covers it)
- 2 Out of 3 Falls, Buried Alive (one title-card string only)

**No trace at all:** Ambulance, Casket, Punjabi Prison, Bra & Panties, Pillow Fight, Evening Gown, Fulfill Your Fantasy, King of the Ring, Street Fight, No Holds Barred (except a stray "NO DQ" string).

## How matches are defined

- **Menu.** `pac/menu/menu.pac` entry MFLO/0000 holds 0x74-byte records (see `port/tools/patch_menu.py`). Match leaves open screen 0x7D0 and carry the **rule id at +0x5C**. Other fields on match leaves: +0x58 = 2, +0x60 = 1, +0x6C = 0x2710, +0x70 = 2.
- **From the menu row to the match** (code):
  1. `sub_8243FCC8` (0x8243FD48) turns the row into group = 2000 + id.
  2. `sub_824402A0` turns the group back into the id. Match Creator screens 1330-1335 map to 0, 3, 0xD, 0xE, 0x23, 0xF; anything else gives 119 = none.
  3. `sub_8245B948` passes the id to `sub_82737CE0`, which calls `sub_827374A0(matchObj = *0x82EDE630, id)`.
  4. That reads the rule record with `sub_828B4A48(id)` = `*(0x82EDE954) + id*100`, and a second 64-byte record (resource type 9) with `sub_828B4B70`.
  5. Live match settings are at 0x82E3DE00 (`sub_825740C8`); byte 0 is the rule id.
  6. The in-match name is string 0x6B08 + id (`sub_824990E8`).
- **Rule record** (misc.pac at 0x9A30: `RUL\0`, u32 LE count 0x77, then 100-byte LE records from 0x9A38; an identical copy at 0x415A30; loaded by `sub_828B5108` / `sub_828B4E78`):

| Offset | Meaning |
|---|---|
| +0 | u16 number of participants |
| +2..+19 | six slots of 3 bytes: index, team, kind. Kind 0 = wrestler, 1 = special referee (only 0x53), 2 = manager, 3 = waiting gauntlet entrant |
| +36 | 1 normal, 4 special (backstage / rumble / chamber / ...); a guess |
| +40 | forced arena (bgNN), 0 = free: rumble 22, chamber 20, backstage 78, 0x56 = 19 |
| +28 / +32 / +44 / +48 | category, sub-name, menu family and label string ids |
| +56 | full name string id (0x9EEF list) |
| +60 | internal name string id (0x52D0 list) |
| +64..+99 | 36 rule flag bytes, partly decoded: f7 cage, f8 over the top rope, f11 timed, f14 entrant count. Pin & give-up cage differs from the normal cage only in f5. |

- **Rule predicates** in code (0x828B3E88-0x828B4E30). Each tests which ids belong to a family; this is how the game knows a given id is a cage, rumble and so on:
  - rumble 0x14-0x18 and 0x56, with rumble size from `4AC8`;
  - Elimination Chamber 0x27/0x28;
  - gauntlet 0x52/0x58;
  - Extreme Rules 0x4D-0x50;
  - First Blood 0x29/0x2A;
  - Hell in a Cell 0x31-0x35;
  - **cage 0x3C-0x42** (includes pin & give-up);
  - Table 0x44-0x47; TLC 0x48-0x4B; Ladder 0x37-0x3A;
  - battle royal 0x22-0x25, with **over the top rope 0x24/0x25** (`48E0`);
  - **Falls Count Anywhere 0x2B-0x30** (`4880` / `4D10`);
  - manager 1, 2, 5, 6, 7, 0x2C, 0x2D (`4970`);
  - backstage and parking lot / GM office / in-ring brawl families.
- **Direct id checks elsewhere:**
  - Falls Count Anywhere ids: `sub_8275E600`, `8275E718`, `8275E970`, `8275E788`, `8275E878`, `8220CBC8`, `82225C68`.
  - 0x42: `82225C68`.
  - 0x53 Special Referee: `sub_8260E150` (a participant-role-1 path).
  - 0x55 Lumberjack: `sub_82613DA0`, `82614298`, `825EEA80`, `82225D68`, `825EAC18`.
  - 0x57/0x58: `822BB1E8`, `828B4D50`, `82301148`.
- **Road to WrestleMania** scenario dispatcher `sub_822C3D08` runs in game mode 7 (+10416; RTWM sets it in `sub_82368A90`). It switches on ids 0x54, 0x55, 0x57, 0x59, 0x5A, 0x5C, 0x5D and 0x61, so those are story matches.

## All rule ids without a menu row

From the rule table itself. Names are its full name and internal name.

| Id | Record | Participants (slot kinds) | Assessment |
|---|---|---|---|
| 01 | SINGLE w/MANAGER: no manager vs w/manager | 3 (0,0,2) | manager slot; character select would have to offer managers |
| 02 | SINGLE w/MANAGER: w/manager vs w/manager | 4 (0,0,2,2) | as 01 |
| 05 | TAG TEAM w/MANAGER: no manager vs w/manager | 5 (..2) | as 01 |
| 06 | TAG TEAM w/MANAGER: w/manager vs w/manager | 6 (..2,2) | as 01 |
| 15 | 15-MAN ROYAL RUMBLE (arena 22) | 6 | **easy**: a row in ROYAL RUMBLE |
| 17 | 25-MAN ROYAL RUMBLE (arena 22) | 6 | **easy** |
| 19 | BACKSTAGE (generic, 1v1, arena 78) | 2 | the old "pick the area in game" backstage; the 7 named areas replaced it |
| 1A | BACKSTAGE / LOCKER ROOM BRAWL (2v2) | 4 | as 19 |
| 24 | 4-MAN BATTLE ROYAL OVER THE TOP ROPE | 4 | **easy**: a row in FATAL-4-WAY |
| 25 | 6-MAN BATTLE ROYAL OVER THE TOP ROPE | 6 | **easy**: a row in 6-MAN |
| 2B | SINGLE FALLS COUNT ANYWHERE | 2 | **easy**: the 1v1 row (label A054 is still in the cut slot) |
| 2C | FCA no manager vs w/manager | 3 (..2) | manager |
| 2D | FCA w/manager vs w/manager | 4 (..2,2) | manager |
| 2E | FALLS COUNT ANYWHERE TORNADO TAG | 4 | **easy**: a TWO ON TWO row (label A062) |
| 2F | TRIPLE THREAT FALLS COUNT ANYWHERE | 3 | **easy** (label A06B) |
| 30 | FATAL-4-WAY FALLS COUNT ANYWHERE | 4 | **easy** (label A073) |
| 40 | STEEL CAGE PIN & GIVE UP | 2 | **easy**: shipped cage plus pin/submission wins |
| 41 | STEEL CAGE TRIPLE THREAT PIN & GIVE UP | 3 | **easy** |
| 42 | STEEL CAGE FATAL-4-WAY PIN & GIVE UP | 4 | **easy** (`82225C68` also checks it) |
| 53 | SPECIAL REFEREE | 3 (0,0,**1**) | the third person is a playable referee; needs character select to fill a kind-1 slot; in-match code exists (`8260E150`). Medium-high risk |
| 54 | OFFICE STAGE brawl (arena 78) | 3 | story (RTWM dispatcher) |
| 55 | LUMBER JACK MATCH | 6, teams 0, 1, 2, 3, 3, 3 | 2 wrestlers + 4 lumberjacks; story code sets it up; medium-high to bring to exhibition |
| 56 | 15-MAN ROYAL RUMBLE, arena **19** (the Druid arena) | 6 | story copy of 0x15 |
| 57 | SINGLE / NORMAL TAG TEAM | 5 (0,0,2,2,3) | story |
| 58 | GAUNTLET MATCH | 5 (0,0,3,3,0) | a 5-person gauntlet (the shipped 0x52 has 4); worth a try |
| 59 | HANDICAP THE DIRT SHEET BRAWL | 3 | story |
| 5A, 5D, 5E | THE DIRT SHEET BRAWL | 2-3 | story |
| 5B | TORNADO TAG TEAM (copy) | 4 | story copy |
| 5C | ARMAGEDDON HELL IN A CELL (copy) | 6 | story copy |
| 5F | SINGLE STEEL CAGE with 3 people | 3 | story |
| 60 | TAG TEAM (copy) | 4 | story copy |
| 61, 62 | PARKING LOT / GM OFFICE OFFICE-STAGE brawls | 2-3 | story |
| 63-68 | IN RING BRAWL 1v1, 2v2, 3v3, 1v2, 1v3, 2v3 | 2-6 | in-ring brawls (strings A4D8-A4DD); probably story or Universe (start a fight in the ring); untested |

Menu labels from cut rows that still sit in their slots:
- A054, A062, A06B, A073: Falls Count Anywhere in 1v1, 2v2, triple threat and fatal-4-way.
- A080 MONEY IN THE BANK: the 6-man MITB row ships labelled "LADDER" (A09C).
- A088-A08B and A0A9-A0AC: older backstage areas.
- Iron Man duration submenus: A08F-A091, A0A6-A0A8, A0CD.
- Royal Rumble size submenu A0CC.

## Arenas and objects (data)

The arenas are `pac/bg/bgNN.pac`; arena number N is string 0x9C42 + N.

- **Shipped and selectable:** bg00-bg18 (12 of them need unlocking).
- **Automatic arenas:** bg20 (Elimination Chamber), bg22 (Royal Rumble), bg78 (backstage, including the parking lot with cars).
- **Story rooms:** bg66, bg67, bg76/77/79, bg80.
- **Create mode:** bg23 (create-moves ring); bg29 (practice arena).
- **Not offered:**
  - **bg19 "DRUID ARENA"** (the Undertaker story arena): complete, and it has a menu thumbnail `arena_drui`, but there's no unlock string. Rule 0x56 (story 15-man rumble) forces it. A good candidate for the arena list.
  - **bg24** (Superstars-style arena, no crowd textures) and **bg26** (a RAW test copy): not in the game's file list, but both copies of stages shipped in bg25.pac.
  - **bg25** "WWE APPROVAL": a test bundle that also holds an old SmackDown-era crowd-brawl stage (STG/0025).
- **Match structures** (bgEtc.pac):
  - Steel cage (CAGE).
  - Two Hell in a Cell structures:
    - the old breakable cell (CELL, with breakable fence panels);
    - the new larger cell (CELO), plus a CELO copy without collision (CELT).
  - Elimination Chamber with pods (CHAI).
  - MITB briefcase and ladder-championship hanging belt (LADM / LADC), plus 14 hanging-prize sets (STGL).
  - Weapons, including **parking-lot tires** `plb_tires07-09`. The weapon parameter table also lists a bumper, tire iron, bat, pool cue, TVs and a pillow, some marked for crowd brawls.
- **Not present:** ambulance, casket, Punjabi Prison, Buried Alive structures, lumberjack-specific props.

## How a restore would be done (when asked)

- **Menu rows.** `port/tools/patch_menu.py` and `src/game_files.cpp` already add rows to MFLO/0000 (GRAPHICS, EXIT, ACHIEVEMENTS). A match row is a copy of a sibling match leaf with +0x5C set to the new id and its own label.
  - **The MFLO table's slot is full.** Each added row must replace a hidden record (8 exist; 3 are already used) or the table must be moved.
  - For more than about 5 rows, a port-side hook that adds rows in memory is better than patching the file.
- **Labels.** Where the cut label still exists (A054, A062, A06B, A073, A080) use it; otherwise add a port string, as menu_hooks.cpp does for GRAPHICS.
- **Test each restored id in game:**
  - win conditions (FCA pins outside the ring, cage pin/submission, rumble count, over-the-top eliminations);
  - the in-match name;
  - the results screen;
  - Universe and records, which store match ids.
- **Special Referee, Lumberjack and manager matches** first need character-select work (slot kinds 1 and 2), which overlaps the "managers playable" task.

## Character-select pointers (for the roster-limits work)

- **Rule record slot kinds** (+2..+19): 0 wrestler, 1 referee, 2 manager, 3 waiting entrant.
- **Flag byte f1** (+65: 3, 6 or 9) may be a roster or gender filter (unverified). Mixed Tag has f0 = 0x0B.
- `sub_828B4970`: the manager-rule predicate.
- `sub_827374A0` (0x82737768-0x827377C8): counts manager slots and subtracts them from the participant count.
- **Participant role** +2624 is set from the slot data by `sub_8257C290`, called at 0x825BE550 and 0x825BE608 (`sub_825BE3F8`).
- `sub_8260E150` (0x8260E1F0): role 1 together with rule 0x53 (Special Referee).

## Open questions

- The meaning of most rule flag bytes and of the 64-byte second record (resource type 9).
- Whether Universe or Story Designer can already pick the FCA / cage pin & give-up ids: script natives `sub_82BB2D80` / `sub_82BB2E08` read family lists that include FCA 0x2B, 0x2E, 0x2F, 0x30.
- The in-ring brawl ids 0x63-0x68: who uses them.
- Whether the Druid arena works as an exhibition arena.

## Implemented (`port/src/match_types.cpp`)

**Menu rows are added at runtime**, not in menu.pac:
- `sub_82BAA6E8(menus, table, size)` parses MFLO/0000. Its caller frees the table afterwards.
- It reads only header +4 (total) and +8 (shown), copies every shown record into 76-byte entries, and builds its group index from the table. All of that is sized dynamically.
- The hook hands it a copy with rows inserted after their anchors and both counts raised. No file-size limit applies, and patch_menu.py's hidden-record trick isn't needed for these rows.
- A submenu finds its rows by (depth, parent node, +0x38). New groups and nodes get ids above the existing ones, and their rows go at the end of the table.

**Rows added** (all tested in game with test saves):

| Menu | Row | Rule | Notes |
|---|---|---|---|
| ONE ON ONE | FALLS COUNT ANYWHERE | 0x2B | the cut row's own label A054 |
| TWO ON TWO | FALLS COUNT ANYWHERE TORNADO TAG | 0x2E | label A062 |
| TRIPLE THREAT | FALLS COUNT ANYWHERE | 0x2F | label A06B |
| FATAL-4-WAY | FALLS COUNT ANYWHERE | 0x30 | label A073; played (4 picks, match card, match) |
| ROYAL RUMBLE | 15-MAN, 25-MAN | 0x15, 0x17 | labels 5313/5314; the select screen opens (play-through not tested) |
| 6-MAN | LUMBERJACK | 0x55 | see below |
| TRIPLE THREAT / FATAL-4-WAY / 6-MAN | BACKSTAGE > the 7 areas | 0x70-0x76, reshaped | see below |

**Backstage with 3, 4 or 6 people.** The area comes from the rule id; 0x70-0x76 are the 2v2 areas.
- `sub_8243FCC8(?, group, row)` returns 2000 + rule. The hook notes when the row is in one of the new submenus' groups.
- `sub_827374A0(match, rule)` sets the match up (it runs as the row is picked, before character select). There the rule's record is reshaped like normal triple threat (0x0D), fatal-4-way (0x0E) or 6-man battle royal (0x23). The fields copied:
  - of the 100-byte record: +0..+27 (people, start places and teams, +20, +24), **+44, +52 and +64**, which choose the select screen;
  - **+35 of the 64-byte option record**, the people count. (The earlier note said +43; the file shows +35.)
- Setting up any other match restores the record.
- Played: a triple threat in the GM office and a 6-man match in the GM office.

**Lumberjack (0x55).**
- Its record keeps its 6 people: 3 fighters (teams 0, 1, 2) and 3 lumberjacks (team 3).
- Its select fields (+24, +36, +44, +52, +64) come from 0x23, giving the 6-person select screen. Without this, select shows 2 slots and crashes on the second pick.
- The story script (`sub_822C3D08` -> `sub_822BB158`, story mode only) marks lumberjacks for the AI (character +2572 -> +168 = 1). The port does the same from `sub_82225D68` (the per-character competitor check, which runs in every match).
- Played: the lumberjacks stay outside the ring while the others fight.

**Test aid.** `SVR2011_TEST_RULE=<hex>` plays that rule from ONE ON ONE -> NORMAL.

## 50-man Royal Rumble: not done

The rumble size comes from `sub_828B4AC8`, a hard-coded table (0x14 -> 10, 0x15 -> 15, 0x16 -> 20, 0x17 -> 25, 0x18 -> 30, 0x56 -> 15), not from the record. 30 is built into the engine:
- the entrant list u32[30] (match object +13544);
- **30 in-match character objects** of 2116 bytes each (match state +424..+63904), with about 40 accessors;
- rumble tables of 30 entries;
- a 32-bit entrant mask in `sub_82735A90`;
- live settings u32[30].

A 50-man rumble would need "slot recycling": reload an eliminated entrant's character slot with entrant 31 onwards, and track numbers and eliminations on the port side. That is a large, risky change (high effort). The 15- and 25-man sizes were added instead.

## Whole backstage as an arena (done)

**Menu.** FREE-ROAMING BACKSTAGE (label 0x9C90) is the last row of every BACKSTAGE list:
- 1v1: rule 0x19;
- 2v2: rule 0x1A;
- triple threat / fatal-4-way / 6-man: rule 0x1A, reshaped for 3, 4 or 6 people.

**Rooms.** bg78 loads every room. The 'CFRS' task (`sub_825310B8`, every frame) hides all but the corridor, the interview set and the one room at the camera position, which shows black through doorways.
- The hook shows rooms 0, 2-9 after the update (`sub_8252FFF0(task, room, 1)`) for rules 0x19/0x1A outside story mode.
- It keeps the objects the game always hides hidden (`sub_8252FF40`).

**"Is backstage".** `sub_821852E0` deliberately says no for 0x19, so the match got the ring camera and the AI stood still. The hook returns yes for 0x19/0x1A outside story mode. That gives:
- the backstage camera (`sub_82313D98` makes it);
- a working AI;
- the fight box.

**Fight box.** A backstage match keeps its fighters in a box set up by `sub_8224EF28(box)` per area: centre +32/+36/+40, half sizes +64..+76, also copied to 0x82D9E9D4..E0 for the clamp `sub_821E5408` and the AI.
- The whole backstage has no area there, so the box stayed a point. Fighters were pinned at the origin, and grapple pairs shot out of the level.
- The hook gives it a box around all of the backstage: centre (-10, 0, -40), half sizes 520/510/525/515. The backstage's own wall mesh (bg78 HMD 0x3E4, the whole outline) then keeps everyone in, as in story mode.

**Rule records.** Rules 0x19/0x1A get the parking lot's records (0x1B / 0x70) apart from their names. That was tried for the AI and kept; the "is backstage" hook was what fixed it.

**Camera.**
- **Distance:** the backstage camera's distance (`sub_822F26B8`: camera+932 = radius x 3.7) is x 2.4/3.7 in arena 78, so closer.
- **Framing** (`sub_82227690`, a sphere around all competitors): with more than 2 people, it becomes the average position of the fighters within 220 of the player (the first competitor), with radius at most 90. The camera stays on the player's fight; a fight elsewhere is left out.

**Tested:**
- 1v1: the CPU fights.
- 6-man: everyone stays inside; the camera stays close on the player's fight.

**Known:**
- Wall collision is skipped during grapples in every area (`sub_821E5910`), so a grapple at a wall could still push a pair through it. Not seen in testing.
- The camera doesn't avoid walls, so a wall can come between it and the fighters.
- Some areas behind the fighters render dark or black. These look like unlit room shells seen from outside.
