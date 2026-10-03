# Superstar mods (arenas branch)

New playable characters from `<game>/Mods/Superstars/<folder>/`, up to 50.

- **Game:** `src/superstar_mods.cpp`.
- **Select screen EXTRA list:** `src/managers.cpp`.
- **Making them:** the Mod Maker's Superstars page.
- **Installing them:** the launcher's Mods tab (PC and Android).

## A mod folder (a .svrmod is a zip of one)

| file | |
|---|---|
| `manifest.txt` | `type=superstar`, `id=`, `name=` (31 chars), `short=`, `base=<id>`, `author=`, `version=`, `song=<file>`, `movie=<file>` |
| `ch.pac` | the model pac (EPK8, like `pac/ch/chNNN.pac`). It can be any character's: the `EMD` names get the slot's id |
| `theme.<ext>` | (optional) the entrance song: .mp3 .m4a .aac .wav .flac .wma .ogg |
| `movie.bik` | (optional) the entrance movie: a 320x320 Bink, made in the launcher's Movies tab |
| `disabled` | (optional) turns the mod off |

`base=` is the disc superstar the mod starts from, which supplies its stats,
moves, entrance motions and pyro, and select render. It must be one of the 91
playable ones, those with `SSFA` renders in `pac/DLC_HD.pac` (which includes
The Hurricane, 274).

## Ids: 50 slots

The pool, in `kPool`:

- the free DLC slots **59-69** (real DLC uses 51-58);
- then 39 disc ids whose `CHAR/DAT` record is a blank placeholder: name "0",
  loaded, not selectable, with no model, render, entrance or match data in any
  pac (111, 114, 121, ...).

Each of these has a record, a profile and room in the save. 61 such disc ids
exist, so the pool could grow to 72.

Each mod keeps its id in `Mods/Superstars/slots.txt` (`<folder>\t<id>`):

- Adding or removing a mod never moves another one's data in the saves.
- Pool ids with no mod are kept unselectable, so a removed mod's record
  left in a save can't be picked.
- Remove a mod's line from slots.txt to free its id.

## How it works

### Files

- Each mod's `ch.pac` gets its EMD names renamed to its id.
- Its base's renders are written to `ssfNNN.pac`.
- Both go in `Mods/SuperstarOverlay/`. They are rebuilt only when the
  source is newer.
- They are mounted with the game's `sub_826A1780` from `smods:`, a device of
  its own made after the files are written. The game's `GAME:` device lists
  its folders once at start, so it would not see files made this run.

### The game's directory buffer

Each mount appends the pac's directory to the game's directory buffer: vfs+56
is the start, vfs+60 the end, and vfs is at 0x82ED5FEC. There is no room for
dozens more: 15 mods crashed. So before mounting, the buffer moves to a bigger
allocation. What was registered keeps pointing into the old buffer, which is
left alone. The render pacs' header +4, the directory size, is their own.

### Records and profiles: no resetting

A slot is set up from the base only when it isn't the mod's yet:

- **The record:** set up when its name isn't the mod's (a new mod, or a save
  from before it). It gets the mod's names, its own id at +32, +218 and +228,
  selectable and the DLC flag.
- **The profile:** set up while it still equals the blank one `CHAR/PRO`
  loaded for the slot.

The game reloads `CHAR/DAT` and `CHAR/PRO` after a save loads (the DLC scan,
`sub_825A09B0`), which puts the placeholders back. The slot as it was just
before is remembered and put back, so the save's copy and the player's edits
stay. The save (`SaveData.dat`) holds both records and profiles.

Tested: the game wrote a save holding an edited mod (ratings 20). On the
next start it still had ratings 20 after the save and the reloads.

### Theme and movie

At start, `song=` is copied to `Music/<name>.<ext>`, making it a USER
PLAYLIST. `movie=` is copied to `Custom Movies/<name>.bik` and gets a user
movie id, kept in that folder's ids.txt. The movie copy happens before the
Custom Movies device is made.

When the profile is first set up, its entrance block (profile +0x1C0;
scratchpad re_entrance) gets:

- music id +0x10 = 254 (USER PLAYLIST), with the name at +0xCC (UTF-16BE);
- movie id +0x12 = the user movie.

The base's motions and pyro stay, because +0xC7 stays 0, the default entrance.

### Owned and listed

DLC-flagged ids must be owned, and mods are. The DLC tile's filter
(`sub_8244A128`) leaves mods out. They are listed under the M (EXTRA) tile,
after the managers.

The managers are Stephanie McMahon, Theodore Long, Paul Bearer, Tiffany and The
Hurricane. Hornswoggle (195) is left out: his moves and animations are broken.

## The MODDED badge

The select panel shows DLC characters with the "DOWNLOADABLE CONTENT" badge,
the texture `DLCtex01` (menuHD.pac `MENx/CHSI` 7001; 256 x 64 DXT5, one per
language). It is layout node `cursor+312` in `sub_82466830`.

- **Tracking:** a hook on that panel's DLC check (`sub_828B6CD0` called from
  0x82466E40) records which panels hover a mod.
- **Finding the texture:** a background thread finds it in guest memory, by
  the swapped DXT5 blocks of any language's original.
- **Swapping:** it writes the "MODDED" pixels while any panel hovers a mod,
  and the original otherwise.

The art comes from `tools/make_modded_badge.py`, which writes
`src/modded_badge.inc`.

## Known limits

- There is no name call: the voice cue `WrestlerVoice_%04d` is per id.
- Only the attires the `ch.pac` holds are available. The select render is the
  base's.
- The theme and movie are set when the mod's profile is first set up. If one is
  added to a mod later, set it in game (Create An Entrance) or remove the mod's
  slots.txt line.

## Tests

`tools/superstar_test.ps1` runs an exhibition one on one: P1 opens the M
tile, steps `-Down` entries (the 5 managers come first) and picks a mod.
Options:

| option | effect |
|---|---|
| `-Com` | COM's pick goes through the M tile too |
| `-NoSkip` | lets the entrances play |
| `-Walk N` | only steps down the list, a screenshot per step |
| `-UserData <folder under runs>` | uses that save folder as it is |

Test aids (environment variables):

- `SVR2011_TEST_VFS_LOG=1`: every virtual file lookup that finds nothing.
- `SVR2011_TEST_STAR_EDIT=<id>`: that mod's ratings set to 20 when set up,
  to test that edits survive a save.

Mod Maker test aids: `--page 3 --star <id> [--star-song f] [--star-movie f]
[--test-star-save out.svrmod]`.

Launcher test aid: `--mods-add <game> <file.svrmod>`.

Verified (2026-10-03):

- **List:** 16 mods (ids 59-69, 111, 114, 121, 127, 128) and the managers
  show in the list, with renders and the MODDED badge.
- **Matches:** a mod on a disc id (114) plays.
- **Full pipeline:** Mod Maker → .svrmod → launcher → game, an Orton-based mod
  with its own theme (played via USER PLAYLIST) and movie (on the titantron).
- **Saves:** edits survive.
