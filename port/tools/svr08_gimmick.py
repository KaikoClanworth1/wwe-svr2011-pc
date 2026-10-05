"""SvR 2008 Parking Lot cars (gm.pac GMGA/0056) -> a replacement for SvR 2011's
parking gimmick package (gm.pac GMGB/78PK), to go with svr08_stage.py's bg78.

    python tools/svr08_gimmick.py <2008 gm.pac> <2008 bg56.pac> <2011 gm.pac> <out gm.pac>
        [--report] [--budget <bytes>] [--trim-motions] [--png <file.png>]
        [--as-new-entry <NAME>] [--keep-rotation]

Both games use the same gimmick package ("GMPD"): 0 record index {u16 id,
u16 type, u16 words, u16 offset}, 1 "GMPD" {count, pool words}, 2 the word
pool, 3 PACH{10000: locators}, 4 PACH{models (+ textures)}, 5 a motion bank.
2011 adds a texture bundle as 4/0, wraps the bank as "YMKs", and puts the
package in a PACH {0: package, 1: particles}. Yuke's own carry-over of the
2008 chamber package (GMGA/0020) shows the rest: record index, header and
locators unchanged, motion fields as full keys (2008 201 -> 2011 12201).

What the conversion does:
  * records (0, 1) unchanged; pool (2): object ids 1000-1017 -> 1040-1057
    (the parking show range 1040-1069; break parts 1500-1513 stay), motion
    numbers in T20 word 3, T21 word 2, T22 word 2 -> 12000 + n (T29 already
    holds a full key). 2008-only record types (20, 24 with 5 words, 25 with 4,
    27, 28) are left as they are: whether 2011 runs them is the game test.
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
  * motions (5): wrapped as YMKs (the 2008 event-size table is kept: the
    bank's own table sizes its events). --trim-motions keeps only the keys the
    records name plus the keys their motions jump to (event ops 49/50/51);
    that drops 12107, 12113, 12145 and 12146 (about 160 KB), but a T22 record
    names 12106, which the 2008 bank does not have, so the default keeps all.
  * particles: 2011's 78PK/1 as is.
  * --budget <bytes>: halves the largest textures (top mip) until the stored
    package is no bigger (default: no shrinking; the report gives the sizes).
Writes a whole gm.pac with only GMGB/78PK replaced, or with
--as-new-entry NAME (e.g. 78P8) a gm.pac that keeps 78PK and every other
entry byte for byte and gains GMGB/NAME (appended to the GMGB group).
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


# ------------------------------------------------------------ build

def build(gm08, bg56, gm11, out, budget=None, keep_motions=True, report=False, png=None,
          new_entry=None, flip_rotation=True):
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
    dx, dz = round(st.BOX_CENTRE[0] - cx, 1), round(st.BOX_CENTRE[1] - cz, 1)
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
        struct.pack_into('<3f', loc, 16 + 32 * k, x + dx, y, z + dz)
        if flip_rotation:
            ry = struct.unpack_from('<f', loc, 16 + 32 * k + 20)[0]
            struct.pack_into('<f', loc, 16 + 32 * k + 20, -ry)
    log.append(f'locators: {n} moved' + (', y rotation negated' if flip_rotation else ''))

    # motions
    T, ents, _ = bank_parse(p08[5])
    if keep_motions:
        keys = {e[2] for e in ents}
    bank, kept, dropped = bank_trim(p08[5], keys)
    log.append(f'motions: kept {len(kept)} keys, dropped {len(dropped)} {dropped}; bank {len(p08[5])} -> {len(bank)} bytes')

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

    def package():
        b4 = f.pach_write([(0, at.tex_bundle_write([(nm + '.dds', d) for nm, d in tex.items()]))]
                          + [(new_id[i], jboy.write(models[i])) for i in sorted(models, key=lambda i: new_id[i])])
        b3 = f.pach_write([(10000, bytes(loc))])
        raw = {0: p08[0], 1: p08[1], 2: pool, 3: b3, 4: b4, 5: bank}
        # 0 and 1 are unchanged: Yuke's own packed blobs (1 is 16 bytes and
        # does not compress; 2011 stores it the same way)
        sub = [(k, stored08[k] if k in (0, 1) else f.bpe_pack(v)) for k, v in raw.items()]
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
        if k not in (0, 1) and len(f.bpe_pack(v)) + 64 > len(v):
            raise SystemExit(f'child {k} does not compress enough to load in place')

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
    open(out, 'wb').write(data)

    old_un = sum(len(v) for v in sub11.values())
    log.append(f'textures: {len(tex)} ({sum(len(d) for d in tex.values())} bytes)'
               + (f', shrunk {len(shrunk)}: ' + ', '.join(f'{nm} 1/{2 ** k}' for nm, k in sorted(shrunk.items()))
                  if shrunk else ', full size'))
    log.append(f'models: {len(models)} ({sum(len(jboy.write(m)) for m in models.values())} bytes)')
    log.append(f'78PK stored {len(pk)} -> {len(entry)}; package unpacked {old_un} -> {sum(len(v) for v in raw.values())}; '
               f'children {", ".join(f"{k}:{len(v)}" for k, v in raw.items())}')
    log.append(f'gm.pac {os.path.getsize(gm11)} -> {len(data)}')
    if png:
        draw(png, loc, models, new_id)
    if report:
        print('\n'.join(log))
    return log


def draw(path, loc, models, new_id):
    from PIL import Image, ImageDraw
    x0, x1, z0, z1 = -80, 550, -800, -300
    S = 1.5
    im = Image.new('RGB', (int((x1 - x0) * S), int((z1 - z0) * S)), 'white')
    dr = ImageDraw.Draw(im)
    tp = lambda x, z: ((x - x0) * S, (z1 - z) * S)
    n = struct.unpack_from('<I', loc, 0)[0]
    for k in range(n):
        x, y, z = struct.unpack_from('<3f', loc, 16 + 32 * k)
        dr.ellipse([tp(x, z)[0] - 2, tp(x, z)[1] - 2, tp(x, z)[0] + 2, tp(x, z)[1] + 2], fill=(220, 0, 0))
        dr.text((tp(x, z)[0] + 3, tp(x, z)[1] - 5), str(k + 1), fill=(120, 0, 0))
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
    if len(pos) != 4:
        print(__doc__)
        sys.exit(1)
    build(*pos, budget=int(bv) if bv else None, keep_motions='--trim-motions' not in a,
          report='--report' in a, png=png, new_entry=ne, flip_rotation='--keep-rotation' not in a)
