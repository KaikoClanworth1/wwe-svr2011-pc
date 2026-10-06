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

## Lumberjack: a custom version (research, 2026-10-03)

**What a real Lumberjack match needs:** two wrestlers in the ring, only they can win. The lumberjacks stand around the ring on the floor. A wrestler who leaves the ring is attacked by them and thrown back in, and there are no count-outs.

**What the game's own Lumberjack (rule 0x55, Road to WrestleMania only) is:**
- **Record:** 6 people, teams 0, 1, 2, 3, 3, 3. That's a three-way fight with three lumberjacks on team 3. The "is a competitor" check `sub_82225D68` says no for team 3 in rule 0x55, and `sub_825EEA80` says no for people 3 and up.
- **Start places:** the placement table (below) puts the three lumberjacks on the floor around the ring.
- **Behaviour:** the story script `sub_822BB158` only sets one AI value on team 3 (character +2572 -> +168 = 1). It does not keep them outside. In test, all four lumberjacks walked into the ring about 3 s after the start and brawled. In the first test, with the 6-man select layout, a team that included lumberjacks won by pinfall. The engine has no lumberjack AI to switch on.

**Start places (done, see match_types.cpp):** `sub_822AD738(placer, rule, ?)` places people from a table at placer+4.
- **Entry layout:** 88 bytes per rule id. +0 is the id, then 6 people x 12 bytes: kind (1 = wrestler, 2 = manager), then int16 x, y, z and facing, in tenths. y is -120 in the ring and 0 on the floor.
- **Last record:** kind 6, the referee.
- **Examples:** managers (rule 02) stand at (400, 0, 110). Tag partners (03) use kind 1 with a value 0x1B62 (the apron) at (220, -120, 227).

**Now:** 2 wrestlers and 4 lumberjacks (person 2 is moved to team 3 and to the floor at (500, 0, 0)), all four starting on the floor, one per side. They still walk in and fight.

**Manager slots (tried):** making people 2-5 kind 2 (managers) changes the select screen to 2 picks (the match card still says LUMBER JACK MATCH). But exhibition never fills manager slots, so the match was a plain one on one. Using them would mean filling the slots ourselves: random superstars, or picks. The people list is not in the live settings at 0x82E3DE00, which only holds per-slot flags at +0xFC..+0x101.

**Custom version - done (2026-10-04, match_types.cpp):**
1. **Who:** you pick 2 wrestlers (the select fields of rule 0x00), and 4 random lumberjacks are chosen.
   - They are selectable, not DLC, of the picks' gender, and not a pick under another id (same name or same person, +228).
   - They are written into the live match's slots (*0x82EDEB48 +432 + i*2116: +8 id*100+attire, which MUST be attire 2; +54 id; +5 team 3; +4 kind 2; -8 CPU) as `sub_828BBEF0` builds the people.
2. **At most 4:** `sub_828BBEF0` takes exactly 6 people from the slots (an unrolled check of 6 slot blocks), then the referee (person 6) and the commentators (7-8). Raising the record's people count is ignored. Raising it before the select screen crashes its set-up (`sub_827374A0` reads 6 slots). 6-8 lumberjacks would mean redoing the people builder and the per-person tables.
3. **Staying outside:** as managers (kind 2) they keep to posts on the floor about 55 out, one per side. As wrestlers (kind 0) they start in the ring and brawl. With entrances off, wrestlers get the in-ring start places.
   - Neither role (+2624: 0 wrestler, 2 manager), the AI mark (+168), the info kind (+12) nor the target (+2224, a character pointer) makes a manager fight during a match. The AI kind is fixed when the character is made.
   - Moving a character by writing +288/+272 or the body does not stick.
4. **Going for a wrestler:** a per-update controller.
   - A wrestler is on the floor when y > -6 outside +-30 (in the ring y = -12; +444 byte: 0 ring, 1 outside).
   - After half a second there, the nearest lumberjacks within 20 (two at most) each start the paired move 1000 with him once, via `sub_82191AA8(fighter, opponent, 1000)` from the fighter control hook `sub_82216C58`. Back in the ring, he can be punished again on his next trip out.
   - Not done: a "throw him back in" move. The candidates checked (804/806/901) are running attacks.
5. **No count-outs:** the rule's 64-byte record (misc.pac OPT, +8 + id*64; bit 7 = locked) has byte 5 = count out (normal 0x0A, Lumberjack/FCA/ER/cage 0x80 = locked off). In a match the live byte 5 is 0.
6. **Can't win:** managers are not competitors (`sub_82225D68`, `sub_825EEA80` say no for team 3 / people 2+).
7. **Referee ejection:** string.pac has the referee motions REFEREE WARNING / GET DOWN / EJECT MANAGER (0x3220, a motion name) and "MANAGER EJECTED" (0x8D2, a message). No ejection was seen in the tests.

## Weapons everywhere (done)

A No DQ match with weapons already lying in and around the ring at the bell.

**Where the game's placed weapons come from:**
- The table is bgEtc.pac `STG/WPON` (`STG/WPBS` backstage) entry 45010, loaded by `sub_8227D938`. It is kept at `*(0x82E3BE98)`:
  - +4 s16 count;
  - +8 -> 8-byte heads {s16 key, n, link, pad};
  - +12 -> {int count; record*} per key.
- The key is the rule id. The link (1000-1002) is the shared ringside set: announce tables, steps, bell, belt.
- Each record is 28 bytes: f32 position, f32 rotation (degrees), s16 motion (15010 lying, 15000 standing, 15305 stacked), u8 place (0 ring, 1 floor), u8 model.
- Models: 4 chair, 5 table (every other one becomes 9), 11 ladder, 63 trash can, 89 guitar, 60 crutch, 90 mop, 17 tire, 112 barbell.
- `sub_8227B938` makes the weapons as the match loads; `sub_8227D130` (from match init `sub_822ADE80`) places them at the start and stows the rest under the ring.
- Models must be in the per-match load list `*(0x82E3C0E8)` (built by `sub_82301148`), or the weapon silently doesn't appear.
- Extreme Rules (0x4D-0x50, live option +56) loads 87, 15, 60, 89, 11, 88, 5, 63 and 90 (plus chair 4 in every match), but has no records.

**What the port does (`match_types.cpp`):**
- A WEAPONS EVERYWHERE row follows each EXTREME RULES row.
- ONE ON ONE's list is full: a list shows at most 14 rows, so a 15th row isn't displayed. There, EXTREME RULES becomes a submenu holding EXTREME RULES and WEAPONS EVERYWHERE.
  - A submenu's rows need depth +0x16 = the parent's + 1.
  - +0x04 is the description string; +0x64 is not.
- For that match, the rule's {count, records} entry points at the port's 8 records while `sub_8227B938` and `sub_8227D130` run, and is put back afterwards:
  - 2 chairs in the ring;
  - 2 tables by the apron;
  - a ladder;
  - a trash can;
  - a guitar;
  - a chair on the floor.
- Tested: 1 on 1 (0x4D) and triple threat (0x4F), with screenshots of the weapons in place.

## Match Creator / option locks (research, 2026-10-05; not done yet)

**OPT record (64 bytes, `*(0x82EDE954)+11900+id*64`, misc.pac OPT at 0xC8B4):**
- Bit 7 = locked; bits 0-6 = the value (forced when locked).
- Bytes:
  - 1 pinfall; 2 KO; 3 rope break; 4 DQ off; 5 count-out; 6 over the top rope; 7 give up; 8 minutes; 9 cage bits; 12 iron man falls;
  - 20 Hell in a Cell; 21 timed; 26 last man standing; 27 ring out; 32 interference (forced 0 offline); 33 outside allowed (guess);
  - **34 people count** (+35 is a free-for-all flag); 36 tornado; 37 elimination; 38 entrance; 40 first blood; 41 chamber; 44 escape door; 56 extreme rules; 58 tag; 59 inferno.
- The rules screen's rows come from `sub_82470BC0`: row disabled if its OPT byte has bit 7.
- The live settings come from `sub_828C48E8`: the user's value is used only where unlocked.

**Match Creator steps:**
- misc.pac `/MRME/MRPD` (PACH at 0x932000, loader `sub_82490558`): per-family "allowed" bytes. 0x09 free, 0x00 disabled. Example: tag allows only the standard ring.
- Families come from `sub_82490918`.

**Arena:**
- `sub_828B5558` with RUL +36: 0 any, 1 flagged arenas, 2 none, 3 arena 20, 4 = RUL +40.

## Slobber Knocker (done, a4ae58f)

**Menu:** HANDICAP -> SLOBBER KNOCKER (after GAUNTLET, group 0x0B).
- Uses the gauntlet rule 0x52, reshaped while it is played:
  - the 1 on 1 select screen (rule 0x00's select fields);
  - 5 people (the count from 0x58; OPT +34);
  - people 2-4 are waiting opponents (team 1). They are kind 2 during select so they aren't picked, and kind 3 in the match. They are random superstars of player 1's gender.

**Gauntlet at run time:**
- Characters at 0x82E3CC50:
  - +446 state: 0 in, 1 waiting, 4 beaten;
  - +447 lost; +448 by whom; +1800 team; +2624 role (3 waiting); +2348 walked in; +476 still present (0 once gone).
- The judge `sub_82245618` brings in a waiting teammate (+446 0) of the loser, and ends the match when there is none.
- The entry task `sub_82328AA0` walks in an active entrant with +476 0 and +2348 != 1 once the match time passes info +40 (s16). An entry time of 0 never walks in.

**Endless:** a beaten opponent, once gone (state 4, lost 0, +476 0 for 1 s), waits again: state 1, role 3, +2348 0, entry time 1. The line goes round the 4 opponents. If a fall comes with no one waiting, a beaten one waits at once.

**The count:** an ImGui overlay "SLOBBER KNOCKER  BEATEN: n" over the match. Falls are counted in the judge hook.

**Not done: a new superstar each time.**
- Unloading a beaten opponent's slot (`sub_8217CCD8`, as the Royal Rumble does) and loading a new one as the run-in task does (`sub_822F5AD8`, `sub_8217CB48`/`CB68`, `sub_8224AB90`) works most times. But the characters' job (thread 22: `sub_82252610` -> `sub_82258CC0`, a per-character component, obj+136) can still use the freed character, and crashes.
  - Waiting for +476 0, setting state 5, and doing it inside the judge (the match's update) all still crashed sometimes.
- The game's swap task (`sub_825BDF60` / `sub_825BDFD8`, from `sub_825A85B8`) fills empty person slots only. With a live character in the slot, the draw crashed.
- A safe point to free a character is needed. The Royal Rumble eliminates first (`sub_8223FD78`), and its manager `sub_8224C1B8` notifies listeners before reusing a slot.

**Test aids:** `SVR2011_TEST_SK_LOG=1`, `SVR2011_TEST_SK_BEAT=<s>` (the opponent loses), `SVR2011_TEST_SK_LOSE=<s>` (player 1 loses).

## Match Creator unlocks (done, first set)

**MATCH CREATOR pages:**
- The `/MRME/MRPD` tables at misc.pac 0x932000 (PACH, 11 chunks, data base 0x93208C) have chunk pairs (defaults, allowed) per page:
  - 1/2 ENVIRONMENT, 8 rows;
  - 3/4 WIN CONDITION, 11 rows;
  - 7/8 RULES, 6 rows;
  - 5/6 and 9/10 are combination tables.
- One byte per family (68 families; `sub_82490918`: table 0x8201E4F8, u32 rule ids) and row. 0x09 free, 0x00 greyed.
- Loaded by `sub_82490558(loader)` into loader +48/+52 (env), +64/+68 (win), +72/+76, +80/+84 (rules), +88/+92.

**Port (`match_types.cpp`, hook on `sub_82490558`):**
- K.O. and FINISHER MATCH are allowed wherever pin and give up is (the cages and others).
- A TIME LIMIT is allowed wherever it was greyed.
- The INFERNO ring is allowed for triple threat (family 28) and fatal-4-way (family 36).
- Royal Rumble (60), Elimination Chamber (48/49) and backstage (12/26/55) are left alone.
- Tested:
  - a steel cage with K.O. and Finisher Match on played to the end;
  - a triple threat in the Inferno ring played 2 minutes.

**Per-match rule locks (OPT bit 7, hook on `sub_828B5078`):**
- K.O., ROPE BREAK and GIVE UP are unlocked, and DQ where it is locked on.
- Not for Royal Rumble, Elimination Chamber, backstage, story rules or Lumberjack.
- These are the rules screens that read the OPT record (`sub_82470BC0`, the online lobby `sub_824F4270`). MATCH CREATOR uses MRPD instead.

**Not unlocked (likely to break):**
- count-outs in matches of more than 2;
- over the top rope outside battle royals;
- structures for tag / 6-man;
- chamber;
- interference (forced off offline in `sub_828C48E8`).

## Elimination rows (done)

**The engine already supports elimination with 3 or 4 people.** MATCH CREATOR -> RULES -> ELIMINATION (MRPD rules row 3) is offered by the game itself for triple threat and fatal-4-way: normal, falls count anywhere and extreme rules. It is not offered with a cage, cell, ladder, table, TLC or backstage.

**What happens with it on:**
- A pin or give up sets the loser to state 4: the judge `sub_82245618` eliminates instead of ending.
- The eliminated wrestler leaves the ring (area 1), and the match goes on.
- The last fall decides the winner (live +288 / +9968).
- End steps and highlights run as usual.

**Port (`match_types.cpp`):** an ELIMINATION row after NORMAL (and FALLS COUNT ANYWHERE) in TRIPLE THREAT (0x0D) and FATAL-4-WAY (0x0E).
- The live rules are built by `sub_828C48E8(rule, live, saved)` from the option record and the player's MATCH CREATOR record for the rule (28 bytes, +18 = ELIMINATION).
- For these rows the saved +18 is 1 during that call, and restored after. The player's MATCH CREATOR settings are not changed.
- Setting only the live +37 byte was NOT enough: the first fall ended the match.

**Tested:**
- triple threat and fatal-4-way ELIMINATION rows: two eliminations, then the winner; highlights played;
- a plain triple threat afterwards had live +37 = 0.

**Test aid:** `SVR2011_TEST_FALL_LOG=1` (`slobber_knocker.cpp`) logs every character's state, lost and team, plus the 64 live option bytes, whenever a fall state changes, and the judge's decision.

## Three Stages of Hell (done)

**Menu:** ONE ON ONE -> EXTREME RULES submenu, the 3rd row (after WEAPONS EVERYWHERE). The row is a normal one on one (rule `0x00`); `match_types.cpp` marks the match and `three_stages.cpp` does the rest.

**How it works:** it is one match, not three, so health and damage carry over.
- The falls judge `sub_82245618` (hooked in `slobber_knocker.cpp`) first calls `ThreeStagesBeforeJudge`.
- A fall is a character's lost byte (+447). If the fall doesn't win the match, it is counted for the other wrestler and taken back (lost and beaten-by set to 0), and the judge goes on.
- The live rules (`0x82E3DE00`) then switch to the next stage. The game reads them mid-match:
  - fall 2, FALLS COUNT ANYWHERE (as rule `0x2B`): +1 = 2, +17 = 1, and no rope break, count-out or ring-out;
  - fall 3, LAST MAN STANDING (as rule `0x3B`): +26 = 1, and no pinfall, give up, count-out or ring-out.
- Overlay: the score and the current fall at the top of the screen. It is kept up by each world update (`ThreeStagesUpdate`), because the judge isn't called during an LMS count.

**Tested:**
- falls 1 and 2 were forced. In fall 3 the CPU played Last Man Standing on its own: the STAND UP / tap-or-hold count showed, and the match ended "RANDY ORTON WINS BY WAY OF KO" (2-1);
- the first test forced all three falls, and the match-end steps and highlights ran.

**Not seen in tests:** a pin outside the ring in fall 2 (the rules are the same bytes as rule `0x2B`).

**Game behaviour:** a long match can bring a run-in. The game loads two hidden characters at match start (slots 2 and 3, role 3 "waiting", one per side), and one walked out about 2 minutes into fall 3. It appears to be the game's own feature: no port code adds those characters.

**Crash found on the way:** `sub_8217AB58` is the sibling of the queued callback `sub_8217A718` that `frame_rate.cpp` already guards. It read a gone object's +156 part (guest 0x5E0) 16 s into a fall-3 test. It is now guarded the same way.

**Test aids:**
- `SVR2011_TEST_3S_FALL=<s>`: every `<s>` seconds person 1, then person 0, loses a fall (falls 1 and 2 only);
- `SVR2011_TEST_3S_LOG=1`: the live bytes and characters, once a second.
