# Hand poses (fingers) - research (D9)

How SvR 2011 poses the fingers during a motion. Research only: nothing in the
port uses this yet. Written for the Mod Maker viewer and for importing motions
from other games.

## Where the poses come from

Motions don't animate the finger bones themselves. A motion's YMKs event block
says "switch the left/right hand to pose N", and the game blends the fingers to
that pose from a shared library.

### Event op 21: hand pose

In a YMKs record's event block (`ymks.split_record` / `ymks.parse_events`):

```
15 ss ii ii nn nn
```

| Field | Meaning |
|---|---|
| `ss` | hand: 1 = left, 2 = right |
| `ii ii` (u16 LE) | pose id: 0-105 left, 200-305 right (= left id + 200) |
| `nn nn` (u16 LE) | probably the blend length in frames (mostly 5) |

Op 16 (`10 ss iiii`) is the face: ids 1000-1802 from the same library.

The game function that handles op 21 is not traced yet.

### The library

`m.pac` -> `MVMT/STAT`, PACH child 99. The same bank is copied at
`MOT/BM03/99`, `MOT/RRBS/99` and `CAF/MOT/99`.

- It is a YMKs bank of 161 entries: hand poses 0-105 / 200-305, and faces 1000+.
- Entries 60 and 260 carry ids 50 and 250.
- Each record of a hand entry holds 15 sub-records of 5 bytes:

```
[k] [count] [s8 x] [s8 y] [s8 z]
```

- angle = (s8 << 8) * 2*pi / 65536, so 1.40625 degrees per step.
- `k` indexes the game's finger table at 0x82DA1090 (getter `sub_823B0668`):

| k | bone |
|---|---|
| 0-14 | `l_yubiFJ`: F = finger 0 (thumb) .. 4 (pinky), J = joint 0-2 (`l_yubi00`, `l_yubi01`, `l_yubi02`, `l_yubi10` ... `l_yubi42`) |
| 15-29 | `r_yubiFJ`, same order |

- The right-hand poses are stored pre-mirrored: use them as they are, don't
  mirror the left ones.

## Applying a pose (viewer recipe)

- Set each finger bone's local rotation on top of the bind pose.
- Euler order Rz * Ry * Rx.
- When a motion has no op 21 events (for example YMBs previews), use the
  defaults 10 (left) / 210 (right): the ring idle hand.

Confidence: medium for the `nn` field and for the rotation order (checked by
eye, not against the game's code).

## Pose shapes

Left ids; the right hand is id + 200.

| Shape | Ids |
|---|---|
| open | 0, 11, 53, 67 |
| relaxed | 2, 4, 6, 29, 100-102, 104 |
| ring idle | 10 |
| fist | 3, 15, 24, 26, 103 |
| point | 12, 13 |
| V | 50, 61, 63 |
| horns | 64, 65 |

## Example

Taunt `MVMT/BASE/7`, motion id 701. Its hand events:

| Frame | Pose |
|---|---|
| 1 | L 6, R 206 |
| 6 | R 226 |
| 22 | L 26 |
| 39 | R 305 |
| 60 | L 105 |
| 73 | R 300 |
| 85 | L 10 |
| 95 | R 210 |

So the hands go relaxed -> fist -> ... and back to the ring idle hand at the end.

## Not hands

YMKs tracks y10-21 and y30-41 are probably the ring ropes (task `'RpAt'`,
`sub_821FBBA0`), not fingers.

## Decoding by hand

With `port/tools/ymks.py`:

1. Read the library bank with `ymks.read_bank`.
2. For each record from `ymks.parse_stream(entry['data']).records()`, call
   `ymks.split_record(rec)` to get `(events, pose)`.
3. Read `pose` as 5-byte sub-records as above.
4. For a motion, walk its records' events with `ymks.parse_events(events, bank.T)`
   and pick op 21 / op 16.
