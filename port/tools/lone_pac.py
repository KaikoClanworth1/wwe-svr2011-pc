"""One character pac on its own (no disc): which game it comes from, and the PS2 archives' characters.

    py lone_pac.py detect --src <file> [--svrmod <svrmod.exe>]

prints JSON {ok, file, format, game (when sure) | null, games: [candidates], characters: [...] (PS2 archives),
id (from the file name, 360), note}. superstar_port.py convert --src <file> uses detect() and, for PS2
archives, ps2_disc() (a temp folder that looks like the disc to ps2_char.py).

What tells the games apart (one pac per game looked at, 2026-10-15):
- 360, xcompressed (0x0FF512ED, decoded by svrmod xdec): HSPC = WWE 2K17; EPAC with 20-byte entries = 2K16;
  EPAC = 2K14; EPK8 = WWE '13 or 2K15 - the first model's JBOY header u32 +0x1C: 0x40 '13, 0x48 2K15.
- 360, plain EPK8 with 8-digit names (00010402): SvR 2010 or WWE '12 - JBOY u32 +8: 0x10 '12, 0 SvR 2010.
- 360, plain EPAC with 4-character names: decimal (0402) SvR 2007 / 2008 - 2008's materials name
  g_bDiffTangent, 2007's don't; hex (6802) SvR 2009 or Legends of WrestleMania (same format: asked).
- PS2: DPK8 = SvR 2010; DPAC with named entries (6601) = SvR 2009; DPAC numbered: parts 0/1/10/11 (BPE'd) = Just
  Bring It; plain YOBJ parts: no 100/101 = Shut Your Mouth, else ids ending in 0 = HCTP, none = SvR 2004;
  BPE'd 2/8/9/26/27 = SvR 2006, 2007 or 2008 (asked).
- PS2, one entry cut out of a CH.PAC (an inner "PAC "): wrapped as an archive of one (_detect_entry).
"""
import json
import os
import re
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

XMAGIC = b'\x0f\xf5\x12\xed'
LABELS = {'svr2007': 'SvR 2007', 'svr2008': 'SvR 2008', 'svr2009': 'SvR 2009', 'low': 'Legends of WrestleMania',
          'svr2010': 'SvR 2010', 'wwe12': "WWE '12", 'wwe13': "WWE '13", 'wwe2k14': 'WWE 2K14', 'wwe2k15': 'WWE 2K15',
          'wwe2k16': 'WWE 2K16', 'wwe2k17': 'WWE 2K17', 'ps2_jbi': 'Just Bring It (PS2)', 'ps2_sym': 'Shut Your Mouth (PS2)',
          'ps2_hctp': 'Here Comes the Pain (PS2)', 'ps2_svr04': 'SvR 2004 (PS2)', 'ps2_svr06': 'SvR 2006 (PS2)',
          'ps2_svr07': 'SvR 2007 (PS2)', 'ps2_svr08': 'SvR 2008 (PS2)', 'ps2_svr09': 'SvR 2009 (PS2)',
          'ps2_svr10': 'SvR 2010 (PS2)', 'ps2_svr11': 'SvR 2011 (PS2)'}


class BadInput(Exception):
    pass


def _jboy_words(blob):
    """the first EMD entry's model (child 0, JBOY) header words +8 and +0x1C, or None"""
    import ch_tool
    import svrfmt
    try:
        kids = dict(svrfmt.pach_read(ch_tool.unpack(blob)))
        j = ch_tool.unpack(kids[0])
    except Exception:   # noqa: BLE001
        return None
    if j[:4] != b'JBOY':
        return None
    return struct.unpack_from('>I', j, 8)[0], struct.unpack_from('>I', j, 0x1C)[0]


def _first_emd(groups):
    for t, es in groups:
        if t.strip(b'\0 ') == b'EMD':
            for n, b in es:
                if n.strip(b'\0 ') and b:
                    return n, b
    return None, None


def decode(path, svrmod=None):
    raw = open(path, 'rb').read()
    if raw[:4] == XMAGIC:
        import wwe13_entrance as WE
        sv = WE.find_svrmod(svrmod)
        if not sv:
            raise BadInput('this pac is xcompressed (WWE \'13 - 2K17): svrmod.exe is needed to read it (--svrmod)')
        return WE.xdec(raw, sv), True
    return raw, False


def detect(path, svrmod=None):
    """-> dict(format, games [candidates, the sure one alone], raw (decoded bytes, 360), note)"""
    if not os.path.isfile(path):
        raise BadInput('--src not found: %s' % path)
    with open(path, 'rb') as f:
        head = f.read(0x4000)
    if head[:4] in (b'DPAC', b'DPK8'):
        return _detect_ps2(path, head)
    if head[:4] == b'PAC ':
        return _detect_entry(path)
    raw, x = decode(path, svrmod)
    import ch_tool
    import svrfmt
    import moves_port as MP
    out = dict(format=None, games=[], raw=raw, note='')
    if raw[:4] == b'HSPC':
        out.update(format='HSPC (xcompressed)', games=['wwe2k17'])
        return out
    if x:
        if raw[:4] == b'EPK8':
            n, b = _first_emd(ch_tool.epk8_read(raw)[1])
            w = _jboy_words(b) if b else None
            out['format'] = 'EPK8 (xcompressed)'
            out['games'] = ['wwe13'] if w and w[1] == 0x40 else ['wwe2k15'] if w and w[1] == 0x48 else ['wwe13', 'wwe2k15']
            return out
        if raw[:4] == b'EPAC':
            try:
                MP.read_pac(raw)
                out.update(format='EPAC (xcompressed)', games=['wwe2k14'])
            except ValueError:
                out.update(format='EPAC, 20-byte entries (xcompressed)', games=['wwe2k16'])
            return out
        raise BadInput('an xcompressed file that is not a character pac (%r)' % raw[:4])
    if raw[:4] == b'EPK8':
        n, b = _first_emd(ch_tool.epk8_read(raw)[1])
        w = _jboy_words(b) if b else None
        out['format'] = 'EPK8'
        out['games'] = ['wwe12'] if w and w[0] == 0x10 else ['svr2010'] if w else ['svr2010', 'wwe12']
        return out
    if raw[:4] == b'EPAC':
        groups = svrfmt.epac_read(raw)[1]
        n, b = _first_emd(groups)
        if n is None:
            raise BadInput('no model (EMD) in this pac')
        s = n.decode('latin1').strip('\0 ')
        out['format'] = 'EPAC'
        try:
            data = b''.join(ch_tool.unpack(k) for _, k in svrfmt.pach_read(ch_tool.unpack(b)))
        except Exception:   # noqa: BLE001
            raise BadInput('the model in this pac does not read (%s)' % s)
        if b'g_f4Fairy' in data:   # (2009's shaders; LoW keeps 2009's formats)
            out['games'] = ['svr2009', 'low']
            out['note'] = 'SvR 2009 and Legends of WrestleMania keep the same format (entry %s)' % s
        else:
            out['games'] = ['svr2008'] if b'g_bDiffTangent' in data else ['svr2007']
            out['note'] = 'entry %s' % s
        return out
    raise BadInput('not a character pac this tool knows (%r)' % raw[:4])


def _detect_ps2(path, head):
    import ps2_char as PC
    out = dict(format=head[:4].decode(), games=[], raw=None, note='')
    if head[:4] == b'DPK8':   # (PS2 2010 and 2011 keep the same archive: asked)
        out['games'] = ['ps2_svr10', 'ps2_svr11']
        out['note'] = 'SvR 2010 and SvR 2011 (PS2) keep the same format'
        return out
    ents = PC.dpac(head)
    if not ents:
        raise BadInput('no models in this DPAC')
    named = struct.unpack_from('<H', head, 0x804)[0] & 0x8000 == 0
    if named:
        out['games'] = ['ps2_svr09']
        return out
    with open(path, 'rb') as f:
        f.seek(ents[0][2])
        inner = PC.pac(f.read(ents[0][3]))
    ids = [e[1] for e in ents]
    plain = any(v[:4] == b'YOBJ' for v in inner.values())
    if not plain and 0 in inner and 1 in inner and 10 in inner:   # (JBI: 0/1 the TIM2 strip, 10/11; 2006-2008 have neither)
        out['games'] = ['ps2_jbi']
    elif plain and 100 not in inner:
        out['games'] = ['ps2_sym']
    elif plain:
        out['games'] = ['ps2_hctp'] if sum(1 for i in ids if i & 0xF == 0) > len(ids) // 4 else ['ps2_svr04']
    else:
        out['games'] = ['ps2_svr06', 'ps2_svr07', 'ps2_svr08']
        out['note'] = 'SvR 2006, 2007 and 2008 (PS2) keep the same archive format'
    return out


WHERE_CH = ("the whole character archive is on the game's disc: PAC\\CH.PAC (and CH2.PAC); SvR 2007 - 2010: "
            "PAC\\CH\\CH.PAC; Just Bring It: CH.PAC at the root. Give the Mod Maker the disc's .iso (My game "
            "discs), or copy the file out of the .iso with 7-Zip")


def entry_parts(d):
    """one entry of a PS2 character archive (an inner "PAC ") -> {part: bytes}, BPE'd parts decoded"""
    import ps2_char as PC
    out = PC.pac(d)
    for k, v in out.items():
        if v[:4] == b'BPE ':
            import svrfmt
            out[k] = svrfmt.bpe_decode(v)
    return out


def _detect_entry(path):
    """one model entry cut out of a PS2 CH.PAC: converted on its own when it holds a body (part 2, a YOBJ - the
    skeleton is in it) and its textures (part 9; Just Bring It: the TIM2 strip, part 1). The game goes by
    which parts there are (2026-10-20, every entry of the PAL discs): BPE'd 0/1/10 Just Bring It; plain
    10/11 Shut Your Mouth; plain 8 (RTX3) only HCTP; plain 100/101 HCTP or SvR 2004; BPE'd 26/27 SvR 2006 -
    2008; BPE'd 5/100 SvR 2009."""
    import ps2_char as PC
    raw = open(path, 'rb').read()
    try:
        packed = PC.pac(raw)
        e = entry_parts(raw)
    except Exception:   # noqa: BLE001
        raise BadInput('this looks like one model entry from a PS2 character archive but it does not read: ' + WHERE_CH)
    bpe = any(v[:4] == b'BPE ' for v in packed.values())
    if bpe and 0 in e and 1 in e and 10 in e:
        games = ['ps2_jbi']
    elif not bpe and 10 in e and 11 in e and 100 not in e:
        games = ['ps2_sym']
    elif not bpe and 8 in e and 100 in e:
        games = ['ps2_hctp']
    elif not bpe and 100 in e:
        games = ['ps2_hctp', 'ps2_svr04']
    elif bpe and (26 in e or 27 in e):
        games = ['ps2_svr06', 'ps2_svr07', 'ps2_svr08']
    elif bpe and 100 in e:
        games = ['ps2_svr09']
    else:
        raise BadInput('one model entry from a PS2 character archive, from a game this tool does not tell '
                       '(parts %s): ' % ' '.join(str(k) for k in sorted(e)) + WHERE_CH)
    if 2 not in e or e[2][:4] != b'YOBJ':
        raise BadInput('this is one entry from a PS2 character archive with no model in it: ' + WHERE_CH)
    try:
        y = PC.read_yobj(e[2])
    except PC.BadInput as x:
        raise BadInput('the model in this PS2 entry does not read (%s): ' % x + WHERE_CH)
    if sum(len(m['pos']) for m in y['meshes']) < PC.Yukes.GEAR_VERTS:
        raise BadInput('this PS2 entry is a small model (entrance gear or a pad), not a wrestler: ' + WHERE_CH)
    if (1 if games == ['ps2_jbi'] else 9) not in e:
        raise BadInput("this PS2 entry has the model but not its textures: " + WHERE_CH)
    note = ('one model entry (parts %s): one attire, no name, pictures or entrance gear - those are in the '
            'rest of the archive' % ' '.join(str(k) for k in sorted(e)))
    if len(games) > 1:
        note += '; ' + ' and '.join(LABELS[g] for g in games) + ' keep the same entry format'
    return dict(format='PAC (one PS2 entry)', games=games, raw=None, note=note)


ENTRY_ID = {'ps2_jbi': 0x1, 'ps2_sym': 0x10}   # (the wrapped entry: character 1, attire 0 - ps2_char's id rules)


def _wrap_entry(path, game, dst):
    """one entry -> a DPAC holding it alone (ps2_char.dpac's numbered "EMD" group)"""
    d = open(path, 'rb').read()
    n = (len(d) + 0xFF) // 0x100
    if n > 0xFFFF:
        raise BadInput('this PS2 entry is too big for an archive entry')
    head = bytearray(0x4000)
    head[:4] = b'DPAC'
    struct.pack_into('<4sHHHH', head, 0x800, b'EMD ', 1 | 0x8000, 0, ENTRY_ID.get(game, 0x100), n)
    with open(dst, 'wb') as f:
        f.write(bytes(head) + d + bytes(n * 0x100 - len(d)))


def ps2_disc(path, game, work):
    """a folder ps2_char.py reads as the game's disc: the archive as its CH.PAC (JBI: at the root), SYSTEM.CNF
    with the game's serial (ps2_char's material rules go by game), SYM's own pac names (its check) -> folder"""
    import ps2_port
    d = os.path.join(work, 'ps2_disc')
    if os.path.exists(d):
        shutil.rmtree(d)
    rel = 'CH.PAC' if game == 'ps2_jbi' else 'PAC/CH/CH.PAC' if game in ('ps2_svr07', 'ps2_svr08', 'ps2_svr09', 'ps2_svr10', 'ps2_svr11') \
        else 'PAC/CH.PAC'
    dst = os.path.join(d, *rel.split('/'))
    os.makedirs(os.path.dirname(dst))
    with open(path, 'rb') as f:
        lone = f.read(4) == b'PAC '
    if lone:   # (one entry on its own: wrapped as an archive of one)
        _wrap_entry(path, game, dst)
    else:
        try:
            os.link(os.path.abspath(path), dst)
        except OSError:
            shutil.copyfile(path, dst)
    serial = ps2_port.SERIALS[game][0]
    open(os.path.join(d, 'SYSTEM.CNF'), 'w').write('BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\n' % serial)
    if game == 'ps2_sym':
        for n in ('DJ.PAC', 'FINKEL.PAC'):
            open(os.path.join(d, 'PAC', n), 'wb').close()
    return d


def ps2_characters(path, game, work, cache=None):
    """[{id (hex, ps2_char's), attires: [...]}] of a PS2 archive - kept in cache (a folder; default
    %LOCALAPPDATA%\\SvR2011\\convert cache) by file, size, time and game: ps2_char reads every model's shape"""
    import zlib
    st = os.stat(path)
    key = '%s|%d|%d|%s' % (os.path.abspath(path).lower(), st.st_size, int(st.st_mtime), game)
    if cache is None and os.environ.get('LOCALAPPDATA'):
        cache = os.path.join(os.environ['LOCALAPPDATA'], 'SvR2011', 'convert cache')
    cf = os.path.join(cache, 'ps2chars_%08x.json' % zlib.crc32(key.encode())) if cache else None
    if cf and os.path.exists(cf):
        try:
            c = json.load(open(cf))
            if c.get('key') == key:
                return c['characters']
        except (OSError, ValueError):
            pass
    out = _ps2_characters(path, game, work)
    if cf:
        try:
            os.makedirs(cache, exist_ok=True)
            open(cf + '.part', 'w').write(json.dumps(dict(key=key, characters=out)))
            os.replace(cf + '.part', cf)
        except OSError:
            pass
    return out


def _ps2_characters(path, game, work):
    import ps2_char as PC
    import discfs
    d = ps2_disc(path, game, work)
    g = None
    for G in PC.GAMES:
        try:
            g = G(discfs.open(d))
            break
        except PC.BadInput:
            continue
    if g is None:
        raise BadInput('ps2_char does not read this archive as %s' % LABELS.get(game, game))
    return [dict(id='%x' % c, attires=sorted(ats)) for c, ats in sorted(g.characters().items())]


def file_id(path):
    m = re.search(r'ch_?(\d+)', os.path.basename(path), re.I)
    return int(m.group(1)) if m else None


def main(argv):
    a, i = {}, 2
    while i < len(argv):
        if argv[i].startswith('--') and i + 1 < len(argv):
            a[argv[i][2:]] = argv[i + 1]
            i += 2
        else:
            i += 1
    if len(argv) < 2 or argv[1] != 'detect' or not a.get('src'):
        print(json.dumps({'ok': False, 'error': 'usage: detect --src <file> [--svrmod <svrmod.exe>] [--game <id>]'}))
        return 2
    try:
        r = detect(a['src'], a.get('svrmod'))
        games = [a['game']] if a.get('game') else r['games']
        out = dict(ok=True, file=os.path.abspath(a['src']), format=r['format'], game=games[0] if len(games) == 1 else None,
                   games=[dict(id=g, name=LABELS.get(g, g)) for g in r['games']], note=r['note'], id=None, characters=None)
        if r['games'][0].startswith('ps2_'):
            if len(games) == 1:
                import tempfile
                w = tempfile.mkdtemp(prefix='svr_lone_')
                try:
                    out['characters'] = ps2_characters(a['src'], games[0], w, a.get('cache'))
                finally:
                    shutil.rmtree(w, ignore_errors=True)
        else:
            out['id'] = file_id(a['src'])
    except BadInput as e:
        print(json.dumps({'ok': False, 'error': str(e)}))
        return 2
    print(json.dumps(out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
