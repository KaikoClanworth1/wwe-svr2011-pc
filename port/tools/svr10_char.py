"""SvR 2010 character model -> a ch.pac SvR 2011 can load (superstar mods).

    python svr10_char.py <2010 pac folder> <id> <2011 pac folder> <out ch.pac>

A 2010 chNNN.pac is the same EPK8 / EMD / PACH / JBOY as 2011's (skeleton,
shaders, vertex formats and textures all also used by shipped 2011 models).
Two children differ (scratchpad research svr10/re_model.md):

- 0x64, the face animation ("JUDE" PACH): 2010's older 9-child version is
  replaced by 2011's generic copy (103 of 2011's models share it; Yuke's gave
  Carlito's 2010 re-export the same), taken from 2011's ch104.
- 0xc8 / 0xc9, a portrait: 256x128 in 2010, 256x256 in 2011. The 2010 select
  bust (DLC_HD.pac SSFC/<id>, 256x256 DXT5, BPE packed) is used for both.

Every other child keeps its packed bytes, except that the texture bundles'
AO textures get 2011's bruise convention (svr10_bruise.py). The EMD entry names keep the 2010
id: the game renames them to the mod's slot (src/superstar_mods.cpp).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ch_tool  # noqa: E402
import svrfmt  # noqa: E402


def epk8_entries(path):
    """-> [(group type, name8, packed blob)] (empty names skipped)"""
    _, groups = ch_tool.epk8_read(open(path, 'rb').read())
    return [(t, n, b) for t, es in groups for n, b in es if n.strip(b'\0')]


def epk8_write(template_path, entries):
    """An EPK8 laid out as the template (header, group table) with the given
    entries' blobs. entries: [(group type, name8, blob)] in the template's order."""
    t = open(template_path, 'rb').read()
    header = bytearray(t[:0x800])
    index = bytearray(t[0x800:0x4000])
    body = bytearray()
    blobs = {(g, n): b for g, n, b in entries}
    p = 0
    while p < len(index) and index[p:p + 4] != b'\0\0\0\0':
        typ = bytes(index[p:p + 4])
        cnt = struct.unpack_from('<H', index, p + 4)[0]
        p += 12
        for _ in range(cnt):
            name = bytes(index[p:p + 8])
            if name.strip(b'\0'):
                blob = blobs[(typ, name)]
                while len(body) % 0x800:
                    body.append(0)
                struct.pack_into('<II', index, p + 8, len(body) // 0x800, (len(blob) + 0xFF) // 0x100)
                body += blob
            p += 16
    while len(body) % 0x800:
        body.append(0)
    struct.pack_into('<I', header, 8, len(body))
    return bytes(header) + bytes(index) + bytes(body) + bytes(0x800)


def dlc_render(pac_dir, group, cid):
    """The packed SSFx render of a superstar from DLC_HD.pac (EPAC)."""
    _, groups, _ = svrfmt.epac_read(open(os.path.join(pac_dir, 'DLC_HD.pac'), 'rb').read())
    name = ('%04d' % cid).encode()
    for typ, es in groups:
        if typ == group.encode():
            for n, b in es:
                if n == name:
                    return b
    raise SystemExit('no %s/%04d in DLC_HD.pac' % (group, cid))


def generic_face(pac11):
    for t, n, b in epk8_entries(os.path.join(pac11, 'ch', 'ch104.pac')):
        if t == b'EMD ':
            return dict(svrfmt.pach_read(ch_tool.unpack(b)))[0x64]
    raise SystemExit('no 2011 face animation (ch104)')


# The bones props are held by (title belts in championship entrances on the
# shoulder, the hands, the waist) and the body bones above them: every shipped
# SvR 2010 model has the same rest rotations there, within 0.1 rad (85 checked;
# the forearms, kote, come in two shapes and are left out). A model edited
# elsewhere (a player's SvR 2010 Chris Benoit) can differ, and a prop then sits
# rotated - the belt held vertical. Not changed here (the mesh is bound to
# those bones): reported, so the model's maker can line them up.
PROP_BONES = ['root', 'koshi', 'mune', 'onaka', 'l_sakotsu', 'r_sakotsu', 'l_ninoude', 'r_ninoude',
              'l_te', 'r_te', 'l_kata_m', 'r_kata_m', 'l_sakotsu_m', 'r_sakotsu_m', 'kubi']


def skeleton(blob):
    """the largest JBOY skeleton (nodes) in an EMD blob, or None"""
    import jboy
    best, stack = None, [ch_tool.unpack(blob)]
    while stack:
        d = stack.pop()
        if d[:4] == b'PACH':
            stack += [ch_tool.unpack(c) for i, c in svrfmt.pach_read(d)]
        elif d[:4] == b'JBOY':
            try:
                m = jboy.read(d)
            except Exception:
                continue
            if len(m.nodes) > 40 and (best is None or len(m.nodes) > len(best.nodes)):
                best = m
    return best


def prop_bone_check(src, reference, tol=0.2):
    """-> [(bone, model rotation, reference rotation)] where the model's rest rotation differs"""
    ref = {}
    for typ, name, blob in epk8_entries(reference):
        m = skeleton(blob) if typ == b'EMD ' else None
        if m:
            ref = {n['name']: n['r'] for n in m.nodes}
            break
    out = []
    for typ, name, blob in epk8_entries(src):
        m = skeleton(blob) if typ == b'EMD ' else None
        if not m:
            continue
        for n in m.nodes:
            if n['name'] in PROP_BONES and n['name'] in ref and                     any(abs(a - b) > tol for a, b in zip(n['r'], ref[n['name']])):
                out.append((n['name'], tuple(round(x, 2) for x in n['r']), tuple(round(x, 2) for x in ref[n['name']])))
        break
    return out


def convert(pac10, cid, pac11, out):
    src = os.path.join(pac10, 'ch', 'ch%d.pac' % cid)
    reference = os.path.join(pac10, 'ch', 'ch105.pac')  # (a shipped 2010 model: the standard skeleton)
    if os.path.exists(reference) and os.path.abspath(reference) != os.path.abspath(src):
        for bone, r, want in prop_bone_check(src, reference):
            print('WARNING: %s: bone %s has rest rotation %s, shipped models %s - props it holds (a title '
                  'belt) will sit rotated in the game' % (os.path.basename(src), bone, r, want))
    face = generic_face(pac11)
    bust = dlc_render(pac10, 'SSFC', cid)
    entries = []
    for typ, name, blob in epk8_entries(src):
        if typ == b'EMD ' and ch_tool.unpack(blob)[:4] == b'PACH':
            kids = svrfmt.pach_read(ch_tool.unpack(blob))
            new = []
            for i, data in kids:
                if i == 0x64:
                    data = face
                elif i in (0xc8, 0xc9):
                    data = bust
                new.append((i, data))
            blob = svrfmt.pach_write(new)
        entries.append((typ, name, blob))
    open(out, 'wb').write(epk8_write(src, entries))
    import svr10_bruise  # (2010 AO textures: the bruise mask moves to R, as 2011's Mk)
    bruise = svr10_bruise.fix_ch_pac(out, out)
    print('%s: %d entries -> %s (%d bytes; %d AO textures converted)' % (os.path.basename(src), len(entries), out,
                                                                        os.path.getsize(out), len(bruise)))


if __name__ == '__main__':
    if len(sys.argv) != 5:
        raise SystemExit(__doc__)
    convert(sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4])
