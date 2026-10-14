"""HCTP (PS2) motion records -> poses.  Decoded from SLES_520.36:
CMotionData::DataAnalysis 0x1b77d0, SetBoneData 0x1b6bd0, CMotionHokan::BoneCreate 0x1b5c60,
type table s_sk_hokan 0x4124a0, names tblBoneName 0x4124c0.

Record = [event block if first byte & 0x80: (b & 0x7f) bytes] + pose sub-records (same op set
as SvR2011 m.pac: 0x60-0x6f / 0x7c / 0x7d full, 0x50-0x5f / 0x7a masked, 0x7e / 0x7f root,
0x7b bone translation, 0x00-0x14 one bone).  Bone types (HCTP table):
  7 7 7 14 23 6 7 3 14 6 7 3 14 7 2 14 10 7 2 14 10
with 14 = y,z 8-bit (value << 8), 10 = y 8-bit, when the motion object is created with
mode 1 (stock MOT banks); other modes widen 10 -> 2 (y s16) and 14 -> 6 (y,z s16).
"""
import struct, sys, os, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ymks as Y

H_TYPES = [7, 7, 7, 14, 23, 6, 7, 3, 14, 6, 7, 3, 14, 7, 2, 14, 10, 7, 2, 14, 10]
H_TYPES_WIDE = [2 if t == 10 else 6 if t == 14 else t for t in H_TYPES]
BONES = Y.BONES


def _sub(pose, p, types):
    """one HCTP pose sub-record at p -> (dict, next p).  Differences from SvR2011:
    masked forms 0x50-0x5f / 0x7a carry an s16 root (2011: f32); no 0x74/0x75/0x76."""
    op = pose[p]
    s = {'op': op}
    if op in (0x7e, 0x7f):
        s['count'] = pose[p + 1]
        if op == 0x7e:
            s['root'] = struct.unpack_from('<3h', pose, p + 2); s['root_fmt'] = 's16'; return s, p + 8
        s['root'] = struct.unpack_from('<3f', pose, p + 2); s['root_fmt'] = 'f32'; return s, p + 14
    if op == 0x7b:
        s['ref'] = pose[p + 1]; s['count'] = pose[p + 2]
        s['trans'] = struct.unpack_from('<3h', pose, p + 3); return s, p + 9
    if op in (0x7d, 0x7c) or 0x60 <= op <= 0x6f:
        if op == 0x7d:
            s['count'] = pose[p + 1]; s['root'] = struct.unpack_from('<3f', pose, p + 2); s['root_fmt'] = 'f32'; q = p + 14
        elif op == 0x7c:
            s['count'] = pose[p + 1]; s['root'] = struct.unpack_from('<3h', pose, p + 2); s['root_fmt'] = 's16'; q = p + 8
        else:
            s['count'] = op - 0x60; s['root'] = struct.unpack_from('<3h', pose, p + 1); s['root_fmt'] = 's16'; q = p + 7
        s['bones'] = {}
        for k in range(21):
            s['bones'][k], ln = Y._rd_bone(pose, q, types[k]); q += ln
        return s, q
    if op == 0x7a or 0x50 <= op <= 0x5f:
        if op == 0x7a:
            s['count'] = pose[p + 1]; q = p + 2
        else:
            s['count'] = op - 0x50; q = p + 1
        s['root'] = struct.unpack_from('<3h', pose, q); s['root_fmt'] = 's16'; q += 6
        m = pose[q] | pose[q + 1] << 8 | pose[q + 2] << 16
        q += 3
        s['mask'] = m
        s['bones'] = {}
        for k in range(21):
            if m >> k & 1:
                s['bones'][k], ln = Y._rd_bone(pose, q, types[k]); q += ln
        return s, q
    if op < 21:
        s['count'] = pose[p + 1]
        v, ln = Y._rd_bone(pose, p + 2, types[op])
        s['bones'] = {op: v}
        return s, p + 2 + ln
    raise ValueError('unknown HCTP pose op 0x%02x' % op)


def decode_body(pose, types=H_TYPES):
    """HCTP pose bytes -> {'subs': [...], 'trail': bytes} (loop while p < size-1, like the game)"""
    subs, p, n = [], 0, len(pose)
    while p < n - 1:
        s, q = _sub(pose, p, types)
        if q > n:
            raise ValueError('sub-record overruns the record')
        subs.append(s)
        p = q
    return {'subs': subs, 'trail': pose[p:]}


def encode_body(d, types=H_TYPES):
    out = bytearray()
    for s in d['subs']:
        op = s['op']
        if op in (0x7e, 0x7f):
            out += bytes([op, s['count']]) + (struct.pack('<3h', *s['root']) if op == 0x7e else struct.pack('<3f', *s['root']))
        elif op == 0x7b:
            out += bytes([op, s['ref'], s['count']]) + struct.pack('<3h', *s['trans'])
        elif op in (0x7d, 0x7c) or 0x60 <= op <= 0x6f:
            if op == 0x7d:
                out += bytes([op, s['count']]) + struct.pack('<3f', *s['root'])
            elif op == 0x7c:
                out += bytes([op, s['count']]) + struct.pack('<3h', *s['root'])
            else:
                out += bytes([op]) + struct.pack('<3h', *s['root'])
            for k in range(21):
                out += Y._wr_bone(s['bones'].get(k, {}), types[k])
        elif op == 0x7a or 0x50 <= op <= 0x5f:
            out += bytes([op, s['count']]) if op == 0x7a else bytes([op])
            out += struct.pack('<3h', *s['root'])
            m = s['mask']
            out += bytes([m & 0xff, m >> 8 & 0xff, m >> 16 & 0xff])
            for k in range(21):
                if m >> k & 1:
                    out += Y._wr_bone(s['bones'][k], types[k])
        else:
            out += bytes([op, s['count']]) + Y._wr_bone(s['bones'][op], types[op])
    return bytes(out) + d['trail']


# ---------------------------------------------------------------- objects (props / weapons)
# CSD3MotionWeapon::SetData 0x1c2260 / SetRot 0x1c1c10: every bone is x,y,z s16 (RotZYX, 2pi/65536),
# root/translations s16 * 0.01 (0x464a28).  Ops: 0x7e/0x7d [op][count][s16 root x3] (8 bytes),
# 0x7a [7a][count][s16 root x3][mask 3 bytes][6 bytes per bone in mask, bones 0-20],
# 0x7b [7b][bone][count][s16 t x3] (9), 0x00-0x1f [bone][count][s16 x,y,z] (8).  Any other op ends the record.

def decode_obj(pose):
    subs, p, n = [], 0, len(pose)
    while p < n - 1:
        op = pose[p]
        s = {'op': op}
        if op in (0x7e, 0x7d):
            s['count'] = pose[p + 1]; s['root'] = struct.unpack_from('<3h', pose, p + 2); q = p + 8
        elif op == 0x7a:
            s['count'] = pose[p + 1]; s['root'] = struct.unpack_from('<3h', pose, p + 2)
            m = pose[p + 8] | pose[p + 9] << 8 | pose[p + 10] << 16
            s['mask'] = m; s['bones'] = {}
            q = p + 11
            for k in range(21):
                if m >> k & 1:
                    s['bones'][k] = dict(zip('xyz', struct.unpack_from('<3h', pose, q))); q += 6
        elif op == 0x7b:
            s['ref'] = pose[p + 1]; s['count'] = pose[p + 2]; s['trans'] = struct.unpack_from('<3h', pose, p + 3); q = p + 9
        elif op < 0x20:
            s['count'] = pose[p + 1]; s['bones'] = {op: dict(zip('xyz', struct.unpack_from('<3h', pose, p + 2)))}; q = p + 8
        else:
            raise ValueError('object op 0x%02x' % op)
        if q > n:
            raise ValueError('object sub-record overruns')
        subs.append(s)
        p = q
    return {'subs': subs, 'trail': pose[p:]}


def encode_obj(d):
    out = bytearray()
    for s in d['subs']:
        op = s['op']
        if op in (0x7e, 0x7d):
            out += bytes([op, s['count']]) + struct.pack('<3h', *s['root'])
        elif op == 0x7a:
            m = s['mask']
            out += bytes([op, s['count']]) + struct.pack('<3h', *s['root']) + bytes([m & 0xff, m >> 8 & 0xff, m >> 16 & 0xff])
            for k in range(21):
                if m >> k & 1:
                    b = s['bones'][k]; out += struct.pack('<3h', b['x'], b['y'], b['z'])
        elif op == 0x7b:
            out += bytes([op, s['ref'], s['count']]) + struct.pack('<3h', *s['trans'])
        else:
            b = s['bones'][op]; out += bytes([op, s['count']]) + struct.pack('<3h', b['x'], b['y'], b['z'])
    return bytes(out) + d['trail']


def track_kind(data, y=0):
    """'char' (CMotionData, HCTP types), 'obj' (CSD3MotionWeapon, all xyz), 'scalar5' (5-byte keys),
    'cam13' (13-byte camera keys), 'events' (no poses) or None.
    A track that parses both ways (only 0x7a / 0x7b / 0x7e / single-bone records) is 'obj' for
    y >= 1 and 'char' for y = 0; single-bone 0x00 and masks of x,y,z bones are byte-identical
    under both readings, so the choice only matters for masks with 8-bit / 2-axis bones."""
    recs = [Y.split_record(r)[1] for f, r in records(data)]
    poses = [p for p in recs if len(p) > 1]
    if not poses:
        return 'events'
    ok = {}
    for kind, fn in (('char', decode_body), ('obj', decode_obj)):
        try:
            ok[kind] = all(not fn(p)['trail'] for p in poses)
        except Exception:
            ok[kind] = False
    if ok['char'] and ok['obj']:
        return 'obj' if y >= 1 else 'char'
    if ok['char']:
        return 'char'
    if ok['obj']:
        return 'obj'
    if all(len(p) == 5 for p in poses):
        return 'scalar5'
    if all(len(p) == 13 for p in poses):
        return 'cam13'
    return None


def read_bank(d):
    """HCTP CEKey bank (no 16-byte header) -> (T, entries); entry: id, x, y, frames, u2, data, off"""
    base = 0x10 if d[:4] == b'YMKs' else 0
    T = d[base:base + 256]
    n = struct.unpack_from('<I', d, base + 0x100)[0]
    dirs = [struct.unpack_from('<IIII', d, base + 0x104 + 16 * i) for i in range(n)]
    data = d[base + 0x104 + 16 * n:]
    offs = sorted(set(e[1] for e in dirs if e[1] < len(data)))
    nxt = {o: (offs[i + 1] if i + 1 < len(offs) else len(data)) for i, o in enumerate(offs)}
    out = []
    for key, off, a, b in dirs:
        # key = id<<16 | x<<8 | y : y (low byte) is the track (0 attacker, 1 defender, ...)
        out.append(dict(id=key >> 16, x=(key >> 8) & 0xff, y=key & 0xff, frames=a, u2=b, off=off,
                        data=data[off:nxt[off]] if off in nxt else b''))
    return T, out


def records(data):
    """HCTP frame stream (CEKey::GetData 0x168f00): b < 0x60 skip b frames; b == 0x60: u8 size
    follows (2011: u16); b > 0x60: size b - 0x60.  Every record is one frame. -> [(frame, bytes)]"""
    out, p, f = [], 0, 0
    while p < len(data):
        b = data[p]
        if b < 0x60:
            f += b; p += 1; continue
        if b == 0x60:
            sz = data[p + 1]; p += 2
        else:
            sz = b - 0x60; p += 1
        if p + sz > len(data):
            raise ValueError('record overruns motion data')
        out.append((f, data[p:p + sz]))
        p += sz
        f += 1
    return out


def decode_track(data, wide=False):
    """-> list of (frame, events bytes, decoded body dict or None, raw pose)"""
    types = H_TYPES_WIDE if wide else H_TYPES
    out = []
    for f, r in records(data):
        ev, pose = Y.split_record(r)
        body = decode_body(pose, types) if len(pose) > 1 else None
        out.append((f, ev, body, pose))
    return out


def is_char_track(data):
    """every pose decodes with no trailing bytes under the HCTP table"""
    try:
        for f, ev, body, pose in decode_track(data):
            if body is not None and body['trail']:
                return False
            if body is None and pose:
                return False
    except Exception:
        return False
    return True


def keys(data, wide=False):
    """absolute keyed poses: list of (target frame, root (x,y,z) 0.01u or None, {bone: {axis: s16}})
    target frame = frame + count + 1 (blend end)."""
    out = []
    for f, ev, body, pose in decode_track(data, wide):
        if not body:
            continue
        for s in body['subs']:
            if 'bones' in s or s['op'] in (0x7e, 0x7f):
                out.append((f + s.get('count', 0) + 1, f, s.get('root'),
                            s.get('bones', {}), s['op']))
    return out


def sample(data, nframes=None, wide=False):
    """per-frame absolute pose (held values, linear blend toward each key like the game:
    the key at frame f with count c is reached at f+c+1). -> list of (root, angles[21][3] s16-float)"""
    ks = keys(data, wide)
    root = [0.0, 0.0, 0.0]
    ang = [[0.0, 0.0, 0.0] for _ in range(21)]
    if nframes is None:
        nframes = max([k[0] for k in ks] + [1]) + 1
    # build timelines per channel: (start frame, start value?, end frame, end value)
    chans = collections.defaultdict(list)
    for tgt, f, r, bones, op in ks:
        if r is not None:
            for a in range(3):
                chans[('r', a)].append((f, tgt, float(r[a])))
        for b, v in bones.items():
            for ai, ax in enumerate('xyz'):
                if ax in v:
                    chans[(b, ai)].append((f, tgt, float(v[ax])))
    frames = []
    cur = {}
    idx = {c: 0 for c in chans}
    state = {}   # chan -> (f0, v0, f1, v1)
    for fr in range(nframes):
        for c, lst in chans.items():
            while idx[c] < len(lst) and lst[idx[c]][0] <= fr:
                f, tgt, v = lst[idx[c]]
                v0 = cur.get(c, v)
                if c[0] != 'r':
                    # shortest-path unwrap (s16 angles)
                    d = (v - v0 + 32768) % 65536 - 32768
                    v = v0 + d
                state[c] = (f, v0, tgt, v)
                idx[c] += 1
            if c in state:
                f0, v0, f1, v1 = state[c]
                t = 1.0 if f1 <= f0 else min(1.0, (fr - f0) / float(f1 - f0))
                cur[c] = v0 + (v1 - v0) * t
        r = [cur.get(('r', a), 0.0) for a in range(3)]
        A = [[cur.get((b, a), 0.0) for a in range(3)] for b in range(21)]
        frames.append((r, A))
    return frames


if __name__ == '__main__':
    path = sys.argv[1]
    T, ents = read_bank(open(path, 'rb').read())
    st = collections.Counter()
    for e in ents:
        if not e['data']:
            st['empty'] += 1
            continue
        st[(e['y'] if e['y'] < 10 or e['y'] >= 100 else 'y10-49' if e['y'] < 50 else 'y50+', track_kind(e['data'], e['y']))] += 1
    print(path, st)
