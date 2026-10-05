#!/usr/bin/env python3
"""Compare two CCCP determinism harness logs and report the first tick at which each state component diverges."""
import sys

COMPONENTS = ["rng", "luaRng", "actors", "items", "particles", "terrain", "activity"]


def load(path):
    rows = {}
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                continue
            try:
                counts, hashes, combined = line.split("|")
                counts = counts.split()
                tick = int(counts[0])
            except ValueError:
                # The last line of a log whose game was killed while writing it.
                continue
            rows[tick] = {"counts": tuple(int(c) for c in counts[1:]), "hashes": hashes.split(), "combined": combined.strip()}
    return rows


def main():
    a, b = load(sys.argv[1]), load(sys.argv[2])
    ticks = sorted(set(a) & set(b))
    if not ticks:
        print("no common ticks")
        return 2
    first = {}
    for t in ticks:
        for i, name in enumerate(COMPONENTS[:min(len(a[t]["hashes"]), len(b[t]["hashes"]))]):
            if name not in first and a[t]["hashes"][i] != b[t]["hashes"][i]:
                first[name] = t
        if "counts" not in first and a[t]["counts"] != b[t]["counts"]:
            first["counts"] = t
    if not first:
        print(f"IDENTICAL for all {len(ticks)} ticks")
        return 0
    order = sorted(first.items(), key=lambda kv: kv[1])
    print(f"DIVERGED at tick {order[0][1]} of {len(ticks)}: " + ", ".join(f"{k}@{v}" for k, v in order))
    t = order[0][1]
    print(f"  counts at tick {t}: {a[t]['counts']} vs {b[t]['counts']}")
    last = ticks[-1]
    print(f"  counts at final tick {last}: {a[last]['counts']} vs {b[last]['counts']}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
