"""Summarise bench/scripts/run.sh output: median, mean +/- 95% CI, and peak RSS per backend."""

import json
import statistics
import sys
from collections import defaultdict

T95 = {2: 12.71, 3: 4.30, 4: 3.18, 5: 2.78, 6: 2.57, 7: 2.45, 8: 2.36, 9: 2.31, 10: 2.26}


def ci95(xs):
    if len(xs) < 2:
        return 0.0
    return T95.get(len(xs), 1.96) * statistics.stdev(xs) / len(xs) ** 0.5


def main(path):
    groups = defaultdict(list)
    order = []
    for line in open(path):
        d = json.loads(line)
        key = (d["workload"], d["detail"])
        if key not in order:
            order.append(key)
        groups[key + (d["backend"],)].append(d)
    backends = sorted({k[2] for k in groups})
    print("| workload | backend | median time (ms) | mean +/- 95% CI (ms) | max (ms) | Mops/s (median) | peak RSS over base (MB, median) | vs system |")
    print("|---|---|---:|---:|---:|---:|---:|---:|")
    for w, det in order:
        sys_med = statistics.median(r["seconds"] for r in groups[(w, det, "system")]) if (w, det, "system") in groups else None
        for b in backends:
            rs = groups.get((w, det, b))
            if not rs:
                continue
            ms = [r["seconds"] * 1000 for r in rs]
            mops = statistics.median(r["mops_per_s"] for r in rs)
            rss = statistics.median(r["peak_rss_mb"] - r["rss_base_mb"] for r in rs)
            med = statistics.median(ms)
            speed = f"{sys_med * 1000 / med:.2f}x" if sys_med else "-"
            print(f"| {w} {det} | {b} | {med:.1f} | {statistics.mean(ms):.1f} +/- {ci95(ms):.1f} | {max(ms):.1f} | {mops:.1f} | {rss:.1f} | {speed} |")


if __name__ == "__main__":
    main(sys.argv[1])
