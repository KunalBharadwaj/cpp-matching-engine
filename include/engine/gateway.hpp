#pragma once
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <vector>
#include "engine/order.hpp"
#include "engine/trade.hpp"
#include "engine/execution_sink.hpp"
#include "engine/fast_order_book.hpp"

// ---- binary wire protocol --------------------------------------------------
// Fixed-size messages (trivial framing: one message = N bytes). Host byte order
// -- both ends are on the same box here; a production protocol would pin
// endianness (e.g. big-endian / network order). Explicit padding keeps the
// layout deterministic, verified by the static_asserts below.

namespace wire {

// inbound (client -> gateway) message types
inline constexpr std::uint8_t NEW    = 'N';
inline constexpr std::uint8_t CANCEL = 'C';

// outbound (gateway -> client) message types
inline constexpr std::uint8_t ACK      = 'A';   // order accepted and resting
inline constexpr std::uint8_t FILL     = 'F';   // a (partial) fill
inline constexpr std::uint8_t DONE     = 'D';   // order fully filled / remainder dropped
inline constexpr std::uint8_t CANCELED = 'X';   // cancel acknowledged
inline constexpr std::uint8_t REJECT   = 'R';   // rejected (see reason)

// reject reasons
enum Reason : std::uint8_t {
    OK = 0, BAD_SIZE = 1, PRICE_BAND = 2, POSITION_LIMIT = 3,
    UNKNOWN_ORDER = 4, DUP_ID = 5, BAD_MESSAGE = 6,
};

struct InboundMsg {
    std::uint8_t  type;          // NEW / CANCEL
    std::uint8_t  side;          // 0 buy, 1 sell
    std::uint8_t  order_type;    // 0 limit, 1 market
    std::uint8_t  tif;           // 0 GTC, 1 IOC, 2 FOK
    std::uint32_t _pad;
    std::uint64_t order_id;
    std::int64_t  price;
    std::uint64_t quantity;
};
static_assert(sizeof(InboundMsg) == 32, "InboundMsg must be 32 bytes");

struct OutboundMsg {
    std::uint8_t  type;          // ACK / FILL / DONE / CANCELED / REJECT
    std::uint8_t  reason;        // Reason (REJECT only)
    std::uint16_t _pad;
    std::uint32_t _pad2;
    std::uint64_t order_id;      // the order this message is about
    std::uint64_t counter_id;    // counterparty order id (FILL), else 0
    std::int64_t  price;         // fill price / order price
    std::uint64_t quantity;      // fill qty, or resting/remaining qty
};
static_assert(sizeof(OutboundMsg) == 40, "OutboundMsg must be 40 bytes");

}  // namespace wire

// Pre-trade risk limits, checked at ingress before anything reaches the book.
struct RiskLimits {
    Quantity max_order_qty = 1'000'000;   // fat-finger size guard
    Price    min_price     = 1;           // price band (collar)
    Price    max_price     = 1'000'000;
    long     max_position  = 10'000'000;  // per-connection net position cap
};

// The gateway's pure request->responses logic: pre-trade risk, matching, and
// routing execution reports to the right connections. No sockets here, so it is
// unit-testable; the epoll server is a thin transport shell over this.
class GatewayCore {
public:
    using Emit = std::function<void(int /*fd*/, const wire::OutboundMsg&)>;

    // The risk price band IS the book's valid price range: any order that
    // passes the band check is guaranteed in-range for the ladder.
    GatewayCore(RiskLimits risk, Price tick, Emit emit)
        : book_(risk.min_price, risk.max_price, tick), risk_(risk), emit_(std::move(emit)) {}

    void on_inbound(int fd, const wire::InboundMsg& m) {
        switch (m.type) {
            case wire::NEW:    handle_new(fd, m);    break;
            case wire::CANCEL: handle_cancel(fd, m); break;
            default:           emit_(fd, make(wire::REJECT, m.order_id, 0, 0, 0, wire::BAD_MESSAGE));
        }
    }

    // Drop a disconnected client's exposure (its resting orders stay live in
    // the book, as on a real venue).
    void on_disconnect(int fd) { exposure_.erase(fd); }

private:
    struct Resting { int fd; Quantity remaining; Side side; };

    struct CaptureSink : ExecutionSink {
        std::vector<Trade> trades;
        void on_trade(const Trade& t) override { trades.push_back(t); }
    };

    FastOrderBook book_;
    RiskLimits risk_;
    Emit emit_;
    CaptureSink sink_;
    std::unordered_map<OrderId, Resting> live_;   // order_id -> owner fd + remaining + side
    // Per-connection net committed exposure = realized position + open orders.
    // Reserved in full when an order is accepted; the unfilled part is released
    // when an order is cancelled or its remainder dropped (IOC/FOK/market). A
    // fully-filled order keeps its reservation (it became a realized position).
    std::unordered_map<int, long> exposure_;

    static long signed_qty(Side s, Quantity q) {
        return (s == Side::Buy) ? static_cast<long>(q) : -static_cast<long>(q);
    }
    void release(int fd, Side side, Quantity qty) { exposure_[fd] -= signed_qty(side, qty); }

    static wire::OutboundMsg make(std::uint8_t type, OrderId id, OrderId counter,
                                  Price price, Quantity qty, std::uint8_t reason = 0) {
        wire::OutboundMsg o{};
        o.type = type; o.reason = reason; o.order_id = id; o.counter_id = counter;
        o.price = price; o.quantity = qty;
        return o;
    }

    void handle_new(int fd, const wire::InboundMsg& m) {
        const Side side = m.side ? Side::Sell : Side::Buy;
        const OrderType ot = m.order_type ? OrderType::Market : OrderType::Limit;
        const TimeInForce tif = m.tif == 1 ? TimeInForce::IOC
                              : m.tif == 2 ? TimeInForce::FOK : TimeInForce::GTC;

        // ---- pre-trade risk checks (reject before the book sees it) ----
        if (m.quantity == 0 || m.quantity > risk_.max_order_qty)
            return emit_(fd, make(wire::REJECT, m.order_id, 0, 0, 0, wire::BAD_SIZE));
        if (ot == OrderType::Limit && (m.price < risk_.min_price || m.price > risk_.max_price))
            return emit_(fd, make(wire::REJECT, m.order_id, 0, 0, 0, wire::PRICE_BAND));
        const long delta = signed_qty(side, m.quantity);
        if (std::labs(exposure_[fd] + delta) > risk_.max_position)   // realized + open orders
            return emit_(fd, make(wire::REJECT, m.order_id, 0, 0, 0, wire::POSITION_LIMIT));
        if (live_.find(m.order_id) != live_.end())
            return emit_(fd, make(wire::REJECT, m.order_id, 0, 0, 0, wire::DUP_ID));

        // ---- match ----
        exposure_[fd] += delta;                          // reserve the full order
        Order order{ m.order_id, m.price, m.quantity, m.quantity, side, ot, tif };
        live_[m.order_id] = { fd, m.quantity, side };    // register (may end up resting)
        sink_.trades.clear();
        book_.process(order, sink_);

        for (const Trade& t : sink_.trades) {
            emit_(fd, make(wire::FILL, t.taker, t.maker, t.price, t.quantity));   // to taker (this client)
            auto mi = live_.find(t.maker);               // route to the maker's owner
            if (mi != live_.end()) {
                emit_(mi->second.fd, make(wire::FILL, t.maker, t.taker, t.price, t.quantity));
                mi->second.remaining -= t.quantity;      // (reservation kept: fill -> realized)
                if (mi->second.remaining == 0) {
                    emit_(mi->second.fd, make(wire::DONE, t.maker, 0, 0, 0));
                    live_.erase(mi);
                }
            }
        }

        // ---- taker outcome ----
        if (order.remaining_quantity == 0) {
            emit_(fd, make(wire::DONE, m.order_id, 0, 0, 0));
            live_.erase(m.order_id);                     // fully filled -> reservation kept
        } else if (ot == OrderType::Limit && tif == TimeInForce::GTC) {
            live_[m.order_id].remaining = order.remaining_quantity;   // rested -> reservation kept
            emit_(fd, make(wire::ACK, m.order_id, 0, m.price, order.remaining_quantity));
        } else {
            release(fd, side, order.remaining_quantity); // IOC/FOK/market remainder dropped
            live_.erase(m.order_id);
            emit_(fd, make(wire::DONE, m.order_id, 0, 0, order.remaining_quantity));
        }
    }

    void handle_cancel(int fd, const wire::InboundMsg& m) {
        auto it = live_.find(m.order_id);
        if (it == live_.end() || it->second.fd != fd)    // unknown, or not the owner
            return emit_(fd, make(wire::REJECT, m.order_id, 0, 0, 0, wire::UNKNOWN_ORDER));
        release(it->second.fd, it->second.side, it->second.remaining);   // free the open exposure
        book_.cancelOrder(m.order_id);
        live_.erase(it);
        emit_(fd, make(wire::CANCELED, m.order_id, 0, 0, 0));
    }
};
