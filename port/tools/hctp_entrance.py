"""SmackDown! Here Comes the Pain (PS2, "HCTP") entrance -> SvR 2011 entrance.

Format notes and the conversion rules: docs/HCTP_ENTRANCES.md. First use: Goldberg (HCTP 3047 ->
2011 503).

  python hctp_entrance.py list EVTENT.PAC [MOVES.PAC]
        every HCTP entrance number, its objects and (with MOVES.PAC) its name
  python hctp_entrance.py convert EVTENT.PAC HCTP_NUMBER NUMBER OUT_DIR [--name TEXT] [--fov RAD]
        [--no-ground] [--no-parts] [--pack]
        writes OUT_DIR/<NUMBER>_evp.bin + <NUMBER>_evt.bin (2011 EVP script + YMKs tracks),
        OUT_DIR/parts/<10000+1000k+NUMBER>_evp/evt.bin (Create-An-Entrance parts, k = 0..4),
        OUT_DIR/report.txt; --pack also writes a move pack folder OUT_DIR/pack (pack.txt with
        `pacentry evt\\NyujyoN.pac EVPx/EVTx <n>` lines + entrance/*.bin) that a superstar mod
        carries in moves/ or that goes in Mods/Moves/<pack>.

HCTP side (all little-endian, PS2):
  EVTENT.PAC = DPAC: group EVET (one CEKey motion bank per entrance, named by the number 3000..3533)
  and group ENTR: SEVP (the scripts of all entrances, 'POF0'/'VE' chunk) and SYDT.
  SEVP: 0x10 u32 count, 0x14 ptr -> table; table {u16 number, u16 objects, u32 0, ptr objs};
        object 28 bytes {u16 idx, u16 model, type[8], u32 -1, u32 0, u16 slots (5), u16 0, ptr};
        slot 40 bytes {u16 slot, u8 on, u8 0, u32 track id, u32 track x, u32 track y,
                       f32 x, f32 y, f32 z, f32 yaw, f32 ?, u16 parent (9999 none), u16 0}.
        Pointers are relative to offset 8. 5 slots = the 5 entrance segments.
  EVET bank: key = (track id, x, y) from the slots, e.g. 3047: id 9950, x = object, y = segment;
        a camera's tracks carry id | 0x4000 (26334).
2011 side: docs/SVR08_ENTRANCES.md (EVP 32-byte objects, 21 x 212-byte slots, segments at slots
  0/5/10/15/20; EVT = YMKs, key (25000 + object, x = slot, y = sub-track)).
"""
import math
import os
import struct
import sys
import collections

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ymks as Y  # noqa: E402
import hctp_motion as H  # noqa: E402
import hand_tracks as HT  # noqa: E402
import svr08_entrance as S  # noqa: E402

CAM_FLAG = 0x4000               # HCTP camera tracks: track id | 0x4000 (3047: 9950 -> 26334)
SEGMENTS = 5                     # HCTP slots 0..4 -> 2011 slots 0, 5, 10, 15, 20
# SmackDown! vs. Raw (2004, PS2): HCTP's files and formats, but 20-24 sub-scenes per object; by position
# 0-3 hidden intro, 4-8 stage, 9-14 ramp, 15-18 ringside, 19-23 in the ring = 2011's 5 segments (no
# intro cut needed); camera keys of 19 bytes (f32 position, s16 rotation). docs/OTHER_GAMES_ENTRANCES.md
GAMES = {'hctp': dict(segments=SEGMENTS, groups=None),
         'svr04': dict(segments=24, groups=[(0, 4), (4, 9), (9, 15), (15, 19), (19, 24)])}
SLOTS_11 = 21
HSLOT = 40
CAM_FOV = 0.7217285633087158     # key fov; with op 112 in the track 2011 uses the zoom instead (section 5)

# HCTP arenas: the stage is level with the ring (character root y -9.3 = standing on y 0) and a
# ramp falls to the arena floor (y +11.8, root y +2.5) between z 237 and z 87. SvR 2011's own
# entrances stand on a flat floor-level stage and ramp (root y +2.5 from z 300 down to the ring).
# A root on HCTP's stage/ramp is moved down by (FLOOR - ground(z)); cameras follow their subject.
FLOOR = 11.8
RAMP_TOP_Z, RAMP_FOOT_Z = 237.0, 87.0

# HCTP event ops: sizes equal in both games; meanings researched in both executables
# (HCTP_ENTRANCES.md section 4). Kept where 2011 does the same thing with the same bytes.
KEEP_CHAR = {18, 98, 102, 103}   # 18 screen fade/flash [r g b a ticks]; 98 ring shake (2011: no-op);
                                 # 102/103 crowd camera flash bulbs
KEEP_CAM = {112, 113}            # 112 camera zoom [z/10][rate/1000]; 113 height follows cast c-1
KEEP_PROP = {182}                # [182] owner motion state 1 (2011 props: b6 at frame 0)
DROP_WHY = {16: 'HCTP face/hand animation id, no 2011 handler',
            118: 'HCTP crowd lighting, no 2011 handler',
            192: 'HCTP sound effect id; 2011 ignores op 192'}
# HCTP entrance props -> 2011 evtobj ids. HCTP 2019 is the WWE (Undisputed) Title folded in the hand
# (HCTP BG.PAC SOBJ/2019 textures); 2011's 2019 is the World Heavyweight Title - Folded.
PROP_MAP = {2019: 2014}          # 2011 2014 = "WWE Title - Folded"



def _u16(d, o):
    return struct.unpack_from('<H', d, o)[0]


def _u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


# ============================================================================ HCTP containers

def dpac(d):
    """HCTP DPAC -> [(group, name, bytes)] (groups from 0x800, data from 0x4000 in 0x800 sectors)"""
    assert d[:4] == b'DPAC', 'not an HCTP DPAC'
    out = []
    off = 0x800
    while off < 0x4000 and d[off:off + 4] != bytes(4):
        tag = d[off:off + 4].decode('latin-1').strip()
        n, start = struct.unpack_from('<HH', d, off + 4)
        if n & 0x8000:
            n &= 0x7FFF
            pos = 0x4000 + start * 0x800
            for i in range(n):
                id_, sz = struct.unpack_from('<HH', d, off + 8 + 4 * i)
                out.append((tag, '%04x' % id_, d[pos:pos + sz * 0x100]))
                pos += (sz * 0x100 + 0x7FF) & ~0x7FF
            off += 8 + 4 * n
        else:
            cnt = 0
            for i in range(n):
                p = off + 8 + 8 * i
                if d[p:p + 4] == bytes(4):
                    break
                cnt += 1
                nm = d[p:p + 4].replace(b'\0', b'').decode('latin-1').strip()
                sec, sz = struct.unpack_from('<HH', d, p + 4)
                o = 0x4000 + sec * 0x800
                out.append((tag, nm, d[o:o + sz * 0x100]))
            off += 8 + 8 * cnt
    return out


def pac(d):
    """HCTP inner 'PAC ' -> [(id, bytes)]"""
    if d[:4] != b'PAC ':
        return []
    n = _u32(d, 4)
    base = 8 + 8 * n
    out = []
    for i in range(n):
        e = d[8 + 8 * i:16 + 8 * i]
        o = int.from_bytes(e[2:5], 'little')
        s = int.from_bytes(e[5:8], 'little')
        out.append((_u16(e, 0), d[base + o:base + o + s]))
    return out


def read_evtent(path):
    ents = dpac(open(path, 'rb').read())
    banks = {int(n): b for g, n, b in ents if g == 'EVET' and n.isdigit()}
    entr = {n: b for g, n, b in ents if g == 'ENTR'}
    return dict(banks=banks, sevp=entr['SEVP'], sydt=entr.get('SYDT'))


def entrance_names(moves_pac):
    """MOVES.PAC group MOVE entry LIST, sub-entry 2: u16 count, count x {u16 number, char name[30]}"""
    names = {}
    for g, n, b in dpac(open(moves_pac, 'rb').read()):
        if g == 'MOVE' and n == 'LIST':
            for i, sub in pac(b):
                if i != 2:
                    continue
                cnt = _u16(sub, 0)
                for k in range(cnt):
                    r = sub[2 + 32 * k:2 + 32 * (k + 1)]
                    names[_u16(r, 0)] = r[2:32].split(b'\0')[0].decode('latin-1')
    return names


def parse_sevp(d):
    """-> {number: dict(num, objs=[dict(idx, model, t0, t1, w4, w5, nslots, lo, slots=[dict])])}"""
    assert d[:4] == b'POF0' and d[8:10] == b'VE', 'not an HCTP SEVP'
    b = 8
    n = _u32(d, 0x10)
    t = _u32(d, 0x14) + b
    out = {}
    for i in range(n):
        num, nobj, _z, op = struct.unpack_from('<HHII', d, t + 12 * i)
        objs = []
        for j in range(nobj):
            q = op + b + 28 * j
            idx, model = struct.unpack_from('<HH', d, q)
            t0, t1 = struct.unpack_from('>II', d, q + 4)      # type bytes read in 2011's order
            w4, w5, ns, lo, sp = struct.unpack_from('<IIHHI', d, q + 12)
            slots = []
            for k in range(ns):
                s = d[sp + b + HSLOT * k:sp + b + HSLOT * (k + 1)]
                slot, on, _ = struct.unpack_from('<HBB', s, 0)
                tid, tx, ty = struct.unpack_from('<III', s, 4)
                params = struct.unpack_from('<5f', s, 16)
                parent = _u16(s, 36)
                slots.append(dict(slot=slot, on=on, tid=tid, tx=tx, ty=ty, params=params, parent=parent))
            objs.append(dict(idx=idx, model=model, t0=t0, t1=t1, w4=w4, w5=w5, nslots=ns, lo=lo, slots=slots))
        out[num] = dict(num=num, objs=objs)
    return out


def kind_of(o):
    typ = (o['t0'], o['t1'])
    if typ == S.TYPE_CHAR:
        return 'char'
    if typ == S.TYPE_PROP:
        return 'prop'
    if typ == (0x00010000, 0):
        return 'camera'
    if typ in S.TYPE_LIGHTS:
        return 'rope'
    if typ == S.TYPE_GLOBAL:
        return 'global'
    return 'other'


# ============================================================================ ground fix

def ground(z):
    """HCTP floor height (y down) under z: 0 on the stage, FLOOR at the foot of the ramp"""
    if z >= RAMP_TOP_Z:
        return 0.0
    if z <= RAMP_FOOT_Z:
        return FLOOR
    return FLOOR * (RAMP_TOP_Z - z) / (RAMP_TOP_Z - RAMP_FOOT_Z)


def shift_at(z):
    """root y change (model units) that puts an HCTP stage/ramp root on 2011's floor-level stage"""
    return FLOOR - ground(z)


# ============================================================================ tracks

def char_keys_shift(data, origin=None):
    """[(frame, y shift)] at every key of an HCTP character track that carries a root (origin: _add_origin's)"""
    out = []
    for t, f, root, bones, op in H.keys(data, wide=True):
        if root is not None:
            out.append((f, shift_at(_add_origin(root, origin)[2] * 0.01)))
    return out


def shift_for_frame(keys, f):
    v = keys[0][1] if keys else 0.0
    for kf, s in keys:
        if kf > f:
            break
        v = s
    return v


def _add_origin(root, origin):
    """root in its slot's frame -> the scene's: origin = (x, y, z root units, pitch rad) - HCTP's slot param 4 tilts
    a segment down the ramp about x (3411 segment 2: 0.08 = 11.8 down over 150)"""
    if not origin:
        return root
    x, y, z = root
    a = origin[3]
    if a:
        y, z = y * math.cos(a) - z * math.sin(a), y * math.sin(a) + z * math.cos(a)
    return (x + origin[0], y + origin[1], z + origin[2])


def conv_char_pose(pose, ground_fix, rep, origin=None):
    d = H.decode_body(pose, H.H_TYPES_WIDE)
    if d['trail']:
        raise ValueError('trailing bytes in a character pose')
    subs = []
    for s in d['subs']:
        op = s['op']
        root = s.get('root')
        if root is not None and op != 0x7b:
            root = _add_origin(root, origin)
        if root is not None and ground_fix and op != 0x7b:
            dy = shift_at(root[2] * 0.01) / 0.01
            if dy:
                rep['character roots moved down to the 2011 stage/ramp'] += 1
            root = (root[0], root[1] + dy, root[2])
        if op in (0x7d, 0x7c) or 0x60 <= op <= 0x6f:
            n = dict(op=0x7d, count=s['count'], root=tuple(float(v) for v in root), bones=s['bones'])
        elif op == 0x7a or 0x50 <= op <= 0x5f:
            n = dict(op=0x50 + s['count'] if s['count'] <= 15 else 0x7a, count=s['count'],
                     root=tuple(float(v) for v in root), mask=s['mask'], bones=s['bones'])
        elif op == 0x7b:
            n = dict(op=0x7b, ref=s['ref'], count=s['count'], root=tuple(s['trans']))
        elif op == 0x7e:
            n = dict(op=0x7e, count=s['count'], root=tuple(int(round(v)) for v in root))
        elif op == 0x7f:
            n = dict(op=0x7f, count=s['count'], root=tuple(float(v) for v in root))
        else:
            n = dict(op=op, count=s['count'], bones=s['bones'])
        subs.append(n)
    return Y.encode_body({'subs': subs, 'trail': b''}, Y.TYPES_M1)


def conv_obj_pose(pose, origin=None):
    """origin: the slot origin baked into the root (_add_origin; a vehicle with its own model)"""
    d = H.decode_obj(pose)
    if d['trail']:
        raise ValueError('trailing bytes in an object pose')
    out = bytearray()
    for s in d['subs']:
        op = s['op']
        if op == 0x7a:
            m = s['mask']
            out += bytes([0x75, s['count']]) + struct.pack('<3h', *_s16root(s['root'], origin)) + bytes([m & 0xff, m >> 8 & 0xff, m >> 16 & 0xff])
            for k in range(21):
                if m >> k & 1:
                    b = s['bones'][k]
                    out += struct.pack('<3h', b['x'], b['y'], b['z'])
        elif op in (0x7d, 0x7e):
            out += bytes([0x7e, s['count']]) + struct.pack('<3h', *_s16root(s['root'], origin))
        elif op == 0x7b:
            out += bytes([0x7b, s['ref'], s['count']]) + struct.pack('<3h', *s['trans'])
        else:
            b = s['bones'][op]
            out += bytes([op, s['count']]) + struct.pack('<3h', b['x'], b['y'], b['z'])
    return bytes(out)




# HCTP's motorbike (BG.PAC SOBJ 2035, Undertaker 3003, also 3203 / 3411 / 3511) is 2011's own bike 2540 (518 / 519
# Chuck Palumbo; 2011's 2035 is a desk). Its 13 nodes onto 2540's 7 keyed ones (2011: 0 the body's turn, 1 and 4 the
# steering, 2 and 3 the wheels, 5 and 6 still): HCTP node 1 and 4 spin about x (the wheels), 2 turns about y (the
# steering), 3 holds one angle (left out), 0 the body - on a world track (no parent) it stands in a frame turned 180
# degrees (x and z near 180): 2011's heading = 180 - y (checked on 3003 segment 3: path toward -x / +z = 135 degrees,
# y about 45). In a segment where the bike is attached to the rider (parent set) its pose stays as HCTP keeps it.
VEHICLES = {2035: dict(model=2540, body=0, wheels={1: 2, 4: 3}, steer=(2, (1, 4)), nodes=7)}


def _wrap16(v):
    v &= 0xFFFF
    return v - 65536 if v >= 32768 else v


def _s16root(root, origin):
    return tuple(max(-32768, min(32767, int(round(v)))) for v in _add_origin(root, origin))


def conv_vehicle_pose(pose, veh, attached, origin=None):
    """an HCTP bike pose -> 2011 2540's (VEHICLES): node map, the body's turn on world tracks; origin: the slot
    origin (root units) baked into a world track's root"""
    d = H.decode_obj(pose)
    out = bytearray()
    for sub in d['subs']:
        op = sub['op']
        if op == 0x7a:
            b = sub['bones']
            nodes = {}
            if veh['body'] in b:
                x, y, z = b[veh['body']]['x'], b[veh['body']]['y'], b[veh['body']]['z']
                if not attached and abs(_wrap16(x - 32768)) < 4096 and abs(_wrap16(z - 32768)) < 4096:
                    x, y, z = _wrap16(x - 32768), _wrap16(32768 - y), _wrap16(z - 32768)
                nodes[0] = (x, y, z)
            for hk, k in veh['wheels'].items():
                if hk in b:
                    nodes[k] = (b[hk]['x'], 0, 0)
            sk, ks = veh['steer']
            if sk in b:
                for k in ks:
                    nodes[k] = (0, b[sk]['y'], 0)
            m = sum(1 << k for k in nodes)
            out += bytes([0x75, sub['count']]) + struct.pack('<3h', *_s16root(sub['root'], origin)) + bytes([m & 0xff, m >> 8 & 0xff, m >> 16 & 0xff])
            for k in sorted(nodes):
                out += struct.pack('<3h', *nodes[k])
        elif op in (0x7d, 0x7e):
            out += bytes([0x7e, sub['count']]) + struct.pack('<3h', *_s16root(sub['root'], origin))
        elif op == 0x7b:
            out += bytes([0x7b, sub['ref'], sub['count']]) + struct.pack('<3h', *sub['trans'])
        else:   # (one node: through the map, else left out)
            k = next(iter(sub['bones']))
            v = sub['bones'][k]
            if k in veh['wheels']:
                out += bytes([veh['wheels'][k], sub['count']]) + struct.pack('<3h', v['x'], 0, 0)
            elif k == veh['steer'][0]:
                for kk in veh['steer'][1]:
                    out += bytes([kk, sub['count']]) + struct.pack('<3h', 0, v['y'], 0)
    return bytes(out)


# 2011 has no vehicle attached to its rider (the bike stayed out of sight in game), so an HCTP segment where the bike
# is the rider's child is baked onto a world track from the rider's: 2011's bike sits at the rider's root plus a
# fixed offset turned by the rider's heading (bone 0 y), its heading the rider's plus a little - measured on the 2K14
# Undertaker (210) riding 2540 in 2011: offset (10, -790, -32) root units rider - bike in the bike's frame, heading
# +150 s16. The bike's own wheels and steering from its HCTP track.
RIDE_OFFSET = (10.0, -790.0, -32.0)
RIDE_TURN = 150
# 2011's bike root y with its wheels on the floor (2K14 210 / HCTP world tracks: 1217-1218); 2011's stage and ramp
# are floor level, so a baked root within BIKE_SNAP of it is put on it (HCTP 3411's rider sits 1.3 lower)
BIKE_FLOOR, BIKE_SNAP = 1217, 300


def _bike_nodes_at(bike_recs):
    """{frame: {node: (x, y, z)}} of a converted bike track (2011 0x75 / single-node poses), held between keys"""
    out, cur = {}, {}
    for f, r in bike_recs:
        _, pose = Y.split_record(r)
        p = 0
        while p < len(pose) - 1:
            op = pose[p]
            if op == 0x75:
                m = pose[p + 8] | pose[p + 9] << 8 | pose[p + 10] << 16
                q = p + 11
                for k in range(24):
                    if m >> k & 1:
                        cur[k] = struct.unpack_from('<3h', pose, q); q += 6
                p = q
            elif op in (0x7e,):
                p += 8
            elif op == 0x7b:
                p += 9
            elif op < 21:
                cur[op] = struct.unpack_from('<3h', pose, p + 2); p += 8
            else:
                break
        out[f] = dict(cur)
    return out


def bake_vehicle(rider_recs, bike_recs, nframes, veh):
    """an attached HCTP bike segment -> 2011 world-track records, one key a frame (from the rider's keys)"""
    import math
    data = Y.build_stream(Y.make_stream(list(rider_recs), nframes))
    ks = [k for k in Y.body_keys(data, S.T11) if k['has_pose']]
    if not ks:
        return []
    nodes_at = _bike_nodes_at(bike_recs)
    frames = sorted(nodes_at)
    out = []
    ki = 0
    last_nodes = {}
    for f in range(nframes):
        while ki + 1 < len(ks) and ks[ki + 1]['frame'] <= f:
            ki += 1
        k = ks[ki]
        rx, ry, rz = k['root']
        h16 = (k['bones'].get(0) or {}).get('y', 0)
        h = h16 * Y.ANG
        lx, ly, lz = RIDE_OFFSET
        dx = math.cos(h) * lx + math.sin(h) * lz
        dz = -math.sin(h) * lx + math.cos(h) * lz
        by = ry - ly
        if abs(by - BIKE_FLOOR) < BIKE_SNAP:
            by = BIKE_FLOOR
        root = tuple(max(-32768, min(32767, int(round(v)))) for v in (rx - dx, by, rz - dz))
        while frames and frames[0] <= f:
            last_nodes = nodes_at[frames.pop(0)]
        nodes = {kk: (0, 0, 0) for kk in range(veh['nodes'])}
        for kk in list(veh['wheels'].values()) + list(veh['steer'][1]):
            if kk in last_nodes:
                nodes[kk] = last_nodes[kk]
        nodes[veh['body']] = (0, _wrap16(h16 + RIDE_TURN), 0)
        m = sum(1 << kk for kk in nodes)
        pose = bytes([0x75, 0]) + struct.pack('<3h', *root) + bytes([m & 0xff, m >> 8 & 0xff, m >> 16 & 0xff])
        for kk in sorted(nodes):
            pose += struct.pack('<3h', *nodes[kk])
        out.append((f, Y.join_record(b'', pose)))
    return out


def _euler_m(e):
    """s16 x / y / z -> rotation (row vectors: v' = v @ R), the YOBJ's order (yobj.euler: X, then Y, then Z)"""
    rx, ry, rz = (v * Y.ANG for v in e)
    cx, sx, cy, sy, cz, sz = math.cos(rx), math.sin(rx), math.cos(ry), math.sin(ry), math.cos(rz), math.sin(rz)
    X = [[1, 0, 0], [0, cx, sx], [0, -sx, cx]]
    Yr = [[cy, 0, -sy], [0, 1, 0], [sy, 0, cy]]
    Z = [[cz, sz, 0], [-sz, cz, 0], [0, 0, 1]]
    mul = lambda a, b: [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]
    return mul(mul(X, Yr), Z)


def _m_euler(R):
    """_euler_m's inverse -> s16 x / y / z"""
    ry = math.asin(max(-1.0, min(1.0, -R[0][2])))
    rx = math.atan2(R[1][2], R[2][2])
    rz = math.atan2(R[0][1], R[0][0])
    return tuple(_wrap16(int(round(a / Y.ANG))) for a in (rx, ry, rz))


def _obj_poses(recs):
    """{frame: (root, {node: (x, y, z)})} of converted object records (0x75 / 0x7e / one-node keys), held"""
    out, root, cur = {}, (0, 0, 0), {}
    for f, r in recs:
        _, pose = Y.split_record(r)
        p = 0
        while p < len(pose) - 1:
            op = pose[p]
            if op == 0x75:
                root = struct.unpack_from('<3h', pose, p + 2)
                m = pose[p + 8] | pose[p + 9] << 8 | pose[p + 10] << 16
                q = p + 11
                for k in range(24):
                    if m >> k & 1:
                        cur[k] = struct.unpack_from('<3h', pose, q); q += 6
                p = q
            elif op == 0x7e:
                root = struct.unpack_from('<3h', pose, p + 2); p += 8
            elif op == 0x7b:
                p += 9
            elif op < 21:
                cur[op] = struct.unpack_from('<3h', pose, p + 2); p += 8
            else:
                break
        out[f] = (root, dict(cur))
    return out


OWN_SEAT = (17.0, -804.0, 680.0)   # (the rider on HCTP's bike, root units in its frame: 3003 segment 3's first frame)


def seat_of(rider_recs, bike_recs):
    """where the rider sits on his own bike, from a world-track segment's first frame (both in world space): his root
    in the bike's frame (root units) - None when either has no key there. The bike's node 0 is HCTP's (x, y, z) near
    (180, y, 180), the same turn as a heading of 180 - y."""
    data = Y.build_stream(Y.make_stream(list(rider_recs), max(f for f, _ in rider_recs) + 1))
    ks = [k for k in Y.body_keys(data, S.T11) if k['has_pose']]
    poses = _obj_poses(bike_recs)
    if not ks or not poses:
        return None
    root, nodes = poses[min(poses)]
    a = _yaw_of(nodes.get(0, (0, 0, 0)))
    d = [ks[0]['root'][j] - root[j] for j in range(3)]
    c, s = math.cos(a), math.sin(a)
    return (d[0] * c - d[2] * s, d[1], d[0] * s + d[2] * c)        # (d @ R_y(a)^T: into the bike's frame)


def _yaw_of(e):
    """a node-0 turn (s16 x / y / z) -> its heading (radians) about y: (180, y, 180) and (0, 180 - y, 0) alike"""
    R = _euler_m(e)
    return math.atan2(-R[0][2], R[2][2]) if abs(R[1][1]) > 0.5 and R[1][1] > 0 else math.atan2(R[0][2], -R[2][2])


def bake_own(rider_recs, bike_recs, nframes, seat):
    """an HCTP bike attached to its rider, with its own model (hctp_bike), onto a world track: it heads where the rider
    goes (his path; standing still, his bone 0's turn fitted to his path in this segment) with its wheels level, and
    he sits on it where he sits in its world segments (seat_of); its wheels, steering and stand from its HCTP track"""
    data = Y.build_stream(Y.make_stream(list(rider_recs), nframes))
    ks = [k for k in Y.body_keys(data, S.T11) if k['has_pose']]
    if not ks or seat is None:
        return []
    at = []
    ki = 0
    for f in range(nframes):
        while ki + 1 < len(ks) and ks[ki + 1]['frame'] <= f:
            ki += 1
        at.append(ks[ki])
    pos = [k['root'] for k in at]
    path = [None] * nframes                       # (heading from the path over +-6 frames, when he moves)
    for f in range(nframes):
        a, b = max(0, f - 6), min(nframes - 1, f + 6)
        dx, dz = pos[b][0] - pos[a][0], pos[b][2] - pos[a][2]
        if math.hypot(dx, dz) > 60:
            path[f] = math.atan2(dx, dz)
    b0y = [((k['bones'].get(0) or {}).get('y', 0)) * Y.ANG for k in at]
    diffs = sorted(((p - y + math.pi) % (2 * math.pi)) - math.pi for p, y in zip(path, b0y) if p is not None)
    turn = diffs[len(diffs) // 2] if diffs else 0.0
    poses = _obj_poses(bike_recs)
    frames = sorted(poses)
    out, last = [], ((0, 0, 0), {})
    for f in range(nframes):
        while frames and frames[0] <= f:
            last = poses[frames.pop(0)]
        # (the bike's own front is its -z, as 2011's 2540: heading the way he goes = his path turned 180 degrees - with
        # the path itself it rode backward, the rider on its seat facing the tail)
        a = (path[f] if path[f] is not None else b0y[f] + turn) + math.pi
        c, s = math.cos(a), math.sin(a)
        off = (seat[0] * c + seat[2] * s, seat[1], -seat[0] * s + seat[2] * c)  # (seat @ R_y(a): back to world)
        p = [pos[f][j] - off[j] for j in range(3)]
        if abs(p[1] - BIKE_FLOOR) < BIKE_SNAP:
            p[1] = BIKE_FLOOR
        nodes = dict(last[1])
        nodes[0] = (0, _wrap16(int(round(a / Y.ANG))), 0)
        root = tuple(max(-32768, min(32767, int(round(v)))) for v in p)
        msk = sum(1 << kk for kk in nodes)
        pose = bytes([0x75, 0]) + struct.pack('<3h', *root) + bytes([msk & 0xff, msk >> 8 & 0xff, msk >> 16 & 0xff])
        for kk in sorted(nodes):
            pose += struct.pack('<3h', *nodes[kk])
        out.append((f, Y.join_record(b'', pose)))
    return out


def world_root(data):
    """an object track whose root is far from its parent (> 15 units): a world position (HCTP 3411 segment 4's bike,
    parked by the ring under a slot that still names the rider)"""
    for f, r in H.records(data):
        _, pose = Y.split_record(r)
        if len(pose) > 1:
            for sub in H.decode_obj(pose)['subs']:
                if sub.get('root') is not None:
                    return math.hypot(*sub['root']) > 1500
    return False


def conv_cam_pose(pose, dy, fov):
    c = pose[0]
    pos = list(struct.unpack_from('<3h', pose, 1))
    rot = struct.unpack_from('<3h', pose, 7)
    pos[1] += dy / 0.01
    return bytes([c]) + struct.pack('<3f3ff', *[float(v) for v in pos], *[v * Y.ANG for v in rot], fov)


def filter_events(ev, T, keep, rep, label, remap=None):
    """keep the ops in `keep` (remap: op -> function(bytes) -> bytes), report the rest as dropped"""
    if not ev:
        return b''
    lst, ok = Y.parse_events(ev, T)
    out = bytearray()
    for op, b in lst:
        if op is None:
            rep['%s: event bytes past the size table dropped' % label] += 1
        elif op in keep:
            if remap and op in remap:
                nb = remap[op](b)
                if nb != b:
                    rep['%s: op %d %s -> %s' % (label, op, b.hex(), nb.hex())] += 1
                b = nb
            out += b
        else:
            rep['%s: op %d dropped (%s)' % (label, op, DROP_WHY.get(op, 'no 2011 meaning'))] += 1
    return bytes(out)


def frames_of(data):
    p, f = 0, 0
    while p < len(data):
        b = data[p]
        if b < 0x60:
            f += b; p += 1; continue
        sz = data[p + 1] if b == 0x60 else b - 0x60
        p += (2 if b == 0x60 else 1) + sz
        f += 1
    return f


def cam_follow(b):
    """op 113 [113][c]: HCTP c = cast index + 1 the camera height follows (71 01 = the entrant);
    2011's stock entrance cameras write 71 00 for the entrant [L]"""
    return bytes([b[0], max(0, b[1] - 1)])


def convert_records(kind, data, T, rep, ground_fix=True, fov=CAM_FOV, shifts=None, vehicle=None, attached=False,
                    origin=None):
    """one HCTP track -> ([(frame, 2011 record)], frames). kind: char / camera / prop / rope.
    shifts: [(frame, dy)] of the entrant in this segment (cameras follow it). vehicle: VEHICLES entry of a prop
    that is a vehicle (attached: this segment's slot has a parent). origin: the slot origin (root units) baked
    into the roots (rider and vehicle of a vehicle entrance)."""
    recs = []
    for f, r in H.records(data):
        ev, pose = Y.split_record(r)
        if kind == 'char':
            ev2 = filter_events(ev, T, KEEP_CHAR, rep, 'character')
            p2 = conv_char_pose(pose, ground_fix, rep, origin) if len(pose) > 1 else pose
        elif kind == 'camera':
            ev2 = filter_events(ev, T, KEEP_CAM, rep, 'camera', {113: cam_follow})
            if len(pose) == 13:
                p2 = conv_cam_pose(pose, shift_for_frame(shifts, f) if (ground_fix and shifts) else 0.0, fov)
            elif len(pose) == 19:                # (SvR 2004: [count][f32 x3][s16 rotation x3])
                pos = list(struct.unpack_from('<3f', pose, 1))
                rot = struct.unpack_from('<3h', pose, 13)
                pos[1] += (shift_for_frame(shifts, f) if (ground_fix and shifts) else 0.0) / 0.01
                p2 = bytes([pose[0]]) + struct.pack('<3f3ff', *pos, *[v * Y.ANG for v in rot], fov)
            else:
                p2 = pose
                if pose:
                    rep['camera key of %d bytes copied' % len(pose)] += 1
        elif kind == 'prop':
            ev2 = filter_events(ev, T, KEEP_PROP, rep, 'prop')
            if vehicle and len(pose) > 1:
                p2 = conv_vehicle_pose(pose, vehicle, attached, origin)
            else:
                p2 = conv_obj_pose(pose, origin) if len(pose) > 1 else pose
        else:                                    # ropes: 5-byte scalar keys, same in 2011
            ev2 = filter_events(ev, T, set(), rep, kind)
            p2 = pose
        if not ev2 and not p2 and (ev or pose):
            continue
        recs.append((f, Y.join_record(ev2, p2)))
    return recs, frames_of(data)


# ---------------------------------------------------------------------------- segment plan

def char_state_at(recs, frame):
    """absolute 2011 character state (root, bones) reached by the keys before `frame`"""
    data = Y.build_stream(Y.make_stream([(f, r) for f, r in recs if f < frame], frame))
    ks = [k for k in Y.body_keys(data, S.T11) if k['has_pose']]
    return (ks[-1]['root'], ks[-1]['bones']) if ks else (None, None)


def cut(kind, recs, f0, f1):
    """records [f0, f1) of a converted track, re-based to frame 0; a cut that starts inside the
    track gets the state at f0 as its first key (character: a full pose; camera: the last key)"""
    out = [(f - f0, r) for f, r in recs if f0 <= f < f1]
    if f0 and kind in ('char', 'camera'):
        if not out or out[0][0] != 0 or len(Y.split_record(out[0][1])[1]) < 2:
            if kind == 'char':
                root, bones = char_state_at(recs, f0)
                head = Y.full_pose(0, root, bones) if root is not None else b''
            else:
                prev = [r for f, r in recs if f < f0 and len(Y.split_record(r)[1]) == 29]
                head = b''
                if prev:
                    p = bytearray(Y.split_record(prev[-1])[1]); p[0] = 0
                    head = bytes(p)
            if head:
                if out and out[0][0] == 0:
                    ev, _ = Y.split_record(out[0][1])
                    out[0] = (0, Y.join_record(ev, head))
                else:
                    out.insert(0, (0, Y.join_record(b'', head)))
    return out


def join(parts):
    """concatenate (records, frames) parts into one stream"""
    recs, base = [], 0
    for rs, n in parts:
        recs += [(f + base, r) for f, r in rs]
        base += n
    return Y.build_stream(Y.make_stream(recs, base)), base


def intro_end(char_recs, cam_recs, hidden_y=15.0):
    """frame where HCTP segment 0's hidden intro ends: the first camera cut (a key with count 0)
    at or after the entrant comes up from below the stage (root y < hidden_y); 0 = no intro"""
    data = Y.build_stream(Y.make_stream(char_recs, max(f for f, _ in char_recs) + 1))
    ks = [k for k in Y.body_keys(data, S.T11) if k['has_pose']]
    if not ks or ks[0]['root'][1] * 0.01 < hidden_y:
        return 0
    up = next((k['frame'] for k in ks if k['root'][1] * 0.01 < hidden_y), None)
    if up is None:
        return 0
    for f, r in cam_recs:
        p = Y.split_record(r)[1]
        if f >= up and len(p) == 29 and p[0] == 0:
            return f
    return up


def plan_groups(nseg, groups):
    """2011 segment -> the whole source sub-scenes grouped by position (SvR 2004)"""
    return {j: [(k, 0, None) for k in range(a, min(b, nseg))] for j, (a, b) in enumerate(groups)
            if a < nseg}


def plan(nseg, intro):
    """2011 segment (0..4) -> [(HCTP segment, from frame, to frame or None)].
    2011's 5 segments: 0 intro (entrant hidden, titantron), 1 stage, 2 ramp, 3 ringside and into
    the ring, 4 in-ring pose. HCTP 3047's: 0 intro + stage, 1 ramp, 2 ringside, 3 through the
    ropes, 4 in-ring pose."""
    if nseg == 5 and intro:
        return {0: [(0, 0, intro)], 1: [(0, intro, None)], 2: [(1, 0, None)],
                3: [(2, 0, None), (3, 0, None)], 4: [(4, 0, None)]}
    return {k: [(k, 0, None)] for k in range(nseg)}


# ============================================================================ 2011 EVP

def slot11(j, on, evt_id, params, parent):
    s = bytearray(S.SLOT)
    struct.pack_into('>HBBII', s, 0, j, on, 0, evt_id, j)
    for i, v in enumerate(params):
        struct.pack_into('>f', s, 12 + 4 * i, v)
    struct.pack_into('>HH', s, S.SLOT - 4, parent, 0)
    return bytes(s)


DEFAULT_HAND_LIB = {}             # (hand_tracks.library(game pac): set by entrance_port for every caller)


def build(hnum, number, src, name, fov=CAM_FOV, ground_fix=True, prop_map=None, game='hctp', hand_lib=None,
          own_props=None):
    """-> (evp bytes, evt bytes, report Counter, notes list); game: hctp / svr04 (GAMES). own_props: {HCTP vehicle:
    2011 id} of vehicles whose own model the pack adds (hctp_bike / entrance_props): their HCTP tracks node for
    node, no 2540 remap"""
    own_props = own_props or {}
    cfg = GAMES[game]
    prop_map = PROP_MAP if prop_map is None else prop_map
    sevp = parse_sevp(src['sevp'])
    if hnum not in sevp:
        raise SystemExit('HCTP entrance %d is not in SEVP' % hnum)
    script = sevp[hnum]
    T, ents = H.read_bank(src['banks'][hnum])
    tracks = {(e['id'], e['x'], e['y']): e for e in ents}
    rep = collections.Counter()
    notes = []
    bank = Y.Bank(b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + S.T11, [])
    bank.lead = b''
    kinds = [kind_of(o) for o in script['objs']]
    nseg = min(cfg['segments'], max(len(o['slots']) for o in script['objs']))
    # every HCTP track converted per HCTP segment: conv[(object, segment)] = (kind, recs, frames)
    conv = {}
    shifts = {}
    attached = {}
    order = sorted(range(len(script['objs'])), key=lambda i: {'char': 0, 'camera': 1}.get(kinds[i], 2))
    ent = next((i for i, kd in enumerate(kinds) if kd == 'char'), 0)
    # A vehicle entrance: the rider's and the vehicle's slot origins go into their roots and their 2011 slots stay at
    # 0 (2011's own vehicles have no slot origin; a 2011 segment made of two HCTP ones has one origin for both - 3003
    # segment 2 rides round the ring 10.2 lower than segment 3, and the bike floated 10 units up)
    baked = set()
    if any(kinds[i] == 'prop' and o['model'] in VEHICLES for i, o in enumerate(script['objs'])):
        baked = {ent} | {i for i, o in enumerate(script['objs']) if kinds[i] == 'prop' and o['model'] in VEHICLES}
        # HCTP's flat-stage versions (32xx / 35xx: the stage segments' slot 12 down = on the arena floor) already
        # stand where 2011's flat stage is: no ground fix (3203 / 3511 sank 12 units with it)
        sl = script['objs'][ent]['slots']
        if ground_fix and len(sl) > 1 and sl[1]['params'][1] >= 9:
            ground_fix = False
            notes.append('flat-stage version (stage slot %.1f down): no ground fix' % sl[1]['params'][1])
    for i in order:
        o = script['objs'][i]
        kind = kinds[i]
        tk = kind if kind in ('char', 'camera', 'prop') else 'rope'
        for k, s in enumerate(o['slots'][:nseg]):
            e = tracks.get((s['tid'], s['tx'], s['ty'])) or tracks.get((s['tid'] | CAM_FLAG, s['tx'], s['ty']))
            if not e or not e['data']:
                continue
            veh = VEHICLES.get(o['model']) if tk == 'prop' else None
            att = s['parent'] != S.NONE16 and not (veh and world_root(e['data']))
            org = tuple(v / 0.01 for v in s['params'][:3]) + (s['params'][4],) if i in baked and not att else None
            if tk == 'char' and i == 0:
                shifts[k] = char_keys_shift(e['data'], org) if ground_fix else []
            recs, n = convert_records(tk, e['data'], T, rep, ground_fix, fov,
                                      shifts.get(k) if tk == 'camera' else None,
                                      None if o['model'] in own_props else veh, att, org)
            if veh:
                attached[(i, k)] = att
            conv[(i, k)] = (tk, recs, max(n, e['frames']))
    cam = next((i for i, kd in enumerate(kinds) if kd == 'camera'), None)
    for i, o in enumerate(script['objs']):   # (a vehicle attached to the rider: baked onto a world track)
        veh = VEHICLES.get(o['model']) if kinds[i] == 'prop' else None
        if not veh:
            continue
        seat = None
        if o['model'] in own_props:   # (where he sits on it: its first world segment, else 3003's)
            kw = next((k for k in range(nseg) if (i, k) in conv and (ent, k) in conv and attached.get((i, k)) is False
                       and len(conv[(i, k)][1]) > 1), None)
            seat = seat_of(conv[(ent, kw)][1], conv[(i, kw)][1]) if kw is not None else None
            seat = seat or OWN_SEAT
            notes.append('object %d: the rider sits at %s on his bike (%s)' % (
                i, tuple(int(v) for v in seat), 'HCTP segment %d' % kw if kw is not None else '3003\'s seat'))
        for k, s in enumerate(o['slots'][:nseg]):
            if attached.get((i, k)) and (i, k) in conv and (ent, k) in conv:
                n = conv[(ent, k)][2]
                baked_recs = (bake_own(conv[(ent, k)][1], conv[(i, k)][1], n, seat) if o['model'] in own_props else
                              bake_vehicle(conv[(ent, k)][1], conv[(i, k)][1], n, veh))
                conv[(i, k)] = ('prop', baked_recs, n)
                notes.append('object %d: HCTP segment %d - the bike on the rider, baked to a world track' % (i, k))
            elif (s['parent'] != S.NONE16 and (i, k) in conv and (i, k - 1) in conv and conv[(i, k - 1)][1]
                  and len(conv[(i, k)][1]) <= 1):
                # one parked key under the rider's slot (3411 / 3511 segment 4: tilted 30 degrees): the bike stays
                # where the segment before left it
                last = Y.split_record(conv[(i, k - 1)][1][-1][1])[1]
                conv[(i, k)] = ('prop', [(0, Y.join_record(b'', last))], conv[(i, k)][2])
                notes.append('object %d: HCTP segment %d - the bike stays parked where segment %d left it' % (i, k, k - 1))
    intro = 0
    if cfg['groups']:
        pl = plan_groups(nseg, cfg['groups'])
    else:
        if (ent, 0) in conv and cam is not None and (cam, 0) in conv:
            intro = intro_end(conv[(ent, 0)][1], conv[(cam, 0)][1])
        pl = plan(nseg, intro)
    notes.append('segments: ' + '; '.join('2011 %d <- %s' % (j, ' + '.join(
        'HCTP %d [%d..%s)' % (hk, a, '' if b is None else b) for hk, a, b in pl[j])) for j in sorted(pl)))
    seg_frames = {}
    for j in sorted(pl):              # a 2011 segment lasts as long as its entrant track
        tot = 0
        for hk, a, b in pl[j]:
            n = conv.get((ent, hk), (None, None, 0))[2]
            tot += (n if b is None else b) - a
        seg_frames[j] = tot
    slot_src = {}                     # (object, 2011 segment) -> HCTP slot used for its EVP slot
    for i, o in enumerate(script['objs']):
        for j in sorted(pl):
            parts, have = [], False
            for hk, a, b in pl[j]:
                full = conv.get((ent, hk), (None, None, 0))[2]
                b2 = full if b is None else b
                c = conv.get((i, hk))
                if c and c[0] == 'prop' and o['model'] not in VEHICLES:
                    # one-frame prop poses: only the first part's, held for the segment
                    if not have:
                        parts.append((c[1], b2 - a)); have = True
                    else:
                        parts.append(([], b2 - a))
                elif c:
                    parts.append((cut(c[0], c[1], a, b2), b2 - a)); have = True
                else:
                    parts.append(([], b2 - a))
            if have:
                data, n = join(parts)
                Y.insert_motion(bank, S.ID_SHIFT + i, 5 * j, 0, data, n, S.RT_2011)
                rep['tracks %s' % kinds[i]] += 1
            hk0 = next((hk for hk, a, b in pl[j] if (i, hk) in conv), pl[j][0][0])
            slot_src[(i, j)] = o['slots'][hk0] if hk0 < len(o['slots']) else None
    # ---- the hands: HCTP switches hand shapes by event (op 16 `10 ss iiii`: ss 1 left / 2 right, 2011's
    # pose ids 0-105 / 200-305; ss 0 = the face); 2011's entrances animate the fingers on the entrant's
    # y3 (left) / y2 (right) tracks: made from the pose library (hand_tracks.py), per 2011 segment
    hand_lib = DEFAULT_HAND_LIB if hand_lib is None else hand_lib
    if hand_lib and ent < len(script['objs']):
        o = script['objs'][ent]
        shape = {1: None, 2: None}
        made = 0
        for j in sorted(pl):
            evs = {1: [], 2: []}
            off = 0
            for hk, a, b in pl[j]:
                full = conv.get((ent, hk), (None, None, 0))[2]
                b2 = full if b is None else b
                s = o['slots'][hk] if hk < len(o['slots']) else None
                e = tracks.get((s['tid'], s['tx'], s['ty'])) if s else None
                if e and e['data']:
                    for f, r in H.records(e['data']):
                        if not a <= f < b2:
                            continue
                        ev, _ = Y.split_record(r)
                        if not ev:
                            continue
                        for op, bb in Y.parse_events(ev, T)[0]:
                            if op == 16 and len(bb) >= 4 and bb[1] in (1, 2):
                                evs[bb[1]].append((off + f - a, struct.unpack_from('<H', bb, 2)[0]))
                off += b2 - a
            n = seg_frames.get(j, 0)
            if n <= 0:
                continue
            for hand in (1, 2):
                data, shape[hand] = HT.track(evs[hand], n, hand, hand_lib, shape[hand])
                if data:
                    Y.insert_motion(bank, S.ID_SHIFT + ent, 5 * j, HT.Y_OF[hand], data, n, S.RT_2011)
                    made += 1
            rep['hand events'] += len(evs[1]) + len(evs[2])
        notes.append('hands: %d finger tracks from %d hand events' % (made, rep['hand events']))
    else:
        notes.append('hands: no finger tracks (the 2011 pac folder - m.pac pose library - not given)')
    objs = []
    for i, o in enumerate(script['objs']):
        kind = kinds[i]
        model = o['model']
        if kind == 'char':
            model = S.PREVIEW_CHAR_11 if i == ent else S.GENERIC_CHAR_11
        elif kind == 'prop':
            if model in own_props:
                new = own_props[model]
                notes.append('object %d: HCTP prop %d -> evtobj %d (its own model, added by the pack; its own '
                             'nodes)' % (i, model, new))
            else:
                new = VEHICLES[model]['model'] if model in VEHICLES else prop_map.get(model, model)
                notes.append('object %d: HCTP prop %d -> 2011 evtobj %d%s' % (i, model, new, ' (the bike: its nodes mapped)'
                                                                          if model in VEHICLES else ''))
            model = new
        slots = []
        for jj in range(SLOTS_11):
            s = slot_src.get((i, jj // 5)) if jj % 5 == 0 else None
            if s is not None and i in baked:
                s = dict(s, params=(0.0, 0.0, 0.0, s['params'][3], 0.0),
                         parent=S.NONE16 if kind == 'prop' else s['parent'])
            if s is None:
                slots.append(slot11(jj, 0, S.ID_SHIFT + i, (), S.NONE16))
                continue
            p = list(s['params'])
            if ground_fix and s['parent'] == S.NONE16 and kind != 'camera' and any(p[:3]):
                p[1] += shift_at(p[2])
            slots.append(slot11(jj, s['on'], S.ID_SHIFT + i, [0.0] + p, s['parent']))
        objs.append(dict(idx=i, model=model, t0=o['t0'], t1=o['t1'], w4=0xffffffff, w5=0, nslots=SLOTS_11,
                         lo=0, slots=slots))
    nm = name.encode('latin-1')[:29]
    e11 = dict(num=number, name=nm + bytes(30 - len(nm)), f10=0x10000, f18=0x10000, f44=0, objs=objs,
               tail=dict(num=number, recs=[(1, 0, 100, [(2, 0, 999), (1, 0, 0), (1, 0, 0)]),
                                           (8, 0, 100, [(3, 0, 999), (3, 0, 0), (3, 0, 0)])]),
               game='2011')
    return S.build_evp(e11, '2011'), Y.write_bank(bank), rep, notes


# ============================================================================ packaging

def nyujyo_for(number):
    """the 2011 entrance pac and group letters a number belongs to (by Yuke's ranges)"""
    ranges = [(100, 134, '0', '1'), (135, 178, '1', '2'), (179, 271, '2', '3'), (272, 290, '3', '4'),
              (291, 479, '4', '5'), (480, 511, '5', 'E'), (512, 531, '6', 'F'), (532, 551, '7', 'G'),
              (552, 599, '8', 'H'), (600, 999, '9', 'I')]
    for lo, hi, f, g in ranges:
        if lo <= number <= hi:
            return 'evt\\Nyujyo%s.pac' % f, 'EVP' + g, 'EVT' + g
    raise SystemExit('pick a number in 100..999 (Nyujyo0-9 ranges)')


def write_pack(out, number, files, label):
    pac, gp, gt = nyujyo_for(number)
    d = os.path.join(out, 'pack')
    os.makedirs(os.path.join(d, 'entrance'), exist_ok=True)
    lines = ['# %s' % label]
    for n, evp, evt in files:
        for g, b, suf in ((gp, evp, 'evp'), (gt, evt, 'evt')):
            fn = 'entrance/%d_%s.bin' % (n, suf)
            open(os.path.join(d, fn), 'wb').write(b)
            lines.append('pacentry %s %s %d %s' % (pac, g, n, fn))
    open(os.path.join(d, 'pack.txt'), 'w', newline='\r\n').write('\n'.join(lines) + '\n')
    return d


def cmd_list(a):
    src = read_evtent(a[0])
    names = entrance_names(a[1]) if len(a) > 1 else {}
    sevp = parse_sevp(src['sevp'])
    for n in sorted(sevp):
        bank = src['banks'].get(n)
        kinds = collections.Counter(kind_of(o) for o in sevp[n]['objs'])
        print('%4d  %-26s %3d objects %-40s bank %s' % (n, names.get(n, ''), len(sevp[n]['objs']),
              ' '.join('%s %d' % kv for kv in sorted(kinds.items())), '%d bytes' % len(bank) if bank else '-'))


def cmd_convert(a):
    def opt(k, d=None):
        return a[a.index(k) + 1] if k in a else d
    src = read_evtent(a[0])
    hnum, number, out = int(a[1]), int(a[2]), a[3]
    if not 100 <= number < 1000:
        raise SystemExit('the 2011 number must be 100..999 (CAE parts are 10000 + 1000k + number)')
    name = opt('--name', 'HCTP %d' % hnum)
    fov = float(opt('--fov', CAM_FOV))
    os.makedirs(out, exist_ok=True)
    evp, evt, rep, notes = build(hnum, number, src, name, fov, '--no-ground' not in a)
    # self-check: both re-parse and every event block walks 2011's size table
    S.parse_evp(evp, '2011')
    b = Y.read_bank(evt)
    for e in b.entries:
        if not e['data']:
            continue
        for f, r in Y.parse_stream(e['data']).records():
            ev, _ = Y.split_record(r)
            if ev and not Y.parse_events(ev, b.T)[1]:
                raise SystemExit('event block does not walk T11 in (%d,%d,%d)' % (e['id'], e['x'], e['y']))
    open(os.path.join(out, '%d_evp.bin' % number), 'wb').write(evp)
    open(os.path.join(out, '%d_evt.bin' % number), 'wb').write(evt)
    files = [(number, evp, evt)]
    if '--no-parts' not in a:
        pd = os.path.join(out, 'parts')
        os.makedirs(pd, exist_ok=True)
        for n, pevp, pevt in S.cae_parts(evp, evt, number):
            open(os.path.join(pd, '%d_evp.bin' % n), 'wb').write(pevp)
            open(os.path.join(pd, '%d_evt.bin' % n), 'wb').write(pevt)
            files.append((n, pevp, pevt))
    lines = ['HCTP %d -> 2011 %d "%s"' % (hnum, number, name),
             'EVP %d bytes, EVT %d bytes, %d tracks' % (len(evp), len(evt), len(b.entries))]
    lines += ['  %-70s %d' % kv for kv in sorted(rep.items())]
    lines += ['note: ' + n for n in notes]
    if '--pack' in a:
        lines.append('move pack: ' + write_pack(out, number, files, 'HCTP entrance %d "%s" as 2011 %d' % (hnum, name, number)))
    open(os.path.join(out, 'report.txt'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
    elif sys.argv[1] == 'list':
        cmd_list(sys.argv[2:])
    elif sys.argv[1] == 'convert':
        cmd_convert(sys.argv[2:])
    else:
        print(__doc__)
