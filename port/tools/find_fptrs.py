"""Find functions the codegen missed because they are only reached by pointer.

Scans the loaded image (dump it with SVR2011_DUMP_IMAGE=<file>) for 32-bit
big-endian words outside the code section that point into it, and for
addresses built in code with lis + addi/ori, and reports
targets that are neither known functions nor labels inside one, and that
start right after a terminating instruction (blr / b / bctr / padding) -
i.e. look like a function start.

    python find_fptrs.py <image.bin> <generated/default> [--toml]
"""
import glob
import os
import re
import struct
import sys

IMAGE_BASE = 0x82000000
CODE_BASE = 0x82150000
CODE_SIZE = 0xC07888


def is_terminator(w):
    if w in (0x4E800020, 0x4E800420, 0x00000000):   # blr, bctr, padding
        return True
    return (w >> 26) == 18 and (w & 3) == 0            # b (no link, relative)


def looks_like_code(w):
    # Rules out padding and inline switch jump tables (which follow a bctr
    # and are themselves lists of code addresses).
    return w != 0 and not (CODE_BASE <= w < CODE_BASE + CODE_SIZE)


def main():
    image_path, gen_dir = sys.argv[1], sys.argv[2]
    as_toml = "--toml" in sys.argv
    img = open(image_path, "rb").read()
    code_lo, code_hi = CODE_BASE, CODE_BASE + CODE_SIZE

    funcs, labels = set(), set()
    for fn in glob.glob(os.path.join(gen_dir, "*.cpp")):
        text = open(fn).read()
        funcs.update(int(m, 16) for m in re.findall(r"DEFINE_REX_FUNC\(sub_([0-9A-F]{8})\)", text))
        labels.update(int(m, 16) for m in re.findall(r"^loc_([0-9A-F]{8}):", text, re.M))
    # Named functions (rexcrt_, __savegpr, ...) are listed in the mapping table.
    for fn in glob.glob(os.path.join(gen_dir, "*.cpp")):
        for m in re.finditer(r"\{\s*0x([0-9A-Fa-f]{8}),\s*[A-Za-z_]", open(fn).read()):
            funcs.add(int(m.group(1), 16))

    def word(addr):
        off = addr - IMAGE_BASE
        return struct.unpack_from(">I", img, off)[0]

    found = {}
    n = len(img) // 4
    for i in range(n):
        addr = IMAGE_BASE + i * 4
        if code_lo <= addr < code_hi:
            continue
        v = struct.unpack_from(">I", img, i * 4)[0]
        if not (code_lo <= v < code_hi) or v & 3:
            continue
        if v in funcs or v in labels:
            continue
        if not is_terminator(word(v - 4)):
            continue
        if not looks_like_code(word(v)):
            continue
        found.setdefault(v, []).append(addr)

    # Pointers built in code: lis rD,hi ... addi rX,rD,lo / ori rX,rD,lo
    # within a few instructions.
    for a in range(code_lo, code_hi - 4, 4):
        x = word(a)
        if x >> 26 != 15 or (x >> 16) & 31 != 0:       # lis = addis rD,0,imm
            continue
        rd, hi = (x >> 21) & 31, x & 0xFFFF
        for b in range(a + 4, min(a + 48, code_hi), 4):
            y = word(b)
            op, ra = y >> 26, (y >> 16) & 31
            if op == 14 and ra == rd:                   # addi (signed low half)
                lo = y & 0xFFFF
                v = ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
            elif op == 24 and (y >> 21) & 31 == rd:     # ori rA,rS,imm (rS = rd)
                v = (hi << 16) | (y & 0xFFFF)
            else:
                continue
            if (code_lo <= v < code_hi and not v & 3 and v not in funcs
                    and v not in labels and is_terminator(word(v - 4)) and looks_like_code(word(v))):
                found.setdefault(v, []).append(a)
            break

    for v in sorted(found):
        refs = found[v]
        if as_toml:
            print(f"0x{v:08X} = {{}}  # {len(refs)} ref(s), e.g. 0x{refs[0]:08X}")
        else:
            print(f"{v:08X}  refs={len(refs)}  first=0x{refs[0]:08X}")
    print(f"# {len(found)} candidate function(s)", file=sys.stderr)


if __name__ == "__main__":
    main()
