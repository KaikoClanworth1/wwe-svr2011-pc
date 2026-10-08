# Main menu backgrounds

The big superstar pictures behind the main menu, and the port's MY WWE -> OPTIONS -> BACKGROUNDS page that switches them on and off (`port/src/backgrounds_page.cpp`).

## Where they are

- **Location:** `pac/menu/menuHD.pac` holds groups `MENU/EB00` to `MENU/EB11`. `menuSD.pac` holds the same 12 at SD size. There are no language copies.
- **Group contents:** each group has a texture bundle `7001` and an animation `7002`. The animation is 16208 bytes and the same in all 12.
  - `EXHBG2` is the superstar picture: 1024 x 512 DXT5, different in each group.
  - `EXHBG` is the "SmackDown vs RAW 2011" logo: 512 x 256 A8R8G8B8, the same in all 12.
- **Layout:** `menu.pac` `MENU/EXBL/0`, with parts `EXHBG2_a/b/c`, `EXHBG_a/b/c` and `Exhibiton_BG`.
- **Names:** the game's data has none (only `EXHBG2` / `Exhibiton_BG`). The page's names come from the pictures:

| # | Group | Picture | Page name |
|---|-------|---------|-----------|
| 1 | EB00 | The Miz with the microphone (shown first after start) | THE MIZ |
| 2 | EB01 | a high flyer mid-flip, red tights | BACKGROUND 2 |
| 3 | EB02 | John Cena, screaming close-up | JOHN CENA |
| 4 | EB03 | CM Punk with the Money in the Bank briefcase | CM PUNK |
| 5 | EB04 | Edge with the WWE Championship | EDGE |
| 6 | EB05 | Triple H, water-spit entrance | TRIPLE H |
| 7 | EB06 | Shawn Michaels, ladder / trash can dive | SHAWN MICHAELS |
| 8 | EB07 | Rey Mysterio, arms up in the ring | REY MYSTERIO |
| 9 | EB08 | Randy Orton, from behind | RANDY ORTON |
| 10 | EB09 | a dark-haired superstar shouting | BACKGROUND 10 |
| 11 | EB10 | Jack Swagger, arms up | JACK SWAGGER |
| 12 | EB11 | The Undertaker, hooded entrance | THE UNDERTAKER |

## How the game chooses: a cycle, not random

- **Strings:** `"/MENU/EXBL"` is at 0x8201BB4C and `"/MENU/EB%02d"` at 0x8201BB58.
- **The background object:** there is one at a time, with its pointer at 0x82E3DB88.
  - The main menu's state machine (`sub_82445B38` → `sub_82445A90`) creates it and sets the menu's +2232 = 1 ("this menu made it"). It then calls `sub_82444928` → constructor `sub_82458F58`.
  - The constructor reads the picture number from `*(0x82E3DDFC) + 8680` into object +68, clamped to 0..11.
- **Update** `sub_82459348`:
  - state 1 builds `"/MENU/EB%02d"` from +68 and loads `7001` / `7002`;
  - state 2 loads the `EXBL` layout;
  - state 4 is shown;
  - states 5 → 6 destroy it.
- **The number:** game data `+8680` is set to 0 when the data struct is made (`sub_8258A9B0`), so every start shows EB00 (The Miz) first. It isn't saved.
- **Moving on:** the main menu's destructor (`sub_82444A10`, when +2232 = 1) calls `sub_8258A990`, its only caller. It adds 1 and goes back to 0 at 12. So the picture changes each time the player leaves the main menu (into a mode, a submenu screen, and so on), in order EB00, EB01, ... EB11, EB00.
- **The 12 is hard-coded twice:** in `sub_8258A990` and in the constructor's clamp.

## Unused / cut pictures

None. The pac holds 12 groups and the cycle uses all 12. Other picture sets in the files belong to other screens, not the main menu:

- `RWBG/0000-0029` and `RWBG/9000`: Road to WrestleMania renders.
- `RSCP`: Universe scene pictures.
- `TITI`: the title screen's Cena / Orton picture.
- `game2dHD GA2D/BASE/3/3/0`: an in-game 2D frame named "XEXHBG".

Adding new pictures would be feasible, since each is one 1024 x 512 DXT5 picture plus the shared logo and animation. It needs:

- new `EB12`+ groups in menuHD.pac and menuSD.pac;
- both 12-limits lifted (`sub_8258A990` replaced, the constructor's clamp redone).

The port does it without new groups (next section).

## Custom pictures

- **Files:** `<game>/Mods/Backgrounds/<id>/`:
  - `picture.dds`: 1024 x 512 DXT5, one level (EXHBG2's size and format);
  - `manifest.txt`: `type=Background`, `id`, `name`, `version`, `logo=0|1`;
  - a `disabled` file turns it off (as other mods).
- **Made by:** the PC launcher's Mods tab, **Add background picture…** (`launcher/backgrounds.c`; test flag `--bg-add <game folder> <picture> [logo]`), and the Android app's Mods page, **Add a background picture…** (`Backgrounds.java`). A `.svrmod` with `type=Background` and `picture.dds` installs as one too.
- **In the cycle:** after the 12, as numbers 12, 13, ... in game data `+8680` (sorted by folder name).
  - `sub_8258A990`'s hook wraps at 12 + the custom count.
  - `sub_82458F58`'s hook reads the number from game data (the constructor's +68 copy is clamped to 0-11). For a custom picture it sets +68 = 0, so EB00 loads.
- **The swap:** EB00's `7001` set is BPE-packed. As the game unpacks it (`sub_826AEF10`, arena_mods.cpp's hook calls `BackgroundsDecoded`), while a custom picture's background is being made:
  - `EXHBG2` gets the custom picture's blocks;
  - with `logo=0`, `EXHBG` (the logo, A8R8G8B8) is zeroed (transparent).
  - No pac change and no memory scan.
- **The page:** the custom pictures are rows after the 12: A on / off (their `disabled` file), X shows / hides the game's logo on one (`logo=` in its manifest, from its next showing).

### Where the picture lands on screen

Measured with a grid picture (the menu's layout `EXBL` draws it in parts; the picture doesn't move). In the game's 1280 x 720 frame, texel (tx, ty) is at:

| | Screen position |
|---|---|
| x, tx < 320 (left part, stretched) | -59.75 + 1.8511 tx |
| x, tx ≥ 320 (the rest, squeezed) | 532.6 + 1.1471 (tx - 320) |
| y | -42.84 + 1.3819 ty |

- So the screen shows tx 32-972 and ty 31-512, down to y 665 (the menu's footer is below).
- The game's own pictures are drawn pre-distorted for this: re-rendering EB00 through the mapping matches a screenshot exactly.
- The converters scale the player's picture to cover the visible 1280 x 665 area, centred (no squashing; what falls outside is cut). Each texel takes the picture's pixel where it lands (bilinear), so it looks right on screen; texels off screen repeat the edge.
- A 16:10 wallpaper loses about 8% at the top and the bottom; a 16:9 one loses about 4% at each.

## The port's page

- **Setting:** `menu_backgrounds_off` in svr2011.toml, the numbers 1-12 that are off, comma-separated (like `jukebox_off`).
- **With every picture off, all 12 play,** so the main menu never goes blank.
- **Hooks:**
  - `sub_8258A990`: the step goes to the next picture that is on.
  - `sub_82458F58`: after the constructor, a background whose number is off takes the next one that is on, written to +68 and to game data +8680. This covers the first picture after start and a set changed on the page.
- **Page:** the JUKEBOX page's look. It lists the 12 with on / off switches, ALL ON / ALL OFF, and shows which one is on screen now.
  - There's no separate preview: the page is translucent and the main menu's own picture is behind it.
  - Input: controller, keyboard or touch (tap a row or a footer button).
  - It holds the game's input while open (graphics_page.cpp's shared hold).
- **Menu row:** OPTIONS row 8, after LANGUAGE. Label id 0x0FA0B200 (BACKGROUNDS / ARRIÈRE-PLANS / HINTERGRÜNDE / FONDOS / SFONDI). It is outside the game's string ids: 0xAFC8, the next one after the port's 0xAFC0-0xAFC7, is the game's own (the help bar's A button text), so the label hook can't use it.
  - `tools/patch_menu.py` and `game_files.cpp` add the record: (0xAFC4, 0x11, 0x0FA0B200), dropping hidden record (0xA482, 0x0E). Table state E5/E2 → E5/E3.
  - Both produce identical bytes. Checked with a script: the disc table run through patch_menu.py equals the installed E5/E2 table plus the new step.
