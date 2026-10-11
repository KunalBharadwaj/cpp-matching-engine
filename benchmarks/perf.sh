#!/usr/bin/env bash
#
# Hardware profiling for the matching engine benchmark: cache-miss / branch-miss
# / IPC counters via `perf stat`, and an optional flamegraph via `perf record`.
#
# Why this isn't baked into CI or the headline numbers: `perf` needs kernel
# performance-counter access, which most sandboxes (and this repo's CI) don't
# grant. Run it yourself on a real box:
#
#     sudo sysctl kernel.perf_event_paranoid=1      # once, to allow user events
#     benchmarks/perf.sh [num_ops] [seed] [core] [book]
#
# book: 1=naive  2=indexed  3=fast (default 3). Profiling one book in isolation
# is why bench.cpp takes a book selector.
set -euo pipefail
cd "$(dirname "$0")/.."

N="${1:-200000}"; SEED="${2:-42}"; CORE="${3:-2}"; BOOK="${4:-3}"
BIN=build/bench

# Release build (perf on a -O0 binary is meaningless).
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build --target bench -j >/dev/null

if ! command -v perf >/dev/null; then
    echo "perf not installed (try: sudo apt-get install linux-tools-\$(uname -r))"; exit 1
fi
PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 99)
if [ "$PARANOID" -gt 1 ]; then
    echo "perf_event_paranoid=$PARANOID blocks user counters."
    echo "Run:  sudo sysctl kernel.perf_event_paranoid=1   then retry."; exit 1
fi

echo "=== perf stat: book $BOOK, $N ops, core $CORE ==="
perf stat -e task-clock,cycles,instructions,cache-references,cache-misses,branch-instructions,branch-misses \
    taskset -c "$CORE" "./$BIN" "$N" "$SEED" "$CORE" "$BOOK"

# Flamegraph (optional): needs Brendan Gregg's FlameGraph scripts on PATH.
if command -v stackcollapse-perf.pl >/dev/null && command -v flamegraph.pl >/dev/null; then
    echo "=== perf record -> benchmarks/flamegraph.svg ==="
    perf record -g -o /tmp/engine-perf.data -- taskset -c "$CORE" "./$BIN" "$N" "$SEED" "$CORE" "$BOOK"
    perf script -i /tmp/engine-perf.data | stackcollapse-perf.pl | flamegraph.pl \
        --title "matching engine (book $BOOK)" > benchmarks/flamegraph.svg
    echo "wrote benchmarks/flamegraph.svg"
else
    echo "(FlameGraph scripts not on PATH — skipping flamegraph."
    echo " git clone https://github.com/brendangregg/FlameGraph and add it to PATH.)"
fi
