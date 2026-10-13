"""WWE '13 (Xbox 360) entrance -> SvR 2011 entrance; also WWE '12, 2K14, 2K15, 2K16, 2K17 (--game ...).

Used by tools/entrance_port.py (game id wwe13); formats and rules: docs/OTHER_GAMES_ENTRANCES.md
(WWE '13). Standalone:

  (every command takes --game wwe13 | wwe12 | wwe2k14, default wwe13)
  python wwe13_entrance.py list <WWE13 extract> [--svrmod exe]
  python wwe13_entrance.py convert <WWE13 extract> <entrance> <2011 number> <out dir> [--game-pac dir] [--svrmod exe]
  python wwe13_entrance.py validate <WWE13 extract> --game-pac <2011 pac dir> <n> [<n> ...] [--step k]
          (converts n and compares it with 2011's own entrance n: FK joint errors, cameras, events)

Source: pac/evt/Entrance00..08.pac, Entrance_dlc01.pac, xcompressed (0x0FF512ED): decoded with the
Mod Maker's `svrmod xdec` (found next to this script, in out/modmaker, in a 'Mod Maker Tools' folder
of an ancestor, or --svrmod / SVRMOD), then EPK8 groups 'EVP ' (script) and 'EVZ ' (tracks).

WWE '12: pac/evt/Nyujyo0-9,A.pac, plain EPK8 (no xcompress), groups EVP1-5,E-I,K / EVZ1-5,E-I,K; same
records, but the bodies are plain FK (no IK calves: the retarget runs with ik=False); its own event
kinds prop/camera 31/t1 and char 130/t1 are dropped (reported).
WWE 2K14: pac/evt/Entrance00..08.pac, xcompressed, the inside is 'EPAC' with EPK8-style group headers
(kind bit 0: 8-byte names, count field / 4; else 4-byte names, count field / 3); motion leaves carry an
id string ("25000101_A00_def", ignored: the leaf index is the object); 40 EVPs have a new tail
(type-13 records: ignored, the standard tail is written); generic extra 25500 -> 13902.

WWE 2K15: pac/evt/Entrance00-10.pac, xcompressed EPK8 (groups EVP2 / EVZ); 2K16 / 2K17: one pac per scene,
pac/evt/NNNNN.pac (entrances 100-899; xcompressed or not), 'EPAC' with 20-byte entries (4-character names:
the number is the file name's) or 2K17's 'HSPC' (BPE blocks: the EVP, then the tracks as 'SHDC' instead of
PACH). From 2K15 on the EVP's slots are 788 bytes: words 0-50 + 195-196 are 2K14's 212-byte slot, the rest
longer rope / turnbuckle key lists (dropped: 2011 holds 8); evp_788_to_212 converts. 2K16 / 2K17 motion is
YANM v5 (read as v4); mode-4 YANM blocks (boned props only) are skipped. 2K15 "one-take" entrances (the whole
entrance in segment 0, which 2011 plays as the hidden intro) are reported: take 2K16's re-cut instead.

The EVP is 2011's own layout (same objects, 212-byte slots, ids 25000+n). Rules:
- object count flag bits masked, the 2011 standard tail (music 999, movie 999), +0x44 = 0;
- positions are in cm (10 x 2011): slot words 4-6, 10-12, 16-18, 22-24, 28-30 of characters, the
  same plus 34-36, 40-42, 46-48 of ropes, 4-6 of turnbuckles are divided by 10 (POS_WORDS; checked
  on the shared pairs: thousands of words at ratio 10.0);
- object 0 -> 16102 (preview body; the entrant replaces it; a female entrant would be 18202 in
  Yuke's data, not applied), generic extras 19202 / 10402 / 51200 / 12502 -> 16102 (objects 1..6)
  or 13902, other characters kept when 2011 has the model, else 13902 (reported);
- props 2029 -> 5009; props 2011's evtobj.pac does not have are reported.
The EVZ (PACH: one WWE '13 motion record per object, blocks 'XXXXYYYY' = EVT key x, y) becomes a
2011 YMKs bank (T11, ids 25000 + object), 60 Hz -> 30 Hz (2011 frames = (frames13 - 1) // 2,
event frame f13 -> f13 // 2):
- characters (y0): retargeted (tools/w13_retarget.py) to dense 110-byte 0x7d keys (count 0);
- cameras (y0, y1): 29-byte mode-1 keys: pos = eye x 10 (raw), rotation = euler_zyx(R13 * M),
  M columns (-e1, -e2, e0), fov from the fov track; y20/y21 dropped;
- ropes / turnbuckles: 5-byte keys [count][s16 10*rope.y][s16 10*rope.z] where the value changes;
- props: 2011's most common track for the same prop model (they are 2-key held poses in WWE '13),
  else events only;
- the global (lights / effects) object: the tracks of the stock 2011 entrance whose global object
  has the same slots (else 101's), copied;
- events: YAT2 commands -> 2011 ops by the table EVENTS (from the 150 entrances both games ship);
  unmapped ones are dropped and counted.
"""
import collections
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import hashlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
import svrfmt  # noqa: E402
import ymks  # noqa: E402
import svr08_entrance as S  # noqa: E402
import yanm_decode as Y  # noqa: E402
import w13_retarget as WR  # noqa: E402

TYPES = {S.TYPE_CHAR: 'char', S.TYPE_PROP: 'prop', (0x10000, 0): 'camera', (1, 0): 'rope',
         (0, 0x01000000): 'turnbuckle', S.TYPE_GLOBAL: 'global'}
GENERIC13 = {19202, 10402, 51200, 12502, 13902, 25500}
GAMES = {'wwe13': dict(name="WWE '13", files=r'(?i)entrance(\d\d|_dlc\d\d)\.pac$', ik=True),
         'wwe12': dict(name="WWE '12", files=r'(?i)nyujyo[0-9a-z]\.pac$', ik=False),
         'wwe2k14': dict(name='WWE 2K14', files=r'(?i)entrance\d\d\.pac$', ik=True),
         'wwe2k15': dict(name='WWE 2K15', files=r'(?i)entrance\d\d\.pac$', ik=True, slot788=True),
         'wwe2k16': dict(name='WWE 2K16', files=r'^\d{5}\.pac$', ik=True, slot788=True, per_file=True),
         'wwe2k17': dict(name='WWE 2K17', files=r'^\d{5}\.pac$', ik=True, slot788=True, per_file=True)}
XMAGIC = b'\x0f\xf5\x12\xed'
# slot words (u32 index in the 212-byte slot) holding positions in cm: 2011 = WWE '13 / 10
POS_WORDS = {'char': [w + j * 6 + d for j in range(5) for d in range(3) for w in (4,)],
             'rope': [4 + j * 6 + d for j in range(8) for d in range(3)],
             'turnbuckle': [4, 5, 6]}


def scale_slot(s, words):
    s = bytearray(s)
    for w in words:
        v = struct.unpack_from('>f', s, 4 * w)[0]
        if v != 0.0 and math.isfinite(v):
            struct.pack_into('>f', s, 4 * w, v / 10.0)
    return bytes(s)
SEGS = (0, 5, 10, 15, 20)
H_EYE, H_TGT, H_FOV, H_ROPE = '#5edfdf5f', '#70d146a5', '#70d304a4', 'rope'
Y.add_names(['rope'])
CAM_M = np.column_stack([[0, -1, 0], [0, 0, -1], [1, 0, 0]]).astype(float)   # (-e1, -e2, e0)


class ToolMissing(Exception):
    """svrmod.exe not found (bad input: exit 2)"""


def log(*a):
    print(*a, file=sys.stderr)


# ============================================================================ source

def find_svrmod(arg=None):
    cands = [arg] if arg else []
    if os.environ.get('SVRMOD'):
        cands.append(os.environ['SVRMOD'])
    cands += [os.path.join(HERE, 'svrmod.exe'), os.path.join(HERE, '..', 'out', 'modmaker', 'svrmod.exe')]
    for start in (os.getcwd(), HERE):
        p = os.path.abspath(start)
        for _ in range(8):
            cands += [os.path.join(p, 'Mod Maker Tools', 'svrmod.exe'),
                      os.path.join(p, 'Game Files', 'Mod Maker Tools', 'svrmod.exe'), os.path.join(p, 'svrmod.exe')]
            p = os.path.dirname(p)
    for c in cands:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    if arg:
        raise ToolMissing('--svrmod not found: %s' % arg)
    raise ToolMissing('svrmod.exe (the Mod Maker\'s pac tool, needed to decode WWE \'13 pacs) was not found: '
                      'put it next to this script or give --svrmod')


def xdec(raw, svrmod):
    """WWE '13 / 2K14 xcompressed pac -> EPK8 / EPAC bytes (svrmod xdec), cached in the temp folder"""
    if raw[:4] != XMAGIC:
        return raw
    if not svrmod:
        raise ToolMissing('svrmod.exe (the Mod Maker\'s pac tool, needed to decode xcompressed pacs) was not found: '
                          'put it next to this script or give --svrmod')
    key = hashlib.sha1(raw[:1 << 16] + struct.pack('<Q', len(raw)) + raw[-4096:]).hexdigest()[:20]
    cache = os.path.join(tempfile.gettempdir(), 'svr_wwe13_xdec')
    os.makedirs(cache, exist_ok=True)
    out = os.path.join(cache, key + '.epk8')
    if os.path.exists(out) and os.path.getsize(out) > 0x4000:
        return open(out, 'rb').read()
    inp = os.path.join(cache, key + '.in')
    open(inp, 'wb').write(raw)
    flags = 0
    if os.name == 'nt':
        flags = 0x00000040 | 0x08000000          # IDLE_PRIORITY_CLASS | CREATE_NO_WINDOW
    r = subprocess.run([svrmod, 'xdec', inp, out + '.tmp'], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       creationflags=flags)
    try:
        os.remove(inp)
    except OSError:
        pass
    if r.returncode != 0 or not os.path.exists(out + '.tmp'):
        raise RuntimeError('svrmod xdec failed: %s' % (r.stderr or r.stdout).decode('latin-1', 'replace').strip()[:300])
    os.replace(out + '.tmp', out)
    d = open(out, 'rb').read()
    if d[:4] not in (b'EPK8', b'EPAC', b'HSPC'):
        raise RuntimeError('svrmod xdec did not give an EPK8 / EPAC / HSPC pac')
    return d


def epk8_entries(d):
    """EPK8 / 2K14 EPAC -> {(group, name): unpacked bytes}; group header {4CC, u16 count field, u16 kind, u32};
    kind bit 0: 16-byte entries (name[8]), count field / 4; else 12-byte (name[4]), count field / 4 in EPK8
    and / 3 in EPAC"""
    out = collections.OrderedDict()
    epac = d[:4] == b'EPAC'
    tsz = struct.unpack_from('<I', d, 4)[0]
    p, end = 0x800, (0x4000 if epac else 0x800 + tsz)
    while p < end and d[p:p + 4].strip(b'\0'):
        typ = d[p:p + 4].decode('latin-1')
        cnt4, kind = struct.unpack_from('<HH', d, p + 4)
        p += 12
        nl = 8 if kind & 1 else 4
        for _ in range(cnt4 // (3 if epac and not kind & 1 else 4)):
            nm = d[p:p + nl].decode('latin-1').strip(' \0')
            sec, sz = struct.unpack_from('<II', d, p + nl)
            b = d[0x4000 + sec * 0x800:0x4000 + sec * 0x800 + sz * 0x100]
            out[(typ, nm)] = svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b
            p += nl + 8
    return out


def epac16_entries(d):
    """2K16 / 2K17 per-entrance EPAC: groups {4CC, u16 count field, u16 kind, u32 0}, 20-byte entries
    {name[4], u32 sector, u32 size / 0x100, u32 size2 / 0x100, u32}: count field / 4"""
    out = {}
    p = 0x800
    while p < 0x4000 and d[p:p + 4].strip(b'\0'):
        typ = d[p:p + 4].decode('latin-1')
        cnt, kind = struct.unpack_from('<HH', d, p + 4)
        p += 12
        for _ in range(cnt // 4):
            nm = d[p:p + 4].decode('latin-1').strip(' \0')
            sec, sz = struct.unpack_from('<II', d, p + 4)
            b = d[0x4000 + sec * 0x800:0x4000 + sec * 0x800 + sz * 0x100]
            out[(typ, nm)] = svrfmt.bpe_decode(b) if b[:4] == b'BPE ' else b
            p += 20
    return out


def shdc_leaves(b):
    """2K17 'SHDC' (PACH's place in HSPC pacs): u32 count at 0x38, table at u32 0x1C of {u32 leaf index,
    u32 offset, u32 size, u32 0} -> [(leaf index, bytes)]"""
    n = struct.unpack_from('<I', b, 0x38)[0]
    t = struct.unpack_from('<I', b, 0x1C)[0]
    out = []
    for i in range(n):
        idx, off, sz, _ = struct.unpack_from('<4I', b, t + 16 * i)
        out.append((idx, b[off:off + sz]))
    return out


def scene_entries(d):
    """2K16 / 2K17 scene pac -> (EVP bytes, track leaves [(index, bytes)]) or None"""
    evp = evz = None
    if d[:4] == b'EPAC':
        for (t, n), v in epac16_entries(d).items():
            if t.startswith('EVP') or t.startswith('EVE'):
                evp = v
            elif t.startswith('EVZ') or t.startswith('EVL'):
                evz = svrfmt.pach_read(v) if v[:4] == b'PACH' else shdc_leaves(v)
    elif d[:4] == b'HSPC':
        for m in re.finditer(rb'BPE \x00\x01\x00\x00', d):
            b = svrfmt.bpe_decode(d[m.start():])
            if b[:4] == b'0FOP':
                evp = b
            elif b[:4] == b'SHDC':
                evz = shdc_leaves(b)
            elif b[:4] == b'PACH':
                evz = svrfmt.pach_read(b)
    return (evp, list(evz)) if evp and evz is not None else None


def evp_788_to_212(d):
    """2K15+ EVP (788-byte slots) -> 2K14 / 2011 layout (212-byte slots: words 0-50 + 195-196)"""
    n = struct.unpack_from('>I', d, 0x40)[0] & 0xffff
    t = struct.unpack_from('>I', d, 0x48)[0] + 8
    e = dict(num=struct.unpack_from('>H', d, 0x20)[0], name=d[0x22:0x40], f10=S._u32(d, 0x10), f18=S._u32(d, 0x18),
             f44=S._u32(d, 0x44), objs=[], tail=None, game='2011', n_hi=0)
    for i in range(n):
        idx, model, t0, t1, w4, w5, ns, bp = struct.unpack_from('>8I', d, t + 32 * i)
        slots = []
        for k in range(ns >> 16):
            sl = d[bp + 8 + 788 * k:bp + 8 + 788 * (k + 1)]
            slots.append(sl[:51 * 4] + sl[195 * 4:197 * 4])
        e['objs'].append(dict(idx=idx, model=model, t0=t0, t1=t1, w4=w4, w5=w5, nslots=ns >> 16, lo=ns & 0xffff,
                              slots=slots))
    return S.build_evp(e)


def leaves_of(evz):
    """track leaves {object index: motion record} from a PACH (bytes) or a leaf list (2K17 SHDC)"""
    return dict(evz) if isinstance(evz, list) else dict(svrfmt.pach_read(evz))


def read_source(src, svrmod, game='wwe13'):
    """src: object with listdir/read (entrance_port.FolderSource or discfs) -> (evp {n: bytes}, evz {n: bytes});
    svrmod may be None for WWE '12 (plain pacs)"""
    evp, evz = {}, {}
    g_ = GAMES[game]
    pacs = [f for f in src.listdir('pac/evt') if re.match(g_['files'], f)]
    if g_.get('per_file'):
        pacs = [f for f in pacs if 100 <= int(f[:5]) <= 899]
    if not pacs:
        raise FileNotFoundError('no entrance pacs in pac/evt (a %s extract or disc?)' % g_['name'])
    if g_.get('per_file'):
        for f in sorted(pacs):
            r = scene_entries(xdec(src.read('pac/evt/' + f), svrmod))
            if r:
                evp[int(f[:5])] = evp_788_to_212(r[0])
                evz[int(f[:5])] = r[1]
        return evp, evz
    for f in sorted(pacs):
        for (g, n), b in epk8_entries(xdec(src.read('pac/evt/' + f), svrmod)).items():
            if n.isdigit() and int(n) < 10000:
                if g.startswith('EVP'):
                    evp[int(n)] = evp_788_to_212(b) if g_.get('slot788') else b
                elif g.startswith('EVZ'):
                    evz[int(n)] = b
    return evp, evz


def parse_evp13(d):
    d = bytearray(d)
    struct.pack_into('>I', d, 0x40, struct.unpack_from('>I', d, 0x40)[0] & 0xffff)
    struct.pack_into('>I', d, 0x1C, 0)       # (7 WWE '13 tails differ; 2011's standard tail is written)
    return S.parse_evp(bytes(d), '2011')


def evp_name(e):
    return e['name'].split(b'\0')[0].decode('latin-1', 'replace').replace('_', ' ').strip()


def kind(o):
    return TYPES.get((o['t0'], o['t1']), '?')


def blocks(leaf):
    """motion record -> [(x, y, offset)]"""
    if len(leaf) < 0x70:
        return []
    out = []
    for b in Y.parse_header(leaf)['blocks']:
        if leaf[b['offset'] + 9] == 4:          # (mode-4 YANM: boned props in 2K16 / 2K17, not decoded)
            continue
        try:
            out.append((int(b['name'][:4]), int(b['name'][4:]), b['offset']))
        except ValueError:
            pass
    return out


def block_events(leaf, off):
    ysz = struct.unpack_from('>I', leaf, off + 4)[0]
    q = leaf.find(b'YAT2', off + 8 + ysz)
    nxt = [o for _, _, o in blocks(leaf) if o > off]
    if q < 0 or (nxt and q >= min(nxt)):
        return []
    return Y.parse_yat2(leaf, q)['events']


def frames11(m):
    return max(1, (m['frames'] - 1) // 2)


# ============================================================================ 2011 side (stock data)

_stock = {}


def stock_2011(game_pac):
    """{number: (evp dict, evt bank)} of 2011's own entrances, + prop ids of evtobj.pac"""
    if game_pac in _stock:
        return _stock[game_pac]
    ents, props = {}, set()
    if game_pac:
        raw = {}
        for f in sorted(os.listdir(os.path.join(game_pac, 'evt'))):
            if re.match(r'(?i)nyujyo.\.pac$', f):
                for (g, n), b in epk8_entries(open(os.path.join(game_pac, 'evt', f), 'rb').read()).items():
                    if n.isdigit() and int(n) < 10000 and g[:3] in ('EVP', 'EVT'):
                        raw.setdefault(int(n), {})[g[:3]] = b
        for n, r in raw.items():
            if 'EVP' in r and 'EVT' in r:
                try:
                    ents[n] = (S.parse_evp(r['EVP'], '2011'), ymks.read_bank(r['EVT']))
                except Exception:
                    pass
        p = os.path.join(game_pac, 'evt', 'evtobj.pac')
        if os.path.exists(p):
            h, groups, t = svrfmt.epac_read(open(p, 'rb').read())
            for ty, es in groups:
                for nm, b in es:
                    nm = nm.decode('latin-1').strip(' \0')
                    if nm.isdigit():
                        props.add(int(nm))
    _stock[game_pac] = (ents, props)
    return _stock[game_pac]


def prop_donor(stock, model):
    """most common 2011 track (data, frames) of a prop with this model, any entrance / segment"""
    c = collections.Counter()
    keep = {}
    for n, (e, b) in stock.items():
        for i, o in enumerate(e['objs']):
            if kind(o) == 'prop' and o['model'] == model:
                for x in b.entries:
                    if x['id'] == 25000 + i and x['y'] == 0 and x['data']:
                        k = (x['data'], x['frames'])
                        c[k] += 1
                        keep[k] = (n, i)
    if not c:
        return None
    (data, fr), cnt = c.most_common(1)[0]
    return data, fr, keep[(data, fr)], cnt


def global_donor(stock, obj13, prefer=None):
    """(number, object index, entries) of the stock 2011 global object whose slots match obj13's best"""
    sig = [s[:4] + s[8:] for s in obj13['slots']]
    best = None
    for n, (e, b) in sorted(stock.items()):
        for i, o in enumerate(e['objs']):
            if kind(o) != 'global':
                continue
            ss = [s[:4] + s[8:] for s in o['slots']]
            score = sum(1 for a, c in zip(sig, ss) if a == c) + (100 if n == prefer else 0) + (0.5 if n == 101 else 0)
            ents = [x for x in b.entries if x['id'] == 25000 + i and x['data']]
            if ents and (best is None or score > best[0]):
                best = (score, n, i, ents)
    return best


# ============================================================================ events

def _vals(d, n):
    """YAT2 command data -> n values (u32 LE each when there is room, else bytes)"""
    if len(d) >= 4 * n:
        return list(struct.unpack_from('<%dI' % n, d, 0))
    return list(d[:n]) + [0] * max(0, n - len(d))


def map_event(cid, typ, d, okind, frame0):
    """-> list of 2011 event byte strings (None = unmapped)"""
    v = _vals(d, 1)[0] if d else 0
    if cid == 254:                                     # end marker
        return []
    if okind in ('char', 'camera', 'prop', 'rope', 'turnbuckle'):
        if cid == 1 and v <= 3:
            return [bytes([100 + v])]
        if cid == 3 and v <= 3:
            return [bytes([96 + v])]
        if cid == 4:
            return [bytes([193]) + d[:2]]
        if cid == 6:
            a, b, c = _vals(d, 3)
            return [bytes([63, a & 0xff, b & 0xff, c & 0xff, (c >> 8) & 0xff])]
        if cid == 8:                               # (a, b, c) -> 2b a (b-50)*2 c 0: 50 -> 0, 100 -> 100
            a, b, c = _vals(d, 3)
            return [bytes([43, a & 0xff, max(0, min(255, (b - 50) * 2)), c & 0xff, 0])]
        if cid == 13:
            return [bytes([62, v & 0xff])]
        if cid == 148:
            return [bytes([81, v & 0xff])]
        if cid == 149:
            return [bytes([80, v & 0xff])]
        if cid == 212:
            return [bytes([162, v & 0xff])]
        if cid == 208:
            return [bytes([137]) + d[:4]]
        if cid == 215:
            return [bytes([171]) + d[:3]]
        if cid == 225:
            return [bytes([108]) + d[:3]]
        if cid == 289:
            return [bytes([44]) + d[:9]]
        if cid == 290:
            return [bytes([45]) + d[:2]]
        if cid in (51, 52) and okind == 'char':
            return []                              # (the layer list: convert_events writes op 64)
    if okind == 'camera':
        if cid == 196:
            return [bytes([128, v & 0xff])]
        if cid == 200:
            a, b = _vals(d, 2)
            return [bytes([58, a & 0xff, b & 0xff])]
        if cid == 205:
            return [bytes([174, v & 0xff])]
        if cid == 192:
            a, b = _vals(d, 2)
            return [bytes([113, a & 0xff])] if b == 0 else [bytes([116, a & 0xff, b & 0xff])]
        if cid == 194:
            a, b = _vals(d, 2)
            return [bytes([130, a & 0xff])] if b == 0 else [bytes([131, a & 0xff, b & 0xff])]
        if cid == 209:
            return [bytes([163, v & 0xff])]
        if cid == 203:
            return [bytes([71, v & 0xff])]
        if cid == 204:
            a, b = _vals(d, 2)
            return [bytes([127, a & 0xff, b & 0xff])]
        if cid == 201:
            return [bytes([165, v & 0xff])]
        if cid == 202:
            return [bytes([164, v & 0xff])]
    if okind == 'prop':
        if cid == 165:
            return [bytes([182])]
        if cid == 179:
            return [bytes([180, v & 0xff])]
        if cid == 256:
            return [bytes([1, 0])]
    if okind in ('rope', 'turnbuckle'):
        if cid == 114:
            return [bytes([160])]
        if cid == 115:
            return [bytes([161])]
    return None


def convert_events(evs, okind, nframes, rep):
    """YAT2 events -> {2011 frame: [event bytes]}"""
    out = collections.defaultdict(list)
    for e in evs:
        f = min(e['frame'] // 2, nframes - 1)
        # face layer list: 5 slots, 0x34 = slot off, 0x33 = slot on -> 2011 op 64: 40 05 + slots 1..4 on/off
        layers = [c['id'] == 51 for c in e['commands'] if c['id'] in (51, 52)]
        done40 = not (okind == 'char' and layers)
        for c in e['commands']:
            d = bytes.fromhex(c['data'])
            if c['id'] in (51, 52) and not done40:
                fl = [1 if v else 0 for v in layers[1:5]] + [0] * max(0, 5 - len(layers))
                out[f].append(bytes([64, len(layers)] + fl[:4]))
                done40 = True
                continue
            r = map_event(c['id'], c['type'], d, okind, e['frame'] == 0)
            if r is None:
                rep['dropped_events'][(okind, c['id'], c['type'])] += 1
                continue
            for b in r:
                if len(b) != S.T11[b[0]]:
                    rep['dropped_events'][(okind, c['id'], c['type'], 'size')] += 1
                    continue
                out[f].append(b)
    for f in list(out):                             # an event block holds at most 127 bytes
        tot, keep = 0, []
        for b in out[f]:
            if tot + len(b) <= 0x7f:
                keep.append(b)
                tot += len(b)
        out[f] = keep
    return out


# ============================================================================ tracks

def camera_track(m, nframes, events):
    recs = []
    for f in range(nframes):
        t = min(2 * f, m['frames'] - 1)
        ev = {n: Y.sample_track(tr, t) for n, tr in m['tracks'].items() if n in (H_EYE, H_TGT, H_FOV)}
        eye = np.array(ev[H_EYE]['pos'] if ev.get(H_EYE) and ev[H_EYE]['pos'] else (0, 0, 0), float)
        R13 = WR.qmat(ev[H_EYE]['rot']) if ev.get(H_EYE) and ev[H_EYE]['rot'] else None
        if R13 is None:                            # (no rotation track: look at the target, no roll)
            tg = np.array(ev[H_TGT]['pos'], float) if ev.get(H_TGT) and ev[H_TGT]['pos'] else eye + (0, 0, 1)
            fwd = tg - eye
            fwd /= (np.linalg.norm(fwd) or 1.0)
            up = np.array([0, -1.0, 0])
            xr = -fwd
            yr = np.cross(up, xr); yr /= (np.linalg.norm(yr) or 1.0)
            zr = np.cross(xr, yr)
            R11 = np.column_stack([xr, yr, zr])
        else:
            R11 = R13 @ CAM_M
        fov = ev[H_FOV]['pos'][0] if ev.get(H_FOV) and ev[H_FOV]['pos'] else 0.7217
        rot = WR.euler_zyx(R11)
        pose = ymks.encode_camera(dict(mode=1, count=0, raw=(tuple(float(v) * 10.0 for v in eye), rot, float(fov))))
        recs.append((f, ymks.join_record(b''.join(events.get(f, [])), pose)))
    return ymks.build_stream(ymks.make_stream(recs, nframes))


def rope_track(m, nframes, events):
    tr = m['tracks'].get(H_ROPE)
    recs, last = [], None
    for f in range(nframes):
        v = (0, 0)
        if tr is not None:
            s = Y.sample_track(tr, min(2 * f, m['frames'] - 1))
            if s['pos']:
                v = (WR.s16(10 * s['pos'][1]), WR.s16(10 * s['pos'][2]))
        pose = b''
        if v != last:
            pose = bytes([0]) + struct.pack('<2h', *v)
            last = v
        ev = b''.join(events.get(f, []))
        if pose or ev:
            recs.append((f, ymks.join_record(ev, pose)))
    return ymks.build_stream(ymks.make_stream(recs, nframes))


def events_track(nframes, events):
    recs = [(f, ymks.join_record(b''.join(ev), b'')) for f, ev in sorted(events.items()) if ev]
    return ymks.build_stream(ymks.make_stream(recs, nframes))


def is_static(m):
    for tr in m['tracks'].values():
        for k, v in tr['parts'].items():
            if isinstance(v, list) and len(v) > 1 and any(val != v[0][1] for t, val in v[1:]):
                return False
    return True


# ============================================================================ convert

def convert(evp13, evz13, number, game_pac=None, name=None, bind='rest', progress=None, game='wwe13'):
    """-> (evp bytes, evt bytes, report dict)"""
    stock, props11 = stock_2011(game_pac)
    rep = dict(notes=[], warnings=[], dropped=[], dropped_events=collections.Counter())
    e, src_num, nm = convert_evp(evp13, number, stock, props11, name, rep)
    return convert_tracks(e, evz13, src_num, nm, stock, rep, bind, progress, ik=GAMES[game]['ik'])


def convert_evp(evp13, number, stock, props11, name, rep):
    """-> (2011 EVP dict, WWE '13 number, name)"""
    e = parse_evp13(evp13)
    src_num = e['num']
    nm = name if name else evp_name(e)
    nb = nm.encode('latin-1', 'replace')[:29]
    e['name'] = nb + bytes(30 - len(nb))
    e['num'] = number
    e['f44'] = 0
    e['f18'] = 0x10000
    e['tail'] = dict(num=number, recs=[(1, 0, 100, [(2, 0, 999), (1, 0, 0), (1, 0, 0)]),
                                       (8, 0, 100, [(3, 0, 999), (3, 0, 0), (3, 0, 0)])])
    e['game'] = '2011'
    chars11 = set()
    for ee, b in stock.values():
        chars11 |= {o['model'] for o in ee['objs'] if kind(o) == 'char'}
    for i, o in enumerate(e['objs']):
        k = kind(o)
        if k in POS_WORDS:
            o['slots'] = [scale_slot(s, POS_WORDS[k]) for s in o['slots']]
        if k == 'char':
            if i == 0:
                o['model'] = S.PREVIEW_CHAR_11
            elif o['model'] in GENERIC13:
                # Yuke's: extras 1..6 next to a preview entrant use the preview body (about 3:1)
                o['model'] = S.PREVIEW_CHAR_11 if 1 <= i <= 6 else S.GENERIC_CHAR_11
            elif stock and o['model'] not in chars11:
                rep['warnings'].append('object %d: character model %d is not in 2011: generic body 13902' % (i, o['model']))
                o['model'] = S.GENERIC_CHAR_11
        elif k == 'prop':
            if o['model'] in S.PROP_MAP:
                rep['notes'].append('object %d: prop %d -> %d' % (i, o['model'], S.PROP_MAP[o['model']]))
                o['model'] = S.PROP_MAP[o['model']]
            if props11 and o['model'] not in props11:
                rep['warnings'].append('object %d: prop %d is not in 2011\'s evtobj.pac (it will not show)' % (i, o['model']))
        elif k == '?':
            rep['warnings'].append('object %d: unknown object type %08x/%08x kept' % (i, o['t0'], o['t1']))
    return e, src_num, nm


def convert_tracks(e, evz13, src_num, nm, stock, rep, bind='rest', progress=None, ik=True):
    leaves = leaves_of(evz13)
    RT = WR.Retarget(bind, ik=ik)
    ents = []
    todo = [(i, leaves[i]) for i in sorted(leaves) if i < len(e['objs']) and blocks(leaves[i])]
    ent = next((i for i, o in enumerate(e['objs']) if kind(o) == 'char'), None)
    if ent in leaves and {x for x, y, _ in blocks(leaves[ent]) if y == 0} == {0}:
        rep['warnings'].append('one-take entrance: the whole entrance is in segment 0, which 2011 plays as the '
                               'hidden intro (the entrant will not show); use another game\'s cut of it (2K16)')
    nwork = sum(len(blocks(l)) for _, l in todo) or 1
    done = 0
    for i, leaf in todo:
        o = e['objs'][i]
        k = kind(o)
        if k == 'global':
            continue
        for x, y, off in blocks(leaf):
            done += 1
            if progress:
                progress(done, nwork, k)
            m = Y.decode(leaf, y=off, sample=False)
            nf = frames11(m)
            evs = convert_events(block_events(leaf, off), k, nf, rep)
            if k == 'char':
                if y != 0:
                    rep['dropped'].append('object %d (%d,%d): character track y%d' % (i, x, y, y))
                    continue
                data = WR.body_track(RT.keys(m, nf), evs)
            elif k == 'camera':
                if y not in (0, 1):
                    rep['dropped'].append('object %d (%d,%d): camera track y%d' % (i, x, y, y))
                    continue
                data = camera_track(m, nf, evs)
            elif k in ('rope', 'turnbuckle'):
                data = rope_track(m, nf, evs)
            elif k == 'prop':
                don = prop_donor(stock, o['model'])
                if don:
                    data, nf = don[0], don[1]
                    if not is_static(m):
                        rep['notes'].append('object %d (%d,%d): animated WWE \'13 prop pose replaced by 2011\'s %d track' % (i, x, y, o['model']))
                else:
                    data = events_track(nf, evs)
                    rep['notes'].append('object %d (%d,%d): prop %d has no 2011 track: events only (held pose)' % (i, x, y, o['model']))
            else:
                rep['dropped'].append('object %d (%d,%d): %s track' % (i, x, y, k))
                continue
            ents.append(dict(y=y, x=x, id=25000 + i, frames=nf, rt=S.RT_2011, data=data, share=False, tail_off=None))
    # ---- global objects: a stock donor's tracks
    for i, o in enumerate(e['objs']):
        if kind(o) != 'global':
            continue
        don = global_donor(stock, o, prefer=src_num if src_num in stock and
                           evp_name(stock[src_num][0]).lower().replace(' ', '') == nm.lower().replace(' ', '') else None)
        if not don:
            rep['warnings'].append('object %d: global (lights/effects) object without tracks (no 2011 pac folder)' % i)
            continue
        score, dn, di, dents = don
        rep['notes'].append('object %d: global object tracks from 2011 entrance %d object %d (%d slots alike)' % (i, dn, di, int(score) % 100))
        for x in dents:
            ents.append(dict(y=x['y'], x=x['x'], id=25000 + i, frames=x['frames'], rt=S.RT_2011, data=x['data'],
                             share=False, tail_off=None))
    ents.sort(key=lambda z: ymks.key_of(z['id'], z['x'], z['y']))
    bank = ymks.Bank(b'YMKs' + struct.pack('<I', 0x100) + bytes(8) + S.T11, ents)
    bank.lead = b''
    evt = ymks.write_bank(bank)
    rep['dropped'] += ['events %s: %d' % ('/'.join(str(v) for v in key), n) for key, n in sorted(rep['dropped_events'].items(), key=str)]
    del rep['dropped_events']
    return S.build_evp(e), evt, rep


def entry_list(evp, evz, game_pac=None):
    stock, _ = stock_2011(game_pac)
    names11 = {evp_name(ee).lower().replace(' ', ''): n for n, (ee, b) in stock.items()}
    out = []
    for n in sorted(evp):
        if n not in evz:
            continue
        try:
            e = parse_evp13(evp[n])
        except Exception as x:
            log('wwe13 %d: EVP not readable (%s)' % (n, x))
            continue
        nm = evp_name(e)
        frames = {}
        leaves = leaves_of(evz[n])
        ci = next((i for i, o in enumerate(e['objs']) if kind(o) == 'char'), 0)
        if ci in leaves:
            for x, y, off in blocks(leaves[ci]):
                if y == 0 and x in SEGS:
                    try:
                        frames[x // 5] = frames11(Y.decode(leaves[ci], y=off, sample=False))
                    except Exception:
                        pass
        out.append(dict(number=n, name=nm, segments=len(frames), frames=[frames.get(k, 0) for k in range(5)],
                        objects=len(e['objs']), props=[o['model'] for o in e['objs'] if kind(o) == 'prop'],
                        has_pyro=any(kind(o) == 'global' for o in e['objs']), has_movie=False, has_theme=False,
                        in_2011=names11.get(nm.lower().replace(' ', ''))))
    return out


# ============================================================================ validation

def sample11(data, nframes):
    """2011 body track -> per frame (root, [21][3]) as the game blends (key f, count c reached at f+c+1)"""
    ks = []
    for f, r in ymks.parse_stream(data).records():
        ev, pose = ymks.split_record(r)
        if len(pose) <= 1:
            continue
        for s in ymks.decode_body(pose)['subs']:
            if 'bones' in s or s['op'] in (0x7e, 0x7f):
                ks.append((f + s.get('count', 0) + 1, f, s.get('root') if s['op'] != 0x7b else None, s.get('bones', {})))
    chans = collections.defaultdict(list)
    for tgt, f, r, bones in ks:
        if r is not None:
            for i in range(3):
                chans[('r', i)].append((f, tgt, float(r[i])))
        for b, v in bones.items():
            for ai, ax in enumerate('xyz'):
                if ax in v:
                    chans[(b, ai)].append((f, tgt, float(v[ax])))
    cur, idx, state, frames = {}, {c: 0 for c in chans}, {}, []
    for fr in range(nframes):
        for c, lst in chans.items():
            while idx[c] < len(lst) and lst[idx[c]][0] <= fr:
                f, tgt, v = lst[idx[c]]
                if c in state:
                    a0, b0, a1, b1 = state[c]
                    v0 = b0 + (b1 - b0) * (1.0 if a1 <= a0 else min(1.0, (fr - a0) / float(a1 - a0)))
                else:
                    v0 = v
                if c[0] != 'r':
                    v = v0 + ((v - v0 + 32768) % 65536 - 32768)
                state[c] = (f, v0, tgt, v)
                idx[c] += 1
            if c in state:
                f0, v0, f1, v1 = state[c]
                t = 1.0 if f1 <= f0 else min(1.0, (fr - f0) / float(f1 - f0))
                cur[c] = v0 + (v1 - v0) * t
        frames.append(([cur.get(('r', i), 0.0) for i in range(3)], [[cur.get((b, i), 0.0) for i in range(3)] for b in range(21)]))
    return frames


JOINTS = ['root', 'atama', 'l_te', 'r_te', 'l_ashi', 'r_ashi']


def validate(evp13, evz13, number, game_pac, step=1, binds=('rest', 'own'), game='wwe13'):
    """compare the conversion of WWE '13 entrance `number` with 2011's own `number` -> report lines"""
    stock, _ = stock_2011(game_pac)
    if number not in stock:
        return ['2011 has no entrance %d' % number]
    e11, b11 = stock[number]
    e = parse_evp13(evp13)
    leaves = leaves_of(evz13)
    lines = ['== %d %s (2011: %s)' % (number, evp_name(e), evp_name(e11))]
    # EVP: converted slots vs 2011's (position words within 1e-3, everything else equal), per object kind
    rp = dict(notes=[], warnings=[], dropped=[])
    ec, _, _ = convert_evp(evp13, number, stock, stock_2011(game_pac)[1], None, rp)
    st = collections.defaultdict(lambda: [0, 0])
    for o, q in zip(ec['objs'], e11['objs']):
        if kind(o) != kind(q):
            continue
        for s, t in zip(o['slots'], q['slots']):
            a = struct.unpack('>53I', s)
            b = struct.unpack('>53I', t)
            ok = all(a[w] == b[w] or (w in POS_WORDS.get(kind(o), ()) and
                     abs(struct.unpack('>f', s[4 * w:4 * w + 4])[0] - struct.unpack('>f', t[4 * w:4 * w + 4])[0]) < 1e-3)
                     for w in range(53) if w != 1)
            st[kind(o)][0 if ok else 1] += 1
    lines.append('  EVP slots equal to 2011 (positions /10): ' + '  '.join('%s %d/%d' % (k, v[0], v[0] + v[1]) for k, v in sorted(st.items())) +
                 '  objects %d vs %d' % (len(ec['objs']), len(e11['objs'])))
    for bind in binds:
        RT = WR.Retarget(bind, ik=GAMES[game]['ik'])
        Sk = WR.Skel11(bind)
        for x, y, off in blocks(leaves.get(0, b'')):
            if y != 0:
                continue
            g = b11.find(25000, x, 0)
            if not g or not g['data']:
                continue
            m = Y.decode(leaves[0], y=off, sample=False)
            nf = frames11(m)
            gt = sample11(g['data'], g['frames'])
            n = min(nf, g['frames'] - 1)
            err, root = [], []
            for f in range(0, n, step):
                r, a = RT.solve(RT.p13.world(m, min(2 * f, m['frames'] - 1)))
                Jo = Sk.joints(Sk.pose(r, a), JOINTS)
                gr, ga = gt[f + 1]
                Jg = Sk.joints(Sk.pose(gr, ga), JOINTS)
                err.append(np.linalg.norm((Jo - Jo[0]) - (Jg - Jg[0]), axis=1) * 10)
                root.append(np.linalg.norm(Jo[0] - Jg[0]) * 10)
            err = np.array(err)
            same = abs(m['frames'] - 2 * g['frames'] - 1) <= 2
            lines.append('  bind %-4s x%-2d frames %d/%d%s  root path %.1f/%.1f cm  ' % (
                bind, x, nf, g['frames'], '' if same else ' (2011 re-cut)', np.median(root), np.percentile(root, 95)) +
                '  '.join('%s %.1f/%.1f' % (j, np.median(err[:, k]), np.percentile(err[:, k], 95)) for k, j in enumerate(JOINTS) if k))
    # events: ours vs 2011's on the same (object, x, y), matched within +-1 frame
    rr = dict(dropped_events=collections.Counter())
    tot = collections.Counter()
    for i, o in enumerate(e['objs']):
        k = kind(o)
        if i not in leaves or k == 'global':
            continue
        for x, y, off in blocks(leaves[i]):
            g = b11.find(25000 + i, x, y)
            m = Y.decode(leaves[i], y=off, sample=False)
            ours = convert_events(block_events(leaves[i], off), k, frames11(m), rr)
            theirs = collections.defaultdict(list)
            if g and g['data']:
                for f, r in ymks.parse_stream(g['data']).records():
                    ev, _ = ymks.split_record(r)
                    if ev:
                        theirs[f] += [bb for op, bb in ymks.parse_events(ev, b11.T)[0] if op is not None]
            for f, lst in ours.items():
                for bb in lst:
                    hit = next((ff for ff in (f, f - 1, f + 1) if bb in theirs.get(ff, [])), None)
                    tot[(k, 'equal' if hit is not None else 'differ')] += 1
                    if hit is not None:
                        theirs[hit].remove(bb)
            tot[(k, '2011 only')] += sum(len(v) for v in theirs.values())
    lines.append('  events (ours vs 2011, +-1 frame): ' + '  '.join(
        '%s %d equal / %d differ / %d 2011-only' % (k, tot[(k, 'equal')], tot[(k, 'differ')], tot[(k, '2011 only')])
        for k in sorted({k for k, _ in tot})) + '; unmapped %s' % dict(rr['dropped_events']))
    # cameras
    for i, o in enumerate(e['objs']):
        if kind(o) != 'camera' or i not in leaves:
            continue
        for x, y, off in blocks(leaves[i]):
            g = b11.find(25000 + i, x, y)
            if y not in (0, 1) or not g or not g['data']:
                continue
            m = Y.decode(leaves[i], y=off, sample=False)
            nf = min(frames11(m), g['frames'])
            ours = ymks.parse_stream(camera_track(m, nf, {})).records()
            theirs = dict(ymks.parse_stream(g['data']).records())
            dp, da, df = [], [], []
            for f, r in ours:
                t = theirs.get(f)
                if not t:
                    continue
                a = ymks.decode_camera(ymks.split_record(r)[1])
                b = ymks.decode_camera(ymks.split_record(t)[1])
                if a['mode'] != 1 or b['mode'] != 1:
                    continue
                dp.append(np.linalg.norm(np.array(a['pos']) - np.array(b['pos'])) * 10)
                Ra, Rb = WR.rotm(a['rot']), WR.rotm(b['rot'])
                da.append(math.degrees(math.acos(max(-1, min(1, float(np.dot(Ra[:, 0], Rb[:, 0])))))))
                df.append(math.degrees(abs(a['fov'] - b['fov'])))
            if dp:
                lines.append('  camera obj %d x%-2d y%d keys %d: pos %.2f/%.2f cm  view dir %.3f/%.3f deg  fov %.3f deg (median/95%%)' % (
                    i, x, y, len(dp), np.median(dp), np.percentile(dp, 95), np.median(da), np.percentile(da, 95), np.median(df)))
    return lines


# ============================================================================ CLI

class _Folder:
    def __init__(self, root):
        self.root = root

    def listdir(self, p):
        q = os.path.join(self.root, *p.split('/'))
        return sorted(os.listdir(q)) if os.path.isdir(q) else []

    def read(self, p):
        return open(os.path.join(self.root, *p.split('/')), 'rb').read()


def _opt(a, k, d=None):
    if k in a:
        i = a.index(k)
        v = a[i + 1]
        del a[i:i + 2]
        return v
    return d


def main(argv):
    a = list(argv)
    if not a:
        print(__doc__)
        return 2
    svr = _opt(a, '--svrmod')
    gp = _opt(a, '--game-pac')
    step = int(_opt(a, '--step', '1'))
    game = _opt(a, '--game', 'wwe13')
    if game not in GAMES:
        log('error: --game must be one of %s' % ', '.join(GAMES))
        return 2
    cmd = a.pop(0)
    try:
        sv = find_svrmod(svr)
    except ToolMissing as x:
        sv = None
        if svr:
            log('error: %s' % x)
            return 2
    try:
        evp, evz = read_source(_Folder(a[0]), sv, game)
    except ToolMissing as x:
        log('error: %s' % x)
        return 2
    if cmd == 'list':
        for r in entry_list(evp, evz, gp):
            print('%4d  %-28s %s objects %d props %s in_2011 %s' % (r['number'], r['name'], r['frames'], r['objects'], r['props'], r['in_2011']))
    elif cmd == 'convert':
        n, num, out = int(a[1]), int(a[2]), a[3]
        os.makedirs(out, exist_ok=True)
        p, t, rep = convert(evp[n], evz[n], num, gp, game=game)
        open(os.path.join(out, '%d_evp.bin' % num), 'wb').write(p)
        open(os.path.join(out, '%d_evt.bin' % num), 'wb').write(t)
        for k in ('notes', 'warnings', 'dropped'):
            for l in rep[k]:
                print(k[:-1] if k != 'dropped' else k, l)
    elif cmd == 'validate':
        for n in a[1:]:
            for l in validate(evp[int(n)], evz[int(n)], int(n), gp, step, game=game):
                print(l)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
