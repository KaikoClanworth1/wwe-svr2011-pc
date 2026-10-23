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
- **SvR 2009** has the same files and its themes load, but the renders and the names line never show. Its layouts have 6 scene groups where 2010's and 2011's have 7 (the likely cause, not checked). `research`; its renders (`vs_renders.py`) work on their own.
- **SvR 2008** has the same files, but its layouts have another shape: 30 nodes, with an extra text line in each group. 2011's screen faults on them in `sub_8269BE90`, with or without the text move, so 2008 is `research`.
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
