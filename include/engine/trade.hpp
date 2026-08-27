#pragma once

#include "engine/types.hpp"

struct Trade {
    OrderId maker {}; // resting order that provides the liquidity
    OrderId taker {}; // takes away the liquidity
    Price price {};
    Quantity quantity {};
    bool operator==(const Trade&) const = default;
    // I don't think if we need a side on the trade because trade is basically a deal between two parties/orders ig
};