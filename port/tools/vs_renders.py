"""An older game's renders for 2011's own superstars (vs_port.py --renders): a media pack.

For every superstar of SvR 2009 / 2010 that 2011 has too (the same id and name, as superstar_port.py's list
finds them), the old game's renders become a media pack (docs/MEDIA_MODS.md): render.<id> = its body render
(SSFB, else SSFA) on a transparent 512 x 512, bust.<id> = its 256 x 256 bust (SSFC). 2011 then shows them
wherever it shows renders (the VS screen, the select screen, the busts) - the player's own disc, never shipped.

    py vs_renders.py --game svr2010 --src <disc> --pac11 <2011 game or pac folder> --out <pack folder>
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ch_tool  # noqa: E402
import superstar_port as sp  # noqa: E402

LABELS = {'svr2009': 'SvR 2009', 'svr2010': 'SvR 2010'}


def pac_folder(pac11):
    for c in (pac11, os.path.join(pac11, 'pac')):
        if os.path.isfile(os.path.join(c, 'ch', 'chEtc.pac')):
            return c
    raise sp.BadInput('--pac11 is not a SvR 2011 game or pac folder: %s' % pac11)


def convert(game, src, pac11, out, cache=None):
    if game not in LABELS:
        raise sp.BadInput('renders from %s are not ready (SvR 2009 and 2010 only)' % game)
    S = sp.svr10()
    s = sp.Src(src, cache or os.path.join(HERE, 'work', 'src', game))
    g = sp.open_game(game, s)
    g11, _ = sp.game11(pac_folder(pac11))
    play = sp.playable11(g11)
    ssf = g.ssf() if callable(getattr(g, 'ssf', None)) else g.ssf
    os.makedirs(out, exist_ok=True)
    lines, done = [], []
    for c in g.roster():
        i11 = g.id11(c) if hasattr(g, 'id11') else c
        if i11 not in play or S.norm(g11.name(i11)) != S.norm(g.name(c)):
            continue
        body = ssf.get(('SSFB', c)) or ssf.get(('SSFA', c))
        bust = ssf.get(('SSFC', c))
        if not body and not bust:
            continue
        if body:
            open(os.path.join(out, 'render_%d.dds' % i11), 'wb').write(sp.pad_render(ch_tool.unpack(body)))
            lines.append('render.%d=render_%d.dds' % (i11, i11))
        if bust:
            open(os.path.join(out, 'bust_%d.dds' % i11), 'wb').write(sp.pad_render(ch_tool.unpack(bust), 256))
            lines.append('bust.%d=bust_%d.dds' % (i11, i11))
        done.append(g.name(c))
    if not done:
        raise sp.BadInput('no superstar of this disc is in 2011 under the same name')
    with open(os.path.join(out, 'manifest.txt'), 'w', encoding='utf-8', newline='\n') as fh:
        fh.write('type=media\nname=%s renders\n' % LABELS[game] + '\n'.join(lines) + '\n')
    return {'out': os.path.abspath(out), 'superstars': len(done),
            'notes': ['%s renders for %d of 2011\'s superstars (%s): a media pack - they show wherever 2011 shows '
                      'renders' % (LABELS[game], len(done), ', '.join(done[:8]) + (' ...' if len(done) > 8 else ''))]}


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(prog='vs_renders.py')
    ap.add_argument('--game', required=True)
    ap.add_argument('--src', required=True)
    ap.add_argument('--pac11', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    try:
        print(json.dumps(convert(a.game, a.src, a.pac11, a.out)))
        return 0
    except sp.BadInput as e:
        print(json.dumps({'error': str(e)}))
        return 2
    except Exception as e:  # noqa: BLE001
        print(json.dumps({'error': 'conversion failed: %s' % e}))
        return 3


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
