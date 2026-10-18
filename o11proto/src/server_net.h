// o11proto - the server's Asio side. Internal: server.cpp and server_live.cpp.
//
// One io_context, run by Server::poll() and runFor() on the caller's thread
// (concurrency hint 1: nothing else runs it, so the handlers need no strand
// and no lock). Every wait is armed only after its socket was drained (read
// until it would block): Asio's waits are not speculative, and on epoll and
// kqueue a readiness that came before the wait was armed is not reported
// again. (The listening socket has an accept instead, which tries at once.)
#ifndef O11PROTO_SERVER_NET_H
#define O11PROTO_SERVER_NET_H

#include <vector>

#include "o11proto/server.h"
#include "socket_compat.h"

namespace o11proto {

struct Server::Net {
    asio::io_context io{1};
    asio::ip::tcp::acceptor acceptor{io};
    asio::ip::udp::socket relay{io};           // Live's UDP port (server_live.cpp)
    asio::steady_timer timer{io};
    std::vector<asio::ip::tcp::socket> accepted;   // by the accept, for the next pass
    unsigned epoch = 0;                        // counts start()s: a stopped server's waits do nothing
    bool accepting = false, accept_ready = false, relay_reading = false, timing = false, pass_due = false;
    // Runs ready handlers, waiting up to `ms` for the first (0: none, < 0:
    // as long as it takes). Bounded, as one pass of a poll() loop was: what is
    // still ready then runs at the next call.
    void run(int ms) {
        if (io.stopped()) io.restart();
        if (ms < 0) io.run_one();
        else if (ms > 0) io.run_one_for(std::chrono::milliseconds(ms));
        for (int i = 0; i < 1024 && io.poll_one(); ++i) {
        }
    }
};

}  // namespace o11proto

#endif  // O11PROTO_SERVER_NET_H
