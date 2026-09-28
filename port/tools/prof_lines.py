"""Hot source lines from a sampler.exe report: each stack's innermost frame in
svr2011.exe (function+offset) is resolved to file:line (with inlining) by
llvm-symbolizer, using the function's address from the PDB (llvm-pdbutil).
    python prof_lines.py <report.txt> <svr2011.exe>"""
import collections, re, subprocess, sys

LLVM = r"C:\Program Files\LLVM\bin"
report, exe = sys.argv[1], sys.argv[2]
pdb = exe[:-4] + ".pdb"

text = open(report, encoding="utf-8", errors="replace").read()
samples = []  # (count, [frames])
for blk in re.split(r"\n  --\s+", text)[1:]:
    lines = [l.strip() for l in blk.split("\n") if l.strip()]
    m = re.search(r"\((\d+)\)", lines[0])
    if not m:
        continue
    samples.append((int(m.group(1)), [l for l in lines[1:] if not l.startswith("====")]))
total = sum(n for n, _ in samples)

# function name -> section offset, from the PDB's procedure symbols
dump = subprocess.run([LLVM + r"\llvm-pdbutil.exe", "dump", "--symbols", pdb],
                      capture_output=True, text=True, errors="replace").stdout
addr = {}
name = None
for line in dump.split("\n"):
    m = re.search(r"S_[LG]PROC32 \[size = \d+\] `(.*)`", line)
    if m:
        name = m.group(1)
        continue
    m = re.search(r"addr = (\d+):(\d+)", line)
    if m and name:
        if int(m.group(1)) == 1:
            addr.setdefault(name, int(m.group(2)))
        name = None

hits = collections.Counter()
funcs = collections.Counter()
queries = {}
for n, frames in samples:
    for f in frames:
        m = re.match(r"svr2011!(.*)\+0x([0-9a-f]+)$", f)
        if not m:
            continue
        fn, off = m.group(1), int(m.group(2), 16)
        funcs[fn] += n
        if fn in addr:
            va = 0x140000000 + 0x1000 + addr[fn] + off - 1  # inside the call / instruction
            queries[va] = None
            hits[va] += n
        break

out = subprocess.run([LLVM + r"\llvm-symbolizer.exe", "--obj=" + exe, "--inlining"],
                     input="\n".join(hex(a) for a in queries), capture_output=True, text=True).stdout
blocks = out.strip().split("\n\n")
lines = collections.Counter()
for a, b in zip(queries, blocks):
    parts = b.strip().split("\n")
    chain = []
    for i in range(0, len(parts) - 1, 2):
        loc = parts[i + 1].split("\\")[-1].rsplit(":", 1)[0]
        chain.append(f"{parts[i][:60]} @ {loc}")
    lines[" <- ".join(chain[:3])] += hits[a]

print(f"{total} samples")
print("--- innermost svr2011 function")
for k, v in funcs.most_common(15):
    print(f"{100 * v / total:5.1f}%  {k[:100]}")
print("--- lines")
for k, v in lines.most_common(40):
    print(f"{100 * v / total:5.1f}%  {k}")
