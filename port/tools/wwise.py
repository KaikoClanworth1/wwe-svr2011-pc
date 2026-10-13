r"""Wwise sound packages (.pck, big-endian Xbox 360) of SvR 2011 and later WWE games:
an event's streamed sound, decoded (themes for the superstar converters).

    py wwise.py events <pck> <name>...            -> JSON {name: [stream id]}
    py wwise.py theme <pck> <event> <out.mp3> [--vgmstream <exe>]

AKPK: header size, version, language map, bank table, stream table (entries
{id, block, size, start block, language}). A bank: chunks BKHD (version), DIDX,
DATA, HIRC (objects). HIRC objects are {type, size, id}: type a u32 in bank
version 46 (SvR 2011), a u8 from version 56 (WWE '12 / '13). Ids are FNV-1 hashes
of names (events: Play_MUS_0101_0_0). Event (type 4) -> actions (type 3, play:
target at +8 of the action body... see target_of) -> sounds (type 2: source id
at +20, the stream) or containers whose bodies list children.

Streams are Wwise Vorbis (RIFX, format 0xFFFF): ffmpeg can't decode them; the
vgmstream-cli can: the port ships it (Mod Maker Tools\vgmstream, r2117; from
port\third_party\vgmstream); else --vgmstream, SVR2011_VGMSTREAM or PATH. Without it the
converters note "no theme" and go on.
"""
import json
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def fnv(s):
    h = 2166136261
    for c in s.lower().encode():
        h = (h * 16777619) & 0xFFFFFFFF
        h ^= c
    return h


class Pck:
    def __init__(self, f):
        """f: a path or a seekable reader (read / seek)"""
        self.f = open(f, 'rb') if isinstance(f, str) else f
        self.f.seek(0)
        hd = self.f.read(0x20)
        if hd[:4] != b'AKPK':
            raise ValueError('not a Wwise package')
        hs, ver, lm, bl, sl = struct.unpack_from('>IIIII', hd, 4)
        self.f.seek(0)
        hd = self.f.read(hs + 8)   # (the whole header: its tables can pass 4 MB, WWE '13)

        def lut(p, size):
            n = struct.unpack_from('>I', hd, p)[0]
            w = 24 if size > 20 * n + 4 else 20
            return [struct.unpack_from('>' + 'I' * (w // 4), hd, p + 4 + w * i) for i in range(n)]
        # the tables start after the header's fields: 0x18 (SvR 2011's version), or 0x1C when the header has an
        # externals table size too (WWE '12 / '13) - whichever adds up to the header size
        ext = struct.unpack_from('>I', hd, 0x18)[0]
        p = (0x1C if 0x1C + lm + bl + sl + ext == hs + 8 else 0x18) + lm
        self.banks = lut(p, bl)
        self.streams = {s[0]: s for s in lut(p + bl, sl)}
        self._objs = None

    @staticmethod
    def _loc(e):
        """(id, block, size, start block) of a table entry: 24 bytes {id, block, size high, size, start,
        language} (SvR 2011) or 20 {id, block, size, start, language} (WWE '12)"""
        return (e[0], e[1], e[3], e[4]) if len(e) == 6 else (e[0], e[1], e[2], e[3])

    def _read(self, off, n):
        self.f.seek(off)
        return self.f.read(n)

    def objs(self):
        if self._objs is None:
            self._objs = {}
            for b in self.banks:
                bid, blk, size, start = self._loc(b)
                d = self._read(start * blk, size)
                p, ver = 0, 46
                while p + 8 <= len(d):
                    tag, ln = struct.unpack_from('>4sI', d, p)
                    if tag == b'BKHD':
                        ver = struct.unpack_from('>I', d, p + 8)[0]
                    elif tag == b'HIRC':
                        n = struct.unpack_from('>I', d, p + 8)[0]
                        q = p + 12
                        for _ in range(n):
                            if ver >= 56:
                                t = d[q]
                                sz, oid = struct.unpack_from('>II', d, q + 1)
                                body = d[q + 9:q + 5 + sz]
                                q += 5 + sz
                            else:
                                t, sz, oid = struct.unpack_from('>III', d, q)
                                body = d[q + 12:q + 8 + sz]
                                q += 8 + sz
                            self._objs[oid] = (t, body)
                    p += 8 + ln
        return self._objs

    def event_streams(self, name, streams=None):
        """event -> [stream source ids] through its play actions and containers"""
        objs = self.objs()
        ev = objs.get(fnv(name))
        if not ev or ev[0] != 4:
            return None
        d = ev[1]
        n = d[0] if len(d) < 4 or struct.unpack_from('>I', d, 0)[0] > 1000 else struct.unpack_from('>I', d, 0)[0]
        start = 1 if (len(d) < 4 or struct.unpack_from('>I', d, 0)[0] > 1000) else 4
        out = []
        for k in range(n):
            if start + 4 * k + 4 > len(d):
                break
            a = objs.get(struct.unpack_from('>I', d, start + 4 * k)[0])
            if not a or a[0] != 3:
                continue
            # the action's target: search its body for an object id that exists
            stack = [struct.unpack_from('>I', a[1], o)[0] for o in range(0, min(len(a[1]) - 3, 16), 1)]
            stack = [o for o in stack if o in objs][:1]
            seen = set()
            while stack:
                o = stack.pop()
                if o in seen or o not in objs:
                    continue
                seen.add(o)
                t, do = objs[o]
                if t == 2:
                    for off in (20, 16, 12, 8, 24, 5):   # (the source id: a stream the package has; +5 in v113 banks, 2K16)
                        if off + 4 <= len(do) and struct.unpack_from('>I', do, off)[0] in (streams or self.streams):
                            out.append(struct.unpack_from('>I', do, off)[0])
                            break
                else:
                    stack += [struct.unpack_from('>I', do, q)[0] for q in range(0, len(do) - 3)
                              if struct.unpack_from('>I', do, q)[0] in objs and struct.unpack_from('>I', do, q)[0] != o][:64]
        return out

    def stream(self, sid):
        _, blk, size, start = self._loc(self.streams[sid])
        return self._read(start * blk, size)


def find_vgmstream(arg=None):
    # the one the port ships (Mod Maker Tools\vgmstream, beside these tools) first, then the player's own
    cands = [arg, os.path.join(HERE, 'vgmstream', 'vgmstream-cli.exe')]
    p = HERE
    for _ in range(5):
        cands.append(os.path.join(p, 'Mod Maker Tools', 'vgmstream', 'vgmstream-cli.exe'))
        cands.append(os.path.join(p, 'third_party', 'vgmstream', 'vgmstream-cli.exe'))
        p = os.path.dirname(p)
    cands += [os.environ.get('SVR2011_VGMSTREAM'), os.path.join(HERE, 'vgmstream-cli.exe'), shutil.which('vgmstream-cli')]
    for c in cands:
        if c and os.path.exists(c):
            return c
    return None


def theme(pck, events, out_mp3, work, vgmstream=None, lufs=-24.0, stream_pck=None):
    """the first of `events` with a stream -> mp3 at `lufs`. -> (event, note) or (None, why)"""
    import svr10_superstar as S
    pk = pck if isinstance(pck, Pck) else Pck(pck)
    # (WWE 2K14: the events in one package, Game_Sound_E_S.pck, their streams in another, Game_Sound_M.pck)
    sp = pk if stream_pck is None else stream_pck if isinstance(stream_pck, Pck) else Pck(stream_pck)
    for ev in events:
        sids = pk.event_streams(ev, sp.streams)
        if not sids:
            continue
        vg = find_vgmstream(vgmstream)
        if not vg:
            return None, '%s found, but no vgmstream-cli to decode Wwise Vorbis (opt in: --vgmstream)' % ev
        wem = os.path.join(work, 'theme.wem')
        wav = os.path.join(work, 'theme.wav')
        open(wem, 'wb').write(sp.stream(sids[0]))
        r = subprocess.run([vg, '-o', wav, wem], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if r.returncode or not os.path.exists(wav):
            return None, 'vgmstream could not decode %s' % ev
        src = S.loudness(wav)
        subprocess.run([S.ffmpeg(), '-hide_banner', '-loglevel', 'error', '-y', '-i', wav, '-af',
                        'volume=%.2fdB' % (lufs - src), '-c:a', 'libmp3lame', '-b:a', '256k', out_mp3], check=True,
                       stdout=subprocess.DEVNULL)
        for f in (wem, wav):
            try:
                os.remove(f)
            except OSError:
                pass
        return ev, 'stream %d' % sids[0]
    return None, 'no event %s' % ' / '.join(events[:3])


def main(argv):
    if len(argv) >= 4 and argv[1] == 'events':
        pk = Pck(argv[2])
        print(json.dumps({n: pk.event_streams(n) for n in argv[3:]}))
        return 0
    if len(argv) >= 5 and argv[1] == 'theme':
        vg = argv[argv.index('--vgmstream') + 1] if '--vgmstream' in argv else None
        print(json.dumps(theme(argv[2], [argv[3]], argv[4], os.path.dirname(os.path.abspath(argv[4])), vg)))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
