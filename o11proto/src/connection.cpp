// o11proto - non-blocking framed TCP connection, on an Asio socket.
#include "o11proto/connection.h"

#include <array>
#include <cstring>

#include "socket_compat.h"

namespace o11proto {

namespace {
// The first `n` buffers of an array, as an Asio buffer sequence.
struct BufferSpan {
    const asio::const_buffer* first;
    std::size_t n;
    const asio::const_buffer* begin() const { return first; }
    const asio::const_buffer* end() const { return first + n; }
};
}  // namespace

std::string maskAddress(const std::string& ip) {
    std::size_t dot = ip.rfind('.');
    if (dot == std::string::npos) return ip;
    return ip.substr(0, dot + 1) + "x";
}

void Connection::NetDeleter::operator()(Net* p) const { delete p; }

Connection::Connection() = default;

Connection::Connection(socket_t fd) { adopt(fd); }

Connection::~Connection() { close(); }

void Connection::adopt(socket_t fd) {
    close();
    if (fd == kInvalidSocket) return;
    // A socket made by someone else: Asio needs its protocol, IPv4 or IPv6.
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    const bool v6 = ::getsockname(fd, reinterpret_cast<sockaddr*>(&ss), &len) == 0 && ss.ss_family == AF_INET6;
    auto io = std::make_unique<asio::io_context>(1);
    asio::ip::tcp::socket s(*io);
    asio::error_code ec;
    s.assign(v6 ? asio::ip::tcp::v6() : asio::ip::tcp::v4(), fd, ec);
    if (ec) {
        compat::closeSocket(fd);
        return;
    }
    adopt(std::unique_ptr<Net, NetDeleter>(new Net(std::move(io), std::move(s))));
}

void Connection::adopt(std::unique_ptr<Net, NetDeleter> net) {
    close();
    net_ = std::move(net);
    last_recv_ = last_send_ = Clock::now();
    inbuf_.clear();
    outq_.clear();
    out_off_ = 0;
    peer_.clear();
    if (!net_) return;
    asio::ip::tcp::socket& s = net_->socket;
    asio::error_code ec;
    compat::noSigpipe(s.native_handle());
    s.non_blocking(true, ec);
    s.set_option(asio::ip::tcp::no_delay(true), ec);
    const asio::ip::tcp::endpoint peer = s.remote_endpoint(ec);
    if (!ec) peer_ = compat::addressText(peer.address());
}

socket_t Connection::fd() const {
    return net_ && net_->socket.is_open() ? static_cast<socket_t>(net_->socket.native_handle()) : kInvalidSocket;
}

bool Connection::isOpen() const { return net_ && net_->socket.is_open(); }

void Connection::close() {
    if (!net_) return;
    asio::error_code ec;
    net_->socket.close(ec);                    // what waits on it ends with operation_aborted
    net_.reset();
}

bool Connection::send(std::uint8_t type, const std::vector<std::uint8_t>& payload) {
    if (!isOpen()) return false;
    if (outq_.size() >= kMaxSendQueue) return false;
    std::vector<std::uint8_t> frame;
    frame.reserve(kHeaderSize + payload.size());
    if (!appendFrame(frame, type, payload)) return false;
    outq_.push_back(std::move(frame));
    last_send_ = Clock::now();
    return true;
}

bool Connection::readAvailable() {
    if (!isOpen()) return false;
    asio::ip::tcp::socket& s = net_->socket;
    std::uint8_t buf[16384];
    for (;;) {
        asio::error_code ec;
        const std::size_t n = s.read_some(asio::buffer(buf), ec);
        if (!ec) {
            inbuf_.insert(inbuf_.end(), buf, buf + n);
            last_recv_ = Clock::now();
            if (n < sizeof(buf)) return true;
            continue;
        }
        if (compat::wouldBlock(ec)) return true;
        if (ec == asio::error::interrupted) continue;
        return false;                                    // EOF (asio::error::eof) or an error
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
    if (!isOpen()) return false;
    asio::ip::tcp::socket& s = net_->socket;
    // Up to kGather queued messages go in one send (a gathering write), not
    // one each: the same bytes in the same order, in fewer system calls.
    constexpr std::size_t kGather = 16;
    while (!outq_.empty()) {
        std::array<asio::const_buffer, kGather> bufs;
        std::size_t count = 0;
        for (auto it = outq_.begin(); it != outq_.end() && count < kGather; ++it, ++count) {
            const std::size_t off = count == 0 ? out_off_ : 0;
            bufs[count] = asio::buffer(it->data() + off, it->size() - off);
        }
        asio::error_code ec;                             // Asio's sends never raise SIGPIPE
        std::size_t n = s.write_some(BufferSpan{bufs.data(), count}, ec);
        if (ec) {
            if (compat::wouldBlock(ec)) return true;
            if (ec == asio::error::interrupted) continue;
            return false;
        }
        while (n > 0 && !outq_.empty()) {
            const std::size_t left = outq_.front().size() - out_off_;
            if (n < left) {
                out_off_ += n;
                break;
            }
            n -= left;
            outq_.pop_front();
            out_off_ = 0;
        }
    }
    return true;
}

bool Connection::keepalive(int interval_seconds, TimePoint now) {
    if (!isOpen()) return false;
    if (interval_seconds <= 0) interval_seconds = kDefaultKeepaliveSeconds;
    auto interval = std::chrono::seconds(interval_seconds);
    if (now - last_recv_ > 3 * interval) return false;
    if (now - last_send_ >= interval) sendKeepalive();
    return true;
}

}  // namespace o11proto
