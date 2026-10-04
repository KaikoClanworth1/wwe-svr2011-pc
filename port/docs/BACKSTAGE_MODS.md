# Backstage mods (arenas branch)

A backstage mod is a rebuilt backstage brawl room.

- **Game:** `LoadBackstage` in `src/arena_mods.cpp`.
- **Making them:** the Mod Maker's Arenas page → Backstage areas → Open area
  in Arena Editor.
- **Installing them:** the launcher's Mods tab (PC and Android).

## The game's backstage

All seven backstage brawl rooms, the corridor and the Road to WrestleMania
rooms are in one file, `pac/bg/bg78.pac`. There is no per-room file and no
room table. The match rule picks the room (1v1 rules 0x1B-0x21), and about 25
functions test the room with 7 hard-coded predicates.

A room is a set of model ids in bg78. Entry 50001 holds the flags.

| room | rule (1v1) | models | the game's box (sub_8224EF28) |
|---|---|---|---|
| Parking lot | 0x1B | 0-19 | 125 0 -410, half 38 x 50 |
| GM's office | 0x1C | 20-39 | -338 -8 217, half 30 x 27 |
| Locker room A | 0x1D | 40-59 | 290 -8 -190, half 34 x 34 |
| Locker room B | 0x1E | 60-79 | 326 -8 241, half 34 x 34 |
| Large locker room | 0x1F | 80-99 | 460 -8 -76, half 25 x 25 |
| Interview area | 0x20 | 160-179 and 140-159 | 0 -8 141, half 30 x 30 |
| Catering area | 0x21 | 140-159 | 20 -8 -190, half 45 x 30 |

About the box column:

- `SVR2011_TEST_BOX_LOG=1` logs these values.
- They are not the area the fighters are kept in. In a parking-lot match the
  fighters roam well beyond it.
- The match cameras do see its middle, so the test edit puts its box there.

The cars, crates, bins and other weapons are placed by the game. They are
not models in bg78 (the 1000+ object ids listed in the research have no
models there), so the editor can't show them and they stay.

## A mod folder (a .svrmod is a zip of one)

| file | |
|---|---|
| `manifest.txt` | `type=backstage`, `id=`, `name=`, `author=`, `version=`, `area=<0-6>` |
| `arena.pac` | the whole bg78, with one room rebuilt |
| `disabled` | (optional) turns the mod off |

Mods install into `<game>/Mods/Backstage/<id>/`. At start the game takes the
last enabled folder by name and redirects BG78 to it (`SetArenaDefault(78)`).
Only one backstage mod plays at a time; the log warns when more are
installed. Every backstage room and Road to WrestleMania read the mod's
bg78, which is why the build keeps the other rooms as they are.

## The Mod Maker

Open area in Arena Editor loads bg78 and shows only that room
(`editor::SetArea`).

- **Hidden:** models of other rooms, effects, and ceilings and light rigs.
  "Show ceilings" brings the ceilings and rigs back.
- **Ring Kit:** none.
- **Outliner:** lists the room's models under "Area".
- **Added objects** are attached to the room's floor model and take its
  material. Attached to another model of the room, they can be invisible in
  game: park_obj02's first mesh is a decal.
- **Build:**
  - The light tint goes to the room's models only.
  - There is no crowd or ring step.
  - FitFile may shrink only the room's own textures. Halving textures shared
    with other rooms froze the interview area at load ("world update stuck").
  - Changed models' node spheres (world space, what the game culls a model
    by) grow to hold their meshes.

## Tests

- **Mod Maker:** `--backstage <0-6>` opens a room.
  `--backstage 0 --test-edit-save runs\test_back.svrmod` adds a 1.5 m box
  next to the box centre and saves, writing its log to `<file>.log`.
- **Launcher:** `--mods-add <game> <file.svrmod>`.
- **Game:** `SVR2011_TEST_RULE=1B` makes ONE ON ONE NORMAL play that rule
  (1B-21 are the rooms), so `superstar_test.ps1 -Down 0` plays a backstage
  brawl.

Verified (2026-10-04):

- **Parking lot:** the log shows "BG78 -> Mods/Backstage/test_edit/arena.pac",
  and the added box stands in the fight with the floor's texture.
- **Interview area:** with that mod installed it plays (after the FitFile
  fix).
- **Arena mods:** still play after the JBOY reader fix.

Not done: two backstage mods for different rooms can't be combined. A merge
at start (each mod's room models into one bg78) would allow that.
