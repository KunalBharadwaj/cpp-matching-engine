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
- **Clock-read overhead is reported**: `steady_clock::now()` costs ~40 ns
  here, which is the noise floor. The fast book's p50s (~70–100 ns) are close
  to that floor, so its *true* op latency is smaller than the raw number — the
  measurement is partly clock overhead, not book work.

## Numbers

Machine: 12th Gen Intel Core i5-1235U · g++ 13.3 · Linux · `-O3 -DNDEBUG`.
Headline run: `./build/bench 200000 42` (clock overhead ≈ 33 ns, both books
booked 119,563 trades).

| op     | metric | OrderBook (oracle) | FastOrderBook | speedup |
|--------|--------|-------------------:|--------------:|--------:|
| add    | p50    |            195 ns  |       115 ns  |   1.7×  |
| add    | p99    |          2,442 ns  |       449 ns  |   5.4×  |
| cancel | p50    |         78,260 ns  |        73 ns  |  ~1070× |
| cancel | p99    |        889,912 ns  |       479 ns  |  ~1860× |
| modify | p50    |         89,875 ns  |        73 ns  |  ~1230× |
| modify | p99    |      1,011,937 ns  |       559 ns  |  ~1810× |

Overall throughput: **0.02 M ops/s (oracle) vs 5.13 M ops/s (fast) — ~250×.**

### Scaling (the real result)

The speedup grows with book depth, because the oracle's cancel/modify are
O(book size) while the fast book's are O(1). `cancel` p50 and throughput:

| ops     | oracle cancel p50 | fast cancel p50 | oracle thru | fast thru   |
|---------|------------------:|----------------:|------------:|------------:|
| 50,000  |         11,721 ns |           59 ns | 0.25 M/s    | 5.94 M/s    |
| 100,000 |         34,491 ns |           70 ns | 0.08 M/s    | 5.19 M/s    |
| 200,000 |         78,260 ns |           73 ns | 0.02 M/s    | 5.13 M/s    |

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
