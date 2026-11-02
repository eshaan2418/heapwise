Trace replays: 7 runs, load average < 3 (raw-20261101.jsonl). Larson 9t, prodcon, churn, fragment: 7 runs at median load 7.7 (raw-20261102-mt.jsonl). Merged in raw-merged.jsonl.

| workload | backend | median time (ms) | mean +/- 95% CI (ms) | max (ms) | Mops/s (median) | peak RSS (MB, median) | vs system |
|---|---|---:|---:|---:|---:|---:|---:|
| replay ffmpeg.hwt | heapwise | 2.7 | 2.8 +/- 0.2 | 3.1 | 34.0 | 48.4 | 1.09x |
| replay ffmpeg.hwt | jemalloc | 2.9 | 2.9 +/- 0.1 | 3.0 | 31.8 | 48.9 | 1.03x |
| replay ffmpeg.hwt | mimalloc | 2.4 | 2.4 +/- 0.1 | 2.5 | 38.6 | 48.4 | 1.24x |
| replay ffmpeg.hwt | system | 2.9 | 3.0 +/- 0.1 | 3.1 | 31.1 | 47.7 | 1.00x |
| replay jq.hwt | heapwise | 23.4 | 26.6 +/- 6.1 | 40.4 | 114.2 | 175.5 | 1.47x |
| replay jq.hwt | jemalloc | 28.5 | 28.3 +/- 1.0 | 29.7 | 93.7 | 173.6 | 1.21x |
| replay jq.hwt | mimalloc | 17.8 | 18.4 +/- 3.0 | 25.4 | 149.6 | 177.2 | 1.93x |
| replay jq.hwt | system | 34.4 | 45.5 +/- 15.1 | 74.0 | 77.7 | 173.2 | 1.00x |
| replay python-ast.hwt | heapwise | 52.8 | 52.4 +/- 1.4 | 54.9 | 259.5 | 292.8 | 1.72x |
| replay python-ast.hwt | jemalloc | 84.0 | 85.4 +/- 4.0 | 91.4 | 163.0 | 290.4 | 1.08x |
| replay python-ast.hwt | mimalloc | 48.9 | 49.3 +/- 2.3 | 52.4 | 280.0 | 290.6 | 1.86x |
| replay python-ast.hwt | system | 90.9 | 91.2 +/- 4.2 | 100.6 | 150.6 | 292.6 | 1.00x |
| replay sqlite.hwt | heapwise | 2.5 | 2.5 +/- 0.1 | 2.7 | 335.5 | 28.6 | 1.59x |
| replay sqlite.hwt | jemalloc | 3.0 | 3.1 +/- 0.1 | 3.3 | 279.1 | 27.1 | 1.32x |
| replay sqlite.hwt | mimalloc | 2.1 | 2.1 +/- 0.2 | 2.3 | 410.2 | 28.4 | 1.94x |
| replay sqlite.hwt | system | 4.0 | 4.0 +/- 0.1 | 4.2 | 211.3 | 25.7 | 1.00x |
| larson 1t | heapwise | 54.0 | 55.4 +/- 5.3 | 68.1 | 74.0 | 5.4 | 3.38x |
| larson 1t | jemalloc | 30.5 | 30.3 +/- 0.8 | 31.4 | 131.1 | 7.1 | 5.99x |
| larson 1t | mimalloc | 44.4 | 44.5 +/- 1.9 | 48.5 | 90.1 | 9.2 | 4.11x |
| larson 1t | system | 182.6 | 180.2 +/- 5.9 | 188.5 | 21.9 | 4.8 | 1.00x |
| larson 9t | heapwise | 110.6 | 113.0 +/- 11.7 | 131.1 | 325.4 | 54.8 | 17.24x |
| larson 9t | jemalloc | 77.2 | 79.0 +/- 9.0 | 91.5 | 466.1 | 32.5 | 24.69x |
| larson 9t | mimalloc | 100.2 | 97.2 +/- 11.3 | 111.9 | 359.3 | 40.1 | 19.04x |
| larson 9t | system | 1907.3 | 1886.7 +/- 127.5 | 2029.0 | 18.9 | 83.1 | 1.00x |
| prodcon 1pairs | heapwise | 588.5 | 585.0 +/- 41.2 | 670.8 | 6.8 | 3.7 | 0.94x |
| prodcon 1pairs | jemalloc | 222.6 | 221.5 +/- 17.6 | 248.2 | 18.0 | 5.1 | 2.49x |
| prodcon 1pairs | mimalloc | 587.8 | 580.2 +/- 35.2 | 620.4 | 6.8 | 9.2 | 0.94x |
| prodcon 1pairs | system | 554.0 | 550.1 +/- 29.2 | 604.1 | 7.2 | 2.8 | 1.00x |
| prodcon 4pairs | heapwise | 748.0 | 715.7 +/- 110.6 | 843.7 | 21.4 | 10.2 | 0.89x |
| prodcon 4pairs | jemalloc | 259.7 | 321.3 +/- 154.9 | 700.0 | 61.6 | 13.6 | 2.57x |
| prodcon 4pairs | mimalloc | 710.2 | 681.0 +/- 147.1 | 824.3 | 22.5 | 9.7 | 0.94x |
| prodcon 4pairs | system | 666.6 | 779.8 +/- 242.3 | 1216.1 | 24.0 | 15.5 | 1.00x |
| churn 1t | heapwise | 53.0 | 53.0 +/- 1.6 | 55.2 | 386.7 | 2.4 | 2.98x |
| churn 1t | jemalloc | 53.7 | 54.7 +/- 1.9 | 57.7 | 381.2 | 4.1 | 2.94x |
| churn 1t | mimalloc | 64.1 | 65.4 +/- 2.7 | 70.1 | 319.5 | 9.2 | 2.46x |
| churn 1t | system | 157.9 | 161.0 +/- 10.3 | 183.7 | 129.7 | 2.2 | 1.00x |
| churn 9t | heapwise | 65.5 | 65.0 +/- 1.1 | 66.7 | 2815.3 | 4.8 | 2.93x |
| churn 9t | jemalloc | 68.1 | 72.4 +/- 12.5 | 102.7 | 2707.8 | 6.3 | 2.82x |
| churn 9t | mimalloc | 63.8 | 67.1 +/- 5.2 | 76.9 | 2890.3 | 9.2 | 3.01x |
| churn 9t | system | 191.7 | 191.5 +/- 2.8 | 195.6 | 961.3 | 4.3 | 1.00x |
| fragment 10M | heapwise | 167.0 | 179.8 +/- 28.7 | 241.3 | 89.8 | 1725.4 | 1.30x |
| fragment 10M | jemalloc | 304.5 | 322.5 +/- 31.2 | 372.9 | 49.3 | 1814.5 | 0.71x |
| fragment 10M | mimalloc | 204.2 | 200.2 +/- 25.4 | 238.5 | 73.5 | 1811.3 | 1.06x |
| fragment 10M | system | 217.4 | 243.5 +/- 60.0 | 380.1 | 69.0 | 1804.1 | 1.00x |
