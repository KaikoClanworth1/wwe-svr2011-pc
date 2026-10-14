"""svr08_stage.py build with a collision index that BPE can compress: BPE needs a byte
value the block never uses, and a big index (300+ triangles) uses all 256. So no u16 in
the index may end in 0xFF: triangles whose index would end in 0xFF are dummies (far away,
in no cell), and a cell start ending in 0xFF is shifted by repeating the previous cell's
last triangle (tested twice: harmless)."""
import math
import struct
import sys

import numpy as np

sys.path.insert(0, r"D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\port\tools")
import svr08_stage as s8

DUMMY_Y = 5000.0


def with_dummies(recs):
    out = []
    for r in recs:
        while (len(out) & 0xFF) == 0xFF:
            d = recs[0].copy()
            M = d[:16].reshape(4, 4).copy()
            M[3, 1] -= DUMMY_Y           # far below the world
            d[:16] = M.flatten()
            d[22] = 0.0                  # (marker: radius 0)
            out.append(d)
        out.append(r)
    return out


def is_dummy(r):
    return float(r[22]) == 0.0 and np.allclose(np.array(s8.tri_corners(r))[:, 1].mean(), DUMMY_Y, atol=1000)


_hw, _ib = s8.hmd_write, s8.index_build


def hmd_write(recs):
    return _hw(with_dummies(recs))


def index_build(recs):
    recs = with_dummies(recs)
    cells = [[] for _ in range(200)]
    for k, r in enumerate(recs):
        if is_dummy(r):
            continue
        zs = [c[2] for c in s8.tri_corners(r)]
        for c in sorted(s8._cells(min(zs), max(zs))):
            if c < 200:
                cells[c].append(k)
    pairs, lst = [], []
    prev = None
    for c in cells:
        while (len(lst) & 0xFF) == 0xFF:
            if prev is not None and pairs and lst:
                lst.append(lst[-1])
                pairs[-1] += 1
            else:
                lst.append(lst[-1] if lst else 0)
        if any((v & 0xFF) == 0xFF for v in (len(lst), len(c))):
            pass
        pairs += [len(lst), len(c)]
        lst += c
        prev = c
    data = struct.pack(f'<{len(pairs)}H', *pairs) + struct.pack(f'<{len(lst)}H', *lst)
    used = set(data)
    if 0xFF in used:
        print('index: byte 0xFF still used')
    return data


s8.hmd_write, s8.index_build = hmd_write, index_build
a = sys.argv[1:]
pos = [x for x in a if not x.startswith('--')]


def opt(name):
    if name not in a:
        return None
    v = a[a.index(name) + 1]
    pos.remove(v)
    return v


ov, rv = opt('--offset'), opt('--rotate')
s8.build(pos[0], pos[1], pos[2], drop_cars='--drop-2011-cars' in a, report='--report' in a,
         offset=tuple(float(x) for x in ov.split(',')) if ov else None, rotate=int(rv) if rv else 0)
