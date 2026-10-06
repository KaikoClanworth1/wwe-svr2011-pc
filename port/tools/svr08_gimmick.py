"""SvR 2008 Parking Lot cars (gm.pac GMGA/0056) -> a replacement for SvR 2011's
parking gimmick package (gm.pac GMGB/78PK), to go with svr08_stage.py's bg78.

    python tools/svr08_gimmick.py <2008 gm.pac> <2008 bg56.pac> <2011 gm.pac> <out gm.pac>
        [--report] [--budget <bytes>] [--all-motions] [--texture-halve N] [--png <file.png>]
        [--as-new-entry <NAME>] [--keep-rotation] [--rotate 0|180] [--spread k] [--elements clean|nohot|scenery|all|t0only|t23only|onecar|2011hdr]
        [--motions-2011 common|none] [--hotspots-2011]
        [--pac-out <file.pac> [--entry <NAME>]] [--offset x,z]

Both games use the same gimmick package ("GMPD"): 0 record index {u16 id,
u16 type, u16 words, u16 offset}, 1 "GMPD" {count, pool words}, 2 the word
pool, 3 PACH{10000: locators}, 4 PACH{models (+ textures)}, 5 a motion bank.
2011 adds a texture bundle as 4/0, wraps the bank as "YMKs", and puts the
package in a PACH {0: package, 1: particles}. Yuke's own carry-over of the
2008 chamber package (GMGA/0020) shows the rest: record index, header and
locators unchanged, motion fields as full keys (2008 201 -> 2011 12201).

What the conversion does:
  * pool (2): object ids 1000-1017 -> 1040-1057 (the parking show range
    1040-1069; break parts 1500-1513 stay), motion numbers in T20 word 3, T21
    word 2, T22 word 2 -> 12000 + n (T29 already holds a full key).
  * element table (0) and its count in the GMPD header (1), --elements:
      clean (default): what 2011 can run. The factory (sub_824B8AB8, switch
        on type-1) builds every type 20-30, but each type's code reads a fixed
        number of words (traced in the recompiled methods): 20 reads up to
        word 11 (2008: 13 words), 21 up to 4 (8), 22 word 0.. (4), 23 up to 6
        (7), 27 up to 4 (6), 28 up to 2 (4), 29 up to 2 (5) - 2008 layouts fit.
        24 reads 8 words (2008 has 5) and 25 reads 7 (2008 has 4; its prop id
        11532/11533 is not in 2011's prop table: NULL write in sub_824BB4E0),
        so they are dropped. Also dropped: 27 and 28 (2011's packages never
        use them; meaning unknown), records naming a motion the bank lacks
        (T22 id 6 -> 12106), records naming an object without a model (1013),
        and records whose referenced record was dropped (T28 -> 104/41,
        T29 -> 105, T23 word 2 -> 110). Kept: 20 (car hot spots; 2011 shows
        their prompt), 21, 22, 23, and the data records of type 0.
      nohot: only the objects (23) and data records (0): cars and props, no
        interactions. scenery: nohot without the break parts (objects
        1500-1599). all: every 2008 record (crashes 2011: type 25).
      Bisection sets (from nohot): t0only (only the two type-0 records),
      t23only (only the objects), onecar (the Porsche object, record 42, plus
      type 0), 2011hdr (the objects with their words 1-3 set to 0 and word 6
      2 -> 1, the values every shipped 2011 object uses).
    In every mode but 'all', type-0 records are written the 2011 way, as
    placeholders (id 0, one word): 2008's 110/111 have locators, so 2011
    builds live elements from them and the match freezes.
    Locators belong to records by id (locator index = record id, as in 2011),
    so they stay as they are.
  * locators (3): moved by the same offset as the stage (2008 enclosure
    centre -> parking fight-box centre). Their y rotation is negated: 2011
    turns objects the other way (in a first test the 2008 cars stood inside
    the enclosure; 2011's own objects line up with their collision only the
    negated way, and the 2008 cars line up with the 2008 enclosure only the
    plain way). --keep-rotation leaves it.
  * models (4): ids as above, local space (placed by locators) so unchanged;
    texture name ent_ref00 -> ar_ref. 4/0 = a named texture bundle with every
    texture the models name, taken from bg56 (ar_ref from 2011's 78PK).
    The 2008 4/0 (8 unnamed DDS) and 6 (an empty bundle) are not carried.
  * motions (5): wrapped as YMKs. Only the 2008 motions the kept records name
    are kept, plus the ones they jump to (event ops 49/50/51); --all-motions
    keeps all 45. With --elements scenery no 2008 motion is left.
  * models (4) that no kept record names (the break parts in scenery) are
    left out, and so are textures no kept model names.
  * --texture-halve N (default 1): every texture loses its top mip N times
    (a quarter of the bytes per step). The game failed to allocate an 11 MB
    buffer around the gimmick load of a 10.5 MB package; keep it near
    78PK's 3.4 MB (the report gives the unpacked size).
  * --motions-2011 common (default): 2011's common backstage motions (keys
    12200-12231, which every shipped 2011 backstage package carries but no
    record names: the game plays them itself, e.g. moves against walls) are
    copied from 78PK's bank into ours, with 78PK's end entry (32767) and
    78PK's event-size table (a superset of 2008's: the only differing op,
    151, is unused by the 2008 bank). Without them the wrestlers freeze when
    the game asks for one of those motions. 'none' leaves them out.
  * --hotspots-2011: each 2008 car the size of 2011's cars (the Porsche, the
    Range Rover, the hearse) gets 2011 78PK car A's front hot spot (type 33,
    motion 12306, follow-ups 12300/12303), linked to the 2008 car body,
    placed 3 units inside the car's front (as 78PK's spot sits 3 units inside
    its car's end) and facing the same way as the car; 78PK's car motions
    12300-12307 are copied into the bank. Only the front: the cars stand
    outside the enclosure, so side and rear spots could never be reached.
  * particles: 2011's 78PK/1 as is.
  * --budget <bytes>: halves the largest textures (top mip) until the stored
    package is no bigger (default: no shrinking; the report gives the sizes).
Writes a whole gm.pac with only GMGB/78PK replaced, or with
--as-new-entry NAME (e.g. 78P8) a gm.pac that keeps 78PK and every other
entry byte for byte and gains GMGB/NAME (appended to the GMGB group).
--pac-out <file.pac> also writes a small EPAC holding only GMGB/<--entry,
default 78P8> (gm.pac's 0x800 header and trailer; +4 = table size), for the
overlay pac list (the group must stay GMGB: sub_824B8398 unwraps the package
only for that group). The <out gm.pac> argument may be '-' to skip it.
--spread k: the same as svr08_stage.py's: every locator moves with its
nearest vehicle by (k-1)(W-c) (W the wall point nearest that vehicle's
body, c the enclosure centre), before the turn and the offset.
--offset x,z and --rotate 0|180: the same as svr08_stage.py's (use the same
values for both). A half turn moves locators to (-x, -z) and adds pi to their
y rotation.
--png draws the cars' footprints as 2011 turns them, with the enclosure,
and the report says how much of each car lies inside the enclosure.
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svrfmt as f  # noqa: E402
import jboy  # noqa: E402
import arena_tool as at  # noqa: E402
import svr08_stage as st  # noqa: E402

OBJ_SHIFT = 40                            # 1000-1017 -> 1040-1057
MOTION_FIELDS = {20: (3,), 21: (2,), 22: (2,)}
REFLECTION_MAP = {'ent_ref00': 'ar_ref'}


def unpack(b):
    return f.bpe_decode(b) if b[:4] == b'BPE ' else b


def fval(w):
    return struct.unpack('<f', struct.pack('<I', w))[0]


def fword(v):
    return struct.unpack('<I', struct.pack('<f', v))[0]


def is_float(w):
    return 0x30000000 < (w & 0x7FFFFFFF) < 0x50000000


def find(path, typ, name):
    h, g, t = f.epac_read(open(path, 'rb').read())
    for gi, (ty, ents) in enumerate(g):
        for ei, (n, b) in enumerate(ents):
            if ty == typ and n == name:
                return (h, g, t), (gi, ei), b
    raise SystemExit(f'{path}: no {typ}/{name}')


# ------------------------------------------------------------ records

def records(tab):
    return [struct.unpack_from('<4H', tab, o) for o in range(0, len(tab) - 7, 8)]


def convert_pool(tab, pool, objs):
    """-> (new pool, motion keys named, log lines)"""
    W = list(struct.unpack(f'<{len(pool) // 4}I', pool))
    keys, log = set(), []
    for rid, typ, n, off in records(tab):
        if not typ:
            continue
        for k in range(n):
            w = W[off + k]
            if not is_float(w):
                continue
            v = fval(w)
            if k in MOTION_FIELDS.get(typ, ()) and v < 1000:
                W[off + k] = fword(12000 + v)
                keys.add(int(12000 + v))
            elif typ == 29 and k == 4:
                keys.add(int(v))
            elif int(v) == v and int(v) in objs:
                W[off + k] = fword(v + OBJ_SHIFT)
    return struct.pack(f'<{len(W)}I', *W), keys, log


# ------------------------------------------------------------ element table

DROP_TYPES = {24: 'reads 8 words, 2008 has 5', 25: 'reads 7 words, 2008 has 4; prop 115xx not in 2011',
              27: '2011 never uses it', 28: '2011 never uses it'}
REF_FIELDS = {28: (1, 2), 29: (1,), 23: (2,)}      # words that name another record by id


def clean_elements(tab, pool, model_ids, bank_keys, mode):
    """-> (new index bytes, [(id, type, 'kept'/'dropped', why)])"""
    W = struct.unpack(f'<{len(pool) // 4}I', pool)
    recs = records(tab)

    def word(off, k):
        return fval(W[off + k]) if is_float(W[off + k]) else None

    why = {}
    for rid, typ, n, off in recs:
        if not typ and mode == 't23only':
            why[rid] = 'bisection: type 23 only'
            continue
        if mode == 'all' or not typ:
            continue
        if mode in ('nohot', '2011hdr', 'scenery') and typ != 23:
            why[rid] = 'interaction (nohot)'
            continue
        if mode == 'scenery' and is_float(W[off]) and 1500 <= fval(W[off]) < 1600:
            why[rid] = 'break part (scenery)'
            continue
        if mode == 't0only' and typ != 0:
            why[rid] = 'bisection: type 0 only'
            continue
        if mode == 't23only' and typ != 23:
            why[rid] = 'bisection: type 23 only'
            continue
        if mode == 'onecar' and not (typ == 23 and rid == 42):
            why[rid] = 'bisection: the Porsche only'
            continue
        if typ in DROP_TYPES:
            why[rid] = DROP_TYPES[typ]
            continue
        for k in MOTION_FIELDS.get(typ, ()):
            v = word(off, k)
            if v is not None and int(v) not in bank_keys:
                why[rid] = f'motion {int(v)} not in the bank'
        for k in range(n):
            v = word(off, k)
            if (v is not None and int(v) == v and 1000 <= v < 1600 and int(v) not in model_ids
                    and int(v) - OBJ_SHIFT not in model_ids and not (typ == 23 and k == 2)):
                why[rid] = f'object {int(v)} has no model'
    changed = mode not in ('t23only', '2011hdr')   # (bisection: keep every object)
    while changed:                              # drop what names a dropped record
        changed = False
        for rid, typ, n, off in recs:
            if rid in why or not typ:
                continue
            for k in REF_FIELDS.get(typ, ()):
                v = word(off, k)
                if v is not None and int(v) in why:
                    why[rid] = f'names dropped record {int(v)}'
                    changed = True
    out, report = bytearray(), []
    for rid, typ, n, off in recs:
        if rid in why:
            report.append((rid, typ, 'dropped', why[rid]))
        elif not typ and mode != 'all':
            # 2011 writes type 0 only as placeholders: id 0, one word. The game
            # builds an element for every record a locator names (sub_824B6D78
            # -> sub_824BA1F8 -> factory), so 2008's type-0 records 110/111
            # (they have locators) became live generic elements and the match
            # froze (bisection: t0only locks, 2011-style placeholders don't).
            out += struct.pack('<4H', 0, 0, 1, off)
            report.append((rid, typ, 'kept as 2011 placeholder (id 0, 1 word)', ''))
        else:
            out += struct.pack('<4H', rid, typ, n, off)
            report.append((rid, typ, 'kept', ''))
    return bytes(out), report


def normalize_objects(tab, pool):
    """Type-23 words 1-3 -> 0 and word 6: 2 -> 1 (the only values shipped 2011
    objects use; 2008 sets 1/110/1 on break parts and 2 on limo/hearse)."""
    W = list(struct.unpack(f'<{len(pool) // 4}I', pool))
    changed = []
    for rid, typ, n, off in records(tab):
        if typ != 23:
            continue
        before = W[off:off + n]
        for k in (1, 2, 3):
            W[off + k] = 0
        if is_float(W[off + 6]) and fval(W[off + 6]) == 2.0:
            W[off + 6] = fword(1.0)
        if W[off:off + n] != before:
            changed.append(rid)
    return struct.pack(f'<{len(W)}I', *W), changed


# ------------------------------------------------------------ placement check

def enclosure(bg56, dx, dz, rotate=0, spread=1.0):
    """Convex hull (x, z) of bg56's collision corners, spread, turned and moved."""
    _, _, _, _, e56 = st.stage(bg56)
    d56 = {i: unpack(b) for i, b in e56}
    sg = -1 if rotate == 180 else 1
    C = [c for r in st.hmd_read(d56[996]) for c in st.tri_corners(r)]
    P = np.array(C)
    pv = ((P[:, 0].min() + P[:, 0].max()) / 2, (P[:, 2].min() + P[:, 2].max()) / 2)
    C = [st.pivot_scale(c, spread, pv) for c in C]
    pts = sorted({(round(sg * c[0] + dx, 2), round(sg * c[2] + dz, 2)) for c in C})

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])
    lo, hi = [], []
    for q in pts:
        while len(lo) >= 2 and cross(lo[-2], lo[-1], q) <= 0:
            lo.pop()
        lo.append(q)
    for q in reversed(pts):
        while len(hi) >= 2 and cross(hi[-2], hi[-1], q) <= 0:
            hi.pop()
        hi.append(q)
    return lo[:-1] + hi[:-1]


def inside(hull, x, z):
    n = len(hull)
    for i in range(n):
        (ax, az), (bx, bz) = hull[i], hull[(i + 1) % n]
        if (bx - ax) * (z - az) - (bz - az) * (x - ax) < 0:
            return False
    return True


def footprints(tab, pool, loc, models, sign=1.0):
    """[(model id, name, corners)] for the car bodies; the stored y rotation
    is applied the 2008 way (sign 1) or the 2011 way (-1)."""
    W = struct.unpack(f'<{len(pool) // 4}I', pool)
    n = struct.unpack_from('<I', loc, 0)[0]
    by_id = {struct.unpack_from('<I', loc, 16 + 32 * k + 12)[0]: struct.unpack_from('<3fI3f', loc, 16 + 32 * k)
             for k in range(n)}
    out = []
    for rid, typ, nw, off in records(tab):
        if typ != 23 or rid not in by_id or not is_float(W[off]):
            continue
        mid = int(fval(W[off])) - OBJ_SHIFT
        m = models.get(mid)
        if not m or not m.name.startswith(('car_', 'n1840')):
            continue
        x, y, z, _, rx, ry, rz = by_id[rid]
        V = np.array([v[:3] for s in m.meshes for v in s.verts])
        x0, x1, z0, z1 = V[:, 0].min(), V[:, 0].max(), V[:, 2].min(), V[:, 2].max()
        a = ry * sign
        c, s_ = np.cos(a), np.sin(a)
        corners = [(x + px * c + pz * s_, z - px * s_ + pz * c) for px, pz in ((x0, z0), (x1, z0), (x1, z1), (x0, z1))]
        out.append((mid, m.name, corners))
    return out


def share_inside(hull, corners, steps=12):
    (ax, az), (bx, bz), _, (dx_, dz_) = corners
    hit = tot = 0
    for i in range(steps + 1):
        for j in range(steps + 1):
            u, v = i / steps, j / steps
            x = ax + (bx - ax) * u + (dx_ - ax) * v
            z = az + (bz - az) * u + (dz_ - az) * v
            tot += 1
            hit += inside(hull, x, z)
    return hit / tot


# ------------------------------------------------------------ motions

def bank_parse(d):
    base = 0x10 if d[:4] == b'YMKs' else 0
    T = d[base:base + 256]
    n = struct.unpack_from('<I', d, base + 0x100)[0]
    ents = [list(struct.unpack_from('<BBHIII', d, base + 0x104 + 16 * i)) for i in range(n)]
    data = d[base + 0x104 + 16 * n:]
    return T, ents, data


def motion_jumps(T, data, off, frames):
    """Motion ids the frames jump to (event ops 49/50/51)."""
    out, p, nf = set(), off, 0
    while nf < frames and p < len(data):
        c = data[p]
        if c < 0x60:
            p += 1
            nf += c
            continue
        if c == 0x60:
            sz = struct.unpack_from('<H', data, p + 1)[0]
            rec = data[p + 3:p + 3 + sz]
            p += 3 + sz
        else:
            rec = data[p + 1:p + 1 + c - 0x60]
            p += 1 + c - 0x60
        nf += 1
        if rec and rec[0] & 0x80:
            ev, q = rec[1:1 + (rec[0] & 0x7F)], 0
            while q < len(ev):
                op, ln = ev[q], T[ev[q]]
                if not ln:
                    break
                if op in (49, 50, 51) and ln >= 3:
                    out.add(struct.unpack_from('<H', ev, q + 1)[0])
                q += ln
    return out


def bank_trim(d, keep):
    """YMKs bank of the motions in keep (closed over jumps), sentinels kept."""
    T, ents, data = bank_parse(d)
    offs = sorted({e[3] for e in ents} | {len(data)})
    length = {o: offs[k + 1] - o for k, o in enumerate(offs[:-1])}     # entries may share data
    ids = {e[2] for e in ents}
    keep = set(k for k in keep if k in ids)
    todo = list(keep)
    while todo:
        k = todo.pop()
        for i, e in enumerate(ents):
            if e[2] == k:
                for j in motion_jumps(T, data, e[3], e[4]):
                    if j in ids and j not in keep:
                        keep.add(j)
                        todo.append(j)
    out_ents, out_data, moved = [], bytearray(), {}
    for e in ents:
        if e[2] in keep or e[2] >= 16000:
            if e[3] not in moved and e[3] < len(data):
                moved[e[3]] = len(out_data)
                out_data += data[e[3]:e[3] + length[e[3]]]
            out_ents.append((e[0], e[1], e[2], moved.get(e[3], len(out_data)), e[4], 0))
    body = b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + T + struct.pack('<I', len(out_ents))
    body += b''.join(struct.pack('<BBHIII', *e) for e in out_ents)
    return body + bytes(out_data), sorted(keep), sorted(ids - keep - {k for k in ids if k >= 16000})


def bank_merge(bank, bank11, keys):
    """Our YMKs bank plus bank11's motions with the given keys (and its end
    entries, id >= 16000), sorted by (id, x, y), under bank11's event table."""
    T8, e8, d8 = bank_parse(bank)
    T11, e11, d11 = bank_parse(bank11)
    out, data = [], bytearray()

    def take(ents, src, pick):
        offs = sorted({e[3] for e in ents} | {len(src)})
        ln = {o: offs[k + 1] - o for k, o in enumerate(offs[:-1])}
        moved = {}
        for e in ents:
            if not pick(e):
                continue
            if e[3] not in moved and e[3] < len(src):
                moved[e[3]] = len(data)
                data.extend(src[e[3]:e[3] + ln[e[3]]])
            out.append((e[0], e[1], e[2], moved.get(e[3], len(data)), e[4], 0))
    take(e8, d8, lambda e: e[2] < 16000)
    have = {e[2] for e in out}
    take(e11, d11, lambda e: (e[2] in keys and e[2] not in have) or e[2] >= 16000)
    out.sort(key=lambda e: (e[2], e[1], e[0]))
    body = b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + T11 + struct.pack('<I', len(out))
    body += b''.join(struct.pack('<BBHIII', *e) for e in out)
    return body + bytes(data)


COMMON_2011 = set(range(12200, 12232))
CAR_2011 = set(range(12300, 12308))
HOTSPOT_CARS = {42: 'car_por', 55: 'car_range01', 57: 'car_hearse01'}   # T23 record id -> body
SPOT_WORDS = (0, 9.0, 12306.0, None, 0, 0, 10.0, 1.0, 1.0, 12300.0, 12303.0)  # 78PK record 72


def add_hotspots(tab, pool, loc, first_id=201):
    """2011 front hot spots for HOTSPOT_CARS: new type-33 records, pool words
    and locators. -> (tab, pool, loc, ids)"""
    W = list(struct.unpack(f'<{len(pool) // 4}I', pool))
    n = struct.unpack_from('<I', loc, 0)[0]
    by_id = {struct.unpack_from('<I', loc, 16 + 32 * k + 12)[0]: struct.unpack_from('<3fI3fI', loc, 16 + 32 * k)
             for k in range(n)}
    kept = {r[0] for r in records(tab)}
    new_tab, extra, ids = bytearray(), bytearray(), []
    rid = first_id
    for car in HOTSPOT_CARS:
        if car not in kept or car not in by_id:
            continue
        x, y, z, _, rx, ry, rz, pad = by_id[car]
        # 2011 shows a stored rotation a as x' = x cos a - z sin a, z' = x sin a + z cos a
        lx, lz = 0.0, -3.0
        wx = x + lx * np.cos(ry) - lz * np.sin(ry)
        wz = z + lx * np.sin(ry) + lz * np.cos(ry)
        off = len(W)
        W += [fword(v) if v else 0 for v in SPOT_WORDS[:3]] + [fword(float(car))] + \
             [fword(v) if v else 0 for v in SPOT_WORDS[4:]]
        W[off + 9] = fword(12300.0)
        W[off + 10] = fword(12303.0)
        new_tab += struct.pack('<4H', rid, 33, len(SPOT_WORDS), off)
        extra += struct.pack('<3fI3fI', wx, y, wz, rid, rx, ry, rz, 0)
        ids.append((rid, HOTSPOT_CARS[car]))
        rid += 1
    # placeholders (id 0) stay last
    ph = bytearray(b''.join(struct.pack('<4H', *r) for r in records(tab) if r[0] == 0 and r[1] == 0))
    rest = bytearray(b''.join(struct.pack('<4H', *r) for r in records(tab) if not (r[0] == 0 and r[1] == 0)))
    tab = bytes(rest + new_tab + ph)
    loc = bytearray(loc)
    struct.pack_into('<I', loc, 0, n + len(ids))
    loc = bytes(loc) + bytes(extra)
    return tab, struct.pack(f'<{len(W)}I', *W), loc, ids


# ------------------------------------------------------------ build

def build(gm08, bg56, gm11, out, budget=None, keep_motions=False, report=False, png=None,
          new_entry=None, flip_rotation=True, elements='clean', pac_out=None, pac_entry='78P8',
          offset=None, motions_2011='common', hotspots_2011=False, texture_halve=1, rotate=0, spread=1.0):
    log = []
    _, _, g56b = find(gm08, b'GMGA', b'0056')
    stored08 = dict(f.pach_read(unpack(g56b)))
    p08 = {i: unpack(b) for i, b in stored08.items()}
    (h11, g11, t11), (gi, ei), pk = find(gm11, b'GMGB', b'78PK')
    top11 = dict(f.pach_read(pk))
    sub11 = {i: unpack(b) for i, b in f.pach_read(top11[0])}

    # offset: the same as svr08_stage
    _, _, _, _, e56 = st.stage(bg56)
    d56 = {i: unpack(b) for i, b in e56}
    R56 = st.hmd_read(d56[996])
    P = np.array([c for r in R56 for c in st.tri_corners(r)])
    cx, cz = (P[:, 0].min() + P[:, 0].max()) / 2, (P[:, 2].min() + P[:, 2].max()) / 2
    dx, dz = st.offset_for(R56, offset, rotate)
    anchors = st.car_anchors(gm08, R56) if spread != 1.0 else None
    if anchors:
        log.append(f'spread x{spread} about ({cx:.1f}, {cz:.1f}): locators move with their nearest vehicle')
    log.append(f'offset (x, z) + ({dx}, {dz})')

    # models
    models = {}
    for i, b in f.pach_read(p08[4]):
        u = unpack(b)
        if u[:4] == b'JBOY':
            m = jboy.read(u)
            m.textures = [REFLECTION_MAP.get(n, n) for n in m.textures]
            models[i] = m
    objs = {i for i in models if 1000 <= i < 1500}
    new_id = {i: (i + OBJ_SHIFT if i in objs else i) for i in models}
    if max(new_id[i] for i in objs) > 1069:
        raise SystemExit('object ids do not fit 1040-1069')

    # records and pool
    pool, keys, _ = convert_pool(p08[0], p08[2], objs)
    rec = records(p08[0])
    log.append(f'records: {len([r for r in rec if r[1]])} (types {sorted({r[1] for r in rec if r[1]})}); '
               f'objects {min(objs)}-{max(objs)} -> {min(objs) + OBJ_SHIFT}-{max(objs) + OBJ_SHIFT}')

    # locators
    loc = bytearray(unpack(dict(f.pach_read(p08[3]))[10000]))
    n = struct.unpack_from('<I', loc, 0)[0]
    for k in range(n):
        x, y, z = struct.unpack_from('<3f', loc, 16 + 32 * k)
        if anchors:
            sx, sz = st.spread_shift(x, z, spread, (cx, cz), anchors)
            x, z = x + sx, z + sz
        if rotate == 180:
            x, z = -x, -z
        struct.pack_into('<3f', loc, 16 + 32 * k, x + dx, y, z + dz)
        ry = struct.unpack_from('<f', loc, 16 + 32 * k + 20)[0]
        if flip_rotation:
            ry = -ry
        if rotate == 180:
            ry = (ry + 2 * np.pi) % (2 * np.pi) - np.pi      # + pi, kept in -pi..pi
        struct.pack_into('<f', loc, 16 + 32 * k + 20, ry)
    log.append(f'locators: {n} moved' + (', y rotation negated' if flip_rotation else ''))

    # element table
    T, ents, _ = bank_parse(p08[5])
    tab, decisions = clean_elements(p08[0], pool, set(models), {e[2] for e in ents}, elements)
    count = len(tab) // 8
    hdr = p08[1][:4] + struct.pack('<I', count) + p08[1][8:]
    if elements == '2011hdr':
        pool, normed = normalize_objects(tab, pool)
        log.append(f'objects set to 2011 values: {normed}')
    kept_t = sorted({t for _, t, d_, _ in decisions if d_ == 'kept'})
    log.append(f'elements ({elements}): kept {count} of {len(decisions)} (types {kept_t})')
    for rid, typ, d_, w in decisions:
        if d_ == 'dropped':
            log.append(f'  dropped {rid:3d} T{typ}: {w}')

    # motions: the ones the kept records name (T20 w3, T21 w2, T22 w2, T29 w4)
    Wk = struct.unpack(f'<{len(pool) // 4}I', pool)
    keys = set()
    for rid, typ, nw, off in records(tab):
        for k in MOTION_FIELDS.get(typ, ()) + ((4,) if typ == 29 else ()):
            if is_float(Wk[off + k]):
                keys.add(int(fval(Wk[off + k])))
    if keep_motions:
        keys = {e[2] for e in ents}
    bank, kept, dropped = bank_trim(p08[5], keys)
    want = (COMMON_2011 if motions_2011 == 'common' else set()) | (CAR_2011 if hotspots_2011 else set())
    if want:
        bank = bank_merge(bank, sub11[5], want)
        log.append(f'2011 motions added: {sorted(want)}; bank {len(bank)} bytes')
    if hotspots_2011:
        tab, pool, loc_b, spot_ids = add_hotspots(tab, pool, bytes(loc))
        loc = bytearray(loc_b)
        hdr = p08[1][:4] + struct.pack('<I', len(tab) // 8) + struct.pack('<I', len(pool) // 4) + p08[1][12:]
        log.append(f'2011 hot spots added: {spot_ids}')
    log.append(f'motions: kept {len(kept)} keys, dropped {len(dropped)} {dropped}; bank {len(p08[5])} -> {len(bank)} bytes')

    # models: only those a kept record names
    Wk = struct.unpack(f'<{len(pool) // 4}I', pool)
    named = set()
    for rid, typ, nw, off in records(tab):
        for k in range(nw):
            if is_float(Wk[off + k]) and 1000 <= fval(Wk[off + k]) < 1600:
                v = int(fval(Wk[off + k]))
                named.add(v - OBJ_SHIFT if 1000 + OBJ_SHIFT <= v < 1500 else v)
    unused = sorted(i for i in models if i not in named)
    models = {i: m for i, m in models.items() if i in named}
    if unused:
        log.append(f'models left out (no kept record names them): {unused}')

    # textures
    tex56 = {n.rsplit('.', 1)[0]: d for n, d in at.tex_bundle(d56[200]) + at.tex_bundle(d56[201])}
    tex11 = {n.rsplit('.', 1)[0]: d for n, d in at.tex_bundle(dict(
        (i, unpack(b)) for i, b in f.pach_read(sub11[4]))[0])}
    names = []
    for m in models.values():
        for nm in m.textures:
            if nm not in names:
                names.append(nm)
    tex = {}
    for nm in names:
        src = tex56.get(nm) or tex11.get(nm)
        if src is None:
            raise SystemExit(f'texture {nm} not found')
        tex[nm] = src
    shrunk = {}
    for nm in list(tex):
        for _ in range(texture_halve):
            h2 = st.dds_halve(tex[nm])
            if h2 is None:
                break
            tex[nm] = h2
            shrunk[nm] = shrunk.get(nm, 0) + 1

    def package():
        b4 = f.pach_write([(0, at.tex_bundle_write([(nm + '.dds', d) for nm, d in tex.items()]))]
                          + [(new_id[i], jboy.write(models[i])) for i in sorted(models, key=lambda i: new_id[i])])
        b3 = f.pach_write([(10000, bytes(loc))])
        raw = {0: tab, 1: hdr, 2: pool, 3: b3, 4: b4, 5: bank}
        # small children may not shrink: real BPE anyway (2011 stores its
        # 16-byte GMPD header as 35 packed bytes too)
        sub = []
        for k, v in raw.items():
            if k in (0, 1) and v == p08[k]:
                sub.append((k, stored08[k]))
            elif len(v) < 4096:
                sub.append((k, f.bpe_encode(v, compress=True)))
            else:
                sub.append((k, f.bpe_pack(v)))
        return f.pach_write([(0, f.pach_write(sub)), (1, top11[1])]), raw

    entry, raw = package()
    while budget and len(entry) > budget:
        big = sorted(tex, key=lambda nm: -len(tex[nm]))
        done = 0
        for nm in big[:6]:
            h2 = st.dds_halve(tex[nm])
            if h2:
                tex[nm] = h2
                shrunk[nm] = shrunk.get(nm, 0) + 1
                done += 1
        if not done:
            log.append('cannot reach the budget by shrinking textures')
            break
        entry, raw = package()
    for k, v in raw.items():
        if len(v) >= 4096 and len(f.bpe_pack(v)) + 64 > len(v):
            raise SystemExit(f'child {k} does not compress enough to load in place')

    if pac_out:
        small = f.epac_write(h11, [(b'GMGB', [(pac_entry.encode('ascii'), entry)])], t11)
        open(pac_out, 'wb').write(small)
        log.append(f'{pac_out}: GMGB/{pac_entry} only, {len(small)} bytes')
    groups = [(ty, list(e)) for ty, e in g11]
    if new_entry:
        name = new_entry.encode('ascii')
        if len(name) != 4 or any(n == name for _, n in groups[gi][1]):
            raise SystemExit(f'entry name {new_entry!r}: 4 characters, not already in GMGB')
        groups[gi][1].append((name, entry))
        log.append(f'GMGB/{new_entry} added; GMGB/78PK kept')
    else:
        groups[gi][1][ei] = (groups[gi][1][ei][0], entry)
    data = f.epac_write(h11, groups, t11)
    if out != '-':
        open(out, 'wb').write(data)

    old_un = sum(len(v) for v in sub11.values())
    log.append(f'textures: {len(tex)} ({sum(len(d) for d in tex.values())} bytes)'
               + (f', shrunk {len(shrunk)}: ' + ', '.join(f'{nm} 1/{2 ** k}' for nm, k in sorted(shrunk.items()))
                  if shrunk else ', full size'))
    log.append(f'models: {len(models)} ({sum(len(jboy.write(m)) for m in models.values())} bytes)')
    log.append(f'78PK stored {len(pk)} -> {len(entry)}; package unpacked {old_un} -> {sum(len(v) for v in raw.values())}; '
               f'children {", ".join(f"{k}:{len(v)}" for k, v in raw.items())}')
    log.append(f'gm.pac {os.path.getsize(gm11)} -> {len(data)}')
    # placement check: as 2011 shows the cars (it turns them opposite to the
    # stored value, so the negated value shows them the 2008 way)
    hull = enclosure(bg56, dx, dz, rotate, spread)
    shown = footprints(p08[0], pool, loc, models, -1.0)
    for mid, name, corners in shown:
        clear = min(min(st.point_tri_dist(sx, sz, [(c[0], 0, c[1]) for c in tri])
                        for tri in (corners[:3], [corners[0], corners[2], corners[3]]))
                    for sx, sz in st.START_SPOTS)
        log.append(f'  {name:13s} {share_inside(hull, corners) * 100:5.1f}% inside the enclosure, '
                   f'{clear:5.1f} from the nearest start spot')
    if png:
        draw(png, hull, shown)
    if report:
        print('\n'.join(log))
    return log


def draw(path, hull, shown):
    from PIL import Image, ImageDraw
    x0, x1, z0, z1 = -20, 300, -600, -280
    S = 2.5
    im = Image.new('RGB', (int((x1 - x0) * S), int((z1 - z0) * S)), 'white')
    dr = ImageDraw.Draw(im)
    tp = lambda x, z: ((x - x0) * S, (z1 - z) * S)    # north (corridor) up
    dr.polygon([tp(*q) for q in hull], outline=(220, 0, 0))
    bx, bz = st.BOX_CENTRE
    dr.rectangle([tp(bx - 38, bz + 50), tp(bx + 38, bz - 50)], outline=(0, 160, 0))
    for k, (sx, sz) in enumerate(st.START_SPOTS):
        x, y = tp(sx, sz)
        dr.ellipse([x - 4, y - 4, x + 4, y + 4], fill=(255, 140, 0))
        dr.text((x + 5, y - 6), f'start {k + 1}', fill=(200, 100, 0))
    for mid, name, corners in shown:
        dr.polygon([tp(*q) for q in corners], outline=(0, 0, 200))
        cx = sum(q[0] for q in corners) / 4
        cz = sum(q[1] for q in corners) / 4
        dr.text(tp(cx, cz), name, fill=(0, 0, 0))
    dr.text((4, 4), 'red: 2008 enclosure (collision hull)  blue: car bodies as 2011 shows them  green: fight box',
            fill=(0, 0, 0))
    im.save(path)


if __name__ == '__main__':
    a = sys.argv[1:]
    pos = [x for x in a if not x.startswith('--')]

    def opt(name):
        if name not in a:
            return None
        v = a[a.index(name) + 1]
        pos.remove(v)
        return v

    bv = opt('--budget')
    png = opt('--png')
    ne = opt('--as-new-entry')
    el = opt('--elements') or 'clean'
    po = opt('--pac-out')
    pe = opt('--entry') or '78P8'
    ov = opt('--offset')
    rv = opt('--rotate')
    sv = opt('--spread')
    m11 = opt('--motions-2011') or 'common'
    th = opt('--texture-halve')
    if len(pos) != 4:
        print(__doc__)
        sys.exit(1)
    build(*pos, budget=int(bv) if bv else None, keep_motions='--all-motions' in a,
          texture_halve=int(th) if th else 1,
          report='--report' in a, png=png, new_entry=ne, flip_rotation='--keep-rotation' not in a,
          elements=el, pac_out=po, pac_entry=pe,
          offset=tuple(float(x) for x in ov.split(',')) if ov else None, rotate=int(rv) if rv else 0,
          spread=float(sv) if sv else 1.0,
          motions_2011=m11, hotspots_2011='--hotspots-2011' in a)
