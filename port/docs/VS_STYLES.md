# VS screen styles

The VS screen is the match set-up page after the superstar select. It shows
both renders, "A VS. B", the PLAY / MATCH CREATOR / SELECT ARENA / SIMULATE
row and "<show> TONIGHT". A VS style replaces it with an older SmackDown vs.
Raw game's look, made from the player's own disc (never shipped).

## What the screen is made of

Each arena has a VS theme of three entries in `pac/menu/MatchHD.pac`:

| Entry | What |
|---|---|
| `MENU/M<nn>L` | the layout, `YXMLBIN VER0002`: resources (`font000`, the frame / background, `nearbg01-05`, `farbg`), nodes, text lines `font000_lname / _rname / _vs / _sub / _champ / _tonaight` |
| `MENU/M<nn>I` | the textures (`0x1B59`, DXT5) and their `ATB0` animations (`0x1B5A`) |
| `MENU/M<nn>F` | the font (`font_smack.yfb` and its page) |

- `sub_8271D868` picks the theme: the arena's number, or 60-66 for some match types; M00 when the theme's file is missing.
- The renders are not part of the theme: the render manager draws them.
- The game looks every theme's names up once, at start (`sub_826B87B0`). The VS screen only checks they exist (`sub_826A17D0`, which logs `VS screen: theme /MENU/M<nn>x` for tests).

## The older games

- **SvR 2010** uses the same system. Its `M<nn>L` are in `pac/menu/menu.pac`, while `M<nn>I` / `M<nn>F` are in `MatchHD.pac`. Its themes play in 2011 as they are; only their text lines sit lower, behind 2011's menu row. Ready.
- **SvR 2008 and 2009** have the same files, but their layouts differ in three ways that 2011's VS screen trips over. `vs_port.reshape` rebuilds them:
  1. **Groups:** 2009 puts beta with the background (6 groups), and 2008 has no beta and three text lines more per group (main / main2 / sub2). 2008 also stores its layouts in another serialization: every record in a row, then a (count, first record) table per group. 2011 crashed in `sub_8269BE90` on that. Rebuilt into 2011's 7 groups (root, four text groups, beta alone, background); the unused lines are dropped and 2011's `dummy_tonaight` is added.
  2. **Parallax layers:** they have five type-11 resources (farbg at index 5) where 2011 expects six (nearbg01-05 index 1-5, farbg index 6). With five, nothing of the theme draws, only the superstars. Replaced by 2011's six.
  3. **Text placement:** a text group's lines are placed from its first node. 2011's is an animation node, whereas an older theme's is its frame picture, wherever it sits (2009 SummerSlam: lines below the menu row). Each rebuilt text group gets an empty first node (the root's record) at y 233, so the lines sit at 2011's places.

  Checked on screen: 2009 RAW / SummerSlam, 2008 RAW / Royal Rumble / ECW.
- **SvR 2007** has no themes of this kind.

## Converter: `tools/vs_port.py`

Follows the Mod Maker's converter contract (`games` / `list` / `convert`).

1. It maps old theme numbers to 2011's through `arena_port.py`'s tables (the 2011 arena of the same show). WrestleMania goes to 11 and Backlash to 12.
2. Each theme's L / I / F are copied under 2011's number.
3. Every text node takes the x, y, scale and width of 2011's node of the same name and occurrence: from the same theme, else from 2011's SmackDown / RAW layout. A node no 2011 layout has is scaled to nothing.

Output is `<out>/manifest.txt` (`type=VsStyle`, `name=`, `game=`, `themes=`), `vs.pac` (EPAC, group MENU) and `report.json`.

`--renders` (`tools/vs_renders.py`) adds `<out>/renders`: a media pack with the old game's renders (`render.<id>`, `bust.<id>`). It covers every superstar both games have under the same id and name, and 2011 shows them wherever it shows renders.

The Mod Maker: Arena > From another game > VS screens. "Install the style" copies the folder to `Mods/VsStyles/<game id>` and the renders to `Mods/Media/<game id>_renders`.

## In the game: `src/vs_styles.cpp`

- At start, the installed styles (`Mods/VsStyles/*/`; a `disabled` file turns one off) are merged into `Mods/SuperstarOverlay/vsstyles.pac`, groups `VS00`, `VS01` and so on. It is rebuilt only when a `vs.pac` changed (`vsstyles.pac.src`).
- The pac is mounted with the superstar mods' overlay pacs.
- While `vs_style` names a style, the resolver hook points `/MENU/M<nn>x` at that style's `/VSnn/M<nn>x`. A theme the style hasn't stays 2011's.
- GRAPHICS > VS SCREEN STYLE (only shown when a style is installed) sets it, from the next start.

## Tests

The scratchpad kit `vs/vs_shot2.ps1`:
1. `route normal`, then `pressuntil A VS screen: theme /MENU`;
2. shots 2 s and 3.5 s later.

It needs a private test game with real pac copies, never hard links into Game Files.

## An arena mod's own VS screen

An arena mod can carry its own theme, `vs/theme.pac`: an EPAC with group MENU and the entries VSTL / VSTI / VSTF (layout, pictures, font). The manifest records where it came from with `vs_theme=` (and `vs_font=`).

- **The game:**
  - At start, each mod's theme joins the overlay pac above as group `VA00`, `VA01`, ... (vs_styles.cpp, from `ArenaThemePacs()`).
  - While that arena is chosen, the VS screen's existence check (`sub_826A17D0`) gets its path rewritten: `/MENU/M<nn>x` -> `/VA<k>/VSTx` (the same length). The game loads by that name right then, so no boot-time alias is needed.
  - The arena's theme wins over VS SCREEN STYLE. Arena mods without one keep the picture swaps (`vs/<name>.dds`) on their base arena's theme.
- **The converter:**
  - `vs_port.py themes` lists every theme of a disc, or of 2011 itself (`--game svr2011 --src <2011 game>`), with a preview PNG.
  - `theme` extracts one theme (an older game's layout rebuilt in 2011's shape, text lines at 2011's places).
  - `parts` / `replace` list the pictures and put a PNG into one of them (its size and DXT format, mips, the game's compression).
  - `font` takes the font of another theme.
  - `arena_port.py` (SvR 2010 discs) gives a converted arena its own 2010 theme.
- **The Mod Maker:**
  - Arena > VS screen: "Pick a theme..." (the 2011 game and the SvR 2008 - 2010 discs from My game discs, previews), "Font from another theme...", "Back to the slot's theme", and Replace on any picture.
  - Making the mod bakes the replaced pictures into `vs/theme.pac`.
  - An opened mod keeps its id, and its `arena.pac` as it was unless the arena was edited.
  - Test aid: `--vs-test "theme=<game>,<src>,<n>[,<label>];font=...;replace=<picture>,<png>;version=<v>;save=<file>"` (with `--open <mod>`).
