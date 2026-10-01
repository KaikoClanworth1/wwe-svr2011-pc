# Paint Tool: more than 20 logos (pages) - research and plan

Status (2026-10-01): phase 1 is implemented in `port/src/paint_pages.{h,cpp}` and tested in game (see Implemented). The launcher's Paint Tool tab and the Community Creations slot list have pages too.

**Decided (user, 2026-10-01):** 10 pages = 200 logos, a fixed limit (not a setting). LB/RB switch pages in every paged menu.

## What the game does today

- **One save file.** `00PaintTool.pt` is 7,888,892 bytes (0x785FFC) and always holds exactly 20 slots.
  - Each slot is 0x604CC bytes: a header, a 256x256 ARGB canvas, an 8-bit TGA, a DXT5 DDS, a timestamp and a checksum.
  - The file is validated on load (magic, version 3, per-slot sums, trailer sum). One wrong sum and the game calls it damaged.
- **Paint Tool mode** (Create A Superstar -> PAINT TOOL):
  - The manager (PTM, global `*0x82EDE67C` +4) owns one 0x7E6518-byte buffer: the file image, a scratch slot and `dirty[20]`. It is allocated at 827948FC.
  - The grid is a fixed **4 rows x 5 columns = 20 layout cells**. The layout tables at 0x82062230/0x82062280 have 20 entries each.
  - Cursor to slot is `row*5+col` (827B3F50). The slot to cursor mapping is 827B3B80.
  - Thumbnails are 20 full textures built from each slot's ARGB canvas (82794300 builds all, 827941E8 builds one).
  - Count 20 is hard-coded in about 15 loops: 827B2C20, 827B35C8, 827B3630, 82793E30, 82793AF0, 82793B48, 82794300, 82794448, 82794668, 82794D30 (x2), 8278EE20, 827B3838, 827B3908, plus the menu object's 20-entry arrays.
  - The slot getters have no bounds checks.
- **Nothing persists a slot index.** Other modes use logos in these ways:

  | User | How it uses a logo | Own copy of the .pt? |
  |---|---|---|
  | Created Superstar logo list (828DC1A8 / 828DBD90) | u32 **content checksum ID** + pixel copy inside the .cas (the checksum is at slot+0x604C8). In game (TATTOOS -> PAINT TOOL DATA) the picker is a vertical carousel of the used logos only, with a counter ("1/6"); its list code is 827E7100. A 5x5-per-page picker with `idx<20` checks also exists (827B8FF0 / 827B96B8 / 827B9AF0, in the Paint Tool code range); where it appears is not known yet. | Yes (828DC228) |
  | 20-logo picker popup (824AE8C8 / 824AF5E0) | Pixel copy (renders a 128x128 DXT1) | Yes (824AF638) |
  | Save-data / Community Creations slot screen (824DCA98) | Runtime index `page*10+cursor`, 2 pages of 10 | Shared manager buffer (824C5428) |
  | Online upload/download (824C5768 / 824C5BA0 / 82509800) | Raw slot copy | Shared buffer |

  So slot indices never reach a save file. The main problems are fixed-size buffers, 20-entry arrays and the 20-cell layout.

## Two ways to do it

1. **Grow the file to N slots.** Rejected.
   - It means patching about 45 hard-coded sites across 5 modes.
   - It enlarges 3 separate 7.9 MB guest allocations and their 20-entry arrays.
   - 200 slots would be a 77 MB file loaded up to three times in guest RAM. The game already uses most of the 360's 512 MB.
   - It also breaks compatibility with the original file format and online.
2. **Pages (banks) of 20. Recommended.**
   - The game only ever sees a normal 20-slot .pt. The port decides which page of 20 that is.
   - Page 1 is the real `00PaintTool.pt`, unchanged, so existing saves, the launcher and online keep working.
   - Pages 2..N live beside it, in the same slot format.

## Plan (pages)

### Storage

- Keep page 1 in `Saves\00PaintTool.pt`.
- Store pages 2..N in `Saves\.paint\` as one file per used slot (`pNN_sMM.bin`, 0x604CC raw slot bytes).
  - Empty slots take no disk space. A full 7.9 MB file per page would be wasteful.
  - The port builds a valid 20-slot image, with correct sums, when a page is shown.
- Keep a host-side index of `content ID -> (page, slot)` across all pages, for the Created Superstar lookups below.
- **Fixed limit: 10 pages (200 logos).** It is not a setting. The worst case on disk is about 69 MB for pages 2-10.

### Hooks: file I/O

- **Read.** Hook the storage read (824AE830 with size 0x785FFC). When the current page is not 1, overwrite the loaded buffer with that page's image.
- **Write.** Hook 82517CF8 with size 0x785FFC. When the page is not 1:
  - write the page's used slots to `.paint\`;
  - hand the game a successful write without touching `00PaintTool.pt`.
  - Checked (see In-game checks): finish the job through the game's own "done, no error" path.
- **Open and close too.** The save opens the file with create-always before writing (824AE6E8, which recreates it). So for pages 2-10 the open (824AE6E8), write (82517CF8) and close (824AE790) of the Paint Tool's storage object (PTM+40) must all be skipped together. The read (824AE830) is simplest skipped the same way, filling the buffer from the page store.
- **Space check.** The required-space table (824DB258) stays at 7.9 MB.

### Hooks: Paint Tool grid

- A page switch is added in the menu update (827B4600) on **LB/RB**. The pages wrap: LB on page 1 goes to page 10. It:
  - is blocked while a prompt or save is in progress;
  - swaps the buffer contents to the new page;
  - clears `dirty[]`;
  - rebuilds all thumbnails through 82794300.
- **Page label:** the grid screen has no title bar; its only text is the A/B/X/Y prompt row at the bottom. Options, cheapest first:
  - an "LB PAGE 3/10 RB" label drawn by the port's own overlay, placed above the prompt row;
  - a fifth prompt in the game's prompt row (needs the prompt-bar code; not researched yet).
- Cell indices stay 0-19, so no layout change is needed.
- **Copy across pages** (the game's copy is within one buffer): keep a host "clipboard" slot. Copy stores the source slot, and paste after a page switch writes it in. This is phase 2. Within-page copy works as-is.
- **Delete and save:** these work per page as-is, because they act on the buffer.

### Hooks: Created Superstar logo picker (needed, or pages 2+ are only usable in the Paint Tool)

- Its list (828DBD90) loads the .pt with the same hooked read, so it shows the current page's logos.
- Add LB/RB page switching in the carousel. It reloads the list (828DBD90) from the next page, and the counter becomes e.g. "PAGE 2 - 1/6".
  - Checked in game: the editor's LB/RB (HEAD/BODY/CLOTHING/OTHER tabs) are disabled while the picker is open, and RB does nothing there. LB/RB are free.
  - An empty page shows the game's own "no logos" state. Pages with no logos could be skipped.
- **Logos from other pages are safe.** Checked in game: a Superstar keeps every logo that is not on the loaded page (see In-game checks). The all-pages ID index is therefore not needed for loading or saving Superstars. It stays useful only for preselecting the current logo's page when the picker opens.
- The 10-logo limit per Superstar in `caw_logos.cpp` is a separate limit and is unaffected.

### Other users (phase 2)

- The 20-logo popup (824AE8C8, owner not yet identified) and the Community Creations slot screen follow the current page automatically through the read hook.
- Paging inside them can come later.
- **Online** (peer session's area): uploads use one raw slot copy, so any page works. Downloads save into a slot of the current page.

### Launcher

- Add a page selector to the Paint Tool tab (export/import per page).
- Update the `--paint-import/--paint-export` flags to take `<page> <slot>`.

## In-game checks (2026-10-01)

Both risks are cleared. They were checked on the clean v1.0.3 build in a separate test folder (`runs\lim_game`, saves `runs\test_userdata_lim`, harness `tools\lim_session.ps1` / `lim_nav.ps1`, backups in `runs\lim\`).

1. **Skipping the real save is safe (code check).**
   - The storage object (PTM+40, vtable 0x8205F580) runs jobs in its own state machine: +60 = job, 54 write, 55 read.
   - The caller polls +32 = done and +44 = error (vtable +8 / +20).
   - When no storage device is present, the game itself completes a job instantly with `sub_82349DA0`: done=1, then +36/+40/+52 = 0.
   - So pages 2-10 can skip the real open/write/close and finish the job the same way, and `00PaintTool.pt` is never opened.
   - This still needs a runtime confirmation when implemented.
2. **A Superstar whose logos are not in the loaded .pt keeps them (runtime check).**
   - Setup: Paint Tool slots 1-4 were emptied with `tools/pt_tool.py clear`. Superstar 2 (`01CreateSuperStar.cas`) uses exactly those 4 logos.
   - Results:
     - The game boots with no warning.
     - The editor shows all 4 logos.
     - Saving runs the game's "Checking Content" step, which passes. The saved .cas still holds the same logo IDs, flags and the port's extra-logo record.
     - After the save, the character-select screen shows the Superstar with the logos.
     - The tattoo picker lists only the 6 remaining logos ("1/6") without touching the logos already applied.
3. **LB/RB are free** in the Paint Tool grid and in the Superstar logo picker: pressing them changes nothing.

## Risks and unknowns

- Thumbnail memory: the grid rebuilds 20 textures of up to 256 KB each on every page switch. That is the same as entering the mode, and should be fine.
- The owner of the 20-logo popup (824AE8C8) and of the 5x5 picker (827B8FF0) still needs a runtime look (trace + screenshots) before their hooks are written. Both are phase 2 unless one turns out to be on the main path.
- How the page label is drawn on the grid (see Page label).

## Test plan

- `tools/paint_pages_test.ps1` with test saves. It should:
  - create a logo on page 2;
  - restart and check it is still there;
  - check page 1 is byte-identical to before;
  - apply the page-2 logo to a Superstar, then save, reload and play a match;
  - check a Superstar that uses a page-2 logo while page 1 is showing.
- Run it on both renderers.

## Implemented (phase 1, `port/src/paint_pages.cpp`)

- **Storage jobs.** On pages 2-10, the hooks on 824AE6E8 (open), 824AE830 (read), 82517CF8 (write) and 824AE790 (close) handle the job when `r4 == 1` (the Paint Tool type; only Paint Tool code calls these four). The read fills the buffer with the page, the write stores the page's used slots, and the job is reported done with st+32 = 1 and st+36 = 1.
  - Success is +36 = 1. The game's no-device path sets 0, which the Paint Tool reads as a failure.
- **Grid.** The grid menu update (827B4600) acts on LB/RB in its idle state (menu+776 == 0) when the manager is idle (PTM+28 == 0). Pages wrap.
  - If any slot is dirty, the current page is saved first, because the game only saves copies and deletes when you leave the Paint Tool:
    1. 827B35C8(file, 1) recomputes the checksums;
    2. the page is written (page 1 straight to `00PaintTool.pt` through a temp file, like the launcher does);
    3. 827B3028 clears the dirty flags.
  - The page image then goes into the buffer, and the game's own refresh does the rest: PTM state 34 rebuilds the thumbnails, and grid menu state 8 waits for them and redraws the cells.
- **Superstar picker** (TATTOOS -> PAINT TOOL DATA). The list is filled when the editor loads, with whatever page is current.
  - 827E7100 (picker open) records the picker. LB/RB in the picker's cursor input (823D4FC0 on picker+28) moves to the next page that has logos (empty pages are skipped).
  - The game's own list loader is restarted (828DC1A8(list, 42)) and its steps (828DBD90) are run from that input hook until done (+16 == 7). Then the picker is opened again with 827E7100, which takes the new list.
  - Cursor input is held while the list reloads.
- **Label.** "LB  PAGE n / 10  RB" is an ImGui overlay. It sits on top of the grid (centred), or right of the picker's "PAINT TOOL DATA" title. It shows only while those screens take input.
- **Verified in game** (test saves; the grid with `tools/paint_pages_test.ps1`, the picker with `tools/paint_pages_caw.ps1`):
  - paging both ways in the grid;
  - copy on page 2 saved on B, and on LB/RB;
  - copy on page 1 saved by the port; the file is valid and loads after a restart;
  - an edit in the editor saved on page 2;
  - `00PaintTool.pt` never touched on pages 2-10;
  - the picker paging through pages 1 <-> 2, skipping empty ones;
  - a page-2 logo applied to a Superstar.

## Community Creations download (research, 2026-10-01)

- **CC screen.** Its state machine is 824DF010, with the state at this+1176. The destination slot is chosen **before** the download.
- **The slot list is a data-select screen (DS)**, vtable 0x82027C20 (list widget at obj+28), with mode at obj+260:
  - mode 1, pick the destination: it reads the .pt with the storage jobs above, at obj+272;
  - mode 2, write: it copies the downloaded 0x604CC record into slot obj+0x8344, sets used, writes the checksums (828D11E8/828D15B8) and saves (82518268 -> 82517CF8).
  - The overwrite prompt may never show for Paint Tool logos (vt+132 returns 1); this still needs checking in game.
- **The slot list has 2 pages x 10 rows.** D-pad LEFT/RIGHT (not LB/RB) change its page. The index is obj+0x8344 = page*10 + cursor (8250FFC0, every frame).
  - Count 20 at 82517B44; rows 10 at 8250FAF8; pages = count/10 at 8250FB38.
  - Labels "PAINT TOOL LOGO SLOT %02d" at 82518668; the label table (20 x 70 at obj+0x9010) cannot grow.
- **Header metadata.**
  - Header +4 = server content id; +13 = downloaded (blocks re-upload: 828D1648 -> 0xC707); +15 = created by you.
  - A 50-entry recent-downloads list in the profile is keyed by content id, never by slot.
  - Downloads never touch CAW data.
- **Already works through the storage hooks:** the slot list reads, and downloads write, the **current** page.
- **Plan (option B):**
  1. Hook 8250FFC0 for the DS (vtable 0x82027C20, mode 0/1).
  2. On LB/RB, change the page, re-read it into obj+272, then call 82517B58 + 82517B90 + 82518668 and 82510518(obj).
  3. Number the labels page*20 + i + 1.
  4. Keep the page fixed from the mode-1 pick to the mode-2 write. The game frees and reallocates the .pt buffer between them (824C5428), so reload the page at 82518248/82518268 before the copy.
  - The page label goes beside the list.

## Launcher (done)

- The Paint Tool tab works on one page at a time. "◀ Page / Page ▶" buttons sit under Refresh, with a "Page n of 10" label.
- Page 1 reads and writes `00PaintTool.pt` as before. Pages 2-10 are built from, and written to, `Saves\.paint\pNN_sMM.bin`, the same format as the game side; empty slots' files are removed.
- **Export all** exports every page: "Logo 3.png" on page 1, "Page 2 logo 3.png" on the others. The usual saves backup also copies `.paint`.
- **Command line:**
  - `--paint-export/--paint-import <file.pt> <slot 1-200>`: slots 21-40 are page 2, and so on.
  - `--capture paint <bmp> --paint-page N`: captures that page.
- **Verified:**
  - command-line export/import on pages 1, 2 and 3;
  - captures of pages 1-3;
  - a launcher-imported page-3 logo shown in the game's grid.

## Community Creations paging (done)

- **Hook on 8250FFC0** (the slot list's update while it has the controller; data-select screen, vtable 0x82027C20, modes 0 upload and 1 download target). On LB/RB it changes the page, writes that page into screen+272, then runs the game's own refreshes:
  - 82517B58: used flags;
  - 82517B90: downloaded flags;
  - 82518668: labels;
  - 82517D78: the upload preview, mode 0 only (downloads preview the download);
  - 82510518: redraw.
- **The label** sits left of the list's own "◀ 1/2 ▶" row.
- **Hook on 82518268** (writes the download into its slot) loads the current page into screen+272 first, because the game freed and reallocated that buffer since the slot was picked. Then it calls the Online session's `SetPaintDownloadCheck` callback (backup and validation; returning false skips the write), and then the game's write. The write is then stored by the page storage hooks.
- **Verified** against the local server (`tools/paint_pages_cc.ps1`; config `runs/test_config_lim_online.toml` with its own online_xuid; saves `runs/test_userdata_lim_online`):
  - downloaded a logo into page 2 slot 4: `p02_s04.bin`, used and downloaded flags set, `00PaintTool.pt` untouched;
  - the upload list on page 2 greys out the downloaded logo;
  - uploaded page 2 slot 1 (server record 6);
  - paging 1 -> 2 -> 3 -> 2 with the preview following.

