r"""The lockstep lag budget of one two-game round (svrtest lan): where each frame between a press and the
moment the game applies it goes.

Reads the two games' logs (host.log, join.log: the lead line, the 5 s "net:" lines) and their net input logs
(SVR2011_NET_INPUT_LOG, net_stats.cpp: "S <ms> <input>" at each lockstep send, a line per take
"<ms> local <slot> | <slot>:<got> <own> <applied> ...", "P <ms> <len> <lr> <head>" / "R <ms> <len> <head>" per packet
the game hands its socket / reads). Both games run on one PC, so the stamps share a clock.

For every change of a game's sent input (a press or release): local = ms and takes until its own game's take
applies it, then until the next update pass (the next S line) that steps the world with it. Remote = ms until the
OTHER game's take applies it (by value: the take lines carry each entry's applied input), split into the trip
(the input packets - 25 bytes, one a frame, in order - so the i-th send is the i-th such packet read over there)
and the wait in the queue after that. Queue depth = input packets read minus takes, over the match (its rise
above its lowest = frames queued ahead). The games' speed: takes a second; a press counts as "at full speed"
when its own game took 57+ frames in the second before and after it (other chats' tests load this PC).

    py -3 lag_budget.py <round folder> <host input log> <join input log>
Writes <round folder>\lag_budget.txt and prints it."""
import bisect
import os
import re
import statistics
import sys

INPUT_PACKET = 25  # (gss input: type 16 + 4 bytes, in its 21-byte envelope)


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(len(xs) * p))]


def fmt(xs, unit="ms"):
    if not xs:
        return "n/a"
    return f"median {statistics.median(xs):.0f} {unit}, p90 {pct(xs, 0.9):.0f}, max {max(xs):.0f}, n {len(xs)}"


def read_input_log(path):
    sends, takes, packets_in = [], [], []
    local = None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"S (\d+) ([0-9A-F]{8})", line)
        if m:
            sends.append((int(m.group(1)), m.group(2)))
            continue
        m = re.match(r"(\d+) local (\d+)(.*)", line)
        if m:
            if local is None:
                local = int(m.group(2))
            entries = {}
            for e in re.finditer(r"\| (\d+):(\d+) ([0-9A-F]{8}) ([0-9A-F]{8})", m.group(3)):
                entries[int(e.group(1))] = (int(e.group(2)), e.group(3), e.group(4))
            takes.append((int(m.group(1)), entries))
            continue
        m = re.match(r"R (\d+) (\d+) ", line)
        if m and int(m.group(2)) == INPUT_PACKET:
            packets_in.append(int(m.group(1)))
    return {"local": local, "sends": sends, "takes": takes, "in": packets_in}


def changes(seq):
    out, last = [], None
    for i, (t, v) in enumerate(seq):
        if v != last:
            out.append((i, t, v))
            last = v
    return out


def read_game_log(path):
    r = {"lead": None, "frames": 0, "waited": 0, "waits": 0, "longest": 0.0, "windows": []}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"net: input lead (\d+) frames \(the game's (\d+)(?:, the host's)?\)", line)
        if m:
            r["lead"] = (int(m.group(1)), int(m.group(2)))
        m = re.search(r"net: ([\d.]+) s, (\d+) frames .*?waited (\d+) frames \((\d+) ms\) in (\d+) waits, longest (\d+) ms", line)
        if m:
            r["frames"] += int(m.group(2))
            r["waited"] += int(m.group(3))
            r["waits"] += int(m.group(5))
            r["longest"] = max(r["longest"], float(m.group(6)))
            r["windows"].append(int(m.group(2)) / float(m.group(1)))
    return r


def takes_per_second(takes):
    """{second: takes in it} from the take stamps (ms)."""
    counts = {}
    for t, _ in takes:
        counts[t // 1000] = counts.get(t // 1000, 0) + 1
    return counts


def full_speed(counts, t):
    s = t // 1000
    return counts.get(s - 1, 0) >= 57 and counts.get(s, 0) >= 57 and counts.get(s + 1, 0) >= 57


def budget(mine, other):
    my_slot = mine["local"]
    sc = changes(mine["sends"])[1:]
    takes_mine, takes_other = mine["takes"], other["takes"]
    take_times_mine = [t for t, _ in takes_mine]
    take_times_other = [t for t, _ in takes_other]
    sends_mine_t = [t for t, _ in mine["sends"]]
    counts_mine = takes_per_second(takes_mine)
    counts_other = takes_per_second(takes_other)
    res = {k: [] for k in ("local_ms", "local_takes", "pass_ms", "remote_ms", "trip_ms", "queue_ms", "queue_takes",
                           "local_ms_fast", "pass_ms_fast", "remote_ms_fast", "trip_ms_fast", "queue_ms_fast")}
    res["unmatched"] = res["n"] = 0
    # (the trip by order: the i-th send is the (i + d)-th input packet read over there; d = the packets in flight /
    # logged before the first send, found as the shift that puts the most arrivals between a press and its take)
    arrivals = other["in"]
    pairs = []
    for i, t, v in sc[:80]:
        j0 = bisect.bisect_left(take_times_other, t)
        j = next((j for j in range(j0, len(takes_other)) if takes_other[j][1].get(my_slot, (0, "", ""))[2] == v), None)
        if j is not None and takes_other[j][0] - t <= 4000:
            pairs.append((i, t, takes_other[j][0]))
    best, shift = -1, 0
    for d in range(-1200, 1201):
        hits = sum(1 for i, t, tr in pairs if 0 <= i + d < len(arrivals) and t <= arrivals[i + d] <= tr)
        if hits > best:
            best, shift = hits, d
    res["shift"] = shift
    for i, t, v in sc:
        i += shift
        res["n"] += 1
        k0 = bisect.bisect_left(take_times_mine, t)
        k = next((k for k in range(k0, len(takes_mine)) if takes_mine[k][1].get(my_slot, (0, "", ""))[2] == v), None)
        if k is None or takes_mine[k][0] - t > 3000:
            res["unmatched"] += 1
            continue
        ta = takes_mine[k][0]
        fast = full_speed(counts_mine, t) and full_speed(counts_mine, ta)
        res["local_ms"].append(ta - t)
        res["local_takes"].append(k - k0 + 1)
        p = bisect.bisect_right(sends_mine_t, ta)
        if p < len(sends_mine_t):
            res["pass_ms"].append(sends_mine_t[p] - t)
            if fast:
                res["pass_ms_fast"].append(sends_mine_t[p] - t)
        if fast:
            res["local_ms_fast"].append(ta - t)
        j0 = bisect.bisect_left(take_times_other, t)
        j = next((j for j in range(j0, len(takes_other)) if takes_other[j][1].get(my_slot, (0, "", ""))[2] == v), None)
        if j is None or takes_other[j][0] - t > 4000:
            continue
        tr = takes_other[j][0]
        fast_r = fast and full_speed(counts_other, tr)
        res["remote_ms"].append(tr - t)
        if fast_r:
            res["remote_ms_fast"].append(tr - t)
        if 0 <= i < len(arrivals) and t <= arrivals[i] <= tr:
            res["trip_ms"].append(arrivals[i] - t)
            res["queue_ms"].append(tr - arrivals[i])
            res["queue_takes"].append(bisect.bisect_right(take_times_other, tr) - bisect.bisect_right(take_times_other, arrivals[i]))
            if fast_r:
                res["trip_ms_fast"].append(arrivals[i] - t)
                res["queue_ms_fast"].append(tr - arrivals[i])
    return res


def queue_depth(mine):
    """Input packets read minus takes, at each take (from the first take on)."""
    if not mine["takes"] or not mine["in"]:
        return []
    arrivals = mine["in"]
    depth = []
    for k, (t, _) in enumerate(mine["takes"]):
        depth.append(bisect.bisect_right(arrivals, t) - (k + 1))
    low = min(depth)
    return [d - low for d in depth]


def tick_offsets(mine):
    offs = []
    sends = [t for t, _ in mine["sends"]]
    for t, _ in mine["takes"]:
        i = bisect.bisect_right(sends, t) - 1
        if i >= 0:
            offs.append(t - sends[i])
    return offs


def main():
    folder, host_in, join_in = sys.argv[1], sys.argv[2], sys.argv[3]
    host, join = read_input_log(host_in), read_input_log(join_in)
    hl, jl = read_game_log(os.path.join(folder, "host.log")), read_game_log(os.path.join(folder, "join.log"))
    lines = [f"lag budget: {folder}", f"lead: host {hl['lead']} joiner {jl['lead']} (the port's, the game's own)"]
    for name, g, log in (("host", host, hl), ("joiner", join, jl)):
        counts = takes_per_second(g["takes"])
        secs = sorted(counts)[1:-1]
        slow = sum(1 for s in secs if counts[s] < 57)
        w = log["windows"]
        lines.append(f"{name}: {log['frames']} lockstep frames, waited {log['waited']} in {log['waits']} waits, longest {log['longest']:.0f} ms; "
                     f"takes {len(g['takes'])}, input packets {len(g['in'])}; speed: {slow} of {len(secs)} seconds under 57 takes"
                     + (f", 5 s windows {min(w):.0f}-{max(w):.0f} frames/s (median {statistics.median(w):.0f})" if w else ""))
        d = queue_depth(g)
        if d:
            lines.append(f"  queue (input packets read - takes, above its lowest): {fmt(d, 'frames')}; takes offset from the last send: {fmt(tick_offsets(g))}")
    lines.append("")
    for name, mine, other in (("host", host, join), ("joiner", join, host)):
        b = budget(mine, other)
        lines += [f"== {name}'s presses (sent input changes, n {b['n']}, unmatched {b['unmatched']}; packet order shift {b['shift']})",
                  f"  local: press -> own take applies it: {fmt(b['local_ms'])}; takes in between: {fmt(b['local_takes'], 'takes')}",
                  f"         press -> the next update pass with it: {fmt(b['pass_ms'])}",
                  f"         at full speed only: take {fmt(b['local_ms_fast'])}; pass {fmt(b['pass_ms_fast'])}",
                  f"  remote: press -> the other game's take applies it: {fmt(b['remote_ms'])}; at full speed {fmt(b['remote_ms_fast'])}",
                  f"          the trip (send -> that packet read over there): {fmt(b['trip_ms'])}; at full speed {fmt(b['trip_ms_fast'])}",
                  f"          then the queue (read -> taken): {fmt(b['queue_ms'])}, {fmt(b['queue_takes'], 'takes')}; at full speed {fmt(b['queue_ms_fast'])}",
                  ""]
    out = "\n".join(lines) + "\n"
    open(os.path.join(folder, "lag_budget.txt"), "w", encoding="utf-8").write(out)
    print(out)


if __name__ == "__main__":
    main()
