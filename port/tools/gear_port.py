"""Entrance gear from one character model pac onto another (a coat, hat or shirt for the entrance).

    py gear_port.py --gear-from <donor ch.pac>,<attire> --onto <target ch.pac>,<attire> <out ch.pac> [report.json]
    py gear_port.py --gear-json <gear.json> --onto <target ch.pac>,<attire> <out ch.pac> [report.json]
    (also: py svr11_build.py --gear-from / --gear-json ... --onto ... <out> [report.json])

--gear-json: gear meshes made elsewhere (the Mod Maker's FBX path) - {"version": 1, "meshes": [...] (the
svr11_build.py mesh format, already on the target's A-pose in 2011 space, weights by 2011 / extra bone
names), "textures": {"color": png, "normal": png (optional)}, "extra_bones": [{"name", "parent",
"position": [x, y, z] (2011 world)}]}: built with svr11_build (one sheet, Mk flat), then onto the target as
below (sheets side by side, masks blanked, the budget), the target's own pieces all kept.

A character's entrance gear is its pac entry "<id><attire - 1>4" (kind 4): a whole model of the wrestler
wearing it, worn for the entrance instead of the wrestler model (kind 2). In the stock pacs it is built
again rather than added on (Undertaker: none of its 32 meshes equal his kind 2's; Cena 4 of 33; Shawn
Michaels 22 of 31), so the gear is told apart by where it lies:
  - a donor kind 4 mesh that uses one of its extra bones (Undertaker's coat: 130 nodes against 102), or
    whose vertices lie off the donor's kind 2 body (median distance over 0.06 units = 6 mm), is gear;
  - the rest (the same skin cut again under the gear) is left out - the target's own body is used.
The gear is posed onto the target: every vertex moved by its bones' (target bind) x (donor bind)^-1, the
bones matched by name; the donor's extra bones are appended to the target's table, hung from the same
parents. Textures: one sheet each (color / normal / Mk) at the stock size, the target's on the left half and
the donor's on the right (UVs moved to match; an extra sheet by another name wasn't used by the game - the
coat drew from the target's sheet), so the entrance model has half the horizontal resolution.
Out: the target pac with that attire's kind 4 entry written (replaced if there was one); the target's
hair and eyelashes, unless the donor's kind 4 has its own (a hat): then the donor's, posed the same way.
"""
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import jboy  # noqa: E402
import model_budget  # noqa: E402
import svrfmt  # noqa: E402
import w13_char as W  # noqa: E402

OFF_BODY = 0.06


def entries(pac):
    _, groups = ch_tool.epk8_read(open(pac, 'rb').read())
    return [(n, b) for t, es in groups if t.strip() == b'EMD' for n, b in es if n.strip(b'\0')]


def kids_of(blob):
    return {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(blob))}


def find(ents, attire, kind):
    for n, b in ents:
        if len(n) >= 8 and n[6:7] == str(attire - 1).encode() and n[7:8] == str(kind).encode():
            return n, b
    return None, None


def worlds(d):
    """true node frames of a JBOY -> [4x4 world]"""
    return W.world(W.read13_nodes(d))


def flat_weights(s):
    nw = len(s.weights)
    flat = [e for blk in s.weights for e in blk]
    vc = len(s.verts)
    return [[(e[0][0], e[1]) for e in flat[v * nw:(v + 1) * nw] if e[0][0] != 255 and e[1] > 0] for v in range(vc)]


def body_points(m):
    return np.array([v[:3] for s in m.meshes for v in s.verts], float)


def min_dist(P, Q, chunk=2048):
    out = np.empty(len(P))
    for i in range(0, len(P), chunk):
        d = ((P[i:i + chunk, None, :] - Q[None, :, :]) ** 2).sum(-1)
        out[i:i + chunk] = np.sqrt(d.min(1))
    return out


def transplant(donor, d_att, target, t_att, out, report=None):
    rep = {'ok': False, 'notes': [], 'warnings': []}
    de, te = entries(donor), entries(target)
    dn4, db4 = find(de, d_att, 4)
    dn2, db2 = find(de, d_att, 2)
    tn2, tb2 = find(te, t_att, 2)
    if not db4:
        raise SystemExit('the donor has no entrance gear (kind 4) for attire %d' % d_att)
    if not tb2:
        raise SystemExit('the target has no attire %d' % t_att)
    d4, d2, t2 = kids_of(db4), kids_of(db2) if db2 else None, kids_of(tb2)
    m4, mt = jboy.read(d4[0]), jboy.read(t2[0])
    m2 = jboy.read(d2[0]) if d2 else None
    names2 = {n['name'] for n in (m2.nodes if m2 else mt.nodes)}
    dnames = [n['name'] for n in m4.nodes]
    extra = [i for i, n in enumerate(m4.nodes) if n['name'] not in names2]
    Wd, Wt = worlds(d4[0]), worlds(t2[0])
    # the target's node table plus the donor's extra bones (same local frames, parents by name)
    fr4 = W.read13_nodes(d4[0])
    nodes = [dict(n) for n in mt.nodes]
    tidx = {n['name']: i for i, n in enumerate(nodes)}
    Wout = list(Wt)
    for i in extra:
        n = dict(m4.nodes[i])
        pname = dnames[fr4[i]['parent']] if fr4[i]['parent'] >= 0 else 'root'
        pi = tidx.get(pname, tidx.get('mune', 0))
        n['u48'] = (pi,) + tuple(n['u48'][1:])
        nodes.append(n)
        tidx[n['name']] = len(nodes) - 1
        L = np.linalg.inv(Wd[fr4[i]['parent']]) @ Wd[i] if fr4[i]['parent'] >= 0 else Wd[i]
        Wout.append(Wout[pi] @ L)
    # per donor node: donor bind -> target bind
    X = {}
    for i, n in enumerate(m4.nodes):
        ti = tidx.get(n['name'])
        X[i] = (Wout[ti] @ np.linalg.inv(Wd[i])) if ti is not None else np.eye(4)
    # gear meshes
    body = body_points(m2) if m2 else None
    gear = []
    for s in m4.meshes:
        pal = [p - 1 for p in s.palette]
        uses_extra = any(p in extra for p in pal)
        P = np.array([v[:3] for v in s.verts], float)
        off = float(np.median(min_dist(P, body))) if body is not None else 1.0
        if uses_extra or off > OFF_BODY:
            gear.append(s)
    rep['notes'].append('donor %s: %d of %d meshes are gear (%d extra bones)' % (dn4.decode(), len(gear), len(m4.meshes),
                                                                                    len(extra)))
    if not gear:
        raise SystemExit('no gear found in the donor\'s kind 4 (it only re-cuts the body)')

    def halve(uvs, right):
        return [(min(1.0, max(0.0, u)) * 0.5 + (0.5 if right else 0.0), v) for u, v in uvs]

    def pose(s, tex_names):
        fw = flat_weights(s)
        pal = [p - 1 for p in s.palette]
        verts = []
        for v, ws in zip(s.verts, fw):
            M = sum(w * X[pal[b]] for b, w in ws) if ws else np.eye(4)
            p = M @ np.array([v[0], v[1], v[2], 1.0])
            nv = M[:3, :3] @ np.array(v[3:6])
            ln = np.linalg.norm(nv) or 1.0
            verts.append((float(p[0]), float(p[1]), float(p[2]), *(float(x) for x in nv / ln), v[6]))
        ns = jboy.Mesh()
        ns.__dict__.update(s.__dict__)
        ns.verts = verts
        ns.palette = [tidx.get(dnames[p], 0) + 1 for p in pal]
        params = []
        for name, typ, val in s.params:
            if typ == 0x0f:
                k = struct.unpack('>i', val[:4])[0]
                if k >= 0:
                    tn = m4.textures[k] if k < len(m4.textures) else ''
                    if tn not in tex_names:
                        tex_names.append(tn)
                    val = struct.pack('>i', tex_names.index(tn)) + val[4:]
            params.append((name, typ, val))
        ns.params = params
        ns.uvs = halve(s.uvs, True)
        A = np.array([v[:3] for v in verts])
        c = (A.min(0) + A.max(0)) / 2
        md = bytearray(ns.raw)
        struct.pack_into('>4f', md, 0xA4, float(c[0]), float(c[1]), float(c[2]), float(np.sqrt(((A - c) ** 2).sum(1)).max()))
        ns.raw = bytes(md)
        return ns

    out_kids = {k: v for k, v in t2.items() if k not in (0x7530, 0x7531, 0x7532, 0xc8, 0xc9)}
    tex_names = list(mt.textures)
    body_m = jboy.Model()
    body_m.__dict__.update(mt.__dict__)
    body_m.nodes = nodes
    def left(s_):
        n_ = jboy.Mesh()
        n_.__dict__.update(s_.__dict__)
        n_.uvs = halve(s_.uvs, False)
        return n_
    body_m.meshes = [left(s_) for s_ in mt.meshes] + [pose(s, tex_names) for s in gear]
    mt.meshes = body_m.meshes[:len(mt.meshes)]   # (the target's own pieces, moved to the left half)
    for k, s in enumerate(body_m.meshes):
        s.u74 = k
    body_m.textures = tex_names
    out_kids[0] = jboy.write(body_m)
    # hair / lashes: the donor's (posed) when its kind 4 has its own (a hat), else the target's
    # hair / lashes: the target's, plus the donor kind 4's pieces its kind 2 doesn't have (a hat), posed
    def same(s1, s2):
        return len(s1.verts) == len(s2.verts) and s1.verts[0][:3] == s2.verts[0][:3]
    for k in (0x2710, 0x4e20):
        if k not in d4 or d4[k][:4] != b'JBOY':
            continue
        hm = jboy.read(d4[k])
        h2 = jboy.read(d2[k]) if d2 is not None and k in d2 and d2[k][:4] == b'JBOY' else None
        # (pieces on the donor's kind 2 hair / body surface are its own hair cut again under the gear - Undertaker's
        # kind 4 hair is all re-cut - and stay behind; pieces off it are gear: a hat)
        ref = np.concatenate([body] + ([np.array([v[:3] for o in h2.meshes for v in o.verts], float)] if h2 else []))
        extra_h = [s_ for s_ in hm.meshes if not (h2 and any(same(s_, o) for o in h2.meshes))
                   and float(np.median(min_dist(np.array([v[:3] for v in s_.verts], float), ref))) > OFF_BODY]
        if not extra_h:
            continue
        hnames = [n['name'] for n in hm.nodes]
        if k in t2 and t2[k][:4] == b'JBOY':
            nm = jboy.read(t2[k])
            nm.meshes = [left(s_) for s_ in nm.meshes]
            hn = list(nm.textures)
        else:
            nm = jboy.Model()
            nm.__dict__.update(hm.__dict__)
            nm.meshes = []
            hn = []
        nm.nodes = nodes
        for s_ in extra_h:   # (its palette names the hair model's own nodes: by name to the donor body's)
            s2 = jboy.Mesh()
            s2.__dict__.update(s_.__dict__)
            s2.palette = [dnames.index(hnames[p - 1]) + 1 if 0 < p <= len(hnames) and hnames[p - 1] in dnames else 1
                          for p in s_.palette]
            hn_save = list(m4.textures)
            m4.textures, keep_t = list(hm.textures), m4.textures   # (texture slots index the hair model's list)
            nm.meshes.append(pose(s2, hn))
            m4.textures = keep_t
        for j, s_ in enumerate(nm.meshes):
            s_.u74 = j
        nm.textures = hn
        out_kids[k] = jboy.write(nm)
        rep['notes'].append('child 0x%x: %d gear piece(s) added (a hat)' % (k, len(extra_h)))
    dt_a = dict(W.tex_bundle_read(d4[0xa])) if 0xa in d4 else {}
    dt_c = dict(W.tex_bundle_read(d4[0xc])) if 0xc in d4 else {}
    sway = {k: d4[k] for k in (0x3c, 0x3d) if k in d4 and extra}   # (sway data for the extra bones)
    return assemble(rep, te, tn2, out_kids, body_m, len(mt.meshes), dt_a, dt_c, sway, target, out, report,
                    len(gear), len(extra))


def assemble(rep, te, tn2, out_kids, body_m, n_own, dt_a, dt_c, sway, target, out, report, n_gear, n_extra):
    """the target attire's kind 4: sheets side by side (target left, gear right), blank damage masks, sway
    data, the vertex budget (covered target pieces left out), written into the target pac"""
    import io
    from PIL import Image

    def side_by_side(t_dds, d_dds, fourcc):
        a = Image.open(io.BytesIO(t_dds)).convert('RGBA')
        b = Image.open(io.BytesIO(d_dds)).convert('RGBA') if d_dds else a
        w, h = a.size
        out_im = Image.new('RGBA', (w, h))
        out_im.paste(W.resize_rgba(a, (w // 2, h)), (0, 0))
        out_im.paste(W.resize_rgba(b, (w // 2, h)), (w // 2, 0))
        return W.dds_encode(out_im, fourcc)
    ta = W.tex_bundle_read(out_kids[0xa])
    tc = W.tex_bundle_read(out_kids[0xc])
    dl = {n.lower(): d for n, d in dt_a.items()}
    ta = [(n, side_by_side(d, dl.get(n.lower()), 'DXT1' if d[84:88] == b'DXT1' else 'DXT5'))
          if n.lower() in ('normal', 'mk') else (n, d) for n, d in ta]
    tc = [(n, side_by_side(d, dt_c.get('color'), 'DXT5')) if n.lower() == 'color' else (n, d) for n, d in tc]
    out_kids[0xa] = W.tex_bundle_write(ta, out_kids[0xa][:16])
    out_kids[0xc] = W.tex_bundle_write(tc, out_kids[0xc][:16])
    # (the target's damage masks are on its UVs: blank them - halved UVs would misplace them)
    for k in (0xd, 0xe, 0xf):
        if k in out_kids:
            n, d = W.tex_bundle_read(out_kids[k])[0]
            out_kids[k] = W.tex_bundle_write([(n, d[:128] + bytes(len(d) - 128))], out_kids[k][:16])
    out_kids.update(sway)
    own = body_m.meshes[:n_own]
    pages, parts = model_budget.attire_pages(out_kids)
    if pages > model_budget.LIMIT:
        # over the budget: leave out the target's body pieces the gear covers (95 % of their vertices within
        # 0.6 units of it - Undertaker's coat sits 0.3-0.5 off the torso), the most covered first, until it fits
        posed = body_m.meshes[n_own:]
        G = np.array([v[:3] for s_ in posed for v in s_.verts], float)
        cover = []
        for k, s_ in enumerate(own):
            P = np.array([v[:3] for v in s_.verts], float)
            d = min_dist(P, G)
            if np.percentile(d, 95) < 0.6:
                cover.append((float(np.median(d)), k))
        drop = set()
        for _, k in sorted(cover):
            if pages <= model_budget.LIMIT:
                break
            drop.add(k)
            body_m.meshes = [s_ for j, s_ in enumerate(own) if j not in drop] + posed
            for j, s_ in enumerate(body_m.meshes):
                s_.u74 = j
            out_kids[0] = jboy.write(body_m)
            pages, parts = model_budget.attire_pages(out_kids)
        if drop:
            rep['notes'].append("left out %d of the target's body pieces under the gear (to fit)" % len(drop))
    rep['notes'].append('%d vertex pages of %d' % (pages, model_budget.LIMIT))
    if pages > model_budget.LIMIT:
        rep['warnings'].append('over the vertex budget: the game may leave it out (%d pages)' % pages)
    name4 = tn2[:7] + b'4'
    ents = [(b'EMD ', n, b) for n, b in te if n != name4] + [(b'EMD ', name4, W.pack_kids(out_kids))]
    W.write_epk8(target, ents, out)
    rep.update(ok=True, out=out, entry=name4.decode(), gear_meshes=n_gear, extra_bones=n_extra, pages=pages)
    if report:
        open(report, 'w').write(json.dumps(rep, indent=1))
    print(json.dumps(rep, indent=1))
    return rep


def gear_json(path, target, t_att, out, report=None):
    """gear meshes from a gear.json (the Mod Maker's FBX path: already in 2011 space on the target's A-pose,
    weights by 2011 bone / extra bone names) onto a target attire's kind 4"""
    import svr11_build as B
    rep = {'ok': False, 'notes': [], 'warnings': []}
    g = json.load(open(path, encoding='utf-8'))
    if not g.get('meshes'):
        raise SystemExit('gear.json has no meshes')
    te = entries(target)
    tn2, tb2 = find(te, t_att, 2)
    if not tb2:
        raise SystemExit('the target has no attire %d' % t_att)
    t2 = kids_of(tb2)
    mt = jboy.read(t2[0])
    # the gear built as an attire on the target (template) plus its extra bones: meshes, palettes, sheets
    b = B.Builder({'template': target, 'attires': []}, os.path.dirname(os.path.abspath(path)))
    for m in g['meshes']:
        m['kind'] = 'skin'   # (one model, the skin shader: a hat too)
    spec = {'meshes': g['meshes'], 'textures': g.get('textures') or {}, 'extra_bones': g.get('extra_bones') or []}
    gk, gr = b.gear(spec, 'gear')
    rep['warnings'] += b.warnings
    gm = jboy.read(gk[0])
    gnames = [n['name'] for n in gm.nodes]
    # the target's node table plus the extra bones (positions in 2011 world space, no rotation)
    nodes = [dict(n) for n in mt.nodes]
    tidx = {n['name']: i for i, n in enumerate(nodes)}
    Wt = worlds(t2[0])
    n_extra = 0
    for eb in spec['extra_bones']:
        if eb['name'][:15] in tidx:
            continue
        pi = tidx.get(eb.get('parent'), tidx.get('mune', 0))
        t = Wt[pi][:3, :3].T @ (np.array(eb['position'], float) - Wt[pi][:3, 3])
        n = dict(nodes[pi])
        n.update(name=eb['name'][:15], t=tuple(float(x) for x in t), r=(0.0, 0.0, 0.0),
                 u48=(pi,) + tuple(n.get('u48', (0, 0, 0, 0))[1:]), sphere=(0.0, 0.0, 0.0, 0.0))
        nodes.append(n)
        M = np.eye(4)
        M[:3, 3] = t
        Wt.append(Wt[pi] @ M)
        tidx[n['name']] = len(nodes) - 1
        n_extra += 1

    def halve(uvs, right):
        return [(min(1.0, max(0.0, u)) * 0.5 + (0.5 if right else 0.0), v) for u, v in uvs]
    tex_names = list(mt.textures)
    posed = []
    for s_ in gm.meshes:
        ns = jboy.Mesh()
        ns.__dict__.update(s_.__dict__)
        ns.palette = [tidx.get(gnames[p - 1], 0) + 1 if 0 < p <= len(gnames) else 1 for p in s_.palette]
        params = []
        for name, typ, val in s_.params:
            if typ == 0x0f:
                k = struct.unpack('>i', val[:4])[0]
                if k >= 0:
                    tn = gm.textures[k] if k < len(gm.textures) else ''
                    if tn not in tex_names:
                        tex_names.append(tn)
                    val = struct.pack('>i', tex_names.index(tn)) + val[4:]
            params.append((name, typ, val))
        ns.params = params
        ns.uvs = halve(s_.uvs, True)
        posed.append(ns)
    out_kids = {k: v for k, v in t2.items() if k not in (0x7530, 0x7531, 0x7532, 0xc8, 0xc9)}
    body_m = jboy.Model()
    body_m.__dict__.update(mt.__dict__)
    body_m.nodes = nodes
    own = []
    for s_ in mt.meshes:
        n_ = jboy.Mesh()
        n_.__dict__.update(s_.__dict__)
        n_.uvs = halve(s_.uvs, False)
        own.append(n_)
    body_m.meshes = own + posed
    for k, s_ in enumerate(body_m.meshes):
        s_.u74 = k
    body_m.textures = tex_names
    out_kids[0] = jboy.write(body_m)
    for k in (0x2710, 0x4e20):   # (hair / lashes: the target's, on the left half of the sheet now)
        if k in t2 and t2[k][:4] == b'JBOY':
            hm = jboy.read(t2[k])
            for s_ in hm.meshes:
                s_.uvs = halve(s_.uvs, False)
            hm.nodes = nodes
            out_kids[k] = jboy.write(hm)
    rep['notes'].append('%d gear mesh(es) from %s, %d extra bones' % (len(posed), os.path.basename(path), n_extra))
    dt_a = dict(W.tex_bundle_read(gk[0xa]))
    dt_c = dict(W.tex_bundle_read(gk[0xc]))
    return assemble(rep, te, tn2, out_kids, body_m, len(own), dt_a, dt_c, {}, target, out, report, len(posed), n_extra)


def main(argv):
    args = [a for a in argv[1:]]
    if '--gear-json' in args:
        try:
            gj = args[args.index('--gear-json') + 1]
            target = args[args.index('--onto') + 1]
        except IndexError:
            print(__doc__)
            return 2
        rest = [a for i, a in enumerate(args) if a not in ('--gear-json', '--onto') and
                (i == 0 or args[i - 1] not in ('--gear-json', '--onto'))]
        tp, ta = target.rsplit(',', 1)
        gear_json(gj, tp, int(ta), rest[0], rest[1] if len(rest) > 1 else None)
        return 0
    try:
        donor = args[args.index('--gear-from') + 1]
        target = args[args.index('--onto') + 1]
    except (ValueError, IndexError):
        print(__doc__)
        return 2
    rest = [a for i, a in enumerate(args) if a not in ('--gear-from', '--onto') and
            (i == 0 or args[i - 1] not in ('--gear-from', '--onto'))]
    dp, da = donor.rsplit(',', 1)
    tp, ta = target.rsplit(',', 1)
    transplant(dp, int(da), tp, int(ta), rest[0], rest[1] if len(rest) > 1 else None)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
