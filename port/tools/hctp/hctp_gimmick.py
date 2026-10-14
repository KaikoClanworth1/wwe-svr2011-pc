"""Gimmick package (GMGB) for an HCTP room hosted in bg78's parking slot.

    python hctp_gimmick.py <2011 gm.pac> <out gimmick.pac> [--entry 78H3] [--props <spec.json>] [--report]

Starts from 2011's 78PK: keeps the type-0 placeholders and the corridor parts
(records naming 15xx objects), drops the cars, the trailer and their hot spots,
keeps the motion bank's common backstage motions (12200-12231; the game asks
for them itself) and 78PK's particles. HCTP props are added as T23 objects
(ids 1040+, record ids 100+), each with its locator (world position, y rotation
as 2011 stores it).

props spec (json list): {"model": <JBOY path>, "pos": [x,y,z], "ry": radians}
"""
import json
import struct
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import svrfmt as f  # noqa: E402
import jboy  # noqa: E402
import arena_tool as at  # noqa: E402
import svr08_gimmick as G  # noqa: E402

COMMON = set(range(12200, 12232)) | {32767}


def bank_keep(bank, keys):
    return G.bank_trim(bank, keys)[0]


def build(gm11, out, entry='78H3', props=(), textures=None, report=False, keep_bank=True, keep_models=True,
          records=(), bank=None):
    (h11, g11, t11), _, pk = G.find(gm11, b'GMGB', b'78PK')
    top = dict(f.pach_read(pk))
    sub = {i: G.unpack(b) for i, b in f.pach_read(top[0])}
    W = list(struct.unpack(f'<{len(sub[2]) // 4}I', sub[2]))
    recs = G.records(sub[0])
    keep = []
    for rid, typ, nw, off in recs:
        if typ == 0:
            keep.append((rid, typ, W[off:off + nw]))
        elif typ == 23 and G.is_float(W[off]) and 1500 <= G.fval(W[off]) < 1600:
            keep.append((rid, typ, W[off:off + nw]))
    L = sub[3]
    loc11 = G.unpack(dict(f.pach_read(L))[10000])
    n = struct.unpack_from('<I', loc11, 0)[0]
    kept_ids = {r[0] for r in keep if r[1]}
    locs = [loc11[16 + 32 * k:48 + 32 * k] for k in range(n)
            if struct.unpack_from('<I', loc11, 16 + 32 * k + 12)[0] in kept_ids]
    # HCTP props
    models = {}
    next_rec, next_obj = 100, 1040
    for p in props:
        m = jboy.read(open(p['model'], 'rb').read())
        oid = next_obj
        next_obj += 1
        if oid > 1079:
            raise SystemExit('more than 40 objects')
        models[oid] = m
        rid = next_rec
        next_rec += 1
        idle = p.get('idle', 0)   # word5: a key the object plays at once (its rest pose)
        words = [G.fword(float(oid)), 0, 0, 0, 0, G.fword(float(idle)) if idle else 0, G.fword(1.0)]
        keep.append((rid, 23, words))
        x, y, z = p['pos']
        locs.append(struct.pack('<3fI3fI', x, y, z, rid, 0.0, p.get('ry', 0.0), 0.0, 0))
    # extra records: {'type': t, 'words': [floats], 'pos': (x,y,z), 'ry': a}
    for r in records:
        rid = next_rec
        next_rec += 1
        keep.append((rid, r['type'], [G.fword(float(w)) for w in r['words']]))
        x, y, z = r['pos']
        locs.append(struct.pack('<3fI3fI', x, y, z, rid, 0.0, r.get('ry', 0.0), 0.0, 0))
    # tables
    keep.sort(key=lambda r: r[0])
    tab, pool = bytearray(), []
    for rid, typ, ws in keep:
        tab += struct.pack('<4H', rid, typ, len(ws), len(pool))
        pool += ws
    hdr = sub[1][:4] + struct.pack('<II', len(keep), len(pool)) + sub[1][12:]
    loc = bytearray(loc11[:16]) + b''.join(locs)
    struct.pack_into('<I', loc, 0, len(locs))
    if bank is not None:
        keep_bank = False
    else:
        bank = bank_keep(sub[5], COMMON)
    tex = dict(textures or {})
    b4 = f.pach_write([(0, at.tex_bundle_write([(nm + '.dds', d) for nm, d in tex.items()]))]
                      + [(i, jboy.write(models[i])) for i in sorted(models)])
    raw = {0: bytes(tab), 1: hdr, 2: struct.pack(f'<{len(pool)}I', *pool),
           3: f.pach_write([(10000, bytes(loc))]), 4: b4, 5: bank}
    def pack(v):
        if len(v) < 4096:
            return f.bpe_encode(v, compress=True)
        try:
            return f.bpe_pack(v)
        except ValueError:
            return v          # does not compress: stored as is
    stored = dict(f.pach_read(top[0]))
    subs = []
    for k, v in raw.items():
        if k == 5 and keep_bank:
            subs.append((k, stored[5]))          # 78PK's bank as stored (BPE)
        elif k == 4 and not models and keep_models:
            subs.append((k, stored[4]))          # 78PK's models + bundle (no record names them)
        else:
            subs.append((k, pack(v)))
    pkg = f.pach_write([(0, f.pach_write(subs)), (1, top[1])])
    small = f.epac_write(h11, [(b'GMGB', [(entry.encode('ascii'), pkg)])], t11)
    open(out, 'wb').write(small)
    if report:
        print(f'records {len(keep)} (types {sorted({t for _, t, _ in keep})}), locators {len(locs)}, '
              f'objects {len(models)}, bank {len(bank)} bytes, package {len(pkg)} bytes, pac {len(small)}')
    return small


if __name__ == '__main__':
    a = sys.argv[1:]

    def opt(k, d=None):
        return a[a.index(k) + 1] if k in a else d
    props = json.load(open(opt('--props'))) if opt('--props') else []
    build(a[0], a[1], entry=opt('--entry', '78H3'), props=props, report='--report' in a)
