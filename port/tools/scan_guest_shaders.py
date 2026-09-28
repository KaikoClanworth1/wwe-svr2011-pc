"""Extract XDK shader containers from the running game's guest memory.

The recompiled game maps the Xbox 360 address space at host 0x1_0000_0000
(virtual) / 0x2_0000_0000 (physical). Shader containers (flags 0x102A11xx,
the format XenosRecomp reads) are decompressed there by the game before it
creates its shaders, so scanning the live process finds all loaded ones.

    python scan_guest_shaders.py <pid> <out_dir>
"""
import ctypes
import ctypes.wintypes as wt
import hashlib
import os
import struct
import sys

PROCESS_VM_READ = 0x10
PROCESS_QUERY_INFORMATION = 0x400
MEM_COMMIT = 0x1000
PAGE_NOACCESS = 0x01
PAGE_GUARD = 0x100

GUEST_BASE = 0x1_0000_0000
GUEST_SIZE = 0x1_0000_0000


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD),
                ("RegionSize", ctypes.c_uint64), ("State", wt.DWORD),
                ("Protect", wt.DWORD), ("Type", wt.DWORD)]


def regions(h):
    k32 = ctypes.windll.kernel32
    addr = GUEST_BASE
    mbi = MBI()
    while addr < GUEST_BASE + GUEST_SIZE:
        if not k32.VirtualQueryEx(h, ctypes.c_uint64(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
            break
        size = mbi.RegionSize
        if (mbi.State == MEM_COMMIT and not (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))):
            yield mbi.BaseAddress, size
        addr = mbi.BaseAddress + size


def main():
    pid, out = int(sys.argv[1]), sys.argv[2]
    os.makedirs(out, exist_ok=True)
    k32 = ctypes.windll.kernel32
    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        raise SystemExit("OpenProcess failed")
    seen, counts = set(), {"vs": 0, "ps": 0}
    scanned = 0
    for base, size in regions(h):
        for off in range(0, size, 64 << 20):
            n = min(64 << 20, size - off)
            buf = ctypes.create_string_buffer(n)
            got = ctypes.c_size_t()
            if not k32.ReadProcessMemory(h, ctypes.c_uint64(base + off), buf, n, ctypes.byref(got)):
                continue
            data = buf.raw[:got.value]
            scanned += len(data)
            i = data.find(b"\x10\x2A\x11")
            while i != -1:
                if i % 4 == 0 and i + 0x24 <= len(data):
                    flags, vsize, psize = struct.unpack_from(">III", data, i)
                    f1c, f20 = struct.unpack_from(">II", data, i + 0x1C)
                    total = vsize + psize
                    if f1c == 0 and f20 == 0 and 0x24 < total < 0x100000 and i + total <= len(data):
                        blob = data[i:i + total]
                        digest = hashlib.sha1(blob).hexdigest()[:16]
                        if digest not in seen:
                            seen.add(digest)
                            kind = "vs" if flags & 1 else "ps"  # bit 0 set = vertex shader
                            counts[kind] += 1
                            guest = base + off + i - GUEST_BASE
                            open(os.path.join(out, f"{digest}.{kind}.xsc"), "wb").write(blob)
                i = data.find(b"\x10\x2A\x11", i + 1)
    print(f"scanned {scanned >> 20} MB: {len(seen)} unique containers "
          f"({counts['vs']} vertex, {counts['ps']} pixel) -> {out}")


if __name__ == "__main__":
    main()
