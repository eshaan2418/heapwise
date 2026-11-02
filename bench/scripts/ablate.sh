#!/usr/bin/env bash
# Ablations: heapwise vs heapwise with one optimisation disabled, on the workloads that
# motivated each optimisation. Same quiet-machine rule and interleaving as run.sh.
# Usage: bench/scripts/ablate.sh <build-dir> <trace-dir> <out.jsonl> [reps]
set -euo pipefail
BUILD=${1:?build dir}; TRACES=${2:?trace dir}; OUT=${3:?output}; REPS=${4:-7}
TIMEOUT=$(command -v gtimeout || command -v timeout)
MAX_LOAD=${MAX_LOAD:-3}
load1() { if [[ -r /proc/loadavg ]]; then cut -d' ' -f1 /proc/loadavg; else sysctl -n vm.loadavg | awk '{print $2}'; fi; }
wait_quiet() {
  for _ in $(seq 1 90); do
    awk -v l="$(load1)" -v m="$MAX_LOAD" 'BEGIN { exit !(l < m) }' && return 0
    echo "load $(load1) >= $MAX_LOAD, waiting" >&2; sleep 30
  done
  echo "machine never became quiet; aborting" >&2; exit 1
}
: > "$OUT"
run() {
  for rep in $(seq 1 "$REPS"); do
    wait_quiet
    for exe in bench_heapwise ablate_no_hysteresis ablate_no_direct_tsd; do
      [[ -x "$BUILD/bench/$exe" ]] || continue
      l=$(load1)
      "$TIMEOUT" 600 "$BUILD/bench/$exe" "$@" | sed "s/\"backend\":\"heapwise\"/\"backend\":\"$exe\"/; s/}$/,\"rep\":$rep,\"load1\":$l}/" >> "$OUT"
    done
  done
}
run larson 1 4000000
run larson 9 4000000
run churn 1 40000
run replay "$TRACES/python-ast.hwt"
run replay "$TRACES/sqlite.hwt"
echo "wrote $(wc -l < "$OUT") results to $OUT"
