"""XACT audio of the Xbox 360 SvR 2007 (version 39, big-endian): a cue's wave
from its wave bank, as a RIFF ffmpeg decodes (XMA2), for the superstar
converters' themes.

    py xact.py cues <sound.pac> [GROUP]                 -> JSON {cue name: wave index}
    py xact.py wav <wave bank .xwb> <index> <out.wav>    one wave as a RIFF (XMA2, ffmpeg decodes it)
    py xact.py theme <extract or ISO> <music id> <out.mp3>   the longest MUS_<id>_* cue -> mp3

SvR 2007 keeps music in music/DVD_Music.xwb (wave bank "WB_Music", 274 XMA
waves) and its cues in pac/sound.pac group XACT entry MUSI: a PACH of the
sound bank (0x3e8, "KBDS") and its XACT C header (0x3e9: the cue enum,
XACT_SOUNDBANK_SB_MUSIC_<music id>_<part> = <cue index>).

Sound bank (version 39): +0x09 u16 simple cue count, +0x0f u16 cue count,
+0x1a u32 simple cues (5 bytes: u8 flags, u32 sound offset). A simple sound:
u8 flags, u16 category, u8 volume, u16 pitch, u8 priority, u16 size, then u16
wave index and u8 wave bank (flags bit 0 clear).

Wave bank (version 39): "DNBW", u32 version, 5 segments {u32 offset, u32
length}: bank data (+0 flags, +4 count, +8 name[64], +72 entry size), entries
(24 bytes: u32 flags | samples << 4, u32 format, u32 data offset, u32 data
length, loop), seek tables, -, wave data. Format: bits 0-1 tag (1 = XMA),
2-4 channels, 5-22 sample rate.
Checked: wave 3 through ffmpeg = vgmstream's decode (correlation 0.99999999).
"""
import json
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def soundbank(pac_bytes, group=b'MUSI'):
    """sound.pac bytes -> (sound bank, C header text) of XACT/<group>"""
    import svrfmt
    import ch_tool
    g = svrfmt.epac_read(pac_bytes)[1]
    e = dict(((ty, n), b) for ty, es in g for n, b in es)[(b'XACT', group)]
    kids = dict(svrfmt.pach_read(ch_tool.unpack(e)))
    return ch_tool.unpack(kids[0x3e8]), ch_tool.unpack(kids[0x3e9]).decode('latin1')


def cue_waves(xsb, header):
    """-> {cue name: wave index} (simple cues of a simple sound)"""
    if xsb[:4] != b'KBDS':
        raise ValueError('not an XACT sound bank (big-endian)')
    nsimple, = struct.unpack_from('>H', xsb, 0x09)
    simple_off, = struct.unpack_from('>I', xsb, 0x1A)
    names = {int(i): n for n, i in re.findall(r'XACT_SOUNDBANK_\w+?_MUSIC_(\w+) = (\d+)', header)}
    if not names:
        names = {int(i): n for n, i in re.findall(r'XACT_SOUNDBANK_\w+?_(\w+) = (\d+)', header)}
    out = {}
    for i in range(nsimple):
        flags = xsb[simple_off + 5 * i]
        so, = struct.unpack_from('>I', xsb, simple_off + 5 * i + 1)
        if so + 12 > len(xsb) or xsb[so] & 1:
            continue
        wave, = struct.unpack_from('>H', xsb, so + 9)
        out[names.get(i, str(i))] = wave
    return out


class WaveBank:
    def __init__(self, f):
        self.f = f
        h = self._read(0, 0x40)
        if h[:4] != b'DNBW':
            raise ValueError('not an XACT wave bank (big-endian)')
        self.segs = [struct.unpack_from('>II', h, 8 + 8 * i) for i in range(5)]
        bd = self._read(*self.segs[0])
        self.flags, self.count = struct.unpack_from('>II', bd, 0)
        self.name = bd[8:72].split(b'\0')[0].decode('latin1')
        self.esize, = struct.unpack_from('>I', bd, 72)

    def _read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def entry(self, i):
        e = self._read(self.segs[1][0] + i * self.esize, self.esize)
        dur, fmt, off, ln = struct.unpack_from('>4I', e, 0)
        return dict(tag=fmt & 3, channels=(fmt >> 2) & 7, rate=(fmt >> 5) & 0x3FFFF, samples=dur >> 4, offset=off, size=ln)

    def riff(self, i):
        """wave i as a RIFF WAVE with an XMA2 format (ffmpeg decodes it)"""
        e = self.entry(i)
        if e['tag'] != 1:
            raise ValueError('wave %d is not XMA (tag %d)' % (i, e['tag']))
        data = self._read(self.segs[4][0] + e['offset'], e['size'])
        ch, rate, samples = e['channels'], e['rate'], e['samples']
        block = 0x8000
        fmt = struct.pack('<HHIIHHH', 0x166, ch, rate, rate * ch * 2, 4, 16, 34)
        fmt += struct.pack('<HIIIIIIIBBH', (ch + 1) // 2, 3 if ch == 2 else 4, samples, block, 0, samples, 0, 0, 0, 4,
                           (len(data) + block - 1) // block)
        return (b'RIFF' + struct.pack('<I', 4 + 8 + len(fmt) + 8 + len(data)) + b'WAVE' + b'fmt ' +
                struct.pack('<I', len(fmt)) + fmt + b'data' + struct.pack('<I', len(data)) + data)


def theme(src, music, out_mp3, work=None, lufs=-24.0):
    """the longest MUS cue of a music id (e.g. 0002_1) of an SvR 2007 disc -> mp3 at `lufs`.
    src: a discfs object (or a folder). -> (cue, seconds) or None"""
    import discfs
    import io
    d = discfs.open(src) if isinstance(src, str) else src
    xsb, hdr = soundbank(d.read('pac/sound.pac'))
    waves = cue_waves(xsb, hdr)
    mine = {n: w for n, w in waves.items() if re.match(r'%04d_\d+$' % music, n)}
    if not mine:
        return None
    path = 'music/DVD_Music.xwb'

    class _F:  # (a seekable reader over the disc file)
        def __init__(self):
            self.p = 0

        def seek(self, p):
            self.p = p

        def read(self, n):
            b = d.read(path, self.p, n)
            self.p += len(b)
            return b
    wb = WaveBank(_F())
    best = max(mine, key=lambda n: wb.entry(mine[n])['samples'])
    e = wb.entry(mine[best])
    work = work or os.path.dirname(os.path.abspath(out_mp3))
    raw = os.path.join(work, 'theme_xma.wav')
    open(raw, 'wb').write(wb.riff(mine[best]))
    import svr10_superstar as S
    src_lufs = S.loudness(raw)
    subprocess.run([S.ffmpeg(), '-hide_banner', '-loglevel', 'error', '-y', '-i', raw, '-af',
                    'volume=%.2fdB' % (lufs - src_lufs), '-c:a', 'libmp3lame', '-b:a', '256k', out_mp3], check=True,
                   stdout=subprocess.DEVNULL)
    try:
        os.remove(raw)
    except OSError:
        pass
    return best, e['samples'] / max(e['rate'], 1)


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[1] == 'cues':
        xsb, hdr = soundbank(open(argv[2], 'rb').read(), (argv[3] if len(argv) > 3 else 'MUSI').encode().ljust(4)[:4])
        print(json.dumps(cue_waves(xsb, hdr), indent=1))
    elif argv[1] == 'wav':
        with open(argv[2], 'rb') as f:
            open(argv[4], 'wb').write(WaveBank(f).riff(int(argv[3])))
    elif argv[1] == 'theme':
        r = theme(argv[2], int(argv[3]), argv[4])
        print(json.dumps({'cue': r[0], 'seconds': round(r[1], 1)} if r else None))
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
