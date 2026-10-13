# Other games: what can come to SvR 2011

One section per kind of asset. Each porting chat keeps its own section up to
date; the converters follow docs/MOD_MAKER_3_PLAN.md section 4
(`tools/<kind>_port.py games / list / convert`).

## Arenas

Converter: `port/tools/arena_port.py` (Arena Porting). It reads the disc
through `tools/discfs.py` (an ISO or an extracted folder). It writes a
normal arena mod: `<out>/<id>/` and `<out>/<id>.svrmod` with `manifest.txt`,
`arena.pac`, `banner.dds`, and where the game has the art, `load.dds` and
`vs/<name>.dds`. The manifest's `base=` decides the tile (on the custom arena
pages) and the VS theme. The game gives the arena file the stage name of the
slot it is played in (`arena_mods.cpp` RedirectArena), so any base slot works.

| Game | Arena files | State | How |
|---|---|---|---|
| SvR 2007 (360) | | research | not extracted yet |
| SvR 2008 (360) | `pac/bg/bgNN.pac`, EPAC | research | different layout (see below); unchanged it crashes the 2011 arena load |
| SvR 2009 (360) | | research | not extracted yet |
| **SvR 2010 (360)** | `pac/bg/bgNN.pac`, EPAC | **ready** | the same format as 2011: the file is used as it is |
| WWE '12 (360) | | research | not extracted yet |
| **WWE '13 (360)** | `pac/bg/bgNN.pac`, `0x0FF512ED` LZX container around an EPK8 | **ready (in the Mod Maker)** | `svrfmt/wwe13.cpp` (`svrmod wwe13`); see below |
| WWE 2K14 (360) | | research | not extracted yet |

### SvR 2010

- **Format:** the same EPAC / PACH / JBOY / texture sets as 2011. The entries
  are 2011's, except that there is no announce table set (0x192, so the table
  shows the game's default pictures) and a smaller crowd pack.
- **Tested:** 2010's RAW (bg01) plays unchanged in 2011 (crowd, ring, props),
  and so does the converted No Mercy (bg06) on the Hell in a Cell slot.
- **Arenas:**

  | bg | Show | Notes |
  |---|---|---|
  | 00 | SmackDown | |
  | 01 | RAW | |
  | 02 | ECW | |
  | 04 | SummerSlam | |
  | 05 | Unforgiven | |
  | 06 | No Mercy | |
  | 07 | Survivor Series | |
  | 08 | Armageddon | |
  | 09 | Royal Rumble | |
  | 10 | No Way Out | |
  | 11 | WrestleMania 25 | |
  | 12 | Backlash | |
  | 13 | Judgment Day | |
  | 14 | Night of Champions | |
  | 15 | The Great American Bash | |
  | 18 | Cyber Sunday | |
  | 24 | Extreme Rules | no banner or VS art on the disc |
  | 16, 20, 22, 25 | | story copies (RAW, No Way Out, Royal Rumble, Road to WrestleMania), left out |
  | 23 | | practice ring, left out |
  | 30 | | not in the arena list |

- **Pictures:**
  - The banner comes from 2010's arena select (MatchHD `MENU/MASI`, the
    `arena_*` textures). 2010's PPV banners are square, so they are shown
    whole over the arena's darkened VS picture.
  - The loading picture is the biggest picture of 2010's VS theme for that
    arena (`MENU/M<nn>I`).
  - That picture is also used for the VS screen when the base slot's 2011
    theme has a single 1024 x 512 background: SmackDown `bg_sd`, RAW
    `RAW01`, Tribute to the Troops `troops_tex05`, SummerSlam `BG`,
    WrestleMania `wm_bg01`. Otherwise the base theme stays.
- **Base slot:** the 2011 arena of the same show (SmackDown 0, RAW 1, ECW 2,
  SummerSlam 4, Survivor Series 7, Royal Rumble 9, Judgment Day 13, Night of
  Champions 14, Bash 15, Extreme Rules 16). Otherwise its own number.
  `--base` changes it.

### SvR 2008

- **Format:** the arena file is an EPAC with the stage group (STG/00NN), but
  laid out differently from 2010 / 2011. It has:
  - no main texture set 0x190;
  - no crowd pack (0x4E20/0x4E2A);
  - no visibility tree (0xC364), no announce table / card entries
    (0x192 / 0xEE48) and no 0x80E8 / 0x7922.

  The models are the same JBOY family: `tools/svr08_stage.py` already moves
  2008 stage models into a 2011 backstage room.
- **Tested:** RAW 2008 (bg01) unchanged crashes the 2011 arena load (in
  `sub_826D73D0`).
- **Next:** where 2008 keeps an arena's textures; then a converter like
  `wwe13.cpp`: the 2008 models and collision, with the missing entries
  (crowd, visibility, light zones) from a 2011 host.

### WWE '13

Done in the Mod Maker (`svrfmt/wwe13.cpp`). Notes:

- The memory notes in the arenas work, and the Mod Maker manual, have the
  details.
- A single model over about 430 KB unpacked crashes the arena load. Use the
  `drop-mesh=` option.
- The TitanTron videos are Bink 2, which can't be converted.
- Arenas without a 0x192 table set get a made one.

## Belts and weapons

See D11 (research in progress).
