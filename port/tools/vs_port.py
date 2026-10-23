"""VS screen styles from older SmackDown vs. Raw games (SvR 2008 / 2009 / 2010).

The VS screen (the match set-up page: both renders, "A VS. B", the menu row and
"<show> TONIGHT") is drawn over a per-arena VS theme, three entries of
pac/menu/MatchHD.pac in 2011: M<nn>L (the YXML layout), M<nn>I (its textures
and ATB animations) and M<nn>F (its font). 2008 - 2010 have the same system:
the same YXMLBIN VER0002 layouts with the same layer and text names, only
their M<nn>L are in pac/menu/menu.pac. Their themes play in 2011 as they are;
only their text lines sit lower than 2011's menu row allows, so each text
node is given the place, size and width of 2011's node of that name in the
same theme (docs/VS_STYLES.md).

This makes a VS style from the player's own disc - never shipped:
<out>/manifest.txt (type=VsStyle) and vs.pac (an EPAC, group MENU, the
themes under 2011's numbers); installed as <game>/Mods/VsStyles/<game id>/.
The game merges the installed styles into an overlay and shows the chosen
one (GRAPHICS -> VS SCREEN STYLE).

The Mod Maker's converter contract (docs/MOD_MAKER_3_PLAN.md section 4):
    py vs_port.py games
    py vs_port.py list --game svr2010 --src <disc image or folder>     (one item: the style)
    py vs_port.py convert --game svr2010 --src <disc> --pac11 <2011 game or its pac folder> --out <folder>
        [--item style] [--renders]  (--renders: the old game's renders for 2011's superstars, matched by
                                     name, as a media pack beside it - vs_renders.py)

JSON on stdout (and <out>/report.json). Exit codes: 0 ok, 2 bad input, 3 conversion failed.
"""
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import discfs  # noqa: E402
import svrfmt as f  # noqa: E402
from arena_port import SVR2008, SVR2009, SVR2010  # noqa: E402

GAMES = {'svr2008': 'SvR 2008', 'svr2009': 'SvR 2009', 'svr2010': 'SvR 2010'}
# SvR 2008's layouts have another shape (30 nodes: an extra text line in each group) and 2011's VS screen faults
# on them (sub_8269BE90 walking a bad element list, 23 Oct) - with or without the text move. Not offered yet.
DISC = {'svr2008': '5451080B', 'svr2009': '54510826', 'svr2010': '54510844'}  # (the Xbox 360 title ids: disc_hint)
# SvR 2009's play, but the renders and the names line never show (its layouts have 6 scene groups, 2010's and
# 2011's 7 - the likely cause, not checked).
RESEARCH = {'svr2008': "SvR 2008's VS screens crash 2011 (their layouts have another shape) - not ready yet",
            'svr2009': "SvR 2009's VS screens don't show the superstars and their names in 2011 yet - not ready yet"}


class BadInput(Exception):
    pass


def shows(game):
    """old theme number -> (show, 2011 theme number or None)"""
    if game == 'svr2010':
        return {n: (v[1], v[2]) for n, v in SVR2010.items()}
    return dict(SVR2008 if game == 'svr2008' else SVR2009)


def theme_map(game):
    """2011 theme number -> old theme number: the arena tables' "2011 arena of that show", plus the shows 2011
    has a theme for under another event's name (WrestleMania 11, Backlash 12)."""
    out = {}
    for n, (show, n11) in sorted(shows(game).items()):
        if n11 is None:
            n11 = 11 if 'WrestleMania' in show else 12 if show == 'Backlash' else None
        if n11 is not None and n11 not in out:
            out[n11] = n
    return out


def entries(data):
    h, groups, t = f.epac_read(data)
    return h, {n.decode(errors='replace').strip('\0'): b for typ, ents in groups for n, b in ents}


def unwrap(blob):
    return f.bpe_decode(blob) if blob[:4] == b'BPE ' else blob


# ---------------------------------------------------------------- the layouts' text nodes

TEXT = re.compile(rb'font000_[a-z]+')


def text_nodes(y):
    """[(name, x/y/scale/width offset)] in file order: font id, x, y, scale x, scale y, width (6 x 4 bytes) sit
    0x5C bytes before the node's name."""
    return [(m.group().decode(), m.start() - 0x58) for m in TEXT.finditer(y)]


def move_text(old, new11, fallback=()):
    """The old layout with each text node at 2011's place (x, y, scale, width) for the node of that name and
    occurrence in the same theme (else in `fallback`, 2011's SmackDown / RAW layouts); a node no 2011 layout
    has (2008's main / main2 / sub2: the game never fills them) scaled to nothing."""
    out = bytearray(old)
    ref = {}
    for y11 in (new11,) + tuple(fallback):
        own = {}
        for name, at in text_nodes(y11):
            own.setdefault(name, []).append(struct.unpack_from('<5f', y11, at))
        for name, v in own.items():
            ref.setdefault(name, v)
    seen = {}
    moved = hidden = 0
    for name, at in text_nodes(old):
        k = seen.get(name, 0)
        seen[name] = k + 1
        r = ref.get(name)
        if r:
            struct.pack_into('<5f', out, at, *r[min(k, len(r) - 1)])
            moved += 1
        else:
            x, y, sx, sy, w = struct.unpack_from('<5f', out, at)
            struct.pack_into('<5f', out, at, x, y, 0.0, 0.0, w)
            hidden += 1
    return bytes(out), moved, hidden


def layout_of(blob):
    """An M<nn>L entry -> (its PACH children, the YXML)"""
    kids = f.pach_read(unwrap(blob))
    return kids, unwrap(kids[0][1])


def with_layout(blob, y):
    kids = f.pach_read(unwrap(blob))
    first = kids[0][1]
    packed = f.bpe_encode(y, compress=True) if first[:4] == b'BPE ' else y
    kids = [(kids[0][0], packed)] + kids[1:]
    out = f.pach_write(kids)
    return f.bpe_encode(out, compress=True) if blob[:4] == b'BPE ' else out


# ---------------------------------------------------------------- the commands

def read_old(src):
    try:
        d = discfs.open(src)
    except Exception as e:  # noqa: BLE001
        raise BadInput('cannot read %s: %s' % (src, e))
    with d:
        for p in ('pac/menu/menu.pac', 'pac/menu/MatchHD.pac'):
            if not d.exists(p):
                raise BadInput('the game files have no %s (SvR 2007 has no VS themes of this kind)' % p)
        return d.read('pac/menu/menu.pac'), d.read('pac/menu/MatchHD.pac')


def list_themes(game, src):
    """One item: the game's VS style (the themes it brings, by show)."""
    menu_raw, match_raw = read_old(src)
    _, menu = entries(menu_raw)
    _, match = entries(match_raw)
    sh = shows(game)
    have = [sh[n][0] for n11, n in sorted(theme_map(game).items())
            if all(('M%02d' % n + x) in d for x, d in (('L', menu), ('I', match), ('F', match)))]
    return [{'key': 'style', 'name': '%s VS screens' % GAMES[game], 'shows': ', '.join(have), 'themes': len(have),
             'in_2011': None, 'status': 'ok' if have else 'special',
             'reason': '' if have else 'no VS theme of this disc fits 2011'}]


def matchhd11(pac11):
    """--pac11: the 2011 game folder, its pac folder or MatchHD.pac itself"""
    for c in (pac11, os.path.join(pac11, 'menu', 'MatchHD.pac'), os.path.join(pac11, 'pac', 'menu', 'MatchHD.pac')):
        if os.path.isfile(c):
            return c
    raise BadInput('no 2011 MatchHD.pac at %s (--pac11: the 2011 game folder)' % pac11)


def convert(game, src, pac11, out, name=None, keep_text=False):
    menu_raw, match_raw = read_old(src)
    h_old, menu = entries(menu_raw)
    _, match = entries(match_raw)
    h11, match11 = entries(open(matchhd11(pac11), 'rb').read())
    sh = shows(game)
    fallback = tuple(layout_of(match11[k])[1] for k in ('M00L', 'M01L') if k in match11)
    ents, themes, notes = [], [], []
    for n11, n in sorted(theme_map(game).items()):
        k, k11 = 'M%02d' % n, 'M%02d' % n11
        if not all(x in d for x, d in ((k + 'L', menu), (k + 'I', match), (k + 'F', match))):
            notes.append('%s: no theme %02d on the disc - 2011\'s stays' % (sh[n][0], n))
            continue
        if k11 + 'L' not in match11:
            notes.append('2011 has no theme %02d - %s left out' % (n11, sh[n][0]))
            continue
        _, y = layout_of(menu[k + 'L'])
        _, y11 = layout_of(match11[k11 + 'L'])
        if not y.startswith(b'YXMLBIN VER0002') or not y11.startswith(b'YXMLBIN VER0002'):
            notes.append('%s: an unknown layout format - left out' % sh[n][0])
            continue
        y2, moved, hidden = (y, 0, 0) if keep_text else move_text(y, y11, fallback)
        ents += [((k11 + 'L').encode(), with_layout(menu[k + 'L'], y2)),
                 ((k11 + 'I').encode(), match[k + 'I']), ((k11 + 'F').encode(), match[k + 'F'])]
        themes.append({'theme': n11, 'from': n, 'show': sh[n][0], 'text_moved': moved, 'text_hidden': hidden})
    if not themes:
        raise BadInput('no VS theme of this disc fits 2011')
    folder = out
    os.makedirs(folder, exist_ok=True)
    data = f.epac_write(h11, [(b'MENU', ents)])
    tmp = os.path.join(folder, 'vs.pac.tmp')
    with open(tmp, 'wb') as fh:
        fh.write(data)
    os.replace(tmp, os.path.join(folder, 'vs.pac'))
    with open(os.path.join(folder, 'manifest.txt'), 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('type=VsStyle\nname=%s\ngame=%s\nthemes=%s\n'
                 % (name or GAMES[game], game, ','.join('%02d' % t['theme'] for t in themes)))
    shown = ', '.join('%s (%02d)' % (t['show'], t['theme']) for t in themes)
    notes = ["%d VS themes: %s; the others stay 2011's" % (len(themes), shown)] + notes
    if any(t['text_hidden'] for t in themes):
        notes.append("text lines 2011 never fills were hidden (%s's extra lines)" % GAMES[game])
    return {'ok': True, 'out': os.path.abspath(folder), 'opens': 'vs_style', 'install_as': game,
            'warnings': [], 'dropped': [], 'notes': notes, 'themes': themes, 'size': len(data)}


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog='vs_port.py')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('games')
    p = sub.add_parser('list')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11')  # (the Mod Maker passes it to every list; not needed here)
    p = sub.add_parser('convert')
    p.add_argument('--game', required=True)
    p.add_argument('--src', required=True)
    p.add_argument('--pac11', required=True)
    p.add_argument('--out', required=True)
    p.add_argument('--item', default='style')
    p.add_argument('--name')
    p.add_argument('--renders', action='store_true')
    p.add_argument('--keep-text', action='store_true', help=argparse.SUPPRESS)  # (test aid: the old layout as it is)
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'games':
            print(json.dumps([{'id': k, 'name': v, 'platform': 'x360', 'disc_hint': DISC[k],
                               'status': 'research' if k in RESEARCH else 'ready'} for k, v in GAMES.items()]))
            return 0
        if a.game not in GAMES:
            raise BadInput('unknown game %r (%s)' % (a.game, ', '.join(GAMES)))
        if a.game in RESEARCH and not getattr(a, 'keep_text', False):  # (--keep-text: the crash's test aid)
            raise BadInput(RESEARCH[a.game])
        if a.cmd == 'list':
            print(json.dumps(list_themes(a.game, a.src)))
            return 0
        rep = convert(a.game, a.src, a.pac11, a.out, a.name, a.keep_text)
        if a.renders:
            import vs_renders
            rep['renders'] = vs_renders.convert(a.game, a.src, a.pac11, os.path.join(a.out, 'renders'))
            rep['notes'] += rep['renders'].get('notes', [])
        with open(os.path.join(a.out, 'report.json'), 'w', encoding='utf-8') as fh:
            json.dump(rep, fh, indent=1)
        print(json.dumps(rep))
        return 0
    except BadInput as e:
        print(json.dumps({'error': str(e)}))
        return 2
    except Exception as e:  # noqa: BLE001
        print(json.dumps({'error': 'conversion failed: %s' % e}))
        return 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
