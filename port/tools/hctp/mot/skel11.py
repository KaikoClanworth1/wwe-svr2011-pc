"""SvR2011 character skeleton (ch.pac body JBOY node table: name, t, r @+32, parent @+48; BE)."""
import os
import sys, struct, math
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
import ch_tool, svrfmt
import w13_char

def nodes(path):
    _, groups = ch_tool.epk8_read(open(path, 'rb').read())
    n, b = groups[0][1][0]
    t = {i: ch_tool.unpack(x) for i, x in svrfmt.pach_read(ch_tool.unpack(b))}
    return w13_char.read13_nodes(t[0])

if __name__ == '__main__':
    ns = nodes(sys.argv[1])
    W = w13_char.world(ns)
    for i, n in enumerate(ns):
        print(i, n['name'], n['parent'], ' t=%.3f %.3f %.3f' % n['t'], ' r=%.1f %.1f %.1f' % tuple(math.degrees(a) for a in n['r']),
              ' W=%.2f %.2f %.2f' % tuple(W[i][:3, 3]))
