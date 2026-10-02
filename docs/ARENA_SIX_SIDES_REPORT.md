# Six-sided (hexagonal) ring: go / no-go (static analysis)

I only read the code for this. Nothing was built, run or changed. Sources: the generated code in
`SvR2011 Arenas\port\generated\default`, `runs\xref.json` and `image.bin`.
Helper scripts are in the scratchpad: build_asm.py, scan.py, sq2.py, xz.py, tally.py.
"Game code" below means 0x82150000..0x82900000, which holds **34,808 functions**. Above that address
the code is the Havok, XDK and CRT libraries.

**Verdict:**
- **Full gameplay hexagon: NO-GO.** The 4-side assumption is not kept in a few places. It is built into
  a "side frame" system that about **400 game functions** use, including move, grapple, AI and camera
  code. On top of that, the animation data is made for 90-degree corners.
- **Looks-only hexagon: technically possible, but it plays badly.** It is only acceptable as a
  novelty or screenshot option.

---

## 1. Shared helpers (CONFIRMED by disassembly)

The side and corner numbering is the same in every helper:

- **Sides:**
  - 0 = -Z side (z<=0, |z|>|x|)
  - 1 = -X side
  - 2 = +Z side
  - 3 = +X side
- **Corners:**
  - 0 = (-,-)
  - 1 = (-,+)
  - 2 = (+,+)
  - 3 = (+,-)
  - These match the post table at 0x82D9D780.

| Helper | What it computes | Callers (functions) / call sites |
|---|---|---|
| **sub_8218E400** | Nearest side from position (r3 -> x at +0, z at +8). Compares \|z\| with \|x\| (abs done by hand with compare + fneg), then the sign picks the side. Returns 0..3. | **59 / 75** |
| sub_8218E470 | Same, with the tie going the other way | 1 |
| **sub_8218CC00** | Nearest corner (quadrant from the signs of x and z). Returns 0..3. | **31 / 47** |
| **sub_8219E108** | Rotates a (x, yaw, z) record by n quarter turns, n in -3..3: swaps or negates x/z and adds n*pi/2 to the yaw, then wraps it. This is how side-local positions become world positions. | **73** |
| sub_8219BB18 | Same quarter-turn rotation on a different record layout (+16/+24, yaw +4) | 26 |
| (8219E108 + 8219BB18) | | **91 functions / 303 call sites** |
| sub_8220D708 | Yaw (+276) to quarter index: ((yaw+pi)*k + 512) >> 10 & 3 | 3 |
| sub_8220CDF8 | Corner index to corner point (+/-31.3, 0, +/-31.3) in scratch 0x82D9E360 | 2 |
| sub_8220CE48 / sub_8220CF18 | Corner index plus position to angle toward that corner (atan2 at 0x828F41C0) | 1 / 3 |
| **sub_8220D1D0** | Distance from a position to corner n (+/-31.3) | **28 / 36** |
| **sub_821BB530** | Inside the ring: \|x\|<28.8 && \|z\|<28.8 (0x82E354E0) | 14 |
| sub_821BB568 | Inside test with the threshold 64*0.5+c | 1 |
| sub_821BC868 | Inside the ring, or in states 173-176 | 2 |
| **sub_821B89B0** | Outside the ring (square test on 28.8, with state exclusions) | 12 |
| sub_821B8C28 / 8DB0 / 8F38 / 90A8 / 9208 | Square zone tests at 42, 40, 55.3 (floor/barricade), 55.3, 30 | 7 / 6 / 15 / 28 / 18 |
| **sub_8218EB88** | Ring-area classifier: \|x\| and \|z\| against 28.8, 30, 32 and 55.3 (ring / apron / floor) | **42** |
| sub_821989C0 to sub_82251A98 | Point on rope n: uses ropeY 0x82E354E4 minus n*4.2, and the 28.8 edge, rotated by side | 4 |
| sub_822EFE50 | Side of another object (\|x\| vs \|z\| at +96/+104) | 3 |

**Per-wrestler "side frame" byte at wrestler+440 (CONFIRMED)**

- It is written from `bl 8218E400` (for example in 82214790, 82233D90, 82285448, 82289048, 8229E998,
  8231F3F8, 8226A360).
- Code reads it and passes `neg r4` to 8219E108 or 8219BB18 to turn world positions into side-local
  positions. Example: 822153F8 and 822D4390 then classify the local yaw against +/-pi/4 and +/-3pi/4.
- Usage counts:
  - **155 functions read it and 49 write it (177 in total).**
  - Of the reads, 51 go straight into a rotation, about 70 compare it with the literals 0..3 and 41
    copy it to another wrestler.
  - There is also 1 "opposite side" computation, (s-2)&3, in 822EA0D0.

## 2. Callers vs inline (CONFIRMED counts; the inline count is heuristic)

| Group | Functions |
|---|---|
| Call at least one 4-side helper (table above) | **228** |
| Read or write the side byte +440 | **177** (85 of them also call helpers) |
| Inline 4-side logic that uses neither the helpers nor +440 | **76** (see note) |
| Union: code that knows the ring is a square | **about 396 functions, about 108,000 PPC instructions** |

How the 76 inline functions were found:
- Ring-shape globals read directly (64.0, 31.3, 28.8, corner and normal tables, the 32 and 38.5
  boxes): 86 functions.
- `|x|` and `|z|` both compared with the same constant: 47 functions.
- x compared directly with z: 21 functions.
- After removing overlaps and helper callers, 76 are left.

Some of these are not ring logic, for example floor or barricade tests at 55.3, the +/-70 arena bounds
and camera code. I estimate about 50 to 60 of them are really about the ring.

Where they are:
- The whole range from 0x8216 to 0x8285.
- The heaviest areas are the character state and move code (0x821B..0x823C) and a large AI block
  (0x827E..0x8285, about 120 functions).
- The largest functions involved are:
  - 822E35E8 (4,835 instructions; 16x `cmpwi ..,3`)
  - 822CBD00 (4,561)
  - 821FD4B0 (2,634)
  - 822D6DF0 (2,282)
  - 822C9F48 (2,042)
  - 82233D90, 82236D98, 82306F90, 823BA890 and 823BD988 (each over 1,500)

Typical inline patterns:
- `cmpwi s,0..3` switches that pick ±28.8 per side (for example 82171248 lines 178-205, the rope
  collision).
- `clrlwi r,r,30` (mod 4), which shows up in 47 candidate functions.
- Square tests against literal-pool constants such as 30.0 at 0x82001F2C, 32.0 at 0x82002874,
  22, 23, 24.5, 28, 29 and 35.

## 3. Data with fixed size 4 (CONFIRMED)

**Ring block 0x82D9D700**

| Address | Contents |
|---|---|
| +04 | 64 |
| +08 | 31.3 edge |
| +0C | -12 |
| +1C | 4.2 |
| 0x82D9D780 | 4 posts (±31.7) |
| 0x82D9D7C0 | 4 turnbuckle yaws (±pi/4, ±3pi/4) |
| 0x82D9D7F0 | 4 corner points (±31.3), filled by 82D35FD8 |
| 0x82D9D830 | 4 inner corner points (±28.8), filled by 82D36010; used by 82199550, 82215AD8, 82235A28, 82277308, 82829190 |
| 0x82D9D870 | **4 side normals** (0,0,1), (1,0,0), (0,0,-1), (-1,0,0); used by the rope mesh code 82199DD8 |

**Derived globals and the angle table**

- Derived globals: 0x82E354E0 = 28.8 (28 users), 0x82E354E4 rope base, 0x82E35510 box ±32,
  0x82E36B24 box ±38.5.
- The 8-direction table at 0x82D9D730..77 has no direct code reference that I found.

**Rope simulation (sub_82168058)**

- Each of the 3 rope objects (rope-physics +32/+36/+40) is a mass-spring chain:
  - **4 sides x 24 nodes = 96 nodes**, 64 bytes each, at +848
  - **100 links** (halfword pairs at +392)
  - **104 rest lengths** (+424)
  - **4 pinned corner anchors** at +6992..+7232, stride 64
- Each side is built by rotating with 8219E108(side).
- **Size of the change for 6 sides:**
  - 144 nodes, about 150 links and 6 anchors per rope.
  - Each rope object grows by about 3.3 KB, and the rope-physics object (16,624 bytes,
    sub_82198C58) grows with it.
  - The loop bounds 24, 96, 100 and 104 are hard-coded in the init code and in the solver,
    including the large solver functions 821A7108 and 821A9130.
  - Practical approach: rewrite this native rope module (about 10 to 15 functions) as a whole.

**Rope collision (Havok)**

- 82170338 and 82171248 loop over **12 rope bodies** (`cmpwi r28,12`, side = i&3), then call
  821989C0.
- Six sides means 18 bodies, plus new Havok shape setup.

**Ring object 0x82E354CC**

- Holds 12 rope-model slots (+8..+52), **4 corner pads (+104..+116)** and the turnbuckle (+120).
- The draw code assumes 4 corners: 8219A508 ropes, 82199550 posts and pads with 4 iterations,
  82199DD8 rope mesh with `cmpwi r25,4`.
- Six sides means 6 pads and 6 posts, plus new assets (a hexagonal ring mesh and 18 rope segments).

## 4. Conclusion

**Full gameplay hexagon (ropes on 6 sides, 6 corners, correct inside/outside tests): NO-GO**

The part that could be contained (estimate):
- About 20 helpers and 15 rope or ring modules could be rewritten natively.
- Sides and corners would be generalised to 6, and the quarter-turn rotation would become a
  60-degree rotation. That one change would carry through all 303 rotation call sites.

The part that is spread out (confirmed counts):
1. **177 functions** store or compare the side byte with 0..3 semantics: mod 4, opposite side =
   s+2, explicit `==3`, and copying it between wrestlers.
2. **About 76 functions** do square tests inline, about 50-60 of them really ring-related. Each one
   needs its own hook or patch.
3. **About 120 AI functions** (0x827E..0x8285) reason in side-local square coordinates.
4. **Data:** corner moves (turnbuckle climbs, tree of woe, corner splashes) and rope animations are
   motion data made for 90-degree corners at 45-degree diagonals. A hexagon has 120-degree corners on
   30/90/150-degree diagonals. Even with all the code patched, these moves would clip or float. This
   cannot be fixed by recompiling.

**Estimate:** about 400 functions to audit and about 100-150 real patch sites, plus new rope physics,
Havok collision and assets. This is a project of several months with a high regression risk across
every match type. It is not a few hooks.

**Looks-only hexagon (gameplay still square)**

What it needs:
- A new ring mesh, apron and turnbuckles.
- New rope rendering. 82199DD8 and 8219A508 build the rope vertices from the 4-side simulated nodes,
  so they would have to draw static hexagon ropes or remap the nodes.
- Posts and pads at 6 positions (82199550).
- About 5-8 render-side functions plus assets.

How it plays (geometry estimate): take a hexagon with the same apothem of 31.3.
- The square's corners (±31.3, ±31.3) end up about **11.5 units (1.15 m) outside** the slanted
  hexagon edges.
- The hexagon's own vertices are at 36.1, so they reach about 5 units (0.5 m) past the square's
  edges.

Result:
- Wrestlers would rebound off invisible ropes in mid-air.
- Corner moves would happen at 4 corners where there is no post.
- Wrestlers would stand "inside" the ring while visually on the apron.
- Rope deformation would not match the drawn ropes.

Verdict: acceptable only as a cosmetic novelty or screenshot mode. It is not a real TNA ring.

---

### Confirmed vs estimated

**Confirmed (read in the disassembly):**
- What each helper computes, and its caller and call-site counts.
- The +440 side byte and its read/write counts.
- The ring tables (4 entries each) and the side-normal table.
- The rope-simulation sizes (4x24, 100, 104, 4 anchors).
- The 12 Havok rope bodies with i&3 indexing.
- The ring object slots and the 4-pad draw loops.

**Heuristic or estimated:**
- The inline count of 76 (it includes some arena and camera false positives; real ring logic is
  about 50-60).
- The union of about 396 functions (exact as a set, but it may include a few unrelated users of the
  +440 byte or the constants).
- The AI-region classification by address range.
- Patch-site counts, effort, and the looks-only gameplay distances (geometry arithmetic).
