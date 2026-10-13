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
  * names: only in the ELF -> 'JBI <number>' unless --name.

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


def read_source(src):
    """src: a discfs disc / extract folder object, a path, or this function's result -> sym_entrance's
    source dict (banks, script, names)"""
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
    banks, script = {}, {}
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
    return dict(banks=banks, script=script, names={}, game=GAME)


def _name(src, num, name=None):
    return name or 'JBI %04d' % num


def list_entrances(src):
    return SYM.list_entrances(read_source(src))


def convert(src, entrance, number, name, rep, ground=True, skip=()):
    """-> (evp bytes, evt bytes) through sym_entrance.convert (camera 0: JBI has one)"""
    jbi = read_source(src)
    if entrance not in jbi['script']:
        raise ValueError('%s has no entrance %d' % (GAME_NAME, entrance))
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
