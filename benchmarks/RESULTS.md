# Benchmark results

Per-operation latency of three order-book implementations under an identical
workload. `OrderBook` (`std::map` + `std::list`, O(n) cancel) is the naive
oracle; `IndexedOrderBook` is the same map **plus an id index** (the fair
baseline, O(log n) cancel); `FastOrderBook` is the price-ladder + object-pool +
intrusive-list design (O(1) cancel). All three are differentially tested to
produce identical output, so this measures *only* the data-structure
difference — and separates the algorithmic fix from the cache-level one.

## How to reproduce

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench
./build/bench [num_ops] [seed] [core]   # default: 200000 ops, seed 42, core 0
# published numbers below use core 2 (a P-core on this hybrid CPU)
```

## Methodology

- **Workload pre-generated** into a vector before timing, so generation cost
  is never inside the measured region. Mix: ~70% limit adds (≈5% marketable)
  clustered around a mid with a ~40-tick stddev, 15% cancels, 15% modifies;
  cancel/modify target previously-seen ids (some already gone — realistic).
- **Each op timed individually** with `rdtsc` (below); latencies stored in a
  pre-reserved vector and percentiled *after* the run.
- **Broken down by op kind** — a blended average would hide the cancel story.
- **Counting sink** (no allocation) so we time the book, not bookkeeping.
- **Identical trade count** across all three books is printed as a cross-check.
- **Release build, `-O3 -DNDEBUG`, no sanitizers.**
- **Timed with `rdtsc`**, serialized with `lfence`/`rdtscp` so out-of-order
  execution can't smear the window, and calibrated to ns against `steady_clock`
  once at startup. Overhead ~8 ns here (printed each run) — low enough that a
  ~28 ns cancel is resolved, not drowned in timer noise.
- **Pinned to a P-core** (`sched_setaffinity`) so the thread isn't migrated
  mid-run. On a hybrid CPU, pin a low-numbered P-core, *not* the last core
  (an E-core) — that would measure the slow cores. Turbo left on, so tails
  would tighten further under full isolation (`isolcpus`, fixed clock).

## Numbers

Machine: 12th Gen Intel Core i5-1235U · g++ 13.3 · Linux · `-O3 -DNDEBUG`.
Headline run: `./build/bench 200000 42 2` (rdtsc timing, ~8 ns overhead, pinned
to core 2; all three books booked 119,563 trades). Latencies in nanoseconds.

| op     | book    |    p50 |     p90 |     p99 |   p99.9 |
|--------|---------|-------:|--------:|--------:|--------:|
| add    | naive   |     80 |     149 |     297 |   2,411 |
| add    | indexed |     84 |     146 |     260 |   1,892 |
| add    | fast    |     48 |     104 |     187 |     326 |
| cancel | naive   | 35,178 |  97,496 | 126,876 | 193,107 |
| cancel | indexed |     30 |     104 |     183 |   2,064 |
| cancel | fast    |     28 |      70 |     175 |     482 |
| modify | naive   | 39,575 |  99,616 | 135,893 | 191,358 |
| modify | indexed |     31 |     160 |     314 |   2,314 |
| modify | fast    |     28 |      93 |     225 |     578 |

Throughput: **naive 0.07 · indexed 9.14 · fast 12.19 M ops/s.**
`naive → indexed` removes the O(n) cancel scan (~1170×, table stakes);
`indexed → fast` is the cache-friendly structures' real gain — ~1.3×
throughput and tighter tails (cancel p90 104→70, add p50 84→48).

### Scaling (the real result)

The naive cancel grows with book depth (O(book size)) while the indexed and
fast books stay flat. `cancel` p50 and throughput:

| ops     | naive cancel p50 | indexed cancel p50 | fast cancel p50 | naive thru | fast thru  |
|---------|-----------------:|-------------------:|----------------:|-----------:|-----------:|
| 50,000  |         3,720 ns |             22 ns  |          22 ns  | 0.66 M/s   | 12.09 M/s  |
| 100,000 |        10,644 ns |             26 ns  |          26 ns  | 0.24 M/s   | 11.74 M/s  |
| 200,000 |        35,178 ns |             30 ns  |          28 ns  | 0.07 M/s   | 12.19 M/s  |

The naive scan climbs ~linearly with the book; indexed and fast stay flat
(and essentially tied on cancel median — the index is what removes the scan).

## Why the gap looks like this

- **naive → indexed** is purely algorithmic: the oracle has no handle to a
  resting order, so cancel/modify *scan* — O(book size), rising with depth. An
  `OrderId → location` index removes the scan (~1000×). Any competent book does
  this; it is the baseline, not the achievement.
- **indexed → fast** is what the ladder + pool + intrusive list actually buy,
  on top of an already-indexed book. On `cancel` *median* it's a tie — both are
  ~constant time. The fast book wins on `add` (~1.75× p50), on throughput
  (~1.3×), and on the **tail** (cancel p90 104→70 ns): contiguous ladder memory
  (no tree pointer-chase or rebalancing), an object pool (no per-order
  `malloc`), and an intrusive list (no node allocation) make its worst cases
  far less ragged. Tighter tails are the point in low-latency work.

## Caveats / next steps

- Single-threaded, single-instrument, synthetic flow. A real tape (e.g. NASDAQ
  ITCH / LOBSTER) via a replay parser would harden the claim.
- The registry is `std::unordered_map`; an open-addressing flat hash map would
  cut the cancel/modify constant further (cache-friendlier probes).
- The fast book's p50 is near the `steady_clock` overhead floor; `rdtsc` or
  batched timing would measure sub-clock-overhead latencies more precisely.
