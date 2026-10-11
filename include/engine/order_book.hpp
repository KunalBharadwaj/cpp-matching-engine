#pragma once
#include <map>
#include <list>
#include <vector>
#include <optional>
#include <functional>
#include <algorithm>
#include <limits>
#include "engine/order.hpp"
#include "engine/trade.hpp"
#include "engine/execution_sink.hpp"


// class so that our fields are private by default
class OrderBook {
    // Using descending map for the ease of logic so that we can just remember that we have to walk forward on the other side by taking the beginning iterator incrementally (same for both side of orders)
    std::map<Price, std::list<Order>, std::greater<Price>> bid;
    std::map<Price, std::list<Order>> ask;

    // I need a core processing pipeline for all types of orders

    void consumeLevel(Order& taker, std::list<Order>& queue, ExecutionSink& execution_sink) {
        while(!queue.empty() && taker.remaining_quantity > 0) {
            Order& maker = queue.front();
            Quantity required_quantity = taker.remaining_quantity;
            Quantity available_quantity = maker.remaining_quantity;
            Quantity tradeable_quantity = required_quantity <= available_quantity ? required_quantity : available_quantity;

            Trade trade {
                .maker = maker.order_id,
                .taker = taker.order_id,
                .price = maker.price,
                .quantity = tradeable_quantity
            };
            execution_sink.on_trade(trade); // emit the trade

            taker.remaining_quantity -= tradeable_quantity;
            maker.remaining_quantity -= tradeable_quantity;
            if(maker.remaining_quantity == 0) {
                queue.pop_front();
            }
        }
        return ;
    }

    void processOrder(Order& order, ExecutionSink& execution_sink, Price limit_price, bool rest_remainder) {
        switch(order.side) {
            case Side::Buy : {
                auto it = ask.begin();
                while((it != ask.end()) && (it->first <= limit_price) && (order.remaining_quantity > 0)) {
                    consumeLevel(order, it->second, execution_sink);
                    auto curr = it;
                    it++;
                    if((curr->second).empty()) ask.erase(curr);
                }
                if((order.remaining_quantity > 0) && rest_remainder) {
                    bid[order.price].push_back(order);
                }
                break;
            }
            case Side::Sell: {
                auto it = bid.begin();
                while((it != bid.end()) && (it->first >= limit_price) && (order.remaining_quantity > 0)) {
                    consumeLevel(order, it->second, execution_sink);
                    auto curr = it;
                    it++;
                    if((curr->second).empty()) bid.erase(curr);
                }
                if((order.remaining_quantity > 0) && rest_remainder) {
                    ask[order.price].push_back(order);
                }
                break;
            }
        }
    }

    void processLimitOrder(Order& order, ExecutionSink& execution_sink) {
        processOrder(order, execution_sink, order.price, true);
    }

    void processMarketOrder(Order& order, ExecutionSink& execution_sink) {
        Price aggressive = (order.side == Side::Buy) ? std::numeric_limits<Price>::max() : std::numeric_limits<Price>::min();
        processOrder(order, execution_sink, aggressive, false);
    }

    // FOK pre-check: is there enough resting liquidity, at prices that cross
    // the order's limit, to fill it completely right now? Walks the opposite
    // side summing quantity, stopping early once we have enough or run past
    // the limit price.
    bool canFullyFill(const Order& order) const {
        Quantity need = order.remaining_quantity;
        Quantity avail = 0;
        if (order.side == Side::Buy) {
            for (const auto& [price, queue] : ask) {       // ascending
                if (price > order.price) break;
                for (const Order& maker : queue) {
                    avail += maker.remaining_quantity;
                    if (avail >= need) return true;
                }
            }
        } else {
            for (const auto& [price, queue] : bid) {       // descending
                if (price < order.price) break;
                for (const Order& maker : queue) {
                    avail += maker.remaining_quantity;
                    if (avail >= need) return true;
                }
            }
        }
        return avail >= need;
    }
    // I guess we can return a vector<Trade> so the async processing of these orders occur out of the hot loop
    // But there's a catch that the opposite side is completely empty in the starting itself then are we going to return back a Trade or not 
    // Keeping that in mind it seems that it is better to take a ExecutionSink object via reference and then call on_tade or on_exec, etc. 

public:

    void process(Order& order, ExecutionSink& execution_sink) {
        if(order.order_type == OrderType::Market) { processMarketOrder(order, execution_sink); return; }
        // Limit order: time-in-force decides what happens to the remainder.
        switch(order.time_in_force) {
            case TimeInForce::GTC:
                processOrder(order, execution_sink, order.price, true);   // rest remainder
                break;
            case TimeInForce::IOC:
                processOrder(order, execution_sink, order.price, false);  // drop remainder
                break;
            case TimeInForce::FOK:
                if(canFullyFill(order))
                    processOrder(order, execution_sink, order.price, false);  // all-or-nothing
                break;
        }
    }

    std::optional<Price> getBestBid() const {
        if(bid.empty()) return std::nullopt;
        return (*bid.begin()).first;
    }

    std::optional<Price> getBestAsk() const {
        if(ask.empty()) return std::nullopt;
        return (*ask.begin()).first;
    }

    // this will return true if found and deleted otherwise false
    bool cancelOrder(OrderId order_id) {
        for(auto level = ask.begin(); level != ask.end(); level++) {
            std::list<Order>& queue = level->second;
            for(auto it = queue.begin(); it != queue.end(); it++) {
                if(it->order_id == order_id) {
                    queue.erase(it);
                    if(queue.empty()) {
                        ask.erase(level);
                    }
                    return true;
                }
            }
        }

        for(auto level = bid.begin(); level != bid.end(); level++) {
            std::list<Order>& queue = level->second;
            for(auto it = queue.begin(); it != queue.end(); it++) {
                if(it->order_id == order_id) {
                    queue.erase(it);
                    if(queue.empty()) {
                        bid.erase(level);
                    }
                    return true;
                }
            }
        }
        return false;
    }
};
