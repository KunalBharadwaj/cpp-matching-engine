// Phase 4 latency benchmark: drive the naive OrderBook (oracle) and the
// FastOrderBook through an IDENTICAL, realistic workload and report
// per-operation latency percentiles. The whole point is to turn "the fast
// book is faster" into numbers -- especially O(1) cancel/modify vs the
// oracle's scans, and the price-ladder match vs the std::map walk.
//
// Methodology notes live next to the code they justify.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <unistd.h>
#if defined(__x86_64__) || defined(__i386__)
#include <x86intrin.h>
#define HAVE_RDTSC 1
#else
#define HAVE_RDTSC 0
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "engine/order.hpp"
#include "engine/order_book.hpp"
#include "engine/indexed_order_book.hpp"
#include "engine/fast_order_book.hpp"
#include "engine/execution_sink.hpp"

using Clock = std::chrono::steady_clock;

// ---- high-resolution timing via the CPU timestamp counter ------------------
// rdtsc reads the invariant TSC directly -- a few ns of overhead against the
// ~25 ns of a steady_clock::now() call, so the per-op signal isn't drowned by
// the timer. We serialize each read (lfence + rdtscp) so out-of-order
// execution can't smear the window, and calibrate cycles->ns once at startup.

static double g_ticks_per_ns = 1.0;   // set by calibrate_tsc(); == TSC GHz

#if HAVE_RDTSC
// Intel's recommended fencing: lfence before rdtsc (no earlier instruction
// leaks into the window), rdtscp at the end (waits for the measured code to
// retire) followed by lfence (nothing later leaks in).
static inline std::uint64_t tsc_begin() {
    _mm_lfence(); return __rdtsc();
}
static inline std::uint64_t tsc_end() {
    unsigned aux; std::uint64_t t = __rdtscp(&aux); _mm_lfence(); return t;
}
static inline std::uint64_t to_ns(std::uint64_t ticks) {
    return static_cast<std::uint64_t>(ticks / g_ticks_per_ns);
}
#else
static inline std::uint64_t tsc_begin() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}
static inline std::uint64_t tsc_end() { return tsc_begin(); }
static inline std::uint64_t to_ns(std::uint64_t ns) { return ns; }  // already ns
#endif

// Busy-spin a fixed wall interval and divide elapsed TSC ticks by elapsed ns.
static void calibrate_tsc() {
#if HAVE_RDTSC
    auto c0 = Clock::now(); std::uint64_t t0 = tsc_begin();
    while (std::chrono::duration<double>(Clock::now() - c0).count() < 0.20) { /* spin */ }
    std::uint64_t t1 = tsc_end(); auto c1 = Clock::now();
    double ns = std::chrono::duration<double, std::nano>(c1 - c0).count();
    g_ticks_per_ns = static_cast<double>(t1 - t0) / ns;
#endif
}

// Pin this thread to one core so the scheduler can't migrate it mid-run (cache
// reload + ragged tails). Returns the core, or -1 if the OS refused.
static int pin_to_core(int core) {
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(core, &set);
    return (sched_setaffinity(0, sizeof(set), &set) == 0) ? core : -1;
}

// Minimal sink: we are timing the book, not trade bookkeeping. No allocation,
// no I/O -- just enough work that the compiler can't delete the trade.
struct CountingSink : ExecutionSink {
    std::uint64_t count = 0;
    void on_trade(const Trade&) override { ++count; }
};

// ---- ladder / workload configuration --------------------------------------

static constexpr Price MIN_PRICE = 1;
static constexpr Price MAX_PRICE = 100'000;
static constexpr Price TICK      = 1;
static constexpr Price MID       = 50'000;   // orders cluster here
static constexpr double SPREAD   = 40.0;     // stddev of price offset, in ticks

struct BenchOp {
    enum class Kind : std::uint8_t { Add, Cancel, Modify } kind;
    Order order{};        // Add
    OrderId target{};     // Cancel / Modify
    Price new_price{};    // Modify
    Quantity new_qty{};   // Modify
};

// Generate a mixed stream: mostly resting limit adds clustered around a mid,
// a few marketable orders to keep the book shallow, plus cancels and modifies
// that target previously-seen ids (some already gone -> realistic misses).
static std::vector<BenchOp> generate(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::normal_distribution<double>        offset(0.0, SPREAD);
    std::uniform_int_distribution<Quantity> qty(1, 100);
    std::uniform_int_distribution<int>      side(0, 1);
    std::uniform_int_distribution<int>      action(0, 99);

    std::vector<BenchOp> ops;
    ops.reserve(n);
    std::vector<OrderId> live;              // ids we have added (may be stale)
    live.reserve(n);
    OrderId next_id = 1;

    auto pick_price = [&]() -> Price {
        long p = MID + static_cast<long>(std::lround(offset(rng)));
        if (p < MIN_PRICE) p = MIN_PRICE;
        if (p > MAX_PRICE) p = MAX_PRICE;
        return static_cast<Price>(p);
    };

    for (std::size_t i = 0; i < n; ++i) {
        int a = action(rng);
        if (a < 15 && !live.empty()) {                 // 15% cancel
            OrderId t = live[std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng)];
            ops.push_back({BenchOp::Kind::Cancel, {}, t, 0, 0});
        } else if (a < 30 && !live.empty()) {          // 15% modify
            OrderId t = live[std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng)];
            ops.push_back({BenchOp::Kind::Modify, {}, t, pick_price(), qty(rng)});
        } else {                                        // 70% add
            bool market = (a >= 95);                    // ~5% of adds are market
            Order o{
                .order_id = next_id,
                .price = market ? 0 : pick_price(),
                .original_quantity = qty(rng),
                .remaining_quantity = 0,                // set below to match original
                .side = side(rng) ? Side::Buy : Side::Sell,
                .order_type = market ? OrderType::Market : OrderType::Limit,
                .time_in_force = TimeInForce::GTC,
            };
            o.remaining_quantity = o.original_quantity;
            ops.push_back({BenchOp::Kind::Add, o, 0, 0, 0});
            live.push_back(next_id);
            ++next_id;
        }
    }
    return ops;
}

// ---- stats -----------------------------------------------------------------

struct Dist {
    std::uint64_t count = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, max = 0;
    double mean = 0.0;
};

static Dist summarize(std::vector<std::uint64_t>& v) {
    Dist d;
    d.count = v.size();
    if (v.empty()) return d;
    std::sort(v.begin(), v.end());
    auto at = [&](double p) { return v[static_cast<std::size_t>(p * (v.size() - 1))]; };
    d.p50 = at(0.50); d.p90 = at(0.90); d.p99 = at(0.99); d.p999 = at(0.999); d.max = v.back();
    unsigned long long sum = 0; for (auto x : v) sum += x;
    d.mean = static_cast<double>(sum) / v.size();
    return d;
}

struct Result {
    double wall_s = 0.0;
    std::uint64_t trades = 0;
    Dist add, cancel, modify;
};

// Timed driver. Templated so both books -- which share process / cancelOrder /
// modifyOrder signatures -- run through exactly the same code.
template <class Book>
static Result run(Book& book, const std::vector<BenchOp>& ops) {
    CountingSink sink;
    std::vector<std::uint64_t> add_ns, cancel_ns, modify_ns;
    add_ns.reserve(ops.size()); cancel_ns.reserve(ops.size()); modify_ns.reserve(ops.size());

    auto wall0 = Clock::now();
    for (const BenchOp& op : ops) {
        switch (op.kind) {
            case BenchOp::Kind::Add: {
                Order o = op.order;                 // process() mutates remaining_quantity
                std::uint64_t t0 = tsc_begin();
                book.process(o, sink);
                std::uint64_t t1 = tsc_end();
                add_ns.push_back(to_ns(t1 - t0));
                break;
            }
            case BenchOp::Kind::Cancel: {
                std::uint64_t t0 = tsc_begin();
                book.cancelOrder(op.target);
                std::uint64_t t1 = tsc_end();
                cancel_ns.push_back(to_ns(t1 - t0));
                break;
            }
            case BenchOp::Kind::Modify: {
                std::uint64_t t0 = tsc_begin();
                book.modifyOrder(op.target, op.new_price, op.new_qty, sink);
                std::uint64_t t1 = tsc_end();
                modify_ns.push_back(to_ns(t1 - t0));
                break;
            }
        }
    }
    auto wall1 = Clock::now();

    Result r;
    r.wall_s = std::chrono::duration<double>(wall1 - wall0).count();
    r.trades = sink.count;
    r.add = summarize(add_ns);
    r.cancel = summarize(cancel_ns);
    r.modify = summarize(modify_ns);
    return r;
}

// Median cost of one back-to-back timer read -- the noise floor under which the
// per-op numbers cannot be trusted. Measured with the same timer the run uses.
static double timer_overhead_ns() {
    constexpr int N = 200'000;
    std::vector<std::uint64_t> d; d.reserve(N);
    for (int i = 0; i < N; ++i) {
        std::uint64_t a = tsc_begin();
        std::uint64_t b = tsc_end();
        d.push_back(b - a);
    }
    std::sort(d.begin(), d.end());
    return static_cast<double>(d[d.size() / 2]) / g_ticks_per_ns;   // ticks -> ns
}

static void print_dist(const char* label, const Dist& d) {
    std::printf("  %-8s n=%-9llu mean=%7.1f  p50=%4llu  p90=%5llu  p99=%6llu  p99.9=%7llu  max=%8llu  (ns)\n",
                label,
                static_cast<unsigned long long>(d.count), d.mean,
                static_cast<unsigned long long>(d.p50),
                static_cast<unsigned long long>(d.p90),
                static_cast<unsigned long long>(d.p99),
                static_cast<unsigned long long>(d.p999),
                static_cast<unsigned long long>(d.max));
}

static void print_result(const char* name, const Result& r, std::size_t n) {
    std::printf("%s\n", name);
    std::printf("  throughput: %.2f M ops/s   wall=%.3fs   trades=%llu\n",
                n / r.wall_s / 1e6, r.wall_s, static_cast<unsigned long long>(r.trades));
    print_dist("add", r.add);
    print_dist("cancel", r.cancel);
    print_dist("modify", r.modify);
    std::printf("\n");
}

int main(int argc, char** argv) {
    // Default kept modest because the oracle's cancel/modify are O(book size):
    // at ~1e6 ops it takes minutes. Pass a larger n on the command line to
    // stress the fast book alone (it handles 1e6 in well under a second).
    std::size_t n   = (argc > 1) ? std::stoull(argv[1]) : 200'000;
    std::uint64_t s = (argc > 2) ? std::stoull(argv[2]) : 42;
    // argv[3] = core to pin to. Default 0: on a hybrid CPU (P + E cores) the
    // low-numbered cores are the fast P-cores, so pin there, not the last core.
    int core_req   = (argc > 3) ? std::stoi(argv[3]) : 0;
    // argv[4] = which book to run (0 = all three; 1/2/3 = just that one). Used
    // by perf.sh so `perf stat` can profile a single implementation in isolation.
    int book_sel   = (argc > 4) ? std::stoi(argv[4]) : 0;

    // Pin to a core and calibrate the timer before measuring anything.
    int core = pin_to_core(core_req);
    calibrate_tsc();

    std::printf("workload: %zu ops, seed %llu, mid %lld +/- ~%.0f ticks\n",
                n, static_cast<unsigned long long>(s),
                static_cast<long long>(MID), SPREAD);
#if HAVE_RDTSC
    std::printf("timing: rdtsc (TSC ~%.3f GHz), overhead ~%.1f ns/op; ",
                g_ticks_per_ns, timer_overhead_ns());
#else
    std::printf("timing: steady_clock, overhead ~%.1f ns/op; ", timer_overhead_ns());
#endif
    if (core >= 0) std::printf("pinned to core %d\n\n", core);
    else           std::printf("NOT pinned (affinity refused)\n\n");

    std::vector<BenchOp> ops = generate(n, s);

    // Fresh book per run; pool pre-sized to n so grow() never fires mid-timing.
    if (book_sel == 0 || book_sel == 1) {
        OrderBook oracle;
        Result r = run(oracle, ops);
        print_result("1. OrderBook  (naive: std::map + std::list, O(n) cancel scan)", r, n);
    }
    if (book_sel == 0 || book_sel == 2) {
        IndexedOrderBook indexed;
        Result r = run(indexed, ops);
        print_result("2. IndexedOrderBook  (std::map + id index, O(log n) cancel)", r, n);
    }
    if (book_sel == 0 || book_sel == 3) {
        FastOrderBook fast(MIN_PRICE, MAX_PRICE, TICK, n);
        Result r = run(fast, ops);
        print_result("3. FastOrderBook  (ladder + pool + intrusive list, O(1) cancel)", r, n);
    }
    return 0;
}
