#!/usr/bin/env bash
# Record hardware and software versions next to benchmark results.
set -uo pipefail
echo "# Benchmark environment ($(date -u +%Y-%m-%dT%H:%MZ))"
echo
if [[ "$(uname)" == Darwin ]]; then
  echo "- Hardware: $(sysctl -n machdep.cpu.brand_string), $(sysctl -n hw.ncpu) cores, $(( $(sysctl -n hw.memsize) / 1073741824 )) GB RAM, cache line $(sysctl -n hw.cachelinesize) B"
  echo "- OS: macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
else
  echo "- Hardware: $(lscpu | sed -n 's/Model name: *//p'), $(nproc) cores"
  echo "- OS: $(uname -sr)"
fi
echo "- Compiler: $(c++ --version | head -1)"
command -v brew >/dev/null && echo "- mimalloc $(brew list --versions mimalloc | awk '{print $2}'), jemalloc $(brew list --versions jemalloc | awk '{print $2}') (Homebrew bottles)"
echo "- Traced programs: CPython $($(uv python find 3.12 2>/dev/null) -c 'import platform; print(platform.python_version())'), SQLite $($(brew --prefix sqlite 2>/dev/null)/bin/sqlite3 --version 2>/dev/null | awk '{print $1}'), $(jq --version 2>/dev/null), ffmpeg $(ffmpeg -version 2>/dev/null | head -1 | awk '{print $3}')"
echo "- Load average at start: $(sysctl -n vm.loadavg 2>/dev/null || cat /proc/loadavg)"
