#pragma once

#include <vector>
#include <list>
#include <optional>
#include <limits>
#include <algorithm>
#include <cstdint>
#include <cassert>
#include "engine/order.hpp"
#include "engine/trade.hpp"
#include "engine/execution_sink.hpp"


// One rung of the price ladder. `total_quantity` is a running sum of the
// resting quantity at this price so top-of-book depth is O(1) later; we keep
// it honest on every add / fill / cancel.
struct PriceLevel {
    std::list<Order> orders;      // FIFO queue -> preserves time priority
    Quantity total_quantity{};
};

// Phase 2 book: replaces the oracle's std::map with a flat array indexed by
// price. Specific-price lookup is O(1) and the storage is contiguous, but a
// vector can't tell us where the top of book is -- that is the whole job of
// best_bid_idx / best_ask_idx, the cursors that replace map::begin().
class FastOrderBook {
    std::size_t size{};
    Price min_price{}, max_price{}, tick_size{};

    // bid[i] and ask[i] both hold the orders resting at indexToPrice(i).
    // Higher index == higher price (idx grows with price).
    std::vector<PriceLevel> bid;
    std::vector<PriceLevel> ask;

    // INVARIANT (the heart of Phase 2):
    //   best_bid_idx = highest non-empty index in `bid`, or -1 if no bids.
    //   best_ask_idx = lowest  non-empty index in `ask`, or -1 if no asks.
    // Every mutating operation must leave these true.
    std::int64_t best_bid_idx{-1};
    std::int64_t best_ask_idx{-1};

    std::int64_t priceToIndex(Price price) const {
        return (price - min_price) / tick_size;
    }

    Price indexToPrice(std::int64_t index) const {
        return min_price + index * tick_size;
    }

    // The best-ask level just emptied: scan upward for the next non-empty
    // rung. If none remain, the ask side is empty -> -1. Walking over a run of
    // empty rungs is why this is amortized cheap, not per-op constant.
    void advanceBestAsk() {
        std::int64_t i = best_ask_idx + 1;
        while (i < static_cast<std::int64_t>(size) && ask[i].orders.empty()) i++;
        best_ask_idx = (i < static_cast<std::int64_t>(size)) ? i : -1;
    }

    // Mirror image for bids: best is the highest index, so we scan downward.
    void advanceBestBid() {
        std::int64_t i = best_bid_idx - 1;
        while (i >= 0 && bid[i].orders.empty()) i--;
        best_bid_idx = (i >= 0) ? i : -1;
    }

    // Rest the unfilled remainder on our own side and repair the cursor.
    void addToBook(Order& order) {
        std::int64_t idx = priceToIndex(order.price);
        assert(idx >= 0 && idx < static_cast<std::int64_t>(size));
        if (order.side == Side::Buy) {
            bid[idx].orders.push_back(order);
            bid[idx].total_quantity += order.remaining_quantity;
            if (best_bid_idx == -1 || idx > best_bid_idx) best_bid_idx = idx;
        } else {
            ask[idx].orders.push_back(order);
            ask[idx].total_quantity += order.remaining_quantity;
            if (best_ask_idx == -1 || idx < best_ask_idx) best_ask_idx = idx;
        }
    }

    // FIFO fill at a single level -- identical semantics to the oracle's
    // consumeLevel, plus we keep PriceLevel::total_quantity in sync.
    void consumeLevel(Order& taker, PriceLevel& level, ExecutionSink& execution_sink) {
        std::list<Order>& queue = level.orders;
        while (!queue.empty() && taker.remaining_quantity > 0) {
            Order& maker = queue.front();
            Quantity tradeable_quantity =
                std::min(taker.remaining_quantity, maker.remaining_quantity);

            Trade trade {
                .maker = maker.order_id,
                .taker = taker.order_id,
                .price = maker.price,          // trade prints at the resting (maker) price
                .quantity = tradeable_quantity
            };
            execution_sink.on_trade(trade);

            taker.remaining_quantity -= tradeable_quantity;
            maker.remaining_quantity -= tradeable_quantity;
            level.total_quantity     -= tradeable_quantity;
            if (maker.remaining_quantity == 0) queue.pop_front();
        }
    }

    // Same shape as the oracle: sweep the opposite side while it is in range
    // and we still have quantity, then (for limits) rest the remainder.
    void processOrder(Order& order, ExecutionSink& execution_sink,
                      Price limit_price, bool rest_remainder) {
        switch (order.side) {
            case Side::Buy: {
                while (best_ask_idx != -1
                       && indexToPrice(best_ask_idx) <= limit_price
                       && order.remaining_quantity > 0) {
                    consumeLevel(order, ask[best_ask_idx], execution_sink);
                    // If we drained the level, move the cursor up; otherwise
                    // the taker is exhausted and the loop condition stops us.
                    if (ask[best_ask_idx].orders.empty()) advanceBestAsk();
                }
                if (order.remaining_quantity > 0 && rest_remainder) addToBook(order);
                break;
            }
            case Side::Sell: {
                while (best_bid_idx != -1
                       && indexToPrice(best_bid_idx) >= limit_price
                       && order.remaining_quantity > 0) {
                    consumeLevel(order, bid[best_bid_idx], execution_sink);
                    if (bid[best_bid_idx].orders.empty()) advanceBestBid();
                }
                if (order.remaining_quantity > 0 && rest_remainder) addToBook(order);
                break;
            }
        }
    }

    void processLimitOrder(Order& order, ExecutionSink& execution_sink) {
        processOrder(order, execution_sink, order.price, true);
    }

    void processMarketOrder(Order& order, ExecutionSink& execution_sink) {
        // A market order crosses at any price, so we hand the walk a sentinel
        // limit that can never gate it, and never rest the remainder.
        Price aggressive = (order.side == Side::Buy)
            ? std::numeric_limits<Price>::max()
            : std::numeric_limits<Price>::min();
        processOrder(order, execution_sink, aggressive, false);
    }

public:

    FastOrderBook(Price min_price, Price max_price, Price tick_size)
        : size(static_cast<std::size_t>((max_price - min_price) / tick_size) + 1),
          min_price(min_price), max_price(max_price), tick_size(tick_size),
          bid(size), ask(size),
          best_bid_idx(-1), best_ask_idx(-1) {}

    void process(Order& order, ExecutionSink& execution_sink) {
        if (order.order_type == OrderType::Limit)       processLimitOrder(order, execution_sink);
        else if (order.order_type == OrderType::Market) processMarketOrder(order, execution_sink);
    }

    std::optional<Price> getBestBid() const {
        if (best_bid_idx == -1) return std::nullopt;
        return indexToPrice(best_bid_idx);
    }

    std::optional<Price> getBestAsk() const {
        if (best_ask_idx == -1) return std::nullopt;
        return indexToPrice(best_ask_idx);
    }

    // O(active levels) scan for now -- same "before" as the oracle. Phase 3's
    // flat hash registry is what turns this into O(1).
    bool cancelOrder(OrderId order_id) {
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(size); i++) {
            std::list<Order>& queue = ask[i].orders;
            for (auto it = queue.begin(); it != queue.end(); ++it) {
                if (it->order_id == order_id) {
                    ask[i].total_quantity -= it->remaining_quantity;
                    queue.erase(it);
                    if (queue.empty() && i == best_ask_idx) advanceBestAsk();
                    return true;
                }
            }
        }
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(size); i++) {
            std::list<Order>& queue = bid[i].orders;
            for (auto it = queue.begin(); it != queue.end(); ++it) {
                if (it->order_id == order_id) {
                    bid[i].total_quantity -= it->remaining_quantity;
                    queue.erase(it);
                    if (queue.empty() && i == best_bid_idx) advanceBestBid();
                    return true;
                }
            }
        }
        return false;
    }
};
