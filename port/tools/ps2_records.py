"""PS2 WWE games' character records (ratings, styles, abilities) for tools/ratings_port.py / abilities_port.py.

    rec = record(src, cid)      # src: ps2_port.GamePS2 (src.g.disc: the discfs image, src.game_id, src.name(c))

Where (research 2026-10-13, every disc): a DPAC's named groups (from 0x800: {tag[4], u16 2n, u16 sector} then
n x {name[4], u16 sector, u16 size / 0x100}, data at 0x4000 + sector * 0x800; numbered groups have bit 15 of the
count and 4-byte {u16 id, u16 size / 0x100} entries packed from their sector). The CHAR group:
  ps2_jbi   M.PAC, entries "0000".."0135" = 512-byte records (the entry name is the record number)
  ps2_sym   PAC/M.PAC, as JBI (records "0000".."0140")
  ps2_hctp  PAC/M.PAC entry "ALL " = a PS2 PAC (u32 n, n x {u16 id, u24 offset, u24 size}) of BPE leaves, 365 B
  ps2_svr04 PAC/M.PAC "ALL ", 384 B;  ps2_svr06 PAC/M.PAC "DAT ", 159 B;  ps2_svr07 / 08 PAC/MISC.PAC "DAT "
  ps2_svr09 / 10 PAC/CH/CHETC.PAC "DAT "  (2007-2010: byte for byte the X360 versions' CHAR/DAT leaves)
A superstar's record: HCTP's by its record number (src.number(cid)); the others by name (NAME_AT).
"""
import re
import struct

SPEC = {  # game id: (file, CHAR entry or None = numbered entries, name offset, name length)
    'ps2_jbi': ('M.PAC', None, 0xF2, 30), 'ps2_sym': ('PAC/M.PAC', None, 0xF8, 30),
    'ps2_hctp': ('PAC/M.PAC', 'ALL', 0x116, 30), 'ps2_svr04': ('PAC/M.PAC', 'ALL', 0x136, 30),
    'ps2_svr06': ('PAC/M.PAC', 'DAT', 0x12, 30), 'ps2_svr07': ('PAC/MISC.PAC', 'DAT', 0x1C, 30),
    'ps2_svr08': ('PAC/MISC.PAC', 'DAT', 0x20, 36), 'ps2_svr09': ('PAC/CH/CHETC.PAC', 'DAT', 38, 32),
    'ps2_svr10': ('PAC/CH/CHETC.PAC', 'DAT', 38, 32),
}
# the PS2 2007-2010 records are the X360 games' (same bytes): their X360 ids in abilities_port / ratings_port
X360_TWIN = {'ps2_svr07': 'svr2007', 'ps2_svr08': 'svr2008', 'ps2_svr09': 'svr2009', 'ps2_svr10': 'svr2010'}


def _groups(head):
    i, out = 0x800, []
    while i + 8 <= 0x4000:
        t = head[i:i + 4]
        if t == bytes(4):
            break
        n = struct.unpack_from('<H', head, i + 4)[0]
        if n & 0x8000:   # numbered
            n &= 0x7FFF
            pos = 0x4000 + struct.unpack_from('<H', head, i + 6)[0] * 0x800
            for k in range(n):
                id_, sz = struct.unpack_from('<HH', head, i + 8 + 4 * k)
                out.append((t, '%04x' % id_, pos, sz * 0x100))
                pos += (sz * 0x100 + 0x7FF) & ~0x7FF
            i += 8 + 4 * n
        else:
            n //= 2
            for k in range(n):
                p = i + 8 + 8 * k
                sec, sz = struct.unpack_from('<HH', head, p + 4)
                out.append((t, head[p:p + 4].decode('latin-1'), 0x4000 + sec * 0x800, sz * 0x100))
            i += 8 + 8 * n
    return out


def table(src):
    """{record number: record bytes} of the game's CHAR table (cached on src)"""
    cached = getattr(src, '_ps2_records', None)
    if cached is not None:
        return cached
    import svrfmt
    f, entry, _no, _nl = SPEC[src.game_id]
    d = src.g.disc
    head = d.read(f, 0, 0x4000)
    out = {}
    for t, name, off, size in _groups(head):
        if t != b'CHAR':
            continue
        if entry is None:
            if name.isdigit():
                out[int(name)] = bytes(d.read(f, off, size))
        elif name.strip() == entry:
            import ps2_char   # (its PS2 'PAC ' reader)
            for k, v in ps2_char.pac(bytes(d.read(f, off, size))).items():
                out[k] = svrfmt.bpe_decode(v) if v[:4] == b'BPE ' else v
    src._ps2_records = out
    return out


def norm(s):
    s = re.sub(r'^the ', '', s.lower().strip())
    s = re.sub(r'[^a-z0-9]', '', s)
    return {'stonecold': 'steveaustin', 'stonecoldsteveaustin': 'steveaustin', 'mrmcmahon': 'vincemcmahon'}.get(s, s)


def record_name(game_id, rec):
    _f, _e, no, nl = SPEC[game_id]
    return rec[no:no + nl].split(b'\0')[0].decode('latin-1').strip()


def record(src, cid):
    """the superstar's record (bytes) or None: by record number (HCTP), else by name"""
    t = table(src)
    if src.game_id == 'ps2_hctp' and hasattr(src, 'number'):
        r = t.get(src.number(cid))
        if r is not None:
            return r
    want = norm(src.name(cid) or '')
    if not want:
        return None
    for n in sorted(t):
        if norm(record_name(src.game_id, t[n])) == want:
            return t[n]
    return None
