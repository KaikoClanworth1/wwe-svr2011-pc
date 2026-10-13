"""wwe2k14_unzlib.py <in.pac> <out.pac>: a WWE 2K14 pac with every entry unpacked: "ZLIB" entries (magic, u32 version 0x1230, u32 packed size, u32 unpacked size, then a zlib stream; sizes equal = stored) and bare zlib streams, in PACH children too. The result is a plain EPAC the WWE '13 converter (svrmod wwe13) reads."""
import struct
import sys
import zlib

sys.path.insert(0, __import__("os").path.dirname(__import__("os").path.abspath(__file__)))
import svrfmt  # noqa: E402


def unz(b):
    if b[:4] == b'ZLIB':
        csize, usize = struct.unpack_from('<II', b, 8)
        if usize == csize:  # stored
            return b[16:16 + usize]
        for wbits in (15, -15):
            try:
                d = zlib.decompressobj(wbits).decompress(b[16:16 + csize])
                if len(d) == usize:
                    return d
            except zlib.error:
                pass
        raise ValueError('zlib: header %s' % b[:16].hex())
    if len(b) > 2 and b[0] == 0x78 and b[1] in (0x01, 0x9C, 0xDA):  # a bare zlib stream
        try:
            return zlib.decompress(b)
        except zlib.error:
            pass
    return b


def walk(b, depth=0):
    b = unz(b)
    if b[:4] == b'PACH' and depth < 4:
        return svrfmt.pach_write([(i, walk(x, depth + 1)) for i, x in svrfmt.pach_read(b)])
    return b


src, out = sys.argv[1], sys.argv[2]
h, g, t = svrfmt.epac_read(open(src, 'rb').read())
g2 = [(typ, [(nm, walk(b)) for nm, b in es]) for typ, es in g]
data = svrfmt.epac_write(h, g2, t)
open(out, 'wb').write(data)
print(out, len(data))
