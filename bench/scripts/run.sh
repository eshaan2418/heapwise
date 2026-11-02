#!/usr/bin/env bash
# Run every workload on every backend REPS times, interleaving backends round-robin so
# thermal or background drift affects all allocators equally. Writes JSON lines.
# Usage: bench/scripts/run.sh <build-dir> <trace-dir> <out.jsonl> [reps]
set -euo pipefail
BUILD=${1:?build dir}; TRACES=${2:?trace dir}; OUT=${3:?output}; REPS=${4:-7}
BACKENDS=$(ls "$BUILD"/bench | sed -n 's/^bench_\(.*\)$/\1/p' | sort)
NCPU=$(sysctl -n hw.ncpu 2>/dev/null || nproc)
HALF=$(( NCPU / 2 ))
: > "$OUT"
run() {  # args after backend
  for rep in $(seq 1 "$REPS"); do
    for b in $BACKENDS; do
      gtimeout 600 "$BUILD/bench/bench_$b" "$@" | sed "s/}$/,\"rep\":$rep}/" >> "$OUT"
    done
  done
}
for t in "$TRACES"/*.hwt; do run replay "$t"; done
run larson 1 4000000
run larson "$HALF" 4000000
run prodcon 1 4000000
run prodcon $(( HALF / 2 )) 4000000
run churn 1 40000
run churn "$HALF" 40000
run fragment 10
echo "wrote $(wc -l < "$OUT") results to $OUT"
