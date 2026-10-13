"""WWE SmackDown! vs. Raw 2006 (PS2, SLES_536.76) entrance -> SvR 2011 entrance.

  python svr06_entrance.py list <iso or extract folder>
  python svr06_entrance.py convert <iso or extract folder> <2006 number> <2011 number> <out dir>
                           [--name TEXT] [--no-parts] [--pack]
        writes <out>/<number>_evp.bin + <number>_evt.bin, <out>/parts/ (Create-An-Entrance parts,
        10000 + 1000k + number), <out>/report.txt; --pack also writes <out>/pack (pack.txt +
        entrance/*.bin, a move pack as hctp_entrance.py --pack).

Module API (tools/entrance_port.py): list_entrances(src), convert(src, entrance, number, name, rep).
`src` is a discfs object (or a path for discfs.open); paths inside are the disc's own.

2006 side (docs/OTHER_GAMES_ENTRANCES.md, research in re_ps2_old.md):
- PAC/EVT.PAC: a DPAC with doubled group counts: groups at 0x800 {tag, u16 2*count, u16 base},
  rows {name[4], u16 sector, u16 size/0x100}, data at 0x4000 + sector*0x800. Group EVNT = one
  motion bank per entrance (named 4000..4500, 9999), EVTS = one script per entrance.
- Script: SvR 2007/2008's EVP (28-byte objects, 21 x 212-byte slots, segments at slots
  0/5/10/15/20, 30-byte name at 0x22, tail) little-endian, with the PS2 traps: 0x10 / 0x18 / 0x44
  and the tail's v / m are two u16 (high half first), bytes 0x40-0x41 are raw (name overflow) with
  the object count in the u16 at 0x42, the 8 type bytes are not swapped, slots are u16 slot,
  u8 on, u8 0, 51 u32, u16 parent, u16 0. Slot track keys are 2011's (object, x = slot, y).
- Bank: HCTP's CEKey bank (T[256], u32 n, n x {key, offset, frames, u2}), key = id << 16 | x << 8
  | y, camera and global-effect tracks carry id | 0x4000; frame stream as HCTP (0x60 + u8 size).
  Event sizes = SvR 2007's, = 2011's except ops 24 (2 vs 0), 65 (2 vs 3), 185 (1 vs 2).
  Character bodies (y0): HCTP's wide 21-bone table, 0x7d full keys with f32 roots, key-reduced
  (2007's are one key per frame); face / hand sub-tracks y1..3 use ops 0x77-0x79 (not 2011's).
  Cameras: already 2011's 29-byte mode-1 keys. Props: HCTP object records (0x7a -> 2011 0x75).
  Ropes ("lights"): 5-byte keys as 2011. Stage at floor level as 2011 (no ground fix).

Rules (2006 -> 2011):
- script: the SvR 2007 rules (svr08_entrance.convert_evp after extras -> generic 3902; type-9
  tail records dropped); belt 2021 (= 2007's 2029, World Heavyweight Title - Rolled) -> 5009;
  every object keeps exactly 21 slots (4077 has 23: the extra two and their tracks dropped).
- tracks: ids + 25000, 2011's size table and framing; character y0 re-encoded to 2011 bodies
  (hctp_entrance.conv_char_pose, no ground fix); y1..3 dropped; cameras copied; props re-encoded
  (hctp_entrance.conv_obj_pose); ropes copied; the global effect (pyro) object's tracks dropped
  (2006 particles are an older PS2 format); events of ops whose size differs (24, 65, 185) and
  op-193 face events 0x30-0x3F on y0 dropped (as svr08_entrance.convert_evt).
- no intro segment in 2006 (4007, 4037, 4040, 4101: the entrant's and camera's slot 0 off; every
  2011 entrance has one): a 1-frame intro from the stage segment's first pose and camera key (as
  2006's own 1-frame intros, e.g. 4003). A switched-on segment without an entrant track gets a
  pose-less one (not seen in the 93).
- tested offline (all 93 convert, re-parse and walk T11; Andre 4066 vs the svr07 path's 4109:
  same EVP size, same frames, roots within 0.4 units, bones within ~1 degree except the foot
  pitch 2006 does not store); not played in game yet.
"""
import collections
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ymks as Y  # noqa: E402
import hctp_motion as H  # noqa: E402
import hctp_entrance as EH  # noqa: E402
import svr08_entrance as S  # noqa: E402

PAC_PATH = 'PAC/EVT.PAC'
G_BANKS, G_SCRIPTS = 'EVNT', 'EVTS'
CAM_FLAG = 0x4000
NSLOTS = 21
SEGS = (0, 5, 10, 15, 20)
SKIP = {4500, 9999}                  # 4500 "test_belt" (one slot), 9999 = not an entrance
PROP_MAP = {2021: 5009, 2029: 5009}  # belt held by the entrant (2007 4109 has 2029 where 2006 4066 has 2021)
# 2006 props that 2011's evtobj.pac has under the same id (checked against Game Files pac/evt/evtobj.pac)
PROPS_IN_2011 = {2024, 2036, 2038, 2042, 2049, 2050, 2062, 2071, 2077, 2500, 2501, 5009}
STD_TAIL = [(1, 0, 100, [(2, 0, 999), (1, 0, 0), (1, 0, 0)]),
            (8, 0, 100, [(3, 0, 999), (3, 0, 0), (3, 0, 0)])]
# 2006 entrances that SvR 2007 (X360, entrance_port.py --game svr07) also has, by name:
# 2006 number -> (2007 number, same frame counts per segment = very likely the same motion)
IN_SVR07 = {
    4000: (4100, False), 4001: (4001, False), 4002: (4002, False), 4004: (4004, False),
    4005: (4005, False), 4006: (4006, False), 4007: (4007, False), 4008: (4008, False),
    4009: (4009, False), 4010: (4010, False), 4011: (4011, False), 4014: (4014, False),
    4017: (4017, False), 4018: (4018, False), 4024: (4024, False), 4026: (4026, False),
    4027: (4027, False), 4029: (4063, True), 4030: (4030, False), 4031: (4114, True),
    4034: (4034, False), 4035: (4107, False), 4037: (4037, False), 4039: (4039, False),
    4040: (4103, False), 4041: (4115, False), 4042: (4042, False), 4045: (4045, False),
    4046: (4104, False), 4047: (4105, False), 4056: (4056, True), 4060: (4060, False),
    4061: (4061, False), 4062: (4108, False), 4063: (4111, False), 4065: (4101, True),
    4066: (4109, True), 4067: (4067, True), 4069: (4069, False), 4077: (4077, False),
    4078: (4078, False), 4082: (4112, False), 4083: (4110, False), 4087: (4087, False),
    4139: (4180, False), 4150: (4150, False), 4153: (4153, False), 4154: (4154, False),
    4155: (4155, False), 4156: (4156, False), 4157: (4157, True), 4158: (4158, False),
    4159: (4159, False), 4160: (4160, False), 4170: (4100, False), 4250: (4251, False),
    4251: (4252, False)}


# ============================================================================ container

def dpac2(d):
    """SvR 2006 DPAC -> [(group, name, bytes)]"""
    if d[:4] != b'DPAC':
        raise ValueError('not a DPAC (%r)' % d[:4])
    out = []
    o = 0x800
    while o + 8 <= 0x4000 and d[o:o + 4] != bytes(4):
        tag = d[o:o + 4].decode('latin-1')
        n2 = struct.unpack_from('<H', d, o + 4)[0]
        o += 8
        for _ in range(n2 // 2):
            nm = d[o:o + 4].replace(b'\0', b'').decode('latin-1').strip()
            sec, sz = struct.unpack_from('<HH', d, o + 4)
            o += 8
            p = 0x4000 + sec * 0x800
            out.append((tag, nm, d[p:p + sz * 0x100]))
    return out


_cache = {}


def _open(src):
    if isinstance(src, str):
        import discfs
        src = discfs.open(src)
    return src


def read_source(src):
    """-> ({number: bank bytes}, {number: script bytes})"""
    src = _open(src)
    key = id(src)
    if key not in _cache:
        if not src.exists(PAC_PATH):
            raise ValueError('not a SmackDown! vs. Raw 2006 disc: no %s' % PAC_PATH)
        banks, scripts = {}, {}
        for g, n, b in dpac2(src.read(PAC_PATH)):
            if n.isdigit():
                if g == G_BANKS:
                    banks[int(n)] = b
                elif g == G_SCRIPTS:
                    scripts[int(n)] = b
        _cache[key] = (banks, scripts)
    return _cache[key]


# ============================================================================ script

def _p16(d, o):
    """a 360 u32 the PS2 build writes as two LE u16, high half first"""
    hi, lo = struct.unpack_from('<HH', d, o)
    return hi << 16 | lo


def _swap_slot(s):
    out = bytearray(s)
    struct.pack_into('>H', out, 0, struct.unpack_from('<H', s, 0)[0])     # (bytes 2, 3: u8 on, u8 0)
    for k in range(1, 52):
        struct.pack_into('>I', out, 4 * k, struct.unpack_from('<I', s, 4 * k)[0])
    struct.pack_into('>HH', out, 208, *struct.unpack_from('<HH', s, 208))
    return bytes(out)


def parse_script(d):
    """2006 EVTS script -> svr08_entrance.parse_evp's dict of the 2008 layout (slots big-endian)"""
    if d[:4] != b'POF0' or d[8:10] != b'VE':
        raise ValueError('not a 2006 entrance script')
    u32 = lambda o: struct.unpack_from('<I', d, o)[0]  # noqa: E731
    e = dict(num=struct.unpack_from('<H', d, 0x20)[0], name=d[0x22:0x40], f10=_p16(d, 0x10),
             f18=_p16(d, 0x18), f44=_p16(d, 0x44), objs=[], tail=None, game='2008',
             n_hi=int.from_bytes(d[0x40:0x42], 'big'))
    n = struct.unpack_from('<H', d, 0x42)[0]
    t = u32(0x48) + 8
    for i in range(n):
        q = t + 28 * i
        idx, model = struct.unpack_from('<HH', d, q)
        t0, t1 = struct.unpack_from('>II', d, q + 4)           # type bytes: the same order as 2011
        w4, w5, ns, bp = struct.unpack_from('<4I', d, q + 12)
        bp += 8
        nslots, lo = ns & 0xffff, ns >> 16
        slots = [_swap_slot(d[bp + S.SLOT * k:bp + S.SLOT * (k + 1)]) for k in range(nslots)]
        e['objs'].append(dict(idx=idx, model=model, t0=t0, t1=t1, w4=w4, w5=w5, nslots=nslots, lo=lo,
                              slots=slots))
    tp = u32(0x1C)
    if tp:
        tp += 8
        num, cnt = struct.unpack_from('<HH', d, tp)
        lp = u32(tp + 4) + 8
        recs = []
        for i in range(cnt):
            r = lp + 16 * i
            typ, z = struct.unpack_from('<HH', d, r)
            v, m, ip = _p16(d, r + 4), _p16(d, r + 8), u32(r + 12) + 8
            items = []
            for j in range(1 if typ == 9 else m):
                k = d[ip + 8 * j]
                low = int.from_bytes(d[ip + 8 * j + 1:ip + 8 * j + 4], 'big')
                vp = u32(ip + 8 * j + 4) + 8
                val = u32(vp) if k == 2 else struct.unpack_from('<H', d, vp)[0]
                items.append((k, low, val))
            recs.append((typ, z, v, items, m) if typ == 9 else (typ, z, v, items))
        e['tail'] = dict(num=num, recs=recs)
    return e


def script_name(e):
    return e['name'].split(b'\0')[0].decode('latin-1', 'replace').strip()


def _off_slot(j, obj):
    s = bytearray(S.SLOT)
    struct.pack_into('>HBBII', s, 0, j, 0, 0, S.ID_SHIFT + obj, j)
    struct.pack_into('>HH', s, S.SLOT - 4, S.NONE16, 0)
    return bytes(s)


def convert_script(e6, number, name, rep):
    """2006 script dict -> 2011 EVP dict"""
    lines = []
    e = dict(e6)
    e['objs'] = [dict(o) for o in e6['objs']]
    kinds = [EH.kind_of(o) for o in e['objs']]
    tag = number_is_team(e6['num'], script_name(e6))
    for i, o in enumerate(e['objs']):
        if i and kinds[i] == 'char' and o['model'] != S.GENERIC_CHAR_08:
            if tag and i in (4, 5) and o['model'] not in (202, 302, 402, 502, 602, 1802, 8900):
                rep['notes'].append('object %d: 2006 team member model %d -> generic (2011 fills team members '
                                    'in at run time, as Yuke\'s 772 "NWO Trio")' % (i, o['model']))
            o['model'] = S.GENERIC_CHAR_08
    e11 = S.convert_evp(e, number, prop_map=PROP_MAP, log=lines)
    if e11['tail'] and any(r[0] == 9 for r in e11['tail']['recs']):
        lines.append('tail: the type-9 record (a music cue, as SvR 2007) dropped')
        e11['tail']['recs'] = [r for r in e11['tail']['recs'] if r[0] != 9]
    if not e11['tail']:
        e11['tail'] = dict(num=number, recs=[tuple(r) for r in STD_TAIL])
        e11['f18'] = 0x10000
        lines.append('tail: none in 2006, the standard one written')
    longer = sorted(set(len(o['slots']) for o in e11['objs'] if len(o['slots']) > NSLOTS))
    if longer:
        rep['warnings'].append('%d slots per object in 2006: slots 21+ (and their tracks) dropped' % longer[-1])
    for i, o in enumerate(e11['objs']):
        sl = list(o['slots'][:NSLOTS])
        while len(sl) < NSLOTS:
            sl.append(_off_slot(len(sl), i))
        o['slots'] = sl
        o['nslots'] = NSLOTS
        if kinds[i] == 'prop' and o['model'] not in PROPS_IN_2011:
            rep['warnings'].append('object %d: prop %d is not in 2011 (it will not show)' % (i, o['model']))
    fill0 = False
    if e11['objs'] and kinds[0] == 'char' and not e11['objs'][0]['slots'][0][2] and e11['objs'][0]['slots'][5][2]:
        # no intro in 2006 (4007, 4037, 4040, 4101); 2011's own all have one: a 1-frame intro (as 2006's
        # 4003 / 4004 ...) from the stage segment's first pose and camera key
        fill0 = True
        for i, o in enumerate(e11['objs']):
            if i == 0 or kinds[i] == 'camera':
                s = bytearray(o['slots'][5])
                struct.pack_into('>H', s, 0, 0)
                struct.pack_into('>I', s, 8, 0)
                o['slots'][0] = bytes(s)
        rep['notes'].append('no intro segment in 2006: a 1-frame intro (the stage segment\'s first pose and camera) added')
    nm = (name or default_name(e6)).encode('latin-1', 'replace')[:29]
    e11['name'] = nm + bytes(30 - len(nm))
    rep['notes'] += [l for l in lines if not l.startswith('WARNING')]
    rep['warnings'] += [l for l in lines if l.startswith('WARNING')]
    return e11, kinds, fill0


def number_is_team(num, name):
    return 4200 <= num <= 4299 or name.upper().startswith('TAG_')


def default_name(e6):
    nm = script_name(e6).replace('_', ' ').strip()
    return re.sub(r'^TAG ', '', nm) or 'SvR06 %d' % e6['num']


# ============================================================================ tracks

def bad_ops(T6):
    return {op for op in range(256) if T6[op] and T6[op] != S.T11[op]}


def _filter_events(ev, T6, bad, kind, y, cnt):
    if not ev:
        return b''
    lst, ok = Y.parse_events(ev, T6)
    out = bytearray()
    for op, b in lst:
        if op is None:
            cnt['%s: event bytes past the size table dropped' % kind] += 1
        elif op in bad:
            cnt['%s: op %d dropped (2006 size %d, 2011 size %d)' % (kind, op, T6[op], S.T11[op])] += 1
        elif kind == 'char' and y == 0 and op == 193 and 0x30 <= b[1] <= 0x3f:
            cnt['char: op-193 face events 0x30-0x3F dropped from y0 (2011 keeps them on y1..3)'] += 1
        else:
            out += b
    return bytes(out)


def convert_track(kind, y, data, T6, bad, cnt):
    """one 2006 track -> [(frame, 2011 record)], frames in the stream"""
    recs = []
    nf = 0
    for f, r in H.records(data):
        nf = f + 1
        ev, pose = Y.split_record(r)
        ev2 = _filter_events(ev, T6, bad, kind, y, cnt)
        p2 = pose
        if len(pose) > 1:
            try:
                if kind == 'char':
                    p2 = EH.conv_char_pose(pose, False, cnt)
                elif kind == 'prop':
                    p2 = EH.conv_obj_pose(pose)
                elif kind == 'camera' and len(pose) != 29:
                    p2 = EH.conv_cam_pose(pose, 0.0, EH.CAM_FOV) if len(pose) == 13 else pose
                    cnt['camera: %d-byte key %s' % (len(pose), 'converted' if len(pose) == 13 else 'copied')] += 1
            except (ValueError, IndexError, struct.error):
                cnt['%s: undecodable pose dropped' % kind] += 1
                p2 = b''
        if not ev2 and not p2 and (ev or pose):
            continue
        recs.append((f, Y.join_record(ev2, p2)))
    return recs, max(nf, EH.frames_of(data))


def _intro_from_stage(bank, kinds):
    """1-frame segment-0 tracks: the entrant's and the cameras' state at frame 0 of segment 1"""
    st = bank.find(S.ID_SHIFT, 5, 0)
    if st and st['data']:
        ks = [k for k in Y.body_keys(st['data'], S.T11) if k['has_pose']]
        if ks:
            rec = Y.full_pose(0, ks[0]['root'], ks[0]['bones'])
            Y.insert_motion(bank, S.ID_SHIFT, 0, 0, Y.build_stream(Y.make_stream([(0, rec)], 1)), 1, S.RT_2011)
    for i, k in enumerate(kinds):
        if k != 'camera':
            continue
        for y in (0, 1):
            c = bank.find(S.ID_SHIFT + i, 5, y)
            if not c or not c['data']:
                continue
            poses = [Y.split_record(r)[1] for f, r in Y.parse_stream(c['data']).records()]
            poses = [p for p in poses if len(p) == 29]
            if poses:
                p = bytes([0]) + poses[0][1:]
                Y.insert_motion(bank, S.ID_SHIFT + i, 0, y, Y.build_stream(Y.make_stream([(0, p)], 1)), 1, S.RT_2011)


def convert_bank(raw, kinds, entrant_on, rep, fill0=False):
    """2006 EVNT bank -> 2011 YMKs bytes. entrant_on: segment slots (x) where object 0 is on;
    fill0: write a 1-frame intro (segment 0) from segment 1's first state."""
    T6, ents = H.read_bank(raw)
    bad = bad_ops(T6)
    cnt = collections.Counter()
    bank = Y.Bank(b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + S.T11, [])
    bank.lead = b''
    seen = set()
    seg_len = collections.Counter()
    for e in ents:
        if not e['frames'] or not e['data']:
            continue
        oi, x, y = e['id'] & 0x3fff, e['x'], e['y']
        kind = kinds[oi] if oi < len(kinds) else None
        if kind is None:
            cnt['tracks of no object dropped'] += 1
            continue
        if x >= NSLOTS:
            cnt['tracks of slots past 20 dropped'] += 1
            continue
        if kind == 'global':
            cnt['global effect (pyro) tracks dropped'] += 1
            continue
        if kind == 'char' and y != 0:
            cnt['source face / hand tracks (y1..3, 2006 ops 0x77-0x79) dropped (hands remade below)'] += 1
            continue
        if (oi, x, y) in seen:
            cnt['duplicate track keys dropped'] += 1
            continue
        seen.add((oi, x, y))
        tk = kind if kind in ('char', 'camera', 'prop') else 'rope'
        recs, n = convert_track(tk, y, e['data'], T6, bad, cnt)
        n = max(n, e['frames'])
        Y.insert_motion(bank, S.ID_SHIFT + oi, x, y, Y.build_stream(Y.make_stream(recs, n)), n, S.RT_2011)
        cnt['tracks %s' % tk] += 1
        seg_len[x] = max(seg_len[x], n)
    if fill0:
        _intro_from_stage(bank, kinds)
        seen.add((0, 0, 0))
    for x in entrant_on:
        if (0, x, 0) not in seen:
            n = seg_len.get(x) or 1
            Y.insert_motion(bank, S.ID_SHIFT, x, 0, Y.build_stream(Y.make_stream([], n)), n, S.RT_2011)
            rep['notes'].append('segment %d: the entrant has no 2006 track; a %d-frame pose-less one added' % (x // 5, n))
    # the entrant's fingers: 2006's y2 / y3 keys are 2011's finger values (0x78 / 0x79 joint-mask groups,
    # expanded to 2011's 5-byte sub-records: svr06_hands.py)
    import svr06_hands
    for x, hd in svr06_hands.hand_tracks(raw, obj=0, frames={x: seg_len[x] for x in seg_len}, fill0=fill0).items():
        if x >= NSLOTS:
            continue
        for y, data in hd.items():
            Y.insert_motion(bank, S.ID_SHIFT, x, y, data, Y.parse_stream(data).nframes(), S.RT_2011)
            cnt['hand tracks (y2/y3, from 2006 0x78/0x79 keys)'] += 1
    for k, v in sorted(cnt.items()):
        (rep['dropped'] if 'dropped' in k else rep['notes']).append('%s: %d' % (k, v))
    used = sorted(op for op in bad if any(('op %d dropped' % op) in k for k in cnt))
    if used:
        rep['warnings'].append('2006 events with another size in 2011 dropped: ops %s' % used)
    return Y.write_bank(bank)


# ============================================================================ API

def _segment_frames(raw):
    T, ents = H.read_bank(raw)
    fr = {}
    for x in ents:
        if x['id'] == 0 and x['y'] == 0 and x['x'] in SEGS and x['frames']:
            fr[x['x'] // 5] = x['frames']
    return fr, ents


def list_entrances(src, game_pac=None):
    """(game_pac: accepted for entrance_port's lister convention; unused)"""
    banks, scripts = read_source(src)
    out = []
    for n in sorted(scripts):
        if n in SKIP or n not in banks:
            continue
        try:
            e = parse_script(scripts[n])
            fr, ents = _segment_frames(banks[n])
        except Exception:
            continue
        kinds = [EH.kind_of(o) for o in e['objs']]
        glob = {i for i, k in enumerate(kinds) if k == 'global'}
        s7 = IN_SVR07.get(n)
        out.append(dict(number=n, name=script_name(e), segments=len(fr), frames=[fr.get(k, 0) for k in range(5)],
                        objects=len(e['objs']),
                        props=[PROP_MAP.get(o['model'], o['model']) for o, k in zip(e['objs'], kinds) if k == 'prop'],
                        has_pyro=any((x['id'] & 0x3fff) in glob and x['frames'] for x in ents),
                        has_movie=False, has_theme=False, in_2011=None,
                        in_svr07=s7[0] if s7 else None))
    return out


def convert(src, entrance, number, name, rep, *more, game_pac=None):
    """-> (2011 EVP bytes, 2011 EVT bytes); rep: dict with lists notes / warnings / dropped.
    Also takes entrance_port's CONVERTERS call (src, entrance, number, name, parts, rep, game_pac=);
    game_pac is unused. ValueError = no such entrance / not a 2006 disc."""
    if not isinstance(rep, dict) and more and isinstance(more[0], dict):
        rep = more[0]
    for k in ('notes', 'warnings', 'dropped'):
        rep.setdefault(k, [])
    banks, scripts = read_source(src)
    if entrance in SKIP or entrance not in scripts or entrance not in banks:
        raise ValueError('SmackDown! vs. Raw 2006 has no entrance %d' % entrance)
    e6 = parse_script(scripts[entrance])
    e11, kinds, fill0 = convert_script(e6, number, name, rep)
    on = [x for x in SEGS if e11['objs'][0]['slots'][x][2]] if kinds and kinds[0] == 'char' else []
    evt = convert_bank(banks[entrance], kinds, on, rep, fill0)
    s7 = IN_SVR07.get(entrance)
    if s7:
        rep['notes'].append('SvR 2007 (X360) has this entrance as %d%s: entrance_port.py --game svr07 keeps its '
                            'full key set and face / hand tracks' % (s7[0], ' (same frame counts)' if s7[1] else
                                                                       ' (re-cut: other frame counts)'))
    rep['notes'].append('not converted: pyro (2006 PS2 particles), face animation, titantron, theme')
    return S.build_evp(e11), evt


# ============================================================================ CLI

def check(evp, evt):
    """the checks entrance_port.cmd_convert makes: EVP re-parses, every event block walks T11"""
    S.parse_evp(evp, '2011')
    b = Y.read_bank(evt)
    for e in b.entries:
        if e['data']:
            for f, r in Y.parse_stream(e['data']).records():
                ev, _ = Y.split_record(r)
                if ev and not Y.parse_events(ev, b.T)[1]:
                    raise ValueError('event block does not walk T11 in (%d,%d,%d)' % (e['id'], e['x'], e['y']))
    return b


def cmd_list(a):
    for d in list_entrances(a[0]):
        s7 = ('  (SvR 2007: %d)' % d['in_svr07']) if d['in_svr07'] else ''
        print('%4d  %-26s %2d objects  frames %-26s props %s%s' % (
            d['number'], d['name'], d['objects'], ' '.join(str(f) for f in d['frames']), d['props'], s7))


def cmd_convert(a):
    def opt(k, dflt=None):
        return a[a.index(k) + 1] if k in a and a.index(k) + 1 < len(a) else dflt
    src, ent, number, out = a[0], int(a[1]), int(a[2]), a[3]
    if not 100 <= number < 1000:
        raise SystemExit('the 2011 number must be 100..999 (CAE parts are 10000 + 1000k + number)')
    rep = dict(notes=[], warnings=[], dropped=[])
    evp, evt = convert(src, ent, number, opt('--name'), rep)
    b = check(evp, evt)
    os.makedirs(out, exist_ok=True)
    files = [(number, evp, evt)]
    if '--no-parts' not in a:
        pd = os.path.join(out, 'parts')
        os.makedirs(pd, exist_ok=True)
        for n, pe, pt in S.cae_parts(evp, evt, number):
            open(os.path.join(pd, '%d_evp.bin' % n), 'wb').write(pe)
            open(os.path.join(pd, '%d_evt.bin' % n), 'wb').write(pt)
            files.append((n, pe, pt))
    open(os.path.join(out, '%d_evp.bin' % number), 'wb').write(evp)
    open(os.path.join(out, '%d_evt.bin' % number), 'wb').write(evt)
    lines = ['SvR 2006 %d -> 2011 %d' % (ent, number),
             'EVP %d bytes, EVT %d bytes, %d tracks' % (len(evp), len(evt), len(b.entries))]
    lines += ['warning: ' + w for w in rep['warnings']] + ['dropped: ' + w for w in rep['dropped']]
    lines += ['note: ' + w for w in rep['notes']]
    if '--pack' in a:
        lines.append('move pack: ' + EH.write_pack(out, number, files, 'SvR 2006 entrance %d as 2011 %d' % (ent, number)))
    open(os.path.join(out, 'report.txt'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
    elif sys.argv[1] == 'list':
        cmd_list(sys.argv[2:])
    elif sys.argv[1] == 'convert' and len(sys.argv) >= 6:
        cmd_convert(sys.argv[2:])
    else:
        print(__doc__)
