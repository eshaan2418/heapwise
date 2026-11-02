# Traces

`fixture.hwt` is the first 6,000 operations of the SQLite trace (see below), small enough to
commit and replay in CI. Full traces are not committed (264 MB); regenerate them on macOS with

    make traces          # bench/scripts/record_traces.sh build bench/traces

which records CPython 3.12 (`PYTHONMALLOC=malloc`) compiling 400 stdlib modules, SQLite 3.53
building and querying a 200k-row table, jq 1.8 reshaping 40k generated JSON records, and ffmpeg
encoding 30 s of a synthetic 720p test pattern.

Format (`bench/trace_format.h`): a 24-byte header (`HWT1`, version, op count, id count) followed
by 16-byte ops `{kind, align_log2, reserved, id, size}`. Addresses are replaced by dense object ids,
so a trace replays identically under any allocator.
