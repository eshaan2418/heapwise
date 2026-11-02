#!/usr/bin/env bash
# Linux only: run real programs with heapwise preloaded as the process allocator.
set -euo pipefail
LIB=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
export LD_PRELOAD=$LIB
# Proof heapwise is active: a 17-byte request lands in the 32-byte class (glibc reports 24).
usable=$(python3 -c 'import ctypes
libc = ctypes.CDLL(None)
libc.malloc.restype = ctypes.c_void_p
libc.malloc_usable_size.argtypes = [ctypes.c_void_p]
print(libc.malloc_usable_size(libc.malloc(17)))')
test "$usable" = 32 || { echo "heapwise not active (usable=$usable)"; exit 1; }
python3 - <<'PY'
import json, random, threading
random.seed(0)
def work(n):
    d = [{"k": str(i), "v": [random.random() for _ in range(20)]} for i in range(n)]
    assert len(json.loads(json.dumps(d))) == n
ts = [threading.Thread(target=work, args=(20000,)) for _ in range(8)]
[t.start() for t in ts]; [t.join() for t in ts]
print("python threads ok")
PY
seq 1 300000 | shuf --random-source=<(yes) | sort -n | tail -1
git --version
echo "override smoke test passed"
