"""Arena format lab (branch `arenas`, Phase 0).

    arena_tool.py roundtrip <pac dir>        repack every EPAC, compare bytes
    arena_tool.py list <file.pac>            groups, PACH entries, magics
    arena_tool.py extract <file.pac> <dir>   unpacked entries + textures
    arena_tool.py textures <file.pac> <dir>  the texture bundle as .dds

Read-only on game data: output goes only to the directory given.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svrfmt as f  # noqa: E402


def load(path):
    d = open(path, 'rb').read()
    return f.epac_read(d)


def unpack(blob):
    return f.bpe_decode(blob) if blob[:4] == b'BPE ' else blob


def tex_bundle(raw):
    """Texture bundle: u32 count, 12 bytes, count x 32-byte records
    {char name[16], "dds\\0", u32 size, u32 offset}. -> [(name, dds)]"""
    n = struct.unpack_from('<I', raw, 0)[0]
    out = []
    for i in range(n):
        r = 16 + 32 * i
        name = raw[r:r + 16].split(b'\0')[0].decode('ascii', 'replace')
        ext = raw[r + 16:r + 20].split(b'\0')[0].decode('ascii', 'replace')
        size, off = struct.unpack_from('<II', raw, r + 20)
        out.append((name + '.' + ext, raw[off:off + size]))
    return out


def is_tex_bundle(raw):
    if len(raw) < 48:
        return False
    n = struct.unpack_from('<I', raw, 0)[0]
    return 0 < n < 4096 and raw[16 + 16:16 + 20] == b'dds\0'


def cmd_roundtrip(root):
    ok = bad = 0
    for dp, _, fs in os.walk(root):
        for fn in sorted(fs):
            p = os.path.join(dp, fn)
            d = open(p, 'rb').read()
            if d[:4] != b'EPAC':
                continue
            h, g, t = f.epac_read(d)
            same = f.epac_write(h, g, t) == d
            for _, ents in g:
                for _, b in ents:
                    if b[:4] == b'PACH':
                        w = f.pach_write(f.pach_read(b))
                        same = same and w == b[:len(w)] and not any(b[len(w):])
            if same:
                ok += 1
            else:
                bad += 1
                print('DIFF', os.path.relpath(p, root))
    print(f'identical {ok}, different {bad}')
    return bad == 0


def cmd_list(path):
    h, g, _ = load(path)
    for typ, ents in g:
        print(typ.decode(), len(ents), 'entries')
        for name, blob in ents:
            print(' ', name.decode('ascii', 'replace'), len(blob), blob[:4])
            if blob[:4] != b'PACH':
                continue
            for eid, b in f.pach_read(blob):
                raw = unpack(b)
                kind = raw[:4]
                if is_tex_bundle(raw):
                    kind = b'TEX%d' % struct.unpack_from('<I', raw)[0]
                print(f'    {eid:#06x} {len(raw):9d} {kind!r}')


def cmd_extract(path, out, textures_only=False):
    os.makedirs(out, exist_ok=True)
    h, g, _ = load(path)
    for typ, ents in g:
        for name, blob in ents:
            sub = os.path.join(out, typ.decode().strip() + '_' + name.decode('ascii', 'replace'))
            if blob[:4] != b'PACH':
                if not textures_only:
                    os.makedirs(sub, exist_ok=True)
                    open(os.path.join(sub, 'raw.bin'), 'wb').write(unpack(blob))
                continue
            for eid, b in f.pach_read(blob):
                raw = unpack(b)
                if is_tex_bundle(raw):
                    td = os.path.join(sub, f'{eid:04x}_tex')
                    os.makedirs(td, exist_ok=True)
                    for tn, dds in tex_bundle(raw):
                        open(os.path.join(td, tn), 'wb').write(dds)
                elif not textures_only:
                    os.makedirs(sub, exist_ok=True)
                    open(os.path.join(sub, f'{eid:04x}.bin'), 'wb').write(raw)


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        print(__doc__)
    elif a[0] == 'roundtrip':
        sys.exit(0 if cmd_roundtrip(a[1]) else 1)
    elif a[0] == 'list':
        cmd_list(a[1])
    elif a[0] == 'extract':
        cmd_extract(a[1], a[2])
    elif a[0] == 'textures':
        cmd_extract(a[1], a[2], textures_only=True)
    else:
        print(__doc__)
