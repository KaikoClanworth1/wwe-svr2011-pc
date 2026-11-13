"""Fills in each profile file's summary: how many sessions it holds, and its largest count.
The console writes the counts through libgcov's stream calls, which leave both as zero, and the compiler takes a
profile of zero runs for an empty one.
  python pgo_summary.py <profile folder> <sessions>
"""
import glob
import os
import struct
import sys

SUMMARY = 0xA1000000
ARCS = 0x01A10000


def fix(path, runs):
    data = bytearray(open(path, 'rb').read())
    if data[:4] != b'adcg':
        raise SystemExit('%s: not a little-endian .gcda file' % path)
    pos, summary, largest = 16, None, 0  # (magic, version, stamp, checksum)
    while pos + 8 <= len(data):
        tag, length = struct.unpack_from('<Ii', data, pos)
        pos += 8
        if tag == 0:
            break
        if length < 0:  # (a record whose counts are all zero carries none)
            continue
        if tag == SUMMARY:
            summary = pos
        elif tag == ARCS:
            counts = struct.unpack_from('<%dQ' % (length // 8), data, pos)
            if counts:
                largest = max(largest, max(counts))
        pos += length
    if summary is None:
        raise SystemExit('%s: no summary record' % path)
    struct.pack_into('<II', data, summary, runs, min(largest, 0xFFFFFFFF))
    open(path, 'wb').write(data)
    return largest


def main():
    folder, runs = sys.argv[1], int(sys.argv[2])
    files = glob.glob(os.path.join(folder, '*.gcda'))
    ran = sum(1 for f in files if fix(f, runs))
    print('pgo: summaries written, %d of %d objects ran' % (ran, len(files)))


if __name__ == '__main__':
    main()
