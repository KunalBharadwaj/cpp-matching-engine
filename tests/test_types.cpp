#include <gtest/gtest.h>
#include "engine/order.hpp"
#include "engine/trade.hpp"

// Test if all the Order fields are properly default-initialized to zero
TEST(OrderTest, DefaultInitializationIsZero) {
    Order order;
    EXPECT_EQ(static_cast<uint64_t>(0), order.order_id);
    EXPECT_EQ(static_cast<int64_t>(0), order.price);
    EXPECT_EQ(static_cast<uint64_t>(0), order.original_quantity);
    EXPECT_EQ(static_cast<uint64_t>(0), order.remaining_quantity);
    EXPECT_EQ(static_cast<uint8_t>(0), static_cast<uint8_t>(order.side));
    EXPECT_EQ(static_cast<uint8_t>(0), static_cast<uint8_t>(order.order_type));
}

// I don't think that we should add test for sizeof(Order) because that is not required for the integrity of the flow also it will hinder modifiability of the code later on.

TEST(OrderTest, EnumValuesAreAsExpected) {
    EXPECT_EQ(static_cast<uint8_t>(0), static_cast<uint8_t>(Side::Buy));
    EXPECT_EQ(static_cast<uint8_t>(1), static_cast<uint8_t>(Side::Sell));
}

// Similarly we can do for Trade fields as well

TEST(TradeTest, DefaultInitializationIsZero) {
    Trade trade;
    EXPECT_EQ(static_cast<uint64_t>(0), trade.maker);
    EXPECT_EQ(static_cast<int64_t>(0), trade.price);
    EXPECT_EQ(static_cast<uint64_t>(0), trade.quantity);
    EXPECT_EQ(static_cast<uint64_t>(0), trade.taker);
}