#!/usr/bin/env bash
# Short demo: replay the committed SQLite trace fixture and a cross-thread workload on every
# allocator backend that was built.
set -euo pipefail
BUILD=${1:-build}
fmt() { python3 -c '
import json, sys
for line in sys.stdin:
    d = json.loads(line)
    print("  %-9s %-16s %8.2f ms  %7.1f Mops/s  peak RSS %.1f MB" % (d["backend"], d["workload"] + " " + d["detail"],
          d["seconds"] * 1000, d["mops_per_s"], d["peak_rss_mb"]))'; }
echo "Replaying 6,000 real SQLite allocation calls (bench/traces/fixture.hwt):"
for b in "$BUILD"/bench/bench_*; do "$b" replay bench/traces/fixture.hwt; done | fmt
echo
echo "4 producer/consumer pairs, 1M messages each, every free on another thread:"
for b in "$BUILD"/bench/bench_*; do "$b" prodcon 4 1000000; done | fmt
