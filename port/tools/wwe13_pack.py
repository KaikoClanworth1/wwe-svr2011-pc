"""wwe13_pack.py <mod folder> <out.svrmod>: a WWE '13 port mod folder (manifest.txt,
arena.pac, banner.dds, load.dds, vs/*.dds) as a .svrmod (store zip). The manifest
gets made_with=Port tools (WWE '13) (the launcher's Mods list, Made with column)."""
import os
import sys
import zipfile

MADE_WITH = "Port tools (WWE '13)"


def stamped(text):
    lines = [l for l in text.replace('\r\n', '\n').split('\n') if l.strip() and not l.startswith('made_with=')]
    return '\n'.join(lines + ['made_with=' + MADE_WITH]) + '\n'


def pack(folder, out):
    with open(os.path.join(folder, 'manifest.txt'), encoding='utf-8') as f:
        manifest = stamped(f.read())
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
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    pack(sys.argv[1], sys.argv[2])
