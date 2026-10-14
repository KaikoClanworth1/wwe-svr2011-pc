"""Pac editor round trip (2.1.2 "full EPAC support"): pacs from game discs or
folders through svrmod archive-test (an unchanged save gives the same bytes; a
rebuild of every list inside gives the same file, or the same once unpacked)
and archive-edit-test (replace / remove / add / rename in the deepest list,
saved, read back the same).

    py archive_test.py <disc image or folder>... [--per 14] [--seed 7] [--keep DIR]

Picks --per pacs (under 40 MB) from each source at random (seeded), copies them
to a scratch folder (--keep: that folder, kept) and prints svrmod's lines and a
count. Exit 1 if any FAIL.
"""
import os
import random
import subprocess
import sys
import tempfile

import discfs

HERE = os.path.dirname(os.path.abspath(__file__))
SVRMOD = os.path.join(HERE, '..', 'out', 'modmaker', 'svrmod.exe')


def main(argv):
    srcs, per, seed, keep = [], 14, 7, None
    i = 0
    while i < len(argv):
        if argv[i] == '--per': per = int(argv[i + 1]); i += 2
        elif argv[i] == '--seed': seed = int(argv[i + 1]); i += 2
        elif argv[i] == '--keep': keep = argv[i + 1]; i += 2
        else: srcs.append(argv[i]); i += 1
    if not srcs:
        print(__doc__)
        return 2
    out = keep or tempfile.mkdtemp(prefix='archive_test_')
    os.makedirs(out, exist_ok=True)
    rnd = random.Random(seed)
    files = []
    for n, src in enumerate(srcs):
        with discfs.open(src) as d:
            pacs = [(f, s) for f, s in d.walk('') if f.lower().endswith('.pac') and 0 < s < 40_000_000]
            for f, _ in rnd.sample(pacs, min(per, len(pacs))):
                dst = os.path.join(out, '%02d_%s' % (n, f.replace('/', '_').replace(chr(92), '_')))
                if not os.path.exists(dst):
                    with open(dst, 'wb') as fo: fo.write(d.read(f))
                files.append(dst)
        print('%s: %d of %d pacs' % (src, min(per, len(pacs)), len(pacs)))
    bad = 0
    for cmd in ('archive-test', 'archive-edit-test'):
        ok = fail = 0
        for f in files:  # (one at a time: a crash names its file)
            r = subprocess.run([SVRMOD, cmd, f], capture_output=True, text=True)
            for line in r.stdout.splitlines():
                print(line)
                ok += line.startswith('ok')
                fail += line.startswith('FAIL')
            if r.returncode > 1:
                print('CRASH %s %s (%d)' % (cmd, f, r.returncode))
                fail += 1
        print('%s: %d ok, %d FAIL' % (cmd, ok, fail))
        bad += fail
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
