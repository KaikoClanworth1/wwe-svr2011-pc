# Match type mods (type=matchtype) - format

A match type mod is a folder `Mods\MatchTypes\<id>\` holding a `manifest.txt`
(and nothing else). Two kinds:

1. **A port match type's switch** (what 2.0.5 ships: the 11 bundles made by
   `tools/make_matchtype_mods.py`). The match type's code is in the game
   (`src/match_types.cpp`); the mod only turns it on - a `disabled` file in
   the folder (the launcher's tick) leaves it out of the menus.
2. **A custom match type** (proposed here, Mod Maker side done; the game side
   is the Limit Breaking session's): the game builds it at start the way it
   builds its own added rows - a menu row whose match is an existing rule
   record reshaped at match set-up (people, slots, arena, option record,
   placed weapons) - so no new rule id is needed.

## Keys

| key | kind | |
|---|---|---|
| `type=matchtype`, `id=`, `name=`, `author=`, `version=`, `made_with=` | both | as every mod |
| `about=` | both | one line for the launcher / Mod Maker |
| `builtin=<key>` | 1 | the port match type this mod switches: `falls_count_anywhere`, `championship_scramble`, `royal_rumble_15_25`, `lumberjack`, `free_roaming_backstage`, `backstage_more_people`, `weapons_everywhere`, `slobber_knocker`, `three_stages_of_hell`, `elimination`, `mystery_opponent` |
| `base=0xNN` | 2 | the rule record (misc.pac RUL, 0x00-0x76) the match starts from: its slots, flags and family |
| `menu=<list>` | 2 | the menu list that gets the row: `ONE ON ONE`, `TWO ON TWO`, `TRIPLE THREAT`, `FATAL-4-WAY`, `6-MAN`, `HANDICAP`, `ROYAL RUMBLE`, `BACKSTAGE` |
| `submenu=<text>` | 2 | the submenu inside that list (e.g. `EXTREME RULES`); made when missing. Required for `ONE ON ONE` (its list is full: 14 rows, the most a list shows) |
| `label=<text>` | 2 | the row's text (the name when absent) |
| `people=<2-6>`, `ffa=<0/1>` | 2 | the option record's +34 (people count) and +35 (free-for-all) |
| `slots=i:t:k,...` | 2 | the participants (at most 6): index, team, kind (0 wrestler, 1 special referee, 2 manager, 3 waiting gauntlet entrant), one per person; absent = the base's |
| `arena=<bgNN>` | 2 | a forced arena (rule +40 with +36 = 4); `0` = the player's pick (the base's otherwise) |
| `opt.<rule>=<value>[!]` | 2 | an option record byte: the value, `!` = locked (the player can't change it). Rules: `pinfall` (1), `ko` (2), `rope_break` (3), `dq_off` (4), `count_out` (5), `over_the_top` (6), `give_up` (7), `minutes` (8), `cage` (9), `iron_man_falls` (12), `hell_in_a_cell` (20), `timed` (21), `last_man_standing` (26), `ring_out` (27), `interference` (32), `outside` (33), `tornado` (36), `elimination` (37), `entrance` (38), `first_blood` (40), `chamber` (41), `escape_door` (44), `extreme_rules` (56), `tag` (58), `inferno` (59) - the byte offsets in brackets |
| `weapons=0xNN` | 2 | the placed-weapons table of that rule (e.g. `0x4D` Extreme Rules) around the ring |

Not in v1: Match Creator allowances (the MRPD rows were where most Match
Creator crashes came from). Custom match types are offline only, or online
when both players have the same mod. Reviewed by the Limit Breaking work
(2026-10-07): the format stands with these rules; the game side comes after
2.0.5.

Everything beyond `base=` is optional: a mod with only `base=`, `menu=` and a
name is a copy of that match under a new row.

## How the game would build one (for the game side)

At start, for each enabled folder with `base=`:

1. a runtime menu row in `menu=` (`submenu=`), label = `label=`/`name=`,
   like the FCA / LUMBERJACK rows (`src/match_types.cpp`, the 82BAA6E8 hook);
2. when the row is chosen: the base rule set up, then at match set-up
   (`sub_827374A0`) the record reshaped like the backstage reshape: people
   and ffa into OPT +34/+35, `slots=` into +2..+19, `arena=` into +36/+40, the
   `opt.*` bytes (bit 7 for `!`) into the live option record, the weapons
   table swapped to `weapons=`'s rule while `sub_8227B938` / `sub_8227D130`
   run;
3. restored at the next set-up (as the backstage reshape does).

The in-match name (string 0x6B08 + id): the game side supplies a runtime
string with the custom name, as for WEAPONS EVERYWHERE. The 11 switches'
game side is in main (53d6583): `MatchTypeOn(id)`, off only with `disabled`.

## The Mod Maker

The Match types page writes both kinds: a switch for any of the 11 port match
types, or a custom type from a form (base match picked from the game's 119
rules by name, menu list, people, slots, arena, every option rule with its
lock, weapons, Match Creator rows), with the game's rule and option records
shown decoded for reference.
