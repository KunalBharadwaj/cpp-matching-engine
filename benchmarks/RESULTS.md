# Benchmark results

Per-operation latency of the two order-book implementations under an
identical workload. The naive `OrderBook` (`std::map` + `std::list`) is the
correctness oracle; `FastOrderBook` is the price-ladder + object-pool +
intrusive-list design. Both are differentially tested to produce identical
output, so this measures *only* the data-structure difference.

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
- **Identical trade count** across both books is printed as a cross-check.
- **Release build, `-O3 -DNDEBUG`, no sanitizers.**
- **Clock-read overhead is reported** (measured each run): `steady_clock::now()`
  costs ~20 ns here, which is the noise floor. The fast book's p50s (~50–90 ns)
  are close to that floor, so its *true* op latency is smaller than the raw
  number — the measurement is partly clock overhead, not book work.

## Numbers

Machine: 12th Gen Intel Core i5-1235U · g++ 13.3 · Linux · `-O3 -DNDEBUG`.
Headline run: `./build/bench 200000 42` (clock overhead ≈ 18 ns, both books
booked 119,563 trades). Latencies in nanoseconds.

| op     | book   |    p50 |     p90 |     p99 |   p99.9 |
|--------|--------|-------:|--------:|--------:|--------:|
| add    | oracle |    112 |     199 |     389 |   2,431 |
| add    | fast   |     86 |     178 |     306 |     469 |
| cancel | oracle | 46,069 | 120,738 | 189,018 | 561,183 |
| cancel | fast   |     56 |     135 |     290 |     529 |
| modify | oracle | 51,623 | 125,263 | 189,555 | 541,061 |
| modify | fast   |     55 |     167 |     364 |     598 |

Overall throughput: **0.06 M ops/s (oracle) vs 7.44 M ops/s (fast) — ~124×.**
Cancel/modify are ~800× faster at the median and into four figures in the tail.

### Scaling (the real result)

The speedup grows with book depth, because the oracle's cancel/modify are
O(book size) while the fast book's are O(1). `cancel` p50 and throughput:

| ops     | oracle cancel p50 | fast cancel p50 | oracle thru | fast thru   |
|---------|------------------:|----------------:|------------:|------------:|
| 50,000  |          4,773 ns |           41 ns | 0.50 M/s    | 8.67 M/s    |
| 100,000 |         14,515 ns |           42 ns | 0.17 M/s    | 8.94 M/s    |
| 200,000 |         46,069 ns |           56 ns | 0.06 M/s    | 7.44 M/s    |

Oracle latency climbs ~linearly with the book; the fast book is flat.

## Why the gap looks like this

- **add** is only modestly faster: both do O(log n) / O(1) price lookup and a
  list push; the ladder wins on cache locality and by avoiding tree rebalancing
  and per-node `malloc`, but add was never the bottleneck.
- **cancel / modify** is where the design pays off. The oracle has no handle to
  a resting order, so it *scans* — O(book size) — and the cost grows as the
  book deepens (visible in the rising oracle numbers at larger `n`). The fast
  book goes straight to the node via the `OrderId → pool-index` registry and
  unlinks in O(1) from the intrusive doubly-linked list. Flat vs. size-
  dependent — that is the headline.

## Caveats / next steps

- Single-threaded, single-instrument, synthetic flow. A real tape (e.g. NASDAQ
  ITCH / LOBSTER) via a replay parser would harden the claim.
- The registry is `std::unordered_map`; an open-addressing flat hash map would
  cut the cancel/modify constant further (cache-friendlier probes).
- The fast book's p50 is near the `steady_clock` overhead floor; `rdtsc` or
  batched timing would measure sub-clock-overhead latencies more precisely.
