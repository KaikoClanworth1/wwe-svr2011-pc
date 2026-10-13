"""Finger tracks for converted entrances (docs/HAND_POSES.md, docs/OTHER_GAMES_ENTRANCES.md "Hands").

2011's entrances pose the fingers with two tracks of the entrant beside the body (y0): y2 = right hand,
y3 = left hand (y1 = face). Their records are the hand-pose library's: 15 sub-records of 5 bytes
[k][count][s8 x][s8 y][s8 z] (k = finger joint 0-14 left, 15-29 right; angle = s8 * 256 * 2pi / 65536),
one per frame (count 0), the first with the event block every such track starts with.

Games that switch hand shapes by event (HCTP / SvR 2004 / Shut Your Mouth / SvR 2006: op 16
`10 ss iiii`, ss 1 left / 2 right, pose id 0-105 / 200-305 - 2011's own pose ids) get these tracks
made from the library: each pose = its library entry's first record (m.pac MVMT/STAT/99, read from the
2011 game's pac folder), blended linearly over BLEND frames from the shape before.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svrfmt  # noqa: E402
import ymks  # noqa: E402

BLEND = 5
DEFAULT = {1: 10, 2: 210}          # the ring idle hand (HAND_POSES.md)
Y_OF = {1: 3, 2: 2}                # hand -> sub-track
FIRST_EVENTS = bytes.fromhex('4005000000008900010000')   # (every stock y1-y3 track's frame 0)
_lib = {}


def library(game_pac):
    """{pose id: [(k, x, y, z)] * 15} from the 2011 game's m.pac MVMT/STAT/99; {} if not found"""
    if game_pac in _lib:
        return _lib[game_pac]
    out = {}
    try:
        d = open(os.path.join(game_pac, 'm.pac'), 'rb').read()
        _h, groups, _t = svrfmt.epac_read(d)
        for ty, es in groups:
            if ty != b'MVMT':
                continue
            for n, b in es:
                if n.strip() != b'STAT':
                    continue
                b = svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b
                lib = dict(svrfmt.pach_read(b))[99]
                lib = svrfmt.bpe_decode(lib) if lib[:4] == b'BPE ' else lib
                bank = ymks.read_bank(lib)
                for e in bank.entries:
                    if not e['data'] or e['id'] in out:
                        continue
                    for f, r in ymks.parse_stream(e['data']).records():
                        _ev, pose = ymks.split_record(r)
                        if len(pose) == 75:
                            out[e['id']] = [(pose[5 * i],) + struct.unpack_from('bbb', pose, 5 * i + 2)
                                            for i in range(15)]
                        break
    except Exception:
        out = {}
    _lib[game_pac] = out
    return out


def _record(shape, first):
    pose = b''.join(struct.pack('BBbbb', k, 0, x, y, z) for k, x, y, z in shape)
    return ymks.join_record(FIRST_EVENTS if first else b'', pose)


def track(events, nframes, hand, lib, start=None):
    """events: [(frame, pose id)] for one hand -> (YMKs motion data, the shape at the end).
    start: the shape the segment begins with (else the hand's default pose)."""
    cur = list(start or lib.get(DEFAULT[hand]) or [])
    if not cur:
        return None, None
    ev = sorted((f, p) for f, p in events if p in lib and 0 <= f < nframes)
    recs = []
    blend_from, blend_to, blend_at = None, None, 0
    i = 0
    for f in range(max(1, nframes)):
        while i < len(ev) and ev[i][0] <= f:
            blend_from, blend_to, blend_at = list(cur), lib[ev[i][1]], ev[i][0]
            i += 1
        if blend_to is not None:
            t = min(1.0, (f - blend_at + 1) / BLEND)
            cur = [(a[0],) + tuple(int(round(a[j] + (b[j] - a[j]) * t)) for j in (1, 2, 3))
                   for a, b in zip(blend_from, blend_to)]
            if t >= 1.0:
                blend_to = None
        recs.append((f, _record(cur, f == 0)))
    return ymks.build_stream(ymks.make_stream(recs, max(1, nframes))), cur
