"""Phase 0 load experiments on one arena (the others stay original).

    arena_experiment.py <variant> <src bgNN.pac> <dst bgNN.pac> [entry id hex]

variants (on one entry, default the first ar_fence model):
    stored     original bytes, re-encoded as stored BPE (tests BPE + size change)
    packed     original bytes, re-compressed with our BPE compressor
    raw        original bytes, stored without BPE at all
    rewrite    JBOY read + write (same content), stored BPE
    raise      barrier raised 15 units, stored BPE
    grow       unchanged entries, but the EPAC padded by one extra sector
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svrfmt as f  # noqa: E402
import jboy  # noqa: E402
import arena_tool as at  # noqa: E402
import arena_lab_mod as lab  # noqa: E402


def main():
    variant, src, dst = sys.argv[1:4]
    want = int(sys.argv[4], 16) if len(sys.argv) > 4 else None
    h, g, t = f.epac_read(open(src, 'rb').read())
    new_groups = []
    done = None
    for typ, ents in g:
        new_ents = []
        for name, blob in ents:
            if blob[:4] != b'PACH':
                new_ents.append((name, blob))
                continue
            out = []
            for eid, b in f.pach_read(blob):
                raw = at.unpack(b)
                hit = (eid == want) if want is not None else (
                    done is None and raw[:4] == b'JBOY' and jboy.read(raw).name.startswith('ar_fen'))
                if hit and variant != 'grow':
                    done = eid
                    if variant == 'stored':
                        b = f.bpe_encode(raw)
                    elif variant == 'packed':
                        b = f.bpe_encode(raw, compress=True)
                        print('packed', len(b), 'unpacked', len(raw))
                    elif variant == 'raw':
                        b = raw
                    elif variant == 'rewrite':
                        b = f.bpe_encode(jboy.write(jboy.read(raw)))
                    elif variant == 'raise':
                        b = f.bpe_encode(lab.raise_model(raw, 15.0))
                out.append((eid, b))
            new_ents.append((name, f.pach_write(out)))
        new_groups.append((typ, new_ents))
    data = f.epac_write(h, new_groups, t)
    if variant == 'grow':
        hdr, groups, trailer = h, g, t
        data = f.epac_write(hdr, [(ty, [(n, b + bytes(0x800)) for n, b in e]) for ty, e in groups], trailer)
    open(dst, 'wb').write(data)
    orig = os.path.getsize(src)
    print(f'{variant}: entry {done if done is None else hex(done)}, size {orig} -> {len(data)} ({len(data) - orig:+d})')


if __name__ == '__main__':
    main()
