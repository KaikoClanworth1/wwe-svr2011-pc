"""Extract every file from an Xbox 360 (or original Xbox) disc image.

The game partition is an XDVDFS file system; its volume descriptor sits at
sector 32 of the partition, whose offset depends on the disc format.

    python extract_iso.py <image.iso> <out_dir>
"""
import os
import struct
import sys
import time

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Partition offsets: plain XISO, XGD1 (original Xbox), XGD2, XGD3.
OFFSETS = (0, 0x18300000, 0xFD90000, 0x2080000)
ATTR_DIR = 0x10


def find_partition(f):
    for off in OFFSETS:
        f.seek(off + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return off
    raise SystemExit("no XDVDFS volume found - not an Xbox disc image?")


def walk(f, part, sector, size, rel, out):
    """Yield (relative path, start sector, size, is_dir) for one directory table.

    The table is a binary tree; walking every entry in table order (rather than
    following the left/right links) is simpler and visits each exactly once.
    """
    if size == 0:
        return
    f.seek(part + sector * SECTOR)
    table = f.read(size)
    pos = 0
    while pos + 14 <= len(table):
        left = struct.unpack_from("<H", table, pos)[0]
        if left == 0xFFFF:  # padding up to the next sector
            pos = (pos // SECTOR + 1) * SECTOR
            continue
        _, _, start, length, attr, nlen = struct.unpack_from("<HHIIBB", table, pos)
        name = table[pos + 14 : pos + 14 + nlen].decode("latin-1")
        path = os.path.join(rel, name)
        is_dir = bool(attr & ATTR_DIR)
        yield path, start, length, is_dir
        if is_dir:
            yield from walk(f, part, start, length, path, out)
        pos = (pos + 14 + nlen + 3) & ~3


def main():
    image, out = sys.argv[1], sys.argv[2]
    with open(image, "rb") as f:
        part = find_partition(f)
        f.seek(part + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        entries = list(walk(f, part, root_sector, root_size, "", out))
        total = sum(e[2] for e in entries if not e[3])
        print(f"partition 0x{part:X}: {len(entries)} entries, {total / 2**30:.2f} GiB", flush=True)
        done, t0, last = 0, time.time(), 0
        for path, start, length, is_dir in entries:
            dst = os.path.join(out, path)
            if is_dir:
                os.makedirs(dst, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(dst) or out, exist_ok=True)
            if os.path.exists(dst) and os.path.getsize(dst) == length:
                done += length  # resume support
                continue
            f.seek(part + start * SECTOR)
            with open(dst + ".part", "wb") as o:
                left = length
                while left:
                    chunk = f.read(min(left, 8 << 20))
                    o.write(chunk)
                    left -= len(chunk)
                    done += len(chunk)
            os.replace(dst + ".part", dst)
            if time.time() - last > 5:
                last = time.time()
                print(f"{100 * done / total:5.1f}%  {path}", flush=True)
        print(f"done: {done / 2**30:.2f} GiB in {time.time() - t0:.0f}s", flush=True)


if __name__ == "__main__":
    main()
