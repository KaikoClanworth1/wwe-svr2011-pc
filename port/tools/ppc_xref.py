"""Cross-references in the recompiled code (generated/default/*.cpp).

The generated C++ keeps the PowerPC disassembly as comments. This script
follows `lis rX,hi` and the `addi/lfs/lwz/stw... rY,lo(rX)` that use it, and
records every absolute address a function builds or loads from.

    python ppc_xref.py index                 builds runs/xref.json (once)
    python ppc_xref.py who <addr> [<addr2>]  functions that touch addr (or the range)
    python ppc_xref.py what <func>           addresses a function touches
    python ppc_xref.py calls <func>          functions it calls (bl)
    python ppc_xref.py callers <func>        functions that call it
    python ppc_xref.py imm <value>           functions using the immediate (li/cmpwi/...)
"""
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(HERE, '..', 'generated', 'default')
OUT = os.path.join(HERE, '..', 'runs', 'xref.json')

FUNC = re.compile(r'^(?:PPC_FUNC_IMPL\(__imp__|PPC_FUNC\()?\s*(?:__imp__)?sub_([0-9A-F]{8})\)')
FUNC2 = re.compile(r'sub_([0-9A-F]{8})\)\s*\{?\s*$')
ASM = re.compile(r'^\s*//\s*([a-z.]+)\s+(.*)$')


def build():
    refs = {}    # func -> sorted addresses
    calls = {}   # func -> callees
    imms = {}    # func -> immediates
    for path in sorted(glob.glob(os.path.join(GEN, 'svr2011_recomp.*.cpp'))):
        func = None
        hi = {}
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('DEFINE_REX_FUNC('):
                    m = re.search(r'sub_([0-9A-F]{8})', line)
                    if m:
                        func = m.group(1)
                        hi = {}
                        refs.setdefault(func, set())
                        calls.setdefault(func, set())
                        imms.setdefault(func, set())
                    continue
                if func is None or '//' not in line:
                    continue
                m = ASM.match(line)
                if not m:
                    continue
                op, args = m.group(1), m.group(2).replace(' ', '')
                parts = args.split(',')
                if op == 'lis' and len(parts) == 2:
                    try:
                        hi[parts[0]] = (int(parts[1], 0) & 0xFFFF) << 16
                    except ValueError:
                        pass
                    continue
                if op in ('bl',) and parts:
                    mm = re.search(r'0x([0-9a-fA-F]{8})', parts[0])
                    if mm:
                        calls[func].add(mm.group(1).upper())
                    continue
                if op in ('addi', 'ori', 'addic') and len(parts) == 3 and parts[1] in hi:
                    try:
                        lo = int(parts[2], 0)
                    except ValueError:
                        continue
                    base = hi[parts[1]]
                    addr = (base + lo) & 0xFFFFFFFF if op != 'ori' else base | (lo & 0xFFFF)
                    refs[func].add(addr)
                    if parts[0] != parts[1]:
                        hi.pop(parts[0], None)
                    else:
                        hi.pop(parts[0], None)
                    continue
                mm = re.match(r'(-?\d+)\((r\d+)\)$', parts[-1]) if parts else None
                if mm and mm.group(2) in hi and (op.startswith('l') or op.startswith('st')):
                    addr = (hi[mm.group(2)] + int(mm.group(1))) & 0xFFFFFFFF
                    refs[func].add(addr)
                    if op.startswith('l') and parts[0] == mm.group(2):
                        hi.pop(parts[0], None)
                    continue
                if op in ('li', 'cmpwi', 'cmplwi', 'cmpw', 'subfic', 'mulli') and parts:
                    try:
                        imms[func].add(int(parts[-1], 0))
                    except ValueError:
                        pass
                # any other write to a tracked register forgets it
                if parts and parts[0] in hi and not op.startswith('st') and not op.startswith('cmp'):
                    hi.pop(parts[0], None)
    data = {
        'refs': {k: sorted(v) for k, v in refs.items()},
        'calls': {k: sorted(v) for k, v in calls.items()},
        'imms': {k: sorted(v) for k, v in imms.items()},
    }
    with open(OUT, 'w') as f:
        json.dump(data, f)
    print('indexed', len(refs), 'functions ->', OUT)


def load():
    with open(OUT) as f:
        return json.load(f)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    if cmd == 'index':
        build()
        return
    d = load()
    if cmd == 'who':
        a = int(sys.argv[2], 16)
        b = int(sys.argv[3], 16) if len(sys.argv) > 3 else a
        for fn, addrs in sorted(d['refs'].items()):
            hit = [x for x in addrs if a <= x <= b]
            if hit:
                print('sub_' + fn, ' '.join('%08X' % x for x in hit))
    elif cmd == 'what':
        print(' '.join('%08X' % x for x in d['refs'].get(sys.argv[2].upper().replace('SUB_', ''), [])))
    elif cmd == 'calls':
        print(' '.join(d['calls'].get(sys.argv[2].upper().replace('SUB_', ''), [])))
    elif cmd == 'callers':
        t = sys.argv[2].upper().replace('SUB_', '')
        print(' '.join('sub_' + fn for fn, c in sorted(d['calls'].items()) if t in c))
    elif cmd == 'imm':
        v = int(sys.argv[2], 0)
        print(' '.join('sub_' + fn for fn, c in sorted(d['imms'].items()) if v in c))




def dis(name):
    """Prints a function's disassembly (from the generated comments)."""
    name = name.upper().replace('SUB_', '')
    for path in glob.glob(os.path.join(GEN, 'svr2011_recomp.*.cpp')):
        with open(path, encoding='utf-8', errors='replace') as f:
            text = f.read()
        i = text.find('DEFINE_REX_FUNC(sub_%s)' % name)
        if i < 0:
            continue
        j = text.find('\n}\n', i)
        for line in text[i:j].splitlines():
            s = line.strip()
            if s.startswith('// '):
                print('   ', s[3:])
            elif s.startswith('loc_'):
                print(s)
        return


if __name__ == '__main__' and len(sys.argv) > 2 and sys.argv[1] == 'dis':
    dis(sys.argv[2])

elif __name__ == '__main__':
    main()
