#!/usr/bin/env bash
# Record allocation traces from real programs (macOS only) and compile them to .hwt.
# Usage: bench/scripts/record_traces.sh <build-dir> <out-dir>
# Programs: CPython (PYTHONMALLOC=malloc) compiling stdlib modules, SQLite building and
# querying a table, jq transforming a generated JSON document, ffmpeg encoding a test pattern.
set -euo pipefail
BUILD=${1:?build dir}
OUT=${2:?output dir}
TRACER="$(cd "$BUILD" && pwd)/bench/libhwtracer.dylib"
COMPILE="$BUILD/bench/hw_tracecompile"
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
WORK=$(mktemp -d "$OUT/work.XXXX")
trap 'rm -rf "$WORK"' EXIT

PY=${PYTHON:-$(uv python find 3.12)}
SQLITE=${SQLITE:-$(brew --prefix sqlite)/bin/sqlite3}
JQ=${JQ:-$(command -v jq)}
FFMPEG=${FFMPEG:-$(command -v ffmpeg)}

record() {  # name, command...
  local name=$1; shift
  local dir="$WORK/$name"; mkdir -p "$dir"
  HWTRACE_DIR="$dir" DYLD_INSERT_LIBRARIES="$TRACER" "$@" > /dev/null
  # Child processes write their own files; keep the largest (the main workload process).
  local raw; raw=$(ls -S "$dir"/trace.*.raw | head -1)
  echo -n "$name: "
  "$COMPILE" "$raw" "$OUT/$name.hwt" | tee "$OUT/$name.summary.json"
}

# 1. CPython: parse + compile 400 stdlib modules with the system allocator path forced.
STDLIB=$("$PY" -c 'import sysconfig; print(sysconfig.get_paths()["stdlib"])')
cat > "$WORK/pywork.py" <<PYEOF
import ast, pathlib
files = sorted(pathlib.Path("$STDLIB").glob("*.py"))[:400]
total = 0
for f in files:
    src = f.read_text(encoding="utf-8", errors="replace")
    tree = ast.parse(src)
    total += sum(1 for _ in ast.walk(tree))
    compile(tree, str(f), "exec")
print(total)
PYEOF
# -I would imply -E and silently ignore PYTHONMALLOC, so use -s -P instead.
PYTHONMALLOC=malloc record python-ast "$PY" -s -P "$WORK/pywork.py"

# 2. SQLite: 200k-row table with text payloads, secondary index, aggregates, deletes.
cat > "$WORK/sqlite.sql" <<'SQLEOF'
PRAGMA journal_mode=OFF;
CREATE TABLE t(id INTEGER PRIMARY KEY, k TEXT, v TEXT, n INTEGER);
WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM c WHERE i < 200000)
INSERT INTO t SELECT i, hex(randomblob(8)), printf('%.*c', abs(random()) % 300, 'x'), abs(random()) % 1000 FROM c;
CREATE INDEX t_k ON t(k);
SELECT n % 17, count(*), sum(length(v)) FROM t GROUP BY 1 ORDER BY 1;
SELECT count(*) FROM t a JOIN t b ON a.n = b.id WHERE a.id < 20000;
DELETE FROM t WHERE n % 2 = 0;
SELECT group_concat(k, ',') FROM (SELECT k FROM t ORDER BY k LIMIT 5000);
VACUUM;
SQLEOF
record sqlite "$SQLITE" "$WORK/db.sqlite" ".read $WORK/sqlite.sql"

# 3. jq: 40k records of nested JSON, filter + group + reshape.
"$PY" -I -c '
import json, random
random.seed(1)
recs = [{"id": i, "name": "user%d" % (i % 500), "score": random.random() * 100,
         "tags": ["t%d" % random.randrange(50) for _ in range(random.randrange(1, 12))],
         "meta": {"city": random.choice(["toronto", "montreal", "vancouver", "calgary"]),
                  "visits": [random.randrange(1000) for _ in range(random.randrange(0, 20))]}}
        for i in range(40000)]
print(json.dumps(recs))' > "$WORK/data.json"
record jq "$JQ" -c '[.[] | select(.score > 20) | {name, n: (.tags | length), city: .meta.city, v: (.meta.visits | add)}]
  | group_by(.city) | map({city: .[0].city, users: (map(.name) | unique | length), visits: (map(.v // 0) | add)})' \
  "$WORK/data.json"

# 4. ffmpeg: encode 30 s of a synthetic 720p test pattern to MPEG-4 Part 2, 4 threads.
record ffmpeg "$FFMPEG" -nostdin -loglevel error -f lavfi -i testsrc2=size=1280x720:rate=30:duration=30 \
  -c:v mpeg4 -q:v 5 -threads 4 -f null -
