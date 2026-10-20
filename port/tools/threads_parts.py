"""Superstar Threads parts for a superstar mod: writes <mod>/threads.txt
(docs/SUPERSTAR_MODS.md, "Superstar Threads").

Threads paints a part where its mask channel is set: part p (1-12) of an attire
is channel (p-1) % 4 of the attire's Mask1/Mask2/Mask3 texture (in the model's
EMD entry, next to `color`). Models converted from WWE '12 / '13 / SvR 2010 bring
their masks; this names each used channel after the body area it covers
(bone weights of the vertices under it, and their height) with the game's own
part names (TRUNKS 1, KNEE PADS, BOOTS DESIGN ...), so the text is translated.
The default colour is the texture's colour under the mask.

    threads_parts.py <mod folder> [--game <Game Files>] [--json] [--dry-run]
    threads_parts.py --names [--game <Game Files>]     the usable part names (JSON)

--json prints {"ok", "file", "attires": [{"attire", "parts": [{"part", "name",
"id", "colour", "area"}]}], "message"} for the Mod Maker. A model without masks
gets no file (exit 3) - its parts can't be painted.
"""
import argparse
import collections
import io
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import jboy  # noqa: E402
import svrfmt  # noqa: E402

GAME = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'Game Files')
FORMAT = 1
AREAS = ('head', 'chest', 'upperarm', 'forearm', 'hand', 'hips', 'thigh', 'shin', 'foot')

# family -> (one base, numbered bases, one design, numbered designs)
FAMILIES = {
    'MASK': ('MASK', ['MASK 1', 'MASK 2'], 'MASK DESIGN', ['MASK DESIGN 1', 'MASK DESIGN 2', 'MASK DESIGN 3', 'MASK DESIGN 4']),
    'SHIRT': ('SHIRT', ['SHIRT 1', 'SHIRT 2', 'SHIRT 3'], 'SHIRT DESIGN', ['SHIRT DESIGN 1', 'SHIRT DESIGN 2', 'SHIRT DESIGN 3']),
    'SINGLET': ('SINGLET', ['SINGLET 1', 'SINGLET 2'], None,
                ['SINGLET DESIGN 1', 'SINGLET DESIGN 2', 'SINGLET DESIGN 3', 'SINGLET DESIGN 4']),
    'ELBOW PADS': ('ELBOW PADS', ['ELBOW PADS 1', 'ELBOW PADS 2'], 'ELBOW PADS DESIGN', []),
    'ARMBANDS': ('ARMBANDS', [], 'ARMBAND DESIGN', []),
    'WRIST TAPE': ('WRIST TAPE', ['WRIST TAPE 1', 'WRIST TAPE 2'], 'WRISTBANDS DESIGN', []),
    'GLOVES': ('GLOVES', ['GLOVES 1', 'GLOVES 2', 'GLOVES 3'], None, []),
    'TRUNKS': ('TRUNKS', ['TRUNKS 1', 'TRUNKS 2'], 'TRUNKS DESIGN',
               ['TRUNKS DESIGN 1', 'TRUNKS DESIGN 2', 'TRUNKS DESIGN 3', 'TRUNKS DESIGN 4']),
    'TIGHTS': ('TIGHTS', ['TIGHTS 1', 'TIGHTS 2', 'TIGHTS 3'], 'TIGHTS DESIGN',
               ['TIGHTS DESIGN 1', 'TIGHTS DESIGN 2', 'TIGHTS DESIGN 3', 'TIGHTS DESIGN 4']),
    'KNEE PADS': ('KNEE PADS', ['KNEE PADS 1', 'KNEE PADS 2', 'KNEE PADS 3'], None,
                  ['KNEE PADS DESIGN 1', 'KNEE PADS DESIGN 2']),
    'KICKPADS': ('KICKPADS', [], 'KICKPADS DESIGN',
                 ['KICKPADS DESIGN 1', 'KICKPADS DESIGN 2', 'KICKPADS DESIGN 3', 'KICKPADS DESIGN 4']),
    'BOOTS': ('BOOTS', ['BOOTS 1', 'BOOTS 2', 'BOOTS 3'], 'BOOTS DESIGN', ['BOOTS DESIGN 1', 'BOOTS DESIGN 2']),
    'SOLES': ('SOLES', [], None, []),
}


def _unpack(b):
    return svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b


# ---- the game's part names: SSTL ids (edit.pac) with their text (string.pac, English group 0600)

def game_names(game):
    """-> {name: id} for every part name a stock Threads record uses"""
    _, groups, _ = svrfmt.epac_read(open(os.path.join(game, 'pac', 'edit', 'edit.pac'), 'rb').read())
    sstl = next(b for t, ents in groups if t == b'EDIT' for n, b in ents if n == b'SSTL')
    rec = _unpack(svrfmt.pach_read(_unpack(sstl))[0][1])
    count = rec[0] | rec[1] << 8
    ids = set()
    for i in range(count):
        ids.update(x for x in struct.unpack_from('<48i', rec, 2 + i * 484 + 4) if x != -1)
    _, groups, _ = svrfmt.epac_read(open(os.path.join(game, 'pac', 'string.pac'), 'rb').read())
    sdb = dict(next(ents for t, ents in groups if t == b'SDB '))[b'0600']
    out = {}
    for _, b in svrfmt.pach_read(sdb):
        u = _unpack(b)
        for k in range(struct.unpack_from('<I', u, 4)[0]):
            off, ln, sid, _ = struct.unpack_from('<4I', u, 8 + 16 * k)
            if sid in ids:
                out.setdefault(u[off:off + ln].split(b'\0')[0].decode('latin1').strip(), sid)
    return out


# ---- the model

def _area(node):
    n = node.lower()
    for k, a in (('yubi', 'hand'), ('_te', 'hand'), ('kote', 'forearm'), ('ninoude', 'upperarm'), ('kata', 'upperarm'),
                 ('sakotsu', 'chest'), ('mune', 'chest'), ('onaka', 'chest'), ('koshi', 'hips'), ('root', 'hips'),
                 ('momo', 'thigh'), ('sune', 'shin'), ('tsumasaki', 'foot'), ('ashi', 'foot')):
        if k in n:
            return a
    return 'head'


def _bundle(raw):
    out = {}
    for k in range(struct.unpack_from('<I', raw, 0)[0]):
        rec = raw[16 + 32 * k:48 + 32 * k]
        size, off = struct.unpack_from('<II', rec, 20)
        out[rec[:16].split(b'\0')[0].decode('latin1').lower()] = raw[off:off + size]
    return out


def pac_attires(path):
    """-> {attire (1-4): (models, textures)} from a model pac's EMD entries"""
    _, groups = ch_tool.epk8_read(open(path, 'rb').read())
    out = {}
    for typ, ents in groups:
        if not typ.startswith(b'EMD'):
            continue
        for name, blob in ents:
            if len(name) != 8 or name[7:8] != b'2' or not name[6:7].isdigit():
                continue
            try:
                kids = svrfmt.pach_read(ch_tool.unpack(blob))
            except ValueError:
                continue
            models, tex = [], {}
            for _, b in kids:
                raw = ch_tool.unpack(b)
                if raw[:4] == b'JBOY':
                    models.append(jboy.read(raw))
                elif ch_tool.is_bundle(raw):
                    tex.update(_bundle(raw))
            out[int(name[6:7]) + 1] = (models, tex)
    return out


def _image(dds):
    import numpy as np
    from PIL import Image
    return np.asarray(Image.open(io.BytesIO(dds)).convert('RGBA'))


def channels(models, tex):
    """-> [None | {cov, share{area}, mid, span, rgb}] per mask channel (4 per mask), or None without masks"""
    import numpy as np
    masks = [tex[k] for k in ('mask1', 'mask2', 'mask3') if k in tex]
    if not masks or 'color' not in tex or not models:
        return None
    color = _image(tex['color'])
    pts = np.array([v[:3] for m in models for s in m.meshes for v in s.verts])
    lo, hi = pts.min(0), pts.max(0)
    up = int(np.argmax(hi - lo))  # (the models' up axis points down: the head is at the low end)
    height = float(hi[up] - lo[up]) or 1.0
    # vertices that sample `color`: (mesh, uv, height 0 feet - 1 head, [(area, weight)])
    verts = []
    for m in models:
        names = [n['name'] for n in m.nodes]
        for s in m.meshes:
            tx = dict((n, v) for n, t, v in s.params).get('texDiffuse')
            ti = int.from_bytes(tx[:4], 'big', signed=True) if tx else -1
            if not (0 <= ti < len(m.textures)) or m.textures[ti] != 'color':
                continue
            nb = len(s.weights)
            flat = [x for blk in s.weights for x in blk]  # (jboy.read splits them per block; they are per vertex)
            for k, (u, v) in enumerate(s.uvs):
                inf = []
                for j in range(nb):
                    b, f = flat[nb * k + j]
                    if f > 0 and b[0] < len(s.palette) and 0 <= s.palette[b[0]] - 1 < len(names):
                        inf.append((_area(names[s.palette[b[0]] - 1]), f))
                verts.append((u % 1.0, v % 1.0, (hi[up] - s.verts[k][up]) / height, inf))
    out = []
    for dds in masks:
        mk = _image(dds)
        h, w = mk.shape[:2]
        for c in range(4):
            ch = mk[:, :, c].astype(np.float32) / 255
            sel = ch > 0.03
            cov = float(sel.mean())
            if cov < 0.0005:
                out.append(None)
                continue
            wgt = collections.Counter()
            ys = []
            for u, v, y, inf in verts:
                val = ch[min(h - 1, int(v * h)), min(w - 1, int(u * w))]
                if val <= 0.03:
                    continue
                ys.append(y)
                for a, f in inf:
                    wgt[a] += f * val
            if not ys:
                out.append(None)
                continue
            tot = sum(wgt.values()) or 1.0
            if color.shape[:2] != (h, w):
                from PIL import Image
                ch = np.asarray(Image.fromarray((ch * 255).astype(np.uint8)).resize(color.shape[1::-1])) / 255.0
                sel = ch > 0.03
            core = ch >= 0.6 * float(ch.max())  # (the mask's core: its soft edges blend into the next part)
            rgb = np.median(color[:, :, :3][core].astype(np.float32), axis=0)
            out.append({'cov': cov, 'share': {a: wgt[a] / tot for a in AREAS},
                        'mid': float(np.median(ys)), 'span': float(np.percentile(ys, 95) - np.percentile(ys, 5)), 'rgb': rgb})
    return out


# ---- naming

def family(c, has_pure_foot):
    s = c['share']
    arms = s['upperarm'] + s['forearm'] + s['hand']
    legs = s['thigh'] + s['shin']
    if s['head'] >= 0.5:
        return 'MASK'
    if s['chest'] >= 0.25 or (s['chest'] >= 0.15 and c['cov'] > 0.05):
        return 'SHIRT' if arms >= 0.2 else 'SINGLET'
    if arms >= 0.6:
        if s['hand'] >= 0.8:
            return 'GLOVES'
        if s['upperarm'] >= 0.3 and s['forearm'] >= 0.3:
            return 'ELBOW PADS'
        if s['upperarm'] >= 0.6:
            return 'ARMBANDS'
        return 'WRIST TAPE'
    if s['hips'] >= 0.6:
        return 'TRUNKS'
    if legs + s['foot'] + s['hips'] >= 0.6 and c['span'] >= 0.3 and c['mid'] >= 0.15:
        return 'TIGHTS'
    if legs >= 0.6 and c['mid'] >= 0.18:
        return 'KNEE PADS' if c['span'] < 0.3 else 'TIGHTS'
    if s['foot'] + s['shin'] >= 0.5 or c['mid'] < 0.18:
        if s['foot'] >= 0.9 and c['mid'] < 0.05 and has_pure_foot == 'soles':
            return 'SOLES'
        if has_pure_foot and s['shin'] >= 0.5 and c['mid'] >= 0.08:
            return 'KICKPADS'
        return 'BOOTS'
    if c['mid'] > 0.8:
        return 'MASK'
    if c['mid'] > 0.6:
        return 'SHIRT'
    return 'TIGHTS' if c['mid'] > 0.2 else 'BOOTS'


def name_parts(chans, known):
    """-> [(part 1-12, name, family)] for the used channels"""
    used = [(i, c) for i, c in enumerate(chans) if c and i < 12]
    feet = [c for _, c in used if c['share']['foot'] >= 0.9 and c['mid'] < 0.05]
    pure = 'soles' if len(feet) >= 2 or (feet and any(c['share']['foot'] >= 0.9 and c['mid'] >= 0.05 or
                                                       c['share']['shin'] < 0.5 and c['mid'] < 0.18 and c not in feet
                                                       for _, c in used)) else bool(feet)
    groups = collections.OrderedDict()
    for i, c in used:
        groups.setdefault(family(c, pure), []).append((i, c))
    torso = groups.pop('SHIRT', []) + groups.pop('SINGLET', [])
    if torso:
        big = max(torso, key=lambda ic: ic[1]['cov'])[1]['share']
        groups['SHIRT' if big['upperarm'] + big['forearm'] + big['hand'] >= 0.2 else 'SINGLET'] = torso
    if 'SOLES' in groups and 'BOOTS' not in groups:
        groups['BOOTS'] = groups.pop('SOLES')
    if len(groups.get('SOLES', [])) > 1:  # (one sole; the rest are boots)
        s = sorted(groups['SOLES'], key=lambda ic: ic[1]['mid'])
        groups['SOLES'], groups['BOOTS'] = s[:1], groups.get('BOOTS', []) + s[1:]
    out = []
    taken = set()
    for fam, items in groups.items():
        one, nums, design, dnums = FAMILIES[fam]
        top = max(c['cov'] for _, c in items)
        bases = sorted([ic for ic in items if ic[1]['cov'] >= 0.5 * top], key=lambda ic: ic[0])
        designs = sorted([ic for ic in items if ic[1]['cov'] < 0.5 * top], key=lambda ic: ic[0])
        if not design and not dnums:  # (no design names: all bases)
            bases, designs = sorted(items, key=lambda ic: ic[0]), []
        plan = []
        plan += [one] if len(bases) == 1 else (nums + [one] * len(bases))[:len(bases)]
        if designs:
            pool = ([design] if len(designs) == 1 and design else []) + dnums + ([design] if design else [])
            pool += [pool[-1] if pool else one] * len(designs)
            plan += pool[:len(designs)]
        for (i, c), nm in zip(bases + designs, plan):
            if nm not in known:  # (a name the game doesn't have: the family's plain one)
                nm = one
            if nm in taken and fam in ('BOOTS', 'TIGHTS', 'TRUNKS', 'KNEE PADS', 'SHIRT') and nums:
                nm = next((n for n in nums if n not in taken and n in known), nm)
            taken.add(nm)
            out.append((i + 1, nm, fam, c))
    return sorted(out)


def colour(rgb):
    """the Threads default for the texture's mean colour: brightened as the game's defaults are, at least 0x15"""
    return '%02x%02x%02x' % tuple(max(0x15, min(255, int(round(255 * (float(x) / 255) ** 0.6)))) for x in rgb)


# ---- the mod

def manifest(folder):
    out = {}
    p = os.path.join(folder, 'manifest.txt')
    if os.path.exists(p):
        for line in open(p, encoding='utf-8', errors='replace'):
            if '=' in line and not line.lstrip().startswith('#'):
                k, v = line.split('=', 1)
                out[k.strip().lower()] = v.strip()
    return out


def mod_attires(folder):
    """-> {attire: (models, textures)}: ch.pac's attires, attireN= pacs' attire 1 in their place"""
    out = pac_attires(os.path.join(folder, 'ch.pac'))
    man = manifest(folder)
    for n in (2, 3, 4):
        f = man.get('attire%d' % n)
        if f and os.path.exists(os.path.join(folder, f)):
            other = pac_attires(os.path.join(folder, f))
            if 1 in other:
                out[n] = other[1]
    return out


def build(folder, game):
    known = game_names(game)
    result = []
    for attire, (models, tex) in sorted(mod_attires(folder).items()):
        chans = channels(models, tex)
        if not chans:
            continue
        parts = [{'part': p, 'name': nm, 'id': known[nm], 'colour': colour(c['rgb']), 'area': fam.lower()}
                 for p, nm, fam, c in name_parts(chans, known)]
        if parts:
            result.append({'attire': attire, 'parts': parts})
    return result


def write(folder, attires):
    lines = ['# Superstar Threads parts (threads_parts.py %d): attire.part = name id, default colour  # name' % FORMAT,
             '# part p paints channel (p-1)%4 of the attire\'s Mask1/2/3 (R G B A). Edit freely; the game checks it.',
             'format=%d' % FORMAT]
    for a in attires:
        for p in a['parts']:
            lines.append('%d.%d=%d,%s  # %s' % (a['attire'], p['part'], p['id'], p['colour'], p['name']))
    path = os.path.join(folder, 'threads.txt')
    tmp = path + '.part'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    os.replace(tmp, path)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('folder', nargs='?')
    ap.add_argument('--game', default=GAME)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--names', action='store_true')
    a = ap.parse_args()
    if a.names:
        print(json.dumps(sorted(game_names(a.game).items())))
        return 0
    if not a.folder or not os.path.exists(os.path.join(a.folder, 'ch.pac')):
        msg = 'no ch.pac in %s' % a.folder
        print(json.dumps({'ok': False, 'message': msg}) if a.json else msg)
        return 2
    attires = build(a.folder, a.game)
    if not attires:
        msg = 'its model has no Threads masks (Mask1/Mask2): no parts to paint'
        print(json.dumps({'ok': False, 'attires': [], 'message': msg}) if a.json else msg)
        return 3
    path = None if a.dry_run else write(a.folder, attires)
    if a.json:
        print(json.dumps({'ok': True, 'file': path, 'attires': attires,
                          'message': '%d part(s) in %d attire(s)' % (sum(len(x['parts']) for x in attires), len(attires))}))
    else:
        for x in attires:
            print('attire %d: %s' % (x['attire'], ', '.join('%d %s #%s' % (p['part'], p['name'], p['colour']) for p in x['parts'])))
        print(path or '(dry run)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
