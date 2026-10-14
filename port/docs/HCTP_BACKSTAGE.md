# HCTP backstage areas (branch hctp-backstage)

SmackDown! Here Comes the Pain (PS2, 2003) backstage areas played in SvR2011
as backstage mods with their own BACKSTAGE row. The first one is the
**Arena Back Entrance** (HCTP stage 0x30, "BackOpenSpace" in HCTP's code):
two levels, the small chain-link cage, crates, valves, a button that rolls a
cart out of the garage.

## Where things are

- The disc: `D:\Roms\Playstation 2\WWE SmackDown! Here Comes the Pain (Europe).iso`,
  extracted to `D:\Xbox Games Ports\HCTP Extract` (`PAC\*.PAC`, unpacked
  `_unpacked\bg\stg_XXXX`, `_unpacked\gm`, `_unpacked\m`).
- Viewer pages (three.js, one per room): `HCTP Extract\viewer\index.html`.
- Converter scripts and research notes are in the session scratchpad
  (`hctp\`), to be moved into `port/tools` once settled:
  - `hpac.py` (archives), `yobj.py` (models, textures), `to_glb.py` /
    `room_view.py` (viewer);
  - `hctp2jboy.py` (models -> JBOY), `hctp_fake56.py` (room -> stand-in
    stage for `svr08_stage.py`), `hctp_props.py` / `build_props.py` /
    `hctp_gimmick.py` (props, climb spots, motions -> GMGB package),
    `build_room30.sh` (all of it);
  - `mot/hctp2ymks.py` (HCTP motions -> 2011 YMKs);
  - `re/re_gimmicks.md` (HCTP's gimmick code), `re/re_svr2011.md` (2011's
    collision attributes, gimmick record types, weapons),
    `mot/re_motion.md` (motion formats).

## Formats (HCTP vs 2011)

Same family. DPAC/PAC archives, BPE, YOBJ (= JBOY little-endian, POF0), HMD
collision (identical 96-byte triangle records with an attribute word), model
flags (50001), camera zones (50005). Units and axes are the same (1 = 10 cm,
y down). HCTP models store geometry as PS2 VIF packets; prop vertices are in
model space with per-batch nodes (batch node number = node index + 1).

Motions: the same bank container and frame stream; the body record has the
same 21 bones in the same order and the same rotation convention, so HCTP
motions convert without retargeting (only re-encoded).

## How the room is built

1. Room models -> parking-room slot of a bg78 (rotated 180, offset 180,-440),
   HCTP collision (room + props at rest) as the parking HMD pair. The stage
   entry STG/0078 must stay last in bg78's STG group (a row match indexes the
   group by position). Empty parking ids get stub models.
2. Gimmick package GMGB/78H3 (gimmick.pac): 2011's corridor parts, HCTP props
   as T23 objects (with their node rigs), climb spots (T22 walk / T21 run on
   the 0xDF faces), and a bank = 78PK's + converted HCTP motions (keys
   12501-12544).
3. Manifest: `row=`, `gimmick=78H3`, `box=`, `camera_height=`, and new keys:
   - `start=<x>,<z>,<facing deg>[,<y>];...` start places (match_types.cpp);
   - `weapons=<model>,<x>,<y>,<z>,<yaw>;...` weapons at the bell (45010 swap);
   - `hctp=30` turns on `src/hctp_room.cpp` for that row.

## What 2011 can't do from data (src/hctp_room.cpp)

| HCTP | status |
|---|---|
| Climb crates / walkway / ladder / cage (wall attributes) | 2011 backstage fighters walk in motion 20500, which never reaches the wall switch (sub_82306B10); hctp_room asks the T22 search itself when a fighter presses into a 0xDF wall |
| Walk off a ledge | works (gravity + floor triangles) |
| Cage walls: grab (X) + mash -> both panels of a side break, vanish, stop blocking | works (player and AI); the panels' collision carries the panel tag (0xA0 + prop), so climbing accepts those tags too |
| Hits / grabs through the standing fence (HCTP cells) | hooks on the hit broker's vt[8] sub_82238F28 and vt[10] sub_8222F678: inside-cage vs outside-cage pairs on the ground are dropped while that side's panels stand; works (test: fighters at x 155 / 166 either side of the east panels, 48 CPU hits across, all dropped, no damage) |
| Roof: 2nd knock-down breaks it, fall into the cage | works (DropIn nudges whoever stands over the hole; gravity does the rest) |
| Button -> shutter opens, cart rolls out, crates fall | works: X in the button zone (12510), shutter 12525 then hidden, cart moved along z (MoveRec + pose copy off, obj+1140), hits whoever is in its way (340 = 12516), crates 12529, fade, 240 HCTP frames later the closing shutter (HCTP 1265, 12530) and everything resets |
| Valves -> steam jets that hurt | works: X in the valve zone toggles (12511 / 12512, 121-frame gap, off after 250); 5 zyouki puffs per jet moved along it; burn 12509 in the jet volume (HMD 2058 / 2068); valve 210 also drops the barrel stack |
| Equipment crate: whip a wrestler in, he's shut in and climbs out | works with the test switch (12513 attacker/victim/crate on the crate's spot, held on the last pose, 12514 at HCTP 420); trigger = whipped run (motion 59) into the crate's floor zone, untested with a real whip |
| Stacks (cardboard, barrels, pipes) collapse and drop a weapon | cardboard + barrels work (2011 has no cardboard box / barrel / lead pipe: trash can 63 / tire iron 120) |
| Railings fall | coded, untested |
| Jump down off the cage top (HCTP GapIn, 12572 -> 12505) | works: X on the cage top facing its front or east edge puts him on the edge and he jumps to the floor |
| Step into the broken roof hole (12092 -> 12506) | works: X while walking toward the hole from the frame (within 6 of it) steps in; he lands in the cage ~1.5 s later |
| Weapons the props drop | cardboard box 17 (stack), trash can 63 (barrels), kendo stick 88 for HCTP's lead pipe (2011 has none); their models load through load_weapons= (hook on sub_82301148, the per-area wanted-model list) |
| Equipment crate rest pose | 12559 = the crate's own track of 12513, 1 frame (was a tilted green slab) |

Test switches (SVR2011_TEST_HCTP_ROOF): `1` roof breaks at the first knock-down, `now` roof breaks at frame 300, `crate` fighter 1 shut in at frame 300, `steam` both valves opened at frame 300, `cart` the button cycle at frame 300, `props` railings, pipes and stacks knocked over at frame 300, `puff` a steam puff over fighter 0, `nograb` no cage-wall grabs or climbs (fence tests).

Camera: in an HCTP row (hctp=), fighters at different heights (one on the cage top) are framed head to feet (match_types.cpp, the sub_82227690 sphere: centre between the lowest feet and the highest head). One on top and one far off below can still leave the lower one at the frame's edge (the camera= distance cap).

Known: walking off the frame into the open hole drops a fighter in by gravity without the step-in act (HCTP's GapIn needs X).

Pitfalls:
- Moving a prop (MoveRec): write elem +32/+48 and call sub_824B9C40(elem); then mark the objects' motion component ('S3MW' via sub_8269DFB0(obj+1088), its +24 -> +12 = 0), or sub_824B46F8 copies the last motion's matrices back every frame. Not obj+1140: that skips the node commit too and the object isn't drawn. PlayRec (sub_824B91C0) sets S3MW running again.
- An object hidden (sub_824B8FE0 + 4198/4150) and shown again isn't drawn while no key plays on it: the steam puffs park under the floor (y 1000) instead.
- Models that never play a key must be converted without their node rig (build_props STILL, the puff 1237): with keep_nodes 2011 draws HCTP's raw node transforms (the cart was a grey plank at head height, the puff invisible).
- The closing shutter (1265) is hidden for the room's first 30 frames (SetUp): its elements may not exist on the first one.
- Placing a fighter for a gimmick act: yaw (+276) before the act, then play it, then write +288..+296, body (+304) +0/+8 (as sub_824B67D0, the T22 snap) - the act's root motion is absolute from where he is when it starts, turned by his yaw: local +z = (sin yaw, cos yaw).
- Weapon models: the game loads a hard-coded set per area (parking 65, 63, 17; sub_82301148); weapons= only places weapons. Extra models: load_weapons= in the manifest. (2011 model 120 is a refrigerator, not a tire iron.)
- 2011 props draw with an alpha cut-off (no blending): soft textures (the zyouki steam) get an ordered-dither alpha.

## Tests

- `SVR2011_TEST_HCTP_LOG=1`: wall switch, T22 search, motion key lookups.
- Start places put the fighters where a test needs them (e.g. the opponent
  upstairs, where the CPU takes a while to come down).

## Room data (hctp.txt, written by build_props.py)

`prop <name> <hctp id> <kind> <attr> <x> <y> <z> <ry> recs=... tris=...`,
`weapon <hctp id> <model> <x> <y> <z>`, `key <key> <frames>`. Each prop's
wall triangles carry the low attribute byte 0xA0 + its index, so a fighter's
touched attribute (+455) names the prop he hit; tris are HMD 1004 indices
(after the converter's padding dummies).

## Rooms 0x31 - 0x36

| room | row (build_room.py ROOMS) | offset dx, dz | interactions (src/hctp_room.cpp) |
|---|---|---|---|
| 0x31 | Parking Lot | 140, -437.7 | broken cars (door / bonnet fly off, crushed by a body on the roof), fence / oil drums / equipment box, generator shock (12079), Irish whip into the trailer (12078 sync), background traffic car, drivable forklift (X on / off, A drive, R1 brake, Y reverse, stick steer, B tips the crate; 321 reloads it) |
| 0x32 | Training Room | 103.5, -388 | lockers (shut in 120 frames), toilet stall, showers, shelves (items fall, become weapons), TV, breakable wall (cracks, then opens and knocks the TV over), sandbag (two pendulums, knocks down), treadmills (X on, A speeds up) |
| 0x33 | Times Square | 86.5, -408 | six taxis (brake, run over, explode when someone goes down on the roof), lamp-post poles (X climbs, X jumps off), shop windows, ladder (0xE0) / wall (0xE6) climbs, helicopter (its act-10 path; X grabs the ladder) |
| 0x34 | Outside the Arena | 205, -334, crop -60,345,-534,-322 | the extinguisher is 2011's own weapon (65); HCTP's other room-34 objects are dead data |
| 0x35 / 0x36 | Fountain Square / (dry) | 235, -617.5 / -607 | lion statues (run straight at one: climb 12005-7; X or 601 frames dives off), fountain throw (Irish whip near a basin, 12027 sync; room 35); HCTP's flood and tower fall are dead code |

What the converter does per room (beyond room 30's steps):
- Models from the room's own 50001 flag list; models with the same flags merged (the parking slot has 15
  free ids), event-only ('e') and light-motion additive layers left out, 'r' / 'y' (draw again mirrored /
  turned) baked in, more than 64 meshes split. A stage over ~13k vertices fails 2011's GPU buffer allocation
  at load (room 34: cropped).
- Big collision indexes: cell counts padded off 0xFF, a zero tail so the entry decompresses in place.
- Props of 22 more HCTP classes (hctp_props.KINDS), their tag params (p=), record models (m=) and HMD
  shapes (h<id>=) in hctp.txt; pole tops and the helicopter's path as `pole` / `heli` lines.
- Motion bank per room: bank_h3 + HCTP backstage acts 12000-12099 as 12600 + n, 12500-12599 as 12800 + n,
  the room's object acts (50102) n as 12700 + n; jumps to 2011's downs 443 / 448 / 476 kept.
- Weapons: HCTP's spots (50000 'w', GmWeaponPosRot) as 2011 weapons in `weapons=`, or `weapons=none`
  (else 2011's parking weapons lie in the room); what props drop in `load_weapons=`.

2011's input object (svr2011::FighterInput): +48 buttons held, +52 pressed this frame (X 0x80, A 0x40,
B 0x20, Y 0x10, R1 0x08), +56 the stick's 8-way direction (4 up the screen = -z, 6 right = -x, 0 down; -1 none).

Test switch `SVR2011_TEST_HCTP_ROOF=props` in rooms 31-36 drives the interactions on fighter 1 (cars,
generator, slam; racks, wall, locker, toilet, shower; lion; window, pole) and puts fighter 0 (the scripted
pad) by the forklift (31) / under the helicopter (33).
