"""Find Xbox 360 XDK shader containers (as XenosRecomp reads them) in files.

A container starts with a big-endian flags word 0x102A11xx, followed by
virtual and physical sizes; XenosRecomp also requires the words at 0x1C and
0x20 to be zero. Reports per file: count, and a few offsets.

    python find_shaders.py <file-or-dir> [...] [--extract <out_dir>]
"""
import hashlib
import os
import struct
import sys


def scan(data):
    hits = []
    i = data.find(b"\x10\x2A\x11")
    n = len(data)
    while i != -1:
        if i + 0x24 <= n and i % 4 == 0:
            flags, vsize, psize = struct.unpack_from(">III", data, i)
            f1c, f20 = struct.unpack_from(">II", data, i + 0x1C)
            size = vsize + psize
            if f1c == 0 and f20 == 0 and 0x24 < size <= n - i and size < 0x100000:
                hits.append((i, size, flags))
        i = data.find(b"\x10\x2A\x11", i + 1)
    return hits


def main():
    args = sys.argv[1:]
    out = None
    if "--extract" in args:
        k = args.index("--extract")
        out = args[k + 1]
        del args[k:k + 2]
        os.makedirs(out, exist_ok=True)
    files = []
    for a in args:
        if os.path.isdir(a):
            for root, _, names in os.walk(a):
                files += [os.path.join(root, n) for n in names]
        else:
            files.append(a)
    total, unique = 0, set()
    for path in sorted(files):
        data = open(path, "rb").read()
        hits = scan(data)
        if not hits:
            continue
        total += len(hits)
        for off, size, flags in hits:
            blob = data[off:off + size]
            h = hashlib.sha1(blob).hexdigest()[:16]
            if h not in unique and out:
                kind = "vs" if flags & 1 else "ps"  # bit 0 set = vertex shader
                open(os.path.join(out, f"{h}.{kind}.xsc"), "wb").write(blob)
            unique.add(h)
        print(f"{len(hits):6d}  {path}  (first at 0x{hits[0][0]:X})")
    print(f"total {total} containers, {len(unique)} unique")


if __name__ == "__main__":
    main()
