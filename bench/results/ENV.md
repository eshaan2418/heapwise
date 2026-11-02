# Benchmark environment (2026-11-02T04:35Z)

- Hardware: Apple M5 Pro, 18 cores, 48 GB RAM, cache line 128 B
- OS: macOS 27.0.1 (26A434)
- Compiler: Apple clang version 21.0.0 (clang-2100.4.1.1)
- mimalloc 3.6.1, jemalloc 5.4.0 (Homebrew bottles)
- Traced programs: CPython 3.12.14, SQLite 3.53.4, jq-1.8.2, ffmpeg 9.0.2
- Load average at start: { 22.86 19.12 14.26 }
- run.sh waited for load < 3 before every trace-replay repetition; the load at start was from unrelated jobs.
