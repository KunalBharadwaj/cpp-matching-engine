// epoll-based TCP order-entry gateway. A thin transport shell over GatewayCore:
// it frames the fixed-size binary protocol off each socket, hands messages to
// the core (risk + matching + exec-report routing), and writes the core's
// outbound messages back to the right client fds.
//
//   ./gateway_server [port] [tick] [min_price] [max_price]   (default 7001 1 1 100000)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "engine/gateway.hpp"

static void set_nonblocking(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static bool write_all(int fd, const void* buf, std::size_t n) {
    const char* p = static_cast<const char*>(buf);
    std::size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, p + off, n - off);
        if (w > 0) off += static_cast<std::size_t>(w);
        else if (w < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        else return false;
    }
    return true;
}

int main(int argc, char** argv) {
    int   port = (argc > 1) ? std::atoi(argv[1]) : 7001;
    Price tick = (argc > 2) ? std::atoll(argv[2]) : 1;
    RiskLimits risk;
    if (argc > 3) risk.min_price = std::atoll(argv[3]);
    if (argc > 4) risk.max_price = std::atoll(argv[4]);

    int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind"); return 1;
    }
    ::listen(listen_fd, 64);
    set_nonblocking(listen_fd);

    int ep = ::epoll_create1(0);
    epoll_event ev{}; ev.events = EPOLLIN; ev.data.fd = listen_fd;
    ::epoll_ctl(ep, EPOLL_CTL_ADD, listen_fd, &ev);

    // Per-connection inbound byte buffer (TCP is a stream; frame fixed 32-byte msgs).
    std::unordered_map<int, std::vector<std::uint8_t>> rbuf;

    GatewayCore core(risk, tick,
        [](int fd, const wire::OutboundMsg& m) { write_all(fd, &m, sizeof(m)); });

    std::printf("gateway: listening on 127.0.0.1:%d  (price band [%lld,%lld], tick %lld)\n",
                port, (long long)risk.min_price, (long long)risk.max_price, (long long)tick);
    std::fflush(stdout);

    std::vector<epoll_event> events(64);
    for (;;) {
        int n = ::epoll_wait(ep, events.data(), static_cast<int>(events.size()), -1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (fd == listen_fd) {
                for (;;) {                                 // drain the accept queue
                    int c = ::accept(listen_fd, nullptr, nullptr);
                    if (c < 0) break;
                    set_nonblocking(c);
                    int yes = 1; ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
                    epoll_event cev{}; cev.events = EPOLLIN; cev.data.fd = c;
                    ::epoll_ctl(ep, EPOLL_CTL_ADD, c, &cev);
                    rbuf[c];
                }
                continue;
            }
            // readable client socket
            std::uint8_t tmp[4096];
            bool closed = false;
            for (;;) {
                ssize_t r = ::read(fd, tmp, sizeof(tmp));
                if (r > 0) rbuf[fd].insert(rbuf[fd].end(), tmp, tmp + r);
                else if (r == 0) { closed = true; break; }
                else if (errno == EAGAIN) break;
                else if (errno == EINTR) continue;
                else { closed = true; break; }
            }
            auto& buf = rbuf[fd];
            while (buf.size() >= sizeof(wire::InboundMsg)) {    // frame + dispatch
                wire::InboundMsg msg;
                std::memcpy(&msg, buf.data(), sizeof(msg));
                buf.erase(buf.begin(), buf.begin() + sizeof(msg));
                core.on_inbound(fd, msg);
            }
            if (closed) {
                ::epoll_ctl(ep, EPOLL_CTL_DEL, fd, nullptr);
                ::close(fd);
                rbuf.erase(fd);
                core.on_disconnect(fd);
            }
        }
    }
    ::close(listen_fd);
    return 0;
}
