"""An entrance's motions, for the Mod Maker's previews: entrance number -> the entrant's tracks per segment.

  python entrance_ring.py <number> --game-pac <2011 pac dir> [--mods <Mods dir>] --out <dir> [--all]

An entrance is not made of the move banks' motions (m.pac MVMT/ANAS's 21000s share no track with any
entrance): its own track bank holds everything. Entrance n = script EVP? <n> + track bank EVT? <n> in
pac\\evt\\NyujyoN.pac (stock) or a move pack's pacentry files (mods: Mods\\Superstars\\<mod>\\moves,
Mods\\Moves\\<pack>; a mod's entry wins over the game's, as in the game). In the bank, a track's key is
(25000 + object, x = script slot, y): segments are slots 0 / 5 / 10 / 15 / 20 (0 = the hidden intro,
20 = the ring part), the entrant is the script's first character object, y0 = body (YMKs 0x7d keys,
2011's skeleton), y2 / y3 = right / left fingers (docs/HAND_POSES.md), y1 = face; the camera object
(type 0x10000/0) has y0 / y1 (29-byte mode-1 camera keys; y1 is the one played).

Output (stdout JSON, like the converters): {ok, number, name, source, segments: [{segment, slot,
frames, tracks: {body, fingers_right, fingers_left, camera}}], bank} and <out>\\entrance_<n>.ymks: a YMKs
bank of those tracks re-keyed as motion id <n>, x = segment (0-4), y = 0 body / 2 / 3 fingers /
10 camera (y0) - the viewer plays it like any m.pac motion. Without --all only segment 4 (the ring part).
Exit 0 / 2 (bad input) / 3.
"""
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import svr08_entrance as S  # noqa: E402
import wwe13_entrance as W  # noqa: E402  (EPK8 reader)
import ymks  # noqa: E402

SLOTS = (0, 5, 10, 15, 20)


def opt(a, k, d=None):
    return a[a.index(k) + 1] if k in a and a.index(k) + 1 < len(a) else d


def from_mods(mods, number):
    """(evp, evt, source) of a move pack's entrance <number>, or None"""
    for root in ('Moves', 'Superstars'):
        base = os.path.join(mods, root)
        if not os.path.isdir(base):
            continue
        for d in sorted(os.listdir(base)):
            pack = os.path.join(base, d, 'moves' if root == 'Superstars' else '', 'pack.txt')
            if not os.path.isfile(pack) or os.path.exists(os.path.join(base, d, 'disabled')):
                continue
            files = {}
            for line in open(pack, encoding='utf-8', errors='replace'):
                p = line.split()
                if len(p) == 5 and p[0] == 'pacentry' and p[3] == str(number) and p[2][:3] in ('EVP', 'EVT'):
                    files[p[2][:3]] = os.path.join(os.path.dirname(pack), p[4])
            if 'EVP' in files and 'EVT' in files:
                return open(files['EVP'], 'rb').read(), open(files['EVT'], 'rb').read(), '%s\\%s' % (root, d)
    return None


def from_game(game_pac, number):
    evt_dir = os.path.join(game_pac, 'evt')
    for f in sorted(os.listdir(evt_dir)):
        if not re.match(r'(?i)nyujyo.\.pac$', f):
            continue
        ents = W.epk8_entries(open(os.path.join(evt_dir, f), 'rb').read())
        evp = next((b for (g, n), b in ents.items() if g.startswith('EVP') and n == str(number)), None)
        evt = next((b for (g, n), b in ents.items() if g.startswith('EVT') and n == str(number)), None)
        if evp and evt:
            return evp, evt, 'pac\\evt\\' + f
    return None


def ring(number, game_pac, mods=None, all_segments=False):
    """-> (report dict, YMKs bank bytes)"""
    src = (from_mods(mods, number) if mods else None) or from_game(game_pac, number)
    if not src:
        raise ValueError('no entrance %d (game or mods)' % number)
    evp, evt, where = src
    e = S.parse_evp(evp, '2011')
    kinds = [(o['t0'], o['t1']) for o in e['objs']]
    ent = next((i for i, k in enumerate(kinds) if k == S.TYPE_CHAR), 0)
    cam = next((i for i, k in enumerate(kinds) if k == (0x10000, 0)), None)
    bank = ymks.read_bank(evt)
    by = {(t['id'] - S.ID_SHIFT, t['x'], t['y']): t for t in bank.entries if t['data']}
    out = ymks.Bank(b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + S.T11, [])
    out.lead = b''
    segs = []
    for k in (range(5) if all_segments else (4,)):
        x = SLOTS[k]
        tr = {}
        for name, key, y in (('body', (ent, x, 0), 0), ('fingers_right', (ent, x, 2), 2),
                             ('fingers_left', (ent, x, 3), 3), ('camera', (cam, x, 1), 10)):
            t = by.get(key)
            if not t and name == 'camera':
                t = by.get((cam, x, 0))
            if t:
                ymks.insert_motion(out, number, k, y, t['data'], t['frames'], S.RT_2011)
                tr[name] = t['frames']
        body = by.get((ent, x, 0))
        segs.append(dict(segment=k, slot=x, frames=body['frames'] if body else 0, tracks=tr))
    name = e['name'].split(b'\0')[0].decode('latin-1', 'replace').replace('_', ' ').strip()
    return dict(ok=True, number=number, name=name, source=where, entrant_object=ent, camera_object=cam,
                segments=segs), ymks.write_bank(out)


def main(argv):
    a = list(argv)
    try:
        number = int(a[0])
    except (IndexError, ValueError):
        print(__doc__, file=sys.stderr)
        return 2
    gp, out = opt(a, '--game-pac'), opt(a, '--out')
    if not gp or not os.path.isdir(os.path.join(gp, 'evt')) or not out:
        print('error: --game-pac <2011 pac dir> and --out are needed', file=sys.stderr)
        return 2
    try:
        rep, data = ring(number, gp, opt(a, '--mods') or os.path.join(os.path.dirname(gp), 'Mods'), '--all' in a)
    except ValueError as x:
        print('error: %s' % x, file=sys.stderr)
        return 2
    except Exception as x:
        print(json.dumps(dict(ok=False, number=number, error=str(x))))
        return 3
    os.makedirs(out, exist_ok=True)
    rep['bank'] = os.path.join(out, 'entrance_%d.ymks' % number)
    open(rep['bank'], 'wb').write(data)
    print(json.dumps(rep))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
