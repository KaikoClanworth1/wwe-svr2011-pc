# Superstar mods (arenas branch)

New playable characters from `<game>/Mods/Superstars/<folder>/`. Code:
`src/superstar_mods.cpp`; the select screen's EXTRA list: `src/managers.cpp`.

## A mod folder

| file | |
|---|---|
| `ch.pac` | the character's model pac (EPK8, as `pac/ch/chNNN.pac`) |
| `manifest.txt` | `type=superstar`, `name=` (full name, 31 chars), `short=`, `base=<id 100-321>` |
| `disabled` | (optional) turns the mod off |

`base=` is the disc superstar the mod starts from. It must be one of the 91
playable ones: those with select renders in `pac/DLC_HD.pac` (`SSFA` entries).
The base supplies the stats, moves, entrance and select render. 106, for
example, is not playable: no render, Overall 2.

## How it works

The game has 20 DLC character slots, ids 50-69. Real DLC uses 51-58, so mods
take **59-69: at most 11 mods**. For each mod, at startup:

- **Model.** `ch.pac` is copied to `Mods/SuperstarOverlay/chNNN.pac`, with its
  `EMD` names renamed from the base's id to the slot's (`%06d%02d`).
- **Renders.** The base's `SSFA/SSFB/SSFC` select renders from `DLC_HD.pac`
  are written to `ssfNNN.pac` under the slot's id (`attire*1000 + id`).
- **Mounting.** Both pacs are mounted with the game's `sub_826A1780(path, 0)`
  once its file system is up.
- **Records.** The base's 260-byte roster record goes to the slot, with the
  mod's names, selectable, the DLC flag and its own "same person" id. So does
  the base's 1056-byte profile, which holds the entrance (number at match
  setup), moves and so on. Both are copied:
  - again after `CHAR/DAT`, `CHAR/PRO` and every save load;
  - before every roster list is built.

  The profile is checked on its own: `CHAR/PRO` and saves load after the
  records. Without that the mod kept the empty slot's entrance 4000, and the
  match hung on a black screen in the entrance.
- **Owned and listed.** DLC-flagged ids must be owned, and mods are. The DLC
  tile's filter (`sub_8244A128`) leaves mods out: they are listed under the
  M (EXTRA) tile, after the managers.

## The MODDED badge

The select panel shows DLC characters with the "DOWNLOADABLE CONTENT" badge.
That is the texture `DLCtex01` (menuHD.pac `MENx/CHSI` 7001; 256 x 64 DXT5,
one per language). It is shown by layout node `cursor+312` in `sub_82466830`.

- **Tracking.** A hook on that panel's DLC check (`sub_828B6CD0` called from
  0x82466E40) records which panels hover a mod.
- **Finding the texture.** A background thread finds the texture in guest
  memory, by the 16-bit-swapped DXT5 blocks of any language's original. It
  scans once per select screen visit and keeps the address while the content
  still matches.
- **Swapping.** The thread writes the "MODDED" pixels while any panel hovers
  a mod, and the language's original otherwise. Both panels share the
  texture, so a mod wins.

The art comes from `tools/make_modded_badge.py`, which writes
`src/modded_badge.inc` (RGBA). The game encodes it to DXT5 at startup.

## Known limits

- The profile is copied from the base on every load. Edits made in game to a
  mod's moves or entrance don't stick.
- There is no name call: the voice cue `WrestlerVoice_%04d` is per id.
- Entrance, titantron and victory are the base's.
- Only the base's attires that `ch.pac` holds are available.

## Tests

`tools/superstar_test.ps1` runs an exhibition one on one: P1 opens the M
tile, steps `-Down` entries (the 5 managers come first) and picks the mod.
Options:

| option | effect |
|---|---|
| `-Com` | COM's pick goes through the M tile too |
| `-NoSkip` | lets the entrances play |

Screenshots go to `runs/<Name>_*.png`.

`SVR2011_TEST_VFS_LOG=1` logs every virtual file lookup that finds nothing,
once per name, with its caller. This is how the missing entrance turned up.

Verified (2026-10-02), Modded Test from Chris Jericho (104) as id 59:

- the list entry, MODDED badge, render and Overall 94 are right;
- COM opens the EXTRA list (player 1);
- the entrance plays with the "MODDED TEST" nameplate;
- the match plays on the slot's own model.
