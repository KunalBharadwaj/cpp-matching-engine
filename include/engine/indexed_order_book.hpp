#pragma once
#include <map>
#include <list>
#include <optional>
#include <unordered_map>
#include <limits>
#include <algorithm>
#include "engine/order.hpp"
#include "engine/trade.hpp"
#include "engine/execution_sink.hpp"

// The *competent* baseline. Same std::map + std::list book as the naive
// OrderBook, but with an OrderId -> location index so cancel/modify go
// straight to the resting order instead of scanning. This is the fair
// comparison for FastOrderBook: it isolates what the price ladder, object
// pool and intrusive list actually buy (cache locality, no per-node malloc,
// no tree rebalancing) once the O(n) cancel strawman is removed.
//
// cancel here is O(log n): O(1) to erase the order from its level via the
// stored list iterator, plus an O(log n) map lookup/erase of the level.
// FastOrderBook is O(1) end to end -- that remaining gap is the honest one.
class IndexedOrderBook {
    std::map<Price, std::list<Order>, std::greater<Price>> bid;
    std::map<Price, std::list<Order>> ask;

    // Where a resting order lives: which side, its price (to find the level),
    // and a stable list iterator (to erase it in O(1)).
    struct Location {
        Side side;
        Price price;
        std::list<Order>::iterator it;
    };
    std::unordered_map<OrderId, Location> index;

    void consumeLevel(Order& taker, std::list<Order>& queue, ExecutionSink& execution_sink) {
        while (!queue.empty() && taker.remaining_quantity > 0) {
            Order& maker = queue.front();
            Quantity tradeable = std::min(taker.remaining_quantity, maker.remaining_quantity);

            Trade trade{ .maker = maker.order_id, .taker = taker.order_id,
                         .price = maker.price, .quantity = tradeable };
            execution_sink.on_trade(trade);

            taker.remaining_quantity -= tradeable;
            maker.remaining_quantity -= tradeable;
            if (maker.remaining_quantity == 0) {
                index.erase(maker.order_id);   // left the book
                queue.pop_front();
            }
        }
    }

    void rest(Order& order) {
        if (order.side == Side::Buy) {
            std::list<Order>& lst = bid[order.price];
            lst.push_back(order);
            index[order.order_id] = {Side::Buy, order.price, std::prev(lst.end())};
        } else {
            std::list<Order>& lst = ask[order.price];
            lst.push_back(order);
            index[order.order_id] = {Side::Sell, order.price, std::prev(lst.end())};
        }
    }

    void processOrder(Order& order, ExecutionSink& execution_sink, Price limit_price, bool rest_remainder) {
        switch (order.side) {
            case Side::Buy: {
                auto it = ask.begin();
                while (it != ask.end() && it->first <= limit_price && order.remaining_quantity > 0) {
                    consumeLevel(order, it->second, execution_sink);
                    auto curr = it; ++it;
                    if (curr->second.empty()) ask.erase(curr);
                }
                if (order.remaining_quantity > 0 && rest_remainder) rest(order);
                break;
            }
            case Side::Sell: {
                auto it = bid.begin();
                while (it != bid.end() && it->first >= limit_price && order.remaining_quantity > 0) {
                    consumeLevel(order, it->second, execution_sink);
                    auto curr = it; ++it;
                    if (curr->second.empty()) bid.erase(curr);
                }
                if (order.remaining_quantity > 0 && rest_remainder) rest(order);
                break;
            }
        }
    }

    void processLimitOrder(Order& order, ExecutionSink& execution_sink) {
        processOrder(order, execution_sink, order.price, true);
    }
    void processMarketOrder(Order& order, ExecutionSink& execution_sink) {
        Price aggressive = (order.side == Side::Buy) ? std::numeric_limits<Price>::max()
                                                     : std::numeric_limits<Price>::min();
        processOrder(order, execution_sink, aggressive, false);
    }

    bool canFullyFill(const Order& order) const {
        Quantity need = order.remaining_quantity, avail = 0;
        if (order.side == Side::Buy) {
            for (const auto& [price, queue] : ask) {
                if (price > order.price) break;
                for (const Order& m : queue) { avail += m.remaining_quantity; if (avail >= need) return true; }
            }
        } else {
            for (const auto& [price, queue] : bid) {
                if (price < order.price) break;
                for (const Order& m : queue) { avail += m.remaining_quantity; if (avail >= need) return true; }
            }
        }
        return avail >= need;
    }

public:
    void process(Order& order, ExecutionSink& execution_sink) {
        if (order.order_type == OrderType::Market) { processMarketOrder(order, execution_sink); return; }
        switch (order.time_in_force) {
            case TimeInForce::GTC: processOrder(order, execution_sink, order.price, true);  break;
            case TimeInForce::IOC: processOrder(order, execution_sink, order.price, false); break;
            case TimeInForce::FOK: if (canFullyFill(order)) processOrder(order, execution_sink, order.price, false); break;
        }
    }

    std::optional<Price> getBestBid() const {
        if (bid.empty()) return std::nullopt;
        return bid.begin()->first;
    }
    std::optional<Price> getBestAsk() const {
        if (ask.empty()) return std::nullopt;
        return ask.begin()->first;
    }

    // O(log n): index lookup -> O(1) list erase, plus the level erase if it empties.
    bool cancelOrder(OrderId order_id) {
        auto found = index.find(order_id);
        if (found == index.end()) return false;
        Location loc = found->second;
        if (loc.side == Side::Buy) {
            auto level = bid.find(loc.price);
            level->second.erase(loc.it);
            if (level->second.empty()) bid.erase(level);
        } else {
            auto level = ask.find(loc.price);
            level->second.erase(loc.it);
            if (level->second.empty()) ask.erase(level);
        }
        index.erase(found);
        return true;
    }

    bool modifyOrder(OrderId order_id, Price new_price, Quantity new_quantity, ExecutionSink& execution_sink) {
        if (new_quantity == 0) return cancelOrder(order_id);
        auto found = index.find(order_id);
        if (found == index.end()) return false;
        Order& o = *found->second.it;
        if (new_price == o.price && new_quantity <= o.remaining_quantity) {
            o.remaining_quantity = new_quantity;       // shrink in place, keep priority
            return true;
        }
        Side side = o.side;                            // capture before cancel invalidates o
        cancelOrder(order_id);
        Order replacement{ .order_id = order_id, .price = new_price,
                           .original_quantity = new_quantity, .remaining_quantity = new_quantity,
                           .side = side, .order_type = OrderType::Limit, .time_in_force = TimeInForce::GTC };
        process(replacement, execution_sink);
        return true;
    }
};
