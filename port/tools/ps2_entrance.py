"""PS2 SmackDown vs. Raw 2007-2010 entrances -> the Xbox 360 versions' bytes, so the 360 converters
(entrance_port.py games svr07 / svr08 / svr09 / svr10) take a PS2 disc too.

The PS2 discs carry the same entrances as the 360 ones (docs/OTHER_GAMES_ENTRANCES.md, "SvR 2007-2011
(PS2)"): the pacs are the 360's containers in little-endian (DPAC 2007-2009, DPK8 2010-2011), the
track banks are the same YMKs banks (little-endian on both), and the EVP script is the 360 layout in
PS2 byte order, with a trap: a plain u32 swap is wrong for some fields -
- +0x10, +0x18, +0x44, the tail's value and item count: a big-endian u32 stored as two little-endian
  u16 (high half first);
- +0x40 (object count): its first two bytes are raw (a 30-character name runs into them);
- the 8 object type bytes and the tail item kind byte: not swapped.
`to_x360(evp)` parses that and rebuilds the 360 bytes with svr08_entrance.build_evp (all 935 shared
entrances rebuild byte-identical).
"""
import collections
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import svrfmt  # noqa: E402
import svr08_entrance as E08  # noqa: E402

SLOT = 212


def groups(d):
    """PS2 DPAC / DPK8 table -> [(group, [(name, sector, size in bytes)])]: groups {4CC, u16 count field,
    u16 start}; DPK8 (bit 0x4000 set): 12-byte entries {name[8], u16 sector, u16 size / 0x100}, count
    field & 0x3FFF / 3; DPAC: 8-byte entries {name[4], ...}, count field / 2"""
    out = []
    p = 0x800
    while p + 8 <= 0x4000 and d[p:p + 4] != bytes(4):
        tag = d[p:p + 4].decode('latin-1')
        n = struct.unpack_from('<H', d, p + 4)[0]
        wide = bool(n & 0x4000)
        es = 12 if wide else 8
        cnt = (n & 0x3FFF) // (3 if wide else 2)
        ents = []
        q = p + 8
        for _ in range(cnt):
            nm = d[q:q + es - 4].replace(b'\0', b'').decode('latin-1').strip()
            sec, sz = struct.unpack_from('<HH', d, q + es - 4)
            ents.append((nm, sec, sz * 0x100))
            q += es
        out.append((tag, ents))
        p = q
    return out


def entries(d):
    """PS2 DPAC / DPK8 -> {(group, name): unpacked bytes}, EVP scripts already in the 360's byte order"""
    out = collections.OrderedDict()
    for tag, ents in groups(d):
        for nm, sec, sz in ents:
            b = d[0x4000 + sec * 0x800:0x4000 + sec * 0x800 + sz]
            if b[:4] == b'BPE ':
                b = svrfmt.bpe_decode(b)
            if tag.startswith('EVP') and is_ps2_evp(b):
                b = to_x360(b)
            out[(tag, nm)] = b
    return out


def is_ps2_evp(d):
    return d[:4] == b'POF0' and d[8:10] == b'VE'


def _u32(d, o):
    return struct.unpack_from('<I', d, o)[0]


def _p16(d, o):
    """a 360 big-endian u32 the PS2 build stores as two little-endian u16 (high half first)"""
    hi, lo = struct.unpack_from('<HH', d, o)
    return hi << 16 | lo


def _swap_slot(s):
    out = bytearray(s)
    struct.pack_into('>H', out, 0, struct.unpack_from('<H', s, 0)[0])
    for k in range(1, 52):
        struct.pack_into('>I', out, 4 * k, _u32(s, 4 * k))
    struct.pack_into('>HH', out, 208, *struct.unpack_from('<HH', s, 208))
    return bytes(out)


def _obj_size(d):
    """28 (2007-2009) or 32 (2010-2011) bytes per object entry: the one whose first slot block follows the table"""
    t = _u32(d, 0x48) + 8
    n = _p16(d, 0x40) & 0xffff
    for es in (28, 32):
        if _u32(d, t + es - 4) + 8 == t + es * n:
            return es
    raise ValueError('PS2 EVP: object entry size not found')


def parse_evp_le(d):
    """PS2 EVP -> the svr08_entrance.parse_evp dict (big-endian slots)"""
    es = _obj_size(d)
    e = dict(num=struct.unpack_from('<H', d, 0x20)[0], name=d[0x22:0x40], f10=_p16(d, 0x10), f18=_p16(d, 0x18),
             f44=_p16(d, 0x44), objs=[], tail=None, game='2008' if es == 28 else '2011',
             n_hi=int.from_bytes(d[0x40:0x42], 'big'))
    n = _p16(d, 0x40) & 0xffff
    t = _u32(d, 0x48) + 8
    for i in range(n):
        q = t + es * i
        if es == 28:
            idx, model = struct.unpack_from('<HH', d, q)
            q += 4
        else:
            idx, model = struct.unpack_from('<II', d, q)
            q += 8
        t0, t1 = struct.unpack_from('>II', d, q)
        w4, w5, ns, bp = struct.unpack_from('<4I', d, q + 8)
        bp += 8
        nslots, lo = ns & 0xffff, ns >> 16
        slots = [_swap_slot(d[bp + SLOT * k:bp + SLOT * (k + 1)]) for k in range(nslots)]
        e['objs'].append(dict(idx=idx, model=model, t0=t0, t1=t1, w4=w4, w5=w5, nslots=nslots, lo=lo, slots=slots))
    tp = _u32(d, 0x1C)
    if tp:
        tp += 8
        num, cnt = struct.unpack_from('<HH', d, tp)
        lp = _u32(d, tp + 4) + 8
        recs = []
        for i in range(cnt):
            typ, z = struct.unpack_from('<HH', d, lp + 16 * i)
            v, m, ip = _p16(d, lp + 16 * i + 4), _p16(d, lp + 16 * i + 8), _u32(d, lp + 16 * i + 12) + 8
            items = []
            for j in range(1 if typ == 9 else m):
                k = d[ip + 8 * j]
                low = int.from_bytes(d[ip + 8 * j + 1:ip + 8 * j + 4], 'big')
                vp = _u32(d, ip + 8 * j + 4) + 8
                items.append((k, low, _u32(d, vp) if k == 2 else struct.unpack_from('<H', d, vp)[0]))
            recs.append((typ, z, v, items, m) if typ == 9 else (typ, z, v, items))
        e['tail'] = dict(num=num, recs=recs)
    return e


def to_x360(d):
    """PS2 EVP bytes -> the 360 version's bytes"""
    e = parse_evp_le(d)
    return E08.build_evp(e, e['game'])
