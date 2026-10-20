"""WWF SmackDown! Just Bring It (PS2, "JBI", SLES_504.77) entrance -> SvR 2011 entrance.

A front end on sym_entrance.py (which feeds hctp_entrance.py's back end; docs/OTHER_GAMES_ENTRANCES.md).
JBI's motion is Shut Your Mouth's: the same CEKey bank (256-byte T, directory, halfword framing),
the same 21-bone body records (hctp_motion.decode_body, H_TYPES_WIDE) and the same 13-byte camera
key [c = frames to the next key - 1][s16 pos x3][s16 rot x3] (+ pad).  What differs, and what this
module turns into sym_entrance's dicts:

  * container: EN3.PAC = DPAC with one group 'ENMT' {u16 0x8000 | count, u16 0}, rows {u16 BCD
        number, u16 size / 0x100}, data from 0x4000, each row sector aligned.  A row is a 'PAC '
        {u32 n, n x (u16 id, u24 offset, u24 size)}, data after the table: item 0 = motion bank,
        item 1 = the script (text).  Numbers 0000-0078 and 1000-1268 (348 entrances); 9999 is a
        shared bank (not an entrance).
  * track keys: id 9000 + take, x 0, y = cast index (characters); id 25384 + take, x 0, y 50
        (the camera).  1500 x 0..10 (4-frame static objects), 9010 / 9999 (1 record): dropped.
  * script: CRLF text
        line 0  ?            line 1  character count
        line 2  6 cast model ids (255 = none)
        line 3  6 x (u, a, b)                   lines 4-6: ? (event / flag values)
        line 7  take count, then per take 6 rows 'x,y,z,yaw' (one per cast index; units 1/100 of
                SYM's, yaw in degrees), a blank line between takes; a last 4-value line.
        Character roots are relative to their take row (as SYM's slots); the camera has no row: its
        keys are world positions (0007: 7-49 units from the entrant in every take; relative to the
        entrant's row it would be 120-260 off on the stage / ramp).
  * names: from the ELF's tables (team names for tag / trio entrances, else the cast's names), with
        the JBI number: 'Hardy Boyz & Lita (JBI 6)', 'The Rock (JBI 31)', 'Triple H & Kurt Angle (JBI
        scene 1001)'.
  * hands: JBI's own pose library (M.PAC MOT/PTS), mapped to 2011's poses by finger curl.

  python jbi_entrance.py list <iso or extract folder>
  python jbi_entrance.py convert <iso> <JBI number> <2011 number> <out dir> [--name TEXT] [--no-ground]
        [--skip TAKE,TAKE]
  python jbi_entrance.py dump <iso> <JBI number>

Module API (entrance_port.py): list_entrances(src) and convert(src, entrance, number, name, rep).
"""
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hctp_motion as H  # noqa: E402
import hctp_entrance as HE  # noqa: E402
import sym_entrance as SYM  # noqa: E402

GAME = 'jbi'
GAME_NAME = 'WWF SmackDown! Just Bring It'
EN3 = 'EN3.PAC'
CHAR_BASE, CAM_BASE, CAM_Y = 9000, 25384, 50
UNIT = 0.01                     # JBI slot units -> SYM / HCTP units
TYPE_CHAR = HE.S.TYPE_CHAR
TYPE_CAM = (0x00010000, 0)

_CACHE = {}


# The floor fix (sym_entrance.ground_shift) goes by the slot's z; a JBI take row can sit at the stage
# while its roots already walk the arena frame (0007 takes 3-4: row z 244, entrant from the ramp foot
# into the ring).  JBI slots carry 'at_z' = where the entrant starts (baked) and the fix goes by that.
_sym_ground_shift = SYM.ground_shift


def _ground_shift(slot):
    if slot is not None and 'at_z' in slot:
        return 0.0 if slot['at_z'] <= SYM.OUTSIDE_Z else SYM.FLOOR - slot['params'][1]
    return _sym_ground_shift(slot)


SYM.ground_shift = _ground_shift


def _first_root(data):
    for f, r in SYM.records_hw(data):
        ev, pose = SYM.Y.split_record(SYM.strip_pad('char', r))
        if len(pose) < 2:
            continue
        for s in H.decode_body(pose, H.H_TYPES_WIDE)['subs']:
            if s.get('root') is not None and s['op'] != 0x7b:
                return tuple(v * 0.01 for v in s['root'])
    return None


def en3_entries(d):
    """EN3.PAC -> {number: {item id: bytes}}"""
    if d[:4] != b'DPAC' or d[0x800:0x804] != b'ENMT':
        raise ValueError('not a Just Bring It EN3.PAC')
    n = struct.unpack_from('<H', d, 0x804)[0] & 0x7fff
    o = 0x4000
    out = {}
    for i in range(n):
        bcd, sz = struct.unpack_from('<HH', d, 0x808 + 4 * i)
        blob = d[o:o + sz * 0x100]
        o += (sz * 0x100 + 0x7ff) & ~0x7ff
        num = int('%x' % bcd)
        if blob[:4] != b'PAC ':
            continue
        cnt = struct.unpack_from('<I', blob, 4)[0]
        base = 8 + 8 * cnt
        items = {}
        for k in range(cnt):
            p = 8 + 8 * k
            iid = struct.unpack_from('<H', blob, p)[0]
            off = int.from_bytes(blob[p + 2:p + 5], 'little')
            ln = int.from_bytes(blob[p + 5:p + 8], 'little')
            items[iid] = blob[base + off:base + off + ln]
        out[num] = items
    return out


def _frame_hw(recs):
    """[(frame, record)] -> halfword frame stream (sym_entrance.records_hw's inverse)"""
    out = bytearray()
    cur = 0
    for f, r in recs:
        gap = f - cur
        while gap > 0:
            k = min(gap, 0x7f)
            out.append(k)
            gap -= k
        if len(r) % 2:
            r = r + b'\0'
        out.append(0x80 | (len(r) // 2))
        out += r
        cur = f + 1
    return bytes(out)


def _lerp_s16(a, b, t):
    d = (b - a + 0x8000) % 0x10000 - 0x8000          # (angles: the short way round)
    v = int(round(a + d * t))
    return (v + 0x8000) % 0x10000 - 0x8000


def camera_track(data):
    """JBI camera keys carry no event block: their first byte is the span c (frames to the next key -
    1), which reads as an event block when >= 0x80 (in 2011's keys too).  -> the same keys with an
    empty event block in front and spans >= 0x80 split by an interpolated key."""
    recs = SYM.records_hw(data)
    out = []
    for i, (f, r) in enumerate(recs):
        k = r[:13]
        while k[0] >= 0x80 and i + 1 < len(recs):
            nf, nr = recs[i + 1]
            t = 0x80 / (nf - f)
            a = struct.unpack_from('<6h', k, 1)
            b = struct.unpack_from('<6h', nr, 1)
            mid = [int(round(a[j] + (b[j] - a[j]) * t)) for j in range(3)]
            mid += [_lerp_s16(a[j], b[j], t) for j in range(3, 6)]
            out.append((f, b'\x80' + bytes([0x7f]) + k[1:]))
            f += 0x80
            k = bytes([k[0] - 0x80]) + struct.pack('<6h', *mid)
        if k[0] >= 0x80:
            k = bytes([0x7f]) + k[1:]
        out.append((f, b'\x80' + k))
    return _frame_hw(out)


def repack_bank(bank, fix):
    """CEKey bank with fix(key tuple, data) -> new data applied to every entry"""
    T, ents = H.read_bank(bank)
    data = bytearray()
    dirs = bytearray()
    seen = {}
    for e in ents:
        d = fix((e['id'], e['x'], e['y']), e['data'])
        if d not in seen:
            seen[d] = len(data)
            data += d
        dirs += struct.pack('<IIII', (e['id'] << 16) | (e['x'] << 8) | e['y'], seen[d], e['frames'], e['u2'])
    return T + struct.pack('<I', len(ents)) + bytes(dirs) + bytes(data)


def fix_bank(bank):
    return repack_bank(bank, lambda key, d: camera_track(d) if key[0] >= CAM_BASE and d else d)


def parse_script(text):
    """-> dict(chars, models[6], takes=[[ (x, y, z, yaw deg) x 6 ]])"""
    lines = text.decode('latin-1').split('\r\n')
    nchar = int(lines[1])
    models = [int(v) for v in lines[2].split(',') if v.strip()]
    ntakes = int(lines[7])
    rows = [l for l in lines[8:] if l.strip() and l.count(',') == 3 and not l.startswith('0000,')]
    takes = []
    for k in range(ntakes):
        tk = []
        for r in rows[6 * k:6 * k + 6]:
            x, y, z, yaw = (int(v) for v in r.split(','))
            tk.append((x, y, z, yaw))
        takes.append(tk)
    return dict(chars=nchar, models=models, takes=takes)


def to_sym(num, script, bank):
    """JBI script + bank -> sym_entrance's script dict (casts with per-take slots)"""
    T, ents = H.read_bank(bank)
    have = {(e['id'], e['x'], e['y']) for e in ents if e['data']}
    data = {(e['id'], e['x'], e['y']): e['data'] for e in ents}
    objs = []
    takes = script['takes']
    at_z = {}
    for k, tk in enumerate(takes):
        root = _first_root(data.get((CHAR_BASE + k, 0, 0), b''))
        if root is not None:
            x, y, z, yaw = tk[0]
            slot = dict(params=(x * UNIT, y * UNIT, z * UNIT, math.radians(yaw), 0.0))
            at_z[k] = SYM.to_world(root, slot)[2]
    for i in range(script['chars']):
        slots = []
        for k, tk in enumerate(takes):
            if (CHAR_BASE + k, 0, i) not in have:
                continue
            x, y, z, yaw = tk[i]
            slots.append(dict(slot=k, on=1, tid=CHAR_BASE + k, tx=0, ty=i,
                              params=(x * UNIT, y * UNIT, z * UNIT, math.radians(yaw), 0.0), parent=9999))
            if k in at_z:
                slots[-1]['at_z'] = at_z[k]
        objs.append(dict(idx=i, model=script['models'][i], t0=TYPE_CHAR[0], t1=TYPE_CHAR[1],
                         w4=0xffffffff, w5=0, nslots=len(slots), lo=0, slots=slots))
    cam = []
    for k, tk in enumerate(takes):
        if (CAM_BASE + k, 0, CAM_Y) not in have:
            continue
        cam.append(dict(slot=k, on=1, tid=CAM_BASE + k, tx=0, ty=CAM_Y,
                        params=(0.0, 0.0, 0.0, 0.0, 0.0), parent=9999))
    objs.append(dict(idx=len(objs), model=0, t0=TYPE_CAM[0], t1=TYPE_CAM[1], w4=0xffffffff, w5=0,
                     nslots=len(cam), lo=0, slots=cam))
    return dict(num=num, objs=objs)


# ---------------------------------------------------------------------------------------------- names
# The ELF keeps the names as plain tables (no code reading needed):
#   * the roster: 84-byte records, name first, indexed by the cast model id (0 The Rock, 1 Steve
#     Austin ... 45 Fred Durst), found as 'The Rock\0' with 'Steve Austin\0' 84 bytes on;
#   * the tag / trio entrance lists: 36-byte records {u16 entrance number, s16 -1, name[32]}
#     ('Tag Original1', 'Hardy Boyz & Lita', ...); their numbers are EN3's (checked by the cast:
#     Hardy Boyz 5 = models 14 + 15, Hardy Boyz & Lita 6 = 14 + 15 + 16).
# (The alphabetical 0-45 list beside them is the theme list; the singles motion list's ids are not
# EN3 numbers.)  Singles and story scenes are named after their cast.

def _cstr(b):
    return b.split(b'\0')[0].decode('latin-1')


def elf_names(elf):
    """-> (roster {model id: name}, teams {entrance number: name}); empty when not found"""
    roster, teams = {}, {}
    p = elf.find(b'The Rock\0')
    while p >= 0 and elf[p + 84:p + 97] != b'Steve Austin\0':
        p = elf.find(b'The Rock\0', p + 1)
    if p >= 0:
        for i in range(64):
            nm = _cstr(elf[p + 84 * i:p + 84 * i + 32])
            if len(nm) < 2 or not all(32 <= ord(c) < 127 for c in nm):
                break
            roster[i] = nm
    for anchor in (b'Tag Original1\0', b'Trio Original1\0'):
        p = elf.find(anchor)
        if p < 4:
            continue
        o = p - 4
        while o >= 36 and struct.unpack_from('<h', elf, o - 36 + 2)[0] == -1 and _cstr(elf[o - 32:o])[:1].isalpha():
            o -= 36
        while struct.unpack_from('<h', elf, o + 2)[0] == -1 and _cstr(elf[o + 4:o + 36])[:1].isalpha():
            num = struct.unpack_from('<H', elf, o)[0]
            if num < 1000:
                teams[num] = _cstr(elf[o + 4:o + 36])
            o += 36
    return roster, teams


def entrance_name(num, script, roster, teams):
    cast = [roster[m] for m in script['models'][:script['chars']] if m in roster]
    if num in teams:
        t = teams[num]
        if cast and ('Original' in t or 'Champion' in t):     # (generic: say who)
            t += ': ' + ' & '.join(cast)
        return '%s (JBI %d)' % (t, num)
    if not cast:
        return 'JBI %04d' % num
    who = ' & '.join(cast[:3]) + (' +%d' % (len(cast) - 3) if len(cast) > 3 else '')
    return '%s (JBI %s%d)' % (who, 'scene ' if num >= 1000 else '', num)


# ---------------------------------------------------------------------------------------------- hands
# JBI's op 16 hand events (`10 ss iiii`, ss 1 left / 2 right) name poses of JBI's own library -
# M.PAC MOT/PTS: ids 0-170 left / 200-370 right, one 60-byte record: per finger (thumb .. pinky) a
# base joint [k][0][s16 x y z] and a bend [k][0][s16], k 0x3b-0x44 left / 0x45-0x4e right.  The ids
# do NOT mean 2011's poses (23 shared ids, no likeness), so every id is mapped by its finger angles:
# per finger the curl (fingers: z of every joint; thumb: y of every joint + z of the base), scaled per
# library and finger to 0 = straight .. 1 = the library's 95th percentile, then the nearest 2011 pose
# of the same hand (thumb weighted 0.5).  On 2011's own shapes the curl reads fist ~ 1 1 1 1 1, open
# ~ 0, point 0 on the index, V index + middle, horns index + pinky.

_R = 2 * math.pi / 65536
HAND_W = (0.5, 1, 1, 1, 1)


def jbi_hand_library(mpac):
    """M.PAC -> {pose id: [finger][joint] (x, y, z) radians}, 5 fingers x 2 joints"""
    bank = next((b for g, n, b in SYM.dpac(mpac) if (g, n) == ('MOT', 'PTS')), None)
    out = {}
    if bank is None:
        return out
    _T, ents = H.read_bank(bank)
    for e in ents:
        if e['id'] >= 400 or not e['data'] or e['id'] in out:
            continue
        r = SYM.records_hw(e['data'])[0][1]
        if len(r) != 60:
            continue
        js, p = [], 0
        while p < len(r):
            j = (r[p] - 0x3b) if r[p] < 0x45 else (r[p] - 0x45)
            if j % 2 == 0:
                v = struct.unpack_from('<3h', r, p + 2)
                p += 8
            else:
                v = (0, 0, struct.unpack_from('<h', r, p + 2)[0])
                p += 4
            js.append(tuple(x * _R for x in v))
        out[e['id']] = [js[2 * f:2 * f + 2] for f in range(5)]
    return out


def _curls(P):
    raw = {}
    for i, p in P.items():
        raw[i] = [sum(j[1] for j in p[0]) + p[0][0][2]] + [sum(j[2] for j in p[f]) for f in range(1, 5)]
    scale = []
    for f in range(5):
        vs = [r[f] for r in raw.values()]
        sgn = 1 if sum(vs) >= 0 else -1
        hi = sorted(sgn * v for v in vs)[int(0.95 * (len(vs) - 1))] or 1.0
        scale.append(sgn / hi)
    return {i: [max(-0.2, min(1.2, r[f] * scale[f])) for f in range(5)] for i, r in raw.items()}


def hand_map(jlib, lib):
    """JBI pose id -> nearest 2011 pose id (same hand).  lib: hand_tracks.library()"""
    lib11 = {i: [[tuple(v * 256 * _R for v in s[3 * f + j][1:]) for j in range(3)] for f in range(5)]
             for i, s in lib.items() if i < 1000}
    out = {}
    for lo, hi in ((0, 200), (200, 1000)):
        a = _curls({i: v for i, v in jlib.items() if lo <= i < hi})
        b = _curls({i: v for i, v in lib11.items() if lo <= i < hi})
        if not a or not b:
            continue
        for i, v in a.items():
            out[i] = min(b, key=lambda t: sum(w * abs(x - y) for w, x, y in zip(HAND_W, v, b[t])))
    return out


class _NoPose(dict):
    """every id -> 0xFFFF (no 2011 pose: hand_tracks skips it)"""
    def __contains__(self, k):
        return True

    def __getitem__(self, k):
        return 0xFFFF


def remap_hands(bank, hmap):
    """rewrite op 16 hand ids of the character tracks through hmap -> (bank, events mapped)"""
    T, _ = H.read_bank(bank)
    count = [0]

    def fix(key, d):
        if not (CHAR_BASE <= key[0] < CHAR_BASE + 100) or not d:
            return d
        recs = []
        for f, r in SYM.records_hw(d):
            ev, pose = SYM.Y.split_record(r)
            if ev:
                lst, ok = SYM.Y.parse_events(ev, T)
                if ok:
                    nev = bytearray()
                    for op, bb in lst:
                        if op == 16 and len(bb) >= 4 and bb[1] in (1, 2):
                            pid = struct.unpack_from('<H', bb, 2)[0]
                            if pid in hmap:
                                bb = bb[:2] + struct.pack('<H', hmap[pid]) + bb[4:]
                                count[0] += 1
                        nev += bb
                    if len(nev) == len(ev):
                        r = SYM.Y.join_record(bytes(nev), pose)
            recs.append((f, r))
        return _frame_hw(recs)
    return repack_bank(bank, fix), count[0]


def read_source(src):
    """src: a discfs disc / extract folder object, a path, or this function's result -> sym_entrance's
    source dict (banks, script, names, hands)"""
    if isinstance(src, dict) and 'script' in src:
        return src
    if isinstance(src, str):
        key = ('path', os.path.abspath(src))
        if key in _CACHE:
            return _CACHE[key]
        res = read_source(SYM._open(src))
        _CACHE[key] = res
        return res
    if not src.exists(EN3):
        raise ValueError('not a Just Bring It disc: no %s' % EN3)
    roster, teams = {}, {}
    elf = next((n for n in ('SLES_504.77', 'SLUS_203.16', 'SLUS_202.69', 'SLUS_201.22', 'SLPM_650.84') if src.exists(n)), None)
    if elf:
        roster, teams = elf_names(src.read(elf))
    hands = jbi_hand_library(src.read('M.PAC')) if src.exists('M.PAC') else {}
    banks, script, names = {}, {}, {}
    for num, items in en3_entries(src.read(EN3)).items():
        if 0 not in items or 1 not in items or len(items[0]) <= 0x200:
            continue
        try:
            sc = parse_script(items[1])
            bank = fix_bank(items[0])
            script[num] = to_sym(num, sc, bank)
        except (ValueError, IndexError, struct.error):
            continue
        banks[num] = bank
        names[num] = entrance_name(num, sc, roster, teams)
    return dict(banks=banks, script=script, names=names, game=GAME, hands=hands)


def _name(src, num, name=None):
    return name or src.get('names', {}).get(num) or 'JBI %04d' % num


def list_entrances(src):
    return SYM.list_entrances(read_source(src))


def convert(src, entrance, number, name, rep, ground=True, skip=()):
    """-> (evp bytes, evt bytes) through sym_entrance.convert (camera 0: JBI has one)"""
    jbi = read_source(src)
    if entrance not in jbi['script']:
        raise ValueError('%s has no entrance %d' % (GAME_NAME, entrance))
    lib = HE.DEFAULT_HAND_LIB
    if lib and not jbi.get('hands'):              # (a lone EN3.PAC: no M.PAC - JBI ids would pick wrong shapes)
        bank, n = remap_hands(jbi['banks'][entrance], _NoPose())
        banks = dict(jbi['banks'])
        banks[entrance] = bank
        jbi = dict(jbi, banks=banks)
        rep.setdefault('notes', []).append('hands: no JBI pose library (M.PAC): %d hand events left out, the '
                                           'fingers keep the ring pose' % n)
    elif lib and jbi.get('hands'):
        hmap = hand_map(jbi['hands'], lib)
        bank, n = remap_hands(jbi['banks'][entrance], hmap)
        banks = dict(jbi['banks'])
        banks[entrance] = bank
        jbi = dict(jbi, banks=banks)
        rep.setdefault('notes', []).append('hands: %d JBI hand events mapped to the nearest 2011 pose by '
                                           'finger curl (M.PAC MOT/PTS)' % n)
    return SYM.convert(jbi, entrance, number, _name(jbi, entrance, name), rep, 0, ground, skip)


def main(a):
    if not a or a[0] not in ('list', 'convert', 'dump'):
        raise SystemExit(__doc__)
    cmd, a = a[0], a[1:]
    if cmd == 'list':
        for e in list_entrances(a[0]):
            print('%4d  %d segments  frames %-26s objects %d' % (
                e['number'], e['segments'], ' '.join('%4d' % f for f in e['frames']), e['objects']))
        return
    jbi = read_source(a[0])
    if cmd == 'dump':
        SYM.cmd_dump([jbi] + a[1:])
        return
    num, number, out = int(a[1]), int(a[2]), a[3]
    if not 100 <= number < 1000:
        raise SystemExit('the 2011 number must be 100..999')
    rep = dict(notes=[], warnings=[], dropped=[])
    name = _name(jbi, num, SYM._opt(a, '--name'))
    evp, evt = convert(jbi, num, number, name, rep, '--no-ground' not in a, SYM._skip(a))
    b = SYM.self_check(evp, evt)
    os.makedirs(out, exist_ok=True)
    open(os.path.join(out, '%d_evp.bin' % number), 'wb').write(evp)
    open(os.path.join(out, '%d_evt.bin' % number), 'wb').write(evt)
    lines = ['JBI %d "%s" -> 2011 %d' % (num, name, number),
             'EVP %d bytes, EVT %d bytes, %d tracks (self-check passed)' % (len(evp), len(evt), len(b.entries))]
    lines += ['note: ' + x for x in rep['notes']] + ['warning: ' + x for x in rep['warnings']]
    lines += ['dropped: ' + x for x in rep['dropped']]
    open(os.path.join(out, 'report.txt'), 'w', encoding='utf-8').write('\n'.join(lines) + '\n')
    print('\n'.join(lines))


if __name__ == '__main__':
    main(sys.argv[1:])
