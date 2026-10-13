"""SmackDown! vs. Raw 2006 (PS2) entrance finger tracks -> SvR 2011 y2 / y3 finger tracks.

  python svr06_hands.py check <2006 iso or extract> [<2006 number> ...] [--svr07 <SvR 2007 extract>]
        converts the entrant's hands of every (or the given) 2006 entrance offline and checks them
        (re-parse, joint ranges, frame counts); with --svr07, compares the entrances SvR 2007 ships
        with the same frame counts (svr06_entrance.IN_SVR07 ... True) against 2007's own y2 / y3
        (2007's are 2011's format, kept by entrance_port's svr07 path): angle error per joint.

Module API (for svr06_entrance.convert_bank):
  hand_tracks(raw_bank, obj=0, frames=None, fill0=False) -> {x: {2: data, 3: data}}
  convert_track(data, nframes) -> 2011 motion data (or None)
  sample(data, nframes) -> per frame {k: [x, y, z]} (s8 steps; the game's key / count blend)

2006 format (decoded here; docs/HAND_POSES.md has 2011's):
The entrant's y1 / y2 / y3 tracks (face / right hand / left hand, as 2011) hold, after the usual event
block, a pose made of sub-records:
  - 0x77 / 0x78 / 0x79 (face / right hand / left hand): [op][count][u16 LE joint mask][3 bytes s8 x y z
    per set mask bit] - a key of several joints with one blend count; mask bit i = finger joint i
    (0-14, l_yubi00 .. l_yubi42 order), k = i (0x79, left) or 15 + i (0x78, right);
  - anything below 0x77: one 2011 sub-record [k][count][s8 x][s8 y][s8 z] (k 0-14 left, 15-29 right).
The values are 2011's own (same joints, axes, 1.40625-degree steps: SvR 2007 4109 has 2006 4066's hand
values byte for byte, one key per frame instead of key-reduced), the count is the body keys' count (the
key is reached count + 1 frames later). So a 2011 track is the same keys written as 5-byte sub-records;
frame 0 gets the event block every stock y1-y3 track starts with (hand_tracks.FIRST_EVENTS) and every
joint (its first value, held); 2006's own events on these tracks (op 79 at the last frame, SvR 2007's
"loop marker" which entrance_port's svr07 path drops too) are dropped.
"""
import collections
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ymks as Y  # noqa: E402
import hctp_motion as H  # noqa: E402

FIRST_EVENTS = bytes.fromhex('4005000000008900010000')    # (= hand_tracks.FIRST_EVENTS)
GROUP = {0x78: 15, 0x79: 0}                                # right hand / left hand group op -> k base
FACE = 0x77
Y_HAND = {2: range(15, 30), 3: range(0, 15)}               # y2 = right (k 15-29), y3 = left (k 0-14)


def decode_pose(pose):
    """2006 hand pose bytes -> [(k, count, x, y, z)]; ValueError on bytes it cannot read"""
    out = []
    p = 0
    while p < len(pose):
        op = pose[p]
        if op in GROUP:
            if p + 4 > len(pose):
                raise ValueError('short hand group')
            cnt = pose[p + 1]
            mask = pose[p + 2] | pose[p + 3] << 8
            if mask & 0x8000:
                raise ValueError('hand group mask bit 15')
            p += 4
            for i in range(15):
                if mask >> i & 1:
                    if p + 3 > len(pose):
                        raise ValueError('short hand group values')
                    out.append((GROUP[op] + i, cnt) + struct.unpack_from('bbb', pose, p))
                    p += 3
        elif op < FACE:
            if op >= 30 or p + 5 > len(pose):
                raise ValueError('bad hand sub-record k=%d' % op)
            out.append((op, pose[p + 1]) + struct.unpack_from('bbb', pose, p + 2))
            p += 5
        else:
            raise ValueError('not a hand sub-record: 0x%02x' % op)
    return out


def convert_track(data, nframes, y=None):
    """one 2006 hand track (y2 / y3) -> 2011 motion data of nframes frames (None: no hand keys)"""
    keys = []
    for f, r in H.records(data):
        if f >= nframes:
            break
        ev, pose = Y.split_record(r)
        if not pose:
            continue
        subs = decode_pose(pose)
        if y in Y_HAND:
            subs = [s for s in subs if s[0] in Y_HAND[y]]
        if subs:
            keys.append((f, subs))
    if not keys:
        return None
    first = {}
    for f, subs in keys:
        for s in subs:
            first.setdefault(s[0], s)
    recs = []
    for f, subs in keys:
        if f == 0:
            have = {s[0] for s in subs}
            subs = [(k, 0) + first[k][2:] for k in sorted(first) if k not in have] + list(subs)
            subs.sort(key=lambda s: s[0])
        pose = b''.join(struct.pack('BBbbb', *s) for s in subs)
        recs.append((f, Y.join_record(FIRST_EVENTS if f == 0 else b'', pose)))
    if keys[0][0] != 0:
        pose = b''.join(struct.pack('BBbbb', k, 0, *first[k][2:]) for k in sorted(first))
        recs.insert(0, (0, Y.join_record(FIRST_EVENTS, pose)))
    return Y.build_stream(Y.make_stream(recs, max(1, nframes)))


def hand_tracks(raw_bank, obj=0, frames=None, fill0=False):
    """2006 EVNT bank bytes -> {x: {2: data, 3: data}} for object `obj` (0 = the entrant).
    frames: {x: frame count} to write (default: the object's y0 track's frames for that x, else the
    hand track's own); fill0: also a 1-frame x0 (intro) from x5's first frame, for the entrances
    svr06_entrance gives a 1-frame intro (convert_script's fill0)."""
    T6, ents = H.read_bank(raw_bank)
    body = {}
    for e in ents:
        if (e['id'] & 0x3fff) == obj and e['y'] == 0 and e['frames'] and e['data']:
            body.setdefault(e['x'], e['frames'])
    out = collections.defaultdict(dict)
    for e in ents:
        if (e['id'] & 0x3fff) != obj or e['id'] & 0x4000 or e['y'] not in (2, 3) or not e['frames'] or not e['data']:
            continue
        if e['x'] in out and e['y'] in out[e['x']]:
            continue
        n = (frames or {}).get(e['x']) or body.get(e['x']) or e['frames']
        try:
            d = convert_track(e['data'], n, e['y'])
        except ValueError:
            continue
        if d:
            out[e['x']][e['y']] = d
    if fill0 and 5 in out:
        for y, d in out[5].items():
            st = sample(d, 1)[0]
            pose = b''.join(struct.pack('BBbbb', k, 0, *[int(round(v)) for v in st[k]]) for k in sorted(st))
            out[0][y] = Y.build_stream(Y.make_stream([(0, Y.join_record(FIRST_EVENTS, pose))], 1))
    return dict(out)


def sample(data, nframes):
    """2011 hand track -> per frame {k: [x, y, z]} (floats, s8 steps), the game's blend: a key at frame f
    with count c is reached at f + c + 1, from the value held at f (as wwe13_entrance.sample11)."""
    chans = collections.defaultdict(list)
    for f, r in Y.parse_stream(data).records():
        ev, pose = Y.split_record(r)
        for i in range(0, len(pose) - 4, 5):
            k, c, x, y, z = struct.unpack_from('BBbbb', pose, i)
            for a, v in enumerate((x, y, z)):
                chans[(k, a)].append((f, f + c + 1, float(v)))
    idx = {c: 0 for c in chans}
    state, cur, out = {}, {}, []
    for fr in range(nframes):
        for c, lst in chans.items():
            while idx[c] < len(lst) and lst[idx[c]][0] <= fr:
                f, tgt, v = lst[idx[c]]
                if c in state:
                    a0, b0, a1, b1 = state[c]
                    v0 = b0 + (b1 - b0) * (1.0 if a1 <= a0 else min(1.0, (fr - a0) / float(a1 - a0)))
                else:
                    v0 = v
                v = v0 + ((v - v0 + 128) % 256 - 128)
                state[c] = (f, v0, tgt, v)
                idx[c] += 1
            if c in state:
                f0, v0, f1, v1 = state[c]
                t = 1.0 if f1 <= f0 else min(1.0, (fr - f0) / float(f1 - f0))
                cur[c] = v0 + (v1 - v0) * t
        fr_out = {}
        for (k, a), v in cur.items():
            fr_out.setdefault(k, [0.0, 0.0, 0.0])[a] = v
        out.append(fr_out)
    return out


def compare(a, b, nframes, ks):
    """two 2011 hand tracks -> {k: (median, 95th percentile, max) angle error in degrees} (per-axis
    wrap, the largest axis per frame)"""
    sa, sb = sample(a, nframes), sample(b, nframes)
    out = {}
    for k in ks:
        errs = []
        for fa, fb in zip(sa, sb):
            if k in fa and k in fb:
                errs.append(max(abs((p - q + 128) % 256 - 128) for p, q in zip(fa[k], fb[k])) * 1.40625)
        if errs:
            errs.sort()
            out[k] = (errs[len(errs) // 2], errs[min(len(errs) - 1, int(len(errs) * 0.95))], errs[-1])
    return out


# ============================================================================ self-check CLI

def _svr07_banks(root):
    import entrance_port as EP
    import svr08_entrance as E08
    evp, evt = EP.old_entrances(EP.FolderSource(root), EP.SRC07)
    return evt, E08


def main(argv):
    a = list(argv)
    if len(a) < 2 or a[0] != 'check':
        print(__doc__)
        return 2
    s07 = None
    if '--svr07' in a:
        i = a.index('--svr07')
        s07 = a[i + 1]
        del a[i:i + 2]
    import svr06_entrance as E6
    banks, scripts = E6.read_source(a[1])
    nums = [int(v) for v in a[2:]] or sorted(n for n in banks if n not in E6.SKIP and n in scripts)
    tot = collections.Counter()
    for n in nums:
        tr = hand_tracks(banks[n])
        T6, ents = H.read_bank(banks[n])
        src = sum(1 for e in ents if e['id'] == 0 and e['y'] in (2, 3) and e['frames'] and e['data'])
        made = sum(len(v) for v in tr.values())
        bad = 0
        for x, d in tr.items():
            for y, data in d.items():
                st = Y.parse_stream(data)
                for f, r in st.records():
                    ev, pose = Y.split_record(r)
                    if len(pose) % 5 or any(pose[i] not in Y_HAND[y] for i in range(0, len(pose), 5)):
                        bad += 1
        tot['entrances'] += 1
        tot['source tracks'] += src
        tot['made'] += made
        tot['bad records'] += bad
        if made < src or bad:
            print('%d: %d of %d hand tracks made, %d bad records' % (n, made, src, bad))
    print('svr06 hands:', dict(tot))
    if s07:
        evt7, E08 = _svr07_banks(s07)
        allk = collections.defaultdict(list)
        for n6, (n7, same) in sorted(E6.IN_SVR07.items()):
            if not same or n6 not in banks or n7 not in evt7:
                continue
            b7 = Y.read_bank(E08.convert_evt(evt7[n7]))
            tr = hand_tracks(banks[n6])
            for x, d in sorted(tr.items()):
                for y, data in sorted(d.items()):
                    g = b7.find(E08.ID_SHIFT, x, y)
                    if not g or not g['data']:
                        continue
                    nf = min(g['frames'], Y.parse_stream(data).nframes())
                    c = compare(data, g['data'], nf, Y_HAND[y])
                    if not c:
                        continue
                    med = sorted(v[0] for v in c.values())
                    p95 = max(v[1] for v in c.values())
                    for k, v in c.items():
                        allk[k].append(v[1])
                    print('2006 %d / 2007 %d x%-2d y%d frames %d: joints %d, median of medians %.2f deg, worst 95%% %.2f deg' % (
                        n6, n7, x, y, nf, len(c), med[len(med) // 2], p95))
        if allk:
            print('per joint (95th percentile, median over tracks):', ' '.join(
                '%d:%.1f' % (k, sorted(v)[len(v) // 2]) for k, v in sorted(allk.items())))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
