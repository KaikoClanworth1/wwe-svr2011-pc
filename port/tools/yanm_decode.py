"""WWE '13 (Yuke's) YANM skeletal animation decoder.

Reverse-engineered from the WWE '13 X360 executable (see YANM_SPEC.md next to this file).

    import yanm_decode as Y
    rec = open('t37281_5.bin','rb').read()          # one decoded motion record
    m = Y.decode(rec)                                # first person/block
    m['frames'], m['fps'], m['bones']['Bip L Thigh']['rot'][0]   # quaternion (x,y,z,w)

    blocks = Y.decode_record(rec)                    # every block (two-man moves: '00000000', '00000001', ...)
    q = Y.sample_track(m['tracks']['Bip L Thigh'], 12.5)          # evaluation at any frame

    # optional full pose (IK + FK) against a WWE'13 skeleton node table
    pose = Y.pose(m, nodes, 12.5)   # nodes = w13_char.read13(...)['nodes']

Conventions (match the WWE'13 model node table):
  * quaternions are (x, y, z, w), Hamilton; q_euler(x,y,z) = qz*qy*qx  ->  R = Rz*Ry*Rx (column vectors),
    i.e. the same Euler convention as the node table (local = T(t)*Rz*Ry*Rx).
  * rotation tracks are ABSOLUTE local rotations (they replace the bind rotation).
  * translation tracks are ADDED to the bind translation (local t = bind t + track), cm, model space has -Y up.
  * time unit = frames of a 60 Hz clock (keys carry integer frame deltas).
"""
import struct, math

FPS = 60.0
PI = math.pi

# ----------------------------------------------------------------------------- names
def _rotl(h, r):
    return ((h << r) | (h >> (32 - r))) & 0xffffffff


def name_hash(s):
    """Track name hash: h = rotl(h,7) ^ c  (game: (h<<7 | c) ^ (h>>25))."""
    h = 0
    for c in s.encode('latin-1'):
        h = _rotl(h, 7) ^ c
    return h


STD_NAMES = ['Bip', 'Bip Pelvis', 'Bip L Thigh', 'Bip L Calf', 'Bip L Foot', 'Bip L Toe0', 'Bip R Thigh',
             'Bip R Calf', 'Bip R Foot', 'Bip R Toe0', 'Bip Spine', 'Bip Spine1', 'Bip Neck', 'Bip Head',
             'Bip L Clavicle', 'Bip L UpperArm', 'Bip L Forearm', 'Bip L Hand', 'Bip R Clavicle',
             'Bip R UpperArm', 'Bip R Forearm', 'Bip R Hand', 'vector', 'root', 'null']
NAMES = {name_hash(n): n for n in STD_NAMES}
NAMES[0x1e796cf7] = '#timing'      # name unknown; zero-valued keys, only its key times (see spec)
NAMES[0x1e796cf4] = '#timing2'
NAMES[0xcd7ed4b1] = '#extra_cd7ed4b1'
NAMES[0x1639dcce] = '#extra_1639dcce'


def add_names(names):
    for n in names:
        NAMES.setdefault(name_hash(n), n)


# Track templates used by "templated" YANMs (header byte1 == 2, u16 = template id). Copied from the
# executable table at 0x82014470 (ids 1..7 -> 0x82013f18, 0x82013fe0, 0x82014088, 0x82014228,
# 0x82014300, 0x820143a8, 0x82014150). Entries: (hash, flags).
_T = lambda *a: [(name_hash(n) if isinstance(n, str) else n, f) for n, f in a]
_LEGS_FULL = [('Bip L Thigh', 0x5000), ('Bip L Calf', 0x0c00), ('Bip L Foot', 0x5000), ('Bip L Toe0', 0x0800),
              ('Bip R Thigh', 0x5000), ('Bip R Calf', 0x0c00), ('Bip R Foot', 0x5000), ('Bip R Toe0', 0x0800)]
_UP_FULL = [('Bip Spine', 0x4000), ('Bip Spine1', 0x4000), ('Bip Neck', 0x4000), ('Bip Head', 0x4000),
            ('Bip L Clavicle', 0x4000), ('Bip L UpperArm', 0x4000), ('Bip L Forearm', 0x0800), ('Bip L Hand', 0x4000),
            ('Bip R Clavicle', 0x4000), ('Bip R UpperArm', 0x4000), ('Bip R Forearm', 0x0800), ('Bip R Hand', 0x4000)]
_LEGS_LITE = [('Bip L Thigh', 0x4000), ('Bip L Calf', 0x0800), ('Bip L Foot', 0x4000),
              ('Bip R Thigh', 0x4000), ('Bip R Calf', 0x0800), ('Bip R Foot', 0x4000)]
_LEGS_LITE_T = [('Bip L Thigh', 0x4000), ('Bip L Calf', 0x0800), ('Bip L Foot', 0x4000), ('Bip L Toe0', 0x0800),
                ('Bip R Thigh', 0x4000), ('Bip R Calf', 0x0800), ('Bip R Foot', 0x4000), ('Bip R Toe0', 0x0800)]
_UP_LITE = [('Bip Spine', 0x4000), ('Bip Spine1', 0x4000), ('Bip Neck', 0x4000), ('Bip Head', 0x4000),
            ('Bip L Clavicle', 0x4000), ('Bip L UpperArm', 0x4000), ('Bip L Forearm', 0x0800),
            ('Bip R Clavicle', 0x4000), ('Bip R UpperArm', 0x4000), ('Bip R Forearm', 0x0800)]
TEMPLATES = {
    1: _T((0x1e796cf7, 0x4000), ('Bip', 0x5100), ('Bip Pelvis', 0x5000), *_LEGS_FULL, *_UP_FULL, ('vector', 0x4100)),
    2: _T((0x1e796cf7, 0x4000), ('Bip', 0x4100), ('Bip Pelvis', 0x4000), *_LEGS_LITE, *_UP_LITE, ('vector', 0x4100)),
    3: _T((0x1e796cf7, 0x4000), ('Bip', 0x4100), ('Bip Pelvis', 0x4000), *_LEGS_LITE_T,
          *[(n, f) for n, f in _UP_FULL], ('vector', 0x4100)),
    4: _T((0x1e796cf7, 0x4000), ('Bip', 0x5100), ('Bip Pelvis', 0x5000), *_UP_FULL, *_LEGS_FULL,
          (0xcd7ed4b1, 0x4000), (0x1639dcce, 0x4000), ('vector', 0x4100)),
    5: _T((0x1e796cf7, 0x4000), ('Bip', 0x4100), ('Bip Pelvis', 0x4000), *_UP_LITE, *_LEGS_LITE, ('vector', 0x4100)),
    6: _T((0x1e796cf4, 0x4100), ('Bip', 0x4100), ('Bip Pelvis', 0x4000), *_UP_FULL, *_LEGS_LITE_T, ('vector', 0x4100)),
    7: _T((0x1e796cf7, 0x2000), ('Bip', 0x0104), ('Bip Pelvis', 0x0004), ('Bip L Thigh', 0x0004), ('Bip L Calf', 0x0004),
          ('Bip L Foot', 0x0004), ('Bip L Toe0', 0x2000), ('Bip R Thigh', 0x0004), ('Bip R Calf', 0x0004),
          ('Bip R Foot', 0x0004), ('Bip R Toe0', 0x2000), *[(n, 0x2000) for n, _ in _UP_FULL],
          (0xcd7ed4b1, 0x2000), (0x1639dcce, 0x2000), ('vector', 0x2100)),
}

# ----------------------------------------------------------------------------- math
def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by,
            aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw,
            aw * bw - ax * bx - ay * by - az * bz)


def qaxis(axis, ang):
    s, c = math.sin(ang * 0.5), math.cos(ang * 0.5)
    v = [0.0, 0.0, 0.0]
    v[axis] = s
    return (v[0], v[1], v[2], c)


def q_euler(x, y, z):
    """Game routine 0x82b26448: q = qz * qy * qx (R = Rz Ry Rx)."""
    return qmul(qmul(qaxis(2, z), qaxis(1, y)), qaxis(0, x))


def qnorm(q):
    n = math.sqrt(sum(c * c for c in q)) or 1.0
    return tuple(c / n for c in q)


def qdot(a, b):
    return sum(x * y for x, y in zip(a, b))


def qslerp(a, b, t):
    d = qdot(a, b)
    if d < 0:
        b, d = tuple(-c for c in b), -d
    if d > 0.9995:
        return qnorm(tuple(x + (y - x) * t for x, y in zip(a, b)))
    th = math.acos(min(1.0, d))
    s = math.sin(th)
    wa, wb = math.sin((1 - t) * th) / s, math.sin(t * th) / s
    return tuple(wa * x + wb * y for x, y in zip(a, b))


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def qlog(q):
    v = math.sqrt(q[0] ** 2 + q[1] ** 2 + q[2] ** 2)
    if v < 1e-9:
        return (0.0, 0.0, 0.0, 0.0)
    th = math.atan2(v, q[3])
    return (q[0] / v * th, q[1] / v * th, q[2] / v * th, 0.0)


def qexp(v):
    th = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2)
    if th < 1e-9:
        return (0.0, 0.0, 0.0, 1.0)
    s = math.sin(th) / th
    return (v[0] * s, v[1] * s, v[2] * s, math.cos(th))


def _squad_inner(qm, q0, qp):
    """XMQuaternionSquadSetup-style control point (game 0x82b267e0)."""
    iq = qconj(q0)
    a, b = qlog(qmul(iq, qp)), qlog(qmul(iq, qm))
    return qmul(q0, qexp(tuple(-(x + y) * 0.25 for x, y in zip(a, b))))


def q_to_mat(q):
    x, y, z, w = q
    return [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]


def mat_to_q(m):
    tr = m[0][0] + m[1][1] + m[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        return qnorm(((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s))
    if m[0][0] > m[1][1] and m[0][0] > m[2][2]:
        s = math.sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2
        return qnorm((0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s))
    if m[1][1] > m[2][2]:
        s = math.sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2
        return qnorm(((m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s))
    s = math.sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2
    return qnorm(((m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s))


def q_to_euler(q):
    """Inverse of q_euler (ZYX): returns (x, y, z) radians like the node table."""
    m = q_to_mat(q)
    sy = -m[2][0]
    sy = max(-1.0, min(1.0, sy))
    y = math.asin(sy)
    if abs(sy) < 0.99999:
        x = math.atan2(m[2][1], m[2][2])
        z = math.atan2(m[1][0], m[0][0])
    else:
        x = math.atan2(-m[1][2], m[1][1])
        z = 0.0
    return (x, y, z)


def _half(h):
    s = -1.0 if h & 0x8000 else 1.0
    e = (h >> 10) & 0x1f
    f = h & 0x3ff
    if e == 0:
        return s * f * 2.0 ** -24
    if e == 31:
        return s * float('inf') if f == 0 else float('nan')
    return s * (1 + f / 1024.0) * 2.0 ** (e - 15)


def _spline4(p0, p1, p2, p3, t):
    """Game routine 0x8237c408: local natural cubic spline through p1..p2 (scalar)."""
    d1 = p2 - 2 * p1 + p0
    d2 = p3 - 2 * p2 + p1
    m2 = (24 * d2 - 6 * d1) / 15.0
    m1 = (6 * d1 - m2) / 4.0
    u = 1 - t
    return p1 * u + p2 * t + ((u ** 3 - u) * m1 + (t ** 3 - t) * m2) / 6.0


# ----------------------------------------------------------------------------- descriptor layout
# flag bit -> (kind, body size). Order in the body = descending bit order (0x8000 first ... 0x0001 last).
COMPONENTS = [
    (0x8000, 'pos8', 24),     # {u32 off, u32 n, f32 scale, f32 bias xyz}; keys s8 dx,dy,dz,u8 dt (deltas)
    (0x4000, 'rot8', 8),      # {off, n}; keys s8 ex,ey,ez,u8 dt; angle = v*pi/128 (absolute Euler)
    (0x2000, 'rot16', 8),     # {off, n}; keys s16 ex,ey,ez,u16 dt; angle = v*pi/32768
    (0x1000, 'rot8f', 8),     # {off, n}; keys s8 + u8 dt, v*pi/4608  (multiplied after rot8: q = q8 * q8f)
    (0x0800, 'hinge', 8),     # {off, u16 n, u16 axis}; keys s8 v,u8 dt; v*pi/128 about axis (0=X,1=Y,2=Z)
    (0x0400, 'hinge_f', 8),   # second hinge, v*pi/4608, multiplied after
    (0x0200, 'scale', 8),     # {off, n}; keys s16 sx,sy,sz,u16 absolute frame; v/256
    (0x0100, 'pos16', 24),    # {off, n, scale, bias xyz}; keys s16 dx,dy,dz,u16 dt (deltas)
    (0x0010, 'rot8ff', 8),    # {off, n}; s8 + dt, v*pi/11520, multiplied after rot8f
    (0x0008, 'hinge_ff', 8),  # third hinge, v*pi/11520
    (0x0004, 'rot16k', 8),    # {off, n}; 20-byte keys: s16 dt, s16 ex,ey,ez (pi/32768), 12 bytes (tangents?)
    (0x0002, 'unk2', 8),
    (0x0001, 'ik', 8),        # {u16 sflags, u16 n, u32 off}: packed vector stream (IK effector) - see spec
]
# 0x20 = "extended" flag: every 8-byte stream block grows by 8, pos blocks by 12 (never seen in data).


def _body_layout(F):
    ext = 1 if F & 0x20 else 0
    out, off = [], 0
    for bit, kind, size in COMPONENTS:
        if not F & bit:
            continue
        if kind.startswith('pos'):
            sz = 24 + 12 * ext if bit == 0x8000 else 24 + 8 * ext
        elif kind in ('unk2', 'ik'):
            sz = 8
        else:
            sz = 8 + 8 * ext
        out.append((bit, kind, off))
        off += sz
    return out, off


# ----------------------------------------------------------------------------- stream readers
_TA = [4, 6, 8, 16, 3, 6, 6, 12]       # key size of part A by code (exe table 0x8338ac00)
_TB = [3, 6, 6, 12, 4, 6, 8, 16]       # key size of part B by code (0x8338ac10)


def _read_ik(d, base, sflags, n, off):
    """Variable-format (flag 0x0001) stream. Returns dict with key times and 4-vectors (and B part)."""
    p = base + off
    a_code = (sflags >> 3) & 7
    b_code = (sflags >> 6) & 7
    single = (sflags & 7) != 0
    hdr = 0 if (sflags & 0x38) == 0x18 else 16
    keys_at = p + hdr
    hb = None
    if not single:
        if (sflags & 0x1c0) != 0xc0:
            hb = keys_at
            keys_at += 12
    size = _TA[a_code] + (0 if single else _TB[b_code])
    stride = size + 1
    if hdr:
        off4 = [_half(x) for x in struct.unpack_from('>4H', d, p)]
        sc4 = [_half(x) for x in struct.unpack_from('>4H', d, p + 8)]
    t, times, va, vb = 0, [], [], []
    for i in range(n):
        k = keys_at + i * stride
        t += d[k]
        times.append(t)
        kd = d[k + 1:k + 1 + size]
        if a_code == 0:
            raw = list(kd[:4])
        elif a_code == 1:
            b0, b1, b2, b3, b4, b5 = kd[:6]
            raw = [(b0 << 4) | (b4 >> 4), (b1 << 4) | (b4 & 15), (b2 << 4) | (b5 >> 4), (b3 << 4) | (b5 & 15)]
        elif a_code == 2:
            raw = [(kd[j] << 8) | kd[j + 4] for j in range(4)]
        elif a_code == 3:
            raw = list(struct.unpack_from('>4f', kd, 0))
        else:
            raw = None
        if raw is None:
            va.append(None)
        elif a_code == 3:
            va.append(tuple(raw))
        else:
            va.append(tuple(o + s * r for o, s, r in zip(off4, sc4, raw)))
        if not single:
            vb.append(kd[_TA[a_code]:])          # raw B part (3-vector; layout not fully decoded)
    return {'mode': sflags & 7, 'sflags': sflags, 'times': times, 'vals': va,
            'b_raw': vb if not single else None,
            'b_hdr': [_half(x) for x in struct.unpack_from('>6H', d, hb)] if hb else None}


def _keys(d, base, off, n, fmt, dt_index, size, absolute=False):
    t, out = 0, []
    for i in range(n):
        k = struct.unpack_from(fmt, d, base + off + size * i)
        dt = k[dt_index]
        t = dt if absolute else t + dt
        out.append((t, k))
    return out


def parse_yanm(d, y=None):
    """Parse one YANM chunk. d = record bytes, y = offset of 'YANM'. Returns list of tracks."""
    if y is None:
        y = d.find(b'YANM')
    base = y + 8
    ver, mode, cnt, first = struct.unpack_from('>BBHI', d, base)
    tracks = []
    if mode == 0:                         # explicit descriptors
        o = base + first
        entries = []
        for i in range(cnt):
            F, sz, h = struct.unpack_from('>HHI', d, o)
            entries.append((h, F, o + 8, sz))
            o += sz
    else:                                 # templated: header u16 = template id, bodies packed
        tpl = TEMPLATES.get(cnt) or TEMPLATES.get(mode)
        if tpl is None:
            raise ValueError('unknown YANM template %d' % cnt)
        o = base + first
        entries = []
        for h, F in tpl:
            lay, size = _body_layout(F)
            entries.append((h, F, o, size + 8))
            o += size
    for h, F, body, sz in entries:
        tr = {'hash': h, 'name': NAMES.get(h, '#%08x' % h), 'flags': F, 'parts': {}}
        lay, _ = _body_layout(F)
        for bit, kind, off in lay:
            b = body + off
            if kind in ('rot8', 'rot8f', 'rot8ff'):
                so, n = struct.unpack_from('>II', d, b)
                sc = {'rot8': PI / 128, 'rot8f': PI / 4608, 'rot8ff': PI / 11520}[kind]
                tr['parts'][kind] = [(t, (k[0] * sc, k[1] * sc, k[2] * sc)) for t, k in _keys(d, base, so, n, '>bbbB', 3, 4)]
            elif kind == 'rot16':
                so, n = struct.unpack_from('>II', d, b)
                sc = PI / 32768
                tr['parts'][kind] = [(t, (k[0] * sc, k[1] * sc, k[2] * sc)) for t, k in _keys(d, base, so, n, '>hhhH', 3, 8)]
            elif kind == 'rot16k':
                so, n = struct.unpack_from('>II', d, b)
                sc = PI / 32768
                tr['parts'][kind] = [(t, (k[1] * sc, k[2] * sc, k[3] * sc)) for t, k in _keys(d, base, so, n, '>hhhh12x', 0, 20)]
            elif kind in ('hinge', 'hinge_f', 'hinge_ff'):
                so, n, ax = struct.unpack_from('>IHH', d, b)
                sc = {'hinge': PI / 128, 'hinge_f': PI / 4608, 'hinge_ff': PI / 11520}[kind]
                tr['parts'][kind] = {'axis': ax, 'keys': [(t, k[0] * sc) for t, k in _keys(d, base, so, n, '>bB', 1, 2)]}
            elif kind in ('pos8', 'pos16'):
                so, n, s, bx, by, bz = struct.unpack_from('>II4f', d, b)
                fmt, size = ('>bbbB', 4) if kind == 'pos8' else ('>hhhH', 8)
                acc, keys = [0, 0, 0], []
                for t, k in _keys(d, base, so, n, fmt, 3, size):
                    acc = [acc[0] + k[0], acc[1] + k[1], acc[2] + k[2]]
                    keys.append((t, (acc[0] * s + bx, acc[1] * s + by, acc[2] * s + bz)))
                tr['parts']['pos'] = keys
            elif kind == 'scale':
                so, n = struct.unpack_from('>II', d, b)
                tr['parts'][kind] = [(t, (k[0] / 256.0, k[1] / 256.0, k[2] / 256.0)) for t, k in
                                     _keys(d, base, so, n, '>hhhH', 3, 8, absolute=True)]
            elif kind == 'ik':
                sf, n, so = struct.unpack_from('>HHI', d, b)
                tr['parts'][kind] = _read_ik(d, base, sf, n, so)
            else:
                tr['parts'][kind] = d[b:b + 8]
        tracks.append(tr)
    return {'version': ver, 'templated': mode != 0, 'template': cnt if mode else None, 'tracks': tracks}


# ----------------------------------------------------------------------------- record (header + blocks)
CHUNKS = (b'YANM', b'POF0', b'YAT2', b'YATR', b'YOBJ', b'YMPC', b'POFF', b'DATA')


def parse_header(d):
    """Record header: u32 header_size (=0x50+0x28*n), u32 total, u32 0x20, u32 first block offset,
    +0x20 u16 kind, u16 1, 'root' ..., +0x48 u32 n blocks, +0x4c move name, entries (0x28 bytes) at 0x70."""
    hdr_size, total, info, first = struct.unpack_from('>4I', d, 0)
    n = struct.unpack_from('>I', d, 0x48)[0]
    h = {'header_size': hdr_size, 'total': total, 'first_block': first,
         'kind': struct.unpack_from('>H', d, 0x20)[0],
         'move': d[0x4c:0x5c].split(b'\0')[0].decode('latin-1'),
         'nblocks': n, 'blocks': []}
    for i in range(n):
        e = 0x70 + 0x28 * i
        flag, x = d[e], struct.unpack_from('>H', d, e + 2)[0]
        nm = d[e + 4:e + 12].decode('latin-1')
        rel = struct.unpack_from('>I', d, e + 0x24)[0]
        h['blocks'].append({'name': nm, 'flag': flag, 'value': x, 'offset': first + rel})
    return h


def parse_yat2(d, o):
    """YAT2 chunk = event list of one block.
    data: u16 version(1), u16 n, u32 entry_off(0xc), u32 payload_off; n entries of 8 bytes:
    (u16 frame, u16 payload offset (rel. payload_off), u16 command count, u16 0);
    command = {u16 id, u8 size (incl. this 4-byte head), u8 type, data[size-4]}."""
    size = struct.unpack_from('>I', d, o + 4)[0]
    b = o + 8
    ver, n, eoff, poff = struct.unpack_from('>HHII', d, b)
    events = []
    for i in range(n):
        frame, off, cnt, _ = struct.unpack_from('>HHHH', d, b + eoff + 8 * i)
        p = b + poff + off
        cmds = []
        for k in range(cnt):
            cid, sz, typ = struct.unpack_from('>HBB', d, p)
            cmds.append({'id': cid, 'type': typ, 'data': d[p + 4:p + max(sz, 4)].hex()})
            p += max(sz, 4)
        events.append({'frame': frame, 'commands': cmds})
    return {'version': ver, 'events': events}


def decode_record(d, **kw):
    """All blocks of a motion record -> list of decode() results (one per person / sub-block)."""
    h = parse_header(d)
    out = []
    offs = [b['offset'] for b in h['blocks']] or [h['first_block']]
    for i, o in enumerate(offs):
        if d[o:o + 4] != b'YANM':
            o = d.find(b'YANM', o)
        m = decode(d, y=o, **kw)
        m['block'] = h['blocks'][i]['name'] if h['blocks'] else '00000000'
        ysz = struct.unpack_from('>I', d, o + 4)[0]
        q = d.find(b'YAT2', o + 8 + ysz)
        nxt = offs[i + 1] if i + 1 < len(offs) else len(d)
        m['events'] = parse_yat2(d, q)['events'] if 0 <= q < nxt else []
        m['move'] = h['move']
        out.append(m)
    return out


# ----------------------------------------------------------------------------- sampling
def _find(keys, f):
    """keys: list of (time, value) with increasing times. -> (i, alpha) so value is between i and i+1."""
    if f <= keys[0][0] or len(keys) == 1:
        return 0, 0.0
    if f >= keys[-1][0]:
        return len(keys) - 1, 0.0
    lo, hi = 0, len(keys) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        if keys[mid][0] <= f:
            lo = mid
        else:
            hi = mid
    t0, t1 = keys[lo][0], keys[lo + 1][0]
    return lo, (f - t0) / float(t1 - t0) if t1 > t0 else 0.0


def _sample_euler(keys, f, cubic):
    i, a = _find(keys, f)
    q1 = q_euler(*keys[i][1])
    if a == 0.0:
        return q1
    q2 = q_euler(*keys[i + 1][1])
    if qdot(q1, q2) < 0:
        q2 = tuple(-c for c in q2)
    if not cubic:
        return qnorm(qslerp(q1, q2, a))
    q0 = q_euler(*keys[i - 1][1]) if i > 0 else q1
    q3 = q_euler(*keys[i + 2][1]) if i + 2 < len(keys) else q2
    if qdot(q0, q1) < 0:
        q0 = tuple(-c for c in q0)
    if qdot(q3, q2) < 0:
        q3 = tuple(-c for c in q3)
    if i == 0:
        A, B = q1, q2                    # game: cache filled with q1,q2 only for the first key
    else:
        A, B = _squad_inner(q0, q1, q2), _squad_inner(q1, q2, q3)
    return qnorm(qslerp(qslerp(q1, q2, a), qslerp(A, B, a), 2 * a * (1 - a)))


def _sample_scalar(keys, f, cubic):
    i, a = _find(keys, f)
    v1 = keys[i][1]
    if a == 0.0:
        return v1
    v2 = keys[i + 1][1]
    if not cubic:
        return v1 + (v2 - v1) * a
    v0 = keys[i - 1][1] if i > 0 else v1
    v3 = keys[i + 2][1] if i + 2 < len(keys) else v2
    return _spline4(v0, v1, v2, v3, a)


def _sample_vec(keys, f, cubic):
    i, a = _find(keys, f)
    v1 = keys[i][1]
    if a == 0.0:
        return tuple(v1)
    v2 = keys[i + 1][1]
    if not cubic:
        return tuple(x + (y - x) * a for x, y in zip(v1, v2))
    v0 = keys[i - 1][1] if i > 0 else v1
    v3 = keys[i + 2][1] if i + 2 < len(keys) else v2
    return tuple(_spline4(p0, p1, p2, p3, a) for p0, p1, p2, p3 in zip(v0, v1, v2, v3))


def sample_track(tr, f, cubic=False):
    """-> {'rot': quat or None, 'pos': xyz offset or None, 'scale': xyz or None, 'ik': (x,y,z,w) or None}"""
    P = tr['parts']
    rot = None
    if 'rot16k' in P:
        rot = _sample_euler(P['rot16k'], f, cubic)
    elif 'rot16' in P:
        rot = _sample_euler(P['rot16'], f, cubic)
    elif 'rot8' in P:
        rot = _sample_euler(P['rot8'], f, cubic)
        for k in ('rot8f', 'rot8ff'):
            if k in P:
                rot = qmul(rot, _sample_euler(P[k], f, cubic))
    elif 'hinge' in P:
        h = P['hinge']
        rot = qaxis(h['axis'], _sample_scalar(h['keys'], f, cubic))
        for k in ('hinge_f', 'hinge_ff'):
            if k in P:
                h = P[k]
                rot = qmul(rot, qaxis(h['axis'], _sample_scalar(h['keys'], f, cubic)))
    pos = _sample_vec(P['pos'], f, cubic) if 'pos' in P else None
    scale = _sample_vec(P['scale'], f, False) if 'scale' in P else None
    ik = None
    if 'ik' in P and P['ik']['vals'] and P['ik']['vals'][0] is not None:
        I = P['ik']
        ik = _sample_vec(list(zip(I['times'], I['vals'])), f, cubic)
        if ik[3] != ik[3]:
            ik = ik[:3] + (0.0,)
    return {'rot': rot, 'pos': pos, 'scale': scale, 'ik': ik}


def track_length(tr):
    L = 0
    for k, v in tr['parts'].items():
        if isinstance(v, list) and v:
            L = max(L, v[-1][0])
        elif isinstance(v, dict) and 'keys' in v and v['keys']:
            L = max(L, v['keys'][-1][0])
        elif isinstance(v, dict) and v.get('times'):
            L = max(L, v['times'][-1])
    return L


# ----------------------------------------------------------------------------- top level
def decode(d, y=None, cubic=False, sample=True):
    """Decode one YANM block of a record.
    Returns {'frames': N, 'fps': 60, 'bones': {name: {'rot': [q per frame] | None, 'pos': [xyz] | None,
             'ik': [(x,y,z,w)] | None, 'flags': F}}, 'tracks': {name: raw track}, ...}.
    'rot' = absolute local quaternion (x,y,z,w); 'pos' = translation OFFSET (add to bind t);
    'ik' = IK effector (model-space ankle target xyz in cm, w = knee angle in radians); resolve with pose()."""
    Y = parse_yanm(d, y)
    tracks = {t['name']: t for t in Y['tracks']}
    frames = max([track_length(t) for t in Y['tracks']] + [0])
    out = {'frames': frames + 1, 'length': frames, 'fps': FPS, 'templated': Y['templated'],
           'template': Y['template'], 'tracks': tracks, 'bones': {}}
    if sample:
        for name, tr in tracks.items():
            s = [sample_track(tr, f, cubic) for f in range(frames + 1)]
            out['bones'][name] = {'flags': tr['flags'],
                                  'rot': [x['rot'] for x in s] if s and s[0]['rot'] is not None else None,
                                  'pos': [x['pos'] for x in s] if s and s[0]['pos'] is not None else None,
                                  'scale': [x['scale'] for x in s] if s and s[0]['scale'] is not None else None,
                                  'ik': [x['ik'] for x in s] if s and s[0]['ik'] is not None else None}
    return out


def evaluate(m, f, cubic=False):
    """Evaluate every track of a decode() result at an arbitrary (fractional) frame."""
    return {n: sample_track(t, f, cubic) for n, t in m['tracks'].items()}


# ----------------------------------------------------------------------------- pose (FK + IK)
def _mmul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(4)) for j in range(4)] for i in range(4)]


def _trs(t, q):
    r = q_to_mat(q)
    return [[r[0][0], r[0][1], r[0][2], t[0]], [r[1][0], r[1][1], r[1][2], t[1]],
            [r[2][0], r[2][1], r[2][2], t[2]], [0, 0, 0, 1]]


def _norm3(v):
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return [c / n for c in v]


def _cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def pose(m, nodes, f, cubic=False, ik=True):
    """Full pose at frame f against a WWE'13 node table (list of {'name','t','r','parent'}).
    Returns (local, world): local = {name: (t, q)} and world = list of 4x4 matrices (model space, cm, -Y up)."""
    ev = evaluate(m, f, cubic)
    idx = {n['name']: i for i, n in enumerate(nodes)}
    loc = []
    for n in nodes:
        e = ev.get(n['name'])
        t = list(n['t'])
        q = q_euler(*n['r'])
        if e:
            if e['rot'] is not None:
                q = e['rot']
            if e['pos'] is not None:
                t = [a + b for a, b in zip(t, e['pos'])]
        loc.append([t, q])
    W = [None] * len(nodes)

    def world(i):
        if W[i] is None:
            M = _trs(*loc[i])
            p = nodes[i]['parent']
            W[i] = M if p < 0 else _mmul(world(p), M)
        return W[i]

    if ik:
        for n in nodes:
            e = ev.get(n['name'])
            if not e or e['ik'] is None or e['rot'] is not None:
                continue
            tr = m['tracks'][n['name']]
            mode = tr['parts']['ik']['mode']
            if mode not in (1, 2):
                continue
            calf = idx[n['name']]
            side = n['name'].split()[1]                  # 'L' / 'R'
            thigh = idx.get('Bip %s Thigh' % side)
            foot = idx.get('Bip %s Foot' % side)
            if thigh is None or foot is None:
                continue
            _solve_ik(nodes, loc, W, world, thigh, calf, foot, e['ik'], mode)
    for i in range(len(nodes)):
        world(i)
    return {nodes[i]['name']: (tuple(loc[i][0]), tuple(loc[i][1])) for i in range(len(nodes))}, W


def _solve_ik(nodes, loc, W, world, thigh, calf, foot, target, mode):
    """Two-bone IK as in game routine 0x82384b78 (mode 1 -> knee about local Y, mode 2 -> about Z)."""
    def pos(M):
        return [M[0][3], M[1][3], M[2][3]]
    Wt = world(thigh)
    hip = pos(Wt)
    # bone lengths from the bind skeleton: thigh->calf, calf->foot
    def chain_len(a, b):
        L, i = [0, 0, 0], b
        tot = 0.0
        while i != a and i >= 0:
            tot += math.sqrt(sum(c * c for c in nodes[i]['t']))
            i = nodes[i]['parent']
        return tot
    L1, L2 = chain_len(thigh, calf), chain_len(calf, foot)
    ax = 1 if mode == 1 else 2
    u = [Wt[0][ax], Wt[1][ax], Wt[2][ax]]               # knee axis of the FK thigh, world space
    tx, ty, tz, w = target
    dv = [tx - hip[0], ty - hip[1], tz - hip[2]]
    dist = math.sqrt(sum(c * c for c in dv)) or 1e-6
    d = [c / dist for c in dv]
    zc = _norm3(_cross(d, u))
    yc = _norm3(_cross(zc, d))
    # target length blend for nearly straight legs (w < 30 deg)
    dw = math.sqrt((L1 + L2 * math.cos(w)) ** 2 + (L2 * math.sin(w)) ** 2)
    k = max(0.0, min(1.0, (w - PI / 6) * -5.729578))
    de = dist + (dw - dist) * k
    ck = max(-1.0, min(1.0, (de * de - L1 * L1 - L2 * L2) / (2 * L1 * L2)))
    knee = math.acos(ck)
    a = math.atan2(-L2 * math.sin(knee), L1 + L2 * ck)
    if mode == 1:
        # frame columns: x = d, y = yc, z = zc ; thigh = frame * Ry(a), calf = Ry(knee)
        F = [[d[0], yc[0], zc[0]], [d[1], yc[1], zc[1]], [d[2], yc[2], zc[2]]]
        Rl = q_to_mat(qaxis(1, a))
        calf_q = qaxis(1, knee)
    else:
        # mode 2 (knee about Z) never occurs in the shipped data; this branch is a best guess.
        zc2 = _norm3([u[0] - d[0] * sum(a_ * b_ for a_, b_ in zip(u, d)),
                      u[1] - d[1] * sum(a_ * b_ for a_, b_ in zip(u, d)),
                      u[2] - d[2] * sum(a_ * b_ for a_, b_ in zip(u, d))])
        yc2 = _norm3(_cross(zc2, d))
        F = [[d[0], yc2[0], zc2[0]], [d[1], yc2[1], zc2[1]], [d[2], yc2[2], zc2[2]]]
        Rl = q_to_mat(qaxis(2, -a))
        calf_q = qaxis(2, -knee)
    Rw = [[sum(F[i][k_] * Rl[k_][j] for k_ in range(3)) for j in range(3)] for i in range(3)]
    p = nodes[thigh]['parent']
    Pw = world(p)
    Pr = [[Pw[i][j] for j in range(3)] for i in range(3)]
    # local = Pr^T * Rw
    Lr = [[sum(Pr[k_][i] * Rw[k_][j] for k_ in range(3)) for j in range(3)] for i in range(3)]
    loc[thigh][1] = mat_to_q(Lr)
    loc[calf][1] = calf_q
    # invalidate cached world matrices below the thigh
    for i in range(len(nodes)):
        j = i
        while j >= 0:
            if j == thigh:
                W[i] = None
                break
            j = nodes[j]['parent']
