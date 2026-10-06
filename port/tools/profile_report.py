"""Reads a profile from the game's sampling profiler (src/profiler.cpp,
SVR2011_PROFILE) and prints where each thread spends its time.

  python tools/profile_report.py <profile.txt> [--threads N] [--top N] [--callers]

Per thread: how many samples it ran or waited (a leaf in libc / the kernel's
wait calls counts as waiting), its busiest functions (leaf), and with
--callers the first function of our own code (libmain / librexruntime /
librexgpu) on the stack - for waits, who waited. Names come from
llvm-symbolizer and the build's unstripped libraries.
"""
import argparse
import bisect
import collections
import os
import subprocess
import sys
from pathlib import Path

PORT = Path(__file__).resolve().parents[1]
ROOT = PORT.parent
NDK = Path(os.environ.get("ANDROID_HOME", r"D:\Android\sdk")) / "ndk"
LIBS = {
    "libmain.so": PORT / "out" / "build" / "Android" / "libmain.so",
    "librexgpu-xenos.so": PORT / "out" / "build" / "Android" / "librexgpu-xenos.so",
    "librexruntime.so": ROOT / "recomp" / "rexglue-sdk" / "out" / "linux-arm64" / "librexruntime.so",
}
# The phone's own libraries (adb pull them into runs/tablet_libs): names for
# libc's waits and the GPU driver.
for _f in (PORT / "runs" / "tablet_libs").glob("*.so"):
    LIBS.setdefault(_f.name, _f)
OURS = ("libmain.so", "librexgpu-xenos.so", "librexruntime.so")
WAITS = ("futex", "nanosleep", "clock_nanosleep", "epoll", "__ioctl", "ioctl", "ppoll", "poll", "sched_yield",
         "__read", "read", "__rt_sigtimedwait", "sigsuspend", "pselect")


def symbolizer():
    for p in sorted(NDK.glob("*/toolchains/llvm/prebuilt/*/bin/llvm-symbolizer*")):
        return str(p)
    sys.exit("no llvm-symbolizer in the NDK")


def dynamic_symbols(lib):
    """Sorted (offset, name) of a library's exported functions (llvm-nm -D)."""
    nm = Path(symbolizer()).with_name("llvm-nm.exe" if symbolizer().endswith(".exe") else "llvm-nm")
    out = subprocess.run([str(nm), "-D", "--defined-only", str(lib)], capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in "tTwW":
            syms.append((int(f[0], 16), f[2]))
    return sorted(syms)


def symbolize(addresses):
    """{"lib+0xoff": name} for our libraries."""
    names = {}
    by_lib = collections.defaultdict(set)
    for a in addresses:
        lib, _, off = a.partition("+")
        if lib in LIBS and LIBS[lib].exists() and off.startswith("0x") and len(off) > 2:
            by_lib[lib].add(off)
    tool = symbolizer()
    for lib, offs in by_lib.items():
        offs = sorted(offs)
        # (return addresses: the call is the instruction before)
        query = "\n".join(hex(int(o, 16) - 4) if o != "0x0" else o for o in offs)
        out = subprocess.run([tool, "--obj", str(LIBS[lib]), "--functions=short", "--no-inlines", "--demangle"],
                             input=query, capture_output=True, text=True).stdout.split("\n\n")
        dyn, keys = None, []
        for off, block in zip(offs, out):
            fn = block.strip().split("\n")[0] if block.strip() else "?"
            if fn in ("??", "?"):
                # (stripped: the nearest exported function before it)
                if dyn is None:
                    dyn = dynamic_symbols(LIBS[lib])
                    keys = [a for a, _ in dyn]
                i = bisect.bisect_right(keys, int(off, 16)) - 1
                fn = f"{lib}:{dyn[i][1].split('@')[0]}" if i >= 0 else f"{lib}+{off}"
            names[f"{lib}+{off}"] = fn
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("profile")
    ap.add_argument("--threads", type=int, default=12)
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--callers", action="store_true")
    args = ap.parse_args()
    rows = []
    for line in open(args.profile, encoding="utf-8", errors="replace"):
        if line.startswith("#") or not line.strip():
            continue
        parts = line.rstrip("\n").split("\t")
        rows.append((parts[0], parts[1:]))
    names = symbolize({a for _, st in rows for a in st})

    def nm(a):
        return names.get(a, a)

    per_thread = collections.defaultdict(list)
    for t, st in rows:
        per_thread[t.split(":")[0] + ":" + t.split(":")[1]].append(st)
    total = len(rows)
    ranked = sorted(per_thread.items(), key=lambda kv: -sum(1 for st in kv[1] if not waiting(st, nm)))
    for t, stacks in ranked[: args.threads]:
        run = [st for st in stacks if not waiting(st, nm)]
        print(f"\n== {t}: {len(stacks)} samples, running {100 * len(run) / max(1, len(stacks)):.0f}%")
        leaf = collections.Counter(nm(st[0]) for st in run)
        for fn, n in leaf.most_common(args.top):
            print(f"   {100 * n / len(stacks):5.1f}%  {fn}")
        if args.callers:
            wait = [st for st in stacks if waiting(st, nm)]
            who = collections.Counter(first_ours(st, nm) for st in wait)
            for fn, n in who.most_common(args.top):
                print(f"   wait {100 * n / len(stacks):5.1f}%  in {fn}")
            own = collections.Counter(first_ours(st, nm) for st in run)
            for fn, n in own.most_common(args.top):
                print(f"   run  {100 * n / len(stacks):5.1f}%  under {fn}")
    print(f"\n{total} samples")


_svc_cache = {}


def after_svc(addr):
    """Whether the instruction before a sampled pc is an svc (the thread is in a
    kernel call - nearly always a wait). Reads the phone's library file."""
    lib, _, off = addr.partition("+")
    if lib not in LIBS or not off.startswith("0x") or len(off) <= 2:
        return False
    key = (lib, off)
    if key not in _svc_cache:
        import struct
        data = _lib_bytes(lib)
        va = int(off, 16) - 4
        # ELF64 program headers: map the address to a file offset
        phoff, = struct.unpack_from("<Q", data, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
        fo = None
        for i in range(phnum):
            ptype, _, pofs, pva, _, pfsz = struct.unpack_from("<IIQQQQ", data, phoff + i * phentsize)
            if ptype == 1 and pva <= va < pva + pfsz:
                fo = pofs + va - pva
        # (at the svc itself too: an interrupted, restartable call is rewound to it)
        _svc_cache[key] = fo is not None and 0xD4000001 in struct.unpack_from("<II", data, fo)
    return _svc_cache[key]


_lib_data = {}


def _lib_bytes(lib):
    if lib not in _lib_data:
        _lib_data[lib] = LIBS[lib].read_bytes()
    return _lib_data[lib]


def waiting(st, nm):
    leaf = nm(st[0])
    return after_svc(st[0]) or (st[0].startswith("libc.so") and any(w in leaf.split(":")[-1] for w in WAITS))


def first_ours(st, nm):
    for a in st:
        if a.startswith(OURS):
            return nm(a)
    return nm(st[0])


if __name__ == "__main__":
    main()
