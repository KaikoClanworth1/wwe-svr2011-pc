"""Refresh the pre-built pac directory (plist360.arc) from the pacs themselves.

    python plist_fix.py <game folder> [pac path as listed, e.g. pac\\misc.pac ...]

The game finds pac entries through plist360.arc - for every pac listed in
pac\\plist360.h (pac index = line number - 1), a big-endian copy of its table
of contents {4CC type, u32 count*3 | flags, u32 0, entries {char name[4], u32
sector, u32 size/0x100}} after an "FF xx <u16 pac index>" record - not through
the pac's own table. A pac whose entries changed size (moved sectors) needs
its record refreshed, or its entries are read from the old places (a failed
load: the game crashes). Without paths every listed pac is checked. Only
sectors and sizes are rewritten; a pac whose groups or entry names differ is
reported and skipped.
"""
import os
import struct
import sys


def pac_toc(path):
    """-> [(type, [(name4, sector, size256)])] from an EPAC header."""
    with open(path, 'rb') as f:
        d = f.read(0x4000)
    if d[:4] != b'EPAC':
        return None
    out = []
    p = 0x800
    while p + 12 <= 0x4000 and d[p:p + 4] != b'\0\0\0\0':
        typ = d[p:p + 4]
        cnt = struct.unpack_from('<I', d, p + 4)[0] // 3
        p += 12
        ents = []
        for _ in range(cnt):
            sec, sz = struct.unpack_from('<II', d, p + 4)
            ents.append((d[p:p + 4], sec, sz))
            p += 12
        out.append((typ, ents))
    return out


def arc_records(arc):
    """-> {pac index: (start, end)} of each record's group data in the arc."""
    starts = [(p, struct.unpack_from('>H', arc, p + 2)[0]) for p in range(0, len(arc) - 3, 4) if arc[p] == 0xFF]
    recs = {}
    for k, (p, idx) in enumerate(starts):
        recs[idx] = (p + 4, starts[k + 1][0] if k + 1 < len(starts) else len(arc))
    return recs


def fix(game, only=None):
    arc_path = os.path.join(game, 'plist360.arc')
    arc = bytearray(open(arc_path, 'rb').read())
    names = open(os.path.join(game, 'pac', 'plist360.h'), 'rb').read().decode('latin1').splitlines()
    recs = arc_records(arc)
    changed = 0
    for idx, line in enumerate(names):
        line = line.strip()
        if not line or (only and line.lower() not in only) or idx not in recs:
            continue
        path = os.path.join(game, line.replace('\\', os.sep))
        toc = pac_toc(path) if os.path.exists(path) else None
        if not toc:
            continue
        p, end = recs[idx]
        n = 0
        ok = True
        for typ, ents in toc:
            if p + 12 > end or arc[p:p + 4] != typ:
                print('%s: group %r differs from the arc, skipped' % (line, typ))
                ok = False
                break
            q = p + 12
            for name, sec, sz in ents:
                if arc[q:q + 4] != name:
                    print('%s: entry %r differs from the arc, skipped' % (line, name))
                    ok = False
                    break
                if struct.unpack_from('>II', arc, q + 4) != (sec, sz):
                    struct.pack_into('>II', arc, q + 4, sec, sz)
                    n += 1
                q += 12
            if not ok:
                break
            p = q
        if n:
            print('%s: %d entries refreshed' % (line, n))
            changed += n
    if changed:
        open(arc_path, 'wb').write(bytes(arc))
    print('plist360.arc: %d entries changed' % changed)
    return changed


if __name__ == '__main__':
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    fix(sys.argv[1], [a.lower() for a in sys.argv[2:]] or None)
