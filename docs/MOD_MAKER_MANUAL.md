# SvR2011 Mod Maker - manual

The Mod Maker makes mods for the WWE SmackDown vs. Raw 2011 PC port: new
arenas, backstage rooms, superstars, move packs, crowd signs and media packs,
all as `.svrmod` files the launcher's Mods tab installs. It also shows what is
inside the game's files, plays the game's animations, and swaps the pictures
of your Created Superstars.

The game's own files are never changed. Mods are kept in `<game>\Mods` and
laid over the game while it runs.

## Getting around

- **Title bar:** New (a fresh project of one type), Open (a project or a
  mod), Save / Save as (the project), the recent list, and the game folder
  button on the right (where the arenas, superstars and pictures are read
  from - normally set by the launcher).
- **Rail:** the pages. MAKE pages each build one kind of mod. SAVES edits your
  Created Superstars' pictures. LOOK pages show the game's assets, animations
  and pictures. HELP is this manual.
- **Status line:** the last message, what a long job is doing, the problem
  count, and Log, which folds the full log out.
- **Problems:** every mod page checks its work as you go. Red problems keep
  Save and Install off until fixed (hover the greyed buttons to read them);
  yellow ones are warnings you confirm once.
- **Save as mod / Install into game / Test in game:** the row at the bottom of
  every mod page. Save writes a `.svrmod` to share; Install writes the mod
  straight into the game folder; Test installs it and starts the game.
- Shortcuts: Ctrl+S saves the project, Ctrl+O opens one.

## Projects

A project (`.svrproj`) is the work on one mod page: every setting and a copy
of every file you picked, in one file you can come back to, move or send to
someone. A `.svrmod` you made earlier opens too (Open accepts both), so a mod
can always be reopened and changed - even by someone else.

Each MAKE page is its own project; switching pages switches projects. The
title bar shows which project is current, with `*` when it has unsaved
changes. Closing the app asks to save.

## Arena

A new arena on the arena select pages, made from one of the game's own.

- **Start tab:** pick the arena to start from. Double-click it or press
  **Open in the 3D editor** to edit it; **Start an empty arena** keeps only
  the ring, the floor and the ringside parts so you can build the rest;
  **Export to Blender** / **Import from Blender** go through an FBX file
  (1 Blender unit = 1 metre; textures come along as PNG).
- **Details tab:** the name shown on the select page, the author and
  version, the banner (any picture, shown at 256 x 128) and the loading
  screen (1024 x 512) shown while a match in it loads.
- **VS screen tab:** the pictures of the screen behind the two superstars
  before the match. Your arena uses the theme of the arena it plays in place
  of; replace any of its pictures.

Your arena plays in the place of the arena you started from (its room, its
VS screen style); on the select pages it sits after the game's arenas.

Limits: each arena file must stay within the room the game gives the
original; the Mod Maker shrinks the biggest textures when needed (the Size
budget panel in the editor shows how full it is). Ringside and entrance parts
move at most 50 cm because the game's collision is not rebuilt; rigged parts
(ropes, flags) can't move.

## 3D editor

The arena in 3D. Right drag orbits, middle drag pans, the wheel zooms, F
focuses the selection, Ctrl+Z undoes.

- **Tools:** Select (Q), Move (W), Turn (E), Scale (R); Snap rounds the
  moves. Ctrl+D duplicates, Delete hides a game object or removes one you
  added.
- **Outliner:** every object by zone (Ring, Ringside, Entrance, Free); tick
  to show or hide; type to find one.
- **Object:** position, turn and scale; the texture, or a picture of yours.
- **Add:** a box, or an object from an OBJ / FBX file (in game units, 10 cm).
- **Library:** any object from any of the 20 arenas, placed where it was or
  at the view centre.
- **Ring Kit:** each rope's colour, picture or absence, the turnbuckles and
  corner pads, the low rope's height and the gap between ropes. Fewer ropes
  change the gameplay (the game adapts: no rope breaks, no rebounds with
  none).
- **Lighting:** presets and a colour / strength, and the crowd in the seats.
- **Size budget:** file and memory against the original arena.
- **Test in game:** installs the mod and starts the game.

## Backstage

A backstage brawl room rebuilt. The game's seven rooms are in one file; your
mod rebuilds one and the game plays it in that room's backstage brawls. Keep
the floor and walls where they are (the game's cameras expect them); the
cars, crates and weapons are placed by the game and are not shown.

**An area of its own** instead gives the mod a row in ONE ON ONE ->
BACKSTAGE (after the room's row) and plays it only there: give it a row
name, a fight box (centre x, z and half sizes in game units), a camera
distance cap and minimum height for small areas, and optionally a gimmick
pac (the area's own cars and props, as `tools/svr08_gimmick.py` makes).

## Superstar

A new playable character under the M tile of the character select (up to 50
mods).

- **Name** (up to 31 letters) and a short name for the match screens.
- **Fighting style:** decides the move-set, the entrance motions and the
  starting attributes (from one of the game's superstars underneath). Move
  the attribute sliders as you like; the moves can be changed in the game's
  CREATE A MOVE-SET.
- **Name call:** what the announcer and commentators call the superstar: one
  of the Created Superstar nicknames, or a recording of the name.
- **More:** the entrance (any superstar's, or Jeff Hardy's SvR 2010 one the
  game ships unused), an announcer name the game's sound banks have clips of
  (JEFFHARDY), the abilities, a moves list (`0xOFF=<move id>` lines over the
  style's move-set) and a move pack folder carried inside the mod.
- **Files:** the model (`ch.pac`, required - one of the game's `pac\ch\chNNN.pac`
  or a converted one), the theme song, the entrance movie (320 x 320 Bink from
  the launcher's Movies tab), up to three more attires (another pac's first
  attire each, with a name), up to four crowd signs the fans hold, a name
  recording, and the select picture (a PNG with a transparent background is
  best: the render, the bust and the face icon are made from it).
- The preview on the right shows the model playing its idle stance - a quick
  check that an imported model works. Drag to turn it.

Theme songs play at full level while the game's own are about 7 dB quieter:
make yours about -24 LUFS.

## Moves

Moves the game doesn't have, in a move pack: the motions (attacker, victim,
props and cameras) and the move's table records. Open a pack folder
(`pack.txt` + `motions\`), check it move by move - each move's name from the
game's table, its tracks, frames and whether it can go in a move-set - and
save it as a mod of its own (installed into `Mods\Moves`) or hand the folder
to a superstar mod (Superstar > More > Move pack folder).

`tools\svr10_moves.py` ports moves from SvR 2010 and `tools\movepack.py`
writes the folder. The game merges every enabled pack into copies of its
files when it starts (a few seconds the first time). Format: docs/MOVE_PACKS.md.

## Crowd signs

Pictures the crowd holds up in every match, with the game's own. Add any
pictures: each is fitted onto a white 128 x 64 board. A superstar's own
signs (held by that superstar's fans) go on the Superstar page instead.

## Media

One pack that replaces the game's own media; every item is optional.

- **Titantron videos:** a superstar's entrance video (320 x 320 Bink), and
  an arena's screen pictures between entrances (10 frames from a video, or
  pictures shown in turn).
- **Menus & renders:** a superstar's render, bust and face icon from one
  picture.
- **Audio:** a superstar's entrance theme, the menu music, and any game sound
  by its event name (without `Play_`; e.g. `SVR10_Chant_Sena_001`).

## CAW pictures

The picture the game shows for a Created Superstar in its lists (the one
FINALIZE made) and the head the Community Creations preview uses, per attire.
Replace either from a picture of yours (a transparent background fits the
lists best). The change goes into the save in the game's Saves folder with its
checksums redone; a `.bak` copy of the save is kept the first time. Close the
game first.

## Game assets

Everything in the game's pac files, read-only. Expand a folder, a file, a
group and an entry; nothing is read until you look at it. Each thing shows as
what it is: textures (with zoom), texture bundles, models (meshes, nodes,
textures), motion banks (every motion, with the move's name), string tables
(every text by id), roster records, text, or bytes. Export writes any of them
out (textures as PNG too).

## Animations

A superstar playing the game's motions. Pick who (a superstar, or any
`ch.pac`), optionally a dummy for the other person, then:

- **Watch here:** the menu motions - stances, taunts, the entrance's ring
  part, finisher previews, abilities - play in the view. Play / pause, step,
  scrub, speed, loop; the camera turns (left drag), tilts (right drag), pans
  (middle drag) and zooms (wheel).
- **In the game:** the match moves (grapples, strikes, a move pack's moves)
  are in a format only the game plays. Pick one by name and Play in game
  starts a test match - the superstar against the dummy, both
  computer-controlled - where the move is done every few seconds. Close the
  game when done.

## Icons & renders

The game's pictures by what they are for: each superstar's renders and face
icon, the arena banners, VS screen themes and loading screens, and the
crowd's signs. Export any as PNG; the pages that replace them are named.

## Installing and sharing

A `.svrmod` is a zip (store it, send it, put it on the Project Index). The
launcher's Mods tab installs it (+), lists it with a tick to turn it on or
off, and removes it (-). Mods also install from the Mod Maker directly
(Install into game). The game picks mods up when it starts.

Formats: docs/SVRMOD_FORMAT.md. The game side of every mod type:
port/docs/SUPERSTAR_MODS.md, BACKSTAGE_MODS.md, MEDIA_MODS.md,
CROWD_SIGNS.md, MOVE_PACKS.md, docs/ARENA_MOD_MAKER_PLAN.md.
