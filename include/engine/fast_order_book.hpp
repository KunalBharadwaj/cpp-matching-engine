#pragma once

#include <vector>
#include <unordered_map>
#include <optional>
#include <limits>
#include <algorithm>
#include <cstdint>
#include <cassert>
#include "engine/order.hpp"
#include "engine/trade.hpp"
#include "engine/execution_sink.hpp"


// A single order living in the pool. The next/prev links are INTRUSIVE --
// they live inside the node itself, so a resting order costs no separate
// list-node allocation. They are indices into FastOrderBook::nodes (not
// pointers) so the pool can grow/reallocate without dangling, and so each
// link is 4 bytes instead of 8. NIL (-1) marks "no node".
struct PoolNode {
    Order order{};
    std::int32_t next{-1};
    std::int32_t prev{-1};
};

// One rung of the ladder. It no longer owns its orders -- it just points at
// the head/tail of its intrusive FIFO chain inside the shared pool.
// total_quantity is the running resting depth at this price (O(1) depth).
struct PriceLevel {
    std::int32_t head{-1};
    std::int32_t tail{-1};
    Quantity total_quantity{};
};

// Phase 2 book: flat price ladder + object pool + intrusive FIFO.
//   * ladder: bid[i]/ask[i] hold the orders at indexToPrice(i); O(1) price lookup.
//   * pool:   one contiguous vector<PoolNode>; alloc/free = free-list pop/push,
//             so there is no malloc on the hot path.
//   * cursors: best_bid_idx/best_ask_idx replace what map::begin() gave the oracle.
class FastOrderBook {
    static constexpr std::int32_t NIL = -1;

    std::size_t size{};
    Price min_price{}, max_price{}, tick_size{};

    std::vector<PriceLevel> bid;
    std::vector<PriceLevel> ask;

    // The object pool and its free list (free nodes are chained via `next`).
    std::vector<PoolNode> nodes;
    std::int32_t free_head{NIL};

    // O(1) cancel registry. INVARIANT: an order_id maps to its pool index
    // exactly while that order is resting in the book -- inserted on rest,
    // erased on full fill or cancel. (A flat/open-addressing hash map would be
    // a cache win over std::unordered_map; deferred to the Phase 4 perf pass.)
    std::unordered_map<OrderId, std::int32_t> id_to_node;

    // INVARIANT: best_bid_idx = highest non-empty bid index (head != NIL), or
    // NIL; best_ask_idx = lowest non-empty ask index, or NIL. Every mutation
    // keeps these true.
    std::int64_t best_bid_idx{NIL};
    std::int64_t best_ask_idx{NIL};

    std::int64_t priceToIndex(Price price) const { return (price - min_price) / tick_size; }
    Price indexToPrice(std::int64_t index) const { return min_price + index * tick_size; }

    // ---- object pool -------------------------------------------------------

    // Append a fresh block of nodes and chain them onto the free list. This is
    // the ONLY allocation path; with a well-sized pool it never runs on the
    // hot path. Called only when the free list is exhausted.
    void grow() {
        std::size_t old = nodes.size();
        std::size_t add = old ? old : 1024;          // double, or seed with 1024
        nodes.resize(old + add);
        for (std::size_t i = nodes.size(); i-- > old; ) {
            nodes[i].next = free_head;
            free_head = static_cast<std::int32_t>(i);
        }
    }

    // Pop a node off the free list and store the order in it. May grow() --
    // callers must not hold a PoolNode& across this call (indices are fine).
    std::int32_t allocNode(const Order& order) {
        if (free_head == NIL) grow();
        std::int32_t idx = free_head;
        free_head = nodes[idx].next;
        nodes[idx].order = order;
        nodes[idx].next = NIL;
        nodes[idx].prev = NIL;
        return idx;
    }

    void freeNode(std::int32_t idx) {
        nodes[idx].next = free_head;
        free_head = idx;
    }

    // ---- intrusive doubly-linked list (O(1) push/unlink) -------------------

    void listPushBack(PriceLevel& level, std::int32_t idx) {
        nodes[idx].prev = level.tail;
        nodes[idx].next = NIL;
        if (level.tail != NIL) nodes[level.tail].next = idx;
        else                   level.head = idx;      // level was empty
        level.tail = idx;
    }

    void listUnlink(PriceLevel& level, std::int32_t idx) {
        std::int32_t p = nodes[idx].prev, n = nodes[idx].next;
        if (p != NIL) nodes[p].next = n; else level.head = n;
        if (n != NIL) nodes[n].prev = p; else level.tail = p;
    }

    // ---- cursors -----------------------------------------------------------

    void advanceBestAsk() {
        std::int64_t i = best_ask_idx + 1;
        while (i < static_cast<std::int64_t>(size) && ask[i].head == NIL) i++;
        best_ask_idx = (i < static_cast<std::int64_t>(size)) ? i : NIL;
    }

    void advanceBestBid() {
        std::int64_t i = best_bid_idx - 1;
        while (i >= 0 && bid[i].head == NIL) i--;
        best_bid_idx = (i >= 0) ? i : NIL;
    }

    // ---- matching ----------------------------------------------------------

    void addToBook(Order& order) {
        std::int64_t idx = priceToIndex(order.price);
        assert(idx >= 0 && idx < static_cast<std::int64_t>(size));
        std::int32_t node = allocNode(order);          // may grow the pool
        id_to_node[order.order_id] = node;             // now resting -> registered
        if (order.side == Side::Buy) {
            listPushBack(bid[idx], node);
            bid[idx].total_quantity += order.remaining_quantity;
            if (best_bid_idx == NIL || idx > best_bid_idx) best_bid_idx = idx;
        } else {
            listPushBack(ask[idx], node);
            ask[idx].total_quantity += order.remaining_quantity;
            if (best_ask_idx == NIL || idx < best_ask_idx) best_ask_idx = idx;
        }
    }

    // FIFO fill at one level. No allocation happens here, so the maker
    // reference into the pool stays valid for the whole call.
    void consumeLevel(Order& taker, PriceLevel& level, ExecutionSink& execution_sink) {
        while (level.head != NIL && taker.remaining_quantity > 0) {
            std::int32_t maker_idx = level.head;
            Order& maker = nodes[maker_idx].order;
            Quantity tradeable_quantity =
                std::min(taker.remaining_quantity, maker.remaining_quantity);

            Trade trade {
                .maker = maker.order_id,
                .taker = taker.order_id,
                .price = maker.price,
                .quantity = tradeable_quantity
            };
            execution_sink.on_trade(trade);

            taker.remaining_quantity -= tradeable_quantity;
            maker.remaining_quantity -= tradeable_quantity;
            level.total_quantity     -= tradeable_quantity;
            if (maker.remaining_quantity == 0) {
                id_to_node.erase(maker.order_id);      // left the book
                listUnlink(level, maker_idx);          // O(1) via prev/next
                freeNode(maker_idx);
            }
        }
    }

    void processOrder(Order& order, ExecutionSink& execution_sink,
                      Price limit_price, bool rest_remainder) {
        switch (order.side) {
            case Side::Buy: {
                while (best_ask_idx != NIL
                       && indexToPrice(best_ask_idx) <= limit_price
                       && order.remaining_quantity > 0) {
                    consumeLevel(order, ask[best_ask_idx], execution_sink);
                    if (ask[best_ask_idx].head == NIL) advanceBestAsk();
                }
                if (order.remaining_quantity > 0 && rest_remainder) addToBook(order);
                break;
            }
            case Side::Sell: {
                while (best_bid_idx != NIL
                       && indexToPrice(best_bid_idx) >= limit_price
                       && order.remaining_quantity > 0) {
                    consumeLevel(order, bid[best_bid_idx], execution_sink);
                    if (bid[best_bid_idx].head == NIL) advanceBestBid();
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
        Price aggressive = (order.side == Side::Buy)
            ? std::numeric_limits<Price>::max()
            : std::numeric_limits<Price>::min();
        processOrder(order, execution_sink, aggressive, false);
    }

    // FOK pre-check. Cheaper than the oracle's: each level already carries its
    // total_quantity, so we sum per level instead of per order.
    bool canFullyFill(const Order& order) const {
        Quantity need = order.remaining_quantity;
        Quantity avail = 0;
        if (order.side == Side::Buy) {
            for (std::int64_t i = best_ask_idx;
                 i != NIL && i < static_cast<std::int64_t>(size) && indexToPrice(i) <= order.price;
                 ++i) {
                avail += ask[i].total_quantity;
                if (avail >= need) return true;
            }
        } else {
            for (std::int64_t i = best_bid_idx;
                 i >= 0 && indexToPrice(i) >= order.price;
                 --i) {
                avail += bid[i].total_quantity;
                if (avail >= need) return true;
            }
        }
        return avail >= need;
    }

public:

    // pool_capacity pre-sizes the free list so steady-state inserts never
    // grow(). Pick it above the peak number of simultaneously-resting orders.
    FastOrderBook(Price min_price, Price max_price, Price tick_size,
                  std::size_t pool_capacity = (1u << 16))
        : size(static_cast<std::size_t>((max_price - min_price) / tick_size) + 1),
          min_price(min_price), max_price(max_price), tick_size(tick_size),
          bid(size), ask(size) {
        nodes.resize(pool_capacity);
        for (std::size_t i = pool_capacity; i-- > 0; ) {   // index 0 handed out first
            nodes[i].next = free_head;
            free_head = static_cast<std::int32_t>(i);
        }
    }

    void process(Order& order, ExecutionSink& execution_sink) {
        if (order.order_type == OrderType::Market) { processMarketOrder(order, execution_sink); return; }
        switch (order.time_in_force) {
            case TimeInForce::GTC:
                processOrder(order, execution_sink, order.price, true);
                break;
            case TimeInForce::IOC:
                processOrder(order, execution_sink, order.price, false);
                break;
            case TimeInForce::FOK:
                if (canFullyFill(order))
                    processOrder(order, execution_sink, order.price, false);
                break;
        }
    }

    std::optional<Price> getBestBid() const {
        if (best_bid_idx == NIL) return std::nullopt;
        return indexToPrice(best_bid_idx);
    }

    std::optional<Price> getBestAsk() const {
        if (best_ask_idx == NIL) return std::nullopt;
        return indexToPrice(best_ask_idx);
    }

    // Cancel-replace with a priority-preserving fast path, O(1) via the
    // registry. Returns false if no such order is resting; new_quantity == 0
    // is treated as a cancel.
    bool modifyOrder(OrderId order_id, Price new_price, Quantity new_quantity,
                     ExecutionSink& execution_sink) {
        if (new_quantity == 0) return cancelOrder(order_id);

        auto it = id_to_node.find(order_id);
        if (it == id_to_node.end()) return false;
        std::int32_t node = it->second;
        Order& o = nodes[node].order;

        // Same price and smaller size -> shrink in place, keep queue position.
        if (new_price == o.price && new_quantity <= o.remaining_quantity) {
            std::int64_t idx = priceToIndex(o.price);
            Quantity delta = o.remaining_quantity - new_quantity;
            if (o.side == Side::Buy) bid[idx].total_quantity -= delta;
            else                     ask[idx].total_quantity -= delta;
            o.remaining_quantity = new_quantity;
            return true;
        }

        // Otherwise lose priority: cancel (frees the node) then re-submit.
        // Capture side first; `o` dangles after cancelOrder frees the node.
        Side side = o.side;
        cancelOrder(order_id);
        Order replacement{
            .order_id = order_id,
            .price = new_price,
            .original_quantity = new_quantity,
            .remaining_quantity = new_quantity,
            .side = side,
            .order_type = OrderType::Limit,
            .time_in_force = TimeInForce::GTC,
        };
        process(replacement, execution_sink);
        return true;
    }

    // O(1): registry lookup -> O(1) unlink. We read side/price back off the
    // node itself, so the registry only has to store the index.
    bool cancelOrder(OrderId order_id) {
        auto it = id_to_node.find(order_id);
        if (it == id_to_node.end()) return false;      // not resting (never was, or already gone)

        std::int32_t node = it->second;
        Order& o = nodes[node].order;
        std::int64_t idx = priceToIndex(o.price);

        if (o.side == Side::Buy) {
            bid[idx].total_quantity -= o.remaining_quantity;
            listUnlink(bid[idx], node);
            if (bid[idx].head == NIL && idx == best_bid_idx) advanceBestBid();
        } else {
            ask[idx].total_quantity -= o.remaining_quantity;
            listUnlink(ask[idx], node);
            if (ask[idx].head == NIL && idx == best_ask_idx) advanceBestAsk();
        }
        freeNode(node);
        id_to_node.erase(it);
        return true;
    }
};
