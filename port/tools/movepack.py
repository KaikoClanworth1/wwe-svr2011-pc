"""A move pack the game merges at start-up (src/move_packs.cpp), from a
port_moves.py build (svr10_moves.py: its out/movepack/manifest.json).

    python movepack.py <movepack folder with manifest.json> <out folder>

Writes <out>/pack.txt and <out>/motions/*.ymk. Put the folder in a superstar
mod as moves/ (or in <game>/Mods/Moves/<name>/).
"""
import json
import os
import shutil
import sys


def main(src, out):
    m = json.load(open(os.path.join(src, 'manifest.json')))
    if m.get('format') != 'svr2011-movepack/1':
        raise SystemExit('not a svr2011-movepack/1 manifest')
    os.makedirs(os.path.join(out, 'motions'), exist_ok=True)
    lines = ['# svr2011 move pack: moves %s' % ' '.join(str(i) for i in m['moves'])]
    n = 0
    for b in m['banks']:
        for e in b['insert']:
            name = os.path.basename(e['file'])
            shutil.copyfile(os.path.join(src, e['file']), os.path.join(out, 'motions', name))
            lines.append('motion %s %s %d %d %d %d motions/%s' % (b['pac'], b['path'], e['id'], e['x'], e['y'], e['frames'], name))
            n += 1
    misc = m.get('misc', {})
    for w in misc.get('waze', []):
        lines.append('waze %d %s' % (w['id'], w['flags_after']))
    for r in misc.get('exh', []):
        lines.append('exh %d %s' % (r['group'], r['record']))
    for r in misc.get('events', []):
        lines.append('evt %d %s %s' % (r['group'], r['record'], r['events']))
    for r in misc.get('mbd', []):
        lines.append('mbd %d %s' % (r['group'], r['record']))
    open(os.path.join(out, 'pack.txt'), 'w', newline='\n').write('\n'.join(lines) + '\n')
    print('%s: %d motions, %d other records' % (out, n, len(lines) - 1 - n))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    main(sys.argv[1], sys.argv[2])
