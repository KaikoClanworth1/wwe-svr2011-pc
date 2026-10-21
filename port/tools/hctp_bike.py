"""HCTP's motorbike (BG.PAC SOBJ 2035: the Undertaker's, entrances 3003 / 3203 / 3411 / 3511) -> a 2011
evtobj.pac SOBJ entry (entrance_props: id 2542): PACH {0: JBOY, 10: DDS bank}.

The YOBJ's 13 nodes are kept in order behind one model node, as 2011's props have them (2540: node 0 'bike').
A prop track's node k drives the node named 'root' (k = 0) or 'bone0k' - in both games (2540: bone02 / bone03
are its wheels, bone01 / bone04 steer; HCTP 2035: bone01 / bone04 wheels, bone03 the stand) - so HCTP's own
track carries over node for node (hctp_entrance: no 2540 remap). Mesh palettes count nodes from 1. Vertices
stay as the YOBJ has them (model space, bound to their batch's node); textures (RTX3) become DXT1 / DXT5. Same units and axes as 2011 (y down). The PS2's light and shadow
parts are left out.

  python hctp_bike.py <HCTP PAC folder or BG.PAC> <2011 pac folder> <out.bin>
"""
import io
import os
import struct
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'hctp'))
import svrfmt  # noqa: E402
import jboy  # noqa: E402
import arena_tool  # noqa: E402
import svr08_stage as s8  # noqa: E402
import hctp_entrance as HE  # noqa: E402
import yobj  # noqa: E402
import hctp2jboy  # noqa: E402

MODEL = 2035


def source_parts(bg_pac_bytes, model=MODEL):
    """BG.PAC SOBJ <model> -> [unpacked sub-entries]"""
    for g, n, b in HE.dpac(bg_pac_bytes):
        if g == 'SOBJ' and n == str(model):
            return [svrfmt.bpe_decode(s) if s[:4] == b'BPE ' else s for _, s in HE.pac(b)]
    raise KeyError('BG.PAC has no SOBJ %d' % model)


def _dds(img):
    """RGBA uint8 (h, w, 4) -> DDS (DXT1 when opaque, else DXT5) with mips"""
    import svr08_char
    # (magenta fill - the PS2's key colour, n35_09 beside the headlight lens - drawn pink on the lens: the
    # texture's own average colour instead)
    rgb = img[..., :3].astype(int)
    key = (rgb[..., 0] > 180) & (rgb[..., 1] < 90) & (rgb[..., 2] > 150)
    if key.any() and (~key).any():
        img = img.copy()
        img[key, :3] = rgb[~key].mean(0).astype(np.uint8)
    im = Image.fromarray(img, 'RGBA')
    opaque = int(img[..., 3].min()) >= 250
    return svr08_char.dds_encode(im, 'DXT1' if opaque else 'DXT5')


def build(bg_pac_bytes, game_pac, name='bike2035'):
    """-> (entry bytes, notes)"""
    subs = source_parts(bg_pac_bytes)
    models = [s for s in subs if s[:4] == b'YOBJ']
    if not models:
        raise ValueError('SOBJ %d has no YOBJ' % MODEL)
    main = max(models, key=len)                   # (the bike; the small ones are its light and shadow)
    banks = [s for s in subs if len(s) > 48 and s[16 + 16:16 + 20].rstrip(b'\0') == b'txc']
    M = yobj.read(main, apply_nodes=False)   # (its vertices are in model space already, as 2011's props are)
    tex = yobj.texbank(banks[0]) if banks else {}
    _, _, _, _, e78 = s8.stage(os.path.join(game_pac, 'bg', 'bg78.pac'))
    tmpl = hctp2jboy.template({i: s8.unpack(b) for i, b in e78})
    J = hctp2jboy.convert(M, tmpl, name=name, keep_nodes=True, double_sided=True)
    # one model node in front (2011's props: the track's node k is the model's k + 1)
    top = dict(J.nodes[0])
    top.update(name=name[:15], t=(0.0, 0.0, 0.0), r=(0.0, 0.0, 0.0), u48=(0xFFFFFFFF, 0, 0, 0))
    nodes = [top]
    for n in J.nodes:
        p = n['u48'][0]
        p = 0xFFFFFFFF if p == 0xFFFFFFFF or p >= len(J.nodes) else p + 1
        nodes.append(dict(n, u48=(p,) + tuple(n['u48'][1:])))
    J.nodes = nodes
    for s in J.meshes:   # (palette entries count nodes from 1 - 2540's wheels turn about node p - 1: HCTP i -> i + 2)
        s.palette = [p + 2 for p in s.palette]
    notes = []
    dds = []
    for t in J.textures:
        img = tex.get(t)
        if img is None:
            img = np.full((8, 8, 4), 128, np.uint8)
            notes.append('texture %s missing: grey' % t)
        dds.append((t + '.dds', _dds(img)))
    entry = svrfmt.pach_write([(0, jboy.write(J)), (10, arena_tool.tex_bundle_write(dds))])
    notes.append('HCTP SOBJ %d: %d meshes, %d nodes, %d textures, %d bytes' % (
        MODEL, len(J.meshes), len(J.nodes), len(dds), len(entry)))
    return entry, notes


def bg_pac(path):
    if os.path.isdir(path):
        for f in os.listdir(path):
            if f.lower() == 'bg.pac':
                return open(os.path.join(path, f), 'rb').read()
        raise FileNotFoundError('no BG.PAC in %s' % path)
    return open(path, 'rb').read()


if __name__ == '__main__':
    if len(sys.argv) != 4:
        print(__doc__)
        sys.exit(2)
    data, notes = build(bg_pac(sys.argv[1]), sys.argv[2])
    open(sys.argv[3], 'wb').write(data)
    print('\n'.join(notes))
