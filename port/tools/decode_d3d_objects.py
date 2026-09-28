"""Decode the D3D objects dumped by the census (runs/d3dtrace/objects.csv).

Layouts (big-endian, guest memory), confirmed on this game's objects:
  all      +0x00 Common: low bits = resource type (1 VB, 2 IB, 3 texture,
                  5 vertex declaration), +0x04 ReferenceCount, +0x08 Fence,
                  +0x0C ReadFence, +0x10 Identifier, +0x14 BaseFlush
  texture  +0x18 MipFlush, +0x1C GPU texture fetch constant (6 dwords)
  VB       +0x18 GPU vertex fetch constant (2 dwords)
  IB       +0x18 GPU address, +0x1C size in bytes
  decl     +0x18 element count, +0x34 D3DVERTEXELEMENT[count] (12 bytes:
                  u16 stream, u16 offset, u32 type, u8 method, u8 usage,
                  u8 usage index, u8 pad)

    python decode_d3d_objects.py [objects.csv]
"""
import collections
import csv
import os
import struct
import sys

TEXTURE_FORMATS = [
    "1_REVERSE", "1", "8", "1_5_5_5", "5_6_5", "6_5_5", "8_8_8_8", "2_10_10_10", "8_A", "8_B",
    "8_8", "Cr_Y1_Cb_Y0_REP", "Y1_Cr_Y0_Cb_REP", "16_16_EDRAM", "8_8_8_8_A", "4_4_4_4",
    "10_11_11", "11_11_10", "DXT1", "DXT2_3", "DXT4_5", "16_16_16_16_EDRAM", "24_8",
    "24_8_FLOAT", "16", "16_16", "16_16_16_16", "16_EXPAND", "16_16_EXPAND",
    "16_16_16_16_EXPAND", "16_FLOAT", "16_16_FLOAT", "16_16_16_16_FLOAT", "32", "32_32",
    "32_32_32_32", "32_FLOAT", "32_32_FLOAT", "32_32_32_32_FLOAT", "32_AS_8", "32_AS_8_8",
    "16_MPEG", "16_16_MPEG", "8_INTERLACED", "32_AS_8_INTERLACED", "32_AS_8_8_INTERLACED",
    "16_INTERLACED", "16_MPEG_INTERLACED", "16_16_MPEG_INTERLACED", "DXN",
    "8_8_8_8_AS_16_16_16_16", "DXT1_AS_16_16_16_16", "DXT2_3_AS_16_16_16_16",
    "DXT4_5_AS_16_16_16_16", "2_10_10_10_AS_16_16_16_16", "10_11_11_AS_16_16_16_16",
    "11_11_10_AS_16_16_16_16", "32_32_32_FLOAT", "DXT3A", "DXT5A", "CTX1",
    "DXT3A_AS_1_1_1_1", "8_8_8_8_GAMMA_EDRAM", "2_10_10_10_FLOAT_EDRAM"]
ENDIAN = ["none", "8in16", "8in32", "16in32"]
DIMENSIONS = ["1D", "2D", "3D", "Cube"]
USAGES = ["POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
          "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"]


def words(hexstr):
    b = bytes.fromhex(hexstr)
    return struct.unpack(">%dI" % (len(b) // 4), b)


def texture(w):
    f = w[7:13]
    dim = (f[5] >> 9) & 3
    size = f[2]
    if dim == 1:
        extent = ((size & 0x1FFF) + 1, ((size >> 13) & 0x1FFF) + 1, 1)
    elif dim == 2 or dim == 3:
        extent = ((size & 0x7FF) + 1, ((size >> 11) & 0x7FF) + 1, ((size >> 22) & 0x3FF) + 1)
    else:
        extent = ((size & 0xFFFFFF) + 1, 1, 1)
    return {
        "format": TEXTURE_FORMATS[f[1] & 0x3F],
        "endian": ENDIAN[(f[1] >> 6) & 3],
        "address": f[1] & 0xFFFFF000,
        "tiled": bool(f[0] >> 31),
        "pitch": ((f[0] >> 22) & 0x1FF) * 32,
        "dimension": DIMENSIONS[dim],
        "size": extent,
        "mips": (((f[4] >> 2) & 0xF), ((f[4] >> 6) & 0xF)),
        "mip_address": f[5] & 0xFFFFF000,
    }


def vertex_buffer(w):
    return {"address": w[6] & 0xFFFFFFFC, "bytes": ((w[7] >> 2) & 0xFFFFFF) * 4,
            "endian": ENDIAN[w[7] & 3]}


def declaration(w):
    n = w[6]
    elems = []
    b = struct.pack(">%dI" % len(w), *w)
    for i in range(min(n, 16)):
        o = 0x34 + 12 * i
        if o + 12 > len(b):
            break
        stream, offset, typ, method, usage, index = struct.unpack_from(">HHIBBB", b, o)
        elems.append((stream, offset, TEXTURE_FORMATS[typ & 0x3F] if typ != 0xFFFFFFFF else "?",
                      USAGES[usage] if usage < len(USAGES) else usage, index))
    return elems


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(__file__), "..", "runs", "d3dtrace", "objects.csv")
    rows = list(csv.reader(open(path)))
    by = collections.defaultdict(list)
    for fn, reg, ptr, args, hexstr in rows:
        by[fn].append((int(ptr, 16), words(hexstr)))

    tex = [texture(w) for _, w in by["82917EC8"] if (w[7] & 3) == 2]
    print(f"textures: {len(tex)} distinct objects bound by SetTexture")
    for k in ("format", "dimension", "endian", "tiled"):
        print(f"  {k:9s}", dict(collections.Counter(t[k] for t in tex).most_common()))
    sizes = collections.Counter(f"{t['size'][0]}x{t['size'][1]}" for t in tex)
    print("  sizes    ", dict(sizes.most_common(12)))

    vbs = [vertex_buffer(w) for _, w in by["8291DD70"] if (w[6] & 3) == 3]
    print(f"vertex buffers: {len(vbs)}; endian {dict(collections.Counter(v['endian'] for v in vbs))}; "
          f"sizes {min(v['bytes'] for v in vbs)}..{max(v['bytes'] for v in vbs)} bytes")
    ibs = [(w[0], w[6], w[7]) for _, w in by["8291DE90"]]
    common = collections.Counter(f"{c:08X}" for c, _, _ in ibs)
    print(f"index buffers: {len(ibs)}; Common words {dict(common)}; "
          f"sizes {min(s for *_, s in ibs)}..{max(s for *_, s in ibs)} bytes")

    print(f"vertex declarations: {len(by['82920F78'])}")
    for ptr, w in by["82920F78"]:
        print(f"  {ptr:08X}:", ", ".join(f"s{s}+{o} {t} {u}{i}" for s, o, t, u, i in declaration(w)))


if __name__ == "__main__":
    main()
