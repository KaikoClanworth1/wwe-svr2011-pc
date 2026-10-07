"""wwe13_pack.py <mod folder> <out.svrmod> [--made-with <text>]: a port-tools arena
mod folder (manifest.txt, arena.pac, banner.dds, load.dds, vs/*.dds) as a .svrmod
(store zip). The manifest gets made_with=<text> (default Port tools (WWE '13); the
launcher's Mods list, Made with column)."""
import os
import sys
import zipfile

MADE_WITH = "Port tools (WWE '13)"


def stamped(text, made_with=MADE_WITH):
    lines = [l for l in text.replace('\r\n', '\n').split('\n') if l.strip() and not l.startswith('made_with=')]
    return '\n'.join(lines + ['made_with=' + made_with]) + '\n'


def pack(folder, out, made_with=MADE_WITH):
    with open(os.path.join(folder, 'manifest.txt'), encoding='utf-8') as f:
        manifest = stamped(f.read(), made_with)
    names = [n for n in ('arena.pac', 'banner.dds', 'load.dds') if os.path.exists(os.path.join(folder, n))]
    vs = os.path.join(folder, 'vs')
    if os.path.isdir(vs):
        names += ['vs/' + n for n in sorted(os.listdir(vs)) if n.lower().endswith('.dds')]
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_STORED) as z:
        z.writestr('manifest.txt', manifest)
        for n in names:
            z.write(os.path.join(folder, n), n)
    print('%s: %d files, %d bytes' % (out, len(names) + 1, os.path.getsize(out)))


if __name__ == '__main__':
    a = sys.argv[1:]
    mw = MADE_WITH
    if '--made-with' in a:
        i = a.index('--made-with')
        mw = a[i + 1]
        del a[i:i + 2]
    if len(a) != 2:
        raise SystemExit(__doc__)
    pack(a[0], a[1], mw)
