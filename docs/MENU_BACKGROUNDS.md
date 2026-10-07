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

That's left for later (custom backgrounds as mods).

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
