#include "gtest/gtest.h"
#include "engine/order.hpp"
#include "engine/command.hpp"
#include "engine/recording_sink.hpp"
#include "engine/execution_sink.hpp"
#include "engine/event_source.hpp"
#include "engine/vector_event_source.hpp"
#include "engine/order_book.hpp"
#include "engine/engine.hpp"

TEST(Engine, CompleteEngineRun) {
    std::vector<Command> cmd(4);

    cmd[0] = {
        .type = CommandType::New,
        .order = Order 
            {
                .order_id = 1,
                .price = 100,
                .original_quantity = 5,
                .remaining_quantity = 5,
                .side = Side::Sell,
                .order_type = OrderType::Limit,
            },
    };

    cmd[1] = {
        .type = CommandType::New,
        .order = Order 
            {
                .order_id = 2,
                .price = 101,
                .original_quantity = 5,
                .remaining_quantity = 5,
                .side = Side::Sell,
                .order_type = OrderType::Limit,
            },
    };

    cmd[2] = {
        .type = CommandType::New,
        .order = Order 
            {
                .order_id = 3,
                .original_quantity = 8,
                .remaining_quantity = 8,
                .side = Side::Buy,
                .order_type = OrderType::Market,
            },
    };

    cmd[3] = {
        .type = CommandType::Cancel,
        .cancel_id = 2,
    };

    RecordingSink recording_sink;
    ExecutionSink& execution_sink = recording_sink;
    OrderBook order_book;
    VectorEventSource vector_event_source(cmd);
    EventSource& event_source = vector_event_source;

    run(event_source, order_book, execution_sink);

    EXPECT_EQ(2, recording_sink.trades.size());

    EXPECT_EQ(std::nullopt, order_book.getBestAsk());
    EXPECT_EQ(std::nullopt, order_book.getBestBid());
}