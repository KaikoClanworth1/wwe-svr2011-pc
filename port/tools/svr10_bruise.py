"""SvR 2010 skin occlusion textures -> SvR 2011's bruise convention.

    python svr10_bruise.py <ch.pac> <out ch.pac>
    python svr10_bruise.py --svrmod <mod.svrmod> [...]   (in place, version +0.1)

2011's skin shaders (yBumpMapSS, g_fBruiseLev / g_f4BruiseCol) darken the
skin by BruiseLev * occlusion.R * BruiseCol: 2011 models bind "Mk" (R = a
small bruise-spot mask). 2010 models bind "AO" there, a grey ambient
occlusion map (R ~0.7-0.9 everywhere) with the bruise mask in A - so as
damage builds, a 2010 body's whole bruise mesh (neck, upper chest,
shoulders) is tinted: a hard-edged "tank top" (2.0.3 report, Carlito).
Yuke's own 2011 re-exports moved the mask to R (Mk.R ~ AO.A, r 0.998).

Each AO / BloodHand_ao_* texture (DXT5) gets R = its A and A = 255 (G, B
kept: the vertex colour factor and the sweat mask), the same size and mips,
so it is rewritten in place in its bundle.
"""
import io
import os
import re
import shutil
import struct
import sys
import tempfile
import zipfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ch_tool  # noqa: E402
import svr10_char  # noqa: E402
import svrfmt  # noqa: E402


def is_ao(name):
    n = name.lower()
    return n == 'ao' or n.startswith('bloodhand_ao')


def ao_to_mk(dds):
    """A DXT5 DDS (128-byte header): R = A, A = 255, every mip re-encoded."""
    h, w = struct.unpack_from('<II', dds, 12)
    mips = max(1, struct.unpack_from('<I', dds, 28)[0])
    a = np.asarray(Image.open(io.BytesIO(dds)).convert('RGBA')).copy()
    a[:, :, 0] = a[:, :, 3]
    a[:, :, 3] = 255
    base = Image.fromarray(a, 'RGBA')
    out = bytearray(dds[:128])
    lw, lh = w, h
    for m in range(mips):
        im = base if m == 0 else base.resize((max(1, lw), max(1, lh)), Image.BOX)
        b = io.BytesIO()
        im.save(b, 'DDS', pixel_format='DXT5')
        out += b.getvalue()[128:]
        lw //= 2
        lh //= 2
    if len(out) != len(dds):
        raise ValueError('re-encoded size %d != %d' % (len(out), len(dds)))
    return bytes(out)


def fix_bundle(raw):
    """A texture bundle (ch_tool.is_bundle): its AO textures converted. -> (raw, names)"""
    raw = bytearray(raw)
    done = []
    for k, name in enumerate(ch_tool.bundle_names(raw)):
        if not is_ao(name):
            continue
        size, off = struct.unpack_from('<II', raw, 16 + 32 * k + 20)
        dds = bytes(raw[off:off + size])
        if dds[:4] != b'DDS ' or dds[84:88] != b'DXT5':
            continue
        raw[off:off + size] = ao_to_mk(dds)
        done.append(name)
    return bytes(raw), done


def fix_ch_pac(src, out):
    """Every EMD entry's texture bundles. -> textures converted"""
    entries, total = [], []
    template = open(src, 'rb').read()  # (src may be out)
    for typ, name, blob in svr10_char.epk8_entries(src):
        if typ == b'EMD ' and ch_tool.unpack(blob)[:4] == b'PACH':
            kids, changed = [], False
            for i, data in svrfmt.pach_read(ch_tool.unpack(blob)):
                raw = ch_tool.unpack(data)
                if ch_tool.is_bundle(raw):
                    new, done = fix_bundle(raw)
                    if done:
                        data = svrfmt.bpe_pack(new) if data[:4] == b'BPE ' else new
                        changed = True
                        total += ['%s/%x:%s' % (name.strip(b'\0').decode('latin1'), i, n) for n in done]
                kids.append((i, data))
            if changed:
                blob = svrfmt.pach_write(kids)
        entries.append((typ, name, blob))
    with tempfile.TemporaryDirectory() as d:
        t = os.path.join(d, 'template.pac')
        open(t, 'wb').write(template)
        data = svr10_char.epk8_write(t, entries)
    open(out, 'wb').write(data)
    return total


def fix_svrmod(path):
    """ch.pac fixed and the manifest's version +0.1, in place. -> textures converted"""
    with tempfile.TemporaryDirectory() as d:
        zin = zipfile.ZipFile(path)
        src = os.path.join(d, 'ch_in.pac')
        open(src, 'wb').write(zin.read('ch.pac'))
        dst = os.path.join(d, 'ch_out.pac')
        done = fix_ch_pac(src, dst)
        if not done:
            return done
        tmp = path + '.tmp'
        with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as zout:
            for info in zin.infolist():
                data = zin.read(info.filename)
                if info.filename == 'ch.pac':
                    data = open(dst, 'rb').read()
                elif info.filename == 'manifest.txt':
                    t = data.decode('utf-8')
                    t = re.sub(r'^version=(\d+)\.(\d+)', lambda m: 'version=%s.%d' % (m.group(1), int(m.group(2)) + 1),
                               t, flags=re.M)
                    data = t.encode('utf-8')
                zout.writestr(info, data)
        zin.close()
        shutil.move(tmp, path)
    return done


if __name__ == '__main__':
    if len(sys.argv) >= 3 and sys.argv[1] == '--svrmod':
        for p in sys.argv[2:]:
            done = fix_svrmod(p)
            print(os.path.basename(p), len(done), 'textures' if done else '(nothing to do)', ' '.join(done))
    elif len(sys.argv) == 3:
        done = fix_ch_pac(sys.argv[1], sys.argv[2])
        print(len(done), 'textures:', ' '.join(done))
    else:
        raise SystemExit(__doc__)
