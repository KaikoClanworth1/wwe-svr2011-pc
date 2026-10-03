"""Compare two frame-rate dumps (tools/rate_dump.ps1): for each 4-byte field of
wrestlers 1 and 2, how fast it changes per real second in each run. A field
changing ~N times as fast at N x the frame rate is stepped per frame.
   python tools/rate_diff.py runs/rd60.dump runs/rd120.dump [ratio]"""
import struct, sys
import numpy as np

SIZE = 0x10000
REC = 8 + 12 + 2 * SIZE

def load(path):
    raw = open(path, 'rb').read()
    n = len(raw) // REC
    t = np.zeros(n); fr = np.zeros(n)
    data = np.zeros((2, n, SIZE // 4), dtype='>u4')
    for i in range(n):
        o = i * REC
        t[i], = struct.unpack_from('<d', raw, o)
        fr[i], = struct.unpack_from('<I', raw, o + 8)
        for c in range(2):
            data[c, i] = np.frombuffer(raw, '>u4', SIZE // 4, o + 20 + c * SIZE)
    return t, fr, data

def speed(t, col, as_float):
    v = col.view('>f4').astype(np.float64) if as_float else col.astype(np.float64)
    d = np.abs(np.diff(v))
    ok = np.isfinite(d)
    if ok.sum() < 10: return None
    d = d[ok]
    lim = np.percentile(d, 90) * 4 + 1e-12   # (drop wraps and resets)
    d = d[d <= lim]
    return d.sum() / (t[-1] - t[0])

def main():
    a, b = sys.argv[1], sys.argv[2]
    ta, fa, da = load(a); tb, fb, db = load(b)
    print(f'{a}: {len(ta)} samples, {(fa[-1]-fa[0])/(ta[-1]-ta[0]):.0f} match frames/s;  '
          f'{b}: {len(tb)} samples, {(fb[-1]-fb[0])/(tb[-1]-tb[0]):.0f}/s')
    rows = []
    for c in range(2):
        for k in range(SIZE // 4):
            for fl in (True, False):
                ca, cb = da[c, :, k], db[c, :, k]
                if fl:
                    fa_ = ca.view('>f4'); fb_ = cb.view('>f4')
                    if not (np.all(np.isfinite(fa_)) and np.all(np.abs(fa_) < 1e6) and np.all(np.abs(fb_) < 1e6)): continue
                    if np.all(ca == ca[0]) and np.all(cb == cb[0]): continue
                else:
                    if ca.max() > 1e7 or cb.max() > 1e7: continue
                    if np.all(ca == ca[0]) and np.all(cb == cb[0]): continue
                sa, sb = speed(ta, ca, fl), speed(tb, cb, fl)
                if sa is None or sb is None: continue
                if sa == 0 and sb == 0: continue
                r = (sb + 1e-9) / (sa + 1e-9)
                rows.append((c, k * 4, 'f' if fl else 'i', sa, sb, r))
    want = float(sys.argv[3]) if len(sys.argv) > 3 else None
    print('wrestler offset type  speed@A  speed@B  B/A')
    for c, off, ty, sa, sb, r in sorted(rows, key=lambda x: -abs(np.log(x[5]))):
        if (r > 1.5 or r < 0.67) and (sa > 0 and sb > 0 or max(sa, sb) > 1):
            print(f'{c+1} +{off:5d} (0x{off:04X}) {ty} {sa:10.3f} {sb:10.3f} {r:6.2f}')

main()
