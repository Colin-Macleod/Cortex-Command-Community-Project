#!/usr/bin/env python3
"""Compare two RNG call traces (written by an RTE_RNG_TRACE build with CCCP_DT_TRACE_TICKS set) and resolve call sites.

usage: symbolize_trace.py <binary> <a.rngtrace> <b.rngtrace> [--summary]

Prints the first point where the sequence of global-RNG draws differs between the two runs, with symbolized stacks,
then a per-call-site summary of draws made from worker threads (any draw from a worker thread is a data race on the
global generator) and of call sites whose draw counts differ between the runs.
"""
import collections
import subprocess
import sys


def load(path):
    with open(path) as f:
        return [line.split() for line in f if line.strip()]


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
