#pragma once
#include <cstdint>

using OrderId  = std::uint64_t;
using Price    = std::int64_t;
using Quantity = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };
