# Media mods (arenas branch)

Media packs replace the game's own media: superstars' entrance videos, themes
and renders, arena screens, the menu music and any game sound.

- **Code:** `src/media_mods.cpp`, plus `music.cpp` (host sounds) and
  `arena_mods.cpp` (`SetArenaDefault`).
- **Making them:** the Mod Maker's Titantron videos, Menus & renders and Audio
  pages. One pack covers all three.
- **Installing them:** the launcher's Mods tab, on PC and Android
  (`type=media`).

## A pack

`<game>/Mods/Media/<pack>/` holds `manifest.txt` (`type=media`, `id`, `name`,
...) and the files it names. A `disabled` file turns a pack off. Later packs,
by folder name, win.

| line | |
|---|---|
| `video.<id>=<file.bik>` | that superstar's entrance video: 320 x 320 Bink, ~30 fps, no audio (the launcher's Movies tab makes them) |
| `theme.<id>=<song>` | his entrance theme (.mp3 .m4a .aac .wav .flac .wma .ogg) |
| `render.<id>=` / `bust.<id>=` / `icon.<id>=` | his menu pictures: 512 x 512, 256 x 256 and 64 x 64 DXT5 |
| `arena.<nn>=<bgNN.pac>` | arena nn with its screens' pictures replaced |
| `menu_music=<song>` | the menu music |
| `sound.<event>=<file>` | any game sound, by event name without `Play_` |

For sound names, `SVR2011_TEST_NAMECALL=1` logs every event the game posts.
Chants are `SVR10_Chant_*` / `SVR11_Chant_*`; the Cena chant is
`SVR10_Chant_Sena_001`.

## How each part works

Background research is in scratchpad re_videos, re_renders and re_audio.

### Videos and themes

The videos are copied into Custom Movies and get user movie ids
(user_movies.h). The themes are copied into Music, where they become USER
PLAYLIST entries.

When a match sets up its wrestlers (`sub_828B5BC0(record, desc)`: the id at
desc+0, the profile copy at record+260), each wrestler's entrance block
(+0x1C0) gets:

- music +0x10 = 254, with the playlist name at +0xCC;
- movie +0x12.

Only the match's copy changes; the roster and the saves keep the game's. The
data is used instead of the movie id because ids are shared: 255 is used by
dozens of characters.

### Renders

An overlay pac, `Mods/SuperstarOverlay/media.pac`, holds:

- RSFA, RSFB and RSFC entries, one per attire key;
- RENU/SSFD, the face icon bank, rebuilt with the replaced icons.

It is mounted with the superstar mods' files. Their file-name resolver hook
(`sub_826B87B0`) asks for these names instead of SSFA/SSFB/SSFC and
MENU/SSFD. The first match in the archive list wins, so a pac mounted later
couldn't simply override the originals.

### Arena screens

There is no arena video. Between entrances the screens flip through about 10
textures, `<prefix>_anim00..09`, in `bgNN.pac` (bg17's are `wss_anim`, 256 x
128).

- **Mod Maker:** turns a .bik (10 frames, evenly spread) or a set of pictures
  into those textures and writes the retextured arena.
- **Game:** arena_mods plays that file for that arena (`SetArenaDefault`) when
  no custom arena is on its tile.

### Sounds

Every game sound is an event posted by name: `sub_82BEC030(name, object,
...)` returns a playing id. For a replaced `Play_<event>`, the event isn't
posted. Instead:

- The file plays on a host voice (music.h `HostSound*`: a pool of XAudio2
  players), tied to the event's game object.
- The hook returns a made-up nonzero playing id.
- Menu music loops; other sounds play once.

Stops and pauses still reach the game's audio, and also act on the host voices:

- `Stop_*` on that object;
- the global `Stop_Menu_Music`, `Stop_Entrance_Music`, chants, SFX and
  `All_Audio`;
- `Pause_start` / `Pause_Resume`;
- `Pause_ALL_Audio` / `Resume_ALL_Audio`;
- the object's release (`sub_82BB6898`).

The game's "is the object still sounding" check (`sub_82BB69E8`) answers yes
while a host sound plays on that object. Otherwise music players and chants
think the sound ended at once.

Volume follows the game's music, SFX and voice settings (floats at
*(0x82EC4C18)+36).

Entrance themes go through USER PLAYLIST, not this path, so the game's own
timing for the entrance music (prepare, then resume) stays.

## Tested (2026-10-03)

- **Orton's replaced video and theme in a match:** the user movie (id 702)
  played on the titantron, and the theme was played as a USER PLAYLIST
  (log).
- **Renders:** the select screen's request for Orton's render (SSFB/0161)
  and the face icon bank resolved to the pack's. Screens that show the 2D
  renders (Universe, online, match cards) were not reached in a scripted
  test, so they are not seen on screen.
- **Menu music:** played on the host voice once (not restarted every frame)
  and stopped when the match loaded.
- **The cursor sound:** replaced by a clip, one play per move.
- **Arena screens:** the Mod Maker built bg17 with all ten `wss_anim` frames
  replaced; the launcher installed it; the game loaded it for the Superstars
  arena. Not seen on screen: in every scripted run the screens showed
  entrance videos. They show the flip-book only when no video plays.

## Limits

- Entrance themes and videos replace a superstar's in every match (not per
  match type).
- A replaced sound plays at the game's volume settings, without the game's 3D
  placement or mixing effects.
- Sound replacements need the event's exact name.
