// o11proto - Live UDP, the client side: hellos, the cookie and the drop
// rules of PROTOCOL.md 15.5-15.6.
#include "o11proto/live_transport.h"

#include <chrono>
#include <thread>

#include "o11proto/connection.h"
#include "o11proto/realtime.h"
#include "socket_compat.h"
#include "udp_compat.h"

namespace o11proto {
namespace live {

struct Transport::Impl {
    Options opt;
    socket_t fd = kInvalidSocket;
    std::thread thread;
    std::atomic<bool> stopping{false};
    bool running = false;
    std::uint8_t buf[2048];
    std::uint64_t stamps[kHelloTimestamps] = {};   // the last hellos' timestamps
    int next_stamp = 0;
    TimePoint last_hello;
    std::atomic<double> loss{0};                   // the test hook
    std::uint32_t rng = 0x9e3779b9u;              // and its generator
    std::atomic<std::int64_t> gap_until_ns{0};     // the send gap's end (steady clock)
    // wake(): a non-blocking pipe, open for the transport's life (the audio
    // thread may write while the network thread stops), and a flag so that
    // it is written once until the network thread has woken.
    int wake_fd[2] = {-1, -1};
    std::atomic<bool> wake_pending{false};
    std::atomic<bool> latency_critical{false};
    std::atomic<std::uint64_t> timeouts{0}, over_5ms{0}, wakes{0};
    std::atomic<std::int64_t> longest_us{0};
    void drainWake() {
#ifndef _WIN32
        char b[64];
        while (wake_fd[0] >= 0 && ::read(wake_fd[0], b, sizeof(b)) > 0) {
        }
#endif
    }
    bool lose() {
        const double l = loss.load(std::memory_order_relaxed);
        if (l <= 0) return false;
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (rng & 0xFFFFFF) < static_cast<std::uint32_t>(l * 16777216.0);
    }
};

namespace {

std::uint64_t monotonicMicros() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count());
}

}  // namespace

Transport::Transport() : impl_(new Impl) {
#ifndef _WIN32
    int p[2];
    if (::pipe(p) == 0) {
        for (int fd : p) {
            ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
            ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
        impl_->wake_fd[0] = p[0];
        impl_->wake_fd[1] = p[1];
    }
#endif
}

Transport::~Transport() {
    stop();
#ifndef _WIN32
    for (int fd : impl_->wake_fd)
        if (fd >= 0) ::close(fd);
#endif
}

void Transport::wake() {
#ifndef _WIN32
    Impl& m = *impl_;
    if (m.wake_fd[1] < 0 || m.wake_pending.exchange(true, std::memory_order_acq_rel)) return;
    const char c = 1;
    if (::write(m.wake_fd[1], &c, 1) != 1) m.wake_pending.store(false);
#endif
}

Transport::Timing Transport::timing() const {
    const Impl& m = *impl_;
    Timing t;
    t.latency_critical = m.latency_critical.load();
    t.timeouts = m.timeouts.load();
    t.over_5ms = m.over_5ms.load();
    t.wakes = m.wakes.load();
    t.longest_ms = static_cast<double>(m.longest_us.load()) / 1000.0;
    return t;
}

bool Transport::running() const { return impl_->running; }
void Transport::setTestReceiveLoss(double fraction) { impl_->loss.store(fraction); }
void Transport::setTestSendGap(int ms) {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    impl_->gap_until_ns.store(now + static_cast<std::int64_t>(ms) * 1000000);
}

bool Transport::start(const Options& o, std::string& error) {
    stop();
    Impl& m = *impl_;
    m.opt = o;
    std::uint32_t ip = 0;
    if (o.peer_id == 0 || o.port == 0 || !udp::parseIPv4(o.server_address, ip) || ip == 0) {
        error = "no peer ID or server address";
        return false;
    }
    socket_t fd = udp::open(kClientSocketBuffer, false);
    if (fd == kInvalidSocket) {
        error = "can't open a UDP socket (error " + std::to_string(compat::lastError()) + ")";
        return false;
    }
    udp::Addr to;
    to.ip = ip;
    to.port = htons(o.port);
    sockaddr_in sa = udp::toSockaddr(to);
    // connect(): the socket then takes datagrams from the server's address and port only.
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) != 0) {
        error = "can't connect the UDP socket to the server (error " + std::to_string(compat::lastError()) + ")";
        compat::closeSocket(fd);
        return false;
    }
    m.fd = fd;
    m.stopping.store(false);
    m.drainWake();
    m.wake_pending.store(false);
    m.timeouts.store(0);
    m.over_5ms.store(0);
    m.wakes.store(0);
    m.longest_us.store(0);
    for (auto& s : m.stamps) s = 0;
    m.next_stamp = 0;
    m.rng = 0x9e3779b9u ^ o.peer_id;
    m.loss.store(o.test_receive_loss);
    answered_.store(false);
    rtt_ms_.store(-1.0);
    cookie_.store(0);
    sent_.store(0);
    received_.store(0);
    dropped_.store(0);
    sendHello();                               // the first one at once
    m.running = true;
    m.thread = std::thread([this] { loop(); });
    return true;
}

void Transport::stop() {
    Impl& m = *impl_;
    if (!m.running) return;
    m.stopping.store(true);
    if (m.thread.joinable()) m.thread.join();
    compat::closeSocket(m.fd);
    m.fd = kInvalidSocket;
    m.running = false;
}

bool Transport::send(std::uint32_t dest, const std::uint8_t* payload, std::size_t len) {
    Impl& m = *impl_;
    if (m.fd == kInvalidSocket || len == 0 || len > kMaxPayload) return false;
    std::uint8_t d[kClientHeader + kMaxPayload];
    d[0] = kMagic0;
    d[1] = kMagic1;
    d[2] = kUdpVersion;
    d[3] = kTypeData;
    putLE32(d + 4, m.opt.peer_id);
    putLE64(d + 8, m.opt.key);
    putLE32(d + 16, dest);
    std::memcpy(d + kClientHeader, payload, len);
    const std::int64_t gap = m.gap_until_ns.load(std::memory_order_relaxed);
    if (gap != 0 &&
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() < gap)
        return true;                           // the test's gap
    if (compat::send(m.fd, d, kClientHeader + len, 0) < 0) return false;
    sent_.fetch_add(1);
    return true;
}

void Transport::sendHello() {
    Impl& m = *impl_;
    std::uint8_t h[kHelloSize] = {};
    h[0] = kMagic0;
    h[1] = kMagic1;
    h[2] = kUdpVersion;
    h[3] = kTypeHello;
    putLE32(h + 4, m.opt.peer_id);
    putLE64(h + 8, m.opt.key);
    const std::uint64_t stamp = monotonicMicros() | (1ULL << 63);   // never 0
    putLE64(h + 20, stamp);
    putLE64(h + 28, cookie_.load());
    m.stamps[m.next_stamp] = stamp;
    m.next_stamp = (m.next_stamp + 1) % kHelloTimestamps;
    m.last_hello = Clock::now();
    if (compat::send(m.fd, h, sizeof(h), 0) >= 0) sent_.fetch_add(1);
}

void Transport::handleDatagram(const std::uint8_t* d, std::size_t n) {
    Impl& m = *impl_;
    if (n < kServerHeader || d[0] != kMagic0 || d[1] != kMagic1 || d[2] != kUdpVersion) {
        dropped_.fetch_add(1);
        return;
    }
    const std::uint32_t origin = getLE32(d + 4);
    if (d[3] == kTypeHello) {
        if (n != kHelloAckSize || origin != 0) {
            dropped_.fetch_add(1);
            return;
        }
        const std::uint64_t stamp = getLE64(d + 8);
        bool mine = false;
        for (std::uint64_t s : m.stamps) mine |= stamp != 0 && s == stamp;
        const std::uint64_t now = monotonicMicros() | (1ULL << 63);
        if (!mine || now < stamp || now - stamp > static_cast<std::uint64_t>(kAckMaxRoundTripUs)) {
            dropped_.fetch_add(1);             // not an answer to one of ours: the cookie stays
            return;
        }
        rtt_ms_.store(static_cast<double>(now - stamp) / 1000.0);
        answered_.store(true);
        const std::uint64_t c = getLE64(d + 16);
        if (c != cookie_.load()) {
            cookie_.store(c);
            sendHello();                       // binds with one round trip
        }
        return;
    }
    if (d[3] != kTypeData || origin == 0 || n == kServerHeader || n - kServerHeader > kMaxPayload) {
        dropped_.fetch_add(1);
        return;
    }
    if (m.lose()) {
        dropped_.fetch_add(1);
        return;
    }
    received_.fetch_add(1);
    if (on_data) on_data(origin, d + kServerHeader, n - kServerHeader);
}

void Transport::loop() {
    Impl& m = *impl_;
    m.latency_critical.store(thread::makeLatencyCritical());
    while (!m.stopping.load()) {
        pollfd p[2] = {};
        p[0].fd = m.fd;
        p[0].events = POLLIN;
        std::size_t n_fds = 1;
#ifndef _WIN32
        if (m.wake_fd[0] >= 0) {
            p[1].fd = m.wake_fd[0];
            p[1].events = POLLIN;
            n_fds = 2;
        }
#endif
        const TimePoint t0 = Clock::now();
        const int ready = compat::poll(p, n_fds, 1);
        if (ready == 0) {                      // the timed wait ran out: how long it took
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
            m.timeouts.fetch_add(1, std::memory_order_relaxed);
            if (us > 5000) m.over_5ms.fetch_add(1, std::memory_order_relaxed);
            if (us > m.longest_us.load(std::memory_order_relaxed)) m.longest_us.store(us, std::memory_order_relaxed);
        }
        if (n_fds == 2 && (p[1].revents & POLLIN)) {
            m.drainWake();                     // then the flag: a wake() after it writes again
            m.wake_pending.store(false, std::memory_order_release);
            m.wakes.fetch_add(1, std::memory_order_relaxed);
        }
        for (int i = 0; i < 1024; ++i) {       // bounded, so the tick runs too
            std::size_t n = 0;
            bool truncated = false, has_local = false;
            udp::Addr from, local;
            udp::Recv r = udp::receive(m.fd, m.buf, sizeof(m.buf), n, truncated, from, local, has_local);
            if (r == udp::Recv::Empty) break;
            if (r == udp::Recv::Error) continue;   // an ICMP error and the like: the next one
            if (truncated || n > kMaxClientDatagram) {
                dropped_.fetch_add(1);
                continue;
            }
            handleDatagram(m.buf, n);
        }
        if (Clock::now() - m.last_hello >= std::chrono::milliseconds(kHelloIntervalMs)) sendHello();
        if (on_tick) on_tick();
    }
    if (on_closing) on_closing();
}

}  // namespace live
}  // namespace o11proto
