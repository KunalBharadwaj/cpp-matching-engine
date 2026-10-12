#include "gtest/gtest.h"
#include <vector>
#include <utility>
#include "engine/gateway.hpp"

namespace {

using Out = std::pair<int, wire::OutboundMsg>;

wire::InboundMsg mkNew(OrderId id, int side, Price px, Quantity q, int ot = 0, int tif = 0) {
    wire::InboundMsg m{};
    m.type = wire::NEW; m.side = static_cast<std::uint8_t>(side);
    m.order_type = static_cast<std::uint8_t>(ot); m.tif = static_cast<std::uint8_t>(tif);
    m.order_id = id; m.price = px; m.quantity = q;
    return m;
}
wire::InboundMsg mkCancel(OrderId id) {
    wire::InboundMsg m{}; m.type = wire::CANCEL; m.order_id = id; return m;
}

// Small price band so the test book's ladder is small.
inline RiskLimits testRisk() { RiskLimits r; r.min_price = 1; r.max_price = 1000; return r; }

// A core whose outbound messages are collected into `out` for inspection.
struct Harness {
    std::vector<Out> out;
    GatewayCore core;
    explicit Harness(RiskLimits r = testRisk())
        : core(r, /*tick*/1,
               [this](int fd, const wire::OutboundMsg& m) { out.push_back({fd, m}); }) {}
    void send(int fd, const wire::InboundMsg& m) { core.on_inbound(fd, m); }
};

}  // namespace

TEST(Gateway, RejectsOversizedOrder) {
    RiskLimits r = testRisk(); r.max_order_qty = 100;
    Harness h(r);
    h.send(1, mkNew(1, /*buy*/0, 100, 500));
    ASSERT_EQ(1u, h.out.size());
    EXPECT_EQ(wire::REJECT, h.out[0].second.type);
    EXPECT_EQ(wire::BAD_SIZE, h.out[0].second.reason);
}

TEST(Gateway, RejectsPriceOutOfBand) {
    Harness h;                                   // default band [1, 1000]
    h.send(1, mkNew(1, 0, 5000, 10));            // price above max
    ASSERT_EQ(1u, h.out.size());
    EXPECT_EQ(wire::REJECT, h.out[0].second.type);
    EXPECT_EQ(wire::PRICE_BAND, h.out[0].second.reason);
}

TEST(Gateway, RejectsOnPositionLimit) {
    RiskLimits r = testRisk(); r.max_position = 50;
    Harness h(r);
    h.send(1, mkNew(1, /*buy*/0, 100, 40));      // ok: projected +40
    h.send(1, mkNew(2, /*buy*/0, 100, 40));      // would reach +80 (resting+this) -> reject
    // first rests (no cross) -> ACK; second rejected on position
    EXPECT_EQ(wire::ACK, h.out[0].second.type);
    ASSERT_GE(h.out.size(), 2u);
    EXPECT_EQ(wire::REJECT, h.out.back().second.type);
    EXPECT_EQ(wire::POSITION_LIMIT, h.out.back().second.reason);
}

TEST(Gateway, RestingLimitAcked) {
    Harness h;
    h.send(7, mkNew(1, /*sell*/1, 105, 5));
    ASSERT_EQ(1u, h.out.size());
    EXPECT_EQ(wire::ACK, h.out[0].second.type);
    EXPECT_EQ(1u, h.out[0].second.order_id);
    EXPECT_EQ(5u, h.out[0].second.quantity);     // resting qty
}

TEST(Gateway, FillRoutedToBothParties) {
    Harness h;
    h.send(/*maker fd*/1, mkNew(10, /*sell*/1, 100, 5));   // rests -> ACK to fd 1
    h.send(/*taker fd*/2, mkNew(20, /*buy*/0, 100, 5));    // crosses fully

    // Collect types per fd.
    bool taker_fill = false, maker_fill = false, taker_done = false, maker_done = false;
    for (auto& [fd, m] : h.out) {
        if (fd == 2 && m.type == wire::FILL && m.order_id == 20) taker_fill = true;
        if (fd == 1 && m.type == wire::FILL && m.order_id == 10) maker_fill = true;
        if (fd == 2 && m.type == wire::DONE && m.order_id == 20) taker_done = true;
        if (fd == 1 && m.type == wire::DONE && m.order_id == 10) maker_done = true;
    }
    EXPECT_TRUE(taker_fill);     // both sides get an execution report
    EXPECT_TRUE(maker_fill);
    EXPECT_TRUE(taker_done);     // both fully filled
    EXPECT_TRUE(maker_done);
}

TEST(Gateway, CancelOwnershipEnforced) {
    Harness h;
    h.send(1, mkNew(10, /*sell*/1, 105, 5));     // fd 1 rests order 10
    h.send(2, mkCancel(10));                     // fd 2 tries to cancel it -> reject
    EXPECT_EQ(wire::REJECT, h.out.back().second.type);
    EXPECT_EQ(wire::UNKNOWN_ORDER, h.out.back().second.reason);
    h.send(1, mkCancel(10));                     // owner cancels -> ok
    EXPECT_EQ(wire::CANCELED, h.out.back().second.type);
}

TEST(Gateway, DuplicateOrderIdRejected) {
    Harness h;
    h.send(1, mkNew(10, /*sell*/1, 105, 5));     // rests
    h.send(1, mkNew(10, /*sell*/1, 106, 5));     // same id still live -> reject
    EXPECT_EQ(wire::REJECT, h.out.back().second.type);
    EXPECT_EQ(wire::DUP_ID, h.out.back().second.reason);
}
