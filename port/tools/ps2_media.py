"""PS2 SmackDown superstars' entrance themes and titantrons -> a 2011 superstar mod's theme.mp3 / movie.mp4
(ps2_port.build_ps2 calls media(); Superstar Porting's ps2_port owns the rest of the mod).

    py ps2_media.py list <disc>                      -> JSON [{number, name, music, movie, theme, titantron}]
    py ps2_media.py convert <disc> <number> <out dir> [--vgmstream <exe>]

HCTP (SmackDown! Here Comes the Pain), found in the ELF (symbols): CEventBGM::Start / CEventTitantron::Start
play the profile's music / movie index (CCharProp data +0x17F / +0x17E, a 0x20-byte header ahead of the
record; an edited entrance overrides them). The profiles: PAC/M.PAC group CHAR entry ALL (the path
"/CHAR/ALL" CManDatReader reads), an inner PAC keyed by the character number (ps2_port's: the model id's
digits read as decimal, 39 John Cena), BPE records of 365 bytes in the edit (CAW) format: +0x15F music,
+0x15E movie.
  music i: BGM/BGM00<i / 40>.AFS entry i % 40 (pj_sound_bgmindex2afsbgm), CRI ADX (the AFS name table says
           bgm<i>.adx; "silent48k16bit.adx" = no theme) -> vgmstream -> mp3 at -24 LUFS (2011's level for
           mod themes)
  movie i: MOVIE/PMV00x.AFS entry pal_nmv<i>.sfd (CRI Sofdec: MPEG video + ADX; "missing.sfd" = none)
           -> ffmpeg -> H.264 movie.mp4 without sound (the theme plays on its own); the .bik fallback is
           made at import / install, as for every movie.mp4
Checked: the CHAR/ALL indexes of Triple H / Jericho / HBK point at the bgm tracks whose chroma matches
2011's own recordings of their themes; the movie indexes equal the edit menus' movie list (MOVES.PAC LIST 0).
Other PS2 games: not yet (media() returns {} with a note).
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def log(*a):
    print(*a, file=sys.stderr, flush=True)


def afs(raw):
    """CRI AFS -> [(name, bytes)] (names from the trailing name table, else '')"""
    if raw[:4] != b'AFS\0':
        raise ValueError('not an AFS archive')
    n = struct.unpack_from('<I', raw, 4)[0]
    ents = [struct.unpack_from('<II', raw, 8 + 8 * i) for i in range(n)]
    nt = struct.unpack_from('<I', raw, 8 + 8 * n)[0]
    out = []
    for i, (o, s) in enumerate(ents):
        nm = raw[nt + 48 * i:nt + 48 * i + 32].split(b'\0')[0].decode('latin-1') if nt else ''
        out.append((nm, raw[o:o + s]))
    return out


class HCTP:
    """HCTP and SvR 2004 (the same profile store; the layout by record size): record size -> (label, music
    offset, movie offset, movie archives)"""
    LAYOUTS = {365: ('Here Comes the Pain', 0x15F, 0x15E, 'MOVIE/PMV%03d.AFS'),
               384: ('SmackDown! vs. Raw (2004)', 0x17C, 0x17B, 'MOVIE/NMVP%03d.AFS')}

    def __init__(self, disc):
        import ps2_char as P
        import svrfmt
        self.disc, self.P = disc, P
        m = disc.read('PAC/M.PAC')
        all_ = None
        p = m.find(b'CHAR', 0x800, 0x4000)   # (DPAC header: 'CHAR' {u16, u16 sector}, then {'ALL ', u16 sector, u16 size / 0x100})
        while 0 <= p and m[p + 8:p + 12] != b'ALL ':
            p = m.find(b'CHAR', p + 4, 0x4000)
        if p >= 0:
            sec, sz = struct.unpack_from('<HH', m, p + 12)
            all_ = m[0x4000 + sec * 0x800:0x4000 + sec * 0x800 + sz * 0x100]
        if all_ is None:
            raise ValueError('no CHAR/ALL in PAC/M.PAC')
        self.prof = {k: svrfmt.bpe_decode(v) if v[:4] == b'BPE ' else v for k, v in P.pac(all_).items()}
        size = max(set(len(r) for r in self.prof.values()), key=lambda n: sum(len(r) == n for r in self.prof.values()))
        if size not in self.LAYOUTS:
            raise ValueError('profile records of %d bytes: layout not known' % size)
        self.label, self.music_off, self.movie_off, self.movie_afs = self.LAYOUTS[size]
        self._bgm, self._mov = {}, None

    def indexes(self, number):
        r = self.prof.get(number)
        if not r or len(r) <= max(self.music_off, self.movie_off):
            return None, None
        return r[self.music_off], r[self.movie_off]

    def bgm(self, i):
        f = 'BGM/BGM%03d.AFS' % (i // 40)
        if f not in self._bgm:
            try:
                self._bgm[f] = afs(self.disc.read(f))
            except Exception:   # noqa: BLE001
                self._bgm[f] = []
        ents = self._bgm[f]
        if i % 40 >= len(ents):
            return None, None
        return ents[i % 40]

    def movie(self, i):
        if self._mov is None:
            self._mov = {}
            for k in range(10):
                try:
                    for nm, b in afs(self.disc.read(self.movie_afs % k)):
                        m = re.match(r'(?:pal_)?nmv(\d+)\.sfd$', nm.lower())
                        if m:
                            self._mov.setdefault(int(m.group(1)), (nm, b))
                except Exception:   # noqa: BLE001
                    break
        return self._mov.get(i, (None, None))

    def generic(self, which, i):
        """an index four or more profiles share is the game's stand-in (placeholder records, a PAL disc's
        missing movies: SvR 2004's The Rock -> movie 57), not the superstar's own; a tag team shares two"""
        return sum(1 for n in self.prof if self.indexes(n)[which] == i) >= 4


class SYM(HCTP):
    """Shut Your Mouth: PAC/M.PAC group CHAR holds per-character move entries "0000".. and then ALL, the
    profiles (an inner PAC keyed by the record number, 329-byte BPE records); CEventBGM / CEventTitantron::
    SetData read the record's music / movie (+0x13F / +0x13E). Music n: ADS/BGM<n / 30>/BGM<n>.ADS (Sony
    SShd/SSbd ADPCM); movie n: MOVIE/TT<n / 30>/<n>.IPU (the ELF's "\\MOVIE\\TT%01d\\%03d.IPU")."""

    def __init__(self, disc):
        import ps2_char as P
        import svrfmt
        self.disc, self.P = disc, P
        m = disc.read('PAC/M.PAC')
        c = m.find(b'CHAR', 0x800, 0x4000)
        p = m.find(b'ALL ', c, 0x4000) if c >= 0 else -1
        if c < 0 or p < 0 or m[c + 8:c + 12] == b'ALL ':
            raise ValueError('not a Shut Your Mouth M.PAC')
        sec, sz = struct.unpack_from('<HH', m, p + 4)
        all_ = m[0x4000 + sec * 0x800:0x4000 + sec * 0x800 + sz * 0x100]
        self.prof = {k: svrfmt.bpe_decode(v) if v[:4] == b'BPE ' else v for k, v in P.pac(all_).items()}
        if not self.prof or len(next(iter(self.prof.values()))) != 329:
            raise ValueError('Shut Your Mouth profiles not found')
        self.label, self.music_off, self.movie_off = 'Shut Your Mouth', 0x13F, 0x13E
        self._bgm, self._mov = {}, {}

    def bgm(self, i):
        rel = 'ADS/BGM%d/BGM%03d.ADS' % (i // 30, i)
        try:
            return 'BGM%03d.ADS' % i, self.disc.read(rel)
        except Exception:   # noqa: BLE001
            return None, None

    def movie(self, i):
        rel = 'MOVIE/TT%d/%03d.IPU' % (i // 30, i)
        try:
            return '%03d.IPU' % i, self.disc.read(rel)
        except Exception:   # noqa: BLE001
            return None, None


GAMES = (HCTP, SYM)


def open_media(disc):
    for G in GAMES:
        try:
            return G(disc)
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
        return None, 'music %d (%s) found, but no vgmstream-cli to decode ADX (opt in: --vgmstream)' % (mi, nm)
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
    nm, b = g.movie(vi)
    if g.generic(1, vi):
        return None, 'movie %d (%s) is the game\'s shared stand-in, not his own' % (vi, nm)
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
    g = open_media(disc)
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
        print(json.dumps({'error': 'usage: list <disc> | convert <disc> <number> <out dir> [--vgmstream <exe>]'}))
        return 2
    disc = discfs.open(argv[2])
    g = open_media(disc)
    if g is None:
        print(json.dumps({'ok': False, 'error': 'not a PS2 game ps2_media reads yet'}))
        return 2
    if argv[1] == 'list':
        out = []
        for n in sorted(g.prof):
            mi, vi = g.indexes(n)
            bn = g.bgm(mi)[0] if mi is not None else None
            out.append(dict(number=n, music=mi, movie=vi, theme=bn, titantron=g.movie(vi)[0] if vi is not None else None))
        print(json.dumps(out, indent=1))
        return 0
    number, outdir = int(argv[3]), os.path.abspath(argv[4])
    vg = argv[argv.index('--vgmstream') + 1] if '--vgmstream' in argv else None
    os.makedirs(outdir, exist_ok=True)
    rep = dict(notes=[], warnings=[], dropped=[])
    res = media('', disc, number, outdir, os.path.join(outdir, '.work'), rep, vg)
    print(json.dumps(dict(ok=True, manifest=res, **rep), indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
