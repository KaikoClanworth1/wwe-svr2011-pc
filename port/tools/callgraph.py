"""Build a call graph of the recompiled game from generated/default/*.cpp.

For every function: address, size (bytes of PPC code), direct callees,
kernel/XAM imports it calls, indirect calls (bctrl), and the 32-bit
constants it builds (lis + ori/addi), which is how PM4 packet headers show up.

    python callgraph.py [generated_dir] [out.json]
"""
import glob
import json
import os
import re
import sys

FUNC_RE = re.compile(r"^DEFINE_REX_FUNC\((\w+)\) \{", re.M)
CALL_RE = re.compile(r"^\s*(sub_[0-9A-F]{8}|__imp__\w+|rexcrt_\w+|__\w+gpr\w*)\(ctx, base\);", re.M)
ASM_RE = re.compile(r"^\s*// (\w[\w.]*)\s*(.*)$", re.M)


def parse_int(tok):
    tok = tok.strip()
    try:
        return int(tok, 0)
    except ValueError:
        return None


def analyze(body):
    callees, imports = set(), set()
    for m in CALL_RE.finditer(body):
        name = m.group(1)
        (imports if name.startswith("__imp__") else callees).add(name)
    insns = ASM_RE.findall(body)
    consts = set()
    lis = {}
    indirect = 0
    for op, args in insns:
        a = [x.strip() for x in args.split(",")]
        if op == "bctrl":
            indirect += 1
        if op == "lis" and len(a) == 2:
            v = parse_int(a[1])
            if v is not None:
                lis[a[0]] = (v & 0xFFFF) << 16
        elif op in ("ori", "addi") and len(a) == 3 and a[1] in lis:
            lo = parse_int(a[2])
            if lo is not None:
                hi = lis[a[1]]
                v = (hi | (lo & 0xFFFF)) if op == "ori" else (hi + lo) & 0xFFFFFFFF
                consts.add(v)
        elif op == "li" and len(a) == 2:
            v = parse_int(a[1])
            if v is not None:
                consts.add(v & 0xFFFFFFFF)
    return callees, imports, consts, indirect, len(insns)


def main():
    gen = sys.argv[1] if len(sys.argv) > 1 else "generated/default"
    out = sys.argv[2] if len(sys.argv) > 2 else "runs/callgraph.json"
    funcs = {}
    for fn in sorted(glob.glob(os.path.join(gen, "*_recomp.*.cpp"))):
        text = open(fn, encoding="utf-8", errors="ignore").read()
        starts = [(m.start(), m.group(1)) for m in FUNC_RE.finditer(text)]
        for i, (pos, name) in enumerate(starts):
            end = starts[i + 1][0] if i + 1 < len(starts) else len(text)
            callees, imports, consts, indirect, n = analyze(text[pos:end])
            funcs[name] = {
                "insns": n,
                "calls": sorted(callees),
                "imports": sorted(i.replace("__imp__", "") for i in imports),
                "consts": sorted(consts),
                "indirect": indirect,
            }
    json.dump(funcs, open(out, "w"))
    print(f"{len(funcs)} functions -> {out}")


if __name__ == "__main__":
    main()
