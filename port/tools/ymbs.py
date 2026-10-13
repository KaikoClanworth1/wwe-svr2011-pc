"""YMBs motion banks (the ADPCM ones: taunts, submissions, some finishers) - decoder and encoder,
for moves_port.py's taunt path (ymbs_retarget.py makes the channel values).

Bank: 'YMBs' u32 ver | 8 zero | T[256] channel-definition table @0x10 |
      u32 count @0x110 | dir[count] {u8 y, u8 x, u16 id, u32 off, u32 frames, u32 rt} |
      database: u32 ? | hdr[count] 20 bytes {u8 type, u8 nseg, u8 interval, u8 pad, f32 a,b,c,d} |
      motion data at database+off. All little-endian.

A motion is channel streams back to back, in the order of the bank's channel list for its type
(T table). Codecs (Yamaha-style ADPCM, step table STEP, step = step * tab >> 6 clamped to
[127, 24576]):
  T32  (translation / IK): s24 init, u8 s (step = s*127), 2K nibbles  -> 2K+1 values
  R16n (rotation, nibble):  s16 init, u8 s, K nibbles               -> K+1 values (16-bit wrap)
  R16_6 (6-bit):            s16 init, u8 s, K bytes                 -> K+1 values
  raw s16:                  K values
Channel flags: 0x80|0x40 IK target xyz (T32), 0x80 limb pole / twist (R16n), 0x40 root translation
(T32), 0x20 extra, else rotation axes (bits 0-2 = x y z; b1 & 0xF0 == 0x40 raw, b1 & 15 == 2 6-bit).
The encoder picks, per channel, the step byte with the least error and then each code against the
decoder's own state (greedy, or a beam search when greedy lags), so it never drifts.
"""
import struct
STEP = (57, 57, 57, 57, 77, 102, 128, 153)

def adpcm(init, step, codes, bits4=True):
    """codes: list of ints (nibbles or 6-bit). returns keys[0..len(codes)] (key0 = init)."""
    v = init; out = [v]
    for c in codes:
        if bits4:
            mag = c & 7; neg = c & 8; d = ((mag * 2 + 1) * step) >> 3; idx = mag
        else:
            mag = c & 0x1F; neg = c & 0x20; d = ((mag * 2 + 1) * step) >> 5; idx = min(mag >> 2, 7)
        v = v - d if neg else v + d
        step = (step * STEP[idx]) >> 6
        step = 127 if step < 127 else (24576 if step > 24576 else step)
        out.append(v)
    return out

def nibbles(b, n):
    out = []
    for i in range(n):
        x = b[i >> 1]
        out.append((x >> 4) if i % 2 == 0 else (x & 15))
    return out

def ch32(d, p, K):
    """translation channel: s24 init, u8 step, 2K nibbles; length K+4"""
    init = int.from_bytes(d[p:p + 3], 'little', signed=True)
    step = d[p + 3] * 127
    return adpcm(init, step, nibbles(d[p + 4:p + 4 + K], 2 * K)), K + 4

def ch16(d, p, K, mode):
    """rotation channel. mode 'n' nibble (len (K+1)//2+3), '6' 6-bit (len K+3), 'raw' s16 (len 2K)"""
    if mode == 'raw':
        return [struct.unpack_from('<h', d, p + 2 * i)[0] for i in range(K)], 2 * K
    init = struct.unpack_from('<h', d, p)[0]
    step = d[p + 2] * 127
    if mode == '6':
        vals = adpcm(init, step, list(d[p + 3:p + 3 + K]), False)
        return [((x + 0x8000) & 0xFFFF) - 0x8000 for x in vals], K + 3
    n = (K + 1) // 2
    vals = adpcm(init, step, nibbles(d[p + 3:p + 3 + n], K))
    return [((x + 0x8000) & 0xFFFF) - 0x8000 for x in vals], n + 3

class Bank:
    def __init__(self, raw):
        assert raw[:4] == b'YMBs'
        self.d = raw
        self.T = raw[0x10:0x110]
        n = struct.unpack_from('<I', raw, 0x110)[0]
        self.dir = [struct.unpack_from('<BBHIII', raw, 0x114 + 16 * i) for i in range(n)]
        self.base = 0x114 + 16 * n
        self.hdr = [struct.unpack_from('<BBBB4f', raw, self.base + 4 + 20 * i) for i in range(n)]
    def find(self, mid, x=0, y=0):
        for k, e in enumerate(self.dir):
            if e[2] == mid and e[1] == x and e[0] == y: return k
    def channels(self, typ):
        T = self.T
        lst = T[T[typ + 1]:]  # T + T[typ+1]: count, ids
        cnt = lst[0]
        desc0 = T[T[0] + 1]   # T + T[T[0]+1] = descriptor table (1-based ids)
        return [(b, T[desc0 + 4 * (b - 1):desc0 + 4 * b]) for b in lst[1:1 + cnt]]
    def decode(self, k):
        """-> dict with per-channel key arrays (single segment motions)."""
        typ, nseg, interval, _, a, b, c, dd = self.hdr[k]
        frames = self.dir[k][4]
        interval = interval or 1
        p = self.base + self.dir[k][3]
        segs = []
        if nseg:
            for s in range(nseg):
                st, kk, off = struct.unpack_from('<HHI', self.d, p + 8 * s)
                segs.append((st, kk, off))
        else:
            K = (frames + interval - 1) // interval
            segs = [(0, K, 0)]
        out = []
        for st, K, off in segs:
            q = p + off
            res = {'start': st, 'K': K, 'root': None, 'ik': [], 'twist': [], 'rot': [], 'extra': None}
            for bid, desc in self.channels(typ):
                f, b1, scale = desc[0], desc[1], desc[2]
                if f & 0x80:
                    if f & 0x40:
                        xyz = []
                        for _ in range(3):
                            v, ln = ch32(self.d, q, K); q += ln; xyz.append(v)
                        res['ik'].append((bid, scale, xyz))
                    else:
                        v, ln = ch16(self.d, q, K, 'n'); q += ln
                        res['twist'].append((bid, scale, v))
                elif f & 0x40:   # root translation (0x50 = same data, game may time-blend it at runtime)
                    xyz = []
                    for _ in range(3):
                        v, ln = ch32(self.d, q, K); q += ln; xyz.append(v)
                    res['root'] = (bid, scale, xyz)
                elif f & 0x20:
                    mode = '6' if (b1 & 15) == 2 else 'n'
                    v, ln = ch16(self.d, q, K, mode); q += ln
                    res['extra'] = (bid, scale, v)
                else:
                    axes = []
                    for ax in range(3):
                        if f & (1 << ax):
                            if (b1 & 0xF0) == 0x40: mode = 'raw'
                            elif (b1 & 15) == 2: mode = '6'
                            else: mode = 'n'
                            v, ln = ch16(self.d, q, K, mode); q += ln
                            axes.append(v)
                        else:
                            axes.append(None)
                    res['rot'].append((bid, scale, axes))
            res['end'] = q
            out.append(res)
        return out


# ---------------------------------------------------------------- encoder

def _run(init, s, targets, bits4, wrap):
    """greedy codes for targets[1:] from init; returns (codes, err, values)"""
    v = init
    step = s * 127
    codes, vals, err = [], [v], 0
    nmag = 8 if bits4 else 32
    for t in targets[1:]:
        best = None
        for mag in range(nmag):
            d = ((mag * 2 + 1) * step) >> (3 if bits4 else 5)
            for neg in (0, 1):
                nv = v - d if neg else v + d
                diff = t - nv
                if wrap:
                    diff = ((diff + 0x8000) & 0xFFFF) - 0x8000
                e = abs(diff)
                if best is None or e < best[0]:
                    best = (e, mag, neg, nv)
        e, mag, neg, nv = best
        codes.append(mag | ((8 if bits4 else 0x20) if neg else 0))
        v = nv
        idx = mag if bits4 else min(mag >> 2, 7)
        step = (step * STEP[idx]) >> 6
        step = 127 if step < 127 else (24576 if step > 24576 else step)
        vals.append(v)
        err += e
    return codes, err, vals


def _beam(init, s, targets, bits4, wrap, width=48):
    """codes for targets[1:] by beam search over (value, step) states: the
    sum of absolute errors, so a sudden jump is reached as soon as the
    adaptive step allows instead of lagging behind (greedy)."""
    nmag = 8 if bits4 else 32
    sh = 3 if bits4 else 5
    beam = [(0, init, s * 127, [])]
    for t in targets[1:]:
        cand = {}
        for err, v, step, codes in beam:
            for mag in range(nmag):
                d = ((mag * 2 + 1) * step) >> sh
                idx = mag if bits4 else min(mag >> 2, 7)
                ns = (step * STEP[idx]) >> 6
                ns = 127 if ns < 127 else (24576 if ns > 24576 else ns)
                for neg in (0, 1):
                    nv = v - d if neg else v + d
                    diff = t - nv
                    if wrap:
                        diff = ((diff + 0x8000) & 0xFFFF) - 0x8000
                    e = err + abs(diff)
                    key = (nv, ns)
                    if key not in cand or cand[key][0] > e:
                        cand[key] = (e, nv, ns, codes + [mag | ((8 if bits4 else 0x20) if neg else 0)])
        beam = sorted(cand.values(), key=lambda c: c[0])[:width]
    e, v, step, codes = beam[0]
    return codes, e


def _best(init, targets, bits4, wrap):
    g = _best_greedy(init, targets, bits4, wrap)
    if g[1] <= 8 * len(targets):  # (greedy already within ~8 units a key: keep it)
        return g
    best = g
    for s in sorted({g[0], max(1, g[0] // 2), min(255, g[0] * 2), 8, 32, 96}):
        codes, err = _beam(init, s, targets, bits4, wrap)
        if err < best[1]:
            best = (s, err, codes)
    return best


def _best_greedy(init, targets, bits4, wrap):
    best = None
    for s in list(range(1, 33)) + list(range(36, 256, 8)):
        codes, err, vals = _run(init, s, targets, bits4, wrap)
        if best is None or err < best[1]:
            best = (s, err, codes)
    return best


def enc_t32(values):
    """values: 2K+1 ints (s24). -> bytes (K+4)"""
    init = max(-0x800000, min(0x7FFFFF, int(round(values[0]))))
    tg = [int(round(x)) for x in values]
    s, err, codes = _best(init, tg, True, False)
    out = bytearray(init.to_bytes(3, 'little', signed=True)) + bytes([s])
    for i in range(0, len(codes), 2):
        out.append((codes[i] << 4) | (codes[i + 1] if i + 1 < len(codes) else 0))
    return bytes(out)


def enc_r16(values, mode):
    """values: K+1 ints (angle s16 units, any wrap) for 'n'/'6'; K for 'raw'."""
    if mode == 'raw':
        return b''.join(struct.pack('<h', ((int(round(x)) + 0x8000) & 0xFFFF) - 0x8000) for x in values)
    init = ((int(round(values[0])) + 0x8000) & 0xFFFF) - 0x8000
    tg = [int(round(x)) for x in values]
    s, err, codes = _best(init, tg, mode == 'n', True)
    out = bytearray(struct.pack('<h', init)) + bytes([s])
    if mode == '6':
        out += bytes(codes)
    else:
        for i in range(0, len(codes), 2):
            out.append((codes[i] << 4) | (codes[i + 1] if i + 1 < len(codes) else 0))
    return bytes(out)


def encode_motion(bank, typ, K, chans):
    """chans: the same structure ymbs.Bank.decode() returns for one segment
    (root (bid, scale, [x,y,z]), ik [(bid, scale, [x,y,z])], twist [(bid,
    scale, v)], rot [(bid, scale, [ax|None]*3)], extra). Values are in the
    stored integer units. Returns the motion's data bytes."""
    out = bytearray()
    ik = iter(chans['ik'])
    tw = iter(chans['twist'])
    rot = iter(chans['rot'])
    for bid, desc in bank.channels(typ):
        f, b1 = desc[0], desc[1]
        if f & 0x80:
            if f & 0x40:
                _, _, xyz = next(ik)
                for v in xyz:
                    out += enc_t32(v)
            else:
                _, _, v = next(tw)
                out += enc_r16(v, 'n')
        elif f & 0x40:
            for v in chans['root'][2]:
                out += enc_t32(v)
        elif f & 0x20:
            mode = '6' if (b1 & 15) == 2 else 'n'
            out += enc_r16(chans['extra'][2], mode)
        else:
            _, _, axes = next(rot)
            for ax in range(3):
                if f & (1 << ax):
                    mode = 'raw' if (b1 & 0xF0) == 0x40 else ('6' if (b1 & 15) == 2 else 'n')
                    out += enc_r16(axes[ax], mode)
    return bytes(out)
