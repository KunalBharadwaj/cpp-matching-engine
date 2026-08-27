#include "gtest/gtest.h"
#include "engine/order.hpp"
#include "engine/order_book.hpp"
#include "engine/recording_sink.hpp"

TEST(LimitOrder, PartiallyFilledRestingOrder) {
    Order sell{
        .order_id = 1,
        .price = 100,
        .original_quantity = 10,
        .remaining_quantity = 10,
        .side = Side::Sell,
        .order_type = OrderType::Limit,
    };

    Order buy{
        .order_id = 2,
        .price = 100,
        .original_quantity = 4,
        .remaining_quantity = 4,
        .side = Side::Buy,
        .order_type = OrderType::Limit,
    };
    RecordingSink recording_sink;
    ExecutionSink& execution_sink = recording_sink;
    OrderBook order_book;

    order_book.processLimitOrder(sell, execution_sink);
    order_book.processLimitOrder(buy, execution_sink);

    ASSERT_EQ(1, recording_sink.trades.size());
    Trade recorded_trade = recording_sink.trades[0];
    EXPECT_EQ(1, recorded_trade.maker);
    EXPECT_EQ(2, recorded_trade.taker);
    EXPECT_EQ(100, recorded_trade.price);
    EXPECT_EQ(4, recorded_trade.quantity);



    EXPECT_EQ(100, order_book.getBestAsk());
    EXPECT_EQ(std::nullopt, order_book.getBestBid());
}


TEST(LimitOrder, NoCross) {
    Order sell{
        .order_id = 1,
        .price = 105,
        .original_quantity = 5,
        .remaining_quantity = 5,
        .side = Side::Sell,
        .order_type = OrderType::Limit,
    };

    Order buy{
        .order_id = 2,
        .price = 100,
        .original_quantity = 3,
        .remaining_quantity = 3,
        .side = Side::Buy,
        .order_type = OrderType::Limit,
    };

    RecordingSink recording_sink;
    ExecutionSink& execution_sink = recording_sink;
    OrderBook order_book;

    order_book.processLimitOrder(sell, execution_sink);
    order_book.processLimitOrder(buy, execution_sink);

    ASSERT_EQ(0, recording_sink.trades.size());

    EXPECT_EQ(100, order_book.getBestBid());
    EXPECT_EQ(105, order_book.getBestAsk());
}

TEST(MarketOrder, MarketOrderWalksMultipleLevels) {

    Order sell1{
        .order_id = 1,
        .price = 100,
        .original_quantity = 5,
        .remaining_quantity = 5,
        .side = Side::Sell,
        .order_type = OrderType::Limit,
    };

    Order sell2{
        .order_id = 2,
        .price = 101,
        .original_quantity = 5,
        .remaining_quantity = 5,
        .side = Side::Sell,
        .order_type = OrderType::Limit,
    };

    Order buy{
        .order_id = 3,
        .original_quantity = 8,
        .remaining_quantity = 8,
        .side = Side::Buy,
        .order_type = OrderType::Market,
    };

    RecordingSink recording_sink;
    ExecutionSink& execution_sink = recording_sink;
    OrderBook order_book;

    order_book.processLimitOrder(sell1, execution_sink);
    order_book.processLimitOrder(sell2, execution_sink);
    order_book.processMarketOrder(buy, execution_sink);

    EXPECT_EQ(2, recording_sink.trades.size());

    EXPECT_EQ(101, order_book.getBestAsk());
    EXPECT_EQ(std::nullopt, order_book.getBestBid());
}

TEST(MarketOrder, ExactFullFillEmptiesBook) {
    Order sell {
        .order_id = 1,
        .price = 100,
        .original_quantity = 5,
        .remaining_quantity = 5,
        .side = Side::Sell,
        .order_type = OrderType::Limit,
    };
    Order buy {
        .order_id = 2,
        .original_quantity = 5,
        .remaining_quantity = 5,
        .side = Side::Buy,
        .order_type = OrderType::Market,
    };

    RecordingSink recording_sink;
    ExecutionSink& execution_sink = recording_sink;
    OrderBook order_book;

    order_book.processLimitOrder(sell, execution_sink);
    order_book.processMarketOrder(buy, execution_sink);

    EXPECT_EQ(1, recording_sink.trades.size());
    EXPECT_EQ(std::nullopt, order_book.getBestAsk());
    EXPECT_EQ(std::nullopt, order_book.getBestBid());
}

TEST(MarketOrder, MarketOrderIntoEmptyBookDoesNothing) {
    Order buy {
        .order_id = 1,
        .original_quantity = 5,
        .remaining_quantity = 5,
        .side = Side::Buy,
        .order_type = OrderType::Market,
    };

    RecordingSink recording_sink;
    ExecutionSink& execution_sink = recording_sink;
    OrderBook order_book;

    order_book.processMarketOrder(buy, execution_sink);

    EXPECT_EQ(0, recording_sink.trades.size());
    EXPECT_EQ(std::nullopt, order_book.getBestAsk());
    EXPECT_EQ(std::nullopt, order_book.getBestBid());
}


TEST(CancelOrder, CancelRemovesRestingOrder) {
    Order sell {
        .order_id = 1,
        .price = 100,
        .original_quantity = 5,
        .remaining_quantity = 5,
        .side = Side::Sell,
        .order_type = OrderType::Limit,
    };

    RecordingSink recording_sink;
    ExecutionSink& execution_sink = recording_sink;
    OrderBook order_book;

    order_book.processLimitOrder(sell, execution_sink);

    EXPECT_TRUE(order_book.cancelOrder(1));
    EXPECT_EQ(std::nullopt, order_book.getBestAsk());
}

TEST(CancelOrder, CancelUnknownIdReturnsFalse) {
    RecordingSink recording_sink;
    ExecutionSink& execution_sink = recording_sink;
    OrderBook order_book;

    EXPECT_FALSE(order_book.cancelOrder(999));
}

TEST(CancelOrder, CancelThenNoTrade) {
    Order sell {
        .order_id = 1,
        .price = 100,
        .original_quantity = 5,
        .remaining_quantity = 5,
        .side = Side::Sell,
        .order_type = OrderType::Limit,
    };

    RecordingSink recording_sink;
    ExecutionSink& execution_sink = recording_sink;
    OrderBook order_book;

    order_book.processLimitOrder(sell, execution_sink);

    EXPECT_TRUE(order_book.cancelOrder(1));

    Order buy {
        .order_id = 2,
        .price = 100,
        .original_quantity = 5,
        .remaining_quantity = 5,
        .side = Side::Buy,
        .order_type = OrderType::Limit,
    };

    order_book.processLimitOrder(buy, execution_sink);

    EXPECT_EQ(0, recording_sink.trades.size());
    EXPECT_EQ(100, order_book.getBestBid());
}