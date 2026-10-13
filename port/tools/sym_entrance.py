"""WWE SmackDown! Shut Your Mouth (PS2, "SYM", SLES_512.83) entrance -> SvR 2011 entrance.

A front end on hctp_entrance.py's back end (docs/HCTP_ENTRANCES.md, docs/OTHER_GAMES_ENTRANCES.md).
SYM keeps HCTP's motion codec but differs in three places, which this module turns into what
hctp_entrance.build() takes (an HCTP SEVP script + an HCTP-framed CEKey bank):

  * script: only the compact ENTR/SYDT (no SEVP).  Per entrance
        u16 number, u16 0, u16 casts; per cast u16 idx, u16 model, u8 type[4] (= SEVP type bytes
        1..4), u16 slots; per slot (37 bytes) u16 slot, u8 on, u32 track id/x/y, f32 x y z yaw ?,
        u16 parent.  2-7 segments (with duplicate / alternate takes), 2 camera casts.
  * frame stream: halfword framing (a byte < 0x80 skips that many frames, else a record of
        (b & 0x7f) * 2 bytes, zero-padded to an even size) -> re-framed to HCTP's (0x60 framing),
        pad bytes stripped.
  * character roots are relative to the slot transform -> baked to absolute (slot position added;
        slots with a yaw keep their transform, like HCTP's own yaw slots).  Stage / ramp heights
        are SYM's (raised stage like HCTP); the ground fix is done here with SYM's floor profile,
        so build() runs with ground_fix=False.

One take per 2011 segment (0 hidden intro, 1 stage, 2 ramp, 3 ringside + into the ring, 4 in-ring
pose) is chosen by where the entrant is; one camera; the other cast kinds as HCTP.

  python sym_entrance.py list <iso or extract folder>
  python sym_entrance.py convert <iso> <SYM number> <2011 number> <out dir> [--name TEXT] [--camera 0|1]
        [--no-ground] [--skip TAKE,TAKE] [--parts]
        writes <out>/<number>_evp.bin + _evt.bin and report.txt (self-checked like entrance_port).
  python sym_entrance.py dump <iso> <SYM number>      the script, takes and the chosen plan

Module API (entrance_port.py): list_entrances(src) and convert(src, entrance, number, name, rep).
"""
import collections
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ymks as Y  # noqa: E402
import hctp_motion as H  # noqa: E402
import hctp_entrance as HE  # noqa: E402
import svr08_entrance as S  # noqa: E402
try:
    import svrfmt  # noqa: E402
except ImportError:          # (names only)
    svrfmt = None

GAME = 'sym'
GAME_NAME = 'SmackDown! Shut Your Mouth'
EVTENT = 'PAC/EVTENT.PAC'
MOVES = 'PAC/MOVES.PAC'
SLOT_SIZE = 37
CAM_FLAG = HE.CAM_FLAG
# build() reads 5 script slots: our synthetic script always has exactly 5 (one take per 2011
# segment, already placed), so HCTP's plan (with the hidden-intro cut of slot 0) applies as is.
GAME_FLAT = 'sym_flat'          # (an entrant who does not start hidden: no intro cut)
HE.GAMES.setdefault(GAME, dict(segments=5, groups=None))
HE.GAMES.setdefault(GAME_FLAT, dict(segments=5, groups=[(k, k + 1) for k in range(5)]))

# SYM prop ids (BG.PAC SOBJ) -> 2011 evtobj.  172 is in almost every entrance (the title belt
# held by the entrant, like HCTP's 2019 [L]); the rest are unresearched and dropped.
PROP_MAP = {172: 2014}       # 2011 2014 = "WWE Title - Folded"


def _u16(d, o):
    return struct.unpack_from('<H', d, o)[0]


def _u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


# ============================================================================ source

_CACHE = {}


def _open(src):
    if isinstance(src, str):
        import discfs
        return discfs.open(src)
    return src


def dpac(d):
    """SYM DPAC -> [(group, name, bytes)].  Group header {tag, u16 2 x count, u16 base}, rows
    {name[4], u16 sector, u16 size / 0x100}, data at 0x4000 + sector * 0x800 (HCTP's reader
    takes the doubled count as the row count and runs into the next group)."""
    if d[:4] != b'DPAC':
        raise ValueError('not a DPAC')
    out = []
    off = 0x800
    while off < 0x4000 and d[off:off + 4] != bytes(4):
        tag = d[off:off + 4].decode('latin-1').strip()
        n2 = _u16(d, off + 4)
        off += 8
        for _ in range(n2 // 2):
            nm = d[off:off + 4].replace(b'\0', b'').decode('latin-1').strip()
            sec, sz = struct.unpack_from('<HH', d, off + 4)
            o = 0x4000 + sec * 0x800
            out.append((tag, nm, d[o:o + sz * 0x100]))
            off += 8
    return out


def read_source(src):
    """src: a discfs disc / extract folder object (entrance_port), a path, or this function's
    result -> dict(banks={number: bytes}, script={number: entrance}, names={number: text})"""
    if isinstance(src, dict) and 'script' in src:
        return src
    if isinstance(src, str):
        key = ('path', os.path.abspath(src))
        if key in _CACHE:
            return _CACHE[key][1]
        res = read_source(_open(src))
        _CACHE[key] = (src, res)
        return res
    key = id(src)
    if key in _CACHE and _CACHE[key][0] is src:
        return _CACHE[key][1]
    if not src.exists(EVTENT):
        raise ValueError('not a Shut Your Mouth disc: no %s' % EVTENT)
    ents = dpac(src.read(EVTENT))
    banks = {int(n): b for g, n, b in ents if g == 'EVET' and n.isdigit()}
    sydt = next((b for g, n, b in ents if g == 'ENTR' and n == 'SYDT'), None)
    if sydt is None:
        raise ValueError('%s has no ENTR/SYDT' % EVTENT)
    names = {}
    if src.exists(MOVES) and svrfmt is not None:
        names = entrance_names(src.read(MOVES))
    res = dict(banks=banks, script=parse_sydt(sydt), names=names)
    _CACHE[key] = (src, res)
    return res


def entrance_names(moves):
    """MOVES.PAC MOVE/LIST, PAC sub 1 (BPE): u32 count, count x {u32 number, u32 0xff, char[24]}"""
    names = {}
    for g, n, b in dpac(moves):
        if g == 'MOVE' and n == 'LIST':
            for i, sub in HE.pac(b):
                if i != 1:
                    continue
                if sub[:4] == b'BPE ':
                    sub = svrfmt.bpe_decode(sub)
                for k in range(_u32(sub, 0)):
                    r = sub[4 + 32 * k:36 + 32 * k]
                    if len(r) == 32:
                        names[_u32(r, 0)] = r[8:32].split(b'\0')[0].decode('latin-1').strip()
    return names


def sevp_types(t4):
    """SYDT type[4] -> HCTP SEVP (t0, t1) (two big-endian u32 of the 8 type bytes)"""
    b = bytes([1 if t4 == bytes(4) else 0]) + t4 + bytes(3)
    return struct.unpack('>II', b)


def parse_sydt(d):
    """-> {number: dict(num, objs=[dict(idx, model, t0, t1, slots=[dict(slot, on, tid, tx, ty,
    params (5 floats), parent)])])}  (the same dicts as hctp_entrance.parse_sevp)"""
    n = _u16(d, 0)
    p = 2
    out = {}
    for _ in range(n):
        num, _z, nc = struct.unpack_from('<HHH', d, p)
        p += 6
        objs = []
        for _j in range(nc):
            idx, model = struct.unpack_from('<HH', d, p)
            t0, t1 = sevp_types(d[p + 4:p + 8])
            ns = _u16(d, p + 8)
            p += 10
            slots = []
            for _k in range(ns):
                slot, on, tid, tx, ty = struct.unpack_from('<HBIII', d, p)
                params = struct.unpack_from('<5f', d, p + 15)
                parent = _u16(d, p + 35)
                slots.append(dict(slot=slot, on=on, tid=tid, tx=tx, ty=ty, params=params, parent=parent))
                p += SLOT_SIZE
            objs.append(dict(idx=idx, model=model, t0=t0, t1=t1, w4=0xffffffff, w5=0, nslots=ns, lo=0,
                             slots=slots))
        out[num] = dict(num=num, objs=objs)
    return out


# ============================================================================ frame streams

def records_hw(data):
    """SYM frame stream -> [(frame, record bytes incl. pad)]"""
    out, p, f = [], 0, 0
    while p < len(data):
        b = data[p]
        if b < 0x80:
            f += b
            p += 1
            continue
        sz = (b & 0x7f) * 2
        p += 1
        if sz == 0 or p + sz > len(data):
            raise ValueError('bad SYM record at %d' % p)
        out.append((f, data[p:p + sz]))
        p += sz
        f += 1
    return out


def strip_pad(kind, rec):
    """drop the halfword pad byte of a SYM record (kind: char / camera / prop / rope)"""
    ev, pose = Y.split_record(rec)
    if not pose:
        return rec
    if pose == b'\0':
        return Y.join_record(ev, b'')
    if kind == 'char' and len(pose) > 1:
        d = H.decode_body(pose, H.H_TYPES_WIDE)
        if d['trail'] and not d['trail'].strip(b'\0'):
            pose = pose[:len(pose) - len(d['trail'])]
    elif kind == 'camera' and len(pose) == 14 and pose[13] == 0:
        pose = pose[:13]
    elif kind == 'prop' and len(pose) > 1:
        d = H.decode_obj(pose)
        if d['trail'] and not d['trail'].strip(b'\0'):
            pose = pose[:len(pose) - len(d['trail'])]
    elif kind == 'rope' and len(pose) == 6 and pose[5] == 0:
        pose = pose[:5]
    return Y.join_record(ev, pose) if ev else pose


def frame_hctp(recs, nframes):
    """[(frame, record)] -> HCTP frame stream (b < 0x60 skip, 0x60 + u8 size, 0x61.. short)"""
    out = bytearray()
    cur = 0
    for f, r in recs:
        gap = f - cur
        while gap > 0:
            k = min(gap, 0x5f)
            out.append(k)
            gap -= k
        if len(r) > 255:
            raise ValueError('record of %d bytes does not fit HCTP framing' % len(r))
        if len(r) <= 0x1f and len(r):
            out.append(0x60 + len(r))
        else:
            out += bytes([0x60, len(r)])
        out += r
        cur = f + 1
    return bytes(out)


def build_bank(T, tracks):
    """HCTP CEKey bank (no header): T[256], u32 n, n x {key, off, frames, 0}, data"""
    dirs, data = bytearray(), bytearray()
    for (tid, x, y), (stream, frames) in sorted(tracks.items()):
        dirs += struct.pack('<IIII', tid << 16 | x << 8 | y, len(data), frames, 0)
        data += stream
    return bytes(T) + struct.pack('<I', len(tracks)) + bytes(dirs) + bytes(data)


# ============================================================================ slot transforms

# SYM casts move in their slot's frame: world = slot position + Ry(YAW_SIGN * slot yaw) * local, for
# character roots and camera positions alike (checked: cameras aim at the entrant 0.90-0.93 with the
# slot added, -0.02 without; the yaw sign by the same aim test on SYM's turned in-ring slots).
YAW_SIGN = 1.0
FLOOR = HE.FLOOR                 # 2011's flat stage / ramp / floor level (HCTP units, y down)
OUTSIDE_Z = 80.0                 # a slot beyond this z sits on SYM's raised stage / ramp
RING = 30.0                      # |x|, |z| inside the ropes
STAGE_Z, RAMP_FOOT_Z = HE.RAMP_TOP_Z, HE.RAMP_FOOT_Z


def _rot(axis, a):
    c, s = math.cos(a), math.sin(a)
    if axis == 'x':
        return ((1, 0, 0), (0, c, -s), (0, s, c))
    if axis == 'y':
        return ((c, 0, s), (0, 1, 0), (-s, 0, c))
    return ((c, -s, 0), (s, c, 0), (0, 0, 1))


def _mm(a, b):
    return tuple(tuple(sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)) for i in range(3))


def _zyx(x, y, z):
    return _mm(_mm(_rot('z', z), _rot('y', y)), _rot('x', x))


def _euler_zyx(R):
    """R = Rz Ry Rx -> both (x, y, z) solutions"""
    sy = max(-1.0, min(1.0, -R[2][0]))
    y = math.asin(sy)
    if abs(sy) < 0.99999:
        x, z = math.atan2(R[2][1], R[2][2]), math.atan2(R[1][0], R[0][0])
    else:
        x, z = 0.0, math.atan2(-R[0][1], R[1][1])
    return [(x, y, z), (x + math.pi, math.pi - y, z + math.pi)]


def _wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def _s16(v):
    v = int(round(v)) & 0xffff
    return v - 0x10000 if v & 0x8000 else v


def turn_euler(codes, yaw, prev=None):
    """s16 Euler codes (x, y, z; R = Rz Ry Rx) turned by Ry(yaw) in world -> s16 codes, the
    solution nearest `prev` (keeps the game's linear blend between keys short)"""
    R = _mm(_rot('y', yaw), _zyx(*[c * Y.ANG for c in codes]))
    best = None
    for sol in _euler_zyx(R):
        cs = tuple(_s16(_wrap(a) / Y.ANG) for a in sol)
        d = sum(abs(_s16(a - b)) for a, b in zip(cs, prev)) if prev else 0
        if best is None or d < best[0]:
            best = (d, cs)
    return best[1]


def to_world(p, slot, dy=0.0):
    """local (x, y, z) in model units -> world: slot position + Ry(yaw) p, then y + dy"""
    x, y, z = p
    yaw = YAW_SIGN * slot['params'][3]
    if yaw:
        c, s = math.cos(yaw), math.sin(yaw)
        x, z = c * x + s * z, -s * x + c * z
    sx, sy, sz = slot['params'][:3]
    return x + sx, y + sy + dy, z + sz


def ground_shift(slot):
    """root y change that puts a take on 2011's floor-level stage / ramp.  A SYM slot out on the
    raised stage / ramp has its y at the ground under it (stage 0, ramp foot +12.3) and the cast
    walks level in its frame; 2011 stands everything outside the ring on FLOOR.  Ring-frame slots
    (z near 0) already carry the arena floor in the roots: unchanged."""
    if slot is None or slot['params'][2] <= OUTSIDE_Z:
        return 0.0
    return FLOOR - slot['params'][1]


S16_MAX = 32767


def bake_char_pose(pose, slot, dy, rep, state):
    """one HCTP-wide character pose: roots slot-relative -> world (+ dy), the root bone turned by
    the slot yaw.  state: dict carried along the track (last root-bone codes)."""
    d = H.decode_body(pose, H.H_TYPES_WIDE)
    if d['trail']:
        raise ValueError('trailing bytes in a character pose')
    yaw = YAW_SIGN * slot['params'][3]
    for s in d['subs']:
        op = s['op']
        if op == 0x7b:
            continue
        if s.get('root') is not None:
            w = to_world([v * 0.01 for v in s['root']], slot, dy)
            v = tuple(c / 0.01 for c in w)
            if s['root_fmt'] == 'f32':
                s['root'] = v
            elif all(abs(c) <= S16_MAX for c in v):
                s['root'] = tuple(int(round(c)) for c in v)
            elif op == 0x7c or 0x60 <= op <= 0x6f:
                s['op'], s['root'], s['root_fmt'] = 0x7d, v, 'f32'
            elif op == 0x7e:
                s['op'], s['root'], s['root_fmt'] = 0x7f, v, 'f32'
            else:
                s['root'] = tuple(max(-S16_MAX, min(S16_MAX, int(round(c)))) for c in v)
                rep['masked character root clamped to s16'] += 1
        if yaw and 0 in s.get('bones', {}):
            b = s['bones'][0]
            codes = turn_euler((b.get('x', 0), b.get('y', 0), b.get('z', 0)), yaw, state.get('r0'))
            s['bones'][0] = dict(zip('xyz', codes))
            state['r0'] = codes
    return H.encode_body(d, H.H_TYPES_WIDE)


def bake_cam_pose(pose, slot, dy):
    """13-byte camera key [c][s16 pos x3][s16 rot x3]: position slot-relative -> world (+ dy)"""
    c = pose[0]
    pos = [v * 0.01 for v in struct.unpack_from('<3h', pose, 1)]
    rot = struct.unpack_from('<3h', pose, 7)
    w = to_world(pos, slot, dy)
    yaw = YAW_SIGN * slot['params'][3]
    if yaw:
        rot = turn_euler(rot, yaw)
    p = [max(-S16_MAX, min(S16_MAX, int(round(v / 0.01)))) for v in w]
    return bytes([c]) + struct.pack('<3h3h', *p, *rot)


# ============================================================================ takes and plan

ROLE_NAMES = {0: 'intro + stage', 1: 'stage', 2: 'ramp', 3: 'ringside / into the ring', 4: 'in the ring'}
ROLE_SLOT = {0: 0, 1: 0, 2: 1, 3: 2, 4: 4}     # synthetic HCTP slot: 2011 segment = HCTP's plan()


def role_of(first):
    x, y, z = first
    if z > STAGE_Z:
        return 1
    if z > RAMP_FOOT_Z:
        return 2
    if max(abs(x), abs(z)) > RING:
        return 3
    return 4


def _slot_of(o, k):
    return next((s for s in o['slots'] if s['slot'] == k), None)


def _kind(o):
    k = HE.kind_of(o)
    return k if k in ('char', 'camera', 'prop') else 'rope'


def char_roots(recs):
    """[(frame, (x, y, z) model units)] of a converted (HCTP-wide) character record list"""
    out = []
    for f, r in recs:
        ev, pose = Y.split_record(r)
        if len(pose) < 2:
            continue
        for s in H.decode_body(pose, H.H_TYPES_WIDE)['subs']:
            if s.get('root') is not None and s['op'] != 0x7b:
                out.append((f, tuple(v * 0.01 for v in s['root'])))
    return out


def analyse(sym, num, camera=0, ground=True, light=False, skip=()):
    """-> dict: objs (kept SYM cast indices, in output order), kinds, takes [dict(k, frames, role,
    first, last, dup)], tracks {(new object, take k): (kind, [(frame, HCTP record)])}, T, notes,
    warnings, dropped, rep (Counter).  light: the entrant and the camera only (listing);
    skip: SYM take numbers to leave out."""
    if num not in sym['script']:
        raise ValueError('Shut Your Mouth has no entrance %d' % num)
    bank = sym['banks'].get(num)
    if not bank or len(bank) <= 0x200:
        raise ValueError('Shut Your Mouth entrance %d has no motion bank' % num)
    script = sym['script'][num]
    T, ents = H.read_bank(bank)
    tracks = {(e['id'], e['x'], e['y']): e for e in ents}
    rep = collections.Counter()
    notes, warnings, dropped = [], [], []
    objs = script['objs']
    kinds = [_kind(o) for o in objs]
    chars = [i for i, k in enumerate(kinds) if k == 'char']
    cams = [i for i, k in enumerate(kinds) if k == 'camera']
    if not chars:
        raise ValueError('entrance %d has no character' % num)
    ent = chars[0]
    keep = list(chars)
    if cams:
        ci = cams[min(camera, len(cams) - 1)]
        keep.append(ci)
        if len(cams) > 1:
            notes.append('camera: SYM cast %d of %s (the other is an alternate camera; --camera picks)' % (ci, cams))
    for i, (o, k) in enumerate(zip(objs, kinds)):
        if k == 'prop':
            if o['model'] in PROP_MAP:
                keep.append(i)
                notes.append('object %d: SYM prop %d -> 2011 evtobj %d [L: held in every entrance like '
                             'HCTP\'s folded title]' % (i, o['model'], PROP_MAP[o['model']]))
            else:
                dropped.append('prop object %d (SYM prop id %d): no 2011 equivalent known' % (i, o['model']))
        elif k == 'rope':
            keep.append(i)
    if light:
        keep = [i for i in keep if i == ent or kinds[i] == 'camera']
    keep = sorted(keep, key=lambda i: (0 if kinds[i] == 'char' else 1, i))
    newi = {i: j for j, i in enumerate(keep)}

    def track_of(o, s):
        if s is None:
            return None
        e = tracks.get((s['tid'], s['tx'], s['ty'])) or tracks.get((s['tid'] | CAM_FLAG, s['tx'], s['ty']))
        return e if e and e['data'] else None

    takes = []
    out = {}
    for s0 in objs[ent]['slots']:
        k = s0['slot']
        e0 = track_of(objs[ent], s0)
        if e0 is None:
            warnings.append('take %d: the entrant has no track, skipped' % k)
            continue
        n = max(e0['frames'], max((f + 1 for f, _ in records_hw(e0['data'])), default=0))
        dy0 = ground_shift(s0) if ground else 0.0
        conv = {}
        for i in keep:
            o = objs[i]
            s = _slot_of(o, k)
            e = track_of(o, s)
            if e is None:
                continue
            kind = kinds[i]
            dy = (ground_shift(s) if ground else 0.0) if kind == 'char' else dy0
            recs = []
            state = {}
            for f, r in records_hw(e['data']):
                if f >= n:
                    rep['records past the entrant take dropped'] += 1
                    continue
                r = strip_pad(kind, r)
                ev, pose = Y.split_record(r)
                if kind == 'char' and len(pose) > 1:
                    pose = bake_char_pose(pose, s, dy, rep, state)
                elif kind == 'camera' and len(pose) == 13:
                    pose = bake_cam_pose(pose, s, dy)
                elif kind == 'camera' and pose:
                    rep['camera key of %d bytes kept as is' % len(pose)] += 1
                recs.append((f, Y.join_record(ev, pose)))
            conv[newi[i]] = (kind, recs)
        roots = char_roots(conv[newi[ent]][1])
        first = roots[0][1] if roots else (0.0, 0.0, 0.0)
        last = roots[-1][1] if roots else first
        # an alternate take: the same entrant motion as an earlier one (same length, same start and
        # end; SYM keeps such pairs with different camera work - e.g. 3000 takes 2 / 3)
        dup = next((t['k'] for t in takes if t['dup'] is None and t['frames'] == n
                    and math.dist(t['first'], first) < 2.0 and math.dist(t['last'], last) < 2.0), None)
        takes.append(dict(k=k, frames=n, first=first, last=last, dup=dup, shift=dy0,
                          yaw=s0['params'][3], role=None))
        for j, c in conv.items():
            out[(j, k)] = c
    # roles: take 0 = hidden intro (+ stage); the rest by where the entrant starts, never back
    prev = 0
    for t in takes:
        if t['dup'] is not None:
            continue
        if skip and t['k'] in skip:
            notes.append('take %d left out (asked)' % t['k'])
            continue
        if t is not takes[0] and t['frames'] <= 1:
            notes.append('take %d left out: one frame (entrant at %.0f %.0f %.0f)' % (t['k'], *t['first']))
            continue
        if t is takes[0]:
            t['role'] = 0
        else:
            t['role'] = max(prev, role_of(t['first']))
        prev = max(prev, t['role'])
    live = [t for t in takes if t['role'] is not None]
    for t0, t1 in zip(live, live[1:]):
        back = t1['first'][2] - t0['last'][2]
        if t1['role'] <= 2 and back > 15:
            notes.append('take %d starts %.0f units back up the stage / ramp from where take %d ends '
                         '(played in sequence; if it is an alternate, leave one out with skip)' % (t1['k'], back, t0['k']))
    return dict(num=num, script=script, objs=keep, kinds=[kinds[i] for i in keep], ent=0, takes=takes,
                tracks=out, T=T, notes=notes, warnings=warnings, dropped=dropped, rep=rep)


def layout(a):
    """role -> [take] in play order (duplicates left out)"""
    by_role = collections.defaultdict(list)
    for t in a['takes']:
        if t['role'] is not None:
            by_role[ROLE_SLOT[t['role']]].append(t)
    return by_role


def slot_track(a, j, takes):
    """object j's records over `takes` joined -> (records, frames, have)"""
    kind = a['kinds'][j]
    recs, base, have = [], 0, False
    for t in takes:
        c = a['tracks'].get((j, t['k']))
        if c and t.get('stub'):
            # one-frame stand-in: the take's first (or last) record, pose only (the state the
            # neighbouring segment starts / ends in)
            rs = [(f, r) for f, r in c[1] if Y.split_record(r)[1]]
            if rs:
                f0 = rs[0][0] if t['stub'] == 'first' else rs[-1][0]
                for f, r in rs:
                    if f == f0:
                        p = Y.split_record(r)[1]
                        if kind == 'camera' and len(p) == 13:
                            p = b'\0' + p[1:]                  # (a cut, no blend)
                        recs.append((base, p))
                have = True
            base += t['frames']
            continue
        if c:
            if not (kind == 'prop' and len(c[1]) <= 1 and have):   # one-frame prop poses: the first take's
                recs += [(f + base, r) for f, r in c[1]]
            have = True
        base += t['frames']
    return recs, base, have


def intro_frames(a, takes0):
    """HCTP's intro cut (hctp_entrance.intro_end) on the joined first-slot takes: 0 = the entrant
    does not start hidden"""
    cam = next((j for j, k in enumerate(a['kinds']) if k == 'camera'), None)
    ch, n, have = slot_track(a, a['ent'], takes0)
    if not have or cam is None:
        return 0
    cm, _, chave = slot_track(a, cam, takes0)
    if not chave:
        return 0
    rep = collections.Counter()
    c11, _ = HE.convert_records('char', frame_hctp(ch, n), a['T'], rep, False)
    m11, _ = HE.convert_records('camera', frame_hctp(cm, n), a['T'], rep, False)
    if not c11:
        return 0
    return HE.intro_end(c11, m11)


def plan_of(a):
    """-> (game key for hctp_entrance.GAMES, [synthetic slot -> takes], intro frames, 2011 frames x5)"""
    by_role = layout(a)
    intro = intro_frames(a, by_role[0]) if by_role[0] else 0
    if intro:
        game, slots = GAME, [by_role[0], by_role[1], by_role[2], [], by_role[4]]
        segs = [(0,), (1,), (2, 3), (4,)]                    # 2011 segments 1-4 from these slots
    else:
        # no hidden start: a one-frame stand-in (frame 0 of the first take) for 2011's hidden segment 0
        game = GAME_FLAT
        slots = [[dict(t, frames=1, stub='first') for t in by_role[0][:1]],
                 by_role[0], by_role[1], by_role[2], by_role[4]]
        segs = [(1,), (2,), (3,), (4,)]
    # an empty 2011 segment gets a one-frame stand-in (the last frame before it) instead of 0 frames
    last = None
    for sls in segs:
        if any(slots[s] for s in sls):
            last = [t for s in sls for t in slots[s]][-1]
        elif last is not None:
            slots[sls[0]] = [dict(last, frames=1, stub='last')]
    n = [sum(t['frames'] for t in s) for s in slots]
    frames = [intro, n[0] - intro, n[1], n[2] + n[3], n[4]] if intro else n
    return game, slots, intro, frames


def synth(a):
    """analyse() result -> (game key, SEVP bytes, HCTP bank bytes, plan lines, 2011 frames x5)
    for hctp_entrance.build"""
    num = a['num']
    game, sl_takes, intro, frames = plan_of(a)
    bank_tracks = {}
    objs = []
    for j, i in enumerate(a['objs']):
        o = a['script']['objs'][i]
        kind = a['kinds'][j]
        src = o['slots'][0] if o['slots'] else None
        parent = src['parent'] if src else S.NONE16
        if parent != S.NONE16:
            parent = a['objs'].index(parent) if parent in a['objs'] else S.NONE16
        tid = 9950 | (CAM_FLAG if kind == 'camera' else 0)
        slots = []
        for sl in range(5):
            recs, base, have = slot_track(a, j, sl_takes[sl])
            if have and recs:
                bank_tracks[(tid, j, sl)] = (frame_hctp(recs, base), base)
            slots.append(dict(slot=sl, on=1, tid=tid, tx=j, ty=sl,
                              params=(0.0, 0.0, 0.0, 0.0, 0.0), parent=parent))
        objs.append(dict(idx=j, model=o['model'], t0=o['t0'], t1=o['t1'], slots=slots))
    sevp = pack_sevp({num: objs})
    bank = build_bank(a['T'], bank_tracks)
    plan = []
    for sl in range(5):
        plan.append('HCTP-form slot %d <- SYM take %s' % (sl, ' + '.join(
            '%d (%s, %d frames)' % (t['k'], ('one-frame stand-in, its %s frame' % t['stub']) if t.get('stub')
                                     else ROLE_NAMES[t['role']], t['frames'])
            for t in sl_takes[sl]) or '- (none)'))
    plan.append('2011 segments 0-4: %s frames (%s)' % (' / '.join(map(str, frames)),
                'hidden intro cut at frame %d of the first takes' % intro if intro else
                'no hidden start found: segment 0 is a one-frame stand-in'))
    dups = ['%d = %d' % (t['k'], t['dup']) for t in a['takes'] if t['dup'] is not None]
    if dups:
        plan.append('alternate takes left out (same motion as an earlier take): ' + ', '.join(dups))
    return game, sevp, bank, plan, frames


def pack_sevp(entries):
    """{number: [obj dict(idx, model, t0, t1, slots)]} -> HCTP SEVP bytes (parse_sevp's layout;
    pointers relative to offset 8)"""
    b = 8
    nums = sorted(entries)
    pos = 0x18 + 12 * len(nums)
    objs_at, slots_at = {}, {}
    for n in nums:
        objs_at[n] = pos
        pos += 28 * len(entries[n])
    for n in nums:
        for o in entries[n]:
            slots_at[(n, o['idx'])] = pos
            pos += HE.HSLOT * len(o['slots'])
    d = bytearray(pos)
    d[0:4] = b'POF0'
    d[8:10] = b'VE'
    struct.pack_into('<II', d, 0x10, len(nums), 0x18 - b)
    for t, n in enumerate(nums):
        struct.pack_into('<HHII', d, 0x18 + 12 * t, n, len(entries[n]), 0, objs_at[n] - b)
        for j, o in enumerate(entries[n]):
            q = objs_at[n] + 28 * j
            struct.pack_into('<HH', d, q, o['idx'], o['model'])
            struct.pack_into('>II', d, q + 4, o['t0'], o['t1'])
            struct.pack_into('<IIHHI', d, q + 12, 0xffffffff, 0, len(o['slots']), 0, slots_at[(n, o['idx'])] - b)
            for k, s in enumerate(o['slots']):
                p = slots_at[(n, o['idx'])] + HE.HSLOT * k
                struct.pack_into('<HBBIII5fHH', d, p, s['slot'], s['on'], 0, s['tid'], s['tx'], s['ty'],
                                 *s['params'], s['parent'], 0)
    return bytes(d)


# ============================================================================ API

def build(sym, num, number, name, camera=0, ground=True, fov=HE.CAM_FOV, skip=()):
    """-> (evp bytes, evt bytes, report Counter, notes, warnings, dropped)"""
    a = analyse(sym, num, camera, ground, skip=skip)
    game, sevp, bank, plan, frames = synth(a)
    evp, evt, r, notes = HE.build(num, number, dict(banks={num: bank}, sevp=sevp), name, fov=fov,
                                  ground_fix=False, prop_map=PROP_MAP, game=game)
    r.update(a['rep'])
    warnings = list(a['warnings'])
    for j, n in enumerate(frames):
        if n <= 0:
            warnings.append('2011 segment %d is empty (SYM has no %s take)' % (
                j, {1: 'stage', 2: 'ramp', 3: 'ringside', 4: 'in-ring'}.get(j, 'intro')))
    if any(len(c[1]) > 1 for (j, k), c in a['tracks'].items() if c[0] == 'prop'):
        warnings.append('an animated prop: hctp_entrance holds its first take for each segment (not cut at the intro)')
    notes = plan + a['notes'] + [n for n in notes if not n.startswith('object ')]
    return evp, evt, r, notes, warnings, a['dropped']


def _name(sym, num, name=None):
    return name or sym['names'].get(num) or 'SYM %d' % num


def list_entrances(src):
    """every SYM entrance with a motion bank -> [dict(number, name, segments, frames=[5], objects,
    props, has_pyro, has_movie, has_theme, in_2011)]"""
    sym = read_source(src)
    out = []
    for n in sorted(sym['script']):
        b = sym['banks'].get(n)
        if not b or len(b) <= 0x200:
            continue
        try:
            a = analyse(sym, n, light=True)
            frames = plan_of(a)[3]
        except Exception:                                     # (listed; a conversion reports why)
            frames = [0] * 5
        objs = sym['script'][n]['objs']
        out.append(dict(number=n, name=sym['names'].get(n, ''), segments=sum(1 for f in frames if f > 0),
                        frames=frames, objects=len(objs),
                        props=[o['model'] for o in objs if HE.kind_of(o) == 'prop'],
                        has_pyro=False, has_movie=False, has_theme=False,
                        in_2011=None))           # (SYM's own motions; 2011 never shipped them)
    return out


def convert(src, entrance, number, name, rep, camera=0, ground=True, skip=()):
    """-> (evp bytes, evt bytes); rep: dict with lists notes / warnings / dropped.
    ValueError for an unknown entrance.  camera: which of SYM's two cameras (0/1); ground: False
    keeps SYM's raised stage heights; skip: SYM take numbers to leave out (alternates)."""
    sym = read_source(src)
    if entrance not in sym['script'] or len(sym['banks'].get(entrance, b'')) <= 0x200:
        raise ValueError('%s has no entrance %d' % (GAME_NAME, entrance))
    evp, evt, r, notes, warnings, dropped = build(sym, entrance, number, _name(sym, entrance, name),
                                                  camera, ground, skip=skip)
    rep.setdefault('notes', []).extend(notes + ['%s: %d' % kv for kv in sorted(r.items())])
    rep.setdefault('warnings', []).extend(warnings)
    rep.setdefault('dropped', []).extend(dropped + ['%s: %d' % kv for kv in sorted(r.items()) if 'dropped' in kv[0]])
    return evp, evt


def self_check(evp, evt):
    """entrance_port.cmd_convert's checks: the EVP re-parses, every event block walks T11"""
    S.parse_evp(evp, '2011')
    b = Y.read_bank(evt)
    for e in b.entries:
        if not e['data']:
            continue
        for f, r in Y.parse_stream(e['data']).records():
            ev, _ = Y.split_record(r)
            if ev and not Y.parse_events(ev, b.T)[1]:
                raise ValueError('event block does not walk T11 in (%d,%d,%d)' % (e['id'], e['x'], e['y']))
    return b


# ============================================================================ command line

def _opt(a, k, d=None):
    return a[a.index(k) + 1] if k in a and a.index(k) + 1 < len(a) else d


def _skip(a):
    v = _opt(a, '--skip')
    return tuple(int(x) for x in v.split(',')) if v else ()


def cmd_list(a):
    for e in list_entrances(a[0]):
        print('%4d  %-24s %d segments  frames %-26s objects %d  props %s' % (
            e['number'], e['name'], e['segments'], ' '.join('%4d' % f for f in e['frames']), e['objects'],
            e['props']))


def cmd_dump(a):
    sym = read_source(a[0])
    num = int(a[1])
    for i, o in enumerate(sym['script'][num]['objs']):
        print('cast %d %-6s model %d' % (i, HE.kind_of(o), o['model']))
        for s in o['slots']:
            print('   take %d on %d track (%d,%d,%d) slot %s parent %d' % (
                s['slot'], s['on'], s['tid'], s['tx'], s['ty'], ' '.join('%.1f' % v for v in s['params']),
                s['parent']))
    an = analyse(sym, num, int(_opt(a, '--camera', 0)), '--no-ground' not in a, skip=_skip(a))
    for t in an['takes']:
        print('take %d: %4d frames, entrant (%.1f %.1f %.1f) -> (%.1f %.1f %.1f), yaw %.2f, y shift %.1f, %s' % (
            t['k'], t['frames'], *t['first'], *t['last'], t['yaw'], t['shift'],
            'same as take %d' % t['dup'] if t['dup'] is not None else ROLE_NAMES[t['role']]))
    print('\n'.join(synth(an)[3]))


def cmd_convert(a):
    sym = read_source(a[0])
    num, number, out = int(a[1]), int(a[2]), a[3]
    if not 100 <= number < 1000:
        raise SystemExit('the 2011 number must be 100..999')
    rep = dict(notes=[], warnings=[], dropped=[])
    name = _name(sym, num, _opt(a, '--name'))
    evp, evt = convert(a[0], num, number, name, rep, int(_opt(a, '--camera', 0)), '--no-ground' not in a,
                       _skip(a))
    b = self_check(evp, evt)
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, '%d_evp.bin' % number), 'wb').write(evp)
    open(os.path.join(out, '%d_evt.bin' % number), 'wb').write(evt)
    lines = ['SYM %d "%s" -> 2011 %d' % (num, name, number),
             'EVP %d bytes, EVT %d bytes, %d tracks (self-check passed)' % (len(evp), len(evt), len(b.entries))]
    lines += ['note: ' + x for x in rep['notes']]
    lines += ['warning: ' + x for x in rep['warnings']]
    lines += ['dropped: ' + x for x in rep['dropped']]
    if '--parts' in a:
        pd = os.path.join(out, 'parts')
        os.makedirs(pd, exist_ok=True)
        for n, pevp, pevt in S.cae_parts(evp, evt, number):
            open(os.path.join(pd, '%d_evp.bin' % n), 'wb').write(pevp)
            open(os.path.join(pd, '%d_evt.bin' % n), 'wb').write(pevt)
        lines.append('Create-An-Entrance parts written')
    open(os.path.join(out, 'report.txt'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    cmds = {'list': cmd_list, 'convert': cmd_convert, 'dump': cmd_dump}
    if len(sys.argv) < 3 or sys.argv[1] not in cmds:
        print(__doc__)
    else:
        cmds[sys.argv[1]](sys.argv[2:])
