# Low-Latency Matching Engine (C++20)

[![CI](https://github.com/KunalBharadwaj/cpp-matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/KunalBharadwaj/cpp-matching-engine/actions/workflows/ci.yml)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License: MIT](https://img.shields.io/badge/License-MIT-green)

A single-instrument limit-order-book and matching engine with **price-time
(FIFO) priority**, built twice: once the obvious, correct way, and once the
fast way — then the fast build is proven identical to the correct one by
differential testing and measured against it.

The point isn't just that it matches orders. It's that every performance
decision is **deliberate, explained, and benchmarked against a *fair*
baseline** — not a strawman. Removing an O(n) cancel scan is ~1000× (table
stakes, and I say so); the cache-friendly data structures on top then buy
~1.3× throughput and markedly tighter tail latency — the part that actually
matters in a low-latency system.

> Single-threaded hot path · header-only engine · C++20 · GoogleTest ·
> clean under AddressSanitizer + UBSan.

---

## Results at a glance

**Three** implementations of the same book, driven through one seeded workload
(200,000 ops: ~70% adds / 15% cancels / 15% modifies, prices around a mid) on a
12th-gen Intel i5-1235U, g++ 13.3, `-O3 -DNDEBUG`, **pinned to a P-core and
timed per-op with `rdtsc`** (timer overhead ~8 ns). All three are
**differentially tested to emit identical trades** (119,563 here), so the
numbers isolate the data structure and nothing else:

| # | Book | cancel/modify | what it changes |
|---|------|:-------------:|-----------------|
| 1 | `OrderBook`        | **O(n)** scan  | naive `std::map` + `std::list` |
| 2 | `IndexedOrderBook` | **O(log n)**   | same map, **plus an `OrderId → location` index** |
| 3 | `FastOrderBook`    | **O(1)**       | price ladder + object pool + intrusive list |

Separating the two effects matters, because it's easy to quote one misleading
headline number. There are really two results:

### Result 1 — don't scan the book (naive → indexed)

Cancel p50 drops **35,178 ns → 30 ns (~1170×)** just by keeping an id index
instead of scanning. This is the number that *looks* huge — but it's table
stakes: any competent implementation does it. The naive book is a deliberately
simple oracle, so I treat its O(n) cancel as the baseline, **not** as the win.
The trap it avoids, as the book deepens:

<p align="center">
  <img src="benchmarks/latency-scaling.svg" alt="Cancel p50 vs book size: the naive O(n) scan climbs from ~4µs to 35µs as the book grows, while the indexed and fast books stay flat near 25ns" width="680">
</p>

### Result 2 — what the cache-friendly structures actually buy (indexed → fast)

This is the honest comparison: both already cancel in ~constant time, so it
measures the **ladder + object pool + intrusive list alone**. They don't beat
the index on cancel *median* — they win on **throughput and tail latency**,
which is what a low-latency system actually cares about:

| op     | metric | Indexed (map + index) | **Fast** (ladder + pool) |  gain |
|--------|--------|----------------------:|-------------------------:|------:|
| add    | p50    |                 84 ns |                **48 ns** | 1.75× |
| add    | p99    |                260 ns |               **187 ns** | 1.4×  |
| cancel | p50    |                 30 ns |                **28 ns** | ~tie  |
| cancel | p90    |                104 ns |                **70 ns** | 1.5×  |
| cancel | p99    |                183 ns |               **175 ns** | ~tie  |
| modify | p90    |                160 ns |                **93 ns** | 1.7×  |

**Throughput: 9.14 → 12.19 M ops/s (~1.3×).**

Why: the contiguous ladder avoids the tree's pointer-chasing and rebalancing,
the object pool removes a `malloc`/`free` per resting order, and the intrusive
list removes the per-node allocation. Those shave the mean and — more
importantly — **compress the tail**; they don't change the O(1) cancel the
index already gave. Modest and honest beats a cherry-picked 1000×.

---

## Try it in 30 seconds

```sh
cmake -S . -B build && cmake --build build --target engine
./build/engine
```

A narrated demo walks a live book through every operation — resting orders, a
crossing limit, a market order sweeping levels, cancel, modify (both the
priority-keeping reduce and the repricing cross), and IOC/FOK — printing the
trades and the book ladder after each step:

```
[12] NEW  #9  BUY     4 @ 105  buy 4 @ 105: #7 (3) fills first, then #8 (1) -- priority proven
      → TRADE     3 @ 105    (maker #7  taker #9)
      → TRADE     1 @ 105    (maker #8  taker #9)
           price    bid    ask
             105             4
        --------------------------
        best bid -   best ask 105
```

---

## What it does

Maintains an order book and matches incoming aggressive orders against resting
liquidity by price-then-time priority, emitting a stream of trades. Supported:

- **Limit** orders — match on cross, rest the remainder.
- **Market** orders — sweep best-available, never rest.
- **Cancel** by order id.
- **Modify (cancel-replace)** — same-price size *decrease* keeps queue
  priority (shrink in place); a reprice or size *increase* goes to the back and
  may cross immediately.
- **Time-in-force**: **GTC**, **IOC** (fill now, drop remainder),
  **FOK** (fill completely now or do nothing).

Core invariant held after every operation: the book is never crossed, and
quantity is conserved across resting + filled.

---

## Architecture

The pipeline is split into seams so the slow parts can be swapped for fast ones
without touching the rest:

```
EventSource ──▶ OrderBook / FastOrderBook ──▶ ExecutionSink
 (ingress)          (match + rest)              (trades out)
```

- **`EventSource`** — abstract order ingress (a vector replayer today; a file
  or network feed could drop in behind the same interface).
- **`OrderBook`** — the naive reference implementation (`std::map`+`std::list`),
  the correctness oracle.
- **`IndexedOrderBook`** — the fair baseline: same map, plus an id index so
  cancel/modify don't scan.
- **`FastOrderBook`** — the optimized implementation (below).
- **`ExecutionSink`** — abstract trade/report output (`RecordingSink` for tests,
  a counting sink for benchmarks).

The hot path stays free of heap allocation and avoids virtual dispatch inside
the book (order type and side are enum branches, not polymorphism).

---

## Design: why the fast book is fast

Three changes, each with a tradeoff worth defending in an interview.

### 1. Price levels → flat "price ladder"

Prices are discrete ticks in a bounded range, so index straight into an array:

```
idx = (price - min_price) / tick_size
```

A `vector<PriceLevel>` per side gives **O(1)** access to any level, contiguous
in memory, versus the map's O(log n) red-black-tree pointer-chase. Two integer
cursors, `best_bid_idx` / `best_ask_idx`, replace what `map::begin()` gave the
naive book and are advanced when a level empties.

**Tradeoff:** memory is proportional to the price *range*, not the order count,
and cursor advance across a run of empty levels is amortized, not worst-case
O(1). Fine for one instrument with realistic tick bounds; a very wide/sparse
range would want a hash-of-levels instead.

### 2. Orders within a level → object pool + intrusive doubly-linked FIFO

Resting orders live in one preallocated `vector<PoolNode>` with a free list —
**no `malloc` on the hot path**. The FIFO links (`next`/`prev`) are *intrusive*
(inside the node) and stored as **pool indices, not pointers**, so the pool can
grow and reallocate without dangling, and each link is 4 bytes instead of 8.

**Doubly-**linked is a deliberate choice: it makes unlink **O(1)**, which is the
hook the next change relies on.

Layout is kept tight on purpose: `Order` is 40 bytes (the three one-byte enums
— side, type, time-in-force — fit in the trailing padding, costing nothing),
and a pool node is 48 bytes (`Order` + two 4-byte index links). Indices rather
than 8-byte pointers keep the links small and reallocation-safe.

### 3. Order registry → `OrderId → pool index`

A hash map from order id to pool index means cancel/modify go *straight* to the
node and unlink in O(1) — no scanning. (The `IndexedOrderBook` baseline does the
same thing on a `std::map`, which is why the *honest* win over it is modest —
see Results. The ~1000× over the naive scan is just not scanning.)

**Honest note:** this uses `std::unordered_map` today. An open-addressing flat
hash map (`ankerl::unordered_dense`, Abseil `flat_hash_map`) would cut the
constant further by being cache-friendlier; it's the first item on the roadmap.

---

## Correctness

A fast engine that fills wrong is worthless, so correctness is enforced first:

- **Differential testing** — the headline technique. A 5,000-operation random
  stream (limit/market, GTC/IOC/FOK, cancel, modify) is fed to the naive oracle
  and asserted, after *every* operation, to produce **identical trades and
  top-of-book** in both the `IndexedOrderBook` baseline and the `FastOrderBook`.
  Each fast implementation's whole reason to exist is to match the oracle
  exactly; this proves both do.
- **Targeted unit tests** for each order type and the nasty edges: partial
  fills, market-into-empty-book, IOC dropping its remainder, FOK killing on
  insufficient liquidity, modify keeping vs. losing priority, cursor repair on
  cancel, object-pool growth under load.
- **28 GoogleTests**, all green under `-fsanitize=address,undefined`.

---

## Benchmarks

### Methodology (how the numbers above were produced)

- **Workload is pre-generated** into a vector before timing — generation cost
  is never inside the measured region.
- **Each operation is timed individually** with `rdtsc` (see below); latencies
  land in a pre-reserved vector and are percentiled *after* the run.
- **Reported per op kind** (add / cancel / modify) — a blended mean would hide
  the cancel story, which is the whole point.
- **Counting sink** (no allocation, no I/O) so the book is measured, not
  bookkeeping.
- **Trade counts are cross-checked** across all three books every run.
- **Release build, `-O3 -DNDEBUG`, sanitizers off** — ASan inflates latency
  several-fold, so perf numbers never come from the test build.
- **Timed with `rdtsc`, not `steady_clock`.** Per-op latency is read straight
  from the CPU timestamp counter, serialized with `lfence`/`rdtscp` so
  out-of-order execution can't smear the measured window, and calibrated to
  nanoseconds against `steady_clock` once at startup. Its overhead is **~8 ns**
  here — about a third of a `steady_clock::now()` call — so a ~28 ns cancel is
  genuinely resolved instead of lost in timer noise. The overhead is measured
  and printed every run.
- **Pinned to a P-core** via `sched_setaffinity`, so the scheduler can't
  migrate the thread mid-run (migration reloads caches and fattens the tail).
  On this hybrid CPU that means a low-numbered P-core, *not* an E-core — pinning
  to the last core would measure the slow cores. Turbo / frequency scaling is
  still on, so absolute tails would tighten further on a fully isolated box
  (`isolcpus`, fixed clock); the relative comparison is robust regardless.
- **Deterministic & reproducible.** The workload is seeded, so a given
  `(num_ops, seed)` replays byte-identically — the same run every time, which
  is what makes the numbers above reproducible and any regression debuggable.

### Interpreting the gap

- **naive vs. indexed** is the O(book size) scan vs. an indexed lookup. Huge on
  cancel/modify (~1000×), but it's an algorithmic correction any competent
  implementation makes, not a property of the fast data structures.
- **indexed vs. fast** is the real test of the ladder + pool + intrusive list.
  `cancel` median is a tie (both already ~constant time); the fast book wins on
  `add` (~1.75×), on throughput (~1.3×), and — most relevantly — on the **tail**
  (cancel p90 104→70 ns), because contiguous memory, no per-order `malloc`, and
  no tree rebalancing make its worst cases far less ragged than the map's.

Numbers are single-machine and directional, not promises — reproduce them on
your own box with the command below.

---

## Build & run

Requires CMake ≥ 3.20 and a C++20 compiler (tested on g++ 13.3). GoogleTest is
fetched automatically.

```sh
# Tests
cmake -S . -B build
cmake --build build --target unit_tests
ctest --test-dir build --output-on-failure

# Tests under sanitizers (development)
cmake -S . -B build-asan -DENABLE_SANITIZERS=ON
cmake --build build-asan --target unit_tests && ./build-asan/unit_tests

# Benchmark (release-optimized)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target bench
./build/bench [num_ops] [seed] [core]   # default: 200000 ops, seed 42, core 0
# e.g. ./build/bench 200000 42 2        # pin to core 2 (a P-core on hybrid CPUs)
```

See [`benchmarks/RESULTS.md`](benchmarks/RESULTS.md) for the full output and
machine details.

---

## Project layout

```
include/engine/
  types.hpp              OrderId / Price / Quantity, Side, OrderType, TimeInForce
  order.hpp  trade.hpp   core structs
  event_source.hpp       ingress interface + vector replayer
  execution_sink.hpp     trade-output interface + recording sink
  command.hpp  engine.hpp the pull-loop runner
  order_book.hpp         naive reference book (the oracle)
  indexed_order_book.hpp fair baseline: std::map + OrderId->location index
  fast_order_book.hpp    price ladder + object pool + intrusive list + registry
tests/                   GoogleTest suites (incl. the differential tests)
benchmarks/              bench harness + RESULTS.md
```

---

## Roadmap

In rough order of value:

1. **Open-addressing flat hash** for the registry — the next constant-factor win.
2. **Real NASDAQ ITCH replay** — swap synthetic flow for a real tape via an
   ITCH parser, so the numbers come from production order flow.
3. **`rdtsc` / batched timing** to measure below the clock-read floor.
4. Templated engine `run()` over book type to drive both through one loop.
5. **Concurrency (stretch):** split ingress and matching across threads over an
   SPSC lock-free ring buffer; multi-instrument sharding.

---

## License

[MIT](LICENSE).
