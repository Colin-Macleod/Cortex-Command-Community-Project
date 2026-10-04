#!/usr/bin/env python3
"""Compare two RNG call traces (written by an RTE_RNG_TRACE build with CCCP_DT_TRACE_TICKS set) and resolve call sites.

usage: symbolize_trace.py <binary> <a.rngtrace> <b.rngtrace> [--per-generator]

Prints the first point where the sequence of global-RNG draws differs between the two runs, with symbolized stacks,
then a per-call-site summary (or, with --per-generator, compares the draw sequence of each generator - global and per Lua state - separately) of draws made from worker threads (any draw from a worker thread is a data race on the
global generator) and of call sites whose draw counts differ between the runs.
"""
import collections
import subprocess
import sys


def load(path):
    records = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            if not parts:
                continue
            # Optional "lua:<script>" token after the thread/generator token; move it to the end so frames stay at index 2+.
            lua = [p for p in parts[2:] if p.startswith("lua:")]
            frames = [p for p in parts[2:] if not p.startswith("lua:")]
            records.append(parts[:2] + frames + lua)
    return records


def lua_of(rec):
    return next((p[4:] for p in rec if p.startswith("lua:")), "")


def generator_of(rec):
    # Records look like "<tick> <M|W>[:<generator>] <frames...>". Older traces have no generator and only cover the global one.
    return rec[1].split(":")[1] if ":" in rec[1] else "G"


def compare_per_generator(binary, a, b):
    """Draws from each Lua state's generator happen on worker threads, so only the per-generator order is meaningful. Compare each separately."""
    gens = sorted(set(generator_of(r) for r in a) | set(generator_of(r) for r in b))
    earliest = None
    for gen in gens:
        sa = [r for r in a if generator_of(r) == gen]
        sb = [r for r in b if generator_of(r) == gen]
        n = min(len(sa), len(sb))
        first = next((i for i in range(n) if sa[i][0] != sb[i][0] or sa[i][2:] != sb[i][2:]), None)
        if first is None and len(sa) == len(sb):
            continue
        if first is None:
            first = n
        tick = int(sa[first][0]) if first < len(sa) else int(sb[first][0])
        print(f"generator {gen}: {len(sa)} vs {len(sb)} draws, first difference at draw #{first} (tick {tick})")
        for label, seq in (("A", sa), ("B", sb)):
            for k in range(max(0, first - 2), min(len(seq), first + 3)):
                rec = seq[k]
                marker = ">>" if k == first else "  "
                print(f"  {label}{marker} draw #{k}: tick {rec[0]} thread {rec[1]} lua {lua_of(rec)}")
            if first < len(seq):
                rec = seq[first]
                frames = [p for p in rec[2:] if not p.startswith("lua:")]
                for fr in symbolize(binary, frames[:7]):
                    print(f"       {short(fr)[:300]}")
        if earliest is None or tick < earliest[0]:
            earliest = (tick, gen)
    if earliest:
        print(f"\nEarliest divergence: generator {earliest[1]} at tick {earliest[0]}")
    else:
        print("All generators identical")


_cache = {}


def symbolize(binary, addrs):
    todo = [a for a in addrs if a not in _cache]
    if todo:
        out = subprocess.run(["addr2line", "-f", "-C", "-i", "-p", "-e", binary] + todo, capture_output=True, text=True).stdout
        # With -i, one address may produce several lines (inlined frames). Re-run one by one to keep mapping exact.
        for a in todo:
            o = subprocess.run(["addr2line", "-f", "-C", "-i", "-p", "-e", binary, a], capture_output=True, text=True).stdout.strip()
            _cache[a] = " <- ".join(part.replace(" (inlined by) ", "").strip() for part in o.splitlines())
    return [_cache[a] for a in addrs]


def short(frame):
    # Keep "function at file:line", strip directories.
    return frame.replace("/home/user/Cortex-Command-Community-Project/", "")


def site(binary, rec, depth=4):
    frames = symbolize(binary, rec[2:2 + depth])
    return " | ".join(short(f) for f in frames)


def main():
    binary, a_path, b_path = sys.argv[1:4]
    a, b = load(a_path), load(b_path)
    if "--per-generator" in sys.argv:
        compare_per_generator(binary, a, b)
        return
    n = min(len(a), len(b))
    first = next((i for i in range(n) if a[i] != b[i]), None)
    print(f"draws: {len(a)} vs {len(b)}")
    if first is None:
        print("traces identical over common length")
    else:
        print(f"first differing draw #{first} (tick {a[first][0]} vs {b[first][0]})")
        for label, rec in (("A", a[first]), ("B", b[first])):
            print(f"  {label}: tick {rec[0]} thread {rec[1]}")
            for fr in symbolize(binary, rec[2:8]):
                print(f"       {short(fr)}")
    workers = collections.Counter()
    for rec in a + b:
        if rec[1] == "W":
            workers[site(binary, rec)] += 1
    if workers:
        print("\nDraws from WORKER threads (data races on the global RNG):")
        for s, c in workers.most_common(15):
            print(f"  {c:8d}  {s}")
    ca = collections.Counter(site(binary, r) for r in a)
    cb = collections.Counter(site(binary, r) for r in b)
    diffs = [(s, ca[s], cb[s]) for s in set(ca) | set(cb) if ca[s] != cb[s]]
    if diffs:
        print("\nCall sites with different draw counts between runs:")
        for s, x, y in sorted(diffs, key=lambda d: -abs(d[1] - d[2]))[:20]:
            print(f"  {x:8d} vs {y:8d}  {s}")


if __name__ == "__main__":
    main()
