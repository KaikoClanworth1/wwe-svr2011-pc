"""Paint Tool save (.pt) tool for tests - the same format and checksums as the
launcher's Paint Tool tab (launcher/svr2011_launcher.c, pt_slot_sum).

    python pt_tool.py info  <pt>                 used slots and their ids
    python pt_tool.py clear <pt> <out> [slots]   empty those slots (default all)
    python pt_tool.py swap  <pt> <out> <a> <b>   exchange two slots

A slot's id (its checksum, file offset 8+k*S+0x604C8) is how a Created
Superstar refers to a Paint Tool logo.
"""
import struct
import sys

SLOTS, S = 20, 0x604CC
SIZE = 8 + SLOTS * S + 4
CANVAS, TGA, DDS, STAMP, SUM = 52, 0x40034, 0x50448, 0x604C8, 0x604D0
EMPTY_HEADER = bytes([0, 0, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0]) + bytes(28)


def slot_sum(f, k):
    b = k * S
    s = sum(struct.unpack_from(">8b", f, b + 20))
    s += sum(struct.unpack_from(">I", f, b + o)[0] for o in (8, 12, 16, 28, 32, 36, 40, 44, 48))
    s += sum(struct.unpack_from(">65536I", f, b + CANVAS))
    t = b + TGA
    s += sum(f[t + i] for i in (0, 1, 2, 7, 16, 17))
    s += sum(struct.unpack_from(">H", f, t + i)[0] for i in (3, 5, 8, 10, 12, 14, 18))
    s += sum(struct.unpack_from(">256I", f, t + 20))
    s += sum(f[t + 0x414:t + 0x414 + 65536])
    d = b + DDS
    s += sum(struct.unpack_from(">32I", f, d))
    s += sum(struct.unpack_from(">" + "HHIHHI" * 2 * 2048, f, d + 128))
    e = b + STAMP
    s += struct.unpack_from(">H", f, e)[0] + sum(f[e + 2:e + 8])
    return s & 0xFFFFFFFF


def fix_sums(f):
    total = sum(struct.unpack_from(">2I", f, 0))
    for k in range(SLOTS):
        v = slot_sum(f, k)
        struct.pack_into(">I", f, k * S + SUM, v)
        total += v
    struct.pack_into(">I", f, SIZE - 4, total & 0xFFFFFFFF)


def used(f, k):
    return struct.unpack_from(">I", f, k * S + 36)[0] != 0


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    f = bytearray(open(path, "rb").read())
    assert len(f) == SIZE and struct.unpack_from(">2I", f, 0) == (0x02A78E9A, 3), "not a .pt"
    if cmd == "info":
        for k in range(SLOTS):
            if used(f, k):
                w = struct.unpack_from(">I", f, k * S + 44)[0]
                print(f"slot {k + 1:2}: {w}px id {struct.unpack_from('>I', f, k * S + SUM)[0]:08X}")
        return
    out = sys.argv[3]
    if cmd == "clear":
        ks = [int(a) - 1 for a in sys.argv[4:]] or range(SLOTS)
        for k in ks:
            f[k * S + 8:k * S + 8 + S] = bytes(S)
            f[k * S + 8:k * S + 8 + len(EMPTY_HEADER)] = EMPTY_HEADER
    elif cmd == "swap":
        a, b = int(sys.argv[4]) - 1, int(sys.argv[5]) - 1
        sa, sb = bytes(f[a * S + 8:a * S + 8 + S]), bytes(f[b * S + 8:b * S + 8 + S])
        f[a * S + 8:a * S + 8 + S], f[b * S + 8:b * S + 8 + S] = sb, sa
    fix_sums(f)
    open(out, "wb").write(f)


if __name__ == "__main__":
    main()
