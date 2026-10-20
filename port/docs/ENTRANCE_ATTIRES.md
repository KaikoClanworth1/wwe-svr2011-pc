# Entrances: how 2011 picks one, per-attire entrances, entrance outfits

Research for checklist D5 (entrance per attire, entrance outfits). Game side:
`src/superstar_mods.cpp` (manifest `attire<N>_entrance=`, hooks below). Converters for other
games' entrances: `tools/entrance_port.py`, docs/OTHER_GAMES_ENTRANCES.md.

Tags: [C] confirmed in code or data or in game, [L] likely, [G] guess.

## 1. The profile's entrance block [C]

E = profile + 0x1C0 (284 bytes). The pac (`ch/chEtc.pac` CHAR/PRO) stores the u16 values
little-endian; guest memory is big-endian (`superstar_mods.cpp` writes the high byte first).
The entrance manager keeps a copy per match slot: S = E + 4 at `mgr + 36 + idx * 280`
(manager `sub_8274FD18()`, copied by `sub_82755FB8`).

| E | S | meaning |
|---|---|---|
| 0x04-0x0D | 0-9 | default copy of music, movie, entrance, champion, multi-title |
| 0x10 | 12 | music (254 = USER PLAYLIST, name at 0xCC); getter `sub_82756468` |
| 0x12 | 14 | movie (700-899 = USER MOVIES); getter `sub_82756648` |
| 0x14 | 16 | **entrance number**; getter `sub_82756538(mgr, idx, part 0)` |
| 0x16 | 18 | **champion entrance**; getter `sub_82756718` |
| 0x18 | 20 | **multi-title entrance**; getter `sub_82756768` |
| 0x1C + p*34 | 24 + p*34 | created-entrance part p (0-4), its number at +14 |
| 0xC7 | 195 | 0 = the single number at 0x14; else a created entrance (per-part numbers) |
| 0xC8 / 0xC9 | 196 / 197 | flags of the normal / champion version (stage effects) [L] |
| 0xCC | 200 | USER PLAYLIST name (UTF-16BE) |

A created part (34 bytes, big-endian u16s in memory; seen in a save's ADVANCED CREATION entrance,
Alicia Fox 297, 2026-10-20): +8 a value of the CAE list category 14 (values 100+, one per
superstar - 157 = ALICIA FOX) [L: the scene's lighting or camera set], +10 the pyro (category 12,
5000 = NONE, 5042 "STAGE BROTHERS OF DESTRUCTION"...), +14 the motion (10000 + 1000p + entrance
number: categories 15 / 7 / 8 / 9 / 10), +16..+22, +26, +28, +32 = 9999 (the default), the rest 0.
The CAE lists themselves: `pac\DLC.pac` ENEX/LSD, per category u16 count then 6-byte rows {value,
string id, unlock} (docs/CAE_MOD_MOTIONS.md).

**`cae=<file>`** (superstar mods): a whole 284-byte block as the game holds it (big-endian, e.g. a
save's created entrance); its +0x1C..+0xC9 (the five parts and the created-entrance flags) is
copied into the mod's profile while the slot's parts are its base's or ones the port set before
(`.applied` `cae=<hash>`), so a player's own CREATE AN ENTRANCE edit stays. Test aid:
`SVR2011_TEST_DUMP_ENTRANCES=<file>` writes every profile's block (`id hex`) once the profiles
are set up. Played 2026-10-20: Rock Chain with Alicia Fox's created entrance walked out with it.

Examples: Cena 139 / 602 John_Cena_Champ / 602; Kane 107 / 600 / 610; Batista, Rey (123),
Undertaker (103): own / own / 610 "General Champ A Normal"; divas multi-title 614.

## 2. How a match picks the number [C]

`sub_8275ED10` walks the entrance queue (68-byte entries at `*(0x82E3DDFC) + 5872 + k*68`):
a single wrestler -> `sub_8275EB48`, a team -> `sub_8275E3D8` (team record +26), others
`sub_8275E330`. For each of the 5 parts: `n = sub_82756538(mgr, idx, p)`, then
`n = sub_8275E480(this, n, title1, title2, idx)`, which

- turns Undertaker 103 into 410 ("with Druid") on some arenas;
- with one title takes the champion number (if not 0), with two the multi-title one;
- on slope-ramp arenas 518 -> 519, 729 -> 730;
- **Rey Mysterio's "Evil" attire:** `sub_82758718(mgr, idx, 123, 1)` (his id, attire 1, still
  his stock entrance) -> **512** (a plain walk-out). This is the only attire-dependent entrance
  in the game, and it is code, not data; Swagger's (267) attire 1 changes only music (1010)
  and movie;
- returns its input when no `/EVP?/n` script exists (so a missing number = no entrance), and a
  changed number in 1-699 resets 0xC7 (a champion entrance drops a created entrance).

Match slot record: 52 bytes at `*(0x82E3DDFC) + 132 + idx * 52`; +16 the costume key
`id * 100 + attire * 10 + kind`; +45 / +47 the title bytes (255 = none).

## 3. `attire<N>_entrance=<number>` (superstar mods) [C]

Hooks on the three getters (`sub_82756538` part 0, `sub_82756718`, `sub_82756768`): when the
slot's costume key is a mod with an entrance for that attire (mod attire N = attire digit N-1),
all three return it, so the attire walks out with it with or without a title. Not when the
wrestler has a created entrance (S+195) or a team entrance (`sub_82756260(mgr, idx)`).
`entrance=` still sets all three of 0x14/0x16/0x18 (else a mod holding a title would play its
base's champion entrance).

Checked in game: Sabu with `attire1_entrance=139` walked out with John Cena's entrance (his
props, salute, ring pose), the match followed; log `superstar mods: Sabu attire 1: entrance 139`.

## 4. Entrance outfits [C]

Model pac entries are named `%06d%02d`: id, attire * 10 + kind. Kind 2 = the match model,
**kind 4 = the entrance model**. `sub_828C3AC8` picks the entrance model per member: kind 4 if
that file exists for the attire, kind 1 for Created Superstars (ids < 50) [L], else - played in game
2026-10-13, a Cena mod in attire 2 - **attire 1's kind 4** (`..04`) when it exists, not the attire's
kind 2: with no `..14` Cena entered attire 2 in attire 1's cap and shirt, with an added `..14` (a copy
of his attire-2 model) without the cap [C]. So an added kind-4 entry for another attire is worn, and
leaving an attire's gear out needs an explicit kind 4 of its own (e.g. a copy of its kind 2);
`sub_82750BD8` builds the entrance character from that key. Stock kind-4 entries: 00010304
Undertaker, 00013104 Punk, 00013904 Cena, 00014504 HBK, 00017504 Morrison (coats, hats).
No event that toggles costume parts was found: the outfit stays for the whole entrance and the
match model replaces it at the cut [L].

For mods: `RenameModel` keeps the kind digit and `AttireMask` counts only kind 2, so **a kind-4
EMD entry in a mod's ch.pac (or attireN.pac) already works as that attire's entrance outfit**.
A mod built from Undertaker / Punk / Cena / HBK / Morrison's pac inherits that superstar's
entrance coat unless the entry is dropped (a Mod Maker check).

Per-(id, attire) prop and belt attach offsets come from a text table read by `sub_82761C88`.

## 5. Open

- Per-attire champion entrances, music and movie (`attire<N>_champion=` etc. through the same
  getters, and `sub_82756468` / `sub_82756648` like the game does Swagger) - not added yet.
- A per-attire number whose script is not installed gives no entrance (the game's fallback);
  the Mod Maker should only offer installed numbers (`entrance_port.py free-numbers` lists the
  used ones).
