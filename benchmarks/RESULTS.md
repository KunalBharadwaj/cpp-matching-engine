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
./build/bench [num_ops] [seed]      # default: 200000 ops, seed 42
```

## Methodology

- **Workload pre-generated** into a vector before timing, so generation cost
  is never inside the measured region. Mix: ~70% limit adds (≈5% marketable)
  clustered around a mid with a ~40-tick stddev, 15% cancels, 15% modifies;
  cancel/modify target previously-seen ids (some already gone — realistic).
- **Each op timed individually** with `steady_clock`; latencies stored in a
  pre-reserved vector and percentiled *after* the run.
- **Broken down by op kind** — a blended average would hide the cancel story.
- **Counting sink** (no allocation) so we time the book, not bookkeeping.
- **Identical trade count** across all three books is printed as a cross-check.
- **Release build, `-O3 -DNDEBUG`, no sanitizers.**
- **Clock-read overhead is reported** (measured each run): `steady_clock::now()`
  costs ~25 ns here, which is the noise floor. The fast book's p50s (~45–70 ns)
  are close to that floor, so its *true* op latency is smaller than the raw
  number — the measurement is partly clock overhead, not book work.

## Numbers

Machine: 12th Gen Intel Core i5-1235U · g++ 13.3 · Linux · `-O3 -DNDEBUG`.
Headline run: `./build/bench 200000 42` (clock overhead ≈ 26 ns; all three
books booked 119,563 trades). Latencies in nanoseconds.

| op     | book    |    p50 |     p90 |     p99 |   p99.9 |
|--------|---------|-------:|--------:|--------:|--------:|
| add    | naive   |    110 |     194 |     358 |   2,546 |
| add    | indexed |    113 |     191 |     317 |   1,289 |
| add    | fast    |     70 |     144 |     245 |     355 |
| cancel | naive   | 44,856 | 126,594 | 156,731 | 207,629 |
| cancel | indexed |     46 |     141 |     235 |     393 |
| cancel | fast    |     46 |      97 |     179 |     358 |
| modify | naive   | 52,717 | 128,507 | 166,753 | 219,604 |
| modify | indexed |     47 |     201 |     388 |     558 |
| modify | fast    |     46 |     126 |     258 |     422 |

Throughput: **naive 0.06 · indexed 7.72 · fast 9.21 M ops/s.**
`naive → indexed` removes the O(n) cancel scan (~1000×, table stakes);
`indexed → fast` is the cache-friendly structures' real gain — ~1.2×
throughput and tighter tails (cancel p90 141→97, add p50 113→70).

### Scaling (the real result)

The naive cancel grows with book depth (O(book size)) while the indexed and
fast books stay flat. `cancel` p50 and throughput:

| ops     | naive cancel p50 | indexed cancel p50 | fast cancel p50 | naive thru | fast thru |
|---------|-----------------:|-------------------:|----------------:|-----------:|----------:|
| 50,000  |         6,302 ns |             42 ns  |          44 ns  | 0.46 M/s   | 8.07 M/s  |
| 100,000 |        12,914 ns |             43 ns  |          44 ns  | 0.19 M/s   | 8.63 M/s  |
| 200,000 |        44,856 ns |             46 ns  |          46 ns  | 0.06 M/s   | 9.21 M/s  |

The naive scan climbs ~linearly with the book; indexed and fast stay flat
(and essentially tied on cancel median — the index is what removes the scan).

## Why the gap looks like this

- **naive → indexed** is purely algorithmic: the oracle has no handle to a
  resting order, so cancel/modify *scan* — O(book size), rising with depth. An
  `OrderId → location` index removes the scan (~1000×). Any competent book does
  this; it is the baseline, not the achievement.
- **indexed → fast** is what the ladder + pool + intrusive list actually buy,
  on top of an already-indexed book. On `cancel` *median* it's a tie — both are
  ~constant time. The fast book wins on `add` (~1.6× p50), on throughput
  (~1.2×), and on the **tail** (cancel p90 141→97 ns): contiguous ladder memory
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
