#include "gtest/gtest.h"
#include <random>
#include "engine/order.hpp"
#include "engine/order_book.hpp"
#include "engine/indexed_order_book.hpp"
#include "engine/recording_sink.hpp"

// IndexedOrderBook must behave identically to the naive OrderBook oracle; it
// only adds an id->location index for fast cancel/modify.

TEST(IndexedOrderBook, CancelViaIndex) {
    Order sell{ .order_id = 1, .price = 100, .original_quantity = 5,
                .remaining_quantity = 5, .side = Side::Sell, .order_type = OrderType::Limit };
    RecordingSink sink; ExecutionSink& es = sink;
    IndexedOrderBook book;
    book.process(sell, es);
    EXPECT_EQ(100, book.getBestAsk());
    EXPECT_TRUE(book.cancelOrder(1));
    EXPECT_EQ(std::nullopt, book.getBestAsk());
    EXPECT_FALSE(book.cancelOrder(1));
}

TEST(IndexedOrderBook, ModifyReduceKeepsPriority) {
    Order s1{ .order_id = 1, .price = 100, .original_quantity = 10,
              .remaining_quantity = 10, .side = Side::Sell, .order_type = OrderType::Limit };
    Order s2{ .order_id = 2, .price = 100, .original_quantity = 5,
              .remaining_quantity = 5, .side = Side::Sell, .order_type = OrderType::Limit };
    RecordingSink sink; ExecutionSink& es = sink;
    IndexedOrderBook book;
    book.process(s1, es); book.process(s2, es);
    EXPECT_TRUE(book.modifyOrder(1, 100, 3, es));
    Order buy{ .order_id = 3, .price = 100, .original_quantity = 4,
               .remaining_quantity = 4, .side = Side::Buy, .order_type = OrderType::Limit };
    book.process(buy, es);
    ASSERT_EQ(2u, sink.trades.size());
    EXPECT_EQ(1u, sink.trades[0].maker);   // id 1 kept its spot
    EXPECT_EQ(2u, sink.trades[1].maker);
}

TEST(IndexedOrderBook, DifferentialAgainstOracle) {
    std::mt19937_64 rng(777);
    std::uniform_int_distribution<Price>    price_dist(1, 1000);
    std::uniform_int_distribution<Quantity> qty_dist(1, 20);
    std::uniform_int_distribution<int>      side_dist(0, 1);
    std::uniform_int_distribution<int>      op_dist(0, 9);
    std::uniform_int_distribution<int>      tif_dist(0, 4);

    OrderBook oracle; IndexedOrderBook indexed;
    RecordingSink os, is_; ExecutionSink& oes = os; ExecutionSink& ies = is_;
    OrderId next_id = 1;

    for (int step = 0; step < 5000; ++step) {
        int op = op_dist(rng);
        if (op < 2 && next_id > 1) {
            OrderId v = std::uniform_int_distribution<OrderId>(1, next_id - 1)(rng);
            EXPECT_EQ(oracle.cancelOrder(v), indexed.cancelOrder(v));
        } else if (op == 2 && next_id > 1) {
            OrderId v = std::uniform_int_distribution<OrderId>(1, next_id - 1)(rng);
            Price np = price_dist(rng); Quantity nq = qty_dist(rng);
            std::size_t before = os.trades.size();
            EXPECT_EQ(oracle.modifyOrder(v, np, nq, oes), indexed.modifyOrder(v, np, nq, ies));
            ASSERT_EQ(os.trades.size(), is_.trades.size());
            for (std::size_t i = before; i < os.trades.size(); ++i) EXPECT_EQ(os.trades[i], is_.trades[i]);
        } else {
            bool market = (op == 3);
            Quantity q = qty_dist(rng);
            TimeInForce tif = TimeInForce::GTC;
            if (!market) { int t = tif_dist(rng); tif = t == 0 ? TimeInForce::IOC : t == 1 ? TimeInForce::FOK : TimeInForce::GTC; }
            Order o{ .order_id = next_id++, .price = market ? 0 : price_dist(rng),
                     .original_quantity = q, .remaining_quantity = q, .side = side_dist(rng) ? Side::Buy : Side::Sell,
                     .order_type = market ? OrderType::Market : OrderType::Limit, .time_in_force = tif };
            std::size_t before = os.trades.size();
            Order a = o, b = o;
            oracle.process(a, oes); indexed.process(b, ies);
            ASSERT_EQ(os.trades.size(), is_.trades.size());
            for (std::size_t i = before; i < os.trades.size(); ++i) EXPECT_EQ(os.trades[i], is_.trades[i]);
        }
        EXPECT_EQ(oracle.getBestBid(), indexed.getBestBid());
        EXPECT_EQ(oracle.getBestAsk(), indexed.getBestAsk());
        if (::testing::Test::HasFailure()) { FAIL() << "divergence at step " << step; }
    }
}
