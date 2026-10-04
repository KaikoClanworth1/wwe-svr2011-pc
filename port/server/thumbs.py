"""The pictures the game keeps with a Community Creations upload (its record's
ThumbData0.. fields, what the game shows when browsing), as PNG images for the
dashboard's View button. Standard library only.

What the game writes (seen in its uploads):
  - a Created Superstar: a 128 x 128 portrait, DXT5, in the Xbox 360's byte
    order (16-bit words swapped) - 17408 bytes (the last 1024 unused here);
  - a Paint Tool logo: a DDS file, 64 x 64 DXT5 (4224 bytes);
  - a screenshot: 128 x 128 DXT1, Xbox byte order (8192 bytes);
  - finishers and the like: none (a 4-byte placeholder).
"""
import struct
import zlib


def _swap16(b):
    out = bytearray(b)
    out[0::2], out[1::2] = b[1::2], b[0::2]
    return bytes(out)


def _rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def _colour_block(block, rgba, w, bx, by, dxt1):
    c0, c1, bits = struct.unpack_from("<HHI", block, 0)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    if c0 > c1 or not dxt1:
        p2 = tuple((2 * a + b) // 3 for a, b in zip(p0, p1))
        p3 = tuple((a + 2 * b) // 3 for a, b in zip(p0, p1))
        palette = [p0 + (255,), p1 + (255,), p2 + (255,), p3 + (255,)]
    else:
        p2 = tuple((a + b) // 2 for a, b in zip(p0, p1))
        palette = [p0 + (255,), p1 + (255,), p2 + (255,), (0, 0, 0, 0)]
    for i in range(16):
        x, y = bx + (i & 3), by + (i >> 2)
        o = (y * w + x) * 4
        rgba[o:o + 4] = bytes(palette[(bits >> (2 * i)) & 3])


def _alpha_block(block, rgba, w, bx, by):
    a0, a1 = block[0], block[1]
    bits = int.from_bytes(block[2:8], "little")
    if a0 > a1:
        table = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
    else:
        table = [a0, a1] + [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]
    for i in range(16):
        x, y = bx + (i & 3), by + (i >> 2)
        rgba[(y * w + x) * 4 + 3] = table[(bits >> (3 * i)) & 7]


def decode_dxt(data, w, h, dxt1):
    """RGBA bytes of a w x h DXT1 / DXT5 image (blocks in rows)."""
    rgba = bytearray(w * h * 4)
    size = 8 if dxt1 else 16
    i = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            block = data[i:i + size]
            i += size
            if len(block) < size:
                return bytes(rgba)
            if dxt1:
                _colour_block(block, rgba, w, bx, by, True)
            else:
                _colour_block(block[8:], rgba, w, bx, by, False)
                _alpha_block(block, rgba, w, bx, by)
    return bytes(rgba)


def png(rgba, w, h):
    raw = b"".join(b"\0" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def thumb_png(blob):
    """The upload's picture as a PNG, or None (none, or a kind not known)."""
    if blob[:4] == b"DDS " and len(blob) >= 128:
        h, w = struct.unpack_from("<II", blob, 12)
        fourcc = blob[84:88]
        if fourcc in (b"DXT5", b"DXT1") and 0 < w <= 1024 and 0 < h <= 1024:
            return png(decode_dxt(blob[128:], w, h, fourcc == b"DXT1"), w, h)
        return None
    if len(blob) >= 16384 and len(blob) < 32768:  # a Superstar's portrait
        return png(decode_dxt(_swap16(blob[:16384]), 128, 128, False), 128, 128)
    if len(blob) == 8192:  # a screenshot
        return png(decode_dxt(_swap16(blob), 128, 128, True), 128, 128)
    return None
