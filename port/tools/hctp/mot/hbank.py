"""HCTP CEKey motion bank reader (no header; T[256], n, 16-byte dir, data)."""
import struct, collections, sys

def read(d):
    base = 0x10 if d[:4] == b'YMKs' else 0
    T = d[base:base+256]
    n = struct.unpack_from('<I', d, base+0x100)[0]
    ents = [struct.unpack_from('<IIII', d, base+0x104+16*i) for i in range(n)]
    data = d[base+0x104+16*n:]
    return T, ents, data

def frames(data, off, end=None):
    """yield (frame, record bytes) for one stream; stops at end offset."""
    p, f = off, 0
    end = len(data) if end is None else end
    while p < end:
        c = data[p]
        if c < 0x60:
            p += 1; f += c; continue
        if c == 0x60:
            sz = struct.unpack_from('<H', data, p+1)[0]; rec = data[p+3:p+3+sz]; p += 3+sz
        else:
            rec = data[p+1:p+1+c-0x60]; p += 1+c-0x60
        yield f, rec
        f += 1

def streams(d):
    T, ents, data = read(d)
    offs = sorted(set(e[1] for e in ents)) + [len(data)]
    nxt = {offs[i]: offs[i+1] for i in range(len(offs)-1)}
    out = []
    for e in ents:
        key, off, a, b = e
        out.append(dict(id=key >> 16, y=(key >> 8) & 0xff, x=key & 0xff, off=off, a=a, b=b, end=nxt.get(off, len(data))))
    return T, out, data

if __name__ == '__main__':
    d = open(sys.argv[1], 'rb').read()
    T, ents, data = streams(d)
    print('n', len(ents), 'T nonzero', sum(1 for t in T if t))
    print('T', ' '.join(f'{i}:{t}' for i, t in enumerate(T) if t))
    c = collections.Counter(); first = collections.Counter(); ys = collections.Counter()
    for e in ents[:]:
        ys[e['y']] += 1
        for f, r in frames(data, e['off'], e['end']):
            c[(e['y'] if e['y'] < 3 else 'x', len(r))] += 1
            if r: first[(e['y'] if e['y'] < 3 else 'x', r[0] if r[0] < 0x80 else 'ev')] += 1
    print('ys', ys)
    print(c.most_common(40))
    print(first.most_common(40))
    for e in ents[:5]: print(e)
