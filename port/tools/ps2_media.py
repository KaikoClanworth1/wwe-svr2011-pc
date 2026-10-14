"""PS2 SmackDown superstars' entrance themes and titantrons -> a 2011 superstar mod's theme.mp3 / movie.mp4
(ps2_port.build_ps2 calls media(); Superstar Porting's ps2_port owns the rest of the mod).

    py ps2_media.py list <disc> [--game ps2_xxx]               -> JSON [{number, music, movie, theme, titantron}]
    py ps2_media.py convert <disc> <number> <out dir> [--game ps2_xxx] [--vgmstream <exe>]

Every game keeps a music and a movie index in the superstar's profile record (the CHAR record number is the
key, as ps2_port's g.number(cid)); themes become theme.mp3 at -24 LUFS (2011's level for mod themes, through
vgmstream), titantrons H.264 movie.mp4 without sound (the theme plays on its own; the .bik fallback is made
at import / install as for every movie.mp4).

ps2_jbi   M.PAC CHAR records "0000".. (512 B): music +0x11B, movie +0x11E; ADS/<n/30>/BGM<n>.ADS, MOVIE/<n/23>/<n>.IPU.
ps2_sym   PAC/M.PAC CHAR group: per-character entries "0000".., then ALL = an inner PAC of 329-byte BPE
          records: music +0x13F, movie +0x13E (CEventBGM / CEventTitantron::SetData in the ELF, which has
          symbols). Music n = ADS/BGM<n/30>/BGM<n>.ADS (Sony ADS); movie n = MOVIE/TT<n/30>/<n>.IPU (the ELF's
          "\\MOVIE\\TT%01d\\%03d.IPU"; ffmpeg's ipu decoder).
ps2_hctp  PAC/M.PAC CHAR/ALL (the "/CHAR/ALL" CManDatReader reads), 365-byte BPE records: music +0x15F, movie
          +0x15E (CEventBGM / CEventTitantron::Start read the profile +0x17F / +0x17E past a 0x20 header).
          Music i = BGM/BGM00<i/40>.AFS entry i%40 (pj_sound_bgmindex2afsbgm), named bgm<i>.adx (CRI ADX);
          movie i = MOVIE/PMV00x.AFS pal_nmv<i>.sfd (CRI Sofdec).
ps2_svr04 as HCTP, 384-byte records: movie +0x17B, music +0x17C (the next byte repeats it); movies
          MOVIE/NMVP00x.AFS nmv<i>.sfd ("missing_PAL.sfd" on the PAL disc where a movie was cut).
ps2_svr06 PAC/M.PAC CHAR/PRO (432 B): movie +0x13C, music +0x13D; music bgm<i>.adx in BGM/BGM<k>.AFS (by name),
          movies MOVIEP/NMVP00x.AFS nmv<i>.sfd.
ps2_svr07-10  the X360 games' records byte for byte: CHAR/PRO (MISC.PAC 2007/08, CH/CHETC.PAC 2009/10);
          entrance block music / movie u16 at PRO +0xE4 (2009) / +0x178 (2010), 2007/2008 have none (the
          record number is both, as the X360 converter has it). Music m = SOUND/MUSIC*.YAF record m*100 (2008-10;
          YAF table {size, offset, id}, records CRI ADX), 2007 MUSIC/ENTRANCE.AFS "<m>_0.adx" (_1 / _2 cuts);
          movie v = MOVIE/NMV*.AFS nmv<v>.sfd.
Left out with a warning (the base's kept): silent / dummy ADX, missing movies, and indexes four or more
profiles share (the game's stand-ins: placeholder records, PAL cuts - SvR 2004's The Rock -> movie 57).
Checked: HCTP Triple H / Jericho / HBK and SYM Triple H / Jericho records point at the tracks whose chroma
matches 2011's own recordings of their themes; HCTP's movie indexes equal its edit menus' movie list.
"""
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def log(*a):
    print(*a, file=sys.stderr, flush=True)


class Afs:
    """a CRI AFS on the disc, read entry by entry: .names [name], .get(i) / .by_name(name) -> bytes"""

    def __init__(self, disc, rel):
        self.disc, self.rel = disc, rel
        h = disc.read(rel, 0, 0x80000)
        if h[:4] != b'AFS\0':
            raise ValueError('%s: not an AFS archive' % rel)
        n = struct.unpack_from('<I', h, 4)[0]
        if 8 + 8 * n + 8 > len(h):
            h = disc.read(rel, 0, 16 + 8 * n)
        self.ents = [struct.unpack_from('<II', h, 8 + 8 * i) for i in range(n)]
        nt, ns = struct.unpack_from('<II', h, 8 + 8 * n)
        raw = disc.read(rel, nt, ns) if nt and ns else b''
        self.names = [raw[48 * i:48 * i + 32].split(b'\0')[0].decode('latin-1') if raw else '' for i in range(n)]

    def get(self, i):
        o, s = self.ents[i]
        return bytes(self.disc.read(self.rel, o, s)) if s else b''


def _listdir(disc, rel):
    try:
        return sorted(disc.listdir(rel))
    except Exception:   # noqa: BLE001
        return []


def _pac_profiles(disc, rel, entry):
    """{record number: record} from a DPAC's CHAR group entry (ALL / PRO), an inner PAC of BPE records"""
    import ps2_char as P
    import ps2_records as R
    import svrfmt
    head = disc.read(rel, 0, 0x4000)
    for t, name, off, size in R._groups(head):
        if t == b'CHAR' and name.strip() == entry:
            raw = bytes(disc.read(rel, off, size))
            return {k: svrfmt.bpe_decode(v) if v[:4] == b'BPE ' else v for k, v in P.pac(raw).items()}
    raise ValueError('no CHAR/%s in %s' % (entry, rel))


class Media:
    """a PS2 game's profiles, music and movies; subclasses fill prof / indexes / bgm / movie"""
    label = ''

    def indexes(self, number):
        """-> (music index, movie index) or (None, None)"""
        raise NotImplementedError

    def bgm(self, i):
        """-> (file name, bytes) or (None, None)"""
        raise NotImplementedError

    def movie(self, i):
        """-> (file name, bytes) or (None, None)"""
        raise NotImplementedError

    NAME_AT = None   # (the record's name offset where the profile records carry one)

    def generic(self, which, i):
        """an index four or more superstars share is the game's stand-in (placeholder records, a PAL disc's cut
        movies), not the superstar's own; a tag team shares two. Counted by name where the records have one
        (JBI keeps copies of The Rock's record as other versions of him), else by record"""
        if not hasattr(self, '_uses'):
            self._uses = ({}, {})
            for n, r in self.prof.items():
                key = n
                if self.NAME_AT is not None:
                    key = r[self.NAME_AT:self.NAME_AT + 30].split(b'\0')[0].strip().lower() or n
                for k, v in enumerate(self.indexes(n)):
                    if v is not None:
                        self._uses[k].setdefault(v, set()).add(key)
        return len(self._uses[which].get(i, ())) >= 4


class AfsNamed(Media):
    """music / movie files found by name in AFS archives: bgm<i>.adx, (pal_)nmv<i>.sfd"""
    BGM_GLOB, MOVIE_GLOB = r'BGM\d*\.AFS$', r'P?N?MVP?\d+\.AFS$'
    BGM_DIR, MOVIE_DIR = 'BGM', 'MOVIE'

    def _index(self, d, pat, rx):
        out = {}
        for f in _listdir(self.disc, d):
            if not re.match(pat, f.upper()):
                continue
            try:
                a = Afs(self.disc, d + '/' + f)
            except Exception:   # noqa: BLE001
                continue
            for k, nm in enumerate(a.names):
                m = re.match(rx, nm.lower())
                if m:
                    out.setdefault(int(m.group(1)), (nm, a, k))
        return out

    def bgm(self, i):
        if not hasattr(self, '_b'):
            self._b = self._index(self.BGM_DIR, self.BGM_GLOB, r'bgm(\d+)\.adx$')
            self._slots = {}
            for f in _listdir(self.disc, self.BGM_DIR):
                m = re.match(r'BGM(\d+)\.AFS$', f.upper())
                if m:
                    try:
                        a = Afs(self.disc, self.BGM_DIR + '/' + f)
                    except Exception:   # noqa: BLE001
                        continue
                    self._slots[int(m.group(1))] = a
        if i in self._b:
            nm, a, k = self._b[i]
            return nm, a.get(k)
        a = self._slots.get(i // 40)   # (HCTP / 2004: index i is BGM00<i/40>.AFS entry i%40 - a stand-in there)
        if a is not None and i % 40 < len(a.names):
            return a.names[i % 40] or 'entry %d' % (i % 40), a.get(i % 40)
        return None, None

    def movie(self, i):
        if not hasattr(self, '_m'):
            self._m = self._index(self.MOVIE_DIR, self.MOVIE_GLOB, r'(?:pal_)?nmv(\d+)\.sfd$')
        if i not in self._m:
            return None, None
        nm, a, k = self._m[i]
        return nm, a.get(k)


class HCTP(AfsNamed):
    """HCTP and SvR 2004 (the profile store PAC/M.PAC CHAR/ALL), SvR 2006 (CHAR/PRO): game id -> (label,
    profile entry, music offset, movie offset, movie folder)"""
    LAYOUTS = {'ps2_hctp': ('Here Comes the Pain', 'ALL', 0x15F, 0x15E, 'MOVIE'),
               'ps2_svr04': ('SmackDown! vs. Raw (2004)', 'ALL', 0x17C, 0x17B, 'MOVIE'),
               'ps2_svr06': ('SmackDown! vs. Raw 2006', 'PRO', 0x13D, 0x13C, 'MOVIEP')}
    SIZES = {365: 'ps2_hctp', 384: 'ps2_svr04', 432: 'ps2_svr06'}

    def __init__(self, disc, game_id=None):
        self.disc = disc
        if game_id is None:   # (by the record size)
            for gid in ('ps2_hctp', 'ps2_svr06'):
                try:
                    prof = _pac_profiles(disc, 'PAC/M.PAC', self.LAYOUTS[gid][1])
                except Exception:   # noqa: BLE001
                    continue
                size = max(set(map(len, prof.values())), key=lambda n: sum(len(r) == n for r in prof.values()))
                if size in self.SIZES:
                    game_id = self.SIZES[size]
                    break
            else:
                raise ValueError('not HCTP / SvR 2004 / 2006')
        self.label, entry, self.music_off, self.movie_off, self.MOVIE_DIR = self.LAYOUTS[game_id]
        self.prof = _pac_profiles(disc, 'PAC/M.PAC', entry)
        self.NAME_AT = {'ps2_hctp': 0x116, 'ps2_svr04': 0x136}.get(game_id)   # (ps2_records; 2006's PRO has none)

    def indexes(self, number):
        r = self.prof.get(number)
        if not r or len(r) <= max(self.music_off, self.movie_off):
            return None, None
        return r[self.music_off], r[self.movie_off]


class SYM(Media):
    """Shut Your Mouth: CHAR group's ALL entry, music +0x13F / movie +0x13E, ADS / IPU files"""
    label = 'Shut Your Mouth'

    def __init__(self, disc, game_id=None):
        self.disc = disc
        self.prof = _pac_profiles(disc, 'PAC/M.PAC', 'ALL')
        if len(next(iter(self.prof.values()))) != 329:
            raise ValueError('not Shut Your Mouth')

    def indexes(self, number):
        r = self.prof.get(number)
        return (r[0x13F], r[0x13E]) if r and len(r) > 0x13F else (None, None)

    def _file(self, rel, nm):
        try:
            return nm, bytes(self.disc.read(rel))
        except Exception:   # noqa: BLE001
            return None, None

    def bgm(self, i):
        return self._file('ADS/BGM%d/BGM%03d.ADS' % (i // 30, i), 'BGM%03d.ADS' % i)

    def movie(self, i):
        return self._file('MOVIE/TT%d/%03d.IPU' % (i // 30, i), '%03d.IPU' % i)


class JBI(SYM):
    """Just Bring It: M.PAC (disc root) CHAR group's numbered records "0000".. (512 B, the record number = the
    entry name): music +0x11B (+0x11F holds a second version of the same theme), movie +0x11E. Music n =
    ADS/<n/30>/BGM<n>.ADS; movie n = MOVIE/<n/23>/<n>.IPU (the ELF's "\\MOVIE\\%02d\\%02d.IPU", 23 a folder).
    Checked: The Rock / Stone Cold / Undertaker / Triple H's +0x11E movies show them; Triple H's and Jericho's
    +0x11B themes match 2011's recordings (chroma 0.56 / 0.44)."""
    label = 'Just Bring It'
    NAME_AT = 0xF2

    def __init__(self, disc, game_id=None):
        import ps2_records as R
        self.disc = disc
        head = disc.read('M.PAC', 0, 0x4000)
        self.prof = {}
        for t, name, off, size in R._groups(head):
            if t == b'CHAR' and name.isdigit():
                self.prof[int(name)] = bytes(disc.read('M.PAC', off, size))
        if not self.prof or len(next(iter(self.prof.values()))) != 512:
            raise ValueError('not Just Bring It')

    def indexes(self, number):
        r = self.prof.get(number)
        return (r[0x11B], r[0x11E]) if r and len(r) > 0x11E else (None, None)

    def bgm(self, i):
        return self._file('ADS/%d/BGM%03d.ADS' % (i // 30, i), 'BGM%03d.ADS' % i)

    def movie(self, i):
        return self._file('MOVIE/%02d/%02d.IPU' % (i // 23, i), '%02d.IPU' % i)


class Twin360(AfsNamed):
    """SvR 2007-2010 (PS2): the X360 versions' records; themes in YAF (2008-10) / ENTRANCE.AFS (2007)"""
    LAYOUTS = {'ps2_svr07': ('SmackDown vs. Raw 2007 (PS2)', 'PAC/MISC.PAC', None),
               'ps2_svr08': ('SmackDown vs. Raw 2008 (PS2)', 'PAC/MISC.PAC', None),
               'ps2_svr09': ('SmackDown vs. Raw 2009 (PS2)', 'PAC/CH/CHETC.PAC', 0xE4),
               'ps2_svr10': ('SmackDown vs. Raw 2010 (PS2)', 'PAC/CH/CHETC.PAC', 0x178)}

    def __init__(self, disc, game_id=None):
        self.disc = disc
        if game_id not in self.LAYOUTS:
            raise ValueError('not a PS2 SvR 2007-2010 (give the game id)')
        self.game_id = game_id
        self.label, rel, self.block = self.LAYOUTS[game_id]
        self.prof = _pac_profiles(disc, rel, 'PRO')

    def indexes(self, number):
        r = self.prof.get(number)
        if r is None:
            return None, None
        if self.block is None:
            return number, number
        if len(r) < self.block + 4:
            return None, None
        return struct.unpack_from('<2H', r, self.block)

    def bgm(self, i):
        if self.game_id == 'ps2_svr07':
            if not hasattr(self, '_e'):
                try:
                    self._e = Afs(self.disc, 'MUSIC/ENTRANCE.AFS')
                except Exception:   # noqa: BLE001
                    self._e = None
            if self._e is not None and '%04d_0.adx' % i in self._e.names:
                k = self._e.names.index('%04d_0.adx' % i)
                return self._e.names[k], self._e.get(k)
            return None, None
        if not hasattr(self, '_y'):
            self._y = {}
            for f in _listdir(self.disc, 'SOUND'):
                if re.match(r'MUSIC\d*\.YAF$', f.upper()):
                    rel = 'SOUND/' + f
                    h = self.disc.read(rel, 0, 0x40000)
                    n, _a, _v, rs, to = struct.unpack_from('<IIIII', h, 0x18)
                    if to + rs * n > len(h):
                        h = self.disc.read(rel, 0, to + rs * n)
                    for k in range(n):
                        size, off, rid = struct.unpack_from('<III', h, to + rs * k)
                        self._y.setdefault(rid, (rel, off, size))
        rid = i * 100
        if rid not in self._y:
            return None, None
        rel, off, size = self._y[rid]
        return '%s record %d.adx' % (rel.split('/')[-1], rid), bytes(self.disc.read(rel, off, size))


def open_media(disc, game_id=None):
    """the game's media reader (by game id; without one, the games found by their files), or None"""
    if game_id in Twin360.LAYOUTS:
        return Twin360(disc, game_id)
    order = {'ps2_jbi': (JBI,), 'ps2_sym': (SYM,), 'ps2_hctp': (HCTP,), 'ps2_svr04': (HCTP,),
             'ps2_svr06': (HCTP,)}.get(game_id, (HCTP, SYM, JBI))
    for G in order:
        try:
            return G(disc, game_id if game_id in HCTP.LAYOUTS else None)
        except Exception:   # noqa: BLE001
            continue
    return None


def theme(g, number, out_mp3, work, vgmstream=None, lufs=-24.0):
    """-> (note, None) when made, (None, why) when not"""
    import svr10_superstar as S
    import wwise
    mi, _ = g.indexes(number)
    if mi is None:
        return None, 'no profile for character %d' % number
    nm, b = g.bgm(mi)
    if g.generic(0, mi):
        return None, 'music %d (%s) is the game\'s shared stand-in, not his own' % (mi, nm)
    if not b or any(k in (nm or '').lower() for k in ('silent', 'dummy')) or len(b) < 300_000:
        return None, 'music %d is %s' % (mi, nm or 'missing')
    vg = wwise.find_vgmstream(vgmstream)
    if not vg:
        return None, 'music %d (%s) found, but no vgmstream-cli to decode it (opt in: --vgmstream)' % (mi, nm)
    os.makedirs(work, exist_ok=True)
    src, wav = os.path.join(work, 'theme' + os.path.splitext(nm)[1].lower()), os.path.join(work, 'theme.wav')
    open(src, 'wb').write(b)
    r = subprocess.run([vg, '-o', wav, src], capture_output=True)
    if r.returncode or not os.path.exists(wav):
        return None, 'vgmstream could not decode music %d (%s)' % (mi, nm)
    gain = lufs - S.loudness(wav)
    subprocess.run([S.ffmpeg(), '-hide_banner', '-loglevel', 'error', '-y', '-i', wav, '-af', 'volume=%.2fdB' % gain,
                    '-c:a', 'libmp3lame', '-b:a', '256k', out_mp3], check=True, stdout=subprocess.DEVNULL)
    for f in (src, wav):
        try:
            os.remove(f)
        except OSError:
            pass
    return 'theme: %s music %d (%s) -> theme.mp3' % (g.label, mi, nm), None


def titantron(g, number, out_mp4, work):
    """-> (note, None) when made, (None, why) when not"""
    import svr10_superstar as S
    _, vi = g.indexes(number)
    if vi is None:
        return None, 'no profile for character %d' % number
    if g.generic(1, vi):
        return None, 'movie %d is the game\'s shared stand-in, not his own' % vi
    nm, b = g.movie(vi)
    if not b:
        return None, 'movie %d is missing on the disc' % vi
    os.makedirs(work, exist_ok=True)
    src = os.path.join(work, 'titantron' + os.path.splitext(nm)[1].lower())
    open(src, 'wb').write(b)
    r = subprocess.run([S.ffmpeg(), '-hide_banner', '-loglevel', 'error', '-y', '-i', src, '-an', '-c:v', 'libx264',
                        '-crf', '20', '-preset', 'medium', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', out_mp4],
                       capture_output=True, text=True, errors='replace')
    try:
        os.remove(src)
    except OSError:
        pass
    if r.returncode or not os.path.exists(out_mp4) or os.path.getsize(out_mp4) < 10_000:
        return None, 'movie %d (%s): ffmpeg could not convert it (%s)' % (vi, nm, r.stderr.strip()[-160:])
    return 'titantron: %s movie %d (%s) -> movie.mp4' % (g.label, vi, nm), None


def media(game_id, disc, number, mod, work, rep, vgmstream=None):
    """ps2_port's hook: theme.mp3 / movie.mp4 into the mod folder -> manifest entries {song, movie} for what
    was made ({} when neither could be: why in rep['warnings']); None when this game's music and movies aren't
    read yet"""
    g = open_media(disc, game_id or None)
    if g is None:
        return None
    out = {}
    note, why = theme(g, number, os.path.join(mod, 'theme.mp3'), work, vgmstream)
    if note:
        out['song'] = 'theme.mp3'
        rep['notes'].append(note)
    else:
        rep['warnings'].append('theme: ' + why)
    note, why = titantron(g, number, os.path.join(mod, 'movie.mp4'), work)
    if note:
        out['movie'] = 'movie.mp4'
        rep['notes'].append(note)
    else:
        rep['warnings'].append('titantron: ' + why)
    return out


def main(argv):
    import discfs
    if len(argv) < 3 or argv[1] not in ('list', 'convert'):
        print(json.dumps({'error': 'usage: list <disc> [--game G] | convert <disc> <number> <out dir> [--game G] [--vgmstream <exe>]'}))
        return 2
    disc = discfs.open(argv[2])
    gid = argv[argv.index('--game') + 1] if '--game' in argv else None
    g = open_media(disc, gid)
    if g is None:
        print(json.dumps({'ok': False, 'error': 'not a PS2 game ps2_media reads (2007-2010 need --game)'}))
        return 2
    if argv[1] == 'list':
        out = []
        for n in sorted(g.prof):
            mi, vi = g.indexes(n)
            out.append(dict(number=n, music=mi, movie=vi,
                            theme=None if mi is None or g.generic(0, mi) else g.bgm(mi)[0],
                            titantron=None if vi is None or g.generic(1, vi) else (g.movie(vi)[0])))
        print(json.dumps(out, indent=1))
        return 0
    number, outdir = int(argv[3]), os.path.abspath(argv[4])
    vg = argv[argv.index('--vgmstream') + 1] if '--vgmstream' in argv else None
    os.makedirs(outdir, exist_ok=True)
    rep = dict(notes=[], warnings=[], dropped=[])
    res = media(gid, disc, number, outdir, os.path.join(outdir, '.work'), rep, vg)
    print(json.dumps(dict(ok=True, manifest=res, **rep), indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
