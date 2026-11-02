Ablation runs (bench/scripts/ablate.sh), 7 interleaved runs each, median 1-minute load average 7.7 on 18 cores (another workload was running; see bench/results/ablation-20261102.jsonl, field load1).

| workload | backend | median time (ms) | mean +/- 95% CI (ms) | max (ms) | Mops/s (median) | peak RSS (MB, median) | vs system |
|---|---|---:|---:|---:|---:|---:|---:|
| larson 1t | ablate_no_direct_tsd | 59.8 | 62.0 +/- 5.9 | 74.5 | 66.9 | 5.4 | - |
| larson 1t | ablate_no_hysteresis | 71.4 | 74.7 +/- 8.7 | 95.3 | 56.0 | 5.2 | - |
| larson 1t | bench_heapwise | 60.1 | 62.8 +/- 6.1 | 75.7 | 66.5 | 5.4 | - |
| larson 9t | ablate_no_direct_tsd | 86.1 | 86.2 +/- 2.4 | 89.6 | 418.1 | 60.2 | - |
| larson 9t | ablate_no_hysteresis | 100.5 | 98.8 +/- 3.5 | 102.6 | 358.3 | 69.8 | - |
| larson 9t | bench_heapwise | 84.9 | 84.6 +/- 2.6 | 88.9 | 424.0 | 56.8 | - |
| churn 1t | ablate_no_direct_tsd | 61.5 | 62.4 +/- 6.4 | 76.5 | 333.0 | 2.4 | - |
| churn 1t | ablate_no_hysteresis | 57.8 | 58.1 +/- 4.5 | 65.1 | 354.5 | 2.4 | - |
| churn 1t | bench_heapwise | 60.0 | 61.3 +/- 7.7 | 73.0 | 341.1 | 2.4 | - |
| replay python-ast.hwt | ablate_no_direct_tsd | 69.8 | 68.2 +/- 6.2 | 75.2 | 196.3 | 292.8 | - |
| replay python-ast.hwt | ablate_no_hysteresis | 62.2 | 65.5 +/- 9.0 | 83.8 | 220.1 | 292.6 | - |
| replay python-ast.hwt | bench_heapwise | 62.4 | 66.1 +/- 8.6 | 85.9 | 219.4 | 292.8 | - |
| replay sqlite.hwt | ablate_no_direct_tsd | 2.9 | 2.8 +/- 0.1 | 3.0 | 295.2 | 28.6 | - |
| replay sqlite.hwt | ablate_no_hysteresis | 2.5 | 2.6 +/- 0.1 | 2.9 | 332.8 | 28.6 | - |
| replay sqlite.hwt | bench_heapwise | 2.5 | 2.6 +/- 0.1 | 2.8 | 334.6 | 28.6 | - |
