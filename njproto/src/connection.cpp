// njproto - non-blocking framed TCP connection.
#include "njproto/connection.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace njproto {

bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool setNoDelay(int fd) {
    int one = 1;
    return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0;
}

std::string peerAddressOf(int fd) {
    sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return "";
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        auto* a = reinterpret_cast<sockaddr_in*>(&ss);
        inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf));
    } else if (ss.ss_family == AF_INET6) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
        inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf));
        // Present IPv4-mapped addresses (::ffff:1.2.3.4) as plain IPv4.
        std::string s(buf);
        if (s.rfind("::ffff:", 0) == 0 && s.find('.') != std::string::npos) return s.substr(7);
    }
    return buf;
}

std::string maskAddress(const std::string& ip) {
    std::size_t dot = ip.rfind('.');
    if (dot == std::string::npos) return ip;
    return ip.substr(0, dot + 1) + "x";
}

Connection::Connection(int fd) { adopt(fd); }

Connection::~Connection() { close(); }

void Connection::adopt(int fd) {
    close();
    fd_ = fd;
    setNonBlocking(fd_);
    setNoDelay(fd_);
    peer_ = peerAddressOf(fd_);
    last_recv_ = last_send_ = Clock::now();
    inbuf_.clear();
    outq_.clear();
    out_off_ = 0;
}

void Connection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Connection::send(std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    if (fd_ < 0) return false;
    if (outq_.size() >= kMaxSendQueue) return false;
    std::vector<std::uint8_t> frame;
    frame.reserve(kHeaderSize + payload.size());
    if (!appendFrame(frame, type, payload)) return false;
    outq_.push_back(std::move(frame));
    last_send_ = Clock::now();
    return true;
}

bool Connection::readAvailable() {
    if (fd_ < 0) return false;
    std::uint8_t buf[16384];
    for (;;) {
        ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n > 0) {
            inbuf_.insert(inbuf_.end(), buf, buf + n);
            last_recv_ = Clock::now();
            if (static_cast<std::size_t>(n) < sizeof(buf)) return true;
            continue;
        }
        if (n == 0) return false;                        // EOF
        if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

bool Connection::nextMessage(RawMessage& out, bool& error) {
    error = false;
    if (inbuf_.empty()) return false;
    std::size_t consumed = 0;
    FrameStatus st = parseFrame(inbuf_.data(), inbuf_.size(), out, consumed);
    if (st == FrameStatus::Error) {
        error = true;
        return false;
    }
    if (st == FrameStatus::NeedMore) return false;
    inbuf_.erase(inbuf_.begin(), inbuf_.begin() + static_cast<std::ptrdiff_t>(consumed));
    return true;
}

bool Connection::flush() {
    if (fd_ < 0) return false;
    while (!outq_.empty()) {
        const auto& front = outq_.front();
        ssize_t n = ::send(fd_, front.data() + out_off_, front.size() - out_off_, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            if (errno == EINTR) continue;
            return false;
        }
        out_off_ += static_cast<std::size_t>(n);
        if (out_off_ >= front.size()) {
            outq_.pop_front();
            out_off_ = 0;
        }
    }
    return true;
}

bool Connection::keepalive(int interval_seconds, TimePoint now) {
    if (fd_ < 0) return false;
    if (interval_seconds <= 0) interval_seconds = kDefaultKeepaliveSeconds;
    auto interval = std::chrono::seconds(interval_seconds);
    if (now - last_recv_ > 3 * interval) return false;
    if (now - last_send_ >= interval) sendKeepalive();
    return true;
}

}  // namespace njproto
