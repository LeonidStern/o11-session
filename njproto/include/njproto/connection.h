// njproto - one non-blocking TCP connection carrying framed messages.
// Shared by the client and the server; it knows nothing about the protocol
// above the framing layer except the keepalive rule.
#ifndef NJPROTO_CONNECTION_H
#define NJPROTO_CONNECTION_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "njproto/message.h"

namespace njproto {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

class Connection {
public:
    Connection() = default;
    explicit Connection(int fd);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // Takes ownership of an already connected (or connecting) socket and
    // makes it non-blocking with TCP_NODELAY.
    void adopt(int fd);
    int fd() const { return fd_; }
    bool isOpen() const { return fd_ >= 0; }
    void close();

    // Queues one framed message. Returns false when the queue is over the
    // limit or the payload is too long; the caller should then close.
    bool send(std::uint8_t type, const std::vector<std::uint8_t>& payload);
    bool send(std::uint8_t type) { return send(type, std::vector<std::uint8_t>()); }
    bool sendKeepalive() { return send(kMsgKeepalive); }

    // Reads whatever the socket has. Returns false on EOF or a socket error.
    bool readAvailable();
    // Pops the next complete message from the input buffer. `error` is set
    // when the stream is malformed (oversized payload or type 0xFF).
    bool nextMessage(RawMessage& out, bool& error);
    // Writes as much of the queue as the socket takes. False on error.
    bool flush();
    bool wantWrite() const { return !outq_.empty(); }
    std::size_t queuedMessages() const { return outq_.size(); }

    TimePoint lastRecv() const { return last_recv_; }
    TimePoint lastSend() const { return last_send_; }
    void touchRecv() { last_recv_ = Clock::now(); }

    // Keepalive housekeeping: sends a keepalive when nothing was sent for
    // `interval` seconds; returns false when nothing was received for
    // 3 x interval seconds (the caller should close).
    bool keepalive(int interval_seconds, TimePoint now);

    // The peer address as text ("1.2.3.4"), filled by adopt() when possible.
    const std::string& peerAddress() const { return peer_; }

private:
    int fd_ = -1;
    std::vector<std::uint8_t> inbuf_;
    std::deque<std::vector<std::uint8_t>> outq_;   // framed messages
    std::size_t out_off_ = 0;                      // bytes of outq_.front() already sent
    TimePoint last_recv_;
    TimePoint last_send_;
    std::string peer_;
};

// Small socket helpers used by both sides.
bool setNonBlocking(int fd);
bool setNoDelay(int fd);
std::string peerAddressOf(int fd);
// Replaces the last octet of a dotted IPv4 address with 'x' (section 4.2).
std::string maskAddress(const std::string& ip);

}  // namespace njproto

#endif  // NJPROTO_CONNECTION_H
