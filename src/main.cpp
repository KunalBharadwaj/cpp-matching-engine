// Narrated demo of the matching engine. Walks a FastOrderBook through every
// supported operation and prints the resulting trades and book after each
// step, so the engine's behaviour is visible at a glance.
//
//   cmake -S . -B build && cmake --build build --target engine && ./build/engine

#include <cstdio>
#include <string>
#include <vector>

#include "engine/order.hpp"
#include "engine/fast_order_book.hpp"
#include "engine/execution_sink.hpp"

// Prints each fill as the book emits it.
struct PrintingSink : ExecutionSink {
    int count = 0;
    void on_trade(const Trade& t) override {
        std::printf("      → TRADE  %4llu @ %-4lld   (maker #%llu  taker #%llu)\n",
                    static_cast<unsigned long long>(t.quantity),
                    static_cast<long long>(t.price),
                    static_cast<unsigned long long>(t.maker),
                    static_cast<unsigned long long>(t.taker));
        ++count;
    }
};

static const char* side_str(Side s)  { return s == Side::Buy ? "BUY " : "SELL"; }

// Render the book as a classic ladder: asks on top (high price down to best
// ask), a mid line, then bids (best bid down). Quantities shown per level.
static void print_book(const FastOrderBook& book) {
    auto bids = book.depth(Side::Buy, 5);
    auto asks = book.depth(Side::Sell, 5);

    std::printf("        %8s %6s %6s\n", "price", "bid", "ask");
    for (auto it = asks.rbegin(); it != asks.rend(); ++it)
        std::printf("        %8lld %6s %6llu\n",
                    static_cast<long long>(it->first), "",
                    static_cast<unsigned long long>(it->second));
    std::printf("        --------------------------\n");
    for (const auto& [price, qty] : bids)
        std::printf("        %8lld %6llu\n",
                    static_cast<long long>(price), static_cast<unsigned long long>(qty));

    auto bb = book.getBestBid();
    auto ba = book.getBestAsk();
    std::printf("        best bid %s   best ask %s\n\n",
                bb ? std::to_string(*bb).c_str() : "-",
                ba ? std::to_string(*ba).c_str() : "-");
}

int step = 0;

static Order limit(OrderId id, Side s, Price p, Quantity q,
                   TimeInForce tif = TimeInForce::GTC) {
    return Order{ id, p, q, q, s, OrderType::Limit, tif };
}
static Order market(OrderId id, Side s, Quantity q) {
    return Order{ id, 0, q, q, s, OrderType::Market, TimeInForce::GTC };
}

int main() {
    FastOrderBook book(/*min*/1, /*max*/1000, /*tick*/1);
    PrintingSink sink;
    ExecutionSink& es = sink;

    auto send = [&](const char* note, Order o) {
        const char* tif = o.time_in_force == TimeInForce::IOC ? " IOC"
                        : o.time_in_force == TimeInForce::FOK ? " FOK" : "";
        const char* type = o.order_type == OrderType::Market ? "MKT" : "LMT";
        std::printf("[%d] NEW  #%llu  %s %4llu %s%s  %s\n", ++step,
                    static_cast<unsigned long long>(o.order_id), side_str(o.side),
                    static_cast<unsigned long long>(o.original_quantity),
                    o.order_type == OrderType::Market ? "" : ("@ " + std::to_string(o.price)).c_str(),
                    tif, note);
        int before = sink.count;
        book.process(o, es);
        if (sink.count == before) std::printf("      (no fill)\n");
        print_book(book);
    };
    auto cancel = [&](const char* note, OrderId id) {
        std::printf("[%d] CANCEL #%llu  %s\n", ++step, static_cast<unsigned long long>(id), note);
        std::printf("      %s\n", book.cancelOrder(id) ? "(removed)" : "(not found)");
        print_book(book);
    };
    auto modify = [&](const char* note, OrderId id, Price np, Quantity nq) {
        std::printf("[%d] MODIFY #%llu -> %llu @ %lld  %s\n", ++step,
                    static_cast<unsigned long long>(id),
                    static_cast<unsigned long long>(nq), static_cast<long long>(np), note);
        int before = sink.count;
        bool ok = book.modifyOrder(id, np, nq, es);
        if (sink.count == before) std::printf("      %s\n", ok ? "(adjusted)" : "(not found)");
        print_book(book);
    };

    std::printf("=== Matching Engine Demo (price-time priority) ===\n\n");

    std::printf("--- 1. Build a two-sided book -------------------------------\n");
    send("resting ask", limit(1, Side::Sell, 102, 5));
    send("resting ask", limit(2, Side::Sell, 101, 10));
    send("resting bid", limit(3, Side::Buy, 99, 8));
    send("resting bid", limit(4, Side::Buy, 98, 4));

    std::printf("--- 2. A limit buy that crosses, then rests the remainder ---\n");
    send("buy 12 @ 101: lifts the 101 ask (10), rests 2 @ 101", limit(5, Side::Buy, 101, 12));

    std::printf("--- 3. A market sell sweeping the bids ----------------------\n");
    send("market sell 11: hits 101(2), 99(8), 98(1)", market(6, Side::Sell, 11));

    std::printf("--- 4. Cancel a resting order -------------------------------\n");
    cancel("remove the rest of bid #4 @ 98", 4);

    std::printf("--- 5. Rebuild, then modify (reduce keeps priority) ---------\n");
    cancel("clear leftover ask #1 @102 so 105 is the best ask", 1);
    send("resting ask", limit(7, Side::Sell, 105, 10));
    send("resting ask (behind #7 at same price)", limit(8, Side::Sell, 105, 5));
    modify("shrink #7 10->3 at same price: keeps its spot in line", 7, 105, 3);
    send("buy 4 @ 105: #7 (3) fills first, then #8 (1) -- priority proven",
         limit(9, Side::Buy, 105, 4));

    std::printf("--- 6. Modify that reprices (loses priority, may cross) -----\n");
    send("resting bid", limit(10, Side::Buy, 100, 5));
    modify("reprice resting ask #8 down to 100: now crosses bid #10", 8, 100, 4);

    std::printf("--- 7. IOC and FOK time-in-force ----------------------------\n");
    send("resting ask", limit(11, Side::Sell, 110, 4));
    send("IOC buy 10 @ 110: fills 4, DROPS the other 6 (never rests)",
         limit(12, Side::Buy, 110, 10, TimeInForce::IOC));
    send("resting ask", limit(13, Side::Sell, 110, 4));
    send("FOK buy 10 @ 110: only 4 available -> KILLED, nothing happens",
         limit(14, Side::Buy, 110, 10, TimeInForce::FOK));
    send("FOK buy 4 @ 110: fully fillable -> executes",
         limit(15, Side::Buy, 110, 4, TimeInForce::FOK));

    std::printf("Demo complete. See README.md for design + benchmarks.\n");
    return 0;
}
