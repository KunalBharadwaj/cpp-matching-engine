#include "gtest/gtest.h"
#include <random>
#include "engine/order.hpp"
#include "engine/order_book.hpp"
#include "engine/fast_order_book.hpp"
#include "engine/recording_sink.hpp"

// Price range for the ladder in these tests. The random generator below stays
// strictly inside [MIN, MAX] so priceToIndex never goes out of bounds.
static constexpr Price MIN = 1;
static constexpr Price MAX = 1000;
static constexpr Price TICK = 1;

// --- Targeted scenarios (the same cases the oracle is tested on) -----------

TEST(FastOrderBook, PartialFillRestsRemainder) {
    Order sell{ .order_id = 1, .price = 100, .original_quantity = 10,
                .remaining_quantity = 10, .side = Side::Sell, .order_type = OrderType::Limit };
    Order buy{ .order_id = 2, .price = 100, .original_quantity = 4,
               .remaining_quantity = 4, .side = Side::Buy, .order_type = OrderType::Limit };

    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);

    book.process(sell, es);
    book.process(buy, es);

    ASSERT_EQ(1u, sink.trades.size());
    EXPECT_EQ(1u, sink.trades[0].maker);
    EXPECT_EQ(2u, sink.trades[0].taker);
    EXPECT_EQ(100, sink.trades[0].price);
    EXPECT_EQ(4u, sink.trades[0].quantity);
    EXPECT_EQ(100, book.getBestAsk());        // 6 left resting
    EXPECT_EQ(std::nullopt, book.getBestBid());
}

TEST(FastOrderBook, NoCrossBothRest) {
    Order sell{ .order_id = 1, .price = 105, .original_quantity = 5,
                .remaining_quantity = 5, .side = Side::Sell, .order_type = OrderType::Limit };
    Order buy{ .order_id = 2, .price = 100, .original_quantity = 3,
               .remaining_quantity = 3, .side = Side::Buy, .order_type = OrderType::Limit };

    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);

    book.process(sell, es);
    book.process(buy, es);

    ASSERT_EQ(0u, sink.trades.size());
    EXPECT_EQ(100, book.getBestBid());
    EXPECT_EQ(105, book.getBestAsk());
}

TEST(FastOrderBook, MarketWalksMultipleLevels) {
    Order sell1{ .order_id = 1, .price = 100, .original_quantity = 5,
                 .remaining_quantity = 5, .side = Side::Sell, .order_type = OrderType::Limit };
    Order sell2{ .order_id = 2, .price = 101, .original_quantity = 5,
                 .remaining_quantity = 5, .side = Side::Sell, .order_type = OrderType::Limit };
    Order buy{ .order_id = 3, .original_quantity = 8,
               .remaining_quantity = 8, .side = Side::Buy, .order_type = OrderType::Market };

    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);

    book.process(sell1, es);
    book.process(sell2, es);
    book.process(buy, es);

    EXPECT_EQ(2u, sink.trades.size());
    EXPECT_EQ(101, book.getBestAsk());        // level 100 drained, cursor advanced
    EXPECT_EQ(std::nullopt, book.getBestBid());
}

TEST(FastOrderBook, MarketIntoEmptyBookDoesNothing) {
    Order buy{ .order_id = 1, .original_quantity = 5,
               .remaining_quantity = 5, .side = Side::Buy, .order_type = OrderType::Market };

    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);

    book.process(buy, es);

    EXPECT_EQ(0u, sink.trades.size());
    EXPECT_EQ(std::nullopt, book.getBestAsk());
    EXPECT_EQ(std::nullopt, book.getBestBid());
}

TEST(FastOrderBook, CancelRepairsCursor) {
    Order sell{ .order_id = 1, .price = 100, .original_quantity = 5,
                .remaining_quantity = 5, .side = Side::Sell, .order_type = OrderType::Limit };
    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);

    book.process(sell, es);
    EXPECT_EQ(100, book.getBestAsk());
    EXPECT_TRUE(book.cancelOrder(1));
    EXPECT_EQ(std::nullopt, book.getBestAsk());  // cursor must fall back to -1
    EXPECT_FALSE(book.cancelOrder(999));
}

// --- Differential test: FastOrderBook must equal the OrderBook oracle -------

namespace {
// Apply one order to both books through separate sinks; assert the emitted
// trades for THIS order match, then assert top-of-book agrees.
void applyAndCompare(Order order,
                     OrderBook& oracle, FastOrderBook& fast,
                     RecordingSink& oracle_sink, RecordingSink& fast_sink) {
    std::size_t before = oracle_sink.trades.size();
    ASSERT_EQ(before, fast_sink.trades.size());

    Order a = order, b = order;               // each book mutates remaining_quantity
    ExecutionSink& oes = oracle_sink;
    ExecutionSink& fes = fast_sink;
    oracle.process(a, oes);
    fast.process(b, fes);

    ASSERT_EQ(oracle_sink.trades.size(), fast_sink.trades.size());
    for (std::size_t i = before; i < oracle_sink.trades.size(); ++i) {
        EXPECT_EQ(oracle_sink.trades[i], fast_sink.trades[i]);  // Trade::operator==
    }
    EXPECT_EQ(oracle.getBestBid(), fast.getBestBid());
    EXPECT_EQ(oracle.getBestAsk(), fast.getBestAsk());
}
}  // namespace

TEST(FastOrderBook, IocFillsThenDropsRemainder) {
    Order sell{ .order_id = 1, .price = 100, .original_quantity = 4,
                .remaining_quantity = 4, .side = Side::Sell, .order_type = OrderType::Limit };
    Order buy{ .order_id = 2, .price = 100, .original_quantity = 10,
               .remaining_quantity = 10, .side = Side::Buy, .order_type = OrderType::Limit,
               .time_in_force = TimeInForce::IOC };

    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);

    book.process(sell, es);
    book.process(buy, es);

    ASSERT_EQ(1u, sink.trades.size());
    EXPECT_EQ(4u, sink.trades[0].quantity);
    EXPECT_EQ(std::nullopt, book.getBestBid());  // 6 unfilled were dropped, not rested
    EXPECT_EQ(std::nullopt, book.getBestAsk());
}

TEST(FastOrderBook, FokKillsWhenNotEnoughLiquidity) {
    Order sell{ .order_id = 1, .price = 100, .original_quantity = 4,
                .remaining_quantity = 4, .side = Side::Sell, .order_type = OrderType::Limit };
    Order buy{ .order_id = 2, .price = 100, .original_quantity = 10,
               .remaining_quantity = 10, .side = Side::Buy, .order_type = OrderType::Limit,
               .time_in_force = TimeInForce::FOK };

    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);

    book.process(sell, es);
    book.process(buy, es);

    EXPECT_EQ(0u, sink.trades.size());           // all-or-nothing: nothing happened
    EXPECT_EQ(100, book.getBestAsk());           // resting sell is untouched
    EXPECT_EQ(std::nullopt, book.getBestBid());
}

TEST(FastOrderBook, FokFillsWhenEnoughLiquidity) {
    Order sell1{ .order_id = 1, .price = 100, .original_quantity = 6,
                 .remaining_quantity = 6, .side = Side::Sell, .order_type = OrderType::Limit };
    Order sell2{ .order_id = 2, .price = 101, .original_quantity = 6,
                 .remaining_quantity = 6, .side = Side::Sell, .order_type = OrderType::Limit };
    Order buy{ .order_id = 3, .price = 101, .original_quantity = 10,
               .remaining_quantity = 10, .side = Side::Buy, .order_type = OrderType::Limit,
               .time_in_force = TimeInForce::FOK };

    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);

    book.process(sell1, es);
    book.process(sell2, es);
    book.process(buy, es);

    EXPECT_EQ(2u, sink.trades.size());           // 6 @100 + 4 @101
    EXPECT_EQ(101, book.getBestAsk());           // 2 left at 101
    EXPECT_EQ(std::nullopt, book.getBestBid());  // FOK never rests
}

// Pool starts tiny so the free list is exhausted almost immediately: this is
// the only test that actually drives grow(). Correctness must be unchanged.
TEST(FastOrderBook, PoolGrowsBeyondInitialCapacity) {
    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK, /*pool_capacity=*/4);

    // Rest 200 non-crossing sell orders at distinct prices (> capacity of 4).
    for (OrderId id = 1; id <= 200; ++id) {
        Order sell{ .order_id = id, .price = static_cast<Price>(200 + id),
                    .original_quantity = 5, .remaining_quantity = 5,
                    .side = Side::Sell, .order_type = OrderType::Limit };
        book.process(sell, es);
    }
    EXPECT_EQ(0u, sink.trades.size());
    EXPECT_EQ(201, book.getBestAsk());        // lowest resting ask = 200 + 1

    // A big buy sweeps all of them -> 200 trades, book emptied.
    Order buy{ .order_id = 999, .original_quantity = 5 * 200,
               .remaining_quantity = 5 * 200, .side = Side::Buy,
               .order_type = OrderType::Market };
    book.process(buy, es);
    EXPECT_EQ(200u, sink.trades.size());
    EXPECT_EQ(std::nullopt, book.getBestAsk());
}

TEST(FastOrderBook, ModifyReduceKeepsPriority) {
    // id 1 is ahead of id 2 at price 100.
    Order s1{ .order_id = 1, .price = 100, .original_quantity = 10,
              .remaining_quantity = 10, .side = Side::Sell, .order_type = OrderType::Limit };
    Order s2{ .order_id = 2, .price = 100, .original_quantity = 5,
              .remaining_quantity = 5, .side = Side::Sell, .order_type = OrderType::Limit };

    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);
    book.process(s1, es);
    book.process(s2, es);

    EXPECT_TRUE(book.modifyOrder(1, 100, 3, es));   // shrink 10 -> 3, same price

    Order buy{ .order_id = 3, .price = 100, .original_quantity = 4,
               .remaining_quantity = 4, .side = Side::Buy, .order_type = OrderType::Limit };
    book.process(buy, es);

    ASSERT_EQ(2u, sink.trades.size());
    EXPECT_EQ(1u, sink.trades[0].maker);  EXPECT_EQ(3u, sink.trades[0].quantity);  // id1 still first
    EXPECT_EQ(2u, sink.trades[1].maker);  EXPECT_EQ(1u, sink.trades[1].quantity);
}

TEST(FastOrderBook, ModifyPriceCrossesAndTrades) {
    Order buy{ .order_id = 1, .price = 100, .original_quantity = 5,
               .remaining_quantity = 5, .side = Side::Buy, .order_type = OrderType::Limit };
    Order sell{ .order_id = 2, .price = 105, .original_quantity = 5,
                .remaining_quantity = 5, .side = Side::Sell, .order_type = OrderType::Limit };

    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);
    book.process(buy, es);
    book.process(sell, es);
    ASSERT_EQ(0u, sink.trades.size());

    // Reprice the resting sell down to 100 -> cancel-replace now crosses.
    EXPECT_TRUE(book.modifyOrder(2, 100, 5, es));
    ASSERT_EQ(1u, sink.trades.size());
    EXPECT_EQ(100, sink.trades[0].price);
    EXPECT_EQ(5u, sink.trades[0].quantity);
    EXPECT_EQ(std::nullopt, book.getBestBid());
    EXPECT_EQ(std::nullopt, book.getBestAsk());
}

TEST(FastOrderBook, ModifyUnknownReturnsFalse) {
    RecordingSink sink;
    ExecutionSink& es = sink;
    FastOrderBook book(MIN, MAX, TICK);
    EXPECT_FALSE(book.modifyOrder(42, 100, 5, es));
}

TEST(FastOrderBook, DifferentialAgainstOracle) {
    std::mt19937_64 rng(12345);               // fixed seed -> reproducible failures
    std::uniform_int_distribution<Price>     price_dist(MIN, MAX);
    std::uniform_int_distribution<Quantity>  qty_dist(1, 20);
    std::uniform_int_distribution<int>       side_dist(0, 1);
    std::uniform_int_distribution<int>       action_dist(0, 9);  // ~20% cancels
    std::uniform_int_distribution<int>       tif_dist(0, 4);     // GTC-heavy, some IOC/FOK

    OrderBook oracle;
    FastOrderBook fast(MIN, MAX, TICK, /*pool_capacity=*/8);  // tiny -> forces grow()
    RecordingSink oracle_sink, fast_sink;

    ExecutionSink& oes = oracle_sink;
    ExecutionSink& fes = fast_sink;
    OrderId next_id = 1;
    for (int step = 0; step < 5000; ++step) {
        int op = action_dist(rng);
        if (op < 2 && next_id > 1) {
            // Cancel a (possibly already-gone) id; both books must agree.
            OrderId victim = std::uniform_int_distribution<OrderId>(1, next_id - 1)(rng);
            EXPECT_EQ(oracle.cancelOrder(victim), fast.cancelOrder(victim));
            EXPECT_EQ(oracle.getBestBid(), fast.getBestBid());
            EXPECT_EQ(oracle.getBestAsk(), fast.getBestAsk());
            continue;
        }
        if (op == 2 && next_id > 1) {
            // Modify a (possibly already-gone) id; cancel-replace may trade.
            OrderId victim = std::uniform_int_distribution<OrderId>(1, next_id - 1)(rng);
            Price np = price_dist(rng);
            Quantity nq = qty_dist(rng);
            std::size_t before = oracle_sink.trades.size();
            ASSERT_EQ(before, fast_sink.trades.size());
            EXPECT_EQ(oracle.modifyOrder(victim, np, nq, oes),
                      fast.modifyOrder(victim, np, nq, fes));
            ASSERT_EQ(oracle_sink.trades.size(), fast_sink.trades.size());
            for (std::size_t i = before; i < oracle_sink.trades.size(); ++i)
                EXPECT_EQ(oracle_sink.trades[i], fast_sink.trades[i]);
            EXPECT_EQ(oracle.getBestBid(), fast.getBestBid());
            EXPECT_EQ(oracle.getBestAsk(), fast.getBestAsk());
            if (::testing::Test::HasFailure()) FAIL() << "modify divergence at step " << step;
            continue;
        }

        Side side = side_dist(rng) ? Side::Buy : Side::Sell;
        bool market = (action_dist(rng) == 0);   // occasional market order
        Quantity q = qty_dist(rng);
        TimeInForce tif = TimeInForce::GTC;
        if (!market) {
            int t = tif_dist(rng);
            tif = (t == 0) ? TimeInForce::IOC : (t == 1) ? TimeInForce::FOK : TimeInForce::GTC;
        }
        Order order{
            .order_id = next_id++,
            .price = market ? 0 : price_dist(rng),
            .original_quantity = q,
            .remaining_quantity = q,
            .side = side,
            .order_type = market ? OrderType::Market : OrderType::Limit,
            .time_in_force = tif,
        };
        applyAndCompare(order, oracle, fast, oracle_sink, fast_sink);
        if (::testing::Test::HasFailure()) {
            FAIL() << "divergence at step " << step << " (order id " << order.order_id << ")";
        }
    }
}
