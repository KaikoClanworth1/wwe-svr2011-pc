"""Per-function totals for one thread of a sampler report.
  python prof_funcs.py <report.txt> <tid> [top]
Self = samples where the function is the leaf; total = samples whose stack
contains it (once per stack)."""
import re, sys, collections
path, tid = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 30
self_c, tot_c, n = collections.Counter(), collections.Counter(), 0
cur, stack, inthr = 0, [], False
def flush():
    global n
    if not stack: return
    n += cur
    self_c[stack[0]] += cur
    for f in set(stack): tot_c[f] += cur
for line in open(path, encoding='utf-8', errors='replace'):
    if line.startswith('===='):
        flush(); stack = []; inthr = (' ' + tid + ' ') in line
        continue
    if not inthr: continue
    m = re.match(r'\s+--\s+[\d.]+%\s+\((\d+)\)', line)
    if m:
        flush(); stack = []; cur = int(m.group(1)); continue
    s = line.strip()
    if s: stack.append(re.sub(r'\+0x[0-9a-f]+$', '', s))
flush()
print(f"{n} samples")
print("-- total (inclusive)")
for f, c in tot_c.most_common(top): print(f"{100*c/n:5.1f}%  {f}")
print("-- self")
for f, c in self_c.most_common(top): print(f"{100*c/n:5.1f}%  {f}")
