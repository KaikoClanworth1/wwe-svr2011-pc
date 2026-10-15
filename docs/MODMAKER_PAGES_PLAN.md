# Mod Maker pages: a simpler sidebar (plan)

Status: approved by the user (one Media page, 6 tabs, 12 entries) and built on branch `sidebar`.

## As built

The plan's "Start from" choice is built as a tab. Each of Superstar, Arena, Moves & entrances and Media has its own tab
for other games: a **From another game** tab (on Media it is **Belts & weapons**).

- That tab runs the Port page's steps for those kinds only (disc or one file).
- Under them are the matching extracted-files tabs from the old Other games page.
- The tabs sit in a row over the page:

| Page | Tabs |
|---|---|
| Superstar | Superstar, Attires, From another game |
| Arena | Arena, Backstage, 3D editor (greyed until an arena is open), From another game |
| Moves & entrances | Moves, Taunts, Entrance, Move pack (the old Moves page), From another game |
| Media | Crowd signs, Menu backgrounds, Titantron videos, Menus & renders, Audio, Belts & weapons |

How it is built:

- Every old page id still exists. A sidebar entry goes back to the tab you last used on that page.
- The old Port from a disc page now opens the From another game tab for the kind it was showing. The old Other games
  page opens Arena > From another game.
- `--page-name` takes every old name plus these new ones: `move-pack`, `moves-entrances`, `taunts`, `entrance`,
  `media-videos`, `media-renders`, `media-audio`, `superstar-port`, `arena-port`, `moves-port`, `belts`.
- `--page 5-7` opens the matching Media tab.

The user asked: "Do we need the Other Games tab? Do we need the Port From Disc tab? Do we need the Moves tab? ...
combine things that are the same." Rule: **no feature is lost.** Each page from another game moves to the page
for the thing it makes. Each of those pages gets a source choice (new / my game discs / one file / a folder).

## The new sidebar: 22 entries become 12

| Section | Entry | What it holds |
|---|---|---|
| MAKE | **Superstar** | tabs: Superstar, Attires. "Start from" choice: new, a 2011 superstar, my game discs, one file. |
| | **Arena** | kind at the top: Arena or Backstage. "Start from" choice: new, my game discs, one file. The **3D editor** opens from a button on this page. |
| | **Moves & entrances** | tabs: Moves, Taunts, Entrance. Each tab has a source: my game discs, one file, a folder / move pack folder, or make your own. |
| | **Media** | tabs: Crowd signs, Menu backgrounds, Titantron videos, Menus & renders, Audio, Belts & weapons |
| | **Match types** | same as now |
| PORTING | **My game discs** | same as now: the only porting entry left |
| LOOK | Game assets, Animations, Icons & renders, Installed mods | same as now |
| SAVES | CAW pictures | same as now |
| HELP | Manual | rewritten to match the new sidebar |

## Where each old page and tab goes

| Old entry / tab | New home |
|---|---|
| Arena | Arena (kind: Arena) |
| 3D editor (sidebar) | A button on Arena that opens the editor. A back button returns to Arena. Arena stays highlighted in the sidebar. |
| Backstage | Arena (kind: Backstage), with the same editor |
| Superstar | Superstar, tab Superstar |
| Attires | Superstar, tab Attires |
| Moves (pack check, move-by-move view, save as .svrmod, hand to a superstar mod) | Moves & entrances, tab Moves, source "a move pack folder" or "make your own" |
| Crowd signs | Media, tab Crowd signs |
| Menu backgrounds | Media, tab Menu backgrounds |
| Media: Titantron videos / Menus & renders / Audio | Media, same tabs |
| Match types | Match types |
| **Port from a disc**, kind Superstars | Superstar, "Start from: my game discs" (the embedded flow that runs today when you import from a disc) |
| Port from a disc, kind Moves | Moves & entrances, Moves tab, source "my game discs" |
| Port from a disc, kind Entrances | Moves & entrances, Entrance tab, source "my game discs" |
| Port from a disc, kind Arenas | Arena, "Start from: my game discs" |
| Port from a disc, kind Attires | Superstar, Attires tab, "From my game discs" |
| Port from a disc, kind Belts / Weapons (texture packs) | Media, tab Belts & weapons, source "my game discs" |
| Port from a disc, kind Backstage | Arena (kind: Backstage), "Start from: my game discs" |
| Port from a disc, one-file source (arena / entrance / character .pac) | the "one file" source on Arena, Entrance and Superstar |
| My game discs | My game discs (unchanged) |
| Port an entrance / Port moves / Port taunts | Moves & entrances, tabs Entrance / Moves / Taunts. These run the same Simple-porting steps as now. |
| **Other games**, WWE '12 - 2K17 arena | Arena, "Start from: one file" (picks up those files) |
| Other games, Superstar file (any game) | Superstar, "Start from: one file" |
| Other games, WWE '13 superstar / SvR 2010 superstar | Superstar, "Start from: my game discs" (or one file) |
| Other games, SvR 2010 moves (svr10_moves.py + movepack.py) | Moves & entrances, Moves tab, source "my game discs" when SvR 2010 is set, otherwise "a folder" |
| Other games, SvR 2008 backstage (svr08_stage / svr08_gimmick) | Arena (kind: Backstage), "Start from: my game discs" (SvR 2008) |
| CAW pictures, Game assets, Animations, Icons & renders, Installed mods, Manual | unchanged |

How the work is done:
- The converters do not change.
- The code moves without being rewritten. The flows in port_page / other_page / simple_port become parts that the new pages call.
- port_page.cpp and other_page.cpp stop being pages. Their flows stay in those files.

## Test aids

- **`--page-name`**: every old name still works, as an alias.

  | Old name | Opens |
  |---|---|
  | `editor` | the Arena editor |
  | `backstage` | Arena with kind Backstage |
  | `attires` | Superstar, Attires tab |
  | `moves`, `port-moves` | Moves & entrances, Moves tab |
  | `port-taunts` | Moves & entrances, Taunts tab |
  | `port-entrance` | Moves & entrances, Entrance tab |
  | `signs`, `backgrounds`, `media` | Media, matching tab |
  | `port`, `discs` | My game discs |
  | `other` | Superstar, "Start from" list |

  New names: `moves-entrances`, `taunts`, `entrance`, `belts`.
- **`--page N`**: this only ever covered 0-7. A small table keeps the old numbers on the same pages.
- **`--port-test kind@game,src,opts`**: keeps its format. It opens the page the kind now lives on, with the same flow.
- **`--simple-port ...`**: format unchanged. Only the page it lands on changes, to Moves & entrances (import still goes to Superstar).
- **`--convert-w13`**: unchanged. It opens the Arena editor.
- **`--ui-script` scripts that click sidebar labels**: these need the new labels. Only my test scripts use them; nothing that ships does.

## Manual (docs/MOD_MAKER_MANUAL.md)

- The sections follow the new sidebar.
- "Getting around" gets the new layout.
- The "Other games" and "Port from a disc" text is split into a short "From another game" part on Superstar, Arena,
  Moves & entrances and Media.
- One line for old users: "Port from a disc and Other games are now the 'Start from' choice on each page."

## Risks

1. **Shared state in port_page.** It keeps one kind, file and conversion at a time in globals. Pages share it: a
   second page can't start a conversion while one runs. That page shows "a conversion is running on <page>" with
   Cancel. Leaving a page during a conversion keeps it running, as it does today.
2. **One screen.** The Media page with 6 tabs, and the source choice on each Moves & entrances tab, must fit at
   1280x720. I'll check each with hidden shots (modmaker_shot.ps1); no windows are shown.
3. **The editor inside Arena.** Unsaved edits must survive going back and forth between Arena and the editor. Today
   two sidebar entries do that; the button must keep the same state and not reload.
4. **Projects (.svrproj) and settings.** Anything that stores a page needs the alias table too. I found no stored
   PageId so far; I'll check again before coding.
5. **Size of the change.** About 6 page files are touched. I'll do it in steps, with a hidden check of every page
   after each step:
   1. sidebar and aliases;
   2. Moves & entrances;
   3. Superstar and Arena sources;
   4. Media;
   5. Manual.
6. **Question for the user.** Media with 6 tabs, or keep Crowd signs + Menu backgrounds as one "Signs & menus"
   page next to Media (13 entries instead of 12)? My pick: one Media page.
