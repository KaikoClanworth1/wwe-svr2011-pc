"""Several HCTP rooms in one SvR2011 bg78 (the HCTP backstage as one stage you walk through).

    py world_stage.py <2011 bg78.pac> <out bg78.pac> <fake56 pac>:<dx>,<dz>[:<x0>,<x1>,<z0>,<z1>] ... [--report]

Each room (a fake bg56 from hctp_fake56.py) goes in its own block of 20 model ids (room k: ids 20k..20k+19,
the way stock bg78 keeps one room per block - CFRS shows / hides them by id), turned a half turn and moved by
its (dx, dz) - build_world puts them all in the parking room's place, one over the other (hctp_room shows and
collides only the current one). Stock models 0-199 not taken become stubs (one zero-area triangle: story code
looks some up by id) and the textures only they used are removed. Collision: every room's triangles in the
parking pair 1003 / 1004 (rooms in their own order, as hctp_room's tris= indices expect); the other stock pairs
stay. Texture names get a room prefix (rooms reuse names). HCTP textures are halved (largest first) until the
bundle fits TEX_BUDGET and the file shipped sizes.
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, HERE)
import svrfmt as f  # noqa: E402
import jboy  # noqa: E402
import arena_tool as at  # noqa: E402
import svr08_stage as s8  # noqa: E402
import stage_wrap as sw  # noqa: E402

BLOCK = 20
STOCK_MODELS = range(0, 200)
KEEP_ALL = (-1e9, 1e9, -1e9, 1e9)
# the texture bundle's most: the stage's textures, the gimmick package and their GPU buffers share one ~17 MB
# physical window (allocator 128, 0x15140000-0x16230000); a full shipped-size bundle with six rooms' props
# ran it out at load (sub_82914DD0 null in sub_826E73F8)
TEX_BUDGET = 3500 * 1024


def build(p78, out_path, rooms, margin=64 * 1024, report=False, tex_budget=TEX_BUDGET):
    """rooms: [(fake56 path, (dx, dz), footprint or None)] -> {log, areas: (x0, x1, z0, z1) per room,
    starts: each room's first triangle in HMD 1004 (before stage_wrap's dummies), triangles: their count}"""
    log = []
    h78, g78, t78, (gi, ei), e78 = s8.stage(p78)
    d78 = {i: s8.unpack(b) for i, b in e78}
    orig_file, orig_400 = os.path.getsize(p78), len(d78[400])
    orig_unpacked = sum(len(u) for u in d78.values())
    Q = s8.rot_matrix(180)
    models, add, ground, rec, flags_new = {}, {}, set(), [], {}
    taken, areas, starts = set(), [], []
    for k, (p56, (dx, dz), crop) in enumerate(rooms):
        _, _, _, _, e56 = s8.stage(p56)
        d56 = {i: s8.unpack(b) for i, b in e56}
        d = np.array([dx, 0.0, dz])
        tex56 = {n.rsplit('.', 1)[0]: dds for n, dds in at.tex_bundle(d56[200])}
        rename = dict(s8.REFLECTION_MAP)
        for n in tex56:
            rename[n] = ('%d%s' % (k, n))[:15]
        if len(set(rename.values())) != len(rename):
            raise SystemExit(f'room {k}: texture names clash after the prefix')
        lines56 = s8.flags_lines(d56[50001].decode('cp932', 'replace'))
        nverts = 0
        for i in sorted(i for i in d56 if i < BLOCK and d56[i][:4] == b'JBOY'):
            m, kept, dropped = s8.move_model(d56[i], Q, d, crop or KEEP_ALL)
            if m is None:
                log.append(f'room {k} model {i}: every triangle outside the crop, left out')
                continue
            names = s8.retexture(m, rename)
            inv = {v: n for n, v in rename.items()}
            for n in names:
                src = inv.get(n, n)
                if src in tex56 and n not in s8.REFLECTION_MAP.values():
                    add[n] = tex56[src]
                    if i in s8.GROUND:
                        ground.add(n)
            vid = BLOCK * k + i
            models[vid] = m
            taken.add(vid)
            fl = [x for x in lines56.get(i, []) if not x.startswith('l(')]
            flags_new[vid] = (['n'] if 'n' not in fl else []) + fl
            nverts += sum(len(s.verts) for s in m.meshes)
            log.append(f'room {k} model {i:2d}->{vid:3d} {m.name:14s} meshes {len(m.meshes):2d} tris {kept:5d} '
                       f'(dropped {dropped})')
        R = [s8.hmd_move(r, Q, d) for r in s8.hmd_read(d56[996])]
        log.append(f'room {k}: {len(R)} collision triangles from record {len(rec)}, {nverts} vertices')
        P = np.array([c for r in R for c in s8.tri_corners(r)])
        areas.append((float(P[:, 0].min()), float(P[:, 0].max()), float(P[:, 2].min()), float(P[:, 2].max())))
        starts.append(len(rec))
        rec += R
    # stock models 0-199 not taken: stubs; textures only they (or the replaced ones) name: removed
    stubs = {i: s8.stub_model(d78[i]) for i in STOCK_MODELS if i not in taken and d78.get(i, b'')[:4] == b'JBOY'}
    named, users = set(), {}
    for i, u in d78.items():
        if u[:4] == b'JBOY':
            for tn in jboy.read(u).textures:
                named.add(tn.lower())
                if i not in STOCK_MODELS:
                    users.setdefault(tn.lower(), set()).add(i)
    tex78 = at.tex_bundle(d78[400])
    keep78 = [(n, dds) for n, dds in tex78 if not (n.rsplit('.', 1)[0].lower() in named
                                                  and not users.get(n.rsplit('.', 1)[0].lower()))]
    log.append(f'stock textures kept {len(keep78)} of {len(tex78)}; stubs {len(stubs)}')
    hmd, idx = sw.hmd_write(rec), sw.index_build(rec)
    log.append(f'collision {s8.PARK_HMD}: {len(rec)} triangles')
    # 50001: stock lines for 0-199 out, the rooms' lines in (stubs keep theirs)
    out_lines, placed = [], False
    for line in d78[50001].split(b'\r\n'):
        p = line.split()
        if p and p[0].isdigit() and int(p[0]) in taken:
            continue
        if p and p[0].isdigit() and int(p[0]) > max(taken) and not placed:
            out_lines += [(f'{i} ' + ' '.join(flags_new[i])).encode() for i in sorted(flags_new)]
            placed = True
        out_lines.append(line)
    if not placed:
        out_lines += [(f'{i} ' + ' '.join(flags_new[i])).encode() for i in sorted(flags_new)]
    flags = b'\r\n'.join(out_lines)

    shrunk = {}

    def bundle():
        return at.tex_bundle_write(keep78 + [(n + '.dds', dds) for n, dds in add.items()])

    model_raw = {i: jboy.write(m) for i, m in models.items()}
    model_raw.update(stubs)

    def unpacked_total(b400):
        tot = 0
        for i, u in d78.items():
            if i in model_raw:
                continue
            tot += {400: len(b400), s8.PARK_HMD: len(hmd), s8.PARK_INDEX: len(idx), 50001: len(flags)}.get(i, len(u))
        return tot + sum(len(r) for r in model_raw.values())

    def halve_one():
        def tier(n):
            w, hh, _, _ = s8.dds_info(add[n])
            return 1 if n in ground else (0 if min(w, hh) > 128 else 2)
        for n in sorted(add, key=lambda n: (tier(n), -len(add[n]))):
            h2 = s8.dds_halve(add[n])
            if h2:
                add[n] = h2
                shrunk[n] = shrunk.get(n, 0) + 1
                return True
        return False

    while True:
        b400 = bundle()
        if len(b400) <= min(orig_400 - margin, tex_budget) and unpacked_total(b400) <= orig_unpacked - margin:
            break
        if not halve_one():
            raise SystemExit('cannot fit the textures')
    packed = {i: f.bpe_pack(r) for i, r in model_raw.items()}
    small = {s8.PARK_HMD: hmd, s8.PARK_INDEX: idx, 50001: flags}
    for kk, v in small.items():
        packed[kk] = f.bpe_pack(v)
    while True:
        b400 = bundle()
        packed[400] = f.bpe_pack(b400)
        entries = [(i, packed.get(i, b)) for i, b in e78 if i not in model_raw]
        entries += [(i, packed[i]) for i in model_raw]
        entries.sort(key=lambda e: e[0])
        groups = [(typ, list(ents)) for typ, ents in g78]
        groups[gi][1][ei] = (groups[gi][1][ei][0], f.pach_write(entries))
        data = f.epac_write(h78, groups, t78)
        if len(data) <= orig_file - margin:
            break
        if not halve_one():
            raise SystemExit('cannot fit the file')
    for i, b in packed.items():
        raw = {**model_raw, **small, 400: b400}[i]
        if len(b) + 64 > len(raw):
            raise SystemExit(f'entry {i} does not compress enough to load in place')
    open(out_path, 'wb').write(data)
    log.append(f'textures: HCTP {len(add)}, halved {len(shrunk)} ({sum(shrunk.values())} levels)')
    log.append(f'bundle 400 raw {len(b400)} / shipped {orig_400}; unpacked {unpacked_total(b400)} / {orig_unpacked}; '
               f'file {len(data)} / {orig_file}')
    if report:
        print('\n'.join(log))
    return dict(log=log, areas=areas, starts=starts, triangles=len(rec))


if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    rooms = []
    for spec in a[2:]:
        # (paths may hold a drive colon: split from the right)
        bits = spec.split(':')
        if len(bits[0]) == 1:                       # drive letter
            bits = [bits[0] + ':' + bits[1]] + bits[2:]
        dx, dz = map(float, bits[1].split(','))
        crop = tuple(map(float, bits[2].split(','))) if len(bits) > 2 else None
        rooms.append((bits[0], (dx, dz), crop))
    build(a[0], a[1], rooms, report='--report' in sys.argv)
