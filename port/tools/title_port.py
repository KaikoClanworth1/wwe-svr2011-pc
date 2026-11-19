"""Title mods (docs/TITLE_MODS.md): a championship's name, belt textures and menu picture replaced.

    py tools/title_port.py titles
    py tools/title_port.py make --game <SvR 2011 game folder or ISO> --title <key> --out <dir> [--name <name>]
        [--short <name>] [--champion <name>] [--picture <png>] [--art <png>] [--set <belt texture>=<picture> ...] [--id <folder>] [--author <name>] [--version <v>] [--svrmod]
    py tools/title_port.py make --new --name <name> --out <dir> [--belt <key>] [--like <key>] [--brand raw|smackdown|both]
        [--tag] [--short <name>] [--champion <name>] [--id <folder>] [--author <name>] [--version <v>] [--svrmod]

titles lists the keys (IC, US, WWE, WHC, WWE_TAG, WORLD_TAG, ECW, DIVAS, WOMENS, HARDCORE, CRUISER, MILLION, WCW)
with their names, belt textures (data/belt_weapon_textures.json) and menu pictures (data/title_pictures.json).
make writes <out>/<id>/ - manifest.txt (type=title) and textures/ named as in texture packs
(<owner>_<texture>_<hash16>.dds / .png: the game serves them in place of the stock ones) - and with --svrmod
<out>/<id>.svrmod as well; <out>/report.json. The picture (--picture) goes in for the title's menu picture in
both menu sizes (menuHD / menuSD), any size. --new makes a title beside the game's instead of replacing one: --belt is
the game's belt it carries, --like the title whose rank class it counts in, --brand puts it in Universe. stdout is JSON
only; exit 0 ok / 2 bad input / 3 failed.
belts --game <svr2008|svr2009|svr2010> --src <disc> lists that game's title belts (the SOBJ entries 2011's title
uses, in its pac/evt/_EvtObj.pac) and which differ from 2011's own; make --belt-from <game> --belt-src <disc> puts
that game's belt for --title in the mod (whole entries, pacentry lines in pack.txt: the move packs add them).
"""
import argparse
import io
import json
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import discfs  # noqa: E402
import reskin_port  # noqa: E402

KEYS = ['IC', 'US', 'WWE', 'WHC', 'WWE_TAG', 'WORLD_TAG', 'ECW', 'DIVAS', 'WOMENS', 'HARDCORE', 'CRUISER', 'MILLION', 'WCW']


NAMES = {'WCW': 'WCW Championship'}  # (no belt table)


class BadInput(Exception):
    pass


def pictures(part='pictures'):
    with open(os.path.join(HERE, 'data', 'title_pictures.json')) as f:
        return json.load(f).get(part, {})


def titles():
    belts = {b['key']: b for b in reskin_port.table('belt')}
    pics = pictures()
    out = []
    for k in KEYS:
        b = belts.get(k, {})
        used = {}
        for o in belts.values():
            if o['key'] != k:
                for t in o['textures']:
                    used[t['hash16']] = True
        out.append({'key': k, 'name': b.get('name', NAMES.get(k, k)), 'belt_textures': [t['name'] for t in b.get('textures', [])],
                    'belt': [{'name': t['name'], 'width': t['width'], 'height': t['height'], 'shared': t['hash16'] in used}
                             for t in b.get('textures', [])],
                    'pictures': [{x: p[x] for x in ('name', 'owner', 'width', 'height')} for p in pics.get(k, [])],
                    'art': [{x: p[x] for x in ('name', 'owner')} for p in pictures('art').get(k, [])]})
    return out


# (the keys the game reads for belt= and like=: src/title_new.cpp)
NEW_KEYS = ['WWE', 'WHC', 'WWE_TAG', 'WORLD_TAG', 'IC', 'US', 'CRUISER', 'WOMENS', 'HARDCORE', 'ECW', 'WCW', 'DIVAS', 'UNIFIED_TAG', 'MILLION']


def make_new(a):
    if not (a.name or '').strip():
        raise BadInput('a new title needs --name')
    if a.title or a.picture or a.art or a.set:
        raise BadInput('--new makes a title of its own: no --title, --picture, --art or --set yet')
    for opt, v in (('--belt', a.belt), ('--like', a.like)):
        if v and v.upper() not in NEW_KEYS:
            raise BadInput('%s %r: one of %s' % (opt, v, ', '.join(NEW_KEYS)))
    if a.brand and a.brand not in ('raw', 'smackdown', 'both'):
        raise BadInput('--brand %r: raw, smackdown or both' % a.brand)
    folder_id = re.sub(r'[^a-z0-9]+', '_', (a.id or a.name).lower()).strip('_')[:40]
    if not folder_id:
        raise BadInput('--id: letters or digits, please (the name has none)')
    mod = os.path.join(a.out, folder_id)
    os.makedirs(mod, exist_ok=True)
    lines = ['type=title', 'new=1', 'id=' + folder_id, 'name=' + a.name]
    for k, v in (('champion', a.champion), ('short', a.short), ('belt', a.belt and a.belt.upper()),
                 ('like', a.like and a.like.upper()), ('brand', a.brand), ('kind', 'tag' if a.tag else None), ('author', a.author)):
        if v:
            lines.append('%s=%s' % (k, v))
    return finish(a, mod, folder_id, lines, [], ['new title "%s"%s' % (a.name, ' in Universe' if a.brand else '')])


def finish(a, mod, folder_id, lines, warnings, notes):
    lines += ['made_with=' + (a.made_with or 'Port tools (title_port)'), 'version=' + (a.version or '1.0')]
    with open(os.path.join(mod, 'manifest.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    rep = {'ok': True, 'out': mod, 'opens': 'title', 'warnings': warnings, 'dropped': [], 'notes': notes}
    if a.svrmod:
        path = os.path.join(a.out, folder_id + '.svrmod')
        with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
            for root, _, files in os.walk(mod):
                for fn in files:
                    full = os.path.join(root, fn)
                    z.write(full, os.path.relpath(full, mod).replace(os.sep, '/'))
        rep['svrmod'] = path
    with open(os.path.join(a.out, 'report.json'), 'w') as f:
        json.dump(rep, f, indent=1)
    reskin_port.progress(100, 'done')
    return rep


# Belts of SvR 2008-2010 (suggestion 5ee174): the same SOBJ entry family as 2011's pac/evt/evtobj.pac, in their
# pac/evt/_EvtObj.pac - a model (JBOY, part 0), its textures (part 0xA) and the attach text (0x64). 2011's own entries
# add a second texture set and bend data (0x1E, 0x3E8, 0x3F2) to most belts, but some of its belts (Hardcore's 2015 /
# 2025, 2309, 2310, 2513) are the old three parts byte for byte, so an old entry loads as it is.
BELT_GAMES = {'svr2008': 'WWE SmackDown vs. Raw 2008', 'svr2009': 'WWE SmackDown vs. Raw 2009',
              'svr2010': 'WWE SmackDown vs. Raw 2010'}
OLD_OBJ_PAC = 'pac/evt/_EvtObj.pac'
OBJ_PAC_2011 = 'pac/evt/evtobj.pac'


def sobj_entries(data):
    import svrfmt
    out = {}
    for typ, ents in svrfmt.epac_read(data)[1]:
        if typ == b'SOBJ':
            for name, blob in ents:
                if name.isdigit():
                    out[int(name)] = blob
    return out


def belt_model(blob):
    """the entry's model part (JBOY), for telling belts apart"""
    import svrfmt
    unp = lambda b: svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b  # noqa: E731
    b = unp(blob)
    if b[:4] != b'PACH':
        return b
    for i, x in svrfmt.pach_read(b):
        if i == 0:
            return unp(x)
    return b''


def old_belts(game, src):
    if game not in BELT_GAMES:
        raise BadInput('--game %r: one of %s' % (game, ', '.join(BELT_GAMES)))
    if not src or not os.path.exists(src):
        raise BadInput('--src: the %s disc (an .iso or a folder)' % BELT_GAMES[game])
    try:
        return sobj_entries(discfs.open(src).read(OLD_OBJ_PAC))
    except BadInput:
        raise
    except Exception as e:  # noqa: BLE001
        raise BadInput('%s has no %s (%s): is it %s?' % (src, OLD_OBJ_PAC, e, BELT_GAMES[game]))


def belts(a):
    """the source game's title belts and whether each differs from 2011's (needs --game11 for that: the 2011 game folder)"""
    old = old_belts(a.game, a.src)
    mine = {}
    if a.game11:
        p = os.path.join(a.game11, *OBJ_PAC_2011.split('/'))
        if os.path.exists(p):
            with open(p, 'rb') as f:
                mine = sobj_entries(f.read())
    out = []
    for b in reskin_port.table('belt'):
        ids = [i for i in b['sobj'] if i in old]
        if not ids:
            continue
        same = None
        if mine:
            same = all(i in mine and belt_model(mine[i]) == belt_model(old[i]) for i in ids)
        out.append({'key': b['key'], 'name': b['name'], 'entries': len(ids), 'same_as_2011': same})
    return {'ok': True, 'game': a.game, 'belts': out}


def add_belt(a, key, mod, notes):
    """the source game's belt entries for title `key` into mod/belt/, and pack.txt's pacentry lines"""
    old = old_belts(a.belt_from, a.belt_src)
    belt = next((b for b in reskin_port.table('belt') if b['key'] == key), None)
    if not belt:
        raise BadInput('the %s title has no belt in 2011 to replace' % key)
    ids = [i for i in belt['sobj'] if i in old]
    if not ids:
        raise BadInput('%s has no %s belt' % (BELT_GAMES[a.belt_from], key))
    os.makedirs(os.path.join(mod, 'belt'), exist_ok=True)
    lines = []
    for i in ids:
        with open(os.path.join(mod, 'belt', '%d.bin' % i), 'wb') as f:
            f.write(old[i])
        lines.append('pacentry evt\\evtobj.pac SOBJ %d belt/%d.bin' % (i, i))
    with open(os.path.join(mod, 'pack.txt'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    notes.append('belt: %s\'s %s belt (%d models: on the shoulder, round the waist, held up, in cut scenes)'
                 % (BELT_GAMES[a.belt_from], key, len(ids)))
    return ids


def make(a):
    if a.new:
        return make_new(a)
    if not a.title:
        raise BadInput('--title: one of %s (or --new)' % ', '.join(KEYS))
    key = a.title.upper()
    if key not in KEYS:
        raise BadInput('--title %r: one of %s' % (a.title, ', '.join(KEYS)))
    if not (a.name or a.picture or a.art or a.set or a.belt_from):
        raise BadInput('nothing to replace: give --name, --picture, --art, --set or --belt-from')
    if a.belt_from and a.set:
        raise BadInput('--belt-from brings its own belt textures: no --set with it')
    from PIL import Image
    folder_id = re.sub(r'[^a-z0-9]+', '_', (a.id or ('%s_%s' % (a.name or 'title', key))).lower()).strip('_')[:40]
    mod = os.path.join(a.out, folder_id)
    tex = os.path.join(mod, 'textures')
    os.makedirs(tex, exist_ok=True)
    notes, warnings = [], []
    if a.picture:
        if not os.path.exists(a.picture):
            raise BadInput('%s: not found' % a.picture)
        img = Image.open(a.picture).convert('RGBA')
        for p in pictures()[key]:
            if img.width * p['height'] != img.height * p['width']:
                warnings.append('the picture is %dx%d, the menu\'s is %dx%d (other proportions stretch)'
                                % (img.width, img.height, p['width'], p['height']))
            img.save(os.path.join(tex, '%s_%s_%s.png' % (p['owner'], p['name'], p['hash16'])))
        notes.append('menu picture: title select, championship screens (menuHD and menuSD)')
    if a.art:  # (the championship screens' art, BG_<title>: Championship Scramble's picture beside the holder)
        if not os.path.exists(a.art):
            raise BadInput('%s: not found' % a.art)
        art = pictures('art').get(key, [])
        if not art:
            raise BadInput('the %s title has no championship art in the game (the tag titles)' % key)
        img = Image.open(a.art).convert('RGBA')
        for p in art:
            img.save(os.path.join(tex, '%s_%s_%s.png' % (p['owner'], p['name'], p['hash16'])))
        notes.append('championship art: the picture beside the holder (Championship Scramble; menuHD and menuSD)')
    if a.set:
        belt = next((b for b in reskin_port.table('belt') if b['key'] == key), None)
        if not belt:
            raise BadInput('no belt texture table for %s: give --name / --picture only' % key)
        for k, s in enumerate(a.set):
            if '=' not in s:
                raise BadInput('--set %r: <texture>=<picture>' % s)
            tname, pic = s.split('=', 1)
            t = next((t for t in belt['textures'] if t['name'].lower() == tname.lower()), None)
            if not t:
                raise BadInput('the %s belt has no texture %r (%s)' % (key, tname, ', '.join(x['name'] for x in belt['textures'])))
            if not os.path.exists(pic):
                raise BadInput('%s: not found' % pic)
            reskin_port.progress(10 + 80 * k // len(a.set), t['name'])
            with open(os.path.join(tex, '%s_%s_%s.dds' % (t['owner'], reskin_port.safe(t['name']), t['hash16'])), 'wb') as f:
                f.write(reskin_port.encode(Image.open(pic), t['format']))
        notes.append('belt textures: %d (entrances, celebrations, cut scenes, the belt picked up in a match)' % len(a.set))
    if a.belt_from:
        add_belt(a, key, mod, notes)
    lines = ['type=title', 'id=' + folder_id, 'title=' + key]
    if a.belt_from:
        lines.append('belt_from=' + a.belt_from)
    if a.name:
        lines.append('name=' + a.name)
        notes.append('name: "%s" wherever the game names the %s title' % (a.name, key))
    if a.champion:
        lines.append('champion=' + a.champion)
    if a.short:
        lines.append('short=' + a.short)
    if a.author:
        lines.append('author=' + a.author)
    return finish(a, mod, folder_id, lines, warnings, notes)


def main(argv):
    ap = argparse.ArgumentParser(prog='title_port.py')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('titles')
    b = sub.add_parser('belts')
    b.add_argument('--game', required=True)
    b.add_argument('--src', required=True)
    b.add_argument('--game11')
    p = sub.add_parser('make')
    p.add_argument('--game')
    p.add_argument('--title')
    p.add_argument('--new', action='store_true')
    p.add_argument('--belt')
    p.add_argument('--like')
    p.add_argument('--brand')
    p.add_argument('--tag', action='store_true')
    p.add_argument('--out', required=True)
    p.add_argument('--name')
    p.add_argument('--short')
    p.add_argument('--champion')
    p.add_argument('--picture')
    p.add_argument('--art')
    p.add_argument('--set', action='append', default=[])
    p.add_argument('--belt-from')
    p.add_argument('--belt-src')
    p.add_argument('--id')
    p.add_argument('--author')
    p.add_argument('--version')
    p.add_argument('--made-with')
    p.add_argument('--svrmod', action='store_true')
    try:
        a = ap.parse_args(argv)
    except SystemExit:
        print(json.dumps({'ok': False, 'error': 'bad arguments'}))
        return 2
    try:
        print(json.dumps(titles() if a.cmd == 'titles' else belts(a) if a.cmd == 'belts' else make(a)))
        return 0
    except BadInput as e:
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 2
    except Exception as e:  # noqa: BLE001
        print('failed: %r' % e, file=sys.stderr)
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
