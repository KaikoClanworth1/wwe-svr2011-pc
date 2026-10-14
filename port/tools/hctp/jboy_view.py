"""Render a 2011 bg pac's models (ids given) into the three.js viewer, from the built file."""
import base64, io, sys, struct
import numpy as np
from PIL import Image
sys.path.insert(0, r"D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\port\tools")
import svr08_stage as s8, jboy, arena_tool as at
import to_glb

class M: pass

def main(pac, ids, out, extra_hmd=None):
    _, _, _, _, e = s8.stage(pac)
    d = {i: s8.unpack(b) for i, b in e}
    tex = {}
    for n, dds in at.tex_bundle(d[400]):
        try:
            tex[n.rsplit('.', 1)[0]] = np.array(Image.open(io.BytesIO(dds)).convert('RGBA'))
        except Exception:
            pass
    models = []
    for i in ids:
        if d.get(i, b'')[:4] != b'JBOY':
            continue
        j = jboy.read(d[i])
        m = M(); m.name = j.name; m.textures = j.textures; m.meshes = []
        for s in j.meshes:
            slot = [struct.unpack('>i', v[:4])[0] for n, t, v in s.params if n == 'texDiffuse']
            slot = slot[0] if slot else -1
            P = np.array([v[:3] for v in s.verts]); N = np.array([v[3:6] for v in s.verts])
            strips = []
            for prim, idx in s.strips:
                st = []
                for k in idx:
                    c = s.verts[k][6]
                    st.append((k, (s.uvs[k][0], s.uvs[k][1], 1.0), (((c >> 16) & 255) / 510, ((c >> 8) & 255) / 510, (c & 255) / 510, (c >> 24) / 510)))
                strips.append(st)
            m.meshes.append(dict(pos=P, nrm=N, subs=[dict(tex=slot, strips=strips)]))
        models.append((f'{i:03d}', m, None))
    debug = []
    R = s8.hmd_read(d[1004])
    debug.append(('collision 1004', [np.array(s8.tri_corners(r)) for r in R], (1, 0.1, 0.1, 0.35)))
    glb = to_glb.build(models, tex, out + '.glb', debug)
    open(out + '.html', 'w').write(to_glb.VIEWER.replace('%GLB%', base64.b64encode(glb).decode()).replace('%TITLE%', 'bg78 ' + out))
    print(out, len(models))

main(sys.argv[1], [int(x) for x in sys.argv[3].split(',')] if len(sys.argv) > 3 else list(range(20)), sys.argv[2])
