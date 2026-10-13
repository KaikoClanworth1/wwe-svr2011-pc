# Mod Maker 3 - plan (checklist section D)

Owner: the Mod Maker chat (ModMakerUIRework). Started 13 Oct 2026.
Source list: `CHECKLIST.md` section D (git-ignored, kept by Main).
Builds on Mod Maker 2 (`docs/MOD_MAKER_PLAN.md`, the 2.0.5 rework).

The aim: the Mod Maker is where everything from the user's other WWE discs
comes into SvR 2011. You pick a disc, see what is on it and what 2011
already has, and convert one thing at a time into a mod you can finish and
test. The converters live in `port/tools` and belong to the porting chats.
The Mod Maker reads the discs, runs the converters, shows their results and
edits the mods.

Game order (the user's): Xbox 360 SvR 2007, 2008, 2009, 2010, then WWE 12,
WWE '13, 2K14. Later: 2K15-2K17, Legends of WrestleMania, the PS2 games
(Just Bring It ... SvR 2011, Crush Hour).

## 1. What the discs look like (surveyed 13 Oct)

| Disc | Container | Partition | Files | Notes |
|---|---|---|---|---|
| X360 SvR 2007 | XDVDFS | 0xFD90000 (XGD2) | 290 | `pac` (185), `.bik`, `.xwb` music |
| X360 SvR 2008 / 2009 / 2010 | XDVDFS | 0xFD90000 | 369 / 372 / 374 | `pac` (254-261), `.bik`, `.yaf` sound |
| X360 Legends of WrestleMania | XDVDFS | 0xFD90000 | 409 | `pac` (236), `.bik`, `.yaf` |
| X360 WWE 12 | XDVDFS | 0xFD90000 | 564 | `pac` (268), `.bik`, `.pck` sound |
| X360 WWE '13 | XDVDFS | 0x2080000 (XGD3) | 716 | `pac` (356), `.bik2` |
| X360 2K14 / 2K15 | XDVDFS | 0x2080000 | 730 / 692 | `pac` (~370), `.bik2`/`.bk2`, `.xzp` |
| X360 2K16 / 2K17 | XDVDFS | 0x2080000 | 4488 / 5242 | `pac` (4083 / 4858: split per asset) |
| PS2 Just Bring It | ISO 9660 | - | 171 | `*.PAC` in the root |
| PS2 Shut Your Mouth | ISO 9660 | - | 299 | `PAC\` |
| PS2 Here Comes the Pain | ISO 9660 | - | 100 | `ED*`, `BGM`, dummy padding files |
| PS2 SvR, SvR 2006 | ISO 9660 | - | 205 | `BELT`, `BGM\*.AFS`, `ED*` |
| PS2 SvR 2007-2011 | ISO 9660 | - | 149-196 | `PAC\`, `PLISTPS2.ARC`, `.AFS`/`.YAF` sound |
| PS2 Crush Hour | ISO 9660 | - | 1065 | a different engine (`BUNDLES`, `SEASONS`) |

Every 360 disc keeps the same Yuke's layout (`pac\`, `movies\`, `sound\`,
`plist360.arc`). The pac files' first 4 bytes, counted per disc:

| X360 disc | EPAC | EPK8 | `0FF512ED` (xcompress, LZX) | other |
|---|---|---|---|---|
| SvR 2007 / 2008 / 2009 | 184 / 260 / 253 | - | - | 1 PACH |
| SvR 2010 | 152 | 105 | - | 1 PACH |
| Legends of WrestleMania | 235 | - | - | 1 PACH |
| WWE 12 | 161 | 106 | - | 1 PACH |
| WWE '13 / 2K14 / 2K15 | 17 / 23 / 19 | 10 / 9 / 13 | 328 / 334 / 337 | 1 PACH |
| 2K16 / 2K17 | - | - | 4082 / 4857 | 1 PACH / SHDC |

So up to WWE 12, the 2011 readers (EPAC, EPK8, PACH, BPE) open the pacs as
they are. From WWE '13 on, most pacs are xcompress-wrapped: the Mod Maker
already unwraps them (`xdec`, built in for WWE '13 arenas), and gets an EPAC
/ EPK8 inside. What the entries hold (model, motion and texture versions) is
D2's per-game table (Superstar Porting); the Mod Maker's viewers follow it.

## 2. Pages (what changes)

New:
- **Discs** (LOOK): the user's disc images (a folder list, default
  `D:\Roms\Xbox 360\WWE` and `D:\Roms\Playstation 2\WWE Games`, editable),
  each recognised by title id / file name with its game id from the
  converters' `games` list, and its status (ready / research).
- **Port** (MAKE): the wizard (D3). Disc, then a kind (superstar, arena,
  moves, entrance, attire, belt, weapon), then the item from the
  converter's `list`. Each item shows what 2011 already has. The converter
  runs and its result opens on the right page (Superstar, Arena, Moves ...)
  as a normal project.

Changed:
- **Game assets** (LOOK) becomes the EPAC tool (D16). Its sources are the
  game folder, any disc image or any folder. It browses, extracts, replaces,
  adds and removes entries, repacks EPAC / EPK8 / PACH with BPE (stored or
  packed), and saves as a copy; it never writes into `Game Files\pac`.
  Viewers stay lazy and cached (D17).
- **Superstar**: overall rating under the sliders (D13); hometown (D8);
  theme volume with a loudness meter (D7); the 4 renders with a picker and a
  template (D7); moveset by name from the game's lists, signatures and
  finishers included (D4); attires as a list with their own entrance and
  entrance outfit (D5 / D6); warnings for limits and crash risks (D12).
- **Animations / model view** (D9, D4): blood layer, finger motion, PS2 and
  360 motions decoded right, preview on the chosen model; a sturdier moveset
  viewer (every slot of a moveset played in order); collision boxes on/off,
  shown on two dummies (green = attacker, red = victim).
- **3D editor**: an entrance preview inside the arena (D5): the superstar
  walks the ramp in the arena being edited.
- **Belts / weapons** (D11): a page for the Arena Porting chat's belt and
  weapon mods once the format exists.
- **New attires for the roster** (D6): a mod that adds attires to a stock
  superstar (format from the game side, below).

## 3. Phases

Work goes in this order. Each phase ends with a commit and a hidden test
(`--behind --shot`).

| Phase | Items | Needs from others |
|---|---|---|
| P1 | D1 disc reading: `port/tools/discfs.py` (folder / XDVDFS / ISO 9660, for every converter) and the same in C++ for the Mod Maker; the Discs page; Game assets browsing a disc | - |
| P2 | D16 EPAC tool + D17 speed (startup time, cached viewers, a measured before / after) | - |
| P3 | D13 overall, D12 warnings, D4 moveset by name, D8 hometown (UI), D7 theme volume (UI) | game keys (section 5) |
| P4 | D3 port wizard on the converter contract; the "2011 already has" checklist | the converters' `games` / `list` / `convert` |
| P5 | D9 model / animation viewer, D4 sturdier moveset viewer and collision boxes | collision data (section 5) |
| P6 | D5 entrances: per-attire picker, entrance outfit, in-arena preview; D6 attires | HCTP Entrance + game keys |
| P7 | D7 renders (4 + template), select / VS animation; D11 belts / weapons | Arena Porting, game side |

## 4. The converter contract (all porting chats)

One script per kind, the same commands for all of them, called through
`tool_run.cpp` (Python 3 + numpy + Pillow; ffmpeg where needed):

```
py tools/<kind>_port.py games
    -> [{id, name, platform: "x360"|"ps2", disc_hint, status: "ready"|"research"}]
py tools/<kind>_port.py list --game <id> --src <ISO or extract folder> [--pac11 <2011 pac>]
    -> [{key, name, ... kind's fields ..., in_2011: <2011 id/number or null>}]
py tools/<kind>_port.py convert --game <id> --src <...> --item <key> --out <dir> [kind options]
    -> <dir>/report.json and the same JSON on stdout:
       {ok, out: <the .svrmod / pack folder / file made>, opens: "superstar"|"arena"|"moves"|...,
        warnings: [], dropped: [], notes: []}
```

- `<kind>`: `superstar` / `moves` / `attire` (Superstar Porting), `entrance`
  (HCTP Entrance Porting, agreed 13 Oct: its `--entrance`/`--number` options
  stay), `arena` / `belt` / `weapon` (Arena Porting), `media` (Extras).
- `--src` takes an ISO or a folder: converters read through
  `tools/discfs.py` and never need an extracted disc.
- stdout carries only JSON. Logs go to stderr, and progress lines read
  exactly `progress <0-100> <text>`.
- Exit codes: 0 ok, 2 bad input, 3 conversion failed; anything else is a
  crash (the Mod Maker shows the stderr tail).
- `in_2011` drives the wizard's "2011 already has this" column.
- Output is a normal mod or pack: the Mod Maker opens it on its page, where
  the user finishes it and saves.

## 5. What the Mod Maker needs from the game side

| Need | For | Who (proposed) |
|---|---|---|
| `entrance=` per attire (`attire<N>_entrance=`), entrance outfit | D5 | HCTP Entrance Porting (offered) |
| `theme_gain=<dB>` (or loudness applied by the game) | D7 | Extras |
| `hometown=<clip id>` and stock name calls for mods and CAWs | D8 | Extras (announcer) |
| a mod that adds attires to a stock superstar (`type=attire`?) | D6 | Superstar Porting + game side |
| which files hold the 4 renders and how each is used | D7 | Superstar Porting (research) |
| the select / VS screen motion per superstar (key + data) | D7 | game side (owner to be named) |
| the characters' collision volumes (where in ch.pac / the motion data) | D4 | Superstar Porting (research) |
| the overall rating formula (or where the game stores it) | D13 | Mod Maker (offline, from the roster data) |
| belt / weapon mod formats | D11 | Arena Porting |
| per-game format table (D2) | all | Superstar Porting |

## 6. Questions for the user

1. D7 "select / VS screen animation": the pose the superstar plays on the
   character select / VS screen (a motion choice per mod)? Or an animated
   (moving) render picture?
2. D9 "animations right for every platform": motions from PS2 and Xbox 360
   discs shown correctly in the viewer? Or the Mod Maker itself running on
   other platforms (Steam Deck / Linux)?
3. D7 "render template": a ready-made background / frame that makes your
   render look like the game's (lighting, crop, size), so a plain photo or
   3D render fits in?
4. D5 "entrance outfits": clothing worn only during the entrance (a jacket,
   a mask), then the match attire?

## 7. Tests

- Mod Maker: always `--behind --shot <png>`, never a visible window, no
  input to the user's mouse / keyboard.
- Disc reading: every disc above lists, a file reads, and its bytes match
  `extract_iso.py` for 360 and a plain ISO 9660 walk for PS2.
- The EPAC tool: a repack with no change is byte-identical; a replaced entry
  reads back; the game loads a repacked copy (in a test game folder, with a
  test slot from Main).
- Converters through the wizard: the hidden Mod Maker runs `list` and
  `convert`, and the mod it makes passes the page's problems check.
- Game tests: only with a slot from Main; muted, `--monitor=2`, mnk_mode and
  p2p_port_mapping off.
