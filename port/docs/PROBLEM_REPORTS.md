# Problem reports

We don't ask players for their saves or mods. What a report needs has to be in
the "Report a problem" zip (launcher `report.c`, `SvR2011 Launcher.exe --report
<game folder> [description]`) and in the logs.

## The zip

| file | |
|---|---|
| `report.txt` | the player's words, then the launcher version, Windows version, date |
| `logs/` | the newest game's log parts (repeated fault lines condensed) and the two games before |
| `crashes/` | the three newest crash reports (host stack, guest registers) |
| `svr2011.toml` | the settings, account token and password removed |
| `saves_check.txt` | each `NNCreateSuperStar.cas`: size, save slot, the attires with a picture, its three checksums and SaveData.dat's copy for its slot, "agree" or "DON'T AGREE"; SaveData.dat's own checksum. Never the saves |
| `mods_text.txt` | each superstar mod's `manifest.txt`, `moves.txt` and `threads.txt` (64 KB each, 512 KB in all). Never models or pictures |

IP addresses are blanked in the logs (x.x.x.x).

## Log lines for reports

- `caw saves: NNCreateSuperStar.cas - slot N, attires with a picture 1+2, checksums agree`
  (`src/caw_check.cpp`, at start, one line per Created Superstar, then a count). A
  warning when the three copies or SaveData.dat's record differ, or the size is
  wrong: the game calls such a CAW damaged or leaves it out (at start it stops on a
  dialog). The value itself isn't checked: the port's logo cache keeps part of a
  CAW outside the file, so the on-disk byte sum doesn't match for every good save.
- `match picks: person 0 CAW slot 3 attire 2, person 1 id 110 attire 1`
  (`src/test_match.cpp`, as the match is built): who was picked, with which attire.
- `match: rule .., arena .. - <names> (person n, team t)`: the match's people.
- `match state (<why>): ...` then one line per person: name and superstar id, its
  mod folder or "not a mod", and its last move (id, moveset slot, whether a move
  pack's / the game's / missing / without a move record, how long ago)
  (`src/match_diag.cpp`). Logged once when the world update is stuck for 3 s
  (with the guest threads' back chains), or when the match clock runs 30 s with no
  wrestler asking for a move.
- Mod Maker session log (`<game>\logs\modmaker_*.log`, part of the Mod Maker's own
  report): `CAW pictures: <file> (slot N) attire 2: ... checksum A -> B; SaveData.dat
  record ... file sum ...` for every CAW pictures save (its first versions go to
  `Saves\.backup`), and `model preview: N models (...), M meshes, V vertices, T
  textures; textures the meshes name but the model lacks (drawn black): ...` for
  every model the 3D preview loads.

## Test aids

- `SVR2011_TEST_MATCH_STATE=<s>`: the `match state` lines once, `<s>` seconds into
  the match clock.
- `SVR2011_TEST_ROSTER_DUMP=1`: each match person's roster record (260 bytes; +0x20
  is the superstar id, +0x22 the name).
