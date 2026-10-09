"""Walk speed from a frame-rate dump (tools/lim_walk.ps1): each wrestler's
position (char +288 x, +296 z) per 100 ms sample while the match clock runs;
prints the moving speed (units per real second and per match frame) for
each wrestler, and the match frames per second.
   python tools/lim_walk.py runs/wk30.dump [runs/wk60.dump ...]"""
import struct, sys
import numpy as np

SIZE = 0x10000
REC = 8 + 12 + 2 * SIZE


def load(path):
    raw = open(path, 'rb').read()
    n = len(raw) // REC
    t = np.zeros(n); fr = np.zeros(n); pos = np.zeros((2, n, 2))
    for i in range(n):
        o = i * REC
        t[i], = struct.unpack_from('<d', raw, o)
        fr[i], = struct.unpack_from('<I', raw, o + 8)
        for c in range(2):
            b = o + 20 + c * SIZE
            pos[c, i] = struct.unpack_from('>ff', raw, b + 288)[0], struct.unpack_from('>f', raw, b + 296)[0]
    return t, fr, pos


for path in sys.argv[1:]:
    t, fr, pos = load(path)
    run = np.where(np.diff(fr) > 0)[0]          # (samples where the match clock moved)
    if len(run) < 10:
        print(path, 'no match frames'); continue
    dt = np.diff(t)[run]; df = np.diff(fr)[run]
    print(f'{path}: {len(run)} running samples, {df.sum() / dt.sum():.1f} match frames/s')
    for c in range(2):
        d = np.hypot(*np.diff(pos[c], axis=0)[run].T)
        moving = d > 0.5
        if moving.sum() < 3:
            print(f'  wrestler {c + 1}: hardly moves'); continue
        v = d[moving] / dt[moving]; vf = d[moving] / df[moving]
        print(f'  wrestler {c + 1}: moving {moving.sum()} samples, speed median {np.median(v):.1f}/s, '
              f'p90 {np.percentile(v, 90):.1f}/s, max {v.max():.1f}/s; per match frame median {np.median(vf):.2f}')
