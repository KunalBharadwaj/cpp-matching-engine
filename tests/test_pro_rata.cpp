#include "gtest/gtest.h"
#include "engine/order.hpp"
#include "engine/fast_order_book.hpp"
#include "engine/recording_sink.hpp"

static Order sell(OrderId id, Price p, Quantity q) {
    return Order{ id, p, q, q, Side::Sell, OrderType::Limit, TimeInForce::GTC };
}
static Order buy(OrderId id, Price p, Quantity q) {
    return Order{ id, p, q, q, Side::Buy, OrderType::Limit, TimeInForce::GTC };
}

// Two resting orders of different size, one aggressor smaller than the level.
// Pro-rata splits by size (5 : 15), where FIFO would fill the first fully (10)
// then the second (10).
TEST(ProRata, SplitsBySize) {
    ProRataOrderBook book(1, 1000, 1);
    RecordingSink sink; ExecutionSink& es = sink;

    Order s1 = sell(1, 100, 10), s2 = sell(2, 100, 30);
    book.process(s1, es); book.process(s2, es);
    Order b = buy(3, 100, 20);
    book.process(b, es);

    ASSERT_EQ(2u, sink.trades.size());
    EXPECT_EQ(1u, sink.trades[0].maker); EXPECT_EQ(5u,  sink.trades[0].quantity);   // 20*10/40
    EXPECT_EQ(2u, sink.trades[1].maker); EXPECT_EQ(15u, sink.trades[1].quantity);   // 20*30/40
    EXPECT_EQ(100, book.getBestAsk());            // 20 still resting (5 + 15)
    EXPECT_EQ(std::nullopt, book.getBestBid());
}

// Equal sizes: floor shares leave leftover lots, which go to the largest
// remainders, ties broken by time priority (so the earliest orders get them).
TEST(ProRata, LeftoverLotsByTimePriority) {
    ProRataOrderBook book(1, 1000, 1);
    RecordingSink sink; ExecutionSink& es = sink;

    Order s1 = sell(1, 100, 10), s2 = sell(2, 100, 10), s3 = sell(3, 100, 10);
    book.process(s1, es); book.process(s2, es); book.process(s3, es);
    Order b = buy(4, 100, 20);                    // floor 6 each (18); leftover 2
    book.process(b, es);

    ASSERT_EQ(3u, sink.trades.size());
    EXPECT_EQ(1u, sink.trades[0].maker); EXPECT_EQ(7u, sink.trades[0].quantity);    // 6 + leftover
    EXPECT_EQ(2u, sink.trades[1].maker); EXPECT_EQ(7u, sink.trades[1].quantity);    // 6 + leftover
    EXPECT_EQ(3u, sink.trades[2].maker); EXPECT_EQ(6u, sink.trades[2].quantity);    // 6
    Quantity total = sink.trades[0].quantity + sink.trades[1].quantity + sink.trades[2].quantity;
    EXPECT_EQ(20u, total);                         // taker fully filled, nothing lost
}

// When the aggressor is >= the level's total, pro-rata collapses to a full
// sweep -- identical to FIFO.
TEST(ProRata, FullSweepMatchesFifo) {
    ProRataOrderBook book(1, 1000, 1);
    RecordingSink sink; ExecutionSink& es = sink;

    Order s1 = sell(1, 100, 10), s2 = sell(2, 100, 30);
    book.process(s1, es); book.process(s2, es);
    Order b = buy(3, 100, 50);                    // 50 >= 40 total
    book.process(b, es);

    ASSERT_EQ(2u, sink.trades.size());
    EXPECT_EQ(10u, sink.trades[0].quantity);
    EXPECT_EQ(30u, sink.trades[1].quantity);
    EXPECT_EQ(std::nullopt, book.getBestAsk());   // level cleared
    EXPECT_EQ(100, book.getBestBid());            // 10 remainder rested
}

// Same workload, FIFO vs pro-rata, must emit the same TOTAL traded quantity
// (conservation) even though the per-order split differs.
TEST(ProRata, ConservesQuantityVsFifo) {
    FastOrderBook fifo(1, 1000, 1);
    ProRataOrderBook pro(1, 1000, 1);
    RecordingSink fs, ps; ExecutionSink& fes = fs; ExecutionSink& pes = ps;

    for (OrderId id = 1; id <= 5; ++id) {
        Order a = sell(id, 100, id * 3);          // sizes 3,6,9,12,15
        Order b = a;
        fifo.process(a, fes); pro.process(b, pes);
    }
    Order fb = buy(100, 100, 20), pb = buy(100, 100, 20);
    fifo.process(fb, fes); pro.process(pb, pes);

    Quantity ftot = 0, ptot = 0;
    for (auto& t : fs.trades) ftot += t.quantity;
    for (auto& t : ps.trades) ptot += t.quantity;
    EXPECT_EQ(ftot, ptot);                        // same liquidity consumed
    EXPECT_EQ(20u, ptot);
}
