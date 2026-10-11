// Phase 4 latency benchmark: drive the naive OrderBook (oracle) and the
// FastOrderBook through an IDENTICAL, realistic workload and report
// per-operation latency percentiles. The whole point is to turn "the fast
// book is faster" into numbers -- especially O(1) cancel/modify vs the
// oracle's scans, and the price-ladder match vs the std::map walk.
//
// Methodology notes live next to the code they justify.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "engine/order.hpp"
#include "engine/order_book.hpp"
#include "engine/fast_order_book.hpp"
#include "engine/execution_sink.hpp"

using Clock = std::chrono::steady_clock;

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
    std::uint64_t count = 0, p50 = 0, p99 = 0, p999 = 0, max = 0;
    double mean = 0.0;
};

static Dist summarize(std::vector<std::uint64_t>& v) {
    Dist d;
    d.count = v.size();
    if (v.empty()) return d;
    std::sort(v.begin(), v.end());
    auto at = [&](double p) { return v[static_cast<std::size_t>(p * (v.size() - 1))]; };
    d.p50 = at(0.50); d.p99 = at(0.99); d.p999 = at(0.999); d.max = v.back();
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
                auto t0 = Clock::now();
                book.process(o, sink);
                auto t1 = Clock::now();
                add_ns.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                break;
            }
            case BenchOp::Kind::Cancel: {
                auto t0 = Clock::now();
                book.cancelOrder(op.target);
                auto t1 = Clock::now();
                cancel_ns.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                break;
            }
            case BenchOp::Kind::Modify: {
                auto t0 = Clock::now();
                book.modifyOrder(op.target, op.new_price, op.new_qty, sink);
                auto t1 = Clock::now();
                modify_ns.push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
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

// Rough cost of a single Clock::now() read -- the noise floor under which the
// per-op numbers below cannot be trusted.
static double clock_overhead_ns() {
    constexpr int N = 200'000;
    std::vector<std::uint64_t> d; d.reserve(N);
    for (int i = 0; i < N; ++i) {
        auto a = Clock::now();
        auto b = Clock::now();
        d.push_back(static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
    }
    std::sort(d.begin(), d.end());
    return static_cast<double>(d[d.size() / 2]);   // median
}

static void print_dist(const char* label, const Dist& d) {
    std::printf("  %-8s n=%-9llu mean=%7.1f  p50=%4llu  p99=%6llu  p99.9=%7llu  max=%8llu  (ns)\n",
                label,
                static_cast<unsigned long long>(d.count), d.mean,
                static_cast<unsigned long long>(d.p50),
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

    std::printf("workload: %zu ops, seed %llu, mid %lld +/- ~%.0f ticks\n",
                n, static_cast<unsigned long long>(s),
                static_cast<long long>(MID), SPREAD);
    std::printf("clock overhead (median of back-to-back now()): %.1f ns\n\n",
                clock_overhead_ns());

    std::vector<BenchOp> ops = generate(n, s);

    // Fresh book per run; pool pre-sized to n so grow() never fires mid-timing.
    {
        OrderBook oracle;
        Result r = run(oracle, ops);
        print_result("OrderBook (oracle: std::map + std::list)", r, n);
    }
    {
        FastOrderBook fast(MIN_PRICE, MAX_PRICE, TICK, n);
        Result r = run(fast, ops);
        print_result("FastOrderBook (ladder + pool + intrusive list)", r, n);
    }
    return 0;
}
