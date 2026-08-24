#pragma once
#include "engine/types.hpp"

struct Order {
    OrderId order_id {};
    Price price {};
    Quantity original_quantity {};
    Quantity remaining_quantity {};
    Side side {};
    OrderType order_type {};
};

// approx Order size is 64 + 8 + 8 + 64 + 64 + 64 + 48 bits of padding = 320 bits or 40 bytes