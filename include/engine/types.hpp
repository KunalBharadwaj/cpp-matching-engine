#pragma once
#include <cstdint>

using OrderId  = std::uint64_t;
using Price    = std::int64_t;
using Quantity = std::uint64_t;

enum class Side : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market };

// Time-in-force. GTC first so a default-initialized Order is GTC (the
// behaviour everything had before this field existed).
//   GTC: rest the unfilled remainder.
//   IOC: fill what you can now, cancel the remainder.
//   FOK: fill completely now, or do nothing at all.
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };
