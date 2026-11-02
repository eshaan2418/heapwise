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
TIMEOUT=$(command -v gtimeout || command -v timeout)
MAX_LOAD=${MAX_LOAD:-3}
load1() { if [[ -r /proc/loadavg ]]; then cut -d' ' -f1 /proc/loadavg; else sysctl -n vm.loadavg | awk '{print $2}'; fi; }
# Benchmarks must run on a quiet machine: wait (up to 45 min) for the 1-minute load average
# to drop below MAX_LOAD, and record the load next to every result.
wait_quiet() {
  for _ in $(seq 1 90); do
    awk -v l="$(load1)" -v m="$MAX_LOAD" 'BEGIN { exit !(l < m) }' && return 0
    echo "load $(load1) >= $MAX_LOAD, waiting" >&2; sleep 30
  done
  echo "machine never became quiet; aborting" >&2; exit 1
}
# WORKLOADS restricts the run to a space-separated subset (e.g. "larson prodcon").
WORKLOADS=${WORKLOADS:-"replay larson prodcon churn fragment"}
run() {  # args after backend
  [[ " $WORKLOADS " == *" $1 "* ]] || return 0
  for rep in $(seq 1 "$REPS"); do
    wait_quiet
    for b in $BACKENDS; do
      l=$(load1)
      "$TIMEOUT" 600 "$BUILD/bench/bench_$b" "$@" | sed "s/}$/,\"rep\":$rep,\"load1\":$l}/" >> "$OUT"
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
