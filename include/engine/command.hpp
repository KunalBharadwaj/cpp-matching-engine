#pragma once

#include <cstdint>
#include "engine/order.hpp"

enum class CommandType : std::uint8_t { New, Cancel };

struct Command {
    CommandType type{};
    Order order{};        // used when type == New
    OrderId cancel_id{};    // used when we have cancel_id
};