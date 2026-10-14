"""Copy entries of a YMKs bank under new keys: rekey(bank, {(id, x): (new id, new x)}) (all tracks y)."""
import struct, sys
sys.path.insert(0, r"D:\Xbox Games Ports\WWE Smackdown Vs Raw 2011\port\tools")
import svr08_gimmick as G

def rekey(d, mapping, frames=None):
    """frames: {(new id, new x): n} - the new entry plays only n frames."""
    base = 0x10 if d[:4] == b'YMKs' else 0
    T, ents, data = G.bank_parse(d)
    add = []
    for e in ents:
        y, x, mid = e[0], e[1], e[2]
        if (mid, x) in mapping:
            nid, nx = mapping[(mid, x)]
            n = (frames or {}).get((nid, nx), e[4])
            add.append([y, nx, nid, e[3], n, e[5]])
    ents = ents + add
    ents.sort(key=lambda e: (e[2], e[1], e[0]))
    head = d[:base] + T
    return head + struct.pack('<I', len(ents)) + b''.join(struct.pack('<BBHIII', *e) for e in ents) + data

def copy_track(d, src, dst, frames=None):
    """One track (id, x, y) of a bank copied as (new id, new x, new y)."""
    base = 0x10 if d[:4] == b'YMKs' else 0
    T, ents, data = G.bank_parse(d)
    add = [[dst[2], dst[1], dst[0], e[3], frames or e[4], e[5]] for e in ents
           if (e[2], e[1], e[0]) == src]
    ents = ents + add
    ents.sort(key=lambda e: (e[2], e[1], e[0]))
    head = d[:base] + T
    return head + struct.pack('<I', len(ents)) + b''.join(struct.pack('<BBHIII', *e) for e in ents) + data


if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    m = {}
    for it in sys.argv[3].split(','):
        a, b = it.split('=')
        i, x = a.split(':'); ni, nx = b.split(':')
        m[(int(i), int(x))] = (int(ni), int(nx))
    out = rekey(d, m)
    open(sys.argv[2], 'wb').write(out)
    T, ents, _ = G.bank_parse(out)
    print('entries', len(ents), [ (e[2], e[1], e[0], e[4]) for e in ents if e[2] in {v[0] for v in m.values()}])
