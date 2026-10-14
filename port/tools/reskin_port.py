"""Belt and weapon reskins as texture packs (shared by belt_port.py / weapon_port.py).

A reskin replaces a stock texture by its content hash (native/texture_packs.h:
any file under "Texture Packs\\<pack>\\" whose name ends with _<hash16> .dds
or .png). The stock textures and their hashes are in data/belt_weapon_textures.json
(tools/belt_weapon_table.py), so this needs no xxhash.

    py tools/belt_port.py games
    py tools/belt_port.py list --game svr2011 --src <SvR 2011 game folder or ISO> [--pictures <dir>]
    py tools/belt_port.py convert --game svr2011 --src <...> --item <key> --out <dir>
        --set <texture>=<picture> [--set ...] [--pack <name>]

(weapon_port.py the same.) list --pictures writes each stock texture as
<dir>/<item>/<texture>.png and gives its path. convert writes
<out>/<pack>/<owner>_<texture>_<hash16>.dds for each --set (DXT1 where the
stock is DXT1, else DXT5, with a mip chain; any size, a multiple of 4), plus
<out>/report.json; <pack> defaults to "<Item name> (reskin)".
stdout is JSON only; exit 0 ok / 2 bad input / 3 failed.
"""
import argparse
import io
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discfs  # noqa: E402
import svrfmt  # noqa: E402

TABLE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'belt_weapon_textures.json')


class BadInput(Exception):
    pass


def progress(p, text):
    print('progress %d %s' % (p, text), file=sys.stderr, flush=True)


def games(kind):
    rest = [('svr2007', "WWE SmackDown vs. Raw 2007", '545107E0'), ('svr2008', "WWE SmackDown vs. Raw 2008", '5451080B'),
            ('svr2009', "WWE SmackDown vs. Raw 2009", '54510826'), ('svr2010', "WWE SmackDown vs. Raw 2010", '54510844'),
            ('wwe12', "WWE '12", '54510890'), ('wwe13', "WWE '13", '545108B4'), ('wwe2k14', 'WWE 2K14', '545408B2')]
    out = [{'id': 'svr2011', 'name': 'WWE SmackDown vs. Raw 2011 (reskin its own %ss)' % kind, 'platform': 'x360',
            'disc_hint': '5451085D', 'status': 'ready'}]
    out += [{'id': i, 'name': n, 'platform': 'x360', 'disc_hint': h, 'status': 'research'} for i, n, h in rest]
    return out


def table(kind):
    with open(TABLE, encoding='utf-8') as f:
        return json.load(f)['belts' if kind == 'belt' else 'weapons']


def stock_dds(disc, tex):
    """A stock texture's DDS, found by its table 'where' (group/entry/child ids)."""
    data = disc.read(tex['pac'])
    _, groups, _ = svrfmt.epac_read(data)
    parts = tex['where'].split('/')

    def un(b):
        return svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b

    blob = None
    if parts[0] == 'SOBJ':
        for _, ents in groups:
            for nm, b in ents:
                if nm.split(b'\0')[0].decode('latin-1') == parts[1]:
                    blob = b
        ids = parts[2:]
    else:  # STG/WPON/<pack>/<child>[/<child>]
        for typ, ents in groups:
            if typ.strip() != b'STG':
                continue
            for nm, b in ents:
                if nm.split(b'\0')[0].decode('latin-1') == parts[1]:
                    blob = b
        ids = parts[2:]
    if blob is None:
        raise RuntimeError('%s: %s not found' % (tex['pac'], tex['where']))
    for i in ids:
        kids = dict(svrfmt.pach_read(un(blob)))
        blob = kids[int(i, 16)]
    raw = un(blob)
    import struct
    n = struct.unpack_from('<I', raw, 0)[0]
    for k in range(n):
        r = raw[16 + 32 * k:48 + 32 * k]
        if r[:16].split(b'\0')[0].decode('latin-1') == tex['name']:
            size, off = struct.unpack_from('<II', r, 20)
            return raw[off:off + size]
    raise RuntimeError('%s: texture %s not in %s' % (tex['pac'], tex['name'], tex['where']))


def encode(img, fmt):
    """A DDS with a full mip chain: DXT1 (no alpha) or DXT5."""
    import struct
    from PIL import Image
    img = img.convert('RGBA')
    w, h = max(4, img.width // 4 * 4), max(4, img.height // 4 * 4)
    if (w, h) != img.size:
        img = img.resize((w, h), Image.LANCZOS)
    pf = 'DXT1' if fmt == 'DXT1' else 'DXT5'
    out = None
    mips = 0
    while True:
        b = io.BytesIO()
        img.save(b, 'DDS', pixel_format=pf)
        v = b.getvalue()
        out = bytearray(v) if out is None else out + v[128:]
        mips += 1
        if img.width <= 4 or img.height <= 4:
            break
        half = (img.width // 2, img.height // 2)
        if img.mode == 'RGBA':   # (RGB and A on their own: Pillow premultiplies, and a low alpha - the colour
            a = img.getchannel('A').resize(half, Image.BOX)   # sheets' specular level - turns the RGB to specks)
            img = img.convert('RGB').resize(half, Image.BOX)
            img.putalpha(a)
        else:
            img = img.resize(half, Image.BOX)
    struct.pack_into('<I', out, 8, struct.unpack_from('<I', out, 8)[0] | 0x20000)
    struct.pack_into('<I', out, 28, mips)
    struct.pack_into('<I', out, 108, struct.unpack_from('<I', out, 108)[0] | 0x400008)
    return bytes(out)


def safe(name):
    return re.sub(r'[<>:"/\\|?*]', '_', name).strip() or 'pack'


def run(kind, argv):
    ap = argparse.ArgumentParser(prog='%s_port.py' % kind)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('games')
    p = sub.add_parser('list')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11')
    p.add_argument('--pictures')
    p = sub.add_parser('convert')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--item', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--set', action='append', default=[])
    p.add_argument('--pack')
    try:
        a = ap.parse_args(argv)
    except SystemExit:
        print(json.dumps({'ok': False, 'error': 'bad arguments'}))
        return 2
    if a.cmd == 'games':
        print(json.dumps(games(kind)))
        return 0
    try:
        if a.game != 'svr2011':
            if a.game not in {g['id'] for g in games(kind)}:
                raise BadInput('unknown game %r' % a.game)
            print(json.dumps({'ok': False, 'out': None, 'opens': kind, 'warnings': [], 'dropped': [],
                              'notes': ["%ss from other games need model swaps (game side not there yet)" % kind]}))
            return 3
        if not os.path.exists(a.src):
            raise BadInput('--src %s: not found' % a.src)
        items = table(kind)
        with discfs.open(a.src) as disc:
            if a.cmd == 'list':
                out = []
                for it in items:
                    texs = []
                    for t in it['textures']:
                        e = {k: t[k] for k in ('name', 'owner', 'width', 'height', 'format', 'hash16')}
                        if a.pictures:
                            from PIL import Image
                            folder = os.path.join(a.pictures, safe(it['key']))
                            os.makedirs(folder, exist_ok=True)
                            png = os.path.join(folder, safe(t['name']) + '.png')
                            if not os.path.exists(png):
                                Image.open(io.BytesIO(stock_dds(disc, t))).save(png)
                            e['picture'] = png
                        texs.append(e)
                    out.append({'key': it['key'], 'name': it['name'], 'textures': texs, 'in_2011': it['key']})
                print(json.dumps(out))
                return 0
            it = next((x for x in items if x['key'].lower() == a.item.lower()), None)
            if not it:
                raise BadInput('item %r: not in the list' % a.item)
            if not a.set:
                raise BadInput('nothing to replace: give --set <texture>=<picture>')
            from PIL import Image
            pack = safe(a.pack or '%s (reskin)' % it['name'])
            folder = os.path.join(a.out, pack)
            os.makedirs(folder, exist_ok=True)
            notes, warnings = [], []
            for k, s in enumerate(a.set):
                if '=' not in s:
                    raise BadInput('--set %r: <texture>=<picture>' % s)
                tname, pic = s.split('=', 1)
                tex = next((t for t in it['textures'] if t['name'].lower() == tname.lower()), None)
                if not tex:
                    raise BadInput('%s has no texture %r (%s)' % (it['name'], tname, ', '.join(t['name'] for t in it['textures'])))
                if not os.path.exists(pic):
                    raise BadInput('%s: not found' % pic)
                progress(10 + 80 * k // len(a.set), tex['name'])
                img = Image.open(pic)
                if img.width * tex['height'] != img.height * tex['width']:
                    warnings.append('%s: %dx%d, the stock is %dx%d (other proportions stretch on the model)'
                                    % (tex['name'], img.width, img.height, tex['width'], tex['height']))
                name = '%s_%s_%s.dds' % (tex['owner'], safe(tex['name']), tex['hash16'])
                with open(os.path.join(folder, name), 'wb') as f:
                    f.write(encode(img, tex['format']))
            notes.append('a texture pack: copy "%s" into the game\'s Texture Packs folder and turn it on '
                         '(native renderer)' % pack)
            if kind == 'belt':
                notes.append('the same pictures show on the belt in entrances, celebrations, cut scenes and '
                             'the belt picked up in a match where the title uses that texture')
            rep = {'ok': True, 'out': folder, 'opens': kind, 'warnings': warnings, 'dropped': [], 'notes': notes}
            with open(os.path.join(a.out, 'report.json'), 'w') as f:
                json.dump(rep, f, indent=1)
            progress(100, 'done')
            print(json.dumps(rep))
            return 0
    except BadInput as e:
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 2
    except Exception as e:  # noqa: BLE001
        print('failed: %r' % e, file=sys.stderr)
        print(json.dumps({'ok': False, 'error': str(e), 'out': None, 'opens': kind, 'warnings': [], 'dropped': [],
                          'notes': []}))
        return 3
