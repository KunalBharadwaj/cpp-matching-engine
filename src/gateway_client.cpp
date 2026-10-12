// Minimal client for the order-entry gateway: connects, sends a scripted
// sequence of orders, and prints the decoded execution reports it receives.
// Demonstrates the full path (wire protocol + risk + matching + reports).
//
//   ./gateway_client [port]        (default 7001; start gateway_server first)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/gateway.hpp"

static bool write_all(int fd, const void* buf, std::size_t n) {
    const char* p = static_cast<const char*>(buf);
    for (std::size_t off = 0; off < n; ) {
        ssize_t w = ::write(fd, p + off, n - off);
        if (w <= 0) return false;
        off += static_cast<std::size_t>(w);
    }
    return true;
}

static wire::InboundMsg mk(std::uint8_t type, OrderId id, int side, Price px, Quantity q,
                           int ot = 0, int tif = 0) {
    wire::InboundMsg m{};
    m.type = type; m.side = (std::uint8_t)side; m.order_type = (std::uint8_t)ot; m.tif = (std::uint8_t)tif;
    m.order_id = id; m.price = px; m.quantity = q;
    return m;
}

static const char* type_name(std::uint8_t t) {
    switch (t) {
        case wire::ACK: return "ACK ";    case wire::FILL: return "FILL";
        case wire::DONE: return "DONE";   case wire::CANCELED: return "CXLD";
        case wire::REJECT: return "RJCT"; default: return "????";
    }
}
static const char* reason_name(std::uint8_t r) {
    switch (r) {
        case wire::BAD_SIZE: return "bad-size";      case wire::PRICE_BAND: return "price-band";
        case wire::POSITION_LIMIT: return "position"; case wire::UNKNOWN_ORDER: return "unknown-order";
        case wire::DUP_ID: return "dup-id";           case wire::BAD_MESSAGE: return "bad-message";
        default: return "";
    }
}

int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 7001;

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("connect (is gateway_server running?)"); return 1;
    }

    // Scripted scenario (prices assume the server's default band [1,100000]).
    wire::InboundMsg script[] = {
        mk(wire::NEW,    1, /*sell*/1, 50, 100),          // rests            -> ACK
        mk(wire::NEW,    2, /*buy*/0, 5000000, 10),       // above price band -> REJECT
        mk(wire::NEW,    3, /*buy*/0, 50, 60),            // crosses 60       -> FILLs + DONE
        mk(wire::CANCEL, 1, 0, 0, 0),                     // cancel the rest of #1 -> CXLD
    };
    const char* note[] = {
        "NEW  SELL 100 @ 50", "NEW  BUY 10 @ 5000000 (out of band)",
        "NEW  BUY 60 @ 50 (crosses)", "CANCEL #1",
    };
    for (std::size_t i = 0; i < sizeof(script) / sizeof(script[0]); ++i) {
        std::printf(">> %s\n", note[i]);
        write_all(fd, &script[i], sizeof(script[i]));
    }

    // Read responses until the socket goes quiet (300 ms).
    timeval tv{0, 300 * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::uint8_t buf[4096]; std::size_t have = 0;
    for (;;) {
        ssize_t r = ::read(fd, buf + have, sizeof(buf) - have);
        if (r <= 0) break;                                 // EOF or timeout
        have += (std::size_t)r;
        std::size_t off = 0;
        while (have - off >= sizeof(wire::OutboundMsg)) {
            wire::OutboundMsg m;
            std::memcpy(&m, buf + off, sizeof(m));
            off += sizeof(m);
            if (m.type == wire::REJECT)
                std::printf("<< %s  order #%llu  (%s)\n", type_name(m.type),
                            (unsigned long long)m.order_id, reason_name(m.reason));
            else if (m.type == wire::FILL)
                std::printf("<< %s  order #%llu  %llu @ %lld  (vs #%llu)\n", type_name(m.type),
                            (unsigned long long)m.order_id, (unsigned long long)m.quantity,
                            (long long)m.price, (unsigned long long)m.counter_id);
            else
                std::printf("<< %s  order #%llu  qty %llu\n", type_name(m.type),
                            (unsigned long long)m.order_id, (unsigned long long)m.quantity);
        }
        std::memmove(buf, buf + off, have - off);
        have -= off;
    }
    ::close(fd);
    return 0;
}
