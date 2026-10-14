"""HCTP (PS2) YOBJ -> SvR2011 JBOY (static, world space, one node).

Every HCTP submesh (one texture) becomes one JBOY mesh made from a template
mesh (bg78 park_floor01 mesh 0: yDefault, vertex format 1 = vertex colours),
with its texDiffuse slot pointing at the submesh's texture. HCTP strips (one per
GIF packet, with per-corner uv + colour) are welded on (index, uv, colour) and
joined into one strip with degenerates, each source strip starting on an even
position so its winding is kept. Same units and axes as 2011 (y down).
"""
import copy
import struct
import sys

import numpy as np

sys.path.insert(0, r"D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\port\tools")
import jboy  # noqa: E402

import yobj  # noqa: E402


def sphere_of(P):
    P = np.asarray(P, np.float64)
    lo, hi = P.min(0), P.max(0)
    c = (lo + hi) / 2
    return (float(c[0]), float(c[1]), float(c[2]), float(np.sqrt(((P - c) ** 2).sum(1)).max()) + 0.01)


def argb(c, tone=1.0):
    r, g, b, a = (max(0, min(255, int(round(x * 255 * (tone if i < 3 else 1.0))))) for i, x in enumerate(c))
    return (a << 24) | (r << 16) | (g << 8) | b


def convert(M, tmpl_model, name=None, tone=1.0, xform=None, tex_rename=None, max_verts=0xFFFF, merge=True,
            keep_nodes=False):
    """M: yobj.Model (world space). tmpl_model: jboy.Model whose meshes[0] and
    node are the template. xform(P) -> P for positions (normals rotate with
    xform's linear part if given as (R, t)). Returns jboy.Model."""
    tm = tmpl_model.meshes[0]
    out = copy.copy(tmpl_model)
    out.name = (name or M.name)[:15]
    out.textures = []
    out.meshes = []
    tex_rename = tex_rename or {}
    for mesh in M.meshes:
        P = mesh['pos']
        N = mesh['nrm']
        VN = mesh.get('node')
        if xform is not None:
            R, t = xform
            P = P @ R + t
            N = N @ R
        for sub in mesh['subs']:
            tname = M.textures[sub['tex']] if sub['tex'] < len(M.textures) else None
            verts, uvs, key, vnodes = [], [], {}, []
            strip = []
            for st in sub['strips']:
                if len(st) < 3:
                    continue
                ids = []
                for vi, stq, c in st:
                    vi &= 0xFFFF
                    q = stq[2] or 1.0
                    uv = (stq[0] / q, stq[1] / q)
                    col = argb(c, tone)
                    k = (vi, round(uv[0], 5), round(uv[1], 5), col)
                    if keep_nodes and VN is not None and vi < len(VN):
                        vnodes_key = int(VN[vi])
                    else:
                        vnodes_key = 0
                    if k not in key:
                        key[k] = len(verts)
                        p, n = P[vi], N[vi]
                        verts.append((float(p[0]), float(p[1]), float(p[2]), float(n[0]), float(n[1]), float(n[2]), col))
                        uvs.append(uv)
                        vnodes.append(vnodes_key)
                    ids.append(key[k])
                if strip:
                    strip += [strip[-1], ids[0]]
                    if len(strip) % 2:
                        strip.append(ids[0])
                strip += ids
            if not strip:
                continue
            if len(verts) > max_verts:
                raise SystemExit(f'{M.name}: {len(verts)} vertices in one mesh')
            s = copy.copy(tm)
            s.verts = verts
            s.uvs = uvs
            if keep_nodes:
                pal = sorted(set(vnodes))
                if len(pal) > 20:
                    raise SystemExit(f'{M.name}: {len(pal)} nodes in one mesh')
                s.palette = pal
                s.weights = [[((pal.index(v), 0, 0, 0), 1.0) for v in vnodes]]
            else:
                s.weights = [[(tm.weights[0][0][0], 1.0)] * len(verts)]
            s.strips = [(6, strip)]
            s.u04 = 1
            if tname is not None:
                tname = tex_rename.get(tname, tname)
                if tname not in out.textures:
                    out.textures.append(tname)
                slot = out.textures.index(tname)
            else:
                slot = -1
            s.params = [(n, t, struct.pack('>i', slot) + v[4:]) if n == 'texDiffuse' else (n, t, v)
                        for n, t, v in tm.params]
            s.sphere = sphere_of([v[:3] for v in verts])
            s.raw = s.raw[:0xA4] + struct.pack('>4f', *s.sphere) + s.raw[0xB4:]
            out.meshes.append(s)
    if not out.meshes:
        return None
    if merge:
        out.meshes = merge_by_texture(out.meshes)
    if keep_nodes and len(M.nodes) > 1:
        out.nodes = []
        for i, n in enumerate(M.nodes):
            out.nodes.append(dict(name=n['name'][:15], t=tuple(n['t']), r=tuple(n['r']), u40=0, parent=0,
                                  u48=((n['parent'] & 0xFFFFFFFF), 0, 0, 0), sphere=tuple(n['sphere'])))
        out.nodes[0]['sphere'] = sphere_of([v[:3] for s in out.meshes for v in s.verts])
        return out
    node = dict(tmpl_model.nodes[0])
    node['name'] = out.name
    node['sphere'] = sphere_of([v[:3] for s in out.meshes for v in s.verts])
    out.nodes = [node]
    return out


def template(bg78_d):
    """bg78 entries dict -> jboy.Model of park_floor01 (id 0)."""
    return jboy.read(bg78_d[0])


def _slot(s):
    return [struct.unpack('>i', v[:4])[0] for n, t, v in s.params if n == 'texDiffuse'][0]


def merge_by_texture(meshes, max_verts=0xFFFF):
    """Meshes with the same texture slot joined into one (strips joined with
    degenerates, each source strip on an even position)."""
    out, by = [], {}
    for s in meshes:
        k = _slot(s)
        m = by.get(k)
        pal = sorted(set(m.palette) | set(s.palette)) if m is not None else []
        if m is not None and len(m.verts) + len(s.verts) <= max_verts and len(pal) <= 20:
            base = len(m.verts)
            remap = lambda ws, p: [((pal.index(p[b[0]]) if b[0] < len(p) else b[0], 0, 0, 0), w) for b, w in ws]
            m.weights = [remap(m.weights[0], m.palette) + remap(s.weights[0], s.palette)]
            m.palette = pal
            m.verts = m.verts + s.verts
            m.uvs = m.uvs + s.uvs
            a = list(m.strips[0][1])
            b = [i + base for i in s.strips[0][1]]
            a += [a[-1], b[0]]
            if len(a) % 2:
                a.append(b[0])
            m.strips = [(6, a + b)]
            m.sphere = sphere_of([v[:3] for v in m.verts])
            m.raw = m.raw[:0xA4] + struct.pack('>4f', *m.sphere) + m.raw[0xB4:]
            continue
        s = copy.copy(s)
        by[k] = s
        out.append(s)
    return out
